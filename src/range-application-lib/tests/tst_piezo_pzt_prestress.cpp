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
#include <rstructural_piezo_axial_restart_codec.h>
#include "rpiezo_experiment_service.h"
#include "piezo_native_backend_bridge.h"
#include "piezo_axial_backend_bridge.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <map>
#include <stdexcept>
#include <limits>

// PC-10 typical PZT material / nonzero prestress numerical representative.
// Common validation helpers copied from frozen tst_piezo_delivery_examples.cpp
// SHA256 d56b3edd2747afda3fd9fd26aa1c211e13e871d1837e999e89752d3a7836473d.
// Original synthetic cases remain separate and unchanged.
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

struct Sample {double time=0;V x,physicalQ,physicalV,physicalA,y,applied,force;std::vector<RStructuralPiezoNativeReadoutSample> absoluteElectrical;};
struct Trace {std::vector<Sample> samples;QByteArray finalBytes;double voltagePeak=0,kktPeak=0;uint ticks=0,limitedFESteps=0;};

RModel pztSource();
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
            RModel source=pztSource();source.getTimeSolver().setInputStartTime(r["dt"].toDouble());
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
        Sample p;p.time=s.time;p.physicalQ=s.displacements;p.physicalV=s.velocities;p.physicalA=s.accelerations;p.applied=va;p.absoluteElectrical=readouts;
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
                for(const char *key:{"primal","stationarity","dual","complementarity","actionStationarity"}){require(kkt[key].isDouble(),"Missing KKT residual");trace.kktPeak=std::max(trace.kktPeak,std::abs(kkt[key].toDouble()));}}
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

