#include "piezo_axial_backend_bridge.h"
#include "rpiezo_axial_experiment_service.h"
#include "piezo_numeric_transport.h"
#include <rml_model.h>
#include <rparallel_execution_context.h>
#include <rstructural_piezo_axial_operators.h>
#include <rstructural_piezo_element_operators.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
namespace {
using V=std::vector<double>;
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
double finite(double v){require(std::isfinite(v),"Native backend nonfinite numeric value");return v;}
QByteArray canonical(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject object(const QByteArray &bytes,const char *label){
    QJsonParseError e;const auto d=QJsonDocument::fromJson(bytes,&e);
    if(e.error!=QJsonParseError::NoError || !d.isObject())throw std::invalid_argument(label);return d.object();
}
QJsonArray array(const V &v){QJsonArray a;for(double x:v)a.append(finite(x));return a;}
QJsonArray matrix(const V &v,size_t rows,size_t cols){
    require(v.size()==rows*cols,"Native backend matrix shape mismatch");QJsonArray a;
    for(size_t i=0;i<rows;++i){QJsonArray row;for(size_t j=0;j<cols;++j)row.append(finite(v[i*cols+j]));a.append(row);}return a;
}
V vector(const QJsonValue &value,size_t n){
    require(value.isArray(),"Backend vector missing");const auto a=value.toArray();require(size_t(a.size())==n,"Backend vector dimension mismatch");V v;
    for(const auto &x:a){require(x.isDouble(),"Backend vector must contain numbers");v.push_back(finite(x.toDouble()));}return v;
}
V dense(const QJsonValue &value,size_t n){
    require(value.isArray() && size_t(value.toArray().size())==n,"Backend C dimensions mismatch");V out;
    for(const auto &row:value.toArray()){const auto v=vector(row,n);out.insert(out.end(),v.begin(),v.end());}return out;
}

QString identity(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
using Ops=RStructuralPiezoAxialOperators;
const QString axes[]={"X","Y","Z"};
struct Output {QString id,unit,quantity;size_t fullIndex=0;bool mechanical=false;};
struct Channels {V B,Lq,Lv,La,D,forceMap,va0; QJsonArray dofs,inputs,outputs,nodes,forceCatalog;std::vector<Output> readouts;};
Channels channels(const Ops &op,const QJsonArray &mechanical) {
    const size_t r=op.free.dofs,n=op.full.dofs;
    Channels out;std::vector<size_t> actuatorColumns;
    for(size_t column=0;column<op.inputs.size();++column)if(!op.inputs[column].portId.isEmpty()) {
        const auto &input=op.inputs[column];require(input.unit=="V","Axial actuator must be a voltage input");
        actuatorColumns.push_back(column);out.inputs.append(QJsonArray{input.portId,"V"});
        const auto port=std::find(op.orderedPortIds.begin(),op.orderedPortIds.end(),input.portId);
        require(port!=op.orderedPortIds.end(),"Axial actuator port missing");out.va0.push_back(op.V0[size_t(port-op.orderedPortIds.begin())]);
    }
    const size_t m=actuatorColumns.size();require(r>0,"Axial backend requires free physical DOFs");out.B.assign(r*m,0);
    for(size_t i=0;i<r;++i){const auto &d=op.freeDofs[i];out.dofs.append(QJsonArray{QString("node:%1:%2").arg(d.physicalNodeId).arg(axes[d.axis]),"m"});for(size_t j=0;j<m;++j)out.B[i*m+j]=op.free.B[i*op.free.inputs+actuatorColumns[j]];}
    std::set<QString> used;
    for(size_t row=0;row<op.outputs.size();++row){const auto &v=op.outputs[row];if(v.actionRecord)continue;
        require(used.insert(v.portId).second,"Duplicate independent axial readout");out.readouts.push_back({v.portId,v.unit,{},0,false});out.outputs.append(QJsonArray{v.portId,v.unit});
        for(size_t j=0;j<r;++j){out.Lq.push_back(op.free.Lq[row*r+j]);out.Lv.push_back(op.free.Lv[row*r+j]);out.La.push_back(op.free.La[row*r+j]);}
        for(size_t j=0;j<m;++j)out.D.push_back(op.free.D[row*op.free.inputs+actuatorColumns[j]]);
    }
    for(const auto &value:mechanical){require(value.isObject(),"Mechanical output must be an object");const auto spec=value.toObject();
        require((spec.size()==4 || (spec.size()==5 && spec.contains("unit"))) && spec.contains("id") && spec.contains("physicalNodeId") && spec.contains("dof") && spec.contains("quantity"),"Axial mechanical output fields mismatch");
        const auto id=spec.value("id").toString(),axis=spec.value("dof").toString(),quantity=spec.value("quantity").toString();
        require(!id.isEmpty() && used.insert(id).second,"Mechanical output ID empty or duplicated");
        require(spec.value("physicalNodeId").isDouble(),"Mechanical node ID must be numeric");const double node=spec.value("physicalNodeId").toDouble();
        require(node>0 && node<=std::numeric_limits<uint>::max() && node==std::floor(node),"Mechanical node ID invalid");
        const auto a=std::find(std::begin(axes),std::end(axes),axis);require(a!=std::end(axes),"Axial outputs support physical X/Y/Z only");
        const auto d=std::find_if(op.fullDofs.begin(),op.fullDofs.end(),[&](const auto &x){return x.physicalNodeId==uint(node) && x.axis==uint(a-std::begin(axes));});require(d!=op.fullDofs.end(),"Mechanical output node missing");
        require(quantity=="displacement" || quantity=="velocity" || quantity=="acceleration","Axial output quantity unsupported");
        QString unit=quantity=="displacement"?"m":quantity=="velocity"?"m/s":"m/s^2";
        if(spec.contains("unit"))require(spec.value("unit").isString() && spec.value("unit").toString()==unit,"Axial mechanical output unit mismatch");
        out.readouts.push_back({id,unit,quantity,d->fullIndex,true});out.outputs.append(QJsonArray{id,unit});
        for(size_t j=0;j<r;++j){const double select=op.freeToFull[j]==d->fullIndex?1:0;out.Lq.push_back(quantity=="displacement"?select:0);out.Lv.push_back(quantity=="velocity"?select:0);out.La.push_back(quantity=="acceleration"?select:0);}
        for(size_t j=0;j<m;++j)out.D.push_back(0);
    }
    require(!out.readouts.empty(),"Axial backend requires independent outputs");out.forceMap.assign(r*n,0);for(size_t i=0;i<r;++i)out.forceMap[i*n+op.freeToFull[i]]=1;
    for(size_t row=0;row<n;++row){const auto &d=op.fullDofs[row];V direction(6,0);direction[d.axis]=1;out.forceCatalog.append(QJsonObject{{"channelId",QString("node:%1:%2").arg(d.physicalNodeId).arg(axes[d.axis])},{"physicalNodeId",double(d.physicalNodeId)},{"dof",axes[d.axis]},{"unit","N"},{"physicalIndex",double(row)},{"coordinateSystem","global"},{"physicalDirection",array(direction)}});if(row%3==0)out.nodes.append(double(d.physicalNodeId));}
    return out;
}
QJsonObject initialization(const Ops &op,const Channels &o,const V &C,const V &bias,const QJsonObject &design,const QJsonObject &controller,const QJsonObject &excitation,uint N,const QString &fp,double time,double dt){
    const size_t r=op.free.dofs,n=op.full.dofs,m=o.va0.size(),ny=o.readouts.size();require(excitation.value("channels").isArray(),"Excitation channels must be an array");
    QJsonArray catalog=o.forceCatalog,disturbances,configured=RPiezoExperimentDeclarations::forceChannels(excitation);std::vector<size_t> columns;std::set<QString> used;
    for(const auto &value:configured){require(value.isObject(),"Excitation channel must be an object");const auto channel=value.toObject();require(channel.value("channelId").isString() && channel.value("unit").isString(),"Force channel ID/unit missing");const auto id=channel.value("channelId").toString();const bool first=used.insert(id).second;int index=-1;for(int i=0;i<catalog.size();++i)if(catalog[i].toObject().value("channelId").toString()==id){index=i;break;}require(index>=0,"Excitation requests an unavailable physical XYZ force");auto binding=catalog[index].toObject();require(channel.value("unit").toString()=="N","Axial physical force unit must be N");if(!first)continue;binding["disturbanceColumn"]=double(columns.size());catalog[index]=binding;columns.push_back(size_t(binding.value("physicalIndex").toDouble()));disturbances.append(QJsonArray{id,"N"});}
    const size_t nf=columns.size();V Bforce(r*nf),Dforce(ny*nf),S(n*nf);for(size_t j=0;j<nf;++j)S[columns[j]*nf+j]=1;for(size_t i=0;i<r;++i)for(size_t j=0;j<nf;++j)Bforce[i*nf+j]=o.forceMap[i*n+columns[j]];
    return {{"op","initialize"},{"sourceFingerprint",fp},{"M",matrix(op.free.M,r,r)},{"K",matrix(op.free.K,r,r)},{"C",matrix(C,r,r)},{"B",matrix(o.B,r,m)},
        {"Lq",matrix(o.Lq,ny,r)},{"Lv",matrix(o.Lv,ny,r)},{"La",matrix(o.La,ny,r)},{"D",matrix(o.D,ny,m)},{"physicalForceDirections",matrix(S,n,nf)},{"sourceFrame",QJsonObject{{"family","axialRod"},{"frame",QJsonObject{{"origin",QJsonArray{0,0,0}},{"ex",QJsonArray{1,0,0}},{"ey",QJsonArray{0,1,0}},{"ez",QJsonArray{0,0,1}}}}}},{"Bforce",matrix(Bforce,r,nf)},{"Dforce",matrix(Dforce,ny,nf)},{"disturbanceInputs",disturbances},
        {"initialTime",finite(time)},{"biasForce",array(bias)},{"dt",dt},{"controlEveryFESteps",double(N)},{"dofs",o.dofs},{"inputs",o.inputs},{"outputs",o.outputs},{"actuatorVoltages0",array(o.va0)},{"nodeIds",o.nodes},{"physicalComponentsPerNode",3},{"forceCatalog",catalog},
        {"configuration",QJsonObject{{"design",design},{"controller",controller},{"excitation",excitation}}}};
}
V reduced(const V &full,const Ops &op){const size_t r=op.free.dofs,n=op.full.dofs;require(full.size()==n*n,"Full axial C dimensions mismatch");V out(r*r);for(size_t i=0;i<r;++i)for(size_t j=0;j<r;++j)out[i*r+j]=full[op.freeToFull[i]*n+op.freeToFull[j]];return out;}
V lifted(const V &C,const Ops &op){const size_t r=op.free.dofs,n=op.full.dofs;require(C.size()==r*r,"Free axial C dimensions mismatch");V out(n*n);for(size_t i=0;i<r;++i)for(size_t j=0;j<r;++j)out[op.freeToFull[i]*n+op.freeToFull[j]]=C[i*r+j];return out;}
QJsonArray readouts(const Ops &op,const Channels &o,const RStructuralTrussDynamicsState &state,const std::vector<RStructuralPiezoNativeReadoutSample> &samples,const std::set<QString> &selected){
    QJsonArray out;for(const auto &entry:o.readouts){if(!selected.count(entry.id))continue;double value=0;
        if(entry.mechanical){const auto &v=entry.quantity=="displacement"?state.displacements:entry.quantity=="velocity"?state.velocities:state.accelerations;require(v.size()==op.full.dofs,"Native physical readout dimensions mismatch");value=v[entry.fullIndex];if(entry.quantity=="displacement")value-=op.q0[entry.fullIndex];}
        else {const auto p=std::find_if(samples.begin(),samples.end(),[&](const auto &x){return x.portId==entry.id;});require(p!=samples.end() && !p->actionRecord && p->unit==entry.unit,"Native electrical readout identity mismatch");value=p->deltaValue;}
        out.append(QJsonObject{{"portId",entry.id},{"unit",entry.unit},{"deltaValue",finite(value)},{"actionRecord",false}});
    }return out;
}
V forceFeedthrough(const Ops &op,const Channels &o){const size_t r=op.free.dofs,n=op.full.dofs,ny=o.readouts.size();V out(ny*n);for(size_t row=0;row<ny;++row)for(size_t i=0;i<r;++i){require(op.free.M[i*r+i]>0,"Axial mass diagonal must be positive");for(size_t j=0;j<r;++j)if(i!=j)require(op.free.M[i*r+j]==0,"Axial native mass must be lumped");out[row*n+op.freeToFull[i]]=finite(o.La[row*r+i]/op.free.M[i*r+i]);}return out;}

void keys(const QJsonObject &o,const std::set<QString> &required,const std::set<QString> &optional={}){
    for(const auto &k:required)require(o.contains(k),"Material/member source field missing");for(auto i=o.constBegin();i!=o.constEnd();++i)require(required.count(i.key()) || optional.count(i.key()),"Unknown material/member source field");
}
double numeric(const QJsonObject &o,const char *key){require(o.value(key).isDouble(),"Material/member source number required");return finite(o.value(key).toDouble());}
QString text(const QJsonObject &o,const char *key){require(o.value(key).isString() && !o.value(key).toString().isEmpty(),"Material/member source identity required");return o.value(key).toString();}
void closeLocal(double actual,double expected,const char *message){const double scale=std::max(std::abs(actual),std::abs(expected));require(std::isfinite(actual) && std::isfinite(expected) && (scale==0 || std::abs(actual/scale-expected/scale)<=1e-10),message);}
template<size_t N> std::array<double,N> fixedArray(const QJsonValue &value){const auto v=vector(value,N);std::array<double,N> out;std::copy(v.begin(),v.end(),out.begin());return out;}
void certifyMaterials(const QJsonObject &recipe,const RModel &model,const RStructuralTrussDynamicsModelAdapter &adapter,QJsonArray &exported,QString &qualification){
    const bool declared=recipe.contains("materials") || recipe.contains("members");
    if(!declared){qualification="effective-native-scalar-source-no-continuum-provenance";return;}
    require(recipe.value("materials").isArray() && recipe.value("members").isArray(),"Material provenance requires complete materials and members");
    std::map<QString,RStructuralPiezoContinuumMaterial> materials;std::set<QString> usedMaterials;
    for(const auto &value:recipe.value("materials").toArray()){
        require(value.isObject(),"Material must be an object");const auto o=value.toObject();
        keys(o,{"materialId","form","elasticityOrCompliance","coupling","permittivity","density","piezoelectric","sourceKind","sourceId","sourceSha256"},{"notes"});
        const auto id=text(o,"materialId"),form=text(o,"form"),sourceKind=text(o,"sourceKind"),sha=text(o,"sourceSha256");text(o,"sourceId");
        require(sourceKind=="example" || sourceKind=="vendor" || sourceKind=="calibration","Unknown material provenance kind");
        require(sha.size()==64 && std::all_of(sha.begin(),sha.end(),[](QChar c){const auto u=c.unicode();return (u>='0' && u<='9') || (u>='a' && u<='f');}),"Invalid declared material source SHA256");
        if(o.contains("notes"))require(o.value("notes").isString(),"Material notes must be a string");
        require(o.value("piezoelectric").isBool(),"Material piezoelectric flag required");const bool piezo=o.value("piezoelectric").toBool();
        const auto elasticity=fixedArray<36>(o.value("elasticityOrCompliance"));const auto coupling=fixedArray<18>(o.value("coupling"));const auto permittivity=fixedArray<9>(o.value("permittivity"));const double rho=numeric(o,"density");
        require(rho>0 && (form=="stressCharge" || form=="strainCharge"),"Invalid material form/density");
        RStructuralPiezoContinuumMaterial material;
        if(form=="strainCharge" && piezo)material=RStructuralPiezoContinuumMaterial::fromStrainCharge(elasticity,coupling,permittivity,rho);
        else if(form=="strainCharge") {require(std::all_of(coupling.begin(),coupling.end(),[](double x){return x==0;}) && std::all_of(permittivity.begin(),permittivity.end(),[](double x){return x==0;}),"Passive compliance source contains hidden electrical tensors");std::array<double,18> zero{};std::array<double,9> dummy{{1,0,0,0,1,0,0,0,1}};material=RStructuralPiezoContinuumMaterial::fromStrainCharge(elasticity,zero,dummy,rho);material.piezoelectric=false;material.permittivity={};material.validate();}
        else {material.elasticity=elasticity;material.coupling=coupling;material.permittivity=permittivity;material.density=rho;material.piezoelectric=piezo;material.validate();}
        require(materials.emplace(id,material).second,"Duplicate material source ID");auto entry=o;entry["certificationScope"]="declared-source-tensors-and-native-scalar-equivalence;source-document-not-authenticated";exported.append(entry);
    }
    const auto &system=adapter.system();const auto &native=model.getProblemSetup().getStructuralTrussDynamicsSetup();std::set<uint> declaredIds;V mass(system.referenceNodes.size());
    for(const auto &value:recipe.value("members").toArray()){
        require(value.isObject(),"Member must be an object");const auto o=value.toObject();const auto kind=text(o,"kind"),id=text(o,"materialId");
        keys(o,{"physicalElementId","kind","materialId","area"},kind=="active"?std::set<QString>{"portId","polarity"}:kind=="cable"?std::set<QString>{"initialTension"}:std::set<QString>{});
        const double raw=numeric(o,"physicalElementId"),area=numeric(o,"area");require(raw>0 && raw<=std::numeric_limits<uint>::max() && raw==std::floor(raw) && area>0,"Invalid member physical ID/area");const uint physical=uint(raw);
        require(declaredIds.insert(physical).second,"Duplicate declared member ID");const auto found=adapter.maps().memberByPhysicalId.find(physical);require(found!=adapter.maps().memberByPhysicalId.end(),"Declared member unavailable in native source");const auto &member=system.members[found->second];
        require(materials.count(id)==1,"Member material source missing");usedMaterials.insert(id);const auto &material=materials.at(id);
        const auto &left=system.referenceNodes[member.node1],&right=system.referenceNodes[member.node2];
        const double length=std::hypot(right[0]-left[0],right[1]-left[1],right[2]-left[2]);require(length>0 && std::isfinite(length),"Member original chord invalid");
        const auto *actualLine=[&]{for(uint line=0;line<model.getNLines();++line){const auto &group=model.getLine(line);for(uint i=0;i<group.size();++i)if(model.getElementPhysicalId(group.get(i))==physical)return &group;}throw std::invalid_argument("Declared member line geometry missing");}();closeLocal(actualLine->getCrossArea(),area,"Declared material area differs from native mesh area");
        const double memberMass=finite(material.density*area*length);require(memberMass>0,"Declared original member mass must be positive");mass[member.node1]=finite(mass[member.node1]+memberMass*.5);mass[member.node2]=finite(mass[member.node2]+memberMass*.5);
        if(kind=="active"){
            require(material.piezoelectric && o.contains("portId") && o.contains("polarity"),"Active material needs piezoelectric tensors, port and polarity");const auto port=text(o,"portId");const double polarity=numeric(o,"polarity");require(polarity==1 || polarity==-1,"Active electrode polarity must be +1 or -1");
            const auto *law=std::get_if<RStructuralTrussPiezoLaw>(&member.law);require(law,"Declared active member differs from native classification");const auto expected=rStructuralPiezoCompileAxialMember(material,area,length);
            closeLocal(law->referenceLength,length,"Active reference length differs from original geometry");closeLocal(law->axialStiffness,expected.k,"Material/native active stiffness mismatch");closeLocal(law->coupling,polarity*expected.g,"Material/native active coupling or polarity mismatch");closeLocal(law->capacitance,expected.Ce,"Material/native active capacitance mismatch");
            const auto spec=std::find_if(native.activeMembers.begin(),native.activeMembers.end(),[&](const auto &x){return x.physicalElementId==physical;});require(spec!=native.activeMembers.end() && spec->portId==port && spec->massSource==RStructuralTrussMassSource::FixedMemberMass,"Active port/fixed replacement mass source mismatch");closeLocal(spec->fixedMass,expected.fixedMass,"Material/native active fixed replacement mass mismatch");
        } else {
            require(!material.piezoelectric,"Passive/cable source cannot hide piezoelectric tensors");
            const auto &lineMaterial=actualLine->getMaterial();const auto densityIndex=lineMaterial.findPosition(RMaterialProperty::Density);require(densityIndex<lineMaterial.size() && !lineMaterial.get(densityIndex).empty(),"Passive/cable native density missing");
            if(kind=="passive")closeLocal(lineMaterial.get(densityIndex).get(293.15),material.density,"Passive material/native member density mismatch");
            if(kind=="cable"){const auto spec=std::find_if(native.cableMembers.begin(),native.cableMembers.end(),[&](const auto &x){return x.physicalElementId==physical;});require(spec!=native.cableMembers.end(),"Native cable source declaration missing");if(spec->massSource==RStructuralTrussMassSource::FixedMemberMass)closeLocal(spec->fixedMass,memberMass,"Cable material/native fixed mass mismatch");else closeLocal(lineMaterial.get(densityIndex).get(293.15),material.density,"Cable material/native density mismatch");}
            const double rigidity=finite(rStructuralPiezoReduceFreeStress(material,{0},0).elasticity[0]*area);
            if(kind=="passive"){const auto *law=std::get_if<RStructuralTrussPassiveLaw>(&member.law);require(law,"Declared passive member differs from native classification");closeLocal(law->referenceLength,length,"Passive reference length differs from original geometry");closeLocal(law->axialRigidity,rigidity,"Material/native passive rigidity mismatch");}
            else {require(kind=="cable","Unknown declared member kind");const auto *law=std::get_if<RStructuralTrussCableLaw>(&member.law);require(law,"Declared cable differs from native classification");closeLocal(law->axialRigidity,rigidity,"Material/native cable rigidity mismatch");if(o.contains("initialTension")){const double tension=numeric(o,"initialTension");require(tension>=0,"Declared cable tension must be nonnegative");closeLocal(law->initialTension,tension,"Declared/native cable tension mismatch");}else require(law->initialTension==0,"Nonzero native cable tension needs explicit source initialTension");const auto *input=std::get_if<RStructuralTrussCableLength>(&adapter.initialCompleteInputs().memberInputs[found->second]);require(input,"Cable length input missing");closeLocal(input->value,length,"Cable reference length differs from original geometry");}
        }
    }
    require(declaredIds.size()==system.members.size(),"Material provenance must cover every native member");
    for(size_t i=0;i<mass.size();++i)closeLocal(system.nodalLumpedMass[i],mass[i],"Declared density-area-chord masses differ from actual native frozen mass");
    qualification="declared-continuum-tensors-certified-to-native-effective-laws-and-fixed-mass";
}

}
PiezoAxialBackendBridge::Result PiezoAxialBackendBridge::create(const RModel &source){const auto runtime=PiezoNumericTransport::defaultRuntime();return create(source,runtime.first,runtime.second);}
PiezoAxialBackendBridge::Result PiezoAxialBackendBridge::create(const RModel &source,const QString &python,const QString &moduleRoot){
    RParallelExecutionConfig policy;policy.setMode(RParallelExecutionMode::SerialDeterministic);const RParallelExecutionContext execution(policy);
    const auto &setup=source.getProblemSetup().getStructuralPiezoAxialSetup();setup.validate(source);require(setup.enabled,"Axial source declaration must be enabled");
    // Compile ONLY the independent source definition. Results and old runtime never supply initial data.
    RModel definition(source);definition.getProblemSetup().setRestart(false);const RStructuralTrussDynamicsModelAdapter adapter(definition);const auto op=Ops::compile(definition);
    const auto recipe=object(setup.recipeConfiguration,"Axial recipe must be whole JSON");const auto fingerprint=setup.computeSourceFingerprint(source);const double dt=adapter.nominalTimeStep(),initialTime=adapter.initialTime();const uint steps=adapter.targetSteps();
    if(recipe.contains("frame")){
        const QJsonObject globalFrame{{"origin",QJsonArray{0,0,0}},{"ex",QJsonArray{1,0,0}},{"ey",QJsonArray{0,1,0}},{"ez",QJsonArray{0,0,1}}};
        require(recipe.value("frame").isObject() && recipe.value("frame").toObject()==globalFrame,"Axial recipe frame must be exact global identity");
    }
    const auto &legacy=source.getProblemSetup().getStructuralDynamicsSetup();require(legacy.getRayleighAlpha()==0 && legacy.getRayleighBeta()==0 && legacy.getModalDampingRatios().empty(),"Axial damping authority cannot coexist with legacy dynamic damping");
    if(recipe.contains("dt"))require(recipe.value("dt").isDouble() && recipe.value("dt").toDouble()==dt,"Recipe dt differs from actual FE clock");if(recipe.contains("steps"))require(recipe.value("steps").isDouble() && recipe.value("steps").toDouble()==steps,"Recipe steps differ from actual FE clock");
    auto config=[&](const char *key,QJsonObject fallback){if(!recipe.contains(key))return fallback;require(recipe.value(key).isObject(),"Axial configuration must be an object");return recipe.value(key).toObject();};
    auto design=config("design",{}),controller=config("controller",{{"kind","none"}}),excitation=config("excitation",{{"channels",QJsonArray{}}});
    require(!excitation.contains("external") && !excitation.contains("executable"),"Executable excitation is unsupported");
    uint N=1;if(controller.contains("controlEveryFESteps")){const auto value=controller.take("controlEveryFESteps");require(value.isDouble() && value.toDouble()>=1 && value.toDouble()<=1000000 && value.toDouble()==std::floor(value.toDouble()),"Axial control FE stride invalid");N=uint(value.toDouble());}
    require(!design.contains("outputs") || design.value("outputs").isArray(),"Design mechanical outputs must be an array");const auto mechanical=design.take("outputs").toArray();auto requestedDamping=design.take("damping");
    const auto o=channels(op,mechanical);require(!o.va0.empty() || controller.value("kind").toString()=="none","A pure sensing axial source requires controller none");const size_t r=op.free.dofs,n=op.full.dofs,m=o.va0.size(),ny=o.readouts.size(),p=op.full.ports;
    for(size_t j=0;j<m;++j){const auto id=o.inputs[int(j)].toArray()[0].toString();const auto member=adapter.maps().memberByPortId.at(id);const auto *actual=std::get_if<RStructuralTrussVoltage>(&adapter.initialCompleteInputs().memberInputs.at(member));require(actual && actual->value==o.va0[j],"Initial actual actuator voltage differs from workpoint reference; separate initialApplied API required");}
    QJsonArray materials;QString materialQualification;certifyMaterials(recipe,definition,adapter,materials,materialQualification);
    V bias(r);for(size_t i=0;i<r;++i){const auto row=op.freeToFull[i];bias[i]=finite(adapter.initialCompleteInputs().externalForces.at(row)-op.f0[row]);}
    auto provider=std::make_shared<PiezoNumericTransport>(python,moduleRoot);auto damping=config("damping",{{"kind","none"}});require(damping.value("kind").isString(),"Damping kind missing");QString kind=damping.value("kind").toString();
    if(kind=="uniformModal")kind="uniform";if(kind=="perMode")kind="per_mode";if(kind=="perCluster")kind="per_cluster";damping["kind"]=kind;
    require(kind=="none" || kind=="rayleigh" || kind=="uniform" || kind=="per_mode" || kind=="per_cluster" || kind=="imported","Unknown axial source damping authority");
    auto clusterEnds=damping.take("clusterEnds");if(clusterEnds.isArray() && clusterEnds.toArray().isEmpty())clusterEnds=QJsonValue(QJsonValue::Undefined);
    if(damping.contains("importedMatrix")){require(kind=="imported" || damping.value("importedMatrix").toArray().isEmpty(),"Irrelevant imported damping matrix");if(kind=="imported"){require(damping.value("importedDimension").isDouble() && damping.value("importedDimension").toDouble()==n,"Imported physical C dimension mismatch");require(!damping.contains("imported_C"),"Duplicate imported damping matrix");damping["imported_C"]=matrix(vector(damping.value("importedMatrix"),n*n),n,n);}damping.remove("importedMatrix");}
    if(damping.contains("importedDimension")){require(kind=="imported" || damping.value("importedDimension").toDouble()==0,"Irrelevant imported damping dimension");damping.remove("importedDimension");}
    for(const auto *key:{"sourceId","sourceSha256"}){if(kind!="imported" && damping.contains(key))require(damping.value(key).isString() && damping.value(key).toString().isEmpty(),"Irrelevant damping source metadata");damping.remove(key);}
    for(const auto *key:{"rayleighAlpha","rayleighBeta","alpha","beta"})if(kind!="rayleigh" && damping.contains(key)){require(damping.value(key).isDouble() && damping.value(key).toDouble()==0,"Irrelevant Rayleigh damping parameter");damping.remove(key);}
    for(const auto *key:{"uniformRatio","zeta"})if(kind!="uniform" && kind!="per_mode" && kind!="per_cluster" && damping.contains(key)){require(damping.value(key).isDouble() && damping.value(key).toDouble()==0,"Irrelevant modal damping parameter");damping.remove(key);}
    if(damping.contains("ratios") && kind!="per_mode" && kind!="per_cluster"){require(damping.value("ratios").isArray() && damping.value("ratios").toArray().isEmpty(),"Irrelevant damping ratios");damping.remove("ratios");}
    if(damping.contains("uniformRatio") && kind!="uniform"){require(damping.value("uniformRatio").isDouble() && damping.value("uniformRatio").toDouble()==0,"Irrelevant uniform damping ratio");damping.remove("uniformRatio");}
    keys(damping,{"kind"},{"alpha","beta","rayleighAlpha","rayleighBeta","uniformRatio","ratios","zeta","imported_C"});
    if(damping.contains("uniformRatio")){require(!damping.contains("zeta"),"Duplicate modal damping ratio");damping["zeta"]=damping.take("uniformRatio");}if(damping.contains("ratios")){require(!damping.contains("zeta"),"Duplicate modal damping ratios");damping["zeta"]=damping.take("ratios");}
    if(damping.contains("rayleighAlpha")){require(!damping.contains("alpha"),"Duplicate Rayleigh alpha");damping["alpha"]=damping.take("rayleighAlpha");}if(damping.contains("rayleighBeta")){require(!damping.contains("beta"),"Duplicate Rayleigh beta");damping["beta"]=damping.take("rayleighBeta");}
    if(kind=="none")require(damping.size()==1,"None damping has irrelevant parameters");
    else if(kind=="rayleigh")require(damping.size()==3 && damping.contains("alpha") && damping.contains("beta"),"Rayleigh source parameters mismatch");
    else if(kind=="imported")require(damping.size()==2 && damping.contains("imported_C"),"Imported source parameters mismatch");
    else require(damping.size()==2 && damping.contains("zeta"),"Modal source parameters mismatch");
    V fullC(n*n),freeC(r*r);const bool modal=kind=="uniform" || kind=="per_mode" || kind=="per_cluster";
    if(kind=="imported"){require(damping.contains("imported_C"),"Imported damping requires full physical imported_C");fullC=dense(damping.value("imported_C"),n);freeC=reduced(fullC,op);damping["imported_C"]=matrix(freeC,r,r);}
    else if(kind=="rayleigh"){require(damping.value("alpha").isDouble() && damping.value("beta").isDouble(),"Rayleigh coefficients missing");const double alpha=finite(damping.value("alpha").toDouble()),beta=finite(damping.value("beta").toDouble());require(alpha>=0 && beta>=0,"Rayleigh coefficients must be nonnegative");for(size_t i=0;i<n*n;++i)fullC[i]=finite(alpha*op.full.M[i]+beta*op.full.K[i]);freeC=reduced(fullC,op);}
    if(!requestedDamping.isUndefined())require(requestedDamping.isObject() && (requestedDamping.toObject()==damping || requestedDamping.toObject()==config("damping",{{"kind","none"}})),"Design damping override differs from source authority");
    if(modal){auto initial=initialization(op,o,freeC,bias,{{"damping",damping}},{{"kind","none"}},{{"channels",QJsonArray{}}},N,fingerprint,initialTime,dt);const auto result=provider->request(initial);freeC=dense(result.value("authoritativeC"),r);fullC=lifted(freeC,op);
        if(!clusterEnds.isUndefined()){require(kind=="per_cluster" && clusterEnds.isArray(),"clusterEnds applies only to per-cluster source authority");const auto clusters=result.value("design").toObject().value("clusters").toArray(),ends=clusterEnds.toArray();require(clusters.size()==ends.size(),"Source modal clusters differ from actual complete eigensystem");uint next=0;for(int i=0;i<clusters.size();++i){for(const auto &index:clusters[i].toArray()){require(index.isDouble() && index.toDouble()==next,"Modal cluster order invalid");++next;}require(ends[i].isDouble() && ends[i].toDouble()==next,"Source clusterEnds differs from native eigensystem");}}
    }else require(clusterEnds.isUndefined(),"clusterEnds requires modal cluster damping");
    auto actualSystem=adapter.system();actualSystem.physicalDamping=fullC;const RStructuralTrussDynamics dampingCertificate(actualSystem); // Certifies complete physical C, including support cross terms.
    auto request=initialization(op,o,freeC,bias,design,controller,excitation,N,fingerprint,initialTime,dt);
    request["targetSteps"]=double(steps);
    quint64 initialBits=0;std::memcpy(&initialBits,&initialTime,sizeof(initialBits));QByteArray initialBytes(8,'\0');
    for(uint i=0;i<8;++i)initialBytes[i]=char((initialBits>>(8*i))&255);
    request["initialTimeBits"]=QString::fromLatin1(initialBytes.toHex());
    const auto result=provider->request(request);require(result.value("identity").isObject() && result.value("initialOpaqueState").isObject(),"Backend identity/state missing");const auto backendIdentity=result.value("identity").toObject();require(backendIdentity.value("sourceFingerprint").toString()==fingerprint,"Backend source identity differs from FE source");
    const auto forceDirect=forceFeedthrough(op,o);const size_t nf=size_t(request.value("disturbanceInputs").toArray().size());V Dforce(ny*nf);for(const auto &value:request.value("forceCatalog").toArray()){const auto binding=value.toObject();if(!binding.contains("disturbanceColumn"))continue;const size_t column=size_t(binding.value("disturbanceColumn").toDouble()),physical=size_t(binding.value("physicalIndex").toDouble());for(size_t row=0;row<ny;++row)Dforce[row*nf+column]=forceDirect[row*n+physical];}
    const auto forceAware=result.value("design").toObject().value("forceAware").toObject();require(forceAware.value("qualification").toString()=="complete-known-forcing" && forceAware.value("allConfiguredForcesMapped").toBool(),"Backend must certify complete physical forcing");const auto direct=forceAware.value("FE").toObject().value("continuous").toObject().value("D").toArray();require(size_t(direct.size())==ny,"Force-aware direct term shape mismatch");
    for(size_t row=0;row<ny;++row){const auto values=vector(direct[int(row)],m+nf);for(size_t col=0;col<nf;++col){const auto expected=Dforce[row*nf+col],actual=values[m+col];require(std::abs(actual-expected)<=1e-10*std::max({std::abs(actual),std::abs(expected),std::numeric_limits<double>::min()}),"Backend acceleration-force direct term differs from native mass");}}
    Result output;auto &cb=output.callbacks;cb.recipeConfiguration=setup.recipeConfiguration;cb.sourceFingerprint=fingerprint;cb.authoritativeFullC=fullC;cb.controlEveryFESteps=N;
    cb.backendIdentity="range-fea/native-python-axial/v1:"+identity(provider->runtimeIdentity().toUtf8()+canonical(backendIdentity));cb.forcingIdentity=backendIdentity.value("excitationSourceHash").toString()+":"+backendIdentity.value("excitationLoadedCodeHash").toString();cb.controllerIdentity=backendIdentity.value("configurationIdentity").toString();cb.initialOpaqueControllerState=canonical(result.value("initialOpaqueState").toObject());
    const auto envelopes=result.value("envelopes").toObject();const bool any=envelopes.value("lower").isArray() || envelopes.value("upper").isArray() || envelopes.value("deltaPerSample").isArray();if(any){require(envelopes.value("lower").isArray() && envelopes.value("upper").isArray() && envelopes.value("deltaPerSample").isArray(),"Amplitude and rate limits must be supplied together");const auto lower=vector(envelopes.value("lower"),m),upper=vector(envelopes.value("upper"),m),delta=vector(envelopes.value("deltaPerSample"),m);for(size_t i=0;i<m;++i){const double rate=finite(delta[i]/finite(N*dt));require(rate>0 && lower[i]<upper[i] && o.va0[i]>=lower[i] && o.va0[i]<=upper[i],"Axial voltage envelope invalid");cb.envelopes.push_back({lower[i],upper[i],rate});}}
    const bool sensorOnly=controller.value("stateSource").toString(controller.value("kind").toString()=="external"?"sensor_only":"full_fe")=="sensor_only";std::set<QString> selected;
    if(controller.contains("selectedOutputs")){require(controller.value("selectedOutputs").isArray(),"Selected outputs must be an array");for(const auto &value:controller.value("selectedOutputs").toArray()){require(value.isString() && selected.insert(value.toString()).second,"Selected output ID invalid");require(std::any_of(o.readouts.begin(),o.readouts.end(),[&](const auto &x){return x.id==value.toString();}),"Selected output is not independent");}}
    if(!sensorOnly || !controller.contains("selectedOutputs"))for(const auto &x:o.readouts)selected.insert(x.id);
    cb.perturbationForce=[provider,n](double time){return vector(provider->request({{"op","force"},{"time",finite(time)}}).value("forces"),n);};
    if(excitation.value("externalSources").isArray() && !excitation.value("externalSources").toArray().isEmpty()){
        // This selection is only a query context. Accepted state lives exclusively
        // in the NativeRun transaction; neither force query nor bind commits it.
        auto ledger=std::make_shared<QJsonObject>(result.value("initialOpaqueState").toObject());
        cb.bindForceState=[ledger](const QByteArray &bytes){
            const auto next=object(bytes,"Frozen force ledger must be JSON");const auto step=next.value("feStep");
            require(step.isDouble() && std::isfinite(step.toDouble()) && step.toDouble()>=0 && step.toDouble()<=1000000 && step.toDouble()==std::floor(step.toDouble()),"Frozen force ledger FE step invalid");*ledger=next;
        };
        cb.prepareFE=[provider](double time,uint step,const QByteArray &bytes){
            const auto answer=provider->request({{"op","force"},{"time",finite(time)},{"feStep",double(step)},{"opaqueState",object(bytes,"Force candidate must be JSON")}});
            require(answer.value("candidateOpaqueState").isObject(),"External force omitted candidate state");return canonical(answer.value("candidateOpaqueState").toObject());
        };
        cb.perturbationForce=[provider,ledger,n,initialTime,dt](double time){
            finite(time);const uint step=uint(ledger->value("feStep").toDouble());
            const auto clock=[=](uint index){return index?finite(initialTime+double(index)*dt):initialTime;};
            uint boundary=0;
            if(time==initialTime)boundary=0;
            else if(time==clock(step))boundary=step;
            else if(time==clock(step+1))boundary=step+1;
            else {const double index=finite((time-initialTime)/dt);require(index>=0 && index<=1000000,"Force ledger time outside canonical grid");boundary=uint(std::llround(index));}
            require(time==clock(boundary),"External force requires a canonical FE boundary");
            const uint interval=boundary?boundary-1:0;
            return vector(provider->request({{"op","force"},{"time",finite(time)},{"feStep",double(interval)},{"opaqueState",*ledger},{"replayOnly",true}}).value("forces"),n);
        };
        cb.intervalPerturbationForce=[provider,ledger,n](double time){return vector(provider->request({{"op","force"},{"time",finite(time)},{"feStep",ledger->value("feStep")},{"opaqueState",*ledger},{"replayOnly",true}}).value("forces"),n);};
    }
    cb.propose=[provider,op,o,sensorOnly,selected,r,m](const auto &state,const auto &samples,uint completed,uint tick,const QByteArray &old){const auto opaque=object(old,"Opaque state must be JSON");require(opaque.value("feStep").toDouble()==completed && opaque.value("controlTick").toDouble()==tick,"Proposal counters differ from opaque history");QJsonObject message{{"op","propose"},{"opaqueState",opaque},{"readouts",readouts(op,o,state,samples,selected)}};
        if(!sensorOnly){require(state.displacements.size()==op.full.dofs && state.velocities.size()==op.full.dofs,"FE physical state shape mismatch");V q(r),v(r);for(size_t i=0;i<r;++i){const auto row=op.freeToFull[i];q[i]=finite(state.displacements[row]-op.q0[row]);v[i]=state.velocities[row];}message["q"]=array(q);message["v"]=array(v);}
        const auto answer=provider->request(message);require(answer.value("candidateOpaqueState").isObject(),"Proposal candidate state missing");RStructuralPiezoAxialControllerProposal p;p.requestedVoltages=vector(answer.value("absoluteVoltages"),m);p.nextOpaqueState=canonical(answer.value("candidateOpaqueState").toObject());p.lastFrameBytes=canonical(answer);return p;};
    cb.postFE=[provider](const auto &,const auto &,uint completed,uint tick,const V &applied,const QByteArray &old){const auto answer=provider->request({{"op","postFE"},{"opaqueState",object(old,"Opaque state must be JSON")},{"appliedVoltages",array(applied)},{"feStep",double(completed)}});require(answer.value("candidateOpaqueState").isObject(),"postFE candidate state missing");const auto next=answer.value("candidateOpaqueState").toObject();require(next.value("controlTick").toDouble()==tick,"postFE control tick mismatch");return canonical(next);};
    cb.validateOpaque=[provider,N](const QByteArray &state,uint completed,uint tick,uint phase){require(phase==completed%N,"Axial controller FE phase mismatch");provider->request({{"op","validate"},{"opaqueState",object(state,"Opaque state must be JSON")},{"feStep",double(completed)},{"controlTick",double(tick)},{"phase","ready"}});};
    cb.validateAcceptedBoundary=[provider,m](const RStructuralPiezoAxialRunState &state){require(state.lastApplied.size()==m,"Accepted actuator dimensions mismatch");provider->request({{"op","validate"},{"opaqueState",object(state.opaqueControllerState,"Opaque state must be JSON")},{"feStep",double(state.completedFE)},{"controlTick",double(state.controlTick)},{"phase","ready"},{"appliedVoltages",array(state.lastApplied)},{"heldVoltages",array(state.heldRequested)}});};
    V T(n*r),P(n*n);double totalMass=0;for(double mass:op.nativeSystem.nodalLumpedMass)totalMass=finite(totalMass+mass);for(size_t i=0;i<r;++i)T[op.freeToFull[i]*r+i]=1;for(size_t i=0;i<n;++i)P[i*n+i]=1;
    QJsonArray ports,nodes,members;for(size_t i=0;i<p;++i){const auto &id=op.orderedPortIds[i];const auto &catalog=adapter.piezoReadoutSetup().ports;const auto port=std::find_if(catalog.begin(),catalog.end(),[&](const auto &x){return x.portId==id;});require(port!=catalog.end(),"Export port classification missing");ports.append(QJsonObject{{"portId",id},{"role",int(port->role)},{"readout",int(port->readout)},{"unit",rStructuralPiezoReadoutUnit(port->readout)},{"actionRecord",port->readout==RStructuralPiezoReadoutMode::None},{"Cf",port->Cf},{"amplifierPolarity",port->amplifierPolarity},{"Qref",port->Qref},{"VoutBias",port->VoutBias}});}
    for(size_t i=0;i<adapter.maps().orderedPhysicalNodeIds.size();++i)nodes.append(QJsonObject{{"physicalNodeId",double(adapter.maps().orderedPhysicalNodeIds[i])},{"physicalOffset",double(3*i)},{"units",QJsonArray{"m","m","m"}},{"fixedLumpedMass",op.nativeSystem.nodalLumpedMass[i]}});
    for(const auto &member:op.nativeSystem.members)members.append(QJsonObject{{"physicalElementId",double(member.physicalElementId)},{"physicalNodeIds",QJsonArray{double(adapter.maps().orderedPhysicalNodeIds[member.node1]),double(adapter.maps().orderedPhysicalNodeIds[member.node2])}}});
    QJsonArray residuals;for(const auto &x:op.reference.electricalResiduals)residuals.append(QJsonObject{{"portId",x.portId},{"voltageResidual",x.voltageResidual},{"chargeResidual",x.chargeResidual},{"voltageScale",x.voltageScale},{"chargeScale",x.chargeScale}});
    output.exportPackage={{"schema","range.piezo-native-axial-fe-export/v1"},{"identityScope","compatibility-not-authentication"},{"sourceFingerprint",fingerprint},{"parentTrussSourceFingerprint",adapter.sourceFingerprint()},{"rawRecipe",QString::fromUtf8(setup.recipeConfiguration)},{"backendIdentity",cb.backendIdentity},{"backend",backendIdentity},{"privateRuntime",provider->runtimeMetadata()},{"design",result.value("design")},
        {"controllerModelScope",forceAware.value("qualification")},{"M",matrix(op.free.M,r,r)},{"K",matrix(op.free.K,r,r)},{"C",matrix(freeC,r,r)},{"B",matrix(o.B,r,m)},{"KE",matrix(op.free.Kshort,r,r)},{"Kmaterial",matrix(op.free.Kmaterial,r,r)},{"Kgeometric",matrix(op.free.Kgeometric,r,r)},{"KfixedCharge",matrix(op.free.KfixedCharge,r,r)},
        {"fullM",matrix(op.full.M,n,n)},{"fullK",matrix(op.full.K,n,n)},{"fullKE",matrix(op.full.Kshort,n,n)},{"fullKmaterial",matrix(op.full.Kmaterial,n,n)},{"fullKgeometric",matrix(op.full.Kgeometric,n,n)},{"fullKfixedCharge",matrix(op.full.KfixedCharge,n,n)},{"fullC",matrix(fullC,n,n)},{"G",matrix(op.full.G,n,p)},{"Ce",matrix(op.full.Ce,p,p)},
        {"T",matrix(T,n,r)},{"P",matrix(P,n,n)},{"p",array(op.nativeSystem.fixedPrescribedDisplacements)},{"q0",array(op.q0)},{"V0",array(op.V0)},{"Q0",array(op.Q0)},{"f0",array(op.f0)},{"internalForce0",array(op.internalForce0)},{"reactions0",array(op.reactions0)},{"backgroundForce",array(adapter.initialCompleteInputs().externalForces)},{"backgroundReducedOffset",array(bias)},
        {"forceResidual",op.reference.freeForceResidualNorm},{"forceScale",op.reference.forceScale},{"forceTolerance",op.reference.forceTolerance},{"electricResiduals",residuals},{"equilibriumCertified",op.reference.equilibriumCertified},{"certificateAuthority",op.certificateAuthority},{"dampingAuthority",config("damping",{{"kind","none"}})},{"materialQualification",materialQualification},{"materials",materials},{"totalMass",totalMass},{"massUnit","kg"},{"massAuthority","actual-fixed-native-nodal-lumped-mass"},
        {"nativeSystemIdentity",QString::fromLatin1(dampingCertificate.systemIdentity().toHex())},{"readoutIdentity",QString::fromLatin1(op.readoutIdentity.toHex())},{"initialDeltaQ",array(op.initialDeltaQ)},{"initialVelocity",array(op.initialVelocity)},
        {"Lq",request.value("Lq")},{"Lv",request.value("Lv")},{"La",request.value("La")},{"D",request.value("D")},{"sourceFrame",request.value("sourceFrame")},{"configuration",request.value("configuration")},{"physicalForceDirections",request.value("physicalForceDirections")},{"Bforce",request.value("Bforce")},{"Dforce",matrix(Dforce,ny,nf)},{"disturbanceInputs",request.value("disturbanceInputs")},{"initialTime",initialTime},{"biasForce",request.value("biasForce")},
        {"reducedPhysicalForceMap",matrix(o.forceMap,r,n)},{"physicalForceDirectFeedthrough",matrix(forceDirect,ny,n)},{"physicalForceDirectFeedthroughStatus","actual-La-M-inverse-physical-force-map"},{"dofs",o.dofs},{"inputs",o.inputs},{"outputs",o.outputs},{"ports",ports},{"nodes",nodes},{"members",members},{"forceCatalog",request.value("forceCatalog")},{"controlEveryFESteps",double(N)},{"dt",dt},{"steps",double(steps)}};
    output.backendInitializationIdentity=backendIdentity;
    output.certificationBatchRequest=[provider](const QJsonObject &message){return provider->request(message);};
    output.certificationBatchPipeline={
        [provider](const QJsonObject &message){provider->beginRequest(message);},
        [provider]{return provider->finishRequest();},
        [provider]{provider->abortPendingRequest();}};
    return output;
}
