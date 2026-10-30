#include "rpiezo_axial_experiment_service.h"
#include <rstructural_piezo_axial_operators.h>
#include <rstructural_piezo_element_operators.h>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <rbl_error.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <stdexcept>

namespace {
void require(bool ok,const QString &message) {if(!ok)throw std::invalid_argument(message.toStdString());}
void keys(const QJsonObject &o,std::initializer_list<const char*> allowed) {
    std::set<QString> names;for(auto n:allowed)names.insert(QString::fromLatin1(n));
    for(auto it=o.begin();it!=o.end();++it)require(names.count(it.key()),"Unknown field: "+it.key());
}
QJsonObject object(const QJsonValue &v){require(v.isObject(),"Expected object");return v.toObject();}
QJsonArray array(const QJsonValue &v){require(v.isArray(),"Expected array");return v.toArray();}
double number(const QJsonValue &v){require(v.isDouble()&&std::isfinite(v.toDouble()),"Expected finite number");return v.toDouble();}
uint integer(const QJsonValue &v){double x=number(v);require(x>0&&x<std::numeric_limits<uint>::max()&&x==std::floor(x),"Expected positive physical ID/count");return uint(x);}
QString string(const QJsonValue &v){require(v.isString(),"Expected string");return v.toString();}
bool boolean(const QJsonValue &v){require(v.isBool(),"Expected boolean");return v.toBool();}
void finiteTree(const QJsonValue &v,uint depth=0){require(depth<=64,"Recipe nesting exceeds bound");if(v.isDouble())number(v);else if(v.isArray())for(auto x:v.toArray())finiteTree(x,depth+1);else if(v.isObject())for(auto x:v.toObject())finiteTree(x,depth+1);else require(v.isString()||v.isBool(),"Null or unsupported recipe value");}
template<size_t N> QJsonArray json(const std::array<double,N> &v){QJsonArray a;for(double x:v)a.append(x);return a;}
void sha(const QString &v){require(QRegularExpression("^[0-9a-f]{64}$").match(v).hasMatch(),"Invalid SHA-256 source identity");}
QJsonObject si(){return {{"system","SI"},{"elasticity","Pa"},{"compliance","1/Pa"},{"stressCoupling","C/m^2"},{"strainCoupling","m/V"},{"permittivity","F/m"},{"density","kg/m^3"},{"length","m"},{"time","s"},{"force","N"},{"voltage","V"},{"charge","C"},{"rotation","rad"}};}
QJsonObject frame(){return {{"origin",QJsonArray{0.,0.,0.}},{"ex",QJsonArray{1.,0.,0.}},{"ey",QJsonArray{0.,1.,0.}},{"ez",QJsonArray{0.,0.,1.}}};}
std::array<double,3> translation(const std::array<double,6> &v){for(uint i=3;i<6;++i)require(v[i]==0,"Axial TRUSS1 does not support rotational fields or moments");return {{v[0],v[1],v[2]}};}
std::vector<RElementGroup*> groups(RModel &m){std::vector<RElementGroup*> out;for(uint i=0;i<m.getNElementGroups();++i){auto *g=m.getElementGroupPtr(i);if(g)out.push_back(g);}return out;}
QJsonArray legacy(RModel &m,bool remove){QJsonArray out;for(auto *g:groups(m)){
    for(uint i=0;i<g->getNBoundaryConditions();++i)out.append(QJsonObject{{"groupPhysicalId",double(g->getPhysicalId())},{"kind","boundary"},{"type",int(g->getBoundaryCondition(i).getType())}});
    for(uint i=0;i<g->getNInitialConditions();++i)out.append(QJsonObject{{"groupPhysicalId",double(g->getPhysicalId())},{"kind","initial"},{"type",int(g->getInitialCondition(i).getType())}});
    for(uint i=0;i<g->getNEnvironmentConditions();++i)out.append(QJsonObject{{"groupPhysicalId",double(g->getPhysicalId())},{"kind","environment"},{"type",int(g->getEnvironmentCondition(i).getType())}});
    if(remove){while(g->getNBoundaryConditions())g->removeBoundaryCondition(uint(0));while(g->getNInitialConditions())g->removeInitialCondition(uint(0));while(g->getNEnvironmentConditions())g->removeEnvironmentCondition(uint(0));}
}return out;}
double length(const RModel &m,const RElement &e){const auto &a=m.getNode(e.getNodeId(0)),&b=m.getNode(e.getNodeId(1));const double l=std::hypot(a.getX()-b.getX(),a.getY()-b.getY(),a.getZ()-b.getZ());require(std::isfinite(l)&&l>0,"Degenerate actual mesh chord");return l;}
RStructuralPiezoContinuumMaterial continuum(const RStructuralPiezoLayeredMaterial &m){
    RStructuralPiezoContinuumMaterial out;
    if(!m.piezoelectric){for(double x:m.coupling)require(x==0,"Passive material contains electrical coupling");for(double x:m.permittivity)require(x==0,"Passive material contains permittivity");}
    if(m.form==RStructuralPiezoMaterialForm::StrainCharge){
        if(m.piezoelectric)out=RStructuralPiezoContinuumMaterial::fromStrainCharge(m.elasticityOrCompliance,m.coupling,m.permittivity,m.density);
        else {for(double x:m.coupling)require(x==0,"Passive material contains electrical coupling");for(double x:m.permittivity)require(x==0,"Passive material contains permittivity");out=RStructuralPiezoContinuumMaterial::fromStrainCharge(m.elasticityOrCompliance,{},{{1,0,0,0,1,0,0,0,1}},m.density);out.piezoelectric=false;out.permittivity={};}
    }else{out.elasticity=m.elasticityOrCompliance;out.coupling=m.coupling;out.permittivity=m.permittivity;out.density=m.density;out.piezoelectric=m.piezoelectric;}
    out.validate();return out;
}
RMaterial mechanical(const RStructuralPiezoContinuumMaterial &m){
    const double modulus=rStructuralPiezoReduceFreeStress(m,{0},0).elasticity[0];
    RMaterial material(RMaterial::Solid);RMaterialProperty young(RMaterialProperty::ModulusOfElasiticity),density(RMaterialProperty::Density);
    young.add(293.15,modulus);density.add(293.15,m.density);material.add(young);material.add(density);
    // The recipe retains the full tensor and provenance. This mechanical host
    // identity binds only its effective native law and must survive re-preview.
    const auto witness=QJsonDocument(QJsonObject{{"schema","range.axial-effective-mechanical-material/v1"},{"youngModulus",modulus},{"density",m.density}}).toJson(QJsonDocument::Compact);
    material.setID(QUuid::createUuidV5(QUuid("{26143efe-6b4c-55a6-9e54-a361f537dc41}"),witness));
    return material;
}
void setComponent(RCondition &condition,RVariableType type,double value,bool enabled){
    auto &c=condition.getComponent(condition.findComponentPosition(type));c.setEnabled(enabled);c.clear();if(enabled)c.add(0,value);
}
QJsonArray regroupMembers(RModel &model,const QJsonArray &members,
                          const std::map<QString,RStructuralPiezoContinuumMaterial> &materials)
{
    struct Declaration{double area;QString materialId;};
    std::map<uint,Declaration> declarations;
    for(auto value:members){const auto member=object(value);const auto id=integer(member["physicalElementId"]);
        const auto material=string(member["materialId"]);const double area=number(member["area"]);
        require(area>0 && materials.count(material),"Invalid member area or material source");
        require(declarations.emplace(id,Declaration{area,material}).second,"Duplicate actual member binding");}
    QJsonArray changes;
    const uint originalLines=model.getNLines();
    for(uint lineId=0;lineId<originalLines;++lineId){
        const RLine original=model.getLine(lineId);
        using Key=std::tuple<double,double,double>;
        struct Partition{Key key;QString materialId;std::vector<uint> elements;};
        std::vector<uint> positions;for(uint j=0;j<original.size();++j)positions.push_back(original.get(j));
        std::sort(positions.begin(),positions.end(),[&](uint a,uint b){return model.getElementPhysicalId(a)<model.getElementPhysicalId(b);});
        std::vector<Partition> partitions;
        for(uint position:positions){const auto &element=model.getElement(position);
            require(element.getType()==R_ELEMENT_TRUSS1,"Axial line grouping requires actual TRUSS1 members");
            const auto found=declarations.find(model.getElementPhysicalId(position));
            require(found!=declarations.end(),"Member provenance must cover every rod");
            const auto &declaration=found->second;const auto &material=materials.at(declaration.materialId);
            const Key key{declaration.area,rStructuralPiezoReduceFreeStress(material,{0},0).elasticity[0],material.density};
            auto partition=std::find_if(partitions.begin(),partitions.end(),[&](const auto &p){return p.key==key;});
            if(partition==partitions.end())partitions.push_back({key,declaration.materialId,{position}});
            else partition->elements.push_back(position);
        }
        if(partitions.size()<=1)continue;
        for(size_t i=0;i<partitions.size();++i){const auto &part=partitions[i];RLine group(original);group.resize(0);
            for(uint element:part.elements)group.add(element);
            group.setCrossArea(std::get<0>(part.key));group.setMaterial(mechanical(materials.at(part.materialId)));
            uint destination=lineId;
            if(i){group.setName(original.getName()+QString(" / %1").arg(i+1));destination=model.getNLines();model.addLine(group);}
            else model.setLine(lineId,group);
            QJsonArray ids;for(uint element:part.elements)ids.append(double(model.getElementPhysicalId(element)));
            changes.append(QJsonObject{{"parentGroupPhysicalId",double(original.getPhysicalId())},
                {"groupPhysicalId",double(model.getLine(destination).getPhysicalId())},{"memberPhysicalIds",ids},
                {"area",std::get<0>(part.key)},{"youngModulus",std::get<1>(part.key)},{"density",std::get<2>(part.key)},
                {"retainedParentIdentity",i==0}});
        }
    }
    return changes;
}
const RVariableType displacement[]={R_VARIABLE_DISPLACEMENT_X,R_VARIABLE_DISPLACEMENT_Y,R_VARIABLE_DISPLACEMENT_Z};
const RVariableType velocity[]={R_VARIABLE_VELOCITY_X,R_VARIABLE_VELOCITY_Y,R_VARIABLE_VELOCITY_Z};
const RVariableType force[]={R_VARIABLE_FORCE_X,R_VARIABLE_FORCE_Y,R_VARIABLE_FORCE_Z};
// Each generated POINT belongs to its own nodal entity. Existing multi-node
// entities are retained, and are not used to smear a nodal declaration.
uint pointForNode(RModel &m,uint node){
    for(uint p=0;p<m.getNPoints();++p){const auto &g=m.getPoint(p);if(g.size()==1){const auto &e=m.getElement(g.get(0));if(e.getType()==R_ELEMENT_POINT&&e.getNodeId(0)==node&&g.getVolume()==0)return p;}}
    uint p=m.getNPoints();RElement e(R_ELEMENT_POINT);e.setNodeId(0,node);m.addElement(e,true,p);m.getPoint(p).setVolume(0);
    RMaterial neutral;neutral.setID(QUuid::createUuidV5(QUuid("{26143efe-6b4c-55a6-9e54-a361f537dc41}"),QByteArrayLiteral("range.axial-zero-mass-nodal-entity/v1")));
    m.getPoint(p).setMaterial(neutral);
    while(m.getPoint(p).getNEnvironmentConditions())m.getPoint(p).removeEnvironmentCondition(uint(0));return p;
}
void channel(const QJsonObject &o,const std::set<uint> &nodes){
    const auto parts=string(o["channelId"]).split(':');require(parts.size()==3&&parts[0]=="node","Invalid physical force channel");bool ok=false;uint id=parts[1].toUInt(&ok);
    require(ok&&nodes.count(id),"Unknown physical force node");require(parts[2]=="X"||parts[2]=="Y"||parts[2]=="Z","Axial force channels support X/Y/Z only");require(string(o["unit"])=="N","Axial force channel unit must be N");
}
// Reuse the same declaration validators for controls, excitation terms and
// damping, without constructing a layered plant or changing the actual mesh.
void commonValidation(const RStructuralPiezoLayeredSetup &s,const std::set<uint> &nodes){
    RStructuralPiezoReadoutSetup readout;for(const auto &p:s.ports){readout.ports.push_back(p.readout);require(std::isfinite(p.initialValue),"Nonfinite port initial value");if(p.readout.role==RStructuralPiezoPortRole::Sensor&&p.readout.readout!=RStructuralPiezoReadoutMode::OpenCircuitVoltage)require(p.initialValue==0,"Short/amplifier sensors require zero voltage");}readout.validate();
    std::set<std::pair<uint,uint>> support;for(const auto &v:s.supports){require(nodes.count(v.physicalNodeId)&&uint(v.dof)<3,"Axial supports require known physical nodes and UX/UY/UZ");require(support.insert({v.physicalNodeId,uint(v.dof)}).second,"Duplicate nodal support binding");}
    for(const auto *fs:{&s.backgroundForces,&s.initialDisplacements,&s.initialVelocities}){std::set<uint> used;for(const auto &v:*fs){require(nodes.count(v.physicalNodeId)&&used.insert(v.physicalNodeId).second,"Unknown or duplicate nodal field binding");translation(v.values);}}
    require(s.workpoint.available||(s.workpoint.nodes.empty()&&s.workpoint.ports.empty()),"Unavailable workpoint contains hidden data");
    std::set<uint> wn;for(const auto &v:s.workpoint.nodes){require(nodes.count(v.physicalNodeId)&&wn.insert(v.physicalNodeId).second,"Unknown or duplicate workpoint node");translation(v.q0);translation(v.f0);}
    const auto &d=s.damping;require(d.rayleighAlpha>=0&&d.rayleighBeta>=0&&d.uniformRatio>=0,"Negative damping parameter");
    require(d.kind==RStructuralPiezoDampingKind::Rayleigh||(d.rayleighAlpha==0&&d.rayleighBeta==0),"Irrelevant Rayleigh damping fields");require(d.kind==RStructuralPiezoDampingKind::UniformModal||d.uniformRatio==0,"Irrelevant uniform damping field");
    bool modal=d.kind==RStructuralPiezoDampingKind::PerMode||d.kind==RStructuralPiezoDampingKind::PerCluster;require(modal?!d.ratios.empty():d.ratios.empty(),"Missing or irrelevant modal ratios");for(double x:d.ratios)require(x>=0,"Negative modal damping ratio");
    if(d.kind==RStructuralPiezoDampingKind::PerCluster){require(!d.clusterEnds.empty()&&d.clusterEnds.size()==d.ratios.size(),"Modal cluster shape mismatch");uint last=0;for(uint x:d.clusterEnds){require(x>last,"Invalid modal cluster ends");last=x;}}else require(d.clusterEnds.empty(),"Irrelevant modal cluster fields");
    if(d.kind==RStructuralPiezoDampingKind::Imported){require(d.importedDimension==3*nodes.size()&&d.importedMatrix.size()==size_t(d.importedDimension)*d.importedDimension,"Imported physical damping shape mismatch");require(!d.sourceId.isEmpty(),"Imported damping source ID missing");sha(d.sourceSha256);}else require(d.importedDimension==0&&d.importedMatrix.empty()&&d.sourceId.isEmpty()&&d.sourceSha256.isEmpty(),"Irrelevant imported damping fields");
}
}