// Independent numerical material/fixture specification. Supplier PZT-5A
// values are typical data, not a measured device or hardware qualification.
constexpr double pztEps0=8.8541878128e-12;
constexpr double pztArea=1e-6;
constexpr double pztDensity=7950.;
constexpr const char *pztPdfSha="b8c70bbc4c3dd44de306bdefa0188b7807bded155d9c4b059dc4b2f462bdbe3b";
constexpr const char *pztPdfUrl="https://info.piezo.com/hubfs/Data-Sheets/piezo-material-properties-data-sheet-20201112.pdf";
RModel pztSource(){
    RModel m;const double xy[5][2]={{-.1,0},{0,.12},{.08,.04},{.02,-.09},{0,0}};
    for(uint i=0;i<5;++i){m.addNode(RNode(xy[i][0],xy[i][1],0));m.setNodePhysicalId(i,1001+i);}
    for(uint i=0;i<4;++i){RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,i);e.setNodeId(1,4);m.addElement(e,true,0);m.setElementPhysicalId(i,2001+i);}
    m.getLine(0).setCrossArea(pztArea);return m;
}
QJsonObject pztMaterial(){
    // Supplier axes: poling axis 3. Engineering order xx yy zz yz xz xy.
    V s(36),d(18),eps(9);
    s[0]=s[7]=15.1e-12;s[14]=18.6e-12;s[1]=s[6]=-4.8e-12;
    s[2]=s[12]=s[8]=s[13]=-7.6e-12;s[21]=s[28]=40e-12;s[35]=39.8e-12;
    d[4]=460e-12;d[9]=460e-12;d[12]=d[13]=-190e-12;d[14]=390e-12;
    eps[0]=eps[4]=1600*pztEps0;eps[8]=1800*pztEps0;
    // Proper cyclic basis permutation (local x,y,z)=(supplier 3,1,2).
    // This puts poling/electrode direction along the production axial x law.
    // Engineering shear is merely permuted, with no factor-of-two conversion.
    const uint vmap[]={2,0,1,5,3,4},emap[]={2,0,1};V localS(36),localD(18),localEps(9);
    for(uint i=0;i<6;++i)for(uint j=0;j<6;++j)localS[6*i+j]=s[6*vmap[i]+vmap[j]];
    for(uint i=0;i<3;++i){for(uint j=0;j<6;++j)localD[6*i+j]=d[6*emap[i]+vmap[j]];for(uint j=0;j<3;++j)localEps[3*i+j]=eps[3*emap[i]+emap[j]];}
    return {{"materialId","supplier-PZT5A"},{"form","strainCharge"},{"elasticityOrCompliance",array(localS)},{"coupling",array(localD)},{"permittivity",array(localEps)},
        {"density",pztDensity},{"piezoelectric",true},{"sourceKind","vendor"},{"sourceId",QString::fromLatin1(pztPdfUrl)+"#PZT-5A/full-sE-d-epsT"},{"sourceSha256",pztPdfSha},
        {"notes","Supplier typical PZT-5A full sE/d/epsilonT SI; rho=7950 kg/m3. Proper cyclic basis local(x,y,z)=supplier(3,1,2), poling along rod x. Production converts cE=sE^-1, e=d*cE, epsilonS=epsilonT-d*cE*d^T then free-stress reduces. No hardware qualification."}};
}
struct PztMember {uint elementId=0;QString port;double length0=0,length=0,extension=0,k=0,g=0,ce=0,mass=0,voltage=0,charge=0,force=0;V direction;bool open=false;};
struct PztPhysics {
    RModel source=pztSource();V q0=V(15),f0=V(15),internal=V(15);std::vector<PztMember> members;
    PztPhysics(){
        q0[12]=2e-7;q0[13]=-1e-7;
        for(uint i=0;i<4;++i){const auto &n=source.getNode(i);q0[3*i]=1e-4*n.getX();q0[3*i+1]=1e-4*n.getY();}
        // Independent one-stress/one-field identities after free transverse
        // stress: Y=1/s33, e33*=d33/s33, eps33*=epsT33-d33^2/s33.
        const double modulus=1./18.6e-12,coupling=390e-12/18.6e-12,epsilon=1800*pztEps0-390e-12*390e-12/18.6e-12;
        require(epsilon>0,"Published PZT reduced dielectric must be positive");
        for(uint i=0;i<4;++i){PztMember a;a.elementId=2001+i;a.port=QString("pzt_%1").arg(i);a.open=i==1;
            const auto &n=source.getNode(i);V c{-n.getX(),-n.getY(),0.},dq{q0[12]-q0[3*i],q0[13]-q0[3*i+1],0.},chord=plus(c,dq);
            a.length0=norm(c);a.length=norm(chord);a.direction=chord;for(double &x:a.direction)x/=a.length;
            // Rationalized length increment is independent of the production
            // force/tangent routines and avoids subtracting nearly equal lengths.
            a.extension=0;for(uint j=0;j<3;++j)a.extension=std::fma(c[j],dq[j],std::fma(.5*dq[j],dq[j],a.extension));a.extension/=.5*(a.length+a.length0);
            a.k=pztArea*modulus/a.length0;a.g=pztArea*coupling/a.length0;a.ce=pztArea*epsilon/a.length0;a.mass=pztDensity*pztArea*a.length0;
            a.voltage=i==0?12.:i==1?4000.:0.;a.charge=std::fma(a.ce,a.voltage,-a.g*a.extension);a.force=std::fma(a.g,a.voltage,a.k*a.extension);
            require(a.force>0,"Fixture must contain actual tensile prestress in all rods");
            for(uint j=0;j<3;++j){internal[3*i+j]-=a.force*a.direction[j];internal[12+j]+=a.force*a.direction[j];}members.push_back(a);
        }
        // Only the free center receives external preload; outer nodes react.
        for(uint j=0;j<3;++j)f0[12+j]=internal[12+j];
    }
};
QJsonObject recipe(const QString &family){
    require(family=="axialRod","PZT prestress fixture is axial only");PztPhysics physics;
    auto r=RPiezoExperimentService::exampleRecipe(physics.source,family);require(!r.isEmpty(),"Actual nonparallel TRUSS1 recipe missing");r.remove("template");r["replaceLegacyConditions"]=true;r["materials"]=QJsonArray{pztMaterial()};
    QJsonArray members,ports;for(uint i=0;i<4;++i){const auto &a=physics.members[i];members.append(QJsonObject{{"physicalElementId",double(a.elementId)},{"kind","active"},{"materialId","supplier-PZT5A"},{"area",pztArea},{"portId",a.port},{"polarity",1}});
        QJsonObject p{{"portId",a.port},{"role",i==0?"actuatorSensor":"sensor"},{"readout",i==0?"actuatorCharge":i==1?"openCircuitVoltage":i==2?"shortCircuitCharge":"idealChargeAmplifier"},{"initialValue",a.open?a.charge:a.voltage}};
        if(i==3){p["Cf"]=1e-9;p["amplifierPolarity"]=-1;p["Qref"]=a.charge;p["VoutBias"]=.2;}ports.append(p);}
    r["members"]=members;r["ports"]=ports;QJsonArray supports,initial,nodes,background,wp;
    const char *dofs[]={"UX","UY","UZ"};for(uint i=0;i<5;++i){V q(6),f(6);for(uint j=0;j<3;++j){q[j]=physics.q0[3*i+j];f[j]=physics.f0[3*i+j];if(i<4||j==2)supports.append(QJsonObject{{"physicalNodeId",double(1001+i)},{"dof",dofs[j]},{"value",q[j]}});}
        initial.append(QJsonObject{{"physicalNodeId",double(1001+i)},{"values",array(q)}});nodes.append(QJsonObject{{"physicalNodeId",double(1001+i)},{"q0",array(q)},{"f0",array(f)}});if(i==4)background.append(QJsonObject{{"physicalNodeId",double(1001+i)},{"values",array(f)}});}
    for(const auto &a:physics.members)wp.append(QJsonObject{{"portId",a.port},{"V0",a.voltage},{"Q0",a.charge}});
    r["supports"]=supports;r["initialDisplacements"]=initial;r["backgroundForces"]=background;r["workpoint"]=QJsonObject{{"available",true},{"nodes",nodes},{"ports",wp}};
    QJsonArray outputs;const char *quantities[]={"displacement","velocity","acceleration"},*units[]={"m","m/s","m/s^2"};for(uint j=0;j<3;++j)for(const char *axis:{"X","Y"})outputs.append(QJsonObject{{"id",QString(QString::fromLatin1(axis)=="X"?"tip_%1":"center_y_%1").arg(quantities[j])},{"physicalNodeId",1005},{"dof",axis},{"quantity",quantities[j]},{"unit",units[j]}});
    r["design"]=QJsonObject{{"outputs",outputs}};
    // Physical Rayleigh model at the much higher actual PZT frequencies;
    // timestep/window remains independently chosen from actual certified omega.
    r["damping"]=QJsonObject{{"kind","rayleigh"},{"rayleighAlpha",.02},{"rayleighBeta",1e-8}};r["dt"]=1e-6;r["steps"]=1;return r;
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
    if(forced){const auto output=r["design"].toObject()["outputs"].toArray()[0].toObject();r["excitation"]=QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId",QString("node:%1:%2").arg(output["physicalNodeId"].toInt()).arg(output["dof"].toString())},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","2e-4 + t"}}}}}}}};}
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
    const QJsonObject metadata{{"revision","PC10-PZT-prestress-v1"},{"controlBasis","all-certified-FE-modes"},{"replayTruncation",indices},{"targetFirstModeEnergyLQRDamping",targetDamping},{"actualFirstModalInput",modalInput},{"energyEffortWeight",effort},{"requestedDuration",duration},{"actualDuration",steps*dt},{"firstModeCycles",steps*dt*omega[0]/(2*pi)},{"maxModalAnglePerFEStep",dt*omega.back()},{"maxModalAnglePerControlSample",stride*dt*omega.back()},{"windowAdmissionMaximumSteps",12000},{"geometry",r.value("template")}};
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
double relativeVectorError(const V &actual,const V &expected){return norm(plus(actual,expected,-1))/std::max(norm(expected),1e-300);}
double matrixNorm(const Matrix &m){double n=0;for(const auto &row:m)n=std::hypot(n,norm(row));return n;}
double relativeMatrixError(const Matrix &a,const Matrix &b){require(a.size()==b.size(),"Independent matrix row count");double n=0;for(size_t i=0;i<a.size();++i)n=std::hypot(n,norm(plus(a[i],b[i],-1)));return n/std::max(matrixNorm(b),1e-300);}
QJsonObject verifyPztWorkingPoint(const Experiment &e){
    require(e.axial,"PZT representative requires actual axial NativeRun");const PztPhysics p;const auto &x=e.exported;
    require(e.recipe["materials"].toArray()==QJsonArray{pztMaterial()},"Supplier material values or axis mapping changed");
    require(e.model.getNNodes()==5&&e.rod->system().members.size()==4&&matrix(x["K"]).size()==2,"Expected actual four nonparallel rods / center XY DOFs");
    require(x["equilibriumCertified"].toBool()&&x["certificateAuthority"].toString()=="native-explicit-workpoint-certificate","Nonzero reference must be explicitly certified by native FE");
    require(norm(numbers(x["q0"]))>0&&norm(numbers(x["V0"]))>0&&norm(numbers(x["Q0"]))>0&&norm(numbers(x["f0"]))>0,"Vacuous q0/V0/Q0/f0 prestress reference");
    require(relativeVectorError(numbers(x["q0"]),p.q0)<1e-13&&relativeVectorError(numbers(x["f0"]),p.f0)<1e-11,"Independent q0/f0 reference mismatch");
    require(x["forceResidual"].toDouble()<=x["forceTolerance"].toDouble(),"Actual native free-force residual failed");
    // Independently sum each member's axial prestress force at its two nodes,
    // then compare the resulting support reactions with the native operator
    // export. This intentionally does not use exported internalForce0 as the
    // expected value, so a shared export/sign error cannot satisfy the check.
    const auto reactions=numbers(x["reactions0"]);require(reactions.size()==p.internal.size()&&p.f0.size()==p.internal.size(),"Native support-reaction dimensions");
    std::map<uint,size_t> offsetByNode;for(auto value:x["nodes"].toArray()){const auto node=value.toObject();const uint id=uint(node["physicalNodeId"].toInt());const size_t offset=size_t(node["physicalOffset"].toInt());require(offset+3<=p.internal.size()&&offsetByNode.emplace(id,offset).second,"Native physical-node map is invalid");}
    std::vector<bool> prescribed(p.internal.size(),false);QJsonArray supportReactionMetrics;uint supportDofs=0;
    for(auto value:e.recipe["supports"].toArray()){const auto support=value.toObject();const uint id=uint(support["physicalNodeId"].toInt());const auto node=offsetByNode.find(id);require(node!=offsetByNode.end(),"Support references an unknown physical node");const QString dof=support["dof"].toString();const int axis=dof=="UX"?0:dof=="UY"?1:dof=="UZ"?2:-1;require(axis>=0,"Axial fixture contains a nontranslational support");const size_t index=node->second+size_t(axis);require(!prescribed[index],"Duplicate support DOF in representative recipe");prescribed[index]=true;++supportDofs;}
    require(supportDofs==13,"Representative support layout must constrain 13 of 15 physical DOFs");
    const double forceScale=std::max({norm(p.internal),norm(p.f0),1e-12});double reactionError=0,supportReactionNorm=0;V supportBalance(3);
    for(size_t i=0;i<reactions.size();++i){const double expected=p.internal[i]-p.f0[i];const double error=std::abs(reactions[i]-expected);reactionError=std::max(reactionError,error);require(error<=1e-10*forceScale+128*std::numeric_limits<double>::epsilon()*std::max(std::abs(expected),std::abs(reactions[i])),"Native support reaction differs from independent PZT member-force assembly");
        if(prescribed[i]){supportReactionNorm=std::hypot(supportReactionNorm,reactions[i]);supportBalance[i%3]+=reactions[i];supportReactionMetrics.append(QJsonObject{{"physicalDofIndex",int(i)},{"reactionN",reactions[i]}});}else require(std::abs(reactions[i])<=1e-10*forceScale,"Unsupported free DOF carries a native reaction");
        supportBalance[i%3]+=p.f0[i];}
    require(supportReactionNorm>1e-6*forceScale,"Support reactions are physically vacuous");
    const double supportBalanceResidual=norm(supportBalance);require(supportBalanceResidual<=1e-10*forceScale,"Independent support reactions do not balance the applied workpoint force");
    Matrix km(15,V(15)),kg=km,kfixed=km,mass=km,g(15,V(4)),ce(4,V(4));V va(4),qa(4);double totalMass=0,minimumForce=1e300;
    const auto ports=x["ports"].toArray();require(ports.size()==4,"Expected independent electrode catalog");
    for(const auto &a:p.members){size_t column=4;for(int j=0;j<ports.size();++j)if(ports[j].toObject()["portId"].toString()==a.port)column=size_t(j);require(column<4,"Port order cannot be inferred from recipe order");
        const auto outer=size_t(a.elementId-2001);va[column]=a.voltage;qa[column]=a.charge;ce[column][column]=a.ce;totalMass+=a.mass;minimumForce=std::min(minimumForce,a.force);
        const auto actual=e.rod->adapter().maps().memberByPortId.at(a.port);const auto &law=std::get<RStructuralTrussPiezoLaw>(e.rod->system().members.at(actual).law);
        require(std::abs(law.axialStiffness/a.k-1)<1e-11&&std::abs(law.coupling/a.g-1)<1e-11&&std::abs(law.capacitance/a.ce-1)<1e-11,"Full-tensor production conversion differs from independent scalar free-stress identities");
        for(size_t i=0;i<3;++i){mass[3*outer+i][3*outer+i]+=.5*a.mass;mass[12+i][12+i]+=.5*a.mass;g[3*outer+i][column]=-a.g*a.direction[i];g[12+i][column]=a.g*a.direction[i];
            for(size_t j=0;j<3;++j){const double nn=a.direction[i]*a.direction[j],material=a.k*nn,geometric=a.force/a.length*((i==j?1.:0.)-nn),fixed=a.open?a.g*a.g/a.ce*nn:0.;
                for(size_t u=0;u<2;++u)for(size_t v=0;v<2;++v){const size_t row=(u?12:3*outer)+i,col=(v?12:3*outer)+j;const double sign=u==v?1.:-1.;km[row][col]+=sign*material;kg[row][col]+=sign*geometric;kfixed[row][col]+=sign*fixed;}}}
    }
    require(relativeVectorError(numbers(x["V0"]),va)<1e-11&&relativeVectorError(numbers(x["Q0"]),qa)<1e-11,"Independent V0/Q0 reference mismatch");
    require(relativeMatrixError(matrix(x["fullM"]),mass)<1e-11&&std::abs(x["totalMass"].toDouble()/totalMass-1)<1e-11,"Real PZT mass/replacement assembly mismatch");
    const double kmError=relativeMatrixError(matrix(x["fullKmaterial"]),km),kgError=relativeMatrixError(matrix(x["fullKgeometric"]),kg),fixedError=relativeMatrixError(matrix(x["fullKfixedCharge"]),kfixed);
    require(kmError<1e-10&&kgError<1e-10&&fixedError<1e-10,"Independent prestress/material/fixed-charge tangent decomposition mismatch");
    require(relativeMatrixError(matrix(x["G"]),g)<1e-11&&relativeMatrixError(matrix(x["Ce"]),ce)<1e-11,"Independent reciprocal electrical operator mismatch");
    const double kgRatio=matrixNorm(matrix(x["Kgeometric"]))/matrixNorm(matrix(x["Kmaterial"]));require(kgRatio>1e-5,"Reduced geometric stiffness must be materially nonzero");
    const auto k=matrix(x["K"]);require(k[0][0]>0&&k[1][1]>0&&k[0][0]*k[1][1]-k[0][1]*k[1][0]>0,"Two-DOF prestressed tangent must be stable");
    const auto &sys=e.rod->system();auto inputs=e.rod->adapter().initialCompleteInputs().memberInputs;const auto q0=numbers(x["q0"]);const auto fullK=matrix(x["fullK"]);
    // Central finite differences use the actual native assembly as measured
    // function values, with independently assembled analytic targets above.
    double tangentFD=0,voltageFD=0,chargeFD=0;constexpr double hq=1e-7,hv=.25;
    for(size_t axis=0;axis<2;++axis){auto left=q0,right=q0;left[12+axis]-=hq;right[12+axis]+=hq;
        const auto lo=RStructuralTrussAssembly::evaluate(sys.referenceNodes,left,sys.members,inputs),hi=RStructuralTrussAssembly::evaluate(sys.referenceNodes,right,sys.members,inputs);
        V measured=plus(hi.internalForces,lo.internalForces,-1),expected(15);for(size_t i=0;i<15;++i){measured[i]/=2*hq;expected[i]=fullK[i][12+axis];}tangentFD=std::max(tangentFD,relativeVectorError(measured,expected));}
    auto voltages=inputs;for(size_t j=0;j<4;++j){const auto id=ports[int(j)].toObject()["portId"].toString();voltages[e.rod->adapter().maps().memberByPortId.at(id)]=RStructuralTrussVoltage{va[j]};}
    for(size_t j=0;j<4;++j){const auto id=ports[int(j)].toObject()["portId"].toString();const auto member=e.rod->adapter().maps().memberByPortId.at(id);auto loIn=voltages,hiIn=voltages;loIn[member]=RStructuralTrussVoltage{va[j]-hv};hiIn[member]=RStructuralTrussVoltage{va[j]+hv};
        const auto lo=RStructuralTrussAssembly::evaluate(sys.referenceNodes,q0,sys.members,loIn),hi=RStructuralTrussAssembly::evaluate(sys.referenceNodes,q0,sys.members,hiIn);
        V measured=plus(hi.internalForces,lo.internalForces,-1),expected(15);for(size_t i=0;i<15;++i){measured[i]/=2*hv;expected[i]=g[i][j];}voltageFD=std::max(voltageFD,relativeVectorError(measured,expected));
        for(size_t axis=0;axis<2;++axis){auto ql=q0,qr=q0;ql[12+axis]-=hq;qr[12+axis]+=hq;const auto low=RStructuralTrussAssembly::evaluate(sys.referenceNodes,ql,sys.members,voltages),high=RStructuralTrussAssembly::evaluate(sys.referenceNodes,qr,sys.members,voltages);
            const double derivative=(high.memberResponses[member].electrical->conductorCharge-low.memberResponses[member].electrical->conductorCharge)/(2*hq),target=-g[12+axis][j];chargeFD=std::max(chargeFD,std::abs(derivative-target)/std::max(std::abs(target),1e-300));}}
    require(tangentFD<1e-6&&voltageFD<1e-6&&chargeFD<1e-6,"Actual native finite-difference prestress/reciprocity gate failed");
    return {{"supplierPdfSha256",pztPdfSha},{"supplierSource",pztPdfUrl},{"qualificationScope","typical-material / numerical FE only"},{"minimumTensileForceN",minimumForce},{"totalMassKg",totalMass},{"reducedKgOverKmaterial",kgRatio},{"materialTangentRelativeError",kmError},{"geometricTangentRelativeError",kgError},{"fixedChargeTangentRelativeError",fixedError},{"nativeDfdqRelativeFDerror",tangentFD},{"nativeDfdVRelativeFDerror",voltageFD},{"nativeDQdqRelativeFDerror",chargeFD},{"forceResidualN",x["forceResidual"]},{"forceToleranceN",x["forceTolerance"]},{"independentSupportReactionMaxErrorN",reactionError},{"independentSupportReactionNormN",supportReactionNorm},{"supportForceBalanceResidualN",supportBalanceResidual},{"supportReactions",supportReactionMetrics}};
}
QJsonObject verifyPztAbsoluteReadouts(const Experiment &e,const Trace &trace){
    const PztPhysics physics;require(e.axial,"Absolute PZT readouts require native axial trace");
    double maximumAbsoluteStrain=0,maximumIncrementalStrain=0,maximumElectricField=0,maximumIncrementalElectricField=0,minimumPrestressStrain=1e300;QJsonArray portMetrics;
    V voltagePeaks(4),chargePeaks(4),deltaVoltagePeaks(4),deltaChargePeaks(4),fieldPeaks(4);
    for(const auto &a:physics.members)minimumPrestressStrain=std::min(minimumPrestressStrain,a.extension/a.length0);
    for(const auto &s:trace.samples){require(s.absoluteElectrical.size()==4,"Missing captured accepted native absolute electrical samples");for(const auto &r:s.absoluteElectrical){const auto it=std::find_if(physics.members.begin(),physics.members.end(),[&](const auto &a){return a.port==r.portId;});require(it!=physics.members.end(),"Unknown absolute PZT readout");
            const size_t outer=size_t(it->elementId-2001);V dq(3),chord(3);for(size_t j=0;j<3;++j){dq[j]=(s.physicalQ[12+j]-physics.q0[12+j])-(s.physicalQ[3*outer+j]-physics.q0[3*outer+j]);chord[j]=it->length*it->direction[j];}
            const double currentLength=norm(plus(chord,dq));double dl=0;for(size_t j=0;j<3;++j)dl=std::fma(chord[j],dq[j],std::fma(.5*dq[j],dq[j],dl));dl/=.5*(currentLength+it->length);
            maximumAbsoluteStrain=std::max(maximumAbsoluteStrain,std::abs((it->extension+dl)/it->length0));maximumIncrementalStrain=std::max(maximumIncrementalStrain,std::abs(dl/it->length0));
            const double field=std::abs(r.voltage/currentLength);maximumElectricField=std::max(maximumElectricField,field);maximumIncrementalElectricField=std::max(maximumIncrementalElectricField,std::abs(r.deltaVoltage/it->length0));
            voltagePeaks[outer]=std::max(voltagePeaks[outer],std::abs(r.voltage));chargePeaks[outer]=std::max(chargePeaks[outer],std::abs(r.charge));deltaVoltagePeaks[outer]=std::max(deltaVoltagePeaks[outer],std::abs(r.deltaVoltage));deltaChargePeaks[outer]=std::max(deltaChargePeaks[outer],std::abs(r.deltaCharge));fieldPeaks[outer]=std::max(fieldPeaks[outer],field);
            const double dv=it->open?it->g*dl/it->ce:(r.portId=="pzt_0"?s.applied.at(0)-it->voltage:0.);
            const double dc=it->open?0.:std::fma(it->ce,dv,-it->g*dl);
            const double roundoff=64*std::numeric_limits<double>::epsilon();
            require(std::abs(r.deltaVoltage-dv)<=roundoff*std::max(std::abs(it->voltage),1.)+1e-8*std::abs(dv),"Independent physical voltage increment mismatch");
            require(std::abs(r.deltaCharge-dc)<=roundoff*std::max(std::abs(it->charge),1e-20)+1e-8*std::abs(dc),"Independent physical charge increment mismatch");
            require(std::abs((r.voltage-r.deltaVoltage)-it->voltage)<=1e-11*std::max(1.,std::abs(it->voltage)),"Absolute voltage reference was lost");require(std::abs((r.charge-r.deltaCharge)-it->charge)<=1e-11*std::max(std::abs(it->charge),1e-20),"Absolute charge reference was lost");
            if(r.portId=="pzt_3")require(std::abs((r.value-r.deltaValue)-.2)<1e-11,"Ideal amplifier absolute VoutBias/Qref reference was lost");}}
    require(maximumAbsoluteStrain<2e-4&&maximumIncrementalStrain/minimumPrestressStrain<.01,"Motion left the declared small-strain incremental prestress scope");
    for(size_t i=0;i<4;++i){const auto &a=physics.members[i];portMetrics.append(QJsonObject{{"portId",a.port},{"length0M",a.length0},{"referenceV0",a.voltage},{"referenceQ0C",a.charge},{"referenceStrain",a.extension/a.length0},{"peakAbsoluteVoltageV",voltagePeaks[i]},{"peakAbsoluteChargeC",chargePeaks[i]},{"peakDeltaVoltageV",deltaVoltagePeaks[i]},{"peakDeltaChargeC",deltaChargePeaks[i]},{"peakElectricFieldVPerM",fieldPeaks[i]},{"electricalBoundary",a.open?"fixed nonzero Q0; open incremental deltaQ=0":"ideal voltage boundary"}});}
    return {{"maximumAbsoluteAxialStrain",maximumAbsoluteStrain},{"maximumIncrementalAxialStrain",maximumIncrementalStrain},{"minimumPrestressAxialStrain",minimumPrestressStrain},{"maxIncrementalOverMinimumPrestressStrain",maximumIncrementalStrain/minimumPrestressStrain},{"maximumAbsoluteElectricFieldVPerM",maximumElectricField},{"maximumIncrementalElectricFieldVPerM",maximumIncrementalElectricField},{"smallIncrementalStrainGate","abs strain <2e-4; incremental/prestrain <.01 (numerical scope, not damage qualification)"},{"ports",portMetrics}};
}

