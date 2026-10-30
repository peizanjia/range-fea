#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <rbl_error.h>
#include <rbl_logger.h>
#include <rstructural_piezo_element_operators.h>
#include <rstructural_piezo_native_readout.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rml_structural_piezo_layered_setup.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include "rpiezo_experiment_service.h"
#include "piezo_native_backend_bridge.h"
#include "piezo_axial_backend_bridge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <limits>
#include <stdexcept>

// PC-10 scientific examples. This target deliberately requires the real private
// provider and actual native FE acceptance path. A provider/DARE/QP failure fails
// the test; there is no controller substitution or synthetic plant fallback.
#ifndef RANGE_PIEZO_BACKEND_PYTHON
#error "Delivery examples require the selected private Python executable"
#endif
#ifndef RANGE_PIEZO_BACKEND_MODULE_ROOT
#error "Delivery examples require the actual range_workflow module root"
#endif

namespace {
using V=std::vector<double>;
using Matrix=std::vector<V>;
using LayerRun=RStructuralPiezoLayeredNativeRun;
using AxialRun=RStructuralPiezoAxialNativeRun;
void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
QByteArray json(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QString sha(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
V numbers(const QJsonValue &value){require(value.isArray(),"Missing numeric array");V result;for(auto x:value.toArray()){require(x.isDouble()&&std::isfinite(x.toDouble()),"Nonfinite numeric array");result.push_back(x.toDouble());}return result;}
Matrix matrix(const QJsonValue &value){require(value.isArray(),"Missing matrix");Matrix out;for(auto row:value.toArray())out.push_back(numbers(row));if(!out.empty())for(const auto &row:out)require(row.size()==out[0].size(),"Ragged matrix");return out;}
bool bitwiseEqual(const Matrix &a,const Matrix &b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i){if(a[i].size()!=b[i].size())return false;for(size_t j=0;j<a[i].size();++j)if(std::memcmp(&a[i][j],&b[i][j],sizeof(double))!=0)return false;}return true;}
QJsonArray array(const V &v){QJsonArray a;for(double x:v)a.append(x);return a;}
QJsonArray diagonal(const V &v){QJsonArray a;for(size_t i=0;i<v.size();++i){V row(v.size());row[i]=v[i];a.append(array(row));}return a;}
V mv(const Matrix &a,const V &x){V y(a.size());for(size_t i=0;i<a.size();++i){require(a[i].size()==x.size(),"Matrix product dimensions");for(size_t j=0;j<x.size();++j)y[i]+=a[i][j]*x[j];}return y;}
V tmv(const Matrix &a,const V &x){require(a.size()==x.size(),"Transpose dimensions");V y(a.empty()?0:a[0].size());for(size_t i=0;i<a.size();++i)for(size_t j=0;j<y.size();++j)y[j]+=a[i][j]*x[i];return y;}
V plus(V a,const V &b,double scale=1){require(a.size()==b.size(),"Vector dimensions");for(size_t i=0;i<a.size();++i)a[i]+=scale*b[i];return a;}
double norm(const V &v){double result=0;for(double x:v){require(std::isfinite(x),"Nonfinite trajectory");result=std::hypot(result,x);}return result;}
V state(const V &q,const V &v){V x=q;x.insert(x.end(),v.begin(),v.end());return x;}
struct EnergyMetric {
    Matrix k,m;
    explicit EnergyMetric(const QJsonObject &e):k(matrix(e["K"])),m(matrix(e["M"])){}
    double operator()(const V &x)const{const size_t n=m.size();require(x.size()==2*n,"Energy state dimensions");V q(x.begin(),x.begin()+n),v(x.begin()+n,x.end());const auto kq=mv(k,q),mv_=mv(m,v);double z=0;for(size_t i=0;i<n;++i)z+=q[i]*kq[i]+v[i]*mv_[i];require(z>=-1e-18,"Negative mechanical energy metric");return std::sqrt(std::max(0.,z));}
};
V lift(const Matrix &phi,const V &x){const size_t r=phi[0].size();require(x.size()==2*r,"Modal lift dimensions");return state(mv(phi,V(x.begin(),x.begin()+r)),mv(phi,V(x.begin()+r,x.end())));}
V project(const QJsonObject &e,const Matrix &phi,const V &x){const size_t n=phi.size();return state(tmv(phi,mv(matrix(e["M"]),V(x.begin(),x.begin()+n))),tmv(phi,mv(matrix(e["M"]),V(x.begin()+n,x.end()))));}

// Independent first-order RK4, not the production Newmark recurrence. The
// source force is linearly interpolated between the SAME recorded endpoints;
// the ACTUAL applied absolute voltage is held over the whole FE interval.
struct Reference {
    Matrix a,b,cy,d;
    V bias,outputBias;
    explicit Reference(const QJsonObject &p):a(matrix(p["continuous"].toObject()["A"])),b(matrix(p["continuous"].toObject()["B"])),cy(matrix(p["continuous"].toObject()["Cy"])),d(matrix(p["continuous"].toObject()["D"])),bias(numbers(p["affineDerivative"])),outputBias(numbers(p["outputBias"])){}
    V derivative(const V &x,const V &u)const{return plus(plus(mv(a,x),mv(b,u)),bias);}
    V output(const V &x,const V &u)const{return plus(plus(mv(cy,x),mv(d,u)),outputBias);}
    V advance(V x,const V &left,const V &right,double dt,uint divisions)const{
        for(uint s=0;s<divisions;++s){const double h=dt/divisions;auto u=[&](double z){return plus(left,plus(right,left,-1),z);};const double z=double(s)/divisions;
            const auto k1=derivative(x,u(z));const auto k2=derivative(plus(x,k1,h/2),u(z+.5/divisions));
            const auto k3=derivative(plus(x,k2,h/2),u(z+.5/divisions));const auto k4=derivative(plus(x,k3,h),u(z+1./divisions));
            for(size_t i=0;i<x.size();++i)x[i]+=h*(k1[i]+2*k2[i]+2*k3[i]+k4[i])/6;
        }require(std::isfinite(norm(x)),"RK4 overflow");return x;
    }
};

struct Sample {double time=0;V x,physicalQ,physicalV,physicalA,y,applied,force;};
struct Trace {std::vector<Sample> samples;QByteArray finalBytes;QJsonArray mpcKKTFrames;double voltagePeak=0,kktPeak=0;uint ticks=0,limitedFESteps=0;};

class Experiment {
public:
    RModel model;
    QJsonObject recipe,exported;
    bool axial;
    PiezoNativeBackendBridge::Result layeredBridge;
    PiezoAxialBackendBridge::Result axialBridge;
    std::unique_ptr<LayerRun> layered;
    std::unique_ptr<AxialRun> rod;
    explicit Experiment(QJsonObject r,const RModel *frozenSource=nullptr):recipe(r),axial(r["family"].toString()=="axialRod"){
        if (frozenSource) model=*frozenSource;
        else {
            RModel source;source.getTimeSolver().setInputStartTime(r["dt"].toDouble());
            const auto p=RPiezoExperimentService::preview(source,r);require(p.valid,qPrintable(p.message));model=p.candidate;
        }
        const QString python=QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),root=QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT);
        if(axial){axialBridge=PiezoAxialBackendBridge::create(model,python,root);exported=axialBridge.exportPackage;rod=std::make_unique<AxialRun>(model,axialBridge.callbacks);}
        else{layeredBridge=PiezoNativeBackendBridge::create(model,python,root);exported=layeredBridge.exportPackage;layered=std::make_unique<LayerRun>(model,layeredBridge.callbacks);}
    }
    V forcing(double t)const{return axial?axialBridge.callbacks.perturbationForce(t):layeredBridge.callbacks.perturbationForce(t);}
    V input(const V &applied,const V &physicalForce)const{
        V u=applied;const auto inputs=exported["inputs"].toArray(),ports=recipe["ports"].toArray();
        for(int i=0;i<inputs.size();++i){bool found=false;for(auto p:ports)if(p.toObject()["portId"].toString()==inputs[i].toArray()[0].toString()){u[size_t(i)]-=p.toObject()["initialValue"].toDouble();found=true;}require(found,"Missing actuator working voltage");}
        V f(size_t(exported["disturbanceInputs"].toArray().size()));for(auto v:exported["forceCatalog"].toArray()){const auto c=v.toObject();if(c.contains("disturbanceColumn"))f[size_t(c["disturbanceColumn"].toInt())]=physicalForce.at(size_t(c["physicalIndex"].toInt()));}
        u.insert(u.end(),f.begin(),f.end());return u;
    }
    Sample sample(const RStructuralTrussDynamicsState &s,const std::vector<RStructuralPiezoNativeReadoutSample> &readouts,const V &va)const{
        Sample p;p.time=s.time;p.physicalQ=s.displacements;p.physicalV=s.velocities;p.physicalA=s.accelerations;p.applied=va;
        // Actual axial bridge export: P=I_(3N), T is a 3N-by-r free-DOF
        // selector, q0 is the complete 3N reference. Keep P explicit so this
        // projection cannot silently borrow the layered generalized q0 layout.
        const auto physical=matrix(exported["P"]),t=matrix(exported["T"]);const auto q0=numbers(exported["q0"]);
        require(physical.size()==s.displacements.size()&&!physical.empty()&&physical[0].size()==q0.size()&&t.size()==q0.size(),"Axial P/T/q0 physical projection dimensions");
        const auto dq=plus(s.displacements,mv(physical,q0),-1);const auto q=tmv(t,tmv(physical,dq)),v=tmv(t,tmv(physical,s.velocities));
        require(norm(plus(mv(physical,mv(t,q)),dq,-1))<=1e-12*(1+norm(dq)),"Axial displacement projection roundtrip");
        require(norm(plus(mv(physical,mv(t,v)),s.velocities,-1))<=1e-12*(1+norm(s.velocities)),"Axial velocity projection roundtrip");p.x=state(q,v);
        std::map<QString,double> readings;for(const auto &r:readouts)readings[r.portId]=r.deltaValue;finish(p,readings);return p;
    }
    Sample sample(const RStructuralPiezoLayeredDynamicsState &s)const{
        Sample p;p.time=s.integrator.state.time;p.x=state(V(s.integrator.state.q.begin(),s.integrator.state.q.end()),V(s.integrator.state.v.begin(),s.integrator.state.v.end()));
        p.physicalQ=s.physicalDisplacements;p.physicalV=s.physicalVelocities;p.physicalA=s.physicalAccelerations;p.applied=s.heldActuatorVoltages;
        std::map<QString,double> readings;for(const auto &r:s.readouts)readings[r.portId]=r.deltaValue;finish(p,readings);return p;
    }
    Sample current()const{return axial?sample(rod->acceptedState(),rod->acceptedRuntime().readouts,rod->acceptedRuntime().lastApplied):sample(layered->acceptedState());}
    QByteArray bytes()const{return axial?RStructuralPiezoAxialRestartCodec::encode(rod->acceptedRuntime()):RStructuralPiezoLayeredRestartCodec::encode(layered->acceptedRuntime());}
    RModel acceptedModel()const{return axial?rod->acceptedModel(model):layered->acceptedModel(model);}
    bool remaining()const{return axial?rod->hasRemainingSteps():layered->hasRemainingSteps();}
    void step(){if(axial){auto p=rod->prepare(model);rod->validatePrepared(*p);rod->commit(*p);}else{auto p=layered->prepare(model);layered->validatePrepared(*p);layered->commit(*p);}}
    Trace run(Trace *acceptedTrace=nullptr){Trace local;Trace &trace=acceptedTrace?*acceptedTrace:local;trace.samples.push_back(current());const bool mpc=recipe["controller"].toObject()["kind"].toString()=="MPC";
        while(remaining()){step();trace.samples.push_back(current());for(double v:trace.samples.back().applied)trace.voltagePeak=std::max(trace.voltagePeak,std::abs(v));
            bool limited=false;if(axial){for(const auto &a:rod->acceptedRuntime().actions)limited=limited||a.amplitudeClipped||a.rateClipped;}else{for(const auto &a:layered->acceptedRuntime().actions)limited=limited||a.amplitudeClipped||a.rateClipped;}if(limited)++trace.limitedFESteps;
            if(mpc){const auto frame=QJsonDocument::fromJson(axial?rod->acceptedRuntime().lastFrameBytes:layered->acceptedRuntime().lastFrameBytes).object();const auto kkt=frame["diagnostics"].toObject()["mpc"].toObject();require(!kkt.isEmpty(),"Missing actual MPC KKT certificate");
                QJsonObject persistedKKT;for(const char *key:{"primal","stationarity","dual","complementarity","actionStationarity"}){require(kkt[key].isDouble()&&std::isfinite(kkt[key].toDouble()),"Missing or non-finite KKT residual");persistedKKT[key]=kkt[key];trace.kktPeak=std::max(trace.kktPeak,std::abs(kkt[key].toDouble()));}trace.mpcKKTFrames.append(persistedKKT);}
        }trace.finalBytes=bytes();trace.ticks=axial?rod->acceptedRuntime().controlTick:layered->acceptedRuntime().controlTick;return trace;
    }
    std::vector<Sample> replay(const Trace &trace,uint subdivisions)const{
        const double t0=trace.samples[0].time,dt=recipe["dt"].toDouble();std::vector<Sample> out{trace.samples[0]};
        auto force=[&](double time){const double z=(time-t0)/dt;const size_t interval=std::min(trace.samples.size()-2,size_t(std::max(0.,std::floor(z))));const double fraction=std::clamp(z-double(interval),0.,1.);return plus(trace.samples[interval].force,plus(trace.samples[interval+1].force,trace.samples[interval].force,-1),fraction);};
        if(axial){const RStructuralTrussDynamics driver(rod->system());auto p=driver.initialize(t0,trace.samples[0].physicalQ,trace.samples[0].physicalV,rod->adapter().initialCompleteInputs());
            RStructuralPiezoNativeReadout readout(rod->adapter(),model.getProblemSetup().getStructuralPiezoReadoutSetup(),rod->system());
            const auto background=numbers(exported["backgroundForce"]);const auto inputs=exported["inputs"].toArray();
            for(size_t i=1;i<trace.samples.size();++i){for(uint s=1;s<=subdivisions;++s){auto in=rod->adapter().initialCompleteInputs();const uint64_t index=(i-1)*subdivisions+s;const double left=t0+double(index-1)*dt/subdivisions,right=t0+double(index)*dt/subdivisions;
                    in.externalForces=plus(background,force(left));in.rightExternalForces=plus(background,force(right));
                    for(int j=0;j<inputs.size();++j)in.memberInputs[rod->adapter().maps().memberByPortId.at(inputs[j].toArray()[0].toString())]=RStructuralTrussVoltage{trace.samples[i].applied[size_t(j)]};
                    p=driver.trialStepOnGrid(p,in,t0,index,dt/subdivisions,rod->adapter().options()).candidate;
                }out.push_back(sample(p,readout.observe(p),trace.samples[i].applied));}
        }else{const RStructuralPiezoLayeredDynamics driver(layered->adapter(),RMatrixSolverConf(RMatrixSolverConf::GMRES),t0,force,"recorded-FOH-example/v1");auto p=driver.initialize(trace.samples[0].applied);
            for(size_t i=1;i<trace.samples.size();++i){for(uint s=1;s<=subdivisions;++s)p=driver.trialOnGrid(p,trace.samples[i].applied,(i-1)*subdivisions+s,dt/subdivisions);out.push_back(sample(p));}
        }return out;
    }
private:
    void finish(Sample &p,const std::map<QString,double> &readings)const{
        p.force=forcing(p.time);const V physicalQ0=mv(matrix(exported["P"]),numbers(exported["q0"]));
        for(auto value:exported["outputs"].toArray()){const auto id=value.toArray()[0].toString();auto read=readings.find(id);if(read!=readings.end()){p.y.push_back(read->second);continue;}
            bool found=false;for(auto v:recipe["design"].toObject()["outputs"].toArray()){const auto o=v.toObject();if(o["id"].toString()!=id)continue;
                const auto channel=QString("node:%1:%2").arg(o["physicalNodeId"].toInt()).arg(o["dof"].toString());for(auto c:exported["forceCatalog"].toArray())if(c.toObject()["channelId"].toString()==channel){const size_t index=size_t(c.toObject()["physicalIndex"].toInt());const auto q=o["quantity"].toString();p.y.push_back(q=="displacement"?p.physicalQ[index]-physicalQ0[index]:q=="velocity"?p.physicalV[index]:p.physicalA[index]);found=true;break;}
            }require(found,"Actual mechanical output map missing");
        }
    }
};