RModel RPiezoAxialExperimentService::templateCandidate(const RModel &source,const QJsonObject &t){
    require(source.getNNodes()==0&&source.getNElements()==0,"Templates require an empty model");keys(t,{"kind","length","width","thickness","elements"});require(string(t["kind"])=="axialTruss","Unsupported axial template");
    double l=number(t["length"]),w=number(t["width"]),h=number(t["thickness"]);uint n=integer(t["elements"]);require(l>0&&w>0&&h>0&&n>=2&&n<=100,"Axial template requires positive dimensions and 2..100 elements");RModel m(source);
    for(uint i=0;i<=n;++i)m.addNode(RNode(l*double(i)/n,0,0));for(uint i=0;i<n;++i){RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);}m.getLine(0).setCrossArea(w*h);
    legacy(m,true);return m;
}

QJsonObject RPiezoAxialExperimentService::exampleRecipe(const RModel &source,const QJsonArray &selection){
    try{
        if(!source.getNNodes()&&!source.getNElements()){QJsonObject t{{"kind","axialTruss"},{"length",1.},{"width",.1},{"thickness",.01},{"elements",4}};auto r=exampleRecipe(templateCandidate(source,t),selection);if(!r.isEmpty())r["template"]=t;return r;}
        std::vector<uint> rods;std::set<uint> actual,chosen;for(uint i=0;i<source.getNElements();++i){auto type=source.getElement(i).getType();require(type==R_ELEMENT_TRUSS1||type==R_ELEMENT_POINT,"Only actual TRUSS1/POINT meshes support axial experiments");if(type==R_ELEMENT_TRUSS1){rods.push_back(i);actual.insert(source.getElementPhysicalId(i));}}
        require(!rods.empty(),"No actual axial members");for(auto v:selection){uint id=integer(v);require(actual.count(id)&&chosen.insert(id).second,"Unknown or duplicate selected rod");}if(chosen.empty())chosen=actual;
        auto host=RStructuralPiezoContinuumMaterial::isotropic(7e10,.3,2700),active=host;active.piezoelectric=true;active.density=7500;active.coupling[0]=.1;active.permittivity={{1e-8,0,0,0,1e-8,0,0,0,1e-8}};
        QJsonArray materials;const auto material=[&](const RStructuralPiezoContinuumMaterial &m,const QString &id){QJsonObject o{{"materialId",id},{"form","stressCharge"},{"elasticityOrCompliance",json(m.elasticity)},{"coupling",json(m.coupling)},{"permittivity",json(m.permittivity)},{"density",m.density},{"piezoelectric",m.piezoelectric},{"sourceKind","example"},{"sourceId","range-fea/synthetic-SI/axial/"+id+"/v1"},{"notes","Synthetic SI demonstration data; reconfigures material, not preserved legacy provenance or hardware qualification"}};o["sourceSha256"]=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());return o;};
        materials.append(material(host,"host"));materials.append(material(active,"examplePiezo"));
        QJsonArray members,ports;uint selectedIndex=0;for(uint i:rods){uint id=source.getElementPhysicalId(i);double area=.001;for(uint l=0;l<source.getNLines();++l){const auto &line=source.getLine(l);for(uint k=0;k<line.size();++k)if(line.get(k)==i&&line.getCrossArea()>0)area=line.getCrossArea();}
            bool enabled=chosen.count(id);QJsonObject member{{"physicalElementId",double(id)},{"kind",enabled?"active":"passive"},{"materialId",enabled?"examplePiezo":"host"},{"area",area}};if(enabled){QString port=QString("rod_%1").arg(id);member["portId"]=port;member["polarity"]=1;bool sensor=selectedIndex++>0;ports.append(QJsonObject{{"portId",port},{"role",chosen.size()==1?"actuatorSensor":sensor?"sensor":"actuator"},{"readout",sensor?"openCircuitVoltage":"actuatorCharge"},{"initialValue",0.}});}members.append(member);
        }
        // The generated chain has a defensible X-axis fixture. Existing 3-D
        // networks retain only supports explicitly present in their source.
        bool xChain=true;double minX=std::numeric_limits<double>::infinity(),maxX=-minX;uint first=0,tip=0;for(uint i=0;i<source.getNNodes();++i){const auto &n=source.getNode(i);const auto &origin=source.getNode(0);if(n.getY()!=origin.getY()||n.getZ()!=origin.getZ())xChain=false;if(n.getX()<minX){minX=n.getX();first=i;}if(n.getX()>maxX){maxX=n.getX();tip=i;}}
        QJsonArray supports;const char *dofs[]={"UX","UY","UZ"};if(xChain){for(uint i=0;i<source.getNNodes();++i)for(uint axis=0;axis<3;++axis)if(axis||i==first)supports.append(QJsonObject{{"physicalNodeId",double(source.getNodePhysicalId(i))},{"dof",dofs[axis]},{"value",0.}});}else{
            std::map<std::pair<uint,uint>,double> bindings;for(uint gi=0;gi<source.getNElementGroups();++gi){const auto *g=source.getElementGroupPtr(gi);if(!g)continue;for(uint ci=0;ci<g->getNBoundaryConditions();++ci){const auto &bc=g->getBoundaryCondition(ci);if(bc.getType()!=R_BOUNDARY_CONDITION_DISPLACEMENT)continue;for(uint a=0;a<3;++a){const auto &c=bc.getComponent(bc.findComponentPosition(displacement[a]));if(!c.getEnabled()||c.size()!=1)continue;for(uint k=0;k<g->size();++k){const auto &e=source.getElement(g->get(k));for(uint j=0;j<e.size();++j){auto key=std::make_pair(source.getNodePhysicalId(e.getNodeId(j)),a);auto inserted=bindings.emplace(key,c.getValue(0));require(inserted.second||inserted.first->second==c.getValue(0),"Conflicting source supports");}}}}}
            for(const auto &v:bindings)supports.append(QJsonObject{{"physicalNodeId",double(v.first.first)},{"dof",dofs[v.first.second]},{"value",v.second}});
        }
        QJsonArray outputs;const char *quantities[]={"displacement","velocity","acceleration"},*units[]={"m","m/s","m/s^2"};for(uint i=0;i<3;++i)outputs.append(QJsonObject{{"id",QString("tip_%1").arg(quantities[i])},{"physicalNodeId",double(source.getNodePhysicalId(tip))},{"dof","X"},{"quantity",quantities[i]},{"unit",units[i]}});
        return {{"family","axialRod"},{"frame",frame()},{"materials",materials},{"members",members},{"ports",ports},{"supports",supports},{"backgroundForces",QJsonArray{}},{"initialDisplacements",QJsonArray{}},{"initialVelocities",QJsonArray{}},{"workpoint",QJsonObject{{"available",false},{"nodes",QJsonArray{}},{"ports",QJsonArray{}}}},{"damping",QJsonObject{{"kind","none"}}},{"controller",QJsonObject{{"kind","none"}}},{"excitation",QJsonObject{{"channels",QJsonArray{}}}},{"design",QJsonObject{{"outputs",outputs}}},{"dt",.001},{"steps",100},{"units",si()},{"replaceLegacyConditions",false}};
    }catch(...){return {};}
}