QJsonArray trajectory(const Trace &trace){QJsonArray samples;for(const auto &s:trace.samples){QJsonArray electrical;for(const auto &r:s.absoluteElectrical)electrical.append(QJsonObject{{"portId",r.portId},{"absoluteValue",r.value},{"deltaValue",r.deltaValue},{"absoluteVoltageV",r.voltage},{"deltaVoltageV",r.deltaVoltage},{"absoluteChargeC",r.charge},{"deltaChargeC",r.deltaCharge}});samples.append(QJsonObject{{"time",s.time},{"deltaState",array(s.x)},{"deltaReadouts",array(s.y)},{"heldAbsoluteVoltage",array(s.applied)},{"physicalEndpointPerturbationForce",array(s.force)},{"actualAcceptedElectrical",electrical}});}return samples;}
void writeEvidence(const Experiment &e,const Trace &trace,const Scenario &setup,const QJsonObject &metrics,const QString &status,const Trace *off=nullptr,const Trace *romOn=nullptr,const Trace *romOff=nullptr){
    const auto destination=qEnvironmentVariable("RANGE_PIEZO_DELIVERY_EVIDENCE_DIR");if(destination.isEmpty())return;
    require(QDir().mkpath(destination),"Cannot create explicitly requested evidence directory");
    QJsonObject record{{"schema","range.piezo-representative-case-run/v2"},{"case",QString::fromLatin1(QTest::currentDataTag())},{"qualificationStatus",status},{"qualified",status=="QUALIFIED"},{"presetMetadata",setup.presetMetadata},{"rawRecipe",e.recipe},{"recipeSha256",sha(json(e.recipe))},{"sourceFingerprint",e.exported["sourceFingerprint"]},{"export",e.exported},{"fullBasisExport",setup.fullExport},{"truncatedBasisExport",setup.truncatedExport},{"actualNativeFETrace",trajectory(trace)},{"metrics",metrics}};
    if(!trace.finalBytes.isEmpty())record["acceptedRuntimeSha256"]=sha(trace.finalBytes);if(off)record["actualNativeFEOffTrace"]=trajectory(*off);if(romOn)record["independentROMOnTrace"]=trajectory(*romOn);if(romOff)record["independentROMOffTrace"]=trajectory(*romOff);
    QFile file(QDir(destination).filePath(QString::fromLatin1(QTest::currentDataTag())+".json"));require(file.open(QIODevice::WriteOnly),"Cannot open requested evidence file");const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Indented);require(file.write(bytes)==bytes.size(),"Incomplete evidence write");
}
}