QJsonObject recipe(const QString &family){
    auto r=RPiezoExperimentService::exampleRecipe(RModel{},family);require(!r.isEmpty(),"Production example recipe missing");r["replaceLegacyConditions"]=true;
    // A broad square plate is the predetermined plate example, rather than a
    // narrow strip with fast transverse modes and a shortened capped window.
    if(family=="flatPlate"){auto geometry=r["template"].toObject();geometry["width"]=1.;r["template"]=geometry;}
    // A declared, reproducible finite SI material, compiled by the same source
    // pipeline. The strong but admissible coupling avoids vacuous zero actions.
    const auto tensor=RStructuralPiezoContinuumMaterial::isotropic(1e6,.25,1000);QJsonArray materials;
    for(auto v:r["materials"].toArray()){auto m=v.toObject();m["elasticityOrCompliance"]=array(V(tensor.elasticity.begin(),tensor.elasticity.end()));m["density"]=1000.;V coupling(18),epsilon(9);if(m["piezoelectric"].toBool()){if(family=="axialRod")coupling[0]=50;else coupling[12]=coupling[13]=50;epsilon[0]=epsilon[4]=epsilon[8]=.01;}m["coupling"]=array(coupling);m["permittivity"]=array(epsilon);m["sourceKind"]="example";m["sourceId"]="range-fea/PC10/finite-SI-strong-coupling/"+family+"/"+m["materialId"].toString()+"/v1";m["notes"]="Independent synthetic SI example E=1e6 Pa, nu=.25, rho=1000 kg/m^3, e=50 C/m^2, eps=.01 F/m; no vendor or hardware qualification";m.remove("sourceSha256");m["sourceSha256"]=sha(json(m));materials.append(m);}r["materials"]=materials;
    auto ports=r["ports"].toArray();
    if(family=="axialRod"){require(ports.size()==4,"Expected four actual rod elements/ports");auto p=ports[0].toObject();p["role"]="actuatorSensor";ports[0]=p;for(int i=1;i<4;++i){p=ports[i].toObject();p["readout"]=i==1?"openCircuitVoltage":i==2?"shortCircuitCharge":"idealChargeAmplifier";if(i==3){p["Cf"]=.01;p["amplifierPolarity"]=-1;}ports[i]=p;}}
    else{auto drive=ports[0].toObject();drive["role"]="actuatorSensor";ports[0]=drive;auto shorted=ports[1].toObject();shorted["portId"]="short";shorted["readout"]="shortCircuitCharge";ports.append(shorted);auto amp=shorted;amp["portId"]="amplifier";amp["readout"]="idealChargeAmplifier";amp["Cf"]=.01;amp["amplifierPolarity"]=-1;ports.append(amp);
        // Shared interface values, including the exact final host interface.
        // Independent arithmetic at adjacent layer endpoints can overlap by
        // one binary64 ulp; retain the production strict overlap guard.
        const double edges[]={-.06,-.06+.01/3,-.06+2*.01/3,-.05};
        QJsonArray members;for(auto v:r["members"].toArray()){auto member=v.toObject();auto layers=member["layers"].toArray();auto host=layers[0].toObject();host["lowerZ"]=edges[3];host["upperZ"]=.05;auto active=layers[1].toObject();active["lowerZ"]=host["upperZ"];active["upperZ"]=.06;auto sensor=layers[2].toObject();QJsonArray stack{host,active};for(int i=0;i<3;++i){auto s=sensor;s["lowerZ"]=edges[i];s["upperZ"]=edges[i+1];s["portId"]=i==0?"sense":i==1?"short":"amplifier";stack.append(s);}member["layers"]=stack;members.append(member);}r["members"]=members;
    }r["ports"]=ports;r["damping"]=QJsonObject{{"kind","rayleigh"},{"rayleighAlpha",.02},{"rayleighBeta",1e-5}};r["dt"]=1e-5;r["steps"]=1;return r;
}

struct Scenario {QJsonObject recipe,fullExport,truncatedExport,presetMetadata;uint stride=1;};
Scenario scenario(const QString &family,const QString &kind,bool forced,uint stride){
    auto r=recipe(family);Experiment probe(r);const auto design=probe.exported["design"].toObject();const auto omega=numbers(design["omega"]);require(!omega.empty()&&omega.front()>0,"Positive actual FE modal frequencies required");
    const double pi=std::acos(-1.),dt=.12/omega.back();const double duration=2*pi/omega.front();const uint steps=uint(std::ceil(duration/dt));
    require(steps>=48&&steps<=12000,"One-period representative window exceeds bounded admission; do not truncate it silently");r["dt"]=dt;r["steps"]=int(steps);
    const auto phi=matrix(design["Phi"]);V unitMode(omega.size());unitMode[0]=1;
    const auto physicalPhi=mv(matrix(probe.exported["P"]),mv(matrix(probe.exported["T"]),mv(phi,unitMode)));
    double peak=0;for(double x:physicalPhi)peak=std::max(peak,std::abs(x));require(peak>0,"Missing first-mode physical motion");
    QJsonArray velocities;const uint components=family=="axialRod"?3:6;for(auto value:probe.exported["nodes"].toArray()){const auto node=value.toObject();const uint offset=uint(node["physicalOffset"].toInt());V values(6);for(uint j=0;j<components;++j)values[j]=1e-5*physicalPhi.at(offset+j)/peak;velocities.append(QJsonObject{{"physicalNodeId",node["physicalNodeId"]},{"values",array(values)}});}r["initialVelocities"]=velocities;
    if(forced){const auto output=r["design"].toObject()["outputs"].toArray()[0].toObject();r["excitation"]=QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId",QString("node:%1:%2").arg(output["physicalNodeId"].toInt()).arg(output["dof"].toString())},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","1e-6 + 2e-5*t"}}}}}}}};}
    Experiment full(r);auto selected=r["design"].toObject();const auto clusters=full.exported["design"].toObject()["clusters"].toArray();require(!clusters.isEmpty(),"Actual modal cluster certificate missing");const auto indices=clusters[0].toArray();require(!indices.isEmpty(),"First cluster missing");selected["modeIndices"]=indices;auto truncatedRecipe=r;truncatedRecipe["design"]=selected;Experiment truncated(truncatedRecipe);
    // Full-basis control is the primary qualification. A first-cluster design
    // cannot certify stability of omitted FE modes; keep it as a separately
    // identified replay approximation rather than assuming spillover is small.
    const auto b=matrix(full.exported["B"]),phi_=matrix(full.exported["design"].toObject()["Phi"]);V col(b.size());for(size_t i=0;i<b.size();++i)col[i]=b[i][0];const double modalInput=tmv(phi_,col)[0];require(std::abs(modalInput)>1e-15,"First actual modal actuation channel must be nonzero");
    const double targetDamping=.05,s=std::pow(2*std::sqrt(1+targetDamping*targetDamping)-1,2)-1;
    // Q is twice physical mechanical energy in mass-normalized coordinates.
    // R follows the isolated first-mode continuous energy-LQR damping formula;
    // this is a fixed effort-weight prescription, not a claim that every pole
    // of the actual sampled full model has the prescribed damping ratio.
    const double effort=modalInput*modalInput/(s*omega[0]*omega[0]);require(std::isfinite(effort)&&effort>0,"Finite physical controller effort weight required");
    QJsonObject controller{{"kind",kind},{"controlEveryFESteps",int(stride)}};
    if(kind!="none"){controller["voltageLower"]=QJsonArray{-20.};controller["voltageUpper"]=QJsonArray{20.};controller["deltaVoltageLimits"]=QJsonArray{20.};}
    if(kind=="PI"){
        // Actual selected nodal velocity measurements form the work-conjugate
        // output B^T*v, giving dissipative proportional feedback over ALL FE
        // modes. A tip-only gain derived from the first mode is noncollocated
        // and does not establish the sign of high-mode control work.
        const auto physicalInput=mv(matrix(full.exported["P"]),mv(matrix(full.exported["T"]),col));const double gamma=2*targetDamping*omega[0]/(modalInput*modalInput);const char *axes[]={"X","Y","Z","RX","RY","RZ"};const double inputScale=norm(physicalInput);V proportional,integral;QJsonArray selectedOutputs;auto edited=r["design"].toObject();auto outputs=edited["outputs"].toArray();
        for(auto value:full.exported["nodes"].toArray()){const auto node=value.toObject();const uint offset=uint(node["physicalOffset"].toInt());for(uint j=0;j<components;++j){const double coefficient=physicalInput.at(offset+j);if(std::abs(coefficient)<=1e-14*inputScale)continue;const auto id=QString("actuation_velocity_%1_%2").arg(node["physicalNodeId"].toInt()).arg(axes[j]);outputs.append(QJsonObject{{"id",id},{"physicalNodeId",node["physicalNodeId"]},{"dof",axes[j]},{"quantity","velocity"},{"unit",j<3?"m/s":"rad/s"}});selectedOutputs.append(id);proportional.push_back(gamma*coefficient);integral.push_back(.002*omega[0]*gamma*coefficient);}}
        require(!proportional.empty(),"Work-conjugate actual PI measurements missing");edited["outputs"]=outputs;r["design"]=edited;QJsonArray kp,ki;kp.append(array(proportional));ki.append(array(integral));controller["kp"]=kp;controller["ki"]=ki;controller["selectedOutputs"]=selectedOutputs;
    }
    if(kind=="LQR"||kind=="MPC"){V cost(2*omega.size(),1.);for(size_t i=0;i<omega.size();++i)cost[i]=omega[i]*omega[i];controller["Q"]=diagonal(cost);controller["R"]=diagonal(V{effort});if(kind=="MPC"){controller["Qf"]=controller["Q"];controller["horizon"]=5;controller["verificationTolerance"]=1e-7;}}
    const QJsonObject metadata{{"revision","PC10-preset-v2"},{"controlBasis","all-certified-FE-modes"},{"replayTruncation",indices},{"targetFirstModeEnergyLQRDamping",targetDamping},{"actualFirstModalInput",modalInput},{"energyEffortWeight",effort},{"requestedDuration",duration},{"actualDuration",steps*dt},{"firstModeCycles",steps*dt*omega[0]/(2*pi)},{"maxModalAnglePerFEStep",dt*omega.back()},{"maxModalAnglePerControlSample",stride*dt*omega.back()},{"windowAdmissionMaximumSteps",12000},{"geometry",r["template"]}};
    r["controller"]=controller;return {r,full.exported,truncated.exported,metadata,stride};
}

