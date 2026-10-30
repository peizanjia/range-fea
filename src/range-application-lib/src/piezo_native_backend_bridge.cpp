#include "piezo_native_backend_bridge.h"
#include "rpiezo_axial_experiment_service.h"
#include "piezo_numeric_transport.h"
#include <rml_model.h>
#include <rparallel_execution_context.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <limits>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using V=std::vector<double>;
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
double finite(double v){require(std::isfinite(v),"Native backend nonfinite numeric value");return v;}
QByteArray canonical(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject object(const QByteArray &bytes,const char *label){
    QJsonParseError e;const auto d=QJsonDocument::fromJson(bytes,&e);
    if(e.error!=QJsonParseError::NoError || !d.isObject())throw std::invalid_argument(label);return d.object();
}
QJsonObject configuration(const QByteArray &bytes,const QJsonObject &fallback={}){
    require(bytes.size()<=1024*1024,"Backend configuration exceeds bounded size");return bytes.isEmpty()?fallback:object(bytes,"Backend configuration must be a JSON object");
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
QByteArray hashFile(const QString &path){
    QFile f(path);require(f.open(QIODevice::ReadOnly),"Frozen backend file unavailable");QCryptographicHash h(QCryptographicHash::Sha256);
    require(h.addData(&f) && f.error()==QFileDevice::NoError,"Frozen backend file hash failed");return h.result();
}
QString file(const QString &path){const QFileInfo f(path);require(f.isFile(),"Backend executable/module path is not a file");const auto p=f.canonicalFilePath();require(!p.isEmpty(),"Backend file has no canonical path");return p;}
QString identity(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
using Provider = PiezoNumericTransport;
const QString axes[]={"X","Y","Z","RX","RY","RZ"};
struct Output {QString id,unit,quantity;size_t physicalIndex=0;bool mechanical=false;};
struct Operators {
    V Lq,Lv,La,D,forceMap;
    QJsonArray dofs,inputs,outputs,forceCatalog,nodes;
    std::vector<Output> readouts;
    V physicalQ0;
};
// A global unit axis must lie completely in the supported physical subspace.
// A tiny nonzero projection does not establish support for the requested axis.
bool supported(const RStructuralPiezoLayeredMeshOperators &mesh,size_t row){
    const size_t n=mesh.operators.dofs,physical=6*mesh.nodeIds.size();double norm=0,residual=0;
    for(size_t j=0;j<n;++j)norm=std::hypot(norm,mesh.physicalTransform[row*n+j]);
    if(norm==0)return false;
    std::vector<size_t> active;for(size_t j=0;j<n;++j)if(mesh.physicalTransform[row*n+j]!=0)active.push_back(j);
    for(size_t i=0;i<physical;++i){double projected=0;for(size_t j:active)projected+=mesh.physicalTransform[i*n+j]*mesh.physicalTransform[row*n+j];residual=std::hypot(residual,projected-(i==row?1.:0.));}
    return std::isfinite(residual) && residual<=1e-10 && std::abs(norm-1)<=1e-10;
}
QJsonObject sourceFrame(const RStructuralPiezoLayeredSetup &s){
    const auto triple=[](const std::array<double,3> &v){return array(V(v.begin(),v.end()));};
    return {{"family",s.family==RStructuralPiezoLayeredModelFamily::PlanarBeam?"planarBeam":s.family==RStructuralPiezoLayeredModelFamily::Solid3D?"solid3D":"flatPlate"},{"frame",QJsonObject{{"origin",triple(s.frame.origin)},{"ex",triple(s.frame.ex)},{"ey",triple(s.frame.ey)},{"ez",triple(s.frame.ez)}}}};
}
V physicalDirection(const QJsonObject &binding,const RStructuralPiezoLayeredMeshOperators &mesh){
    const auto node=std::find(mesh.nodeIds.begin(),mesh.nodeIds.end(),uint(binding.value("physicalNodeId").toDouble()));require(node!=mesh.nodeIds.end(),"Force direction node missing");
    const auto nodal=vector(binding.value("physicalDirection"),6);V full(6*mesh.nodeIds.size(),0);std::copy(nodal.begin(),nodal.end(),full.begin()+6*size_t(node-mesh.nodeIds.begin()));
    mesh.projectPhysical(full,RStructuralPiezoPhysicalDomain::ForceAndMoment);return full;
}
Operators outputOperators(const RStructuralPiezoLayeredModelAdapter &adapter,const QJsonArray &mechanical,const RStructuralPiezoLayeredSetup &sourceSetup){
    const auto &d=adapter.data();const auto &mesh=adapter.mesh();const auto &op=mesh.operators;
    const size_t n=d.fullDofs,r=d.freeDofs,m=d.actuatorVoltages0.size(),physical=6*mesh.nodeIds.size(),np=op.portIds.size();Operators out;
    require(r>0,"Backend design requires free mechanical DOFs");
    for(size_t j=0;j<r;++j){QString unit;for(size_t i=0;i<n;++i)if(d.T[i*r+j]!=0){if(unit.isEmpty())unit=d.generalizedCoordinateUnits[i];else require(unit==d.generalizedCoordinateUnits[i],"Reduced coordinate mixes translation and rotation units");}
        require(!unit.isEmpty(),"Native free coordinate is empty");out.dofs.append(QJsonArray{QString("free:%1").arg(j),unit});}
    const auto &actuators=adapter.electrical().condensed().actuatorPortIds;
    for(const auto &id:actuators)out.inputs.append(QJsonArray{id,"V"});
    for(const auto &port:d.portMetadata){const auto &p=port.readout;if(p.readout==RStructuralPiezoReadoutMode::None)continue;
        const auto position=std::find(op.portIds.begin(),op.portIds.end(),p.portId);require(position!=op.portIds.end(),"Native electrical port identity missing");const size_t k=size_t(position-op.portIds.begin());
        const QString unit=rStructuralPiezoReadoutUnit(p.readout);out.readouts.push_back({p.portId,unit,{ },0,false});out.outputs.append(QJsonArray{p.portId,unit});
        double coefficient=-1;
        if(p.readout==RStructuralPiezoReadoutMode::OpenCircuitVoltage){require(op.capacitance[k*np+k]>0,"Open readout capacitance invalid");coefficient=1/op.capacitance[k*np+k];}
        if(p.readout==RStructuralPiezoReadoutMode::IdealChargeAmplifier)coefficient=-double(p.amplifierPolarity)/p.Cf;
        for(size_t j=0;j<r;++j){double value=0;for(size_t i=0;i<n;++i)value+=op.coupling[i*np+k]*d.T[i*r+j];out.Lq.push_back(finite(coefficient*value));out.Lv.push_back(0);out.La.push_back(0);}
        for(size_t j=0;j<m;++j){double value=0;if(p.readout==RStructuralPiezoReadoutMode::ActuatorCharge){const auto act=std::find(op.portIds.begin(),op.portIds.end(),actuators[j]);value=op.capacitance[k*np+size_t(act-op.portIds.begin())];}out.D.push_back(finite(value));}
    }
    std::set<QString> used;for(const auto &o:out.readouts)used.insert(o.id);
    for(const auto &value:mechanical){require(value.isObject(),"Mechanical output must be a JSON object");const auto spec=value.toObject();require((spec.size()==4 || (spec.size()==5 && spec.contains("unit"))) && spec.contains("id") && spec.contains("physicalNodeId") && spec.contains("dof") && spec.contains("quantity"),"Mechanical output fields mismatch");
        const auto id=spec.value("id").toString(),axis=spec.value("dof").toString(),quantity=spec.value("quantity").toString();
        require(!id.isEmpty() && used.insert(id).second,"Mechanical output ID is empty or duplicated");require(spec.value("physicalNodeId").isDouble(),"Mechanical node ID must be numeric");const double rawNode=spec.value("physicalNodeId").toDouble();require(rawNode>0 && rawNode<=std::numeric_limits<uint>::max() && rawNode==std::floor(rawNode),"Mechanical node ID invalid");
        const auto node=std::find(mesh.nodeIds.begin(),mesh.nodeIds.end(),uint(rawNode));const auto component=std::find(std::begin(axes),std::end(axes),axis);
        require(node!=mesh.nodeIds.end() && component!=std::end(axes),"Mechanical output node/DOF is unavailable");const size_t c=size_t(component-std::begin(axes)),row=6*size_t(node-mesh.nodeIds.begin())+c;
        double rowNorm=0;for(size_t j=0;j<n;++j)rowNorm=std::hypot(rowNorm,mesh.physicalTransform[row*n+j]);
        // Reading a global component of the admitted displacement differs from
        // injecting a pure global force axis. A partial projected response is
        // meaningful; only an exactly absent component is unsupported.
        require(rowNorm>0,"Mechanical output requests an unsupported global physical component");require(quantity=="displacement" || quantity=="velocity" || quantity=="acceleration","Mechanical output quantity unsupported (reaction outputs pending force feedthrough qualification)");
        QString unit=c<3?"m":"rad";if(quantity=="velocity")unit+="/s";if(quantity=="acceleration")unit+="/s^2";
        if(spec.contains("unit"))require(spec.value("unit").isString() && spec.value("unit").toString()==unit,"Mechanical output unit differs from native quantity/DOF");
        out.readouts.push_back({id,unit,quantity,row,true});out.outputs.append(QJsonArray{id,unit});
        for(size_t j=0;j<r;++j){double v=0;for(size_t i=0;i<n;++i)v+=mesh.physicalTransform[row*n+i]*d.T[i*r+j];out.Lq.push_back(quantity=="displacement"?finite(v):0);out.Lv.push_back(quantity=="velocity"?finite(v):0);out.La.push_back(quantity=="acceleration"?finite(v):0);}for(size_t j=0;j<m;++j)out.D.push_back(0);
    }
    require(!out.readouts.empty(),"Backend requires at least one independent output");out.physicalQ0=mesh.reconstructPhysical(d.q0);
    out.forceMap.assign(r*physical,0);
    for(size_t row=0;row<physical;++row){for(size_t i=0;i<n;++i)if(mesh.physicalTransform[row*n+i]!=0)for(size_t j=0;j<r;++j)out.forceMap[j*physical+row]+=d.T[i*r+j]*mesh.physicalTransform[row*n+i];
        if(supported(mesh,row)){const size_t c=row%6;const uint node=mesh.nodeIds[row/6];V direction(6,0);direction[c]=1;out.forceCatalog.append(QJsonObject{{"channelId",QString("node:%1:%2").arg(node).arg(axes[c])},{"physicalNodeId",double(node)},{"dof",axes[c]},{"unit",c<3?"N":"N m"},{"physicalIndex",double(row)},{"coordinateSystem","global"},{"physicalDirection",array(direction)}});}}
    const QString localAxes[]={"EX","EY","EZ","RX","RY","RZ"};
    const auto frameIdentity=sourceFrame(sourceSetup);const auto family=frameIdentity.value("family").toString();
    for(uint node:mesh.nodeIds)for(size_t c=0;c<6;++c){
        if(mesh.family==RStructuralPiezoLayeredFamily::Beam?!(c==0||c==2||c==4)
            :mesh.family==RStructuralPiezoLayeredFamily::Solid?c>=3:c==5)continue;
        QJsonObject binding{{"channelId",QString("node:%1:local:%2").arg(node).arg(localAxes[c])},{"physicalNodeId",double(node)},{"dof",localAxes[c]},{"unit",c<3?"N":"N m"},{"coordinateSystem","local"}};
        const auto direction=RPiezoExperimentDeclarations::forceDirection(binding,family,sourceSetup.frame);binding["physicalDirection"]=array(V(direction.begin(),direction.end()));physicalDirection(binding,mesh);out.forceCatalog.append(binding);
    }

    for(uint id:mesh.nodeIds)out.nodes.append(double(id));return out;
}
QJsonObject initialization(const RStructuralPiezoLayeredModelAdapter &adapter,const Operators &o,const QJsonObject &design,
                           const QJsonObject &controller,const QJsonObject &excitation,uint N,const QString &source,double initialTime){
    const auto &d=adapter.data();const size_t r=d.freeDofs,m=d.actuatorVoltages0.size(),ny=o.readouts.size();
    const auto selected=RPiezoExperimentDeclarations::forceChannels(excitation);
    QJsonArray catalog=o.forceCatalog,disturbances;std::vector<V> directions;std::set<QString> used;
    for(const auto &value:selected){
        require(value.isObject(),"Native excitation channel must be an object");const auto channel=value.toObject();
        require(channel.value("channelId").isString() && channel.value("unit").isString(),"Native excitation channel ID/unit missing");
        const auto id=channel.value("channelId").toString();
        int index=-1;for(int i=0;i<catalog.size();++i)if(catalog[i].toObject().value("channelId").toString()==id){index=i;break;}
        require(index>=0,"Native excitation requests an unsupported physical force axis");auto binding=catalog[index].toObject();
        require(binding.value("unit").toString()==channel.value("unit").toString(),"Native excitation force/moment unit mismatch");
        if(!used.insert(id).second)continue;
        binding["disturbanceColumn"]=double(directions.size());catalog[index]=binding;
        directions.push_back(physicalDirection(binding,adapter.mesh()));disturbances.append(QJsonArray{id,binding.value("unit")});
    }
    const size_t physical=6*adapter.mesh().nodeIds.size(),nf=directions.size();V Bforce(r*nf),Dforce(ny*nf,0),S(physical*nf,0);
    for(size_t j=0;j<nf;++j)for(size_t k=0;k<physical;++k){S[k*nf+j]=directions[j][k];for(size_t i=0;i<r;++i)Bforce[i*nf+j]+=o.forceMap[i*physical+k]*directions[j][k];}
    for(size_t j=0;j<nf;++j){const auto actual=adapter.reduceForce(directions[j]);double scale=0;for(double x:actual)scale=std::max(scale,std::abs(x));for(size_t i=0;i<r;++i)require(std::abs(Bforce[i*nf+j]-actual[i])<=1e-10*std::max({scale,std::abs(Bforce[i*nf+j]),std::numeric_limits<double>::min()}),"Directional force map differs from actual native force reduction");}
    const auto stiffness=matrix(d.stiffness,r,r);
    if(adapter.mesh().family==RStructuralPiezoLayeredFamily::Solid){
        for(size_t i=0;i<r;++i)for(size_t j=i+1;j<r;++j)
            require(stiffness[i].toArray()[int(j)]==stiffness[j].toArray()[int(i)],"Solid reduced K is not exactly symmetric at backend serialization");
    }
    // The service's Dforce is additional algebraic feedthrough, before it adds
    // La*M^-1*Bforce. Native mechanical forces have no additional term. Export
    // the actual total separately below, after checking the service certificate.
    return {{"op","initialize"},{"sourceFingerprint",source},{"M",matrix(d.mass,r,r)},{"K",stiffness},{"C",matrix(d.damping,r,r)},
        {"B",matrix(d.input,r,m)},{"Lq",matrix(o.Lq,ny,r)},{"Lv",matrix(o.Lv,ny,r)},{"La",matrix(o.La,ny,r)},{"D",matrix(o.D,ny,m)},
        {"physicalForceDirections",matrix(S,physical,nf)},{"Bforce",matrix(Bforce,r,nf)},{"Dforce",matrix(Dforce,ny,nf)},{"disturbanceInputs",disturbances},{"initialTime",finite(initialTime)},{"biasForce",array(d.backgroundOffset)},
        {"dofs",o.dofs},{"inputs",o.inputs},{"outputs",o.outputs},{"actuatorVoltages0",array(d.actuatorVoltages0)},
        {"dt",0}, // Caller replaces with the source clock.
        {"controlEveryFESteps",double(N)},{"nodeIds",o.nodes},{"forceCatalog",catalog},
        {"configuration",QJsonObject{{"design",design},{"controller",controller},{"excitation",excitation}}}};
}
QJsonArray readouts(const Operators &o,const RStructuralPiezoLayeredDynamicsState &state,const std::set<QString> &selected){
    QJsonArray a;for(const auto &entry:o.readouts){if(!selected.count(entry.id))continue;double value=0;
        if(entry.mechanical){const auto *v=entry.quantity=="displacement"?&state.physicalDisplacements:entry.quantity=="velocity"?&state.physicalVelocities:&state.physicalAccelerations;
            require(entry.physicalIndex<v->size(),"Accepted physical output dimensions mismatch");value=(*v)[entry.physicalIndex];if(entry.quantity=="displacement")value-=o.physicalQ0[entry.physicalIndex];}
        else {const auto v=std::find_if(state.readouts.begin(),state.readouts.end(),[&](const auto &p){return p.portId==entry.id;});require(v!=state.readouts.end() && !v->actionRecord && v->unit==entry.unit,"Accepted electrical readout identity mismatch");value=v->deltaValue;}
        a.append(QJsonObject{{"portId",entry.id},{"deltaValue",finite(value)},{"unit",entry.unit},{"actionRecord",false}});}
    return a;
}
// Neumaier accumulation plus the FMA product error retains cancellation terms
// in nearly uncoupled original-M rows. It does not change their residual bound.
struct AccurateSum {
    double sum=0,correction=0;
    void add(double value){const double next=finite(sum+value);correction=finite(correction+(std::abs(sum)>=std::abs(value)?(sum-next)+value:(value-next)+sum));sum=next;}
    void product(double a,double b){const double value=finite(a*b);add(value);add(finite(std::fma(a,b,-value)));}
    double value() const{return finite(sum+correction);}
};
V forceFeedthrough(const RStructuralPiezoLayeredCompiledData &d,const Operators &o,size_t physical){
    const size_t r=d.freeDofs,ny=o.readouts.size();V result(ny*physical,0);bool needed=false;for(double x:o.La)needed=needed||x!=0;if(!needed)return result;
    V scale(r),lower(r*r);for(size_t i=0;i<r;++i){require(d.mass[i*r+i]>0,"Acceleration output mass diagonal invalid");scale[i]=std::sqrt(d.mass[i*r+i]);}
    for(size_t i=0;i<r;++i)for(size_t j=0;j<=i;++j){double value=d.mass[i*r+j]/scale[i]/scale[j];for(size_t k=0;k<j;++k)value-=lower[i*r+k]*lower[j*r+k];
        if(i==j){require(value>0,"Acceleration output mass factorization failed");lower[i*r+j]=std::sqrt(value);}else lower[i*r+j]=finite(value/lower[j*r+j]);}
    const auto solve=[&](const V &rhs){V solved(r);
        for(size_t i=0;i<r;++i){AccurateSum value;value.add(finite(rhs[i]/scale[i]));for(size_t k=0;k<i;++k)value.product(-lower[i*r+k],solved[k]);solved[i]=finite(value.value()/lower[i*r+i]);}
        for(size_t i=r;i-->0;){AccurateSum value;value.add(solved[i]);for(size_t k=i+1;k<r;++k)value.product(-lower[k*r+i],solved[k]);solved[i]=finite(value.value()/lower[i*r+i]);}
        for(size_t i=0;i<r;++i)solved[i]=finite(solved[i]/scale[i]);return solved;
    };
    for(size_t row=0;row<ny;++row){V rhs(r);bool active=false;for(size_t i=0;i<r;++i){rhs[i]=o.La[row*r+i];active=active||rhs[i]!=0;}if(!active)continue;
        V solved=solve(rhs);
        constexpr unsigned maximumCorrections=8;
        for(unsigned iteration=0;;++iteration){V residual(r);bool qualified=true;size_t worstRow=0;double worstRatio=-1,worstResidual=0,worstBound=0;
            // Always certify the original M, rather than its scaled factor or
            // a normwise bound that could conceal a tiny coupled row.
            for(size_t i=0;i<r;++i){AccurateSum actual,bound;bound.add(std::abs(rhs[i]));for(size_t j=0;j<r;++j){actual.product(d.mass[i*r+j],solved[j]);bound.add(std::abs(finite(d.mass[i*r+j]*solved[j])));}
                AccurateSum difference;difference.add(rhs[i]);difference.add(-actual.sum);difference.add(-actual.correction);residual[i]=difference.value();const double scaleBound=bound.value();
                const double ratio=scaleBound==0?(residual[i]==0?0:std::numeric_limits<double>::infinity()):std::abs(residual[i])/scaleBound;
                if(!(ratio<=1e-10))qualified=false;
                if(ratio>worstRatio){worstRatio=ratio;worstRow=i;worstResidual=residual[i];worstBound=scaleBound;}
            }
            if(qualified)break;
            if(iteration==maximumCorrections)throw std::invalid_argument(qPrintable(QString("Acceleration force-feedthrough mass solve residual failed: output=%1, massRow=%2, residual=%3, componentwiseBound=%4, ratio=%5, threshold=1e-10, corrections=%6")
                .arg(o.readouts[row].id).arg(qulonglong(worstRow)).arg(worstResidual,0,'g',17).arg(worstBound,0,'g',17).arg(worstRatio,0,'g',17).arg(iteration)));
            const V correction=solve(residual);for(size_t i=0;i<r;++i)solved[i]=finite(solved[i]+correction[i]);
        }
        for(size_t col=0;col<physical;++col){double value=0;for(size_t k=0;k<r;++k)value+=solved[k]*o.forceMap[k*physical+col];result[row*physical+col]=finite(value);}}
    return result;
}
V lift(const V &T,const V &C,size_t n,size_t r){V temp(n*r),full(n*n);for(size_t i=0;i<n;++i)for(size_t j=0;j<r;++j)for(size_t k=0;k<r;++k)temp[i*r+j]+=T[i*r+k]*C[k*r+j];for(size_t i=0;i<n;++i)for(size_t j=0;j<n;++j)for(size_t k=0;k<r;++k)full[i*n+j]+=temp[i*r+k]*T[j*r+k];return full;}
}

PiezoNativeBackendBridge::Result PiezoNativeBackendBridge::create(const RModel &source){
    const QDir installed(QCoreApplication::applicationDirPath()+QStringLiteral("/.."));
    QString python=installed.filePath(QStringLiteral("runtime/python/python.exe"));
    QString modules=installed.filePath(QStringLiteral("share/range-workflow"));
    if(!QFileInfo(python).isFile() || !QFileInfo(QDir(modules).filePath("range_workflow/piezo_native_backend.py")).isFile()){
#if defined(RANGE_PIEZO_BACKEND_PYTHON) && defined(RANGE_PIEZO_BACKEND_MODULE_ROOT)
        python=QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON);modules=QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT);
#else
        throw std::invalid_argument("Installed private numerical backend is unavailable and no compiled runtime is configured");
#endif
    }
    return create(source,python,modules);
}