#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoPztPrestress:public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {RLogger::getInstance().setLevel(R_LOG_LEVEL_QUIET);}
    void sameSourceScientificEvidence_data();
    void sameSourceScientificEvidence();
    void savedExperimentControllerPhases_data();
    void savedExperimentControllerPhases();
};
void TestPiezoPztPrestress::sameSourceScientificEvidence_data(){
    QTest::addColumn<QString>("family");QTest::addColumn<QString>("kind");QTest::addColumn<bool>("forced");QTest::addColumn<uint>("stride");
    for(const auto &family:{QString("axialRod")})for(const auto &kind:{QString("LQR"),QString("MPC")})for(bool forced:{false,true})QTest::newRow(qPrintable(QString("PZT-prestress-")+kind+(forced?"-forced":"-free")))<<family<<kind<<forced<<(forced?3u:2u);
}
void TestPiezoPztPrestress::sameSourceScientificEvidence() {
    QFETCH(QString,family);QFETCH(QString,kind);QFETCH(bool,forced);QFETCH(uint,stride);Scenario setup;std::unique_ptr<Experiment> instance;Trace trace,uncontrolled,romOn,romOff;QJsonObject metrics;
    const auto fail=[&](const QString &message){metrics["failure"]=message;qWarning().noquote()<<"PC10 FAILED"<<QTest::currentDataTag()<<json(metrics);
        if(instance){if(trace.samples.empty())trace.samples.push_back(instance->current());trace.finalBytes=instance->bytes();writeEvidence(*instance,trace,setup,metrics,"FAILED",uncontrolled.samples.empty()?nullptr:&uncontrolled,romOn.samples.empty()?nullptr:&romOn,romOff.samples.empty()?nullptr:&romOff);}
        else{const auto destination=qEnvironmentVariable("RANGE_PIEZO_DELIVERY_EVIDENCE_DIR");if(!destination.isEmpty()){require(QDir().mkpath(destination),"Cannot create failure evidence directory");QFile file(QDir(destination).filePath(QString::fromLatin1(QTest::currentDataTag())+".json"));require(file.open(QIODevice::WriteOnly),"Cannot open failure evidence file");const auto bytes=QJsonDocument(QJsonObject{{"schema","range.piezo-representative-case-run/v2"},{"case",QString::fromLatin1(QTest::currentDataTag())},{"qualified",false},{"qualificationStatus","FAILED_BEFORE_PRIMARY_FE_CONSTRUCTION"},{"family",family},{"controllerKind",kind},{"forced",forced},{"presetRevision","PC10-PZT-prestress-v1"},{"rawRecipe",setup.recipe},{"metrics",metrics}}).toJson(QJsonDocument::Indented);require(file.write(bytes)==bytes.size(),"Incomplete failure evidence write");}}};
    try {
    setup=scenario(family,kind,forced,stride);instance=std::make_unique<Experiment>(setup.recipe);auto &e=*instance;
    metrics["independentPztPrestressPhysics"]=verifyPztWorkingPoint(e);
    Trace initial;initial.samples.push_back(e.current());initial.finalBytes=e.bytes();writeEvidence(e,initial,setup,metrics,"QUALIFICATION_IN_PROGRESS");
    uint physical=0;const auto type=family=="axialRod"?R_ELEMENT_TRUSS1:family=="planarBeam"?R_ELEMENT_BEAM1:R_ELEMENT_QUAD1;for(uint i=0;i<e.model.getNElements();++i)if(e.model.getElement(i).getType()==type)++physical;require(physical==4,"Representative source must contain four actual structural elements");
    for(const char *key:{"M","K","C"})require(e.exported[key]==setup.fullExport[key]&&e.exported[key]==setup.truncatedExport[key],"Full/selected ROM must use identical actual source operators");
    const auto runtime=e.exported["privateRuntime"].toObject();for(const char *key:{"python","numpy","scipy"})require(runtime[key].isString()&&!runtime[key].toString().isEmpty(),"Missing actual private runtime version");
    const auto design=e.exported["design"].toObject();require(design["massOrthogonalityError"].isDouble()&&design["massOrthogonalityError"].toDouble()<1e-8,"Actual modal mass orthogonality failed");for(double residual:numbers(design["modalResiduals"]))require(residual<1e-8,"Actual modal residual failed");
    const auto controller=design["controller"].toObject();if(kind=="LQR"){require(controller["riccatiRelativeResidual"].isDouble()&&controller["riccatiRelativeResidual"].toDouble()<1e-8&&controller["K"].isArray(),"Actual LQR Riccati certificate missing or failed");require(!controller["closedPoles"].toArray().isEmpty(),"LQR closed poles missing");for(auto p:controller["closedPoles"].toArray()){const auto pole=numbers(p);require(pole.size()==2&&std::hypot(pole[0],pole[1])<1.,"Actual LQR closed pole is not stable");}}
    if(kind!="none"){
        // The initial boundary is a real control sample for every N. Proposal
        // calls are pure: neither comparison consumes an accepted state.
        if(e.axial){auto p=e.rod->acceptedState();const auto &r=e.rod->acceptedRuntime();const auto a=e.axialBridge.callbacks.propose(p,r.readouts,r.completedFE,r.controlTick,r.opaqueControllerState);for(double &v:p.velocities)v*=1.1;const auto b=e.axialBridge.callbacks.propose(p,r.readouts,r.completedFE,r.controlTick,r.opaqueControllerState);require(norm(plus(a.requestedVoltages,b.requestedVoltages,-1))>1e-14,"Actual controller ignores FE velocity feedback");}
        else{auto p=e.layered->acceptedState();const auto &r=e.layered->acceptedRuntime();const auto a=e.layeredBridge.callbacks.propose(p,r.completedFE,r.controlTick,r.opaqueControllerState);for(double &v:p.integrator.state.v)v*=1.1;for(double &v:p.physicalVelocities)v*=1.1;const auto b=e.layeredBridge.callbacks.propose(p,r.completedFE,r.controlTick,r.opaqueControllerState);require(norm(plus(a.requestedVoltages,b.requestedVoltages,-1))>1e-14,"Actual controller ignores FE velocity feedback");}
    }
    e.run(&trace);require(trace.samples.size()>2&&trace.ticks==uint((trace.samples.size()-2+stride)/stride),"Actual FE control counters incorrect");metrics["actualNativeFEOn"]=trajectoryMetrics(e.exported,trace,setup.presetMetadata["replayTruncation"].toArray());writeEvidence(e,trace,setup,metrics,"ACCEPTED_FE_TRACE_NUMERICAL_CHECKS_PENDING");verifyReadouts(e,trace);metrics["actualNativePztStrainFieldReadouts"]=verifyPztAbsoluteReadouts(e,trace);const auto error=measureReplay(e,trace,setup.fullExport,setup.truncatedExport);
    const double scale=std::max(error.peak,1e-300);metrics["sameInputReplay"]=QJsonObject{{"nativeRelativeEnergyError",error.native1/scale},{"nativeHalfStepRelativeEnergyError",error.native2/scale},{"nativeQuarterStepRelativeEnergyError",error.native4/scale},{"refinement2Ratio",error.native2/std::max(error.native1,1e-300)},{"refinement4Ratio",error.native4/std::max(error.native2,1e-300)},{"RK4EightVersusSixteenRelativeError",error.rk4/scale},{"fullBasisRelativeError",error.fullBasis/scale},{"selectedBasisRelativeError",error.truncated/scale},{"resolvedDefectRelativeBound",error.defectBound/scale},{"maximumBoundExcess",error.maxBoundExcess},{"qualified",replayQualified(error)}};
    double ratio=1,romRatio=1;if(kind!="none"){auto off=setup.recipe;off["controller"]=QJsonObject{{"kind","none"},{"controlEveryFESteps",int(stride)}};Experiment baseline(off);for(const char *key:{"M","K","C"})require(baseline.exported[key]==e.exported[key],"Off/on source operator mismatch");baseline.run(&uncontrolled);
        require(uncontrolled.samples.size()==trace.samples.size()&&uncontrolled.samples[0].x==trace.samples[0].x,"Off/on initial state or grid mismatch");for(size_t i=0;i<trace.samples.size();++i)require(uncontrolled.samples[i].time==trace.samples[i].time&&uncontrolled.samples[i].force==trace.samples[i].force,"Off/on force endpoint mismatch");
        ratio=rmsEnergy(e.exported,trace)/rmsEnergy(baseline.exported,uncontrolled);metrics["FEClosedLoopOffOnEnergyRMS"]=ratio;metrics["actualNativeFEOff"]=trajectoryMetrics(baseline.exported,uncontrolled,setup.presetMetadata["replayTruncation"].toArray());
        qInfo().noquote()<<"PC10 FE MEASURED"<<QTest::currentDataTag()<<"preset"<<json(setup.presetMetadata)<<"metrics"<<json(metrics);writeEvidence(e,trace,setup,metrics,ratio<=1.02&&replayQualified(error)?"FE_MEASURED_ROM_CLOSED_LOOP_PENDING":"FAILED_FE_QUALIFICATION",&uncontrolled);
        romOn=independentROMClosedLoop(e,trace,true);romOff=independentROMClosedLoop(e,trace,false);romRatio=rmsEnergy(e.exported,romOn)/rmsEnergy(e.exported,romOff);metrics["independentROMClosedLoopOffOnEnergyRMS"]=romRatio;metrics["independentROMOn"]=trajectoryMetrics(e.exported,romOn,setup.presetMetadata["replayTruncation"].toArray());metrics["independentROMOff"]=trajectoryMetrics(e.exported,romOff,setup.presetMetadata["replayTruncation"].toArray());
    }
    double deltaActionPeak=0,romDeltaActionPeak=0;const double voltage0=12.;for(const auto &sample:trace.samples)for(double voltage:sample.applied)deltaActionPeak=std::max(deltaActionPeak,std::abs(voltage-voltage0));for(const auto &sample:romOn.samples)for(double voltage:sample.applied)romDeltaActionPeak=std::max(romDeltaActionPeak,std::abs(voltage-voltage0));require(deltaActionPeak>1e-12&&romDeltaActionPeak>1e-12,"Nonzero absolute bias is not a control action");metrics["deltaAppliedVoltagePeakV"]=deltaActionPeak;metrics["independentROMDeltaAppliedVoltagePeakV"]=romDeltaActionPeak;metrics["actualMPCKKTMaximum"]=trace.kktPeak;const bool qualified=replayQualified(error)&&ratio<=1.02&&romRatio<=1.02&&(kind=="none"||(trace.voltagePeak>1e-12&&romOn.voltagePeak>1e-12))&&(kind!="MPC"||trace.kktPeak<1e-6);
    writeEvidence(e,trace,setup,metrics,qualified?"QUALIFIED":"FAILED",uncontrolled.samples.empty()?nullptr:&uncontrolled,romOn.samples.empty()?nullptr:&romOn,romOff.samples.empty()?nullptr:&romOff);
    qInfo().noquote()<<"PC10"<<QTest::currentDataTag()<<"recipeSha256"<<sha(json(setup.recipe))<<"source"<<e.exported["sourceFingerprint"].toString()<<"exportSha256"<<sha(json(e.exported))<<"backend"<<e.exported["backendIdentity"].toString()<<"privateRuntime"<<json(runtime)<<"modalResiduals"<<QJsonDocument(design["modalResiduals"].toArray()).toJson(QJsonDocument::Compact)<<"massOrthogonality"<<design["massOrthogonalityError"].toDouble()<<"omega"<<QJsonDocument(setup.fullExport["design"].toObject()["omega"].toArray()).toJson(QJsonDocument::Compact)<<"clusters"<<QJsonDocument(setup.fullExport["design"].toObject()["clusters"].toArray()).toJson(QJsonDocument::Compact)<<"controllerCertificate"<<json(controller)<<"dt"<<setup.recipe["dt"].toDouble()<<"steps"<<trace.samples.size()-1<<"nativeError"<<error.native1/error.peak<<"refinement2"<<error.native2/std::max(error.native1,1e-30)<<"refinement4"<<error.native4/std::max(error.native2,1e-30)<<"RK4error"<<error.rk4/error.peak<<"fullBasisError"<<error.fullBasis/error.peak<<"truncatedError"<<error.truncated/error.peak<<"resolvedDefectBound"<<error.defectBound/error.peak<<"FEclosedLoopOffOnEnergyRMS"<<ratio<<"independentROMClosedLoopOffOnEnergyRMS"<<romRatio<<"appliedVoltagePeak"<<trace.voltagePeak<<"actualKKT"<<trace.kktPeak;
    require(replayQualified(error),"Independent FE/ROM numerical qualification failed; inspect saved metrics");require(ratio<=1.02,"Actual FE closed-loop energy RMS regresses; inspect saved ratio/peak/spillover");require(romRatio<=1.02,"Independent ROM closed-loop energy RMS regresses; inspect saved metrics");require(kind=="none"||(trace.voltagePeak>1e-12&&romOn.voltagePeak>1e-12),"Vacuous actual control action");require(kind!="MPC"||trace.kktPeak<1e-6,"Actual MPC KKT residual failed");
    }catch(const RError &error){const auto message=error.getMessage();try{fail(message);}catch(const std::exception &write){qWarning()<<"Failure evidence write failed:"<<write.what();}QFAIL(qPrintable(message));}catch(const std::exception &error){const auto message=QString::fromUtf8(error.what());try{fail(message);}catch(const std::exception &write){qWarning()<<"Failure evidence write failed:"<<write.what();}QFAIL(qPrintable(message));}
}
void TestPiezoPztPrestress::savedExperimentControllerPhases_data(){
    QTest::addColumn<QString>("family");QTest::addColumn<QString>("extension");QTest::addColumn<uint>("stride");QTest::addColumn<QString>("kind");
    for(const auto &kind:{QString("LQR"),QString("MPC")})for(const auto &extension:{QString("ram"),QString("rbm")})for(uint stride:{2u,3u})QTest::newRow(qPrintable(QString("PZT-prestress-%1-%2-N%3").arg(kind,extension).arg(stride)))<<QString("axialRod")<<extension<<stride<<kind;
}
void TestPiezoPztPrestress::savedExperimentControllerPhases() try {
    QFETCH(QString,family);QFETCH(QString,extension);QFETCH(uint,stride);QFETCH(QString,kind);auto setup=scenario(family,kind,true,stride);setup.recipe["steps"]=int(2*stride+1);Experiment complete(setup.recipe);verifyPztWorkingPoint(complete);const auto expected=complete.run().finalBytes;QTemporaryDir dir;QVERIFY(dir.isValid());
    // A sample boundary and every held-command phase. Existing native suites
    // retain the larger every-split and nonzero-workpoint qualification matrix.
    // Reuse the exact original source, including material UUIDs. Independent
    // template construction creates a distinct source even with equal geometry.
    for(uint phase=0;phase<stride;++phase){Experiment first(setup.recipe,&complete.model);QCOMPARE(first.exported["sourceFingerprint"],complete.exported["sourceFingerprint"]);for(uint i=0;i<stride+phase;++i)first.step();auto model=first.acceptedModel();const auto before=first.bytes();const auto recipeBefore=RPiezoExperimentService::recipeFromModel(model);QVERIFY(!recipeBefore.isEmpty());const auto path=dir.filePath(QString("phase-%1.%2").arg(phase).arg(extension));const auto nativePath=model.nativeWritePaths(path,false).first();model.write(path,false);RModel disk;disk.read(nativePath);QCOMPARE(RPiezoExperimentService::recipeFromModel(disk),recipeBefore);disk.getProblemSetup().setRestart(true);
        if(family=="axialRod"){auto bridge=PiezoAxialBackendBridge::create(disk,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));AxialRun restored(disk,bridge.callbacks);QCOMPARE(RStructuralPiezoAxialRestartCodec::encode(restored.acceptedRuntime()),before);while(restored.hasRemainingSteps()){auto p=restored.prepare(disk);restored.validatePrepared(*p);restored.commit(*p);}QCOMPARE(RStructuralPiezoAxialRestartCodec::encode(restored.acceptedRuntime()),expected);}
        else{auto bridge=PiezoNativeBackendBridge::create(disk,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));LayerRun restored(disk,bridge.callbacks);QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(restored.acceptedRuntime()),before);while(restored.hasRemainingSteps()){auto p=restored.prepare(disk);restored.validatePrepared(*p);restored.commit(*p);}QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(restored.acceptedRuntime()),expected);}
    }
} REPORT_EXCEPTIONS
QTEST_GUILESS_MAIN(TestPiezoPztPrestress)
#include "tst_piezo_pzt_prestress.moc"