struct ReplayMetrics {double native1=0,native2=0,native4=0,rk4=0,fullBasis=0,truncated=0,defectBound=0,peak=0,maxBoundExcess=0;};
ReplayMetrics measureReplay(const Experiment &e,const Trace &trace,const QJsonObject &fullExport,const QJsonObject &truncatedExport){
    const auto selected=truncatedExport["design"].toObject(),full=fullExport["design"].toObject();const auto force=selected["forceAware"].toObject();
    Reference fe(force["FE"].toObject()),rom(force["ROM"].toObject()),fullRom(full["forceAware"].toObject()["ROM"].toObject());
    const auto phi=matrix(selected["Phi"]),allPhi=matrix(full["Phi"]);V x=trace.samples[0].x,xFine=x,z=project(e.exported,phi,x),allZ=project(e.exported,allPhi,x);
    const EnergyMetric energy(e.exported);auto fine=e.replay(trace,2),finer=e.replay(trace,4);ReplayMetrics m;m.defectBound=energy(plus(x,lift(phi,z),-1));
    const double dt=e.recipe["dt"].toDouble();
    for(size_t i=1;i<trace.samples.size();++i){const auto &s=trace.samples[i];const auto ul=e.input(s.applied,trace.samples[i-1].force),ur=e.input(s.applied,s.force);const double h=s.time-trace.samples[i-1].time;
        // Integral energy-norm defect bound for a passive FE generator. The
        // supremum is numerically resolved at 16 subdivisions, with a 10%
        // quadrature allowance. It is explicitly a numerical bound, not a
        // proof for arbitrary unresolved forcing or arbitrary nonlinear motion.
        double defect=0;for(uint j=0;j<16;++j){const auto u=plus(ul,plus(ur,ul,-1),double(j)/16);const auto lifted=lift(phi,z);defect=std::max(defect,energy(plus(lift(phi,rom.derivative(z,u)),fe.derivative(lifted,u),-1)));const auto unext=plus(ul,plus(ur,ul,-1),double(j+1)/16);z=rom.advance(z,u,unext,h/16,1);}
        m.defectBound+=1.1*h*defect;x=fe.advance(x,ul,ur,h,8);xFine=fe.advance(xFine,ul,ur,h,16);allZ=fullRom.advance(allZ,ul,ur,h,16);
        m.peak=std::max(m.peak,energy(xFine));m.native1=std::max(m.native1,energy(plus(s.x,xFine,-1)));m.native2=std::max(m.native2,energy(plus(fine[i].x,xFine,-1)));m.native4=std::max(m.native4,energy(plus(finer[i].x,xFine,-1)));m.rk4=std::max(m.rk4,energy(plus(x,xFine,-1)));m.fullBasis=std::max(m.fullBasis,energy(plus(lift(allPhi,allZ),xFine,-1)));m.truncated=std::max(m.truncated,energy(plus(lift(phi,z),xFine,-1)));
        m.maxBoundExcess=std::max(m.maxBoundExcess,energy(plus(lift(phi,z),xFine,-1))-m.defectBound-1e-8*std::max(m.peak,1e-14));
        require(std::abs(h-dt)<=dt*1e-10,"Unexpected noncanonical source interval");
    }return m;
}
bool replayQualified(const ReplayMetrics &m){const double floor=1e-6*m.peak;return m.peak>1e-14&&m.rk4/m.peak<1e-7&&m.fullBasis/m.peak<2e-7&&m.native1/m.peak<.025&&m.native2<=.55*m.native1+floor&&m.native4<=.55*m.native2+floor&&m.maxBoundExcess<=0;}
void verifyReadouts(const Experiment &e,const Trace &trace){
    Reference fe(e.exported["design"].toObject()["forceAware"].toObject()["FE"].toObject());const auto phi=matrix(e.exported["design"].toObject()["Phi"]);const auto lq=matrix(e.exported["Lq"]),d=matrix(e.exported["D"]);V peak(fe.cy.size()),error(peak.size());
    for(const auto &s:trace.samples){const auto expected=fe.output(s.x,e.input(s.applied,s.force));require(expected.size()==s.y.size(),"Output shape mismatch");for(size_t j=0;j<peak.size();++j){peak[j]=std::max(peak[j],std::abs(s.y[j]));error[j]=std::max(error[j],std::abs(s.y[j]-expected[j]));}}
    for(size_t j=0;j<peak.size();++j)require(error[j]<=1e-11+2e-6*peak[j],"Actual output omits displacement/velocity/acceleration/force/electrical direct terms");
    for(auto p:e.recipe["ports"].toArray()){const auto port=p.toObject();const auto id=port["portId"].toString();int row=-1;const auto outputs=e.exported["outputs"].toArray();for(int i=0;i<outputs.size();++i)if(outputs[i].toArray()[0].toString()==id)row=i;require(row>=0,"Missing positive-piezo output row");
        require(peak[size_t(row)]>1e-15,"Vacuous positive-piezo time readout");V modal(phi[0].size());for(size_t i=0;i<phi.size();++i)for(size_t j=0;j<modal.size();++j)modal[j]+=lq[size_t(row)][i]*phi[i][j];require(norm(modal)>1e-15,"Vacuous positive-piezo modal readout");
        if(port["readout"].toString()=="actuatorCharge")require(norm(d[size_t(row)])>0,"Actuator charge capacitive direct feedthrough missing");else require(norm(d[size_t(row)])<1e-12,"Isolated ideal sensor has unexpected actuator direct term");
    }
}
double rmsEnergy(const QJsonObject &e,const Trace &trace){const EnergyMetric energy(e);double sum=0;for(size_t i=1;i<trace.samples.size();++i){const double n=energy(trace.samples[i].x);sum+=n*n;}return std::sqrt(sum/double(trace.samples.size()-1));}
QJsonObject trajectoryMetrics(const QJsonObject &e,const Trace &trace,const QJsonArray &retained){
    require(!trace.samples.empty(),"Trajectory metrics require accepted samples");const EnergyMetric energy(e);const auto phi=matrix(e["design"].toObject()["Phi"]);const auto omega=numbers(e["design"].toObject()["omega"]);std::vector<bool> low(omega.size());for(auto i:retained)low.at(size_t(i.toInt()))=true;
    const double initial=.5*std::pow(energy(trace.samples[0].x),2);double peak=initial,highPeak=0,highFinal=0,tipPeak=0,tipRMS=0,integral=0;
    int tip=-1;const auto outputs=e["outputs"].toArray();for(int i=0;i<outputs.size();++i)if(outputs[i].toArray()[0].toString()=="tip_displacement")tip=i;
    for(size_t i=0;i<trace.samples.size();++i){const auto &s=trace.samples[i];const double en=.5*std::pow(energy(s.x),2);peak=std::max(peak,en);const auto modal=project(e,phi,s.x);double high=0;for(size_t j=0;j<omega.size();++j)if(!low[j])high+=.5*(omega[j]*omega[j]*modal[j]*modal[j]+modal[omega.size()+j]*modal[omega.size()+j]);highPeak=std::max(highPeak,high);highFinal=high;
        if(tip>=0){const double displacement=s.y.at(size_t(tip));tipPeak=std::max(tipPeak,std::abs(displacement));tipRMS+=displacement*displacement;}
        if(i>0){const double before=.5*std::pow(energy(trace.samples[i-1].x),2);integral+=(s.time-trace.samples[i-1].time)*.5*(before+en);}
    }return {{"initialMechanicalEnergyJ",initial},{"peakMechanicalEnergyJ",peak},{"finalMechanicalEnergyJ",.5*std::pow(energy(trace.samples.back().x),2)},{"integratedMechanicalEnergyJs",integral},{"energyNormRMS",trace.samples.size()>1?rmsEnergy(e,trace):0.},{"tipDisplacementPeakM",tipPeak},{"tipDisplacementRMSM",std::sqrt(tipRMS/trace.samples.size())},{"outsideFirstClusterPeakEnergyJ",highPeak},{"outsideFirstClusterFinalEnergyJ",highFinal},{"outsideFirstClusterPeakOverInitialEnergy",initial>0?highPeak/initial:0.},{"appliedVoltagePeakV",trace.voltagePeak},{"amplitudeOrRateLimitedFESteps",double(trace.limitedFESteps)},{"completedFESteps",double(trace.samples.size()-1)}};
}