PiezoNativeBackendBridge::Result PiezoNativeBackendBridge::create(const RModel &source,const QString &python,const QString &moduleRoot){
    // Include implicit equilibrium compilation and the authoritative modal-C
    // lift in the same deterministic host-thread scope as native dynamics.
    RParallelExecutionConfig policy;policy.setMode(RParallelExecutionMode::SerialDeterministic);
    const RParallelExecutionContext execution(policy);
    const auto &s=source.getProblemSetup().getStructuralPiezoLayeredSetup();s.validate(source);const auto fingerprint=s.computeSourceFingerprint(source);const double dt=source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep();
    const double initialTime=finite(source.getTimeSolver().getInputStartTime()-dt);
    auto design=configuration(s.designConfiguration),controller=configuration(s.controllerConfiguration,{{"kind","none"}}),excitation=configuration(s.excitationConfiguration,{{"channels",QJsonArray{}}});
    require(!excitation.contains("external") && !excitation.contains("executable"),"Use the declared time-only externalFunctions interface");
    uint N=1;if(controller.contains("controlEveryFESteps")){const auto value=controller.take("controlEveryFESteps");require(value.isDouble() && value.toDouble()>=1 && value.toDouble()<=1000000 && value.toDouble()==std::floor(value.toDouble()),"Invalid native controller FE stride");N=uint(value.toDouble());}
    require(!design.contains("outputs") || design.value("outputs").isArray(),"Design outputs must be an array");const auto mechanical=design.take("outputs").toArray();
    const auto desiredDamping=design.take("damping");const bool modal=s.damping.kind==RStructuralPiezoDampingKind::UniformModal || s.damping.kind==RStructuralPiezoDampingKind::PerMode || s.damping.kind==RStructuralPiezoDampingKind::PerCluster;
    auto provider=std::make_shared<Provider>(python,moduleRoot);V fullC;std::unique_ptr<RStructuralPiezoLayeredModelAdapter> adapter;
    if(modal){RModel preliminary(source);auto &setup=preliminary.getProblemSetup().getStructuralPiezoLayeredSetup();setup.damping={};setup.bindToModel(preliminary);RStructuralPiezoLayeredModelAdapter undamped(preliminary,setup);const auto operators=outputOperators(undamped,mechanical,setup);
        QJsonObject damping;if(s.damping.kind==RStructuralPiezoDampingKind::UniformModal)damping={{"kind","uniform"},{"zeta",s.damping.uniformRatio}};
        else damping={{"kind",s.damping.kind==RStructuralPiezoDampingKind::PerMode?"per_mode":"per_cluster"},{"zeta",array(s.damping.ratios)}};
        if(!desiredDamping.isUndefined())require(desiredDamping.isObject() && desiredDamping.toObject()==damping,"Design damping differs from the native source authority");
        auto request=initialization(undamped,operators,{{"damping",damping}},{{"kind","none"}},{{"channels",QJsonArray{}}},N,fingerprint,initialTime);request["dt"]=dt;request["sourceFrame"]=sourceFrame(s);const auto result=provider->request(request);
        if(s.damping.kind==RStructuralPiezoDampingKind::PerCluster){const auto clusters=result.value("design").toObject().value("clusters").toArray();require(size_t(clusters.size())==s.damping.clusterEnds.size(),"Backend does not provide matching complete modal clusters");uint next=0;
            for(int i=0;i<clusters.size();++i){require(clusters[i].isArray(),"Backend modal cluster invalid");for(const auto &index:clusters[i].toArray()){require(index.isDouble() && index.toDouble()==next,"Backend modal cluster order invalid");++next;}require(next==s.damping.clusterEnds[size_t(i)],"Source clusterEnds differs from actual native eigensystem");}}
        const auto C=dense(result.value("authoritativeC"),undamped.data().freeDofs);fullC=lift(undamped.data().T,C,undamped.data().fullDofs,undamped.data().freeDofs);
        adapter=std::make_unique<RStructuralPiezoLayeredModelAdapter>(source,s,fullC);const auto &actual=adapter->data().damping;require(actual.size()==C.size(),"Modal C reduction shape mismatch");
        const size_t free=adapter->data().freeDofs;
        for(size_t i=0;i<free;++i)for(size_t j=0;j<free;++j){const double diagonalScale=std::sqrt(std::abs(C[i*free+i]))*std::sqrt(std::abs(C[j*free+j]));
            require(std::abs(actual[i*free+j]-C[i*free+j])<=1e-10*std::max({std::abs(actual[i*free+j]),std::abs(C[i*free+j]),diagonalScale,std::numeric_limits<double>::min()}),"Native full-C lift does not recover authoritative reduced damping");}
    }else {adapter=std::make_unique<RStructuralPiezoLayeredModelAdapter>(source,s);
        if(!desiredDamping.isUndefined()){QJsonObject expected;
            if(s.damping.kind==RStructuralPiezoDampingKind::None)expected={{"kind","none"}};
            else if(s.damping.kind==RStructuralPiezoDampingKind::Rayleigh)expected={{"kind","rayleigh"},{"alpha",s.damping.rayleighAlpha},{"beta",s.damping.rayleighBeta}};
            else expected={{"kind","imported"},{"imported_C",matrix(adapter->data().damping,adapter->data().freeDofs,adapter->data().freeDofs)}};
            require(desiredDamping.isObject() && desiredDamping.toObject()==expected,"Design damping override differs from the native source authority");}}
    const auto operators=outputOperators(*adapter,mechanical,s);auto request=initialization(*adapter,operators,design,controller,excitation,N,fingerprint,initialTime);request["dt"]=dt;request["sourceFrame"]=sourceFrame(s);
    // The typed dynamics target is the native Run authority. The legacy GUI
    // input count may still be its default before acceptedModel seals the clock.
    request["targetSteps"]=double(source.getProblemSetup().getStructuralDynamicsSetup().getNSteps());
    quint64 initialBits=0;std::memcpy(&initialBits,&initialTime,sizeof(initialBits));QByteArray initialBytes(8,'\0');
    for(uint i=0;i<8;++i)initialBytes[i]=char((initialBits>>(8*i))&255);
    request["initialTimeBits"]=QString::fromLatin1(initialBytes.toHex());
    const auto result=provider->request(request);
    require(result.value("identity").isObject() && result.value("initialOpaqueState").isObject(),"Backend initialization identity/state missing");const auto backendIdentity=result.value("identity").toObject();
    require(backendIdentity.value("sourceFingerprint").toString()==fingerprint,"Backend source identity differs from actual FE");
    const auto &d=adapter->data();const auto &op=adapter->mesh().operators;const size_t r=d.freeDofs,n=d.fullDofs,m=d.actuatorVoltages0.size(),physical=6*adapter->mesh().nodeIds.size();
    const auto forceDirect=forceFeedthrough(d,operators,physical);const size_t nf=size_t(request.value("disturbanceInputs").toArray().size()),ny=operators.readouts.size();V Dforce(ny*nf);
    for(const auto &value:request.value("forceCatalog").toArray()){const auto binding=value.toObject();if(!binding.contains("disturbanceColumn"))continue;
        const size_t column=size_t(binding.value("disturbanceColumn").toDouble());const auto direction=physicalDirection(binding,adapter->mesh());
        for(size_t row=0;row<ny;++row)for(size_t k=0;k<physical;++k)Dforce[row*nf+column]+=forceDirect[row*physical+k]*direction[k];}
    const auto forceAware=result.value("design").toObject().value("forceAware").toObject();
    require(forceAware.value("qualification").toString()=="complete-known-forcing" && forceAware.value("allConfiguredForcesMapped").toBool(),"Backend did not qualify the complete native force mapping");
    const auto backendD=forceAware.value("FE").toObject().value("continuous").toObject().value("D").toArray();
    require(size_t(backendD.size())==ny,"Backend force-aware output certificate shape mismatch");
    for(size_t row=0;row<ny;++row){const auto values=vector(backendD[int(row)],m+nf);double scale=0;for(size_t column=0;column<physical;++column)scale=std::max(scale,std::abs(forceDirect[row*physical+column]));
        for(size_t column=0;column<nf;++column){const double expected=Dforce[row*nf+column],actual=values[m+column];
            require(std::abs(actual-expected)<=1e-10*std::max({scale,std::abs(actual),std::abs(expected),std::numeric_limits<double>::min()}),"Backend force-aware direct term differs from the actual native mass solve");}}
    Result output;auto &cb=output.callbacks;cb.authoritativeFullC=fullC;cb.sourceFingerprint=fingerprint;cb.excitationConfiguration=s.excitationConfiguration;cb.controllerConfiguration=s.controllerConfiguration;cb.designConfiguration=s.designConfiguration;cb.controlEveryFESteps=N;
    cb.backendIdentity="range-fea/native-python-layered/v1:"+identity(provider->runtimeIdentity().toUtf8()+canonical(backendIdentity));cb.forcingIdentity=backendIdentity.value("excitationSourceHash").toString()+":"+backendIdentity.value("excitationLoadedCodeHash").toString();cb.controllerIdentity=backendIdentity.value("configurationIdentity").toString();
    cb.initialOpaqueControllerState=canonical(result.value("initialOpaqueState").toObject());
    const auto envelopes=result.value("envelopes").toObject();const bool any=envelopes.value("lower").isArray() || envelopes.value("upper").isArray() || envelopes.value("deltaPerSample").isArray();
    if(any){require(envelopes.value("lower").isArray() && envelopes.value("upper").isArray() && envelopes.value("deltaPerSample").isArray(),"Bounded native control requires amplitude and per-sample rate limits together");const auto lower=vector(envelopes.value("lower"),m),upper=vector(envelopes.value("upper"),m),delta=vector(envelopes.value("deltaPerSample"),m);
        for(size_t i=0;i<m;++i){const double rate=finite(delta[i]/finite(double(N)*dt));require(rate>0 && lower[i]<upper[i] && d.actuatorVoltages0[i]>=lower[i] && d.actuatorVoltages0[i]<=upper[i],"Native absolute voltage envelope/initial voltage invalid");cb.envelopes.push_back({lower[i],upper[i],rate});}}
    const bool sensorOnly=controller.value("stateSource").toString(controller.value("kind").toString()=="external"?"sensor_only":"full_fe")=="sensor_only";std::set<QString> selected;
    if(controller.contains("selectedOutputs")){require(controller.value("selectedOutputs").isArray(),"Selected outputs must be an array");for(const auto &id:controller.value("selectedOutputs").toArray()){require(id.isString() && selected.insert(id.toString()).second,"Selected output identity invalid");}}
    if(!sensorOnly || !controller.contains("selectedOutputs"))for(const auto &o:operators.readouts)selected.insert(o.id);
    cb.perturbationForce=[provider,physical](double time){return vector(provider->request({{"op","force"},{"time",finite(time)}}).value("forces"),physical);};
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
        cb.perturbationForce=[provider,ledger,physical,initialTime,dt](double time){
            finite(time);const uint step=uint(ledger->value("feStep").toDouble());
            const auto clock=[=](uint index){return index?finite(initialTime+double(index)*dt):initialTime;};
            uint boundary=0;
            if(time==initialTime)boundary=0;
            else if(time==clock(step))boundary=step;
            else if(time==clock(step+1))boundary=step+1;
            else {const double index=finite((time-initialTime)/dt);require(index>=0 && index<=1000000,"Force ledger time outside canonical grid");boundary=uint(std::llround(index));}
            require(time==clock(boundary),"External force requires a canonical FE boundary");
            const uint interval=boundary?boundary-1:0;
            return vector(provider->request({{"op","force"},{"time",finite(time)},{"feStep",double(interval)},{"opaqueState",*ledger},{"replayOnly",true}}).value("forces"),physical);
        };
        cb.intervalPerturbationForce=[provider,ledger,physical](double time){return vector(provider->request({{"op","force"},{"time",finite(time)},{"feStep",ledger->value("feStep")},{"opaqueState",*ledger},{"replayOnly",true}}).value("forces"),physical);};
    }
    cb.propose=[provider,operators,sensorOnly,selected,r,m](const auto &state,uint completed,uint tick,const QByteArray &old){
        const auto opaque=object(old,"Opaque controller state is not JSON");require(opaque.value("feStep").toDouble()==completed && opaque.value("controlTick").toDouble()==tick,"Native proposal counters differ from opaque state");QJsonObject message{{"op","propose"},{"opaqueState",opaque},{"readouts",readouts(operators,state,selected)}};
        if(!sensorOnly){require(state.integrator.state.q.size()==r && state.integrator.state.v.size()==r,"Native reduced FE state dimensions mismatch");message["q"]=array(V(state.integrator.state.q.begin(),state.integrator.state.q.end()));message["v"]=array(V(state.integrator.state.v.begin(),state.integrator.state.v.end()));}
        const auto answer=provider->request(message);require(answer.value("candidateOpaqueState").isObject(),"Backend proposal omitted candidate state");RStructuralPiezoLayeredControllerProposal p;p.requestedVoltages=vector(answer.value("absoluteVoltages"),m);p.nextOpaqueState=canonical(answer.value("candidateOpaqueState").toObject());p.lastFrameBytes=canonical(answer);return p;
    };
    cb.postFE=[provider](const auto &,uint completed,uint tick,const V &applied,const QByteArray &old){const auto answer=provider->request({{"op","postFE"},{"opaqueState",object(old,"Opaque controller state is not JSON")},{"appliedVoltages",array(applied)},{"feStep",double(completed)}});require(answer.value("candidateOpaqueState").isObject(),"Backend postFE omitted candidate state");const auto next=answer.value("candidateOpaqueState").toObject();require(next.value("controlTick").toDouble()==tick,"Backend postFE control tick mismatch");return canonical(next);};
    cb.validateOpaque=[provider,N](const QByteArray &state,uint completed,uint tick,uint phase){require(phase==completed%N,"Native controller FE phase mismatch");provider->request({{"op","validate"},{"opaqueState",object(state,"Opaque controller state is not JSON")},{"feStep",double(completed)},{"controlTick",double(tick)},{"phase","ready"}});};
    cb.validateAcceptedBoundary=[provider](const RStructuralPiezoLayeredRunState &state){provider->request({{"op","validate"},{"opaqueState",object(state.opaqueControllerState,"Opaque controller state is not JSON")},{"feStep",double(state.completedFE)},{"controlTick",double(state.controlTick)},{"phase","ready"},{"appliedVoltages",array(state.lastApplied)},{"heldVoltages",array(state.heldRequested)}});};
    QJsonArray nodeMap,memberMap,portMap,materialMap;for(size_t i=0;i<adapter->mesh().nodeIds.size();++i){QJsonArray units;for(uint c=0;c<6;++c)units.append(c<3?"m":"rad");nodeMap.append(QJsonObject{{"physicalNodeId",double(adapter->mesh().nodeIds[i])},{"physicalOffset",double(6*i)},{"units",units}});}
    for(size_t i=0;i<adapter->mesh().memberIds.size();++i){QJsonArray connectivity;for(uint node:adapter->mesh().memberNodeIndices[i])connectivity.append(double(adapter->mesh().nodeIds[node]));memberMap.append(QJsonObject{{"physicalElementId",double(adapter->mesh().memberIds[i])},{"physicalNodeIds",connectivity}});}
    for(const auto &material:d.materialProvenance)materialMap.append(QJsonObject{{"materialId",material.materialId},{"sourceId",material.sourceId},{"sourceSha256",material.sourceSha256},{"sourceKind",int(material.sourceKind)},{"form",int(material.form)},{"density",material.density},{"piezoelectric",material.piezoelectric},{"elasticityOrCompliance",matrix(V(material.elasticityOrCompliance.begin(),material.elasticityOrCompliance.end()),6,6)},{"coupling",matrix(V(material.coupling.begin(),material.coupling.end()),3,6)},{"permittivity",matrix(V(material.permittivity.begin(),material.permittivity.end()),3,3)}});for(const auto &port:d.portMetadata)portMap.append(QJsonObject{{"portId",port.readout.portId},{"role",int(port.readout.role)},{"readout",int(port.readout.readout)},{"unit",rStructuralPiezoReadoutUnit(port.readout.readout)},{"Cf",port.readout.Cf},{"amplifierPolarity",port.readout.amplifierPolarity},{"Qref",port.readout.Qref},{"VoutBias",port.readout.VoutBias}});
    V Q0,V0;for(const auto &p:adapter->electrical().referenceResponse().ports){Q0.push_back(p.charge);V0.push_back(p.voltage);}
    output.exportPackage={{"schema","range.piezo-native-fe-export/v1"},{"sourceFingerprint",fingerprint},{"identityScope","compatibility-not-authentication"},{"backendIdentity",cb.backendIdentity},{"backend",backendIdentity},{"design",result.value("design")},
        {"privateRuntime",provider->runtimeMetadata()},{"controllerModelScope",forceAware.value("qualification")},{"M",matrix(d.mass,r,r)},{"K",matrix(d.stiffness,r,r)},{"C",matrix(d.damping,r,r)},{"B",matrix(d.input,r,m)},
        {"sourceFrame",request.value("sourceFrame")},{"configuration",request.value("configuration")},{"physicalForceDirections",request.value("physicalForceDirections")},{"Bforce",request.value("Bforce")},{"Dforce",matrix(Dforce,ny,nf)},{"disturbanceInputs",request.value("disturbanceInputs")},{"initialTime",initialTime},{"biasForce",request.value("biasForce")},
        {"fullM",matrix(op.mass,n,n)},{"fullKE",matrix(op.stiffness,n,n)},{"fullK",matrix(adapter->electrical().condensed().stiffness,n,n)},{"fullC",matrix(d.fullDamping,n,n)},{"G",matrix(op.coupling,n,op.portIds.size())},{"Ce",matrix(op.capacitance,op.portIds.size(),op.portIds.size())},
        {"T",matrix(d.T,n,r)},{"p",array(d.p)},{"P",matrix(adapter->mesh().physicalTransform,physical,n)},
        {"q0",array(d.q0)},{"V0",array(V0)},{"Q0",array(Q0)},{"f0",array(d.referenceForce)},{"backgroundForce",array(d.backgroundForce)},{"backgroundReducedOffset",array(d.backgroundOffset)},
        {"forceResidual",d.forceResidual},{"electricResidual",d.electricResidual},{"dampingAuthority",d.dampingAuthority},
        {"Lq",request.value("Lq")},{"Lv",request.value("Lv")},{"La",request.value("La")},{"D",request.value("D")},
        {"reducedPhysicalForceMap",matrix(operators.forceMap,r,physical)},{"physicalForceDirectFeedthrough",matrix(forceDirect,ny,physical)},{"physicalForceDirectFeedthroughStatus","actual-La-M-inverse-reduced-force-map; force-aware-controller"},
        {"dofs",operators.dofs},{"inputs",operators.inputs},{"outputs",operators.outputs},{"ports",portMap},{"materials",materialMap},{"nodes",nodeMap},{"members",memberMap},{"forceCatalog",request.value("forceCatalog")},{"controlEveryFESteps",double(N)}};
    output.backendInitializationIdentity=backendIdentity;
    output.certificationBatchRequest=[provider](const QJsonObject &message){return provider->request(message);};
    output.certificationBatchPipeline={
        [provider](const QJsonObject &message){provider->beginRequest(message);},
        [provider]{return provider->finishRequest();},
        [provider]{provider->abortPendingRequest();}};
    return output;
}