RPiezoExperimentService::Result RPiezoAxialExperimentService::preview(const RModel &source,const QJsonObject &r){
    RPiezoExperimentService::Result result;result.candidate=source;try{
        finiteTree(r);keys(r,{"family","frame","materials","members","ports","supports","backgroundForces","initialDisplacements","initialVelocities","workpoint","damping","controller","excitation","design","dt","steps","units","replaceLegacyConditions","template"});require(string(r["family"])=="axialRod","Unsupported axial family");require(object(r["units"])==si(),"Recipe must declare exact SI units");require(object(r["frame"])==frame(),"Axial frame must be global identity; member local x follows its actual chord");
        const auto declarations=RPiezoExperimentDeclarations::parseAxialCommon(r);RPiezoExperimentDeclarations::validateController(r,declarations);double dt=number(r["dt"]);uint steps=integer(r["steps"]);require(dt>0&&steps<=1000000,"Invalid integration grid");
        bool bound=source.getProblemSetup().getStructuralPiezoAxialSetup().enabled;if(bound)source.getProblemSetup().getStructuralPiezoAxialSetup().validate(source);
        RModel candidate=r.contains("template")?templateCandidate(source,object(r["template"])):source;
        require(candidate.getNNodes()<=2048/3,"Axial whole-experiment dense operator limit is 2048 physical DOFs");
        require((source.getProblemTaskTree().getProblemTypeMask()&~(R_PROBLEM_STRESS|R_PROBLEM_STRESS_MODAL))==0,"Unsupported source analysis");
        std::map<uint,uint> nodePositions,rodPositions;for(uint i=0;i<candidate.getNNodes();++i)require(nodePositions.emplace(candidate.getNodePhysicalId(i),i).second,"Duplicate physical node ID");std::set<uint> nodeIds;for(const auto &v:nodePositions)nodeIds.insert(v.first);
        for(uint i=0;i<candidate.getNElements();++i){const auto &e=candidate.getElement(i);require(e.getType()==R_ELEMENT_TRUSS1||e.getType()==R_ELEMENT_POINT,"Axial experiment requires actual TRUSS1/POINT topology");if(e.getType()==R_ELEMENT_TRUSS1){length(candidate,e);require(rodPositions.emplace(candidate.getElementPhysicalId(i),i).second,"Duplicate physical rod ID");}}
        require(!rodPositions.empty(),"No actual TRUSS1 source members");commonValidation(declarations,nodeIds);for(auto v:RPiezoExperimentDeclarations::forceChannels(object(r["excitation"])))channel(object(v),nodeIds);
        for(auto value:array(object(r["design"])["outputs"])){auto o=object(value);require(nodeIds.count(integer(o["physicalNodeId"])),"Unknown design output node");auto dof=string(o["dof"]);require(dof=="X"||dof=="Y"||dof=="Z","Axial design outputs support X/Y/Z only");}
        auto control=object(r["controller"]);if(control["kind"].toString()=="discrete_state_space"){uint stride=control.contains("controlEveryFESteps")?integer(control["controlEveryFESteps"]):1;require(number(control["samplingPeriod"])==dt*stride,"Controller period differs from FE control grid");}
        bool replace=r.contains("replaceLegacyConditions")?boolean(r["replaceLegacyConditions"]):false;auto removed=legacy(candidate,false);require(removed.isEmpty()||replace||bound,"Legacy conditions require explicit replaceLegacyConditions=true; all old loads/supports will be replaced");legacy(candidate,true);
        auto &p=candidate.getProblemSetup();p.setRestart(false);p.setStructuralDynamicsRestartData({});p.setStructuralTrussDynamicsRestartData({});p.setStructuralPiezoLayeredRestart({});p.setStructuralPiezoAxialRestart({});p.getStructuralPiezoLayeredSetup()={};p.getStructuralPiezoAxialSetup()={};p.getStructuralNativeControlState()={};p.getStructuralNativeControlSpec()={};p.getStructuralControlSetup()={};p.getStructuralPiezoReadoutSetup()={};
        candidate.clearResults();candidate.clearEntityVariableData();RProblemTaskItem task;task.setNIterations(1);task.addChild(RProblemTaskItem(R_PROBLEM_STRESS));candidate.setProblemTaskTree(task);p.setMassMatrixType(RMassMatrixType::Lumped);p.getStructuralDynamicsSetup()={};auto &d=p.getStructuralDynamicsSetup();d.setEnabled(true);d.setTimeStep(dt);d.setNSteps(steps);
        double start=source.getTimeSolver().getInputStartTime(),initial=start-dt;require(std::isfinite(start)&&std::isfinite(initial)&&initial<start,"Invalid source initial clock");std::vector<double> times;times.reserve(steps);for(uint i=0;i<steps;++i){double t=initial+double(i+1)*dt;require(std::isfinite(t)&&t>(times.empty()?initial:times.back()),"Integration clock cannot represent requested grid");times.push_back(t);}auto &time=candidate.getTimeSolver();time.setEnabled(true);time.setTimes(times);time.setInputStartTime(start);time.setInputTimeStepSize(dt);time.setInputNTimeSteps(steps);time.setCurrentTimeStep(0);time.setComputedTime(initial);
        std::map<QString,RStructuralPiezoContinuumMaterial> materials;for(const auto &m:declarations.materials){require(!m.materialId.isEmpty()&&!m.sourceId.isEmpty(),"Material source identity missing");sha(m.sourceSha256);require(materials.emplace(m.materialId,continuum(m)).second,"Duplicate material ID");}
        RStructuralTrussDynamicsSetup truss;truss.enabled=true;RStructuralPiezoReadoutSetup catalog;std::map<QString,RStructuralPiezoLayeredPort> ports;for(const auto &port:declarations.ports){require(ports.emplace(port.readout.portId,port).second,"Duplicate electrical port");catalog.ports.push_back(port.readout);}
        const auto regrouped=regroupMembers(candidate,array(r["members"]),materials);
        struct LineSource{bool set=false,host=false;double area=0,young=0,density=0;RStructuralPiezoContinuumMaterial material;};std::map<uint,LineSource> lines;std::set<uint> members;std::set<QString> usedMaterials,usedPorts;
        for(auto value:array(r["members"])){auto o=object(value);auto kind=string(o["kind"]);if(kind=="active")keys(o,{"physicalElementId","kind","materialId","area","portId","polarity"});else if(kind=="cable")keys(o,{"physicalElementId","kind","materialId","area","initialTension"});else{require(kind=="passive","Unknown axial member kind");keys(o,{"physicalElementId","kind","materialId","area"});}
            uint id=integer(o["physicalElementId"]);require(rodPositions.count(id)&&members.insert(id).second,"Unknown or duplicate actual member binding");auto materialId=string(o["materialId"]);require(materials.count(materialId),"Member material source missing");usedMaterials.insert(materialId);const auto &m=materials.at(materialId);double area=number(o["area"]);require(area>0,"Nonpositive rod area");uint position=rodPositions.at(id);const auto &e=candidate.getElement(position);double chord=length(candidate,e);
            uint lineId=std::numeric_limits<uint>::max(),occurrences=0;for(uint line=0;line<candidate.getNLines();++line){const auto &g=candidate.getLine(line);for(uint j=0;j<g.size();++j)if(g.get(j)==position){lineId=line;++occurrences;}}require(occurrences==1,"Each actual rod must belong to exactly one RLine");
            double young=rStructuralPiezoReduceFreeStress(m,{0},0).elasticity[0];auto &line=lines[lineId];require(!line.set||line.area==area,"Different areas in one RLine require explicit member grouping");bool host=kind!="active";
            if(host&&line.host)require(line.young==young&&line.density==m.density,"Different passive/cable materials in one RLine require explicit member grouping; whole-preview automatic grouping is unavailable");
            if(!line.set||(host&&!line.host)){line.material=m;line.young=young;line.density=m.density;}line.area=area;line.set=true;line.host=line.host||host;
            if(kind=="active"){require(m.piezoelectric,"Active member requires piezoelectric material");QString port=string(o["portId"]);require(ports.count(port)&&usedPorts.insert(port).second,"Unknown port or unsupported shared axial circuit");double polarity=number(o["polarity"]);require(polarity==1||polarity==-1,"Invalid axial polarity");auto prop=rStructuralPiezoCompileAxialMember(m,area,chord);RStructuralTrussActiveMember member;member.physicalElementId=id;member.portId=port;member.referenceLength=chord;member.axialStiffness=prop.k;member.coupling=polarity*prop.g;member.capacitance=prop.Ce;member.massSource=RStructuralTrussMassSource::FixedMemberMass;member.fixedMass=prop.fixedMass;const auto &portSpec=ports.at(port);member.kind=portSpec.readout.readout==RStructuralPiezoReadoutMode::OpenCircuitVoltage?RStructuralTrussElectricalKind::ConductorCharge:RStructuralTrussElectricalKind::Voltage;member.initialValue=portSpec.initialValue;truss.activeMembers.push_back(member);}
            else {require(!m.piezoelectric,"Passive/cable source cannot hide active electrical tensors");if(kind=="passive")truss.passiveMembers.push_back({id,chord});else{RStructuralTrussCableMember member;member.physicalElementId=id;member.initialReferenceLength=chord;member.initialTension=o.contains("initialTension")?number(o["initialTension"]):0;require(member.initialTension>=0,"Cable initial tension must be nonnegative");truss.cableMembers.push_back(member);}}
        }
        require(members.size()==rodPositions.size()&&usedPorts.size()==ports.size()&&!ports.empty(),"Member/port provenance must cover every rod and use every electrical port");
        QJsonArray unusedMaterialIds;for(const auto &entry:materials)if(!usedMaterials.count(entry.first))unusedMaterialIds.append(entry.first);
        for(const auto &entry:lines){auto &g=candidate.getLine(entry.first);g.setCrossArea(entry.second.area);g.setMaterial(mechanical(entry.second.material));}
        std::map<uint,std::array<bool,3>> supportMask;std::map<uint,std::array<double,3>> supportValues;for(const auto &v:declarations.supports){supportMask[v.physicalNodeId][uint(v.dof)]=true;supportValues[v.physicalNodeId][uint(v.dof)]=v.value;}
        for(const auto &entry:supportMask){auto &g=candidate.getPoint(pointForNode(candidate,nodePositions.at(entry.first)));RBoundaryCondition bc(R_BOUNDARY_CONDITION_DISPLACEMENT);for(uint a=0;a<3;++a)setComponent(bc,displacement[a],supportValues[entry.first][a],entry.second[a]);g.addBoundaryCondition(bc);}
        const auto fields=[&](const std::vector<RStructuralPiezoLayeredNodeField> &values,int kind){for(const auto &v:values){auto q=translation(v.values);auto &g=candidate.getPoint(pointForNode(candidate,nodePositions.at(v.physicalNodeId)));if(kind==0){RBoundaryCondition bc(R_BOUNDARY_CONDITION_FORCE);for(uint a=0;a<3;++a)setComponent(bc,force[a],q[a],true);g.addBoundaryCondition(bc);}else{RInitialCondition ic(kind==1?R_INITIAL_CONDITION_DISPLACEMENT:R_INITIAL_CONDITION_VELOCITY);for(uint a=0;a<3;++a)setComponent(ic,kind==1?displacement[a]:velocity[a],q[a],true);g.addInitialCondition(ic);}}};fields(declarations.backgroundForces,0);fields(declarations.initialDisplacements,1);fields(declarations.initialVelocities,2);
        catalog.workpoint.available=declarations.workpoint.available;for(const auto &v:declarations.workpoint.nodes)catalog.workpoint.nodes.push_back({v.physicalNodeId,translation(v.q0),translation(v.f0)});for(const auto &v:declarations.workpoint.ports)catalog.workpoint.ports.push_back({v.portId,v.V0,v.Q0});p.setStructuralPiezoReadoutSetup(catalog);
        bool autoZeroWorkpoint=false;QJsonObject actualWorkpoint;
        if(!catalog.workpoint.available&&std::any_of(declarations.initialDisplacements.begin(),declarations.initialDisplacements.end(),[](const auto &v){return std::any_of(v.values.begin(),v.values.end(),[](double x){return x!=0;});})){
            for(const auto &v:declarations.supports)require(v.value==0,"Nonzero initial vibration with biased supports requires an explicit complete working point");
            for(const auto &v:declarations.backgroundForces)for(double x:v.values)require(x==0,"Nonzero initial vibration with background load requires an explicit complete working point");
            for(const auto &v:declarations.ports)require(v.initialValue==0,"Nonzero initial vibration with electrical bias requires an explicit complete working point");
            for(const auto &v:truss.cableMembers)require(v.initialTension==0,"Preloaded initial vibration requires an explicit complete working point");
            QJsonArray wn,wp;catalog.workpoint.available=true;
            for(const auto &v:nodePositions){catalog.workpoint.nodes.push_back({v.first,{}, {}});wn.append(QJsonObject{{"physicalNodeId",double(v.first)},{"q0",QJsonArray{0.,0.,0.,0.,0.,0.}},{"f0",QJsonArray{0.,0.,0.,0.,0.,0.}}});}
            for(const auto &v:catalog.ports){catalog.workpoint.ports.push_back({v.portId,0,0});wp.append(QJsonObject{{"portId",v.portId},{"V0",0.},{"Q0",0.}});}
            actualWorkpoint={{"available",true},{"nodes",wn},{"ports",wp}};autoZeroWorkpoint=true;p.setStructuralPiezoReadoutSetup(catalog);
        }
        // Install the detached declaration through its builder reference before
        // binding; the public setter accepts only already certified sources.
        p.getStructuralTrussDynamicsSetup()=truss;truss.bindToModel(candidate);p.setStructuralTrussDynamicsSetup(truss);catalog.validate(candidate);
        QJsonObject stored=r;if(autoZeroWorkpoint)stored["workpoint"]=actualWorkpoint;stored.remove("template");stored["replaceLegacyConditions"]=false;RStructuralPiezoAxialSetup setup;setup.enabled=true;setup.recipeConfiguration=QJsonDocument(stored).toJson(QJsonDocument::Compact);setup.bindToModel(candidate);p.setStructuralPiezoAxialSetup(setup);
        const auto op=RStructuralPiezoAxialOperators::compile(candidate);double totalMass=0;for(double mass:op.nativeSystem.nodalLumpedMass)totalMass+=mass;
        QJsonArray portReport;for(const auto &v:catalog.ports)portReport.append(QJsonObject{{"portId",v.portId},{"role",int(v.role)},{"readout",int(v.readout)},{"unit",rStructuralPiezoReadoutUnit(v.readout)}});
        result.valid=true;result.code="ok";result.message="Whole axial experiment preview compiled";result.candidate=std::move(candidate);result.normalizedArguments=r;result.normalizedArguments["replaceLegacyConditions"]=replace;result.units=si();result.invalidation={{"results",true},{"restart",true},{"modes",true},{"rom",true},{"controllerGains",true},{"approval",true}};for(auto it=r.begin();it!=r.end();++it)result.changedPaths.append("/"+it.key());for(const char *path:{"/problemSetup","/results","/timeSolver","/nodalPointEntities","/lineMaterials"})result.changedPaths.append(path);
        result.report={{"family","axialRod"},{"totalMass",totalMass},{"massUnit","kg"},{"fullDofs",int(op.full.dofs)},{"freeDofs",int(op.free.dofs)},{"denseDofLimit",2048},{"members",int(members.size())},{"ports",portReport},{"sourceFingerprint",setup.sourceFingerprint},{"parentTrussSourceFingerprint",truss.sourceFingerprint},{"removedLegacyConditions",removed},{"legacyConditionPolicy",bound?"Previously bound recipe-owned loads/supports are REPLACED":"Old loads/supports are REPLACED by the recipe, not migrated"},{"materialQualification","declared tensors compiled to native scalar replacement laws; production backend independently certifies equivalence"},{"unusedMaterialIds",unusedMaterialIds},{"materialLibraryScope","Unused materials are validated source declarations; they do not contribute to plant assembly"},{"massAuthority","actual fixed native nodal lumped mass; active replacement does not add host mass"},{"certification","Actual native M/K/G/Ce and full reference tangent certified"},{"designBackendRequired",true},{"modalBackendRequired",declarations.damping.kind!=RStructuralPiezoDampingKind::None},{"declarationValidation","Damping, controller design and external source/dependency/environment certification require the production backend before approval"},{"initialTime",initial},{"targetTime",times.back()},{"freshSourceInitialization",true},{"oldResultsInvalidated",true}};
        result.report["automaticLineGrouping"]=regrouped;
        result.report["autoZeroWorkpoint"]=autoZeroWorkpoint;if(autoZeroWorkpoint)result.normalizedArguments["workpoint"]=actualWorkpoint;
    }catch(const RError &e){result.code="invalid_recipe";result.message=e.getMessage();}catch(const std::exception &e){result.code="invalid_recipe";result.message=QString::fromUtf8(e.what());}
    if(!result.valid)result.diagnostics.append(QJsonObject{{"code",result.code},{"message",result.message}});return result;
}

QJsonObject RPiezoAxialExperimentService::recipeFromModel(const RModel &source){
    try{const auto &s=source.getProblemSetup().getStructuralPiezoAxialSetup();require(s.enabled&&!s.sourceFingerprint.isEmpty(),"No bound axial experiment");s.validate(source);QJsonParseError error;auto doc=QJsonDocument::fromJson(s.recipeConfiguration,&error);require(error.error==QJsonParseError::NoError&&doc.isObject(),"Invalid stored actual axial recipe");auto r=doc.object();r.remove("template");r["replaceLegacyConditions"]=false;r["dt"]=source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep();r["steps"]=int(source.getProblemSetup().getStructuralDynamicsSetup().getNSteps());return r;}catch(...){return {};}
}