// A separate ROM closed-loop experiment: each proposal sees the ROM's own
// reconstructed state/output, not the voltage trace from the FE experiment.
// This qualifies the exported design/reference simulation only; NativeRun is
// still the sole actual FE plant and acceptance/restart authority above.
Trace independentROMClosedLoop(const Experiment &e,const Trace &clock,bool controlOn){
    const auto design=e.exported["design"].toObject(),force=design["forceAware"].toObject();const auto phi=matrix(design["Phi"]);
    const Reference rom(force["ROM"].toObject()),fe(force["FE"].toObject());V z=project(e.exported,phi,clock.samples[0].x);
    const auto transform=matrix(e.exported["T"]),physical=matrix(e.exported["P"]);const auto q0=mv(physical,numbers(e.exported["q0"]));
    const uint stride=uint(e.recipe["controller"].toObject()["controlEveryFESteps"].toInt());require(stride>0,"ROM control stride must be positive");V applied=clock.samples[0].applied,held=applied;
    QByteArray opaque=e.axial?e.axialBridge.callbacks.initialOpaqueControllerState:e.layeredBridge.callbacks.initialOpaqueControllerState;uint tick=0;Trace out;
    struct Boundary {Sample sample;RStructuralTrussDynamicsState axial;std::vector<RStructuralPiezoNativeReadoutSample> axialReadouts;RStructuralPiezoLayeredDynamicsState layered;};
    // Retain port metadata and constant absolute-reference readout offsets only.
    // No FE trajectory value enters a ROM proposal or postFE boundary.
    const auto axialMetadata=e.axial?e.rod->acceptedRuntime().readouts:std::vector<RStructuralPiezoNativeReadoutSample>{};
    const auto layeredMetadata=e.axial?std::vector<RStructuralPiezoLayeredDynamicsReadout>{}:e.layered->acceptedState().readouts;
    const auto initialApplied=applied;
    auto boundary=[&](size_t i){
        Boundary b;auto &s=b.sample;s.time=clock.samples[i].time;s.x=lift(phi,z);s.applied=applied;s.force=clock.samples[i].force;const size_t n=s.x.size()/2;
        const V q(s.x.begin(),s.x.begin()+n),v(s.x.begin()+n,s.x.end());const auto u=e.input(applied,s.force),dx=fe.derivative(s.x,u);s.y=rom.output(z,u);
        s.physicalQ=plus(mv(physical,mv(transform,q)),q0);s.physicalV=mv(physical,mv(transform,v));
        // Exported ROM Cy retains the full physical FE acceleration readout
        // closure, including La force/electrical/bias direct terms. Reconstruct
        // that SAME closure at this ROM state/input; no accepted FE a is read.
        s.physicalA=mv(physical,mv(transform,V(dx.begin()+n,dx.end())));
        const auto check=fe.output(s.x,u);require(norm(plus(s.y,check,-1))<=1e-11*(1+norm(check)),"ROM output versus exported physical readout closure");
        std::map<QString,double> readings;const auto outputs=e.exported["outputs"].toArray();for(int j=0;j<outputs.size();++j)readings[outputs[j].toArray()[0].toString()]=s.y[size_t(j)];
        if(e.axial){auto &p=b.axial;p.time=s.time;p.displacements=s.physicalQ;p.velocities=s.physicalV;p.accelerations=s.physicalA;p.appliedInputs=e.rod->adapter().initialCompleteInputs();p.appliedInputs.externalForces=plus(numbers(e.exported["backgroundForce"]),s.force);
            const auto inputs=e.exported["inputs"].toArray();for(int j=0;j<inputs.size();++j)p.appliedInputs.memberInputs[e.rod->adapter().maps().memberByPortId.at(inputs[j].toArray()[0].toString())]=RStructuralTrussVoltage{applied[size_t(j)]};
            b.axialReadouts=axialMetadata;for(auto &r:b.axialReadouts){require(readings.count(r.portId),"ROM electrical readout missing");const double reference=r.value-r.deltaValue;r.deltaValue=readings.at(r.portId);r.value=reference+r.deltaValue;}
        }else{auto &p=b.layered;p.integrator.state.time=s.time;p.integrator.state.q=RRVector(q);p.integrator.state.v=RRVector(v);p.integrator.state.a=RRVector(V(dx.begin()+n,dx.end()));p.physicalDisplacements=s.physicalQ;p.physicalVelocities=s.physicalV;p.physicalAccelerations=s.physicalA;p.heldActuatorVoltages=applied;p.initialActuatorVoltages=initialApplied;p.perturbationForces=s.force;
            p.electrical=e.layered->adapter().electrical().evaluate(plus(numbers(e.exported["q0"]),mv(transform,q)),applied);p.readouts=layeredMetadata;for(auto &r:p.readouts){require(readings.count(r.portId),"ROM electrical readout missing");const double reference=r.value-r.deltaValue;r.deltaValue=readings.at(r.portId);r.value=reference+r.deltaValue;}
        }return b;
    };
    auto history=[&](uint completed,const char *phase){const auto s=QJsonDocument::fromJson(opaque).object();require(s["feStep"].toInt()==int(completed)&&s["controlTick"].toInt()==int(tick)&&s["phase"].toString()==QString::fromLatin1(phase),"Independent ROM opaque counter/phase mismatch");require(numbers(s["previousApplied"])==applied,"Independent ROM previous-applied mismatch");require(plus(numbers(s["heldDelta"]),initialApplied)==held,"Independent ROM held-request mismatch");};
    if(controlOn){history(0,"ready");if(e.axial)e.axialBridge.callbacks.validateOpaque(opaque,0,0,0);else e.layeredBridge.callbacks.validateOpaque(opaque,0,0,0);}out.samples.push_back(boundary(0).sample);
    for(size_t i=1;i<clock.samples.size();++i){const uint completed=uint(i-1);const double dt=clock.samples[i].time-clock.samples[i-1].time;
        if(controlOn)history(completed,"ready");
        if(controlOn&&completed%stride==0){const auto b=boundary(i-1);
            if(e.axial){const auto p=e.axialBridge.callbacks.propose(b.axial,b.axialReadouts,completed,tick,opaque);held=p.requestedVoltages;opaque=p.nextOpaqueState;}
            else{const auto p=e.layeredBridge.callbacks.propose(b.layered,completed,tick,opaque);held=p.requestedVoltages;opaque=p.nextOpaqueState;}
            ++tick;history(completed,"proposed");
        }
        if(controlOn){for(size_t j=0;j<applied.size();++j){if(e.axial){const auto &b=e.axialBridge.callbacks.envelopes[j];applied[j]=std::clamp(std::clamp(held[j],b.minimum,b.maximum),applied[j]-b.maximumRate*dt,applied[j]+b.maximumRate*dt);}else{const auto &b=e.layeredBridge.callbacks.envelopes[j];applied[j]=std::clamp(std::clamp(held[j],b.minimum,b.maximum),applied[j]-b.maximumRate*dt,applied[j]+b.maximumRate*dt);}}}
        const auto ul=e.input(applied,clock.samples[i-1].force),ur=e.input(applied,clock.samples[i].force);z=rom.advance(z,ul,ur,dt,16);const auto accepted=boundary(i);out.samples.push_back(accepted.sample);
        if(controlOn){if(e.axial){opaque=e.axialBridge.callbacks.postFE(accepted.axial,accepted.axialReadouts,uint(i),tick,applied,opaque);e.axialBridge.callbacks.validateOpaque(opaque,uint(i),tick,uint(i)%stride);}else{opaque=e.layeredBridge.callbacks.postFE(accepted.layered,uint(i),tick,applied,opaque);e.layeredBridge.callbacks.validateOpaque(opaque,uint(i),tick,uint(i)%stride);}history(uint(i),"ready");require(tick==(uint(i)+stride-1)/stride,"Independent ROM control-sampling tick mismatch");}
        for(double v:applied)out.voltagePeak=std::max(out.voltagePeak,std::abs(v));
    }out.ticks=tick;return out;
}
QJsonArray trajectory(const Trace &trace){QJsonArray samples;for(const auto &s:trace.samples)samples.append(QJsonObject{{"time",s.time},{"deltaState",array(s.x)},{"deltaReadouts",array(s.y)},{"heldAbsoluteVoltage",array(s.applied)},{"physicalEndpointPerturbationForce",array(s.force)}});return samples;}
void writeEvidence(const Experiment &e,const Trace &trace,const Scenario &setup,const QJsonObject &metrics,const QString &status,const Trace *off=nullptr,const Trace *romOn=nullptr,const Trace *romOff=nullptr){
    const auto destination=qEnvironmentVariable("RANGE_PIEZO_DELIVERY_EVIDENCE_DIR");if(destination.isEmpty())return;
    require(QDir().mkpath(destination),"Cannot create explicitly requested evidence directory");
    QJsonObject record{{"schema","range.piezo-representative-case-run/v2"},{"case",QString::fromLatin1(QTest::currentDataTag())},{"qualificationStatus",status},{"qualified",status=="QUALIFIED"},{"presetMetadata",setup.presetMetadata},{"rawRecipe",e.recipe},{"recipeSha256",sha(json(e.recipe))},{"sourceFingerprint",e.exported["sourceFingerprint"]},{"export",e.exported},{"fullBasisExport",setup.fullExport},{"truncatedBasisExport",setup.truncatedExport},{"actualNativeFETrace",trajectory(trace)},{"metrics",metrics}};
    if(!trace.finalBytes.isEmpty())record["acceptedRuntimeSha256"]=sha(trace.finalBytes);if(off)record["actualNativeFEOffTrace"]=trajectory(*off);if(romOn)record["independentROMOnTrace"]=trajectory(*romOn);if(romOff)record["independentROMOffTrace"]=trajectory(*romOff);
    QFile file(QDir(destination).filePath(QString::fromLatin1(QTest::currentDataTag())+".json"));require(file.open(QIODevice::WriteOnly),"Cannot open requested evidence file");const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Indented);require(file.write(bytes)==bytes.size(),"Incomplete evidence write");
}
}

#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoDeliveryExamples:public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {RLogger::getInstance().setLevel(R_LOG_LEVEL_QUIET);}
    void sameSourceScientificEvidence_data();
    void sameSourceScientificEvidence();
    void layeredModalClusterFromActualFE();
    void layeredExternalTimeFunctionEndpointLedger();
    void savedExperimentControllerPhases_data();
    void savedExperimentControllerPhases();
};
void TestPiezoDeliveryExamples::sameSourceScientificEvidence_data(){
    QTest::addColumn<QString>("family");QTest::addColumn<QString>("kind");QTest::addColumn<bool>("forced");QTest::addColumn<uint>("stride");
    for(const auto &family:{QString("axialRod"),QString("planarBeam"),QString("flatPlate")})for(const auto &kind:{QString("none"),QString("PI"),QString("LQR"),QString("MPC")})for(bool forced:{false,true})QTest::newRow(qPrintable(family+"-"+kind+(forced?"-forced":"-free")))<<family<<kind<<forced<<(forced?3u:2u);
}
void TestPiezoDeliveryExamples::sameSourceScientificEvidence() {
    QFETCH(QString,family);QFETCH(QString,kind);QFETCH(bool,forced);QFETCH(uint,stride);Scenario setup;std::unique_ptr<Experiment> instance;Trace trace,uncontrolled,romOn,romOff;QJsonObject metrics;
    const auto fail=[&](const QString &message){metrics["failure"]=message;qWarning().noquote()<<"PC10 FAILED"<<QTest::currentDataTag()<<json(metrics);
        if(instance){if(trace.samples.empty())trace.samples.push_back(instance->current());trace.finalBytes=instance->bytes();writeEvidence(*instance,trace,setup,metrics,"FAILED",uncontrolled.samples.empty()?nullptr:&uncontrolled,romOn.samples.empty()?nullptr:&romOn,romOff.samples.empty()?nullptr:&romOff);}
        else{const auto destination=qEnvironmentVariable("RANGE_PIEZO_DELIVERY_EVIDENCE_DIR");if(!destination.isEmpty()){require(QDir().mkpath(destination),"Cannot create failure evidence directory");QFile file(QDir(destination).filePath(QString::fromLatin1(QTest::currentDataTag())+".json"));require(file.open(QIODevice::WriteOnly),"Cannot open failure evidence file");const auto bytes=QJsonDocument(QJsonObject{{"schema","range.piezo-representative-case-run/v2"},{"case",QString::fromLatin1(QTest::currentDataTag())},{"qualified",false},{"qualificationStatus","FAILED_BEFORE_PRIMARY_FE_CONSTRUCTION"},{"family",family},{"controllerKind",kind},{"forced",forced},{"presetRevision","PC10-preset-v2"},{"rawRecipe",setup.recipe},{"metrics",metrics}}).toJson(QJsonDocument::Indented);require(file.write(bytes)==bytes.size(),"Incomplete failure evidence write");}}};
    double feedbackProposalDeltaPeakV=0.;try {
    setup=scenario(family,kind,forced,stride);instance=std::make_unique<Experiment>(setup.recipe);auto &e=*instance;
    Trace initial;initial.samples.push_back(e.current());initial.finalBytes=e.bytes();writeEvidence(e,initial,setup,metrics,"QUALIFICATION_IN_PROGRESS");
    uint physical=0;const auto type=family=="axialRod"?R_ELEMENT_TRUSS1:family=="planarBeam"?R_ELEMENT_BEAM1:R_ELEMENT_QUAD1;for(uint i=0;i<e.model.getNElements();++i)if(e.model.getElement(i).getType()==type)++physical;require(physical==4,"Representative source must contain four actual structural elements");
    for(const char *key:{"M","K","C"})require(e.exported[key]==setup.fullExport[key]&&e.exported[key]==setup.truncatedExport[key],"Full/selected ROM must use identical actual source operators");
    const auto runtime=e.exported["privateRuntime"].toObject();for(const char *key:{"python","numpy","scipy"})require(runtime[key].isString()&&!runtime[key].toString().isEmpty(),"Missing actual private runtime version");
    const auto design=e.exported["design"].toObject();require(design["massOrthogonalityError"].isDouble()&&design["massOrthogonalityError"].toDouble()<1e-8,"Actual modal mass orthogonality failed");for(double residual:numbers(design["modalResiduals"]))require(residual<1e-8,"Actual modal residual failed");
    const auto controller=design["controller"].toObject();if(kind=="LQR"){require(controller["riccatiRelativeResidual"].isDouble()&&controller["riccatiRelativeResidual"].toDouble()<1e-8&&controller["K"].isArray(),"Actual LQR Riccati certificate missing or failed");require(!controller["closedPoles"].toArray().isEmpty(),"LQR closed poles missing");for(auto p:controller["closedPoles"].toArray()){const auto pole=numbers(p);require(pole.size()==2&&std::hypot(pole[0],pole[1])<1.,"Actual LQR closed pole is not stable");}}
    if(kind!="none"){
        // The initial boundary is a real control sample for every N. Proposal
        // calls are pure: neither comparison consumes an accepted state.
        if(e.axial){auto p=e.rod->acceptedState();const auto &r=e.rod->acceptedRuntime();const auto a=e.axialBridge.callbacks.propose(p,r.readouts,r.completedFE,r.controlTick,r.opaqueControllerState);for(double &v:p.velocities)v*=1.1;const auto b=e.axialBridge.callbacks.propose(p,r.readouts,r.completedFE,r.controlTick,r.opaqueControllerState);feedbackProposalDeltaPeakV=norm(plus(a.requestedVoltages,b.requestedVoltages,-1));require(feedbackProposalDeltaPeakV>1e-14,"Actual controller ignores FE velocity feedback");}
        else{auto p=e.layered->acceptedState();const auto &r=e.layered->acceptedRuntime();const auto a=e.layeredBridge.callbacks.propose(p,r.completedFE,r.controlTick,r.opaqueControllerState);for(double &v:p.integrator.state.v)v*=1.1;for(double &v:p.physicalVelocities)v*=1.1;const auto b=e.layeredBridge.callbacks.propose(p,r.completedFE,r.controlTick,r.opaqueControllerState);feedbackProposalDeltaPeakV=norm(plus(a.requestedVoltages,b.requestedVoltages,-1));require(feedbackProposalDeltaPeakV>1e-14,"Actual controller ignores FE velocity feedback");}
    }
    e.run(&trace);require(trace.samples.size()>2&&trace.ticks==uint((trace.samples.size()-2+stride)/stride),"Actual FE control counters incorrect");metrics["actualNativeFEOn"]=trajectoryMetrics(e.exported,trace,setup.presetMetadata["replayTruncation"].toArray());writeEvidence(e,trace,setup,metrics,"ACCEPTED_FE_TRACE_NUMERICAL_CHECKS_PENDING");verifyReadouts(e,trace);const auto error=measureReplay(e,trace,setup.fullExport,setup.truncatedExport);
    const double scale=std::max(error.peak,1e-300);metrics["sameInputReplay"]=QJsonObject{{"nativeRelativeEnergyError",error.native1/scale},{"nativeHalfStepRelativeEnergyError",error.native2/scale},{"nativeQuarterStepRelativeEnergyError",error.native4/scale},{"refinement2Ratio",error.native2/std::max(error.native1,1e-300)},{"refinement4Ratio",error.native4/std::max(error.native2,1e-300)},{"RK4EightVersusSixteenRelativeError",error.rk4/scale},{"fullBasisRelativeError",error.fullBasis/scale},{"selectedBasisRelativeError",error.truncated/scale},{"resolvedDefectRelativeBound",error.defectBound/scale},{"maximumBoundExcess",error.maxBoundExcess},{"qualified",replayQualified(error)}};
    double ratio=1,romRatio=1;if(kind!="none"){auto off=setup.recipe;off["controller"]=QJsonObject{{"kind","none"},{"controlEveryFESteps",int(stride)}};Experiment baseline(off);for(const char *key:{"M","K","C"})require(baseline.exported[key]==e.exported[key],"Off/on source operator mismatch");baseline.run(&uncontrolled);
        require(uncontrolled.samples.size()==trace.samples.size()&&uncontrolled.samples[0].x==trace.samples[0].x,"Off/on initial state or grid mismatch");for(size_t i=0;i<trace.samples.size();++i)require(uncontrolled.samples[i].time==trace.samples[i].time&&uncontrolled.samples[i].force==trace.samples[i].force,"Off/on force endpoint mismatch");
        ratio=rmsEnergy(e.exported,trace)/rmsEnergy(baseline.exported,uncontrolled);metrics["FEClosedLoopOffOnEnergyRMS"]=ratio;metrics["actualNativeFEOff"]=trajectoryMetrics(baseline.exported,uncontrolled,setup.presetMetadata["replayTruncation"].toArray());
        qInfo().noquote()<<"PC10 FE MEASURED"<<QTest::currentDataTag()<<"preset"<<json(setup.presetMetadata)<<"metrics"<<json(metrics);writeEvidence(e,trace,setup,metrics,ratio<=1.02&&replayQualified(error)?"FE_MEASURED_ROM_CLOSED_LOOP_PENDING":"FAILED_FE_QUALIFICATION",&uncontrolled);
        romOn=independentROMClosedLoop(e,trace,true);romOff=independentROMClosedLoop(e,trace,false);romRatio=rmsEnergy(e.exported,romOn)/rmsEnergy(e.exported,romOff);metrics["independentROMClosedLoopOffOnEnergyRMS"]=romRatio;metrics["independentROMOn"]=trajectoryMetrics(e.exported,romOn,setup.presetMetadata["replayTruncation"].toArray());metrics["independentROMOff"]=trajectoryMetrics(e.exported,romOff,setup.presetMetadata["replayTruncation"].toArray());
    }
    metrics["actualMPCKKTMaximum"]=trace.kktPeak;if(kind=="MPC")metrics["actualMPCKKTFrames"]=trace.mpcKKTFrames;if(kind!="none")metrics["feedbackProposalDeltaPeakV"]=feedbackProposalDeltaPeakV;const bool qualified=replayQualified(error)&&ratio<=1.02&&romRatio<=1.02&&(kind=="none"||(trace.voltagePeak>1e-12&&romOn.voltagePeak>1e-12))&&(kind!="MPC"||trace.kktPeak<1e-6);
    writeEvidence(e,trace,setup,metrics,qualified?"QUALIFIED":"FAILED",uncontrolled.samples.empty()?nullptr:&uncontrolled,romOn.samples.empty()?nullptr:&romOn,romOff.samples.empty()?nullptr:&romOff);
    qInfo().noquote()<<"PC10"<<QTest::currentDataTag()<<"recipeSha256"<<sha(json(setup.recipe))<<"source"<<e.exported["sourceFingerprint"].toString()<<"exportSha256"<<sha(json(e.exported))<<"backend"<<e.exported["backendIdentity"].toString()<<"privateRuntime"<<json(runtime)<<"modalResiduals"<<QJsonDocument(design["modalResiduals"].toArray()).toJson(QJsonDocument::Compact)<<"massOrthogonality"<<design["massOrthogonalityError"].toDouble()<<"omega"<<QJsonDocument(setup.fullExport["design"].toObject()["omega"].toArray()).toJson(QJsonDocument::Compact)<<"clusters"<<QJsonDocument(setup.fullExport["design"].toObject()["clusters"].toArray()).toJson(QJsonDocument::Compact)<<"controllerCertificate"<<json(controller)<<"dt"<<setup.recipe["dt"].toDouble()<<"steps"<<trace.samples.size()-1<<"nativeError"<<error.native1/error.peak<<"refinement2"<<error.native2/std::max(error.native1,1e-30)<<"refinement4"<<error.native4/std::max(error.native2,1e-30)<<"RK4error"<<error.rk4/error.peak<<"fullBasisError"<<error.fullBasis/error.peak<<"truncatedError"<<error.truncated/error.peak<<"resolvedDefectBound"<<error.defectBound/error.peak<<"FEclosedLoopOffOnEnergyRMS"<<ratio<<"independentROMClosedLoopOffOnEnergyRMS"<<romRatio<<"appliedVoltagePeak"<<trace.voltagePeak<<"actualKKT"<<trace.kktPeak;
    require(replayQualified(error),"Independent FE/ROM numerical qualification failed; inspect saved metrics");require(ratio<=1.02,"Actual FE closed-loop energy RMS regresses; inspect saved ratio/peak/spillover");require(romRatio<=1.02,"Independent ROM closed-loop energy RMS regresses; inspect saved metrics");require(kind=="none"||(trace.voltagePeak>1e-12&&romOn.voltagePeak>1e-12),"Vacuous actual control action");require(kind!="MPC"||trace.kktPeak<1e-6,"Actual MPC KKT residual failed");
    }catch(const RError &error){const auto message=error.getMessage();try{fail(message);}catch(const std::exception &write){qWarning()<<"Failure evidence write failed:"<<write.what();}QFAIL(qPrintable(message));}catch(const std::exception &error){const auto message=QString::fromUtf8(error.what());try{fail(message);}catch(const std::exception &write){qWarning()<<"Failure evidence write failed:"<<write.what();}QFAIL(qPrintable(message));}
}
void TestPiezoDeliveryExamples::layeredModalClusterFromActualFE() try {
    auto fullRecipe=recipe("planarBeam");
    const auto preview=RPiezoExperimentService::preview(RModel{},fullRecipe);
    require(preview.valid,qPrintable(preview.message));
    RModel source=preview.candidate;
    const uint originalNodes=source.getNNodes(),originalElements=source.getNElements();
    require(originalElements==4&&source.getNLines()==1,"Expected the supported four-element production BEAM1 example");

    // Add a geometrically separate but exactly matching second cantilever. Its
    // nodes and elements are distinct FE entities; the bridge assembles both
    // components from the persisted layered source declaration.
    std::vector<uint> twinElementIds;
    std::map<uint,uint> twinNodePhysicalByOriginal;
    for(uint i=0;i<originalNodes;++i){const auto &node=source.getNode(i);source.addNode(RNode(node.getX(),node.getY(),node.getZ()+.2));const uint physical=source.getNodePhysicalId(source.getNNodes()-1);twinNodePhysicalByOriginal.emplace(source.getNodePhysicalId(i),physical);}
    for(uint i=0;i<originalElements;++i){RElement element=source.getElement(i);for(uint j=0;j<element.size();++j)element.setNodeId(j,element.getNodeId(j)+originalNodes);source.addElement(element,true,0);twinElementIds.push_back(source.getElementPhysicalId(source.getNElements()-1));}

    auto layered=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    const auto originalMembers=layered.members;
    const auto originalPorts=layered.ports;
    const auto originalSupports=layered.supports;
    require(originalMembers.size()==originalElements,"Layered source must declare every original BEAM1 host");
    std::map<QString,QString> twinPortIds;
    for(const auto &port:originalPorts){auto twin=port;const QString original=port.readout.portId;twin.readout.portId=original+"_twin";twinPortIds.emplace(original,twin.readout.portId);layered.ports.push_back(twin);}
    for(size_t i=0;i<originalMembers.size();++i){auto twin=originalMembers[i];twin.physicalElementId=twinElementIds.at(i);for(auto &layer:twin.layers)if(!layer.portId.isEmpty()){const auto mapped=twinPortIds.find(layer.portId);require(mapped!=twinPortIds.end(),"Twin FE layer has no independent electrical port mapping");layer.portId=mapped->second;}layered.members.push_back(twin);}
    for(const auto &support:originalSupports){auto twin=support;const auto mapped=twinNodePhysicalByOriginal.find(support.physicalNodeId);require(mapped!=twinNodePhysicalByOriginal.end(),"Twin support has no matching physical node");twin.physicalNodeId=mapped->second;layered.supports.push_back(twin);}
    layered.bindToModel(source);
    source.getProblemSetup().getStructuralPiezoLayeredSetup()=layered;
    require(source.getProblemSetup().getStructuralPiezoLayeredSetup().members.size()==2*originalMembers.size(),"Twin BEAM1 component was not included in the layered FE model");

    Experiment full(fullRecipe,&source);
    const auto sourceFingerprint=source.getProblemSetup().getStructuralPiezoLayeredSetup().computeSourceFingerprint(source);
    require(full.exported["sourceFingerprint"].toString()==sourceFingerprint,"Full modal export sourceFingerprint differs from its source model");
    const Matrix M=matrix(full.exported["M"]),K=matrix(full.exported["K"]),Phi=matrix(full.exported["design"].toObject()["Phi"]);
    const V omega=numbers(full.exported["design"].toObject()["omega"]);
    require(!M.empty()&&M.size()==K.size()&&Phi.size()==M.size()&&omega.size()==Phi[0].size(),"Actual FE reduced modal export dimensions mismatch");

    // Recompute the certificates from the exported actual reduced operators;
    // this check is independent of the provider's residual/orthogonality fields.
    V flatM,flatK;for(const auto &row:M)flatM.insert(flatM.end(),row.begin(),row.end());for(const auto &row:K)flatK.insert(flatK.end(),row.begin(),row.end());
    const double massScale=norm(flatM),stiffnessScale=norm(flatK);
    auto independentModalMetrics=[&](const Matrix &basis,const V &frequencies){
        require(!basis.empty()&&basis.size()==M.size()&&!frequencies.empty()&&basis.front().size()==frequencies.size(),"Independent modal certificate dimensions mismatch");
        for(const auto &row:basis)require(row.size()==frequencies.size(),"Independent modal basis rows have inconsistent dimensions");
        double maxResidual=0,maxMassOrthogonality=0;
        for(size_t j=0;j<frequencies.size();++j){V phi(basis.size());for(size_t i=0;i<basis.size();++i)phi[i]=basis[i][j];const double lambda=frequencies[j]*frequencies[j];const auto residual=plus(mv(K,phi),mv(M,phi),-lambda);const double denominator=(stiffnessScale+lambda*massScale)*norm(phi);maxResidual=std::max(maxResidual,norm(residual)/std::max(denominator,std::numeric_limits<double>::min()));}
        std::vector<V> mPhi(frequencies.size());for(size_t j=0;j<frequencies.size();++j){V phi(basis.size());for(size_t i=0;i<basis.size();++i)phi[i]=basis[i][j];mPhi[j]=mv(M,phi);}
        for(size_t i=0;i<frequencies.size();++i){double rowSum=0;for(size_t j=0;j<frequencies.size();++j){double value=0;for(size_t k=0;k<basis.size();++k)value+=basis[k][i]*mPhi[j][k];rowSum+=std::abs(value-(i==j?1.:0.));}maxMassOrthogonality=std::max(maxMassOrthogonality,rowSum);}
        return QJsonObject{{"maximumGeneralizedResidual",maxResidual},{"massOrthogonalityInfinityNorm",maxMassOrthogonality}};
    };
    const auto fullMetrics=independentModalMetrics(Phi,omega);
    const double maxResidual=fullMetrics["maximumGeneralizedResidual"].toDouble();
    const double maxMassOrthogonality=fullMetrics["massOrthogonalityInfinityNorm"].toDouble();
    require(maxResidual<1e-8,"Independent actual-FE generalized modal residual failed");
    require(maxMassOrthogonality<1e-8,"Independent actual-FE Phi^T M Phi certificate failed");

    QJsonArray cluster;
    for(const auto &value:full.exported["design"].toObject()["clusters"].toArray())if(value.toArray().size()>=2){cluster=value.toArray();break;}
    require(cluster.size()>=2,"Two identical supported BEAM1 components did not produce a certified multi-mode near-root cluster");
    const size_t first=static_cast<size_t>(cluster[0].toInt()),second=static_cast<size_t>(cluster[1].toInt());
    require(second==first+1&&first<omega.size()&&second<omega.size(),"Near-root cluster indices are not adjacent valid full-basis modes");
    const double lambda0=omega[first]*omega[first],lambda1=omega[second]*omega[second];
    require(std::abs(lambda1-lambda0)<=1e-7*std::max(std::abs(lambda0),std::abs(lambda1)),"Confirmed cluster roots are not near-degenerate at the provider threshold");

    auto selectedRecipe=fullRecipe;auto selectedDesign=selectedRecipe["design"].toObject();selectedDesign["modeIndices"]=cluster;selectedRecipe["design"]=selectedDesign;
    RModel selectedSource=source;auto selectedSetup=selectedSource.getProblemSetup().getStructuralPiezoLayeredSetup();selectedSetup.designConfiguration=QJsonDocument(selectedDesign).toJson(QJsonDocument::Compact);selectedSetup.bindToModel(selectedSource);selectedSource.getProblemSetup().getStructuralPiezoLayeredSetup()=selectedSetup;
    Experiment selected(selectedRecipe,&selectedSource);
    const QString selectedFingerprint=selectedSource.getProblemSetup().getStructuralPiezoLayeredSetup().computeSourceFingerprint(selectedSource);
    require(selected.exported["sourceFingerprint"].toString()==selectedFingerprint,"Whole-cluster export sourceFingerprint differs from its source model");
    require(selectedFingerprint!=sourceFingerprint,"Selection-specific source authority was not represented in sourceFingerprint");
    const auto selectedDesignExport=selected.exported["design"].toObject();
    const QJsonArray selectedModes=selectedDesignExport["selectedModes"].toArray();
    require(selectedModes==cluster,"Provider selectedModes do not exactly match the requested whole cluster");
    const Matrix selectedPhi=matrix(selectedDesignExport["Phi"]);
    const V selectedOmega=numbers(selectedDesignExport["omega"]);
    require(selectedPhi.size()==M.size()&&!selectedPhi.empty()&&selectedPhi.front().size()==cluster.size()&&selectedOmega.size()==cluster.size(),"Whole-cluster selected Phi/omega dimensions do not match requested modes");
    const auto selectedMetrics=independentModalMetrics(selectedPhi,selectedOmega);
    const double selectedMaxResidual=selectedMetrics["maximumGeneralizedResidual"].toDouble();
    const double selectedMassOrthogonality=selectedMetrics["massOrthogonalityInfinityNorm"].toDouble();
    require(selectedMaxResidual<1e-8,"Independent selected-basis actual-FE generalized modal residual failed");
    require(selectedMassOrthogonality<1e-8,"Independent selected-basis actual-FE Phi^T M Phi certificate failed");
    require(full.exported["backend"].toObject()["sourceFingerprint"].toString()==sourceFingerprint&&selected.exported["backend"].toObject()["sourceFingerprint"].toString()==selectedFingerprint,"Private provider identity differs from its bound source model");
    require(bitwiseEqual(M,matrix(selected.exported["M"]))&&bitwiseEqual(K,matrix(selected.exported["K"])),"Whole-cluster selection changed actual reduced FE M/K bits");
    require(full.exported["dofs"]==selected.exported["dofs"]&&full.exported["nodes"]==selected.exported["nodes"],"Whole-cluster selection changed stable FE DOF/node identity mapping");
    require(bitwiseEqual(matrix(full.exported["T"]),matrix(selected.exported["T"]))&&bitwiseEqual(matrix(full.exported["P"]),matrix(selected.exported["P"])),"Whole-cluster selection changed the physical-to-reduced FE map");
    require(!full.exported["backendIdentity"].toString().isEmpty()&&!selected.exported["backendIdentity"].toString().isEmpty(),"Bridge backend identity missing for full or selected export");
    require(full.exported["backend"].isObject()&&selected.exported["backend"].isObject(),"Private provider identity missing for full or selected export");
    require(full.exported["privateRuntime"]==selected.exported["privateRuntime"],"Full and selected exports did not use the same pinned private runtime");

    QJsonArray split;for(int i=0;i<cluster.size()-1;++i)split.append(cluster[i]);auto splitRecipe=fullRecipe;auto splitDesign=splitRecipe["design"].toObject();splitDesign["modeIndices"]=split;splitRecipe["design"]=splitDesign;
    RModel splitSource=source;auto splitSetup=splitSource.getProblemSetup().getStructuralPiezoLayeredSetup();splitSetup.designConfiguration=QJsonDocument(splitDesign).toJson(QJsonDocument::Compact);splitSetup.bindToModel(splitSource);splitSource.getProblemSetup().getStructuralPiezoLayeredSetup()=splitSetup;
    QString splitProviderError;
    try {Experiment rejected(splitRecipe,&splitSource);Q_UNUSED(rejected);}
    catch(const RError &error){splitProviderError=error.getMessage();}
    catch(const std::exception &error){splitProviderError=QString::fromUtf8(error.what());}
    require(splitProviderError.contains("selection splits a near-root cluster"),qPrintable("Single-member modal-cluster split did not return the expected provider error: "+splitProviderError));

    QJsonArray selectedOmegaJson;for(double value:selectedOmega)selectedOmegaJson.append(value);
    qInfo().noquote()<<"PC04-A01 REAL-FE MODAL CLUSTER"<<QJsonDocument(QJsonObject{
        {"sourceFingerprintFull",sourceFingerprint},{"sourceFingerprintWholeCluster",selectedFingerprint},
        {"backendIdentityFull",full.exported["backendIdentity"]},{"backendIdentityWholeCluster",selected.exported["backendIdentity"]},
        {"providerIdentityFull",full.exported["backend"]},{"providerIdentityWholeCluster",selected.exported["backend"]},
        {"privateRuntimeFull",full.exported["privateRuntime"]},{"privateRuntimeWholeCluster",selected.exported["privateRuntime"]},
        {"physicalElements",int(source.getNElements())},{"cluster",cluster},{"clusterOmega",QJsonArray{omega[first],omega[second]}},
        {"selectedModes",selectedModes},{"selectedPhiRows",int(selectedPhi.size())},{"selectedPhiColumns",int(selectedPhi.front().size())},{"selectedOmega",selectedOmegaJson},
        {"independentMaximumGeneralizedResidual",maxResidual},{"independentMassOrthogonalityInfinityNorm",maxMassOrthogonality},
        {"independentSelectedMaximumGeneralizedResidual",selectedMaxResidual},{"independentSelectedMassOrthogonalityInfinityNorm",selectedMassOrthogonality},
        {"actualReducedMAndKBitwiseEqual",true},{"physicalMapBitwiseEqual",true},
        {"splitProviderError",splitProviderError},{"qualificationStatus","PC04-A01 MODAL CLUSTER TEST PASS; STAGE PENDING"}
    }).toJson(QJsonDocument::Compact);
} REPORT_EXCEPTIONS
void TestPiezoDeliveryExamples::layeredExternalTimeFunctionEndpointLedger() try {
    // PC-06 acceptance is bounded to one real planar BEAM1, one private time-only
    // source f(t)=a+b*t, eight canonical Newmark intervals, and a fixed absolute
    // actuator voltage. Independently checked identities are Fphys(t)=f(t)e_tipZ,
    // Fred=T^T P^T Fphys, Rphys=P(M*a+C*v+K^E*q+G*V-Fbg-P^T Fphys),
    // dWext=(Fbg+P^T(Fleft+Fright)/2)^T*dq, dWelec=sum(Va*dQact), and
    // dWdamp=h*vmean^T*Cfull*vmean, Ekin=.5*v^T*M*v, and
    // Estored=.5*q^T*K^E*q+.5*V^T*Ce*V. This does not qualify other waveform
    // classes, general time-grid accuracy beyond this linear-force refinement,
    // multi-channel functions, control updates, full restart, or nonlinear FE.
    constexpr uint steps=8;
    constexpr double dt=1e-5,forceOffset=.075,forceSlope=1800.,heldVoltage=1.25;
    auto r=recipe("planarBeam");r["dt"]=dt;r["steps"]=int(steps);
    auto ports=r["ports"].toArray();bool driveFound=false;
    for(int i=0;i<ports.size();++i){auto p=ports[i].toObject();if(p["portId"].toString()=="drive"){p["initialValue"]=heldVoltage;ports[i]=p;driveFound=true;}}
    require(driveFound,"Layered linear-force fixture is missing its drive port");r["ports"]=ports;
    const auto tip=r["design"].toObject()["outputs"].toArray().first().toObject();
    const QString channelId=QString("node:%1:Z").arg(tip["physicalNodeId"].toInt());

    QJsonObject privateRuntime;
    {Experiment probe(r);privateRuntime=probe.exported["privateRuntime"].toObject();}
    for(const char *key:{"python","numpy","scipy"})require(privateRuntime.value(key).isString()&&!privateRuntime.value(key).toString().isEmpty(),"Private provider runtime identity is incomplete");
    QTemporaryDir dir;require(dir.isValid(),"Cannot create external-force fixture directory");
    const QString sourcePath=dir.filePath("linear_time_force.py");
    const QByteArray sourceBytes=QString("def force(t):\n    return {'%1': {'value': %2 + %3 * t, 'unit': 'N'}}\n")
        .arg(channelId).arg(forceOffset,0,'g',17).arg(forceSlope,0,'g',17).toUtf8();
    auto writeSource=[&](const QByteArray &bytes){QFile file(sourcePath);require(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Cannot write external-force source");require(file.write(bytes)==bytes.size(),"External-force source write was incomplete");file.close();};
    writeSource(sourceBytes);
    const QJsonObject external{{"sourcePath",sourcePath},{"functionName","force"},{"sourceSha256",sha(sourceBytes)},
        {"dependencyFiles",QJsonArray{}},{"environment",QJsonObject{{"pythonVersion",privateRuntime["python"]},
            {"numpyVersion",privateRuntime["numpy"]},{"scipyVersion",privateRuntime["scipy"]}}},
        {"channels",QJsonArray{QJsonObject{{"channelId",channelId},{"unit","N"}}}},
        {"missingPolicy","error"},{"cacheExactTime",false}};
    r["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{external}}};

    Experiment e(r);auto &run=*e.layered;
    require(!e.axial&&e.layered&&e.model.getNElements()==4,"Expected the real four-element planar BEAM1 source");
    for(uint i=0;i<e.model.getNElements();++i)require(e.model.getElement(i).getType()==R_ELEMENT_BEAM1,"External-force fixture contains a non-BEAM1 element");
    require(e.exported["backendIdentity"].toString().startsWith("range-fea/native-python-layered/v1:")
        && e.exported["backend"].isObject(),"Actual layered private-provider bridge identity is missing");
    const auto backend=e.exported["backend"].toObject();
    require(!backend["excitationSourceHash"].toString().isEmpty()&&!backend["excitationLoadedCodeHash"].toString().isEmpty(),"Private provider did not bind the external excitation source");
    require(e.exported["disturbanceInputs"].toArray().size()==1,"Expected exactly one physical external-force input");

    const auto P=matrix(e.exported["P"]),T=matrix(e.exported["T"]),mass=matrix(e.exported["fullM"]),
        stiffness=matrix(e.exported["fullKE"]),damping=matrix(e.exported["fullC"]),coupling=matrix(e.exported["G"]),capacitance=matrix(e.exported["Ce"]),
        B=matrix(e.exported["B"]);
    const V q0=numbers(e.exported["q0"]),background=numbers(e.exported["backgroundForce"]),
        backgroundOffset=numbers(e.exported["backgroundReducedOffset"]);
    V actuatorV0;
    for(const auto &value:e.exported["inputs"].toArray()){
        const QString id=value.toArray()[0].toString();bool found=false;
        for(const auto &portValue:e.recipe["ports"].toArray()){const auto port=portValue.toObject();if(port["portId"].toString()==id){actuatorV0.push_back(port["initialValue"].toDouble());found=true;break;}}
        require(found,"Frozen recipe is missing an exported actuator port");
    }
    require(!P.empty()&&P[0].size()==q0.size()&&T.size()==q0.size()&&!T.empty()&&T[0].size()>0,
        "Actual layered generalized/physical maps have invalid dimensions");
    const size_t n=q0.size(),physicalSize=P.size(),portCount=capacitance.size();
    const auto &assembledPortIds=run.adapter().mesh().operators.portIds;
    std::set<QString> declaredPortIds;
    for(const auto &value:e.exported["ports"].toArray()){
        const QString id=value.toObject()["portId"].toString();
        require(!id.isEmpty()&&declaredPortIds.insert(id).second,"Frozen source has an invalid or duplicate electrical port identity");
    }
    require(declaredPortIds.size()==portCount&&assembledPortIds.size()==portCount,
        "Frozen source electrical port count differs from the actual operator");
    require(mass.size()==n&&stiffness.size()==n&&damping.size()==n&&coupling.size()==n&&background.size()==n,
        "Actual layered full operators have inconsistent generalized dimensions");
    require(portCount>0&&capacitance[0].size()==portCount&&coupling[0].size()==portCount&&!B.empty()&&actuatorV0.size()==B[0].size(),
        "Actual layered electrical/input operator dimensions mismatch");
    int physicalIndex=-1;
    for(const auto &value:e.exported["forceCatalog"].toArray()){const auto c=value.toObject();if(c["channelId"].toString()==channelId){
        require(c["unit"].toString()=="N"&&c["coordinateSystem"].toString()=="global","External-force catalog channel has wrong physical unit/frame");
        physicalIndex=c["physicalIndex"].toInt();}}
    require(physicalIndex>=0&&size_t(physicalIndex)<physicalSize,"Tip force is absent from the actual physical force catalog");

    auto closeScalar=[](double actual,double expected,double relative,double absolute,const char *message){
        require(std::isfinite(actual)&&std::isfinite(expected),message);
        require(std::abs(actual-expected)<=absolute+relative*std::max(std::abs(actual),std::abs(expected)),message);
    };
    auto closeVector=[&](const V &actual,const V &expected,double relative,double absolute,const char *message){
        require(actual.size()==expected.size(),message);
        require(norm(plus(actual,expected,-1))<=absolute+relative*std::max(norm(actual),norm(expected)),message);
    };
    auto dotProduct=[&](const V &a,const V &b){require(a.size()==b.size(),"Independent ledger dot-product dimensions mismatch");double result=0.;for(size_t i=0;i<a.size();++i)result+=a[i]*b[i];return result;};
    auto forceAt=[&](double time){V physical(physicalSize);physical[size_t(physicalIndex)]=forceOffset+forceSlope*time;return physical;};
    auto fullForceAt=[&](double time){return tmv(P,forceAt(time));};
    auto reducedForceAt=[&](double time){return tmv(T,fullForceAt(time));};
    auto fullCoordinate=[&](const RStructuralPiezoLayeredDynamicsState &s){return plus(mv(T,V(s.integrator.state.q.begin(),s.integrator.state.q.end())),q0);};
    auto fullVelocity=[&](const RStructuralPiezoLayeredDynamicsState &s){return mv(T,V(s.integrator.state.v.begin(),s.integrator.state.v.end()));};
    auto fullAcceleration=[&](const RStructuralPiezoLayeredDynamicsState &s){return mv(T,V(s.integrator.state.a.begin(),s.integrator.state.a.end()));};
    auto portVoltages=[&](const RStructuralPiezoLayeredDynamicsState &s){V result;for(const auto &p:s.electrical.ports)result.push_back(p.voltage);return result;};
    auto chargesFromOperators=[&](const RStructuralPiezoLayeredDynamicsState &s){
        const V q=fullCoordinate(s),v=portVoltages(s);return plus(mv(capacitance,v),tmv(coupling,q),-1);
    };
    auto kineticEnergyFromOperators=[&](const RStructuralPiezoLayeredDynamicsState &s){const V v=fullVelocity(s);return .5*dotProduct(v,mv(mass,v));};
    auto storedEnergyFromOperators=[&](const RStructuralPiezoLayeredDynamicsState &s){
        const V q=fullCoordinate(s),voltage=portVoltages(s);
        return .5*dotProduct(q,mv(stiffness,q))+.5*dotProduct(voltage,mv(capacitance,voltage));
    };
    double independentlyReconstructedInitialEnergy=std::numeric_limits<double>::quiet_NaN();
    auto checkBoundary=[&](const RStructuralPiezoLayeredDynamicsState &s,double time){
        const V physicalForce=forceAt(time),projected=fullForceAt(time),reducedForce=reducedForceAt(time);
        closeScalar(s.integrator.state.time,time,0.,0.,"Accepted endpoint time is not the canonical force endpoint");
        closeVector(s.perturbationForces,physicalForce,1e-12,1e-13,"Private-provider endpoint force differs from analytic f(t)=a+b*t");
        const V q=fullCoordinate(s),v=fullVelocity(s),a=fullAcceleration(s);
        closeScalar(s.kineticEnergy,kineticEnergyFromOperators(s),3e-8,1e-13,"Native kinetic energy differs from independent 0.5*v^T*M*v");
        closeScalar(s.storedEnergy,storedEnergyFromOperators(s),3e-8,1e-13,"Native stored energy differs from independent 0.5*q^T*K^E*q+0.5*V^T*Ce*V");
        closeScalar(s.initialEnergy,independentlyReconstructedInitialEnergy,3e-8,1e-13,"Native initial energy differs from independently reconstructed initial kinetic plus stored energy");
        closeVector(s.physicalDisplacements,mv(P,q),2e-9,1e-12,"Physical displacement does not follow exported P*T projection");
        closeVector(s.physicalVelocities,mv(P,v),2e-9,1e-12,"Physical velocity does not follow exported P*T projection");
        closeVector(s.physicalAccelerations,mv(P,a),2e-8,1e-10,"Physical acceleration does not follow exported P*T projection");
        V voltage=portVoltages(s);require(voltage.size()==portCount&&s.electrical.ports.size()==portCount,"Actual electrical port order/dimensions changed");
        std::set<QString> seenPortIds;
        for(size_t i=0;i<portCount;++i){
            const QString &id=s.electrical.ports[i].portId;
            require(declaredPortIds.count(id)&&seenPortIds.insert(id).second,
                "Actual electrical port identities differ from the frozen source map");
            require(id==assembledPortIds[i],"Actual electrical port row differs from the assembled G/Ce column identity");
        }
        for(int j=0;j<e.exported["inputs"].toArray().size();++j){
            const QString id=e.exported["inputs"].toArray()[j].toArray()[0].toString();size_t index=portCount;
            for(size_t k=0;k<portCount;++k)if(s.electrical.ports[k].portId==id)index=k;
            require(index<portCount,"Held actuator is absent from actual electrical ports");
            closeScalar(s.heldActuatorVoltages[size_t(j)],heldVoltage,0.,0.,"Layered held actuator voltage changed during the force-only run");
            closeScalar(voltage[index],heldVoltage,1e-13,1e-14,"Electrical actuator boundary is not the fixed absolute voltage");
        }
        const V charge=chargesFromOperators(s);
        require(charge.size()==portCount,"Independent electrical charge formula has the wrong dimensions");
        for(size_t i=0;i<portCount;++i)closeScalar(s.electrical.ports[i].charge,charge[i],3e-7,1e-13,"Actual electrical charge differs from Ce*V-G^T*q");
        const V expectedRhs=plus(plus(backgroundOffset,reducedForce),mv(B,plus(s.heldActuatorVoltages,actuatorV0,-1)));
        closeVector(V(s.integrator.state.externalForce.begin(),s.integrator.state.externalForce.end()),expectedRhs,3e-8,1e-12,
            "Accepted Newmark RHS differs from T^T*P^T*f(t) plus frozen source/input terms");

        const V internal=plus(mv(stiffness,q),mv(coupling,voltage));
        closeVector(s.electrical.internalForce,internal,2e-8,1e-11,"Electrical internal force differs from exported K^E*q+G*V");
        V reaction=plus(plus(mv(mass,a),mv(damping,v)),internal);
        reaction=plus(plus(reaction,background,-1),projected,-1);
        closeVector(s.physicalReactions,mv(P,reaction),3e-7,1e-10,
            "Physical support reactions differ from independently reconstructed M*a+C*v+K^E*q+G*V-backgroundForce-P^T*f(t)");
    };

    const auto &initial=run.acceptedState();const V held=initial.heldActuatorVoltages;
    Trace acceptedTrace;acceptedTrace.samples.push_back(e.current());
    require(held.size()==1&&actuatorV0.size()==1,"Fixture must contain exactly one held voltage actuator");
    independentlyReconstructedInitialEnergy=kineticEnergyFromOperators(initial)+storedEnergyFromOperators(initial);
    closeScalar(held[0],heldVoltage,0.,0.,"Initial absolute actuator voltage differs from the fixture");
    double expectedExternalWork=0.,expectedElectricalWork=0.,expectedDamping=0.;bool mutationRejected=false;
    const double initialTime=run.acceptedRuntime().initialTime;
    checkBoundary(initial,initialTime);
    for(uint step=1;step<=steps;++step){
        const auto &left=run.acceptedState();const double leftTime=initialTime+double(step-1)*dt,rightTime=initialTime+double(step)*dt;
        checkBoundary(left,leftTime);
        const V qLeft=fullCoordinate(left),vLeft=fullVelocity(left),qLeftCharge=chargesFromOperators(left);
        std::unique_ptr<LayerRun::PreparedStep> prepared;
        if(step==4){
            const QByteArray acceptedBefore=RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime());
            auto uninterruptedReference=run.prepare(e.model);
            const QByteArray uninterruptedCandidateBytes=uninterruptedReference->candidateModel().getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes;
            require(!uninterruptedCandidateBytes.isEmpty()&&uninterruptedCandidateBytes==RStructuralPiezoLayeredRestartCodec::encode(uninterruptedReference->runtime()),
                "Uninterrupted boundary reference did not expose complete candidate restart bytes");
            require(run.completedSteps()==step-1&&RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime())==acceptedBefore,
                "Uninterrupted candidate reference advanced the native accepted FE state");
            uninterruptedReference.reset();
            writeSource(sourceBytes+QByteArray("\n# source changed during a pending FE boundary\n"));
            QString rejection;
            try {auto rejected=run.prepare(e.model);Q_UNUSED(rejected);}
            catch(const RError &error){rejection=error.getMessage();}
            catch(const std::exception &error){rejection=QString::fromUtf8(error.what());}
            require(rejection.contains("external source/dependency SHA256 mismatch; recompile required"),
                "Mutated source was not rejected for its external source SHA256 mismatch");
            require(run.completedSteps()==step-1&&RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime())==acceptedBefore,
                "Rejected source mutation advanced the native accepted FE state");
            writeSource(sourceBytes);mutationRejected=true;
            prepared=run.prepare(e.model);
            const QByteArray retriedCandidateBytes=prepared->candidateModel().getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes;
            require(retriedCandidateBytes==uninterruptedCandidateBytes&&retriedCandidateBytes==RStructuralPiezoLayeredRestartCodec::encode(prepared->runtime()),
                "Restored-source same-step retry differs from the uninterrupted complete candidate restart bytes");
        }else{
            prepared=run.prepare(e.model);
        }
        run.validatePrepared(*prepared);
        const auto candidate=prepared->result();checkBoundary(candidate,rightTime);
        const V qRight=fullCoordinate(candidate),vRight=fullVelocity(candidate),qRightCharge=chargesFromOperators(candidate);
        const V dq=plus(qRight,qLeft,-1),meanForce=plus(plus(background,fullForceAt(leftTime),.5),fullForceAt(rightTime),.5);
        expectedExternalWork+=dotProduct(meanForce,dq);
        const V voltageLeft=portVoltages(left),voltageRight=portVoltages(candidate),deltaVoltage=plus(voltageRight,voltageLeft,-1);
        const V deltaCharge=plus(mv(capacitance,deltaVoltage),tmv(coupling,dq),-1);
        for(int j=0;j<e.exported["inputs"].toArray().size();++j){
            const QString id=e.exported["inputs"].toArray()[j].toArray()[0].toString();size_t index=portCount;
            for(size_t k=0;k<portCount;++k)if(candidate.electrical.ports[k].portId==id)index=k;
            require(index<portCount,"Fixed actuator disappeared from independent electrical-work mapping");
            closeScalar(qRightCharge[index]-qLeftCharge[index],deltaCharge[index],3e-7,1e-13,"Independent Ce*deltaV-G^T*deltaq charge increment mismatch");
            expectedElectricalWork+=held[size_t(j)]*deltaCharge[index];
        }
        const V meanVelocity=plus(vLeft,plus(vRight,vLeft,-1),.5);const double h=rightTime-leftTime;
        expectedDamping+=h*dotProduct(meanVelocity,mv(damping,meanVelocity));
        closeScalar(candidate.backgroundExternalWork,expectedExternalWork,3e-7,1e-16,"External work ledger differs from endpoint trapezoid force dot full-coordinate increment");
        closeScalar(candidate.electricalWork,expectedElectricalWork,3e-7,1e-15,"Electrical work ledger differs from fixed Va times independently computed actuator charge increment");
        closeScalar(candidate.viscousDissipation,expectedDamping,3e-7,1e-18,"Damping ledger differs from h*vMean^T*Cfull*vMean");
        const double independentKineticEnergy=kineticEnergyFromOperators(candidate),independentStoredEnergy=storedEnergyFromOperators(candidate);
        const double balance=independentKineticEnergy+independentStoredEnergy-independentlyReconstructedInitialEnergy-expectedExternalWork-expectedElectricalWork+expectedDamping;
        const double balanceScale=std::abs(independentKineticEnergy)+std::abs(independentStoredEnergy)+std::abs(independentlyReconstructedInitialEnergy)
            +std::abs(expectedExternalWork)+std::abs(expectedElectricalWork)+std::abs(expectedDamping);
        closeScalar(candidate.energyBalanceResidual,balance,0.,3e-7*std::max(balanceScale,1e-12),
            "Layered energy residual differs from the independently accumulated work/dissipation ledger");
        require(std::abs(balance)<=2e-5*std::max(balanceScale,1e-12),"Actual FE endpoint energy ledger does not close within the bounded solver tolerance");
        run.commit(*prepared);
        require(run.completedSteps()==step,"Validated layered endpoint was not atomically accepted");
        acceptedTrace.samples.push_back(e.current());
    }
    require(run.completedSteps()==steps&&mutationRejected,"Eight accepted FE steps and mutation rejection/retry were not completed");
    // The source is exactly linear in time, so interpolation between accepted
    // endpoints is the original f(t). A separate native driver at 1/2/4/8/32
    // of the FE step checks convergence without asking the private provider to
    // invent intermediate forces or treating the finest grid as analytic truth.
    const auto refinedReference=e.replay(acceptedTrace,32).back();
    V displacementErrors,velocityErrors;
    for(const uint subdivision:{1u,2u,4u,8u}){
        const auto sample=e.replay(acceptedTrace,subdivision).back();
        const double displacementError=norm(plus(sample.physicalQ,refinedReference.physicalQ,-1));
        const double velocityError=norm(plus(sample.physicalV,refinedReference.physicalV,-1));
        displacementErrors.push_back(displacementError);velocityErrors.push_back(velocityError);
        qInfo().noquote()<<"PC06 external-force FE step refinement"<<subdivision
            <<"displacementError"<<displacementError<<"velocityError"<<velocityError;
        if(subdivision==1){
            require(norm(plus(sample.physicalQ,acceptedTrace.samples.back().physicalQ,-1))<=1e-13 &&
                norm(plus(sample.physicalV,acceptedTrace.samples.back().physicalV,-1))<=1e-12,
                "The actual accepted force-only FE endpoint differs from replay on its original grid");
        }
    }
    require(displacementErrors.front()>1e-13&&velocityErrors.front()>1e-11,
        "External-force step-refinement fixture has a vacuous coarse-grid defect");
    for(size_t i=1;i<displacementErrors.size();++i)
        require(displacementErrors[i]<0.35*displacementErrors[i-1]&&velocityErrors[i]<0.35*velocityErrors[i-1],
            "External-force displacement or velocity did not converge under FE step refinement");
    qInfo().noquote()<<"PC06-A02/A03 LAYERED EXTERNAL ENDPOINT LEDGER"<<json(QJsonObject{
        {"sourceSha256",sha(sourceBytes)},{"sourceFunction","f(t)=0.075+1800*t N"},{"physicalChannel",channelId},
        {"privateBackendIdentity",e.exported["backendIdentity"]},{"acceptedBEAM1Elements",int(e.model.getNElements())},{"acceptedSteps",int(run.completedSteps())},
        {"fixedHeldVoltageV",heldVoltage},{"externalWorkJ",expectedExternalWork},{"electricalWorkJ",expectedElectricalWork},
        {"dampingDissipationJ",expectedDamping},{"sourceMutationRejectedWithoutAdvanceAndRetryAccepted",mutationRejected},
        {"stepRefinementDisplacementErrorsM",array(displacementErrors)},{"stepRefinementVelocityErrorsMPerS",array(velocityErrors)},
        {"independentInitialEnergyJ",independentlyReconstructedInitialEnergy},
        {"qualificationStatus","bounded PC-06 endpoint ledger PASS; stage pending"}});
} REPORT_EXCEPTIONS
void TestPiezoDeliveryExamples::savedExperimentControllerPhases_data(){
    QTest::addColumn<QString>("family");QTest::addColumn<QString>("extension");QTest::addColumn<uint>("stride");
    for(const auto &family:{QString("axialRod"),QString("planarBeam"),QString("flatPlate")})for(const auto &extension:{QString("ram"),QString("rbm")})QTest::newRow(qPrintable(family+"-"+extension))<<family<<extension<<(extension=="ram"?2u:3u);
}
void TestPiezoDeliveryExamples::savedExperimentControllerPhases() try {
    QFETCH(QString,family);QFETCH(QString,extension);QFETCH(uint,stride);auto setup=scenario(family,"MPC",true,stride);setup.recipe["steps"]=int(2*stride+1);Experiment complete(setup.recipe);const auto expected=complete.run().finalBytes;QTemporaryDir dir;QVERIFY(dir.isValid());
    QJsonArray phaseReceipts;
    auto writeReceipt=[&](const QString &status){
        const auto destination=qEnvironmentVariable("RANGE_PIEZO_DELIVERY_EVIDENCE_DIR");
        if(destination.isEmpty())return;
        require(QDir().mkpath(destination),"Cannot create explicitly requested persistence evidence directory");
        const QString caseName=family+"-"+extension;
        const QJsonObject receipt{{"schema","range.piezo-persistence-case-run/v1"},
            {"case",caseName},{"family",family},{"extension",extension},{"controlEveryFESteps",int(stride)},
            {"qualificationStatus",status},{"qualified",status=="QUALIFIED"},
            {"sourceFingerprint",complete.exported["sourceFingerprint"]},
            {"recipeSha256",sha(json(setup.recipe))},{"phaseResults",phaseReceipts}};
        QFile file(QDir(destination).filePath(caseName+".json"));
        require(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Cannot open persistence receipt");
        const auto bytes=QJsonDocument(receipt).toJson(QJsonDocument::Indented);
        require(file.write(bytes)==bytes.size(),"Incomplete persistence receipt write");
    };
    writeReceipt("IN_PROGRESS");
    // A sample boundary and every held-command phase. Existing native suites
    // retain the larger every-split and nonzero-workpoint qualification matrix.
    // Reuse the exact original source, including material UUIDs. Independent
    // template construction creates a distinct source even with equal geometry.
    for(uint phase=0;phase<stride;++phase){Experiment first(setup.recipe,&complete.model);QCOMPARE(first.exported["sourceFingerprint"],complete.exported["sourceFingerprint"]);for(uint i=0;i<stride+phase;++i)first.step();auto model=first.acceptedModel();const auto before=first.bytes();const auto recipeBefore=RPiezoExperimentService::recipeFromModel(model);QVERIFY(!recipeBefore.isEmpty());const auto path=dir.filePath(QString("phase-%1.%2").arg(phase).arg(extension));const auto nativePath=model.nativeWritePaths(path,false).first();model.write(path,false);RModel disk;disk.read(nativePath);const auto recipeAfterRead=RPiezoExperimentService::recipeFromModel(disk);QCOMPARE(recipeAfterRead,recipeBefore);disk.getProblemSetup().setRestart(true);
        QByteArray reopenedRestart,resumedFinal;
        if(family=="axialRod"){auto bridge=PiezoAxialBackendBridge::create(disk,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));AxialRun restored(disk,bridge.callbacks);reopenedRestart=RStructuralPiezoAxialRestartCodec::encode(restored.acceptedRuntime());QCOMPARE(reopenedRestart,before);while(restored.hasRemainingSteps()){auto p=restored.prepare(disk);restored.validatePrepared(*p);restored.commit(*p);}resumedFinal=RStructuralPiezoAxialRestartCodec::encode(restored.acceptedRuntime());QCOMPARE(resumedFinal,expected);}
        else{auto bridge=PiezoNativeBackendBridge::create(disk,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));LayerRun restored(disk,bridge.callbacks);reopenedRestart=RStructuralPiezoLayeredRestartCodec::encode(restored.acceptedRuntime());QCOMPARE(reopenedRestart,before);while(restored.hasRemainingSteps()){auto p=restored.prepare(disk);restored.validatePrepared(*p);restored.commit(*p);}resumedFinal=RStructuralPiezoLayeredRestartCodec::encode(restored.acceptedRuntime());QCOMPARE(resumedFinal,expected);}
        phaseReceipts.append(QJsonObject{{"phase",int(phase)},{"completedFEStepsAtSave",int(stride+phase)},
            {"savedRecipeSha256",sha(json(recipeBefore))},{"restoredRecipeSha256",sha(json(recipeAfterRead))},
            {"savedRestartSha256",sha(before)},
            {"reopenedRestartSha256",sha(reopenedRestart)},{"uninterruptedFinalRestartSha256",sha(expected)},
            {"resumedFinalRestartSha256",sha(resumedFinal)},{"restoredRecipeEqualsSaved",recipeAfterRead==recipeBefore},
            {"reopenedRestartEqualsSaved",reopenedRestart==before},{"resumedFinalEqualsUninterrupted",resumedFinal==expected}});
        writeReceipt("IN_PROGRESS");
    }
    writeReceipt("QUALIFIED");
} REPORT_EXCEPTIONS
QTEST_GUILESS_MAIN(TestPiezoDeliveryExamples)
#include "tst_piezo_delivery_examples.moc"
