#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <rbl_error.h>
#include <rml_model.h>
#include <rml_file_utils.h>
#include <rsolver.h>
#include <rstructural_native_control.h>
#include <rstructural_truss_dynamics_restart_adapter.h>
#include <rstructural_truss_native_run.h>
#include "native_accepted_checkpoint.h"

namespace {
using Store=NativeAcceptedCheckpointStore;
using Record=RStructuralTrussDynamicsRestartData;
using Spec=RStructuralNativeControlSpec;
using Control=RStructuralNativeControlState;
using Measurement=RStructuralNativeMeasurementQuantity;
using Action=RStructuralNativeActionQuantity;
constexpr double nominalDt=.0034567890123456789,mass=2.5;
void require(bool condition,const char *message)
{if(!condition)throw std::runtime_error(message);}
void close(double actual,double expected,double tolerance,const char *field,uint step)
{
    if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance)
        throw std::runtime_error(qPrintable(QString("Native control step %1 %2: actual %3, independent %4, tolerance %5")
            .arg(step).arg(QString::fromLatin1(field)).arg(actual,0,'g',17).arg(expected,0,'g',17).arg(tolerance,0,'g',8)));
}
void set(RBoundaryCondition &bc,RVariableType type,double value,bool enabled=true)
{auto &c=bc.getComponent(bc.findComponentPosition(type));c.setEnabled(enabled);c.clear();if(enabled)c.add(0,value);}
RBoundaryCondition support(bool x)
{RBoundaryCondition bc(R_BOUNDARY_CONDITION_DISPLACEMENT);set(bc,R_VARIABLE_DISPLACEMENT_X,0,x);
 set(bc,R_VARIABLE_DISPLACEMENT_Y,0);set(bc,R_VARIABLE_DISPLACEMENT_Z,0);return bc;}
RModel fixture()
{
    RModel m;m.getProblemSetup().setMassMatrixType(RMassMatrixType::Lumped);
    m.addNode(RNode(0,0,0));m.addNode(RNode(1,0,0));m.setNodePhysicalId(0,101);m.setNodePhysicalId(1,202);
    const uint ids[]={901,503,777,609,811};
    for(uint i=0;i<5;++i){RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,0);e.setNodeId(1,1);m.addElement(e,true,i);m.setElementPhysicalId(i,ids[i]);
        RMaterial mat(RMaterial::Solid);RMaterialProperty young(RMaterialProperty::ModulusOfElasiticity),rho(RMaterialProperty::Density);
        young.add(293.15,10);rho.add(293.15,1);mat.add(young);mat.add(rho);
        m.getLine(i).setMaterial(mat);m.getLine(i).setCrossArea(1);m.getLine(i).addBoundaryCondition(support(false));}
    for(uint i=0;i<2;++i){RElement e(R_ELEMENT_POINT);e.setNodeId(0,i);m.addElement(e,true,i);}
    m.getPoint(0).addBoundaryCondition(support(true));
    RInitialCondition q(R_INITIAL_CONDITION_DISPLACEMENT),v(R_INITIAL_CONDITION_VELOCITY);
    const RVariableType qt[]={R_VARIABLE_DISPLACEMENT_X,R_VARIABLE_DISPLACEMENT_Y,R_VARIABLE_DISPLACEMENT_Z};
    const RVariableType vt[]={R_VARIABLE_VELOCITY_X,R_VARIABLE_VELOCITY_Y,R_VARIABLE_VELOCITY_Z};
    for(uint i=0;i<3;++i){auto &qc=q.getComponent(q.findComponentPosition(qt[i]));qc.setEnabled(true);qc.clear();qc.add(0,i==0?.035:0);
        auto &vc=v.getComponent(v.findComponentPosition(vt[i]));vc.setEnabled(true);vc.clear();vc.add(0,i==0?.08:0);}
    m.getPoint(1).addInitialCondition(q);m.getPoint(1).addInitialCondition(v);
    RBoundaryCondition load(R_BOUNDARY_CONDITION_FORCE);set(load,R_VARIABLE_FORCE_X,1.2);
    set(load,R_VARIABLE_FORCE_Y,0,false);set(load,R_VARIABLE_FORCE_Z,0,false);m.getPoint(1).addBoundaryCondition(load);
    for(uint i=0;i<m.getNElementGroups();++i){auto *g=m.getElementGroupPtr(i);if(!g||!g->hasEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION))continue;
        auto &gravity=g->getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        for(auto type:{R_VARIABLE_G_ACCELERATION_X,R_VARIABLE_G_ACCELERATION_Y,R_VARIABLE_G_ACCELERATION_Z}){
            auto &c=gravity.getComponent(gravity.findComponentPosition(type));c.setEnabled(true);c.clear();c.add(0,0);}}
    m.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));auto &d=m.getProblemSetup().getStructuralDynamicsSetup();
    d.setEnabled(true);d.setTimeStep(nominalDt);d.setNSteps(12);d.setOutputFrequency(1);
    auto &clock=m.getTimeSolver();clock.setEnabled(true);clock.setInputStartTime(nominalDt);clock.setInputTimeStepSize(nominalDt);
    clock.setInputNTimeSteps(12);clock.setOutputFrequency(1);std::vector<double> times;
    for(uint n=1;n<=12;++n)times.push_back(double(n)*nominalDt);clock.setTimes(times);
    auto &s=m.getProblemSetup().getStructuralTrussDynamicsSetup();s.enabled=true;s.passiveMembers={{901,1}};
    s.absoluteForceTolerance=1e-10;s.relativeForceTolerance=1e-10;s.linearRelativeTolerance=1e-12;
    for(uint i=0;i<2;++i){RStructuralTrussActiveMember a;a.physicalElementId=ids[i+1];a.portId=i==0?"V-port":"Q-port";
        a.referenceLength=1;a.axialStiffness=10;a.coupling=.2;a.capacitance=.5;a.initialValue=i==0?.7:.15;
        a.kind=i==0?RStructuralTrussElectricalKind::Voltage:RStructuralTrussElectricalKind::ConductorCharge;s.activeMembers.push_back(a);}
    s.cableMembers={{609,1,1},{811,3,0}};s.heldCommands.clear();
    Spec spec;spec.source=RStructuralNativeControlSource::LocalV3;RStructuralNativeControl::stampBuiltinIdentity(spec);
    // Catalog order deliberately differs from physical IDs, port order, and PI order.
    spec.sensors={{"Q-voltage",Measurement::PortVoltage,0,0,"Q-port",0},
        {"left-a",Measurement::NodeAcceleration,202,0,{},0},
        {"cable-force",Measurement::MemberAxialForce,0,609,{},0},
        {"tip-q",Measurement::NodeDisplacement,202,0,{},0},
        {"V-charge",Measurement::PortCharge,0,0,"V-port",0},
        {"support-R",Measurement::SupportReaction,101,0,{},0},
        {"Q-charge",Measurement::PortCharge,0,0,"Q-port",0},
        {"V-voltage",Measurement::PortVoltage,0,0,"V-port",0},
        {"tip-v",Measurement::NodeVelocity,202,0,{},0}};
    spec.actions={{"F-delta",Action::Force,202,0,{},0,-.2,.2,.5,0},
        {"V",Action::Voltage,0,0,"V-port",0,0,2,.01,.7},
        {"Q",Action::Charge,0,0,"Q-port",0,-.5,.5,.001,.15},
        {"L0",Action::CableReferenceLength,0,609,{},0,.8,1.2,.05,1}};
    spec.piLoops={{"Q","tip-q",0,.02,.03,.15,.01},{"F-delta","tip-q",0,2,3,.1,.01},
        {"L0","tip-q",0,.04,.05,1,.02},{"V","tip-q",0,.1,.2,.7,.02}};
    m.getProblemSetup().setStructuralNativeControlSpec(spec);s.bindToModel(m);return m;
}
const Record &record(const RModel &m){return m.getProblemSetup().getStructuralTrussDynamicsRestartData();}
const Control &control(const RModel &m){return m.getProblemSetup().getStructuralNativeControlState();}

// Separate field-by-field QDataStream witness. No production serialization,
// identityBytes(), fingerprintBytes(), stateFingerprint(), or MODEL I/O supplies it.
struct Witness {
    QByteArray bytes;QDataStream out;
    Witness():out(&bytes,QIODevice::WriteOnly){}
    void real(double x){quint64 bits;std::memcpy(&bits,&x,sizeof(bits));out<<bits;}
    void vec(const std::vector<double>&v){out<<quint64(v.size());for(double x:v)real(x);}
    void ids(const std::vector<uint>&v){out<<quint64(v.size());for(uint x:v)out<<x;}
    void physical(const Record &r){out<<r.available<<r.completedSteps<<r.runTargetSteps<<r.nextCommandIndex;
        real(r.initialTime);real(r.time);real(r.fixedTimeStep);ids(r.orderedPhysicalNodeIds);ids(r.orderedMemberPhysicalIds);
        out<<quint64(r.orderedPortIds.size());for(const auto &id:r.orderedPortIds)out<<id;
        out<<r.sourceFingerprint<<r.systemIdentity;vec(r.q);vec(r.v);vec(r.a);vec(r.reactions);vec(r.externalForce);
        ids(r.memberInputKinds);vec(r.memberInputValues);vec(r.portVoltages);vec(r.portCharges);vec(r.portEnthalpies);
        real(r.storedEnergy);real(r.kineticEnergy);real(r.initialTotalEnergy);real(r.cumulativeExternalWork);
        real(r.cumulativeSupportWork);real(r.cumulativeElectricalWork);real(r.cumulativeRestLengthWork);}
    void spec(const Spec &s){out<<s.schemaVersion<<quint8(s.source)<<s.providerId<<s.providerRevision<<s.implementationSha256
        <<s.dependencyEnvironmentManifest<<s.dependencyEnvironmentSha256<<s.controlEveryFESteps<<s.samplePhase<<s.deterministicSeed;
        out<<quint64(s.sensors.size());for(const auto &v:s.sensors)out<<v.sensorId<<quint8(v.quantity)<<v.physicalNodeId<<v.physicalMemberId<<v.portId<<v.axis;
        out<<quint64(s.actions.size());for(const auto &v:s.actions){out<<v.actionId<<quint8(v.quantity)<<v.physicalNodeId<<v.physicalMemberId<<v.portId<<v.axis;
            real(v.minimum);real(v.maximum);real(v.rateLimit);real(v.initialCommand);out<<v.delayTicks<<v.holdTicks;real(v.noiseAmplitude);}
        out<<quint64(s.piLoops.size());for(const auto &v:s.piLoops){out<<v.actionId<<v.sensorId;real(v.referenceValue);
            real(v.proportionalGain);real(v.integralGain);real(v.bias);real(v.initialIntegral);}}
    void state(const Control &s){out<<s.available<<s.schemaVersion<<s.sourceFingerprint<<s.providerRevision<<s.completedFE
        <<s.completedControlTicks<<s.fePhase<<s.samplePhase<<s.lastControllerInputState<<s.controllerState;
        const auto &f=s.lastFrame;out<<f.completedFE<<f.controlTick<<f.fePhase<<f.samplePhase;
        real(f.sampleTime);real(f.nextFETime);real(f.nextControlTime);real(f.physicalDt);
        out<<quint64(f.samples.size());for(const auto &v:f.samples){out<<v.sensorId<<quint8(v.quantity);real(v.value);}
        out<<quint64(s.lastActions.size());for(const auto &v:s.lastActions){out<<v.actionId<<quint8(v.quantity);
            real(v.previousApplied);real(v.requested);real(v.conditioned);real(v.applied);out<<v.limitFlags;}
        vec(s.heldValues);vec(s.lastAppliedValues);}
};
QByteArray combined(const Record &r,const Spec &s,const Control &c)
{Witness w;w.physical(r);w.spec(s);w.state(c);return w.bytes;}
QByteArray combined(const RModel &m)
{return combined(record(m),m.getProblemSetup().getStructuralNativeControlSpec(),control(m));}
QByteArray resultsAndClock(const RModel &m)
{
    Witness w;w.real(m.getTimeSolver().getComputedTime());w.out<<m.getNVariables();
    for(uint i=0;i<m.getNVariables();++i){const auto &v=m.getVariable(i);w.out<<uint(v.getType())<<uint(v.getApplyType())
        <<v.getName()<<v.getUnits()<<v.getNVectors()<<v.getNValues();const auto &d=v.getVariableData();
        w.real(d.getMinDisplayValue());w.real(d.getMaxDisplayValue());w.out<<d.getDisplayValueRange()<<d.getValueRangeName();w.real(d.getScale());
        for(uint a=0;a<v.getNVectors();++a)for(uint b=0;b<v.getNValues();++b)w.real(v.getValue(a,b));}return w.bytes;
}
QByteArray raw(const QString &p)
{QFile f(p);require(f.open(QIODevice::ReadOnly),"Cannot read test artifact");return f.readAll();}
QString digest(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
QString fileDigest(const QString &p){return digest(raw(p));}
NativeAcceptedCheckpointStateView view(const RModel &m)
{
    RStructuralTrussDynamicsModelAdapter a(m);auto r=record(m);auto c=control(m);
    if(r.available)RStructuralTrussDynamicsRestartAdapter::restore(a,r,c);
    else {RStructuralNativeControl provider(a);c=provider.initialize();r=RStructuralTrussDynamicsRestartAdapter::build(a,a.initialize(),0,0,c);}
    return {r.completedSteps,r.time,r.fixedTimeStep,QCryptographicHash::hash(combined(r,a.nativeControlSpec(),c),QCryptographicHash::Sha256)};
}
QByteArray context(const QString &input,const QString &runtime)
{return QJsonDocument(QJsonObject{{"schema","range.native-checkpoint-context/v1"},{"sourceSha256",input},{"modelSha256",input},
    {"controlSha256",NativeAcceptedCheckpointContext::noControlSha256()},{"runtimeSha256",runtime},{"packageSha256",QString(64,'d')},
    {"policy","accepted-steps-or-sim-time-v1"},{"maxAcceptedSteps",10},{"maxSimulationSeconds",1.0},{"dtS",nominalDt}}).toJson(QJsonDocument::Compact);}
std::shared_ptr<Store> participant(const QString &dir,const RModel &initial)
{const QString input=QString(64,'a'),runtime=QString(64,'c');return std::make_shared<Store>(dir,context(input,runtime),input,
    NativeAcceptedCheckpointContext::noControlSha256(),runtime,false,initial,view);}
QString failure(RSolver &s)
{try{s.run();return {};}catch(const RError &e){return e.getMessage();}catch(const std::exception &e){return QString::fromUtf8(e.what());}}
template<class F> bool rejected(F f)
{try{f();}catch(const RError &){return true;}catch(const std::exception &){return true;}return false;}
QString latestPath(const QString &dir)
{
    const auto p=QJsonDocument::fromJson(raw(QDir(dir).filePath("latest.json"))).object();
    const auto envelopePath=QDir(dir).filePath(p.value("envelopeFile").toString());
    require(fileDigest(envelopePath)==p.value("envelopeSha256").toString(),"Envelope hash mismatch");
    const auto e=QJsonDocument::fromJson(raw(envelopePath)).object();const auto path=QDir(dir).filePath(e.value("modelFile").toString());
    require(fileDigest(path)==e.value("checkpointModelSha256").toString(),"MODEL hash mismatch");return path;
}
RModel latest(const QString &dir){RModel m;m.read(latestPath(dir));return m;}
QString executable()
{
#ifdef RANGE_FEA_SOLVER_EXECUTABLE
    return QString::fromUtf8(RANGE_FEA_SOLVER_EXECUTABLE);
#elif defined(Q_OS_WIN)
    return QDir(QCoreApplication::applicationDirPath()).filePath("fea-solver.exe");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("fea-solver");
#endif
}
struct ProcessResult {int code;QByteArray diagnostics;};
ProcessResult cli(const QString &input,const QString &dir,const QByteArray &ctx,uint budget=0,bool restart=false)
{
    QStringList args={"--file="+input,"--native-checkpoint-dir="+dir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx),
        "--native-mixed-steps="+QString::number(budget)};if(restart)args.append("--restart");
    QProcess p;p.setProcessChannelMode(QProcess::MergedChannels);p.start(executable(),args);
    if(!p.waitForStarted(10000))throw std::runtime_error(qPrintable(p.errorString()));
    if(!p.waitForFinished(60000)){p.kill();p.waitForFinished(5000);throw std::runtime_error("Native control CLI timed out");}
    const auto out=p.readAll();require(p.exitStatus()==QProcess::NormalExit,"Native control CLI abnormal exit");return {p.exitCode(),out};
}

// Independent mechanics and PI history: action order F,V,Q,L; envelope order Q,F,L,V.
// No production assembly, driver, provider, restore, or serialization computes this reference.
struct Expected {
    double q=.035,v=.08,a=0,rightA=0,cable=0,reaction=0;
    double stored=0,kinetic=0,initial=0,external=0,electrical=0,restLength=0;
    std::array<double,4> applied{{0,.7,.15,1}},integrals{{.01,.02,.01,.02}};
    std::array<double,4> oldI{},previous{},requested{};
    std::array<quint32,4> flags{};
};
double cableForce(double q,double length){return 1+(10/length)*(1+q-length);}
double electricalEnergy(double q,double voltage,double charge)
{const double qVoltage=(charge+.2*q)/.5;return .25*voltage*voltage+.25*qVoltage*qVoltage;}
double cableEnergy(double q,double length)
{const double force=cableForce(q,length);return .5*force*force/(10/length);}
void observe(Expected &s)
{
    const double length=s.applied[3],K=30.08+10/length,B=.2*s.applied[1]+.4*s.applied[2]+1+(10/length)*(1-length);
    s.cable=cableForce(s.q,length);s.reaction=-(K*s.q+B);s.a=(1.2+s.applied[0]+s.reaction)/mass;
    s.stored=.5*30*s.q*s.q+electricalEnergy(s.q,s.applied[1],s.applied[2])+cableEnergy(s.q,length);
    s.kinetic=.5*mass*s.v*s.v;
}
std::vector<Expected> history()
{
    const std::array<double,4> bias{{.1,.7,.15,1}},kp{{2,.1,.02,.04}},ki{{3,.2,.03,.05}},
        minimum{{-.2,0,-.5,.8}},maximum{{.2,2,.5,1.2}},rate{{.5,.01,.001,.05}};
    Expected s;observe(s);s.initial=s.stored+s.kinetic;std::vector<Expected> values{s};
    close(s.a,-40.08*.035/mass,2e-14,"source left acceleration",0);
    for(uint step=1;step<=12;++step){const auto left=s;const double h=double(step)*nominalDt-double(step-1)*nominalDt;
        s.previous=left.applied;s.oldI=left.integrals;
        for(size_t i=0;i<4;++i){const double error=-left.q;s.requested[i]=bias[i]+kp[i]*error+ki[i]*left.integrals[i];
            s.integrals[i]=left.integrals[i]+nominalDt*error;
            const double amplitude=std::max(minimum[i],std::min(maximum[i],s.requested[i]));
            s.applied[i]=std::max(left.applied[i]-rate[i]*h,std::min(left.applied[i]+rate[i]*h,amplitude));
            s.flags[i]=(amplitude!=s.requested[i]?R_NATIVE_CONTROL_AMPLITUDE_LIMITED:0)
                |(s.applied[i]!=amplitude?R_NATIVE_CONTROL_RATE_LIMITED:0);}
        require(cableForce(left.q,s.applied[3])>0,"Cable609 must be taut at interval start");
        require((10.0/3)*(1+left.q-3)<0,"Cable811 must be slack at interval start");
        s.electrical+=electricalEnergy(left.q,s.applied[1],s.applied[2])-electricalEnergy(left.q,left.applied[1],left.applied[2]);
        s.restLength+=cableEnergy(left.q,s.applied[3])-cableEnergy(left.q,left.applied[3]);
        const double length=s.applied[3],K=30.08+10/length,B=.2*s.applied[1]+.4*s.applied[2]+1+(10/length)*(1-length),F=1.2+s.applied[0];
        s.rightA=(F-B-K*left.q)/mass;const double predictor=left.q+h*left.v+.25*h*h*s.rightA,coefficient=4*mass/(h*h);
        s.q=(coefficient*predictor+F-B)/(coefficient+K);observe(s);s.v=left.v+.5*h*(s.rightA+s.a);observe(s);
        require(s.cable>0,"Cable609 must be taut at interval end");require((10.0/3)*(1+s.q-3)<0,"Cable811 must be slack at interval end");
        s.external+=F*(s.q-left.q);s.electrical+=s.applied[1]*(-.2)*(s.q-left.q);values.push_back(s);
    }return values;
}
std::array<double,4> decodeIndependent(const QByteArray &bytes)
{
    // Documented opaque v1 envelope, interpreted directly rather than via provider code.
    const QByteArray prefix("range-fea/native-pi/integral/v1\0",32);
    require(bytes.startsWith(prefix)&&bytes.size()==prefix.size()+8+4*8,"Unexpected PI envelope schema/size");
    const auto integer=[&](int position){quint64 value=0;for(uint j=0;j<8;++j)value|=quint64(uchar(bytes.at(position+int(j))))<<(8*j);return value;};
    require(integer(prefix.size())==4,"Unexpected PI integral count");std::array<double,4> values{};
    for(uint i=0;i<4;++i){const quint64 bits=integer(prefix.size()+8+8*int(i));std::memcpy(&values[i],&bits,sizeof(bits));}return values;
}
void check(const RModel &m,const std::vector<Expected> &values)
{
    const auto &r=record(m);const auto &c=control(m);const uint n=r.completedSteps;
    require(r.available&&n>0&&n<values.size(),"Accepted native control record unavailable");const auto &s=values[n],&left=values[n-1];
    require(r.runTargetSteps==12&&r.nextCommandIndex==0&&r.q.size()==6&&r.v.size()==6&&r.a.size()==6&&r.reactions.size()==6,
        "Physical dimensions, original target, or frozen-table cursor changed");
    require(r.orderedPhysicalNodeIds==std::vector<uint>({101,202})&&r.orderedMemberPhysicalIds==std::vector<uint>({901,503,777,609,811})
        &&r.orderedPortIds==std::vector<QString>({"V-port","Q-port"}),"Physical ID mapping/order changed");
    close(r.q[3],s.q,2e-11,"q",n);close(r.v[3],s.v,2e-10,"v",n);close(r.a[3],s.a,2e-9,"a",n);
    close(r.reactions[0],s.reaction,2e-9,"support reaction",n);close(r.reactions[3],0,2e-9,"free residual",n);
    for(uint dof:{0u,1u,2u,4u,5u}){close(r.q[dof],0,2e-12,"fixed/transverse q",n);close(r.v[dof],0,2e-11,"fixed/transverse v",n);
        close(r.a[dof],0,2e-10,"fixed/transverse a",n);if(dof!=0)close(r.reactions[dof],0,2e-9,"transverse reaction",n);}
    require(r.memberInputKinds==std::vector<uint>({0,1,2,3,3})&&r.memberInputValues.size()==5&&r.externalForce.size()==6,
        "Complete input kind/count changed");
    close(r.externalForce[3],1.2+s.applied[0],2e-12,"background plus force increment",n);
    for(uint i:{0u,1u,2u,4u,5u})close(r.externalForce[i],0,2e-12,"unactuated force",n);
    close(r.memberInputValues[1],s.applied[1],2e-12,"applied V",n);close(r.memberInputValues[2],s.applied[2],2e-12,"applied Q",n);
    close(r.memberInputValues[3],s.applied[3],2e-12,"applied L0",n);close(r.memberInputValues[4],3,2e-12,"slack L0",n);
    require(r.portVoltages.size()==2&&r.portCharges.size()==2,"Independent port observations unavailable");
    close(r.portVoltages[0],s.applied[1],2e-12,"V port voltage",n);close(r.portCharges[0],.5*s.applied[1]-.2*s.q,2e-11,"V port charge",n);
    close(r.portVoltages[1],(s.applied[2]+.2*s.q)/.5,2e-11,"Q port voltage",n);close(r.portCharges[1],s.applied[2],2e-12,"Q port charge",n);
    close(r.storedEnergy,s.stored,2e-10,"stored energy",n);close(r.kineticEnergy,s.kinetic,2e-10,"kinetic energy",n);
    close(r.initialTotalEnergy,s.initial,2e-10,"initial energy",n);close(r.cumulativeExternalWork,s.external,2e-10,"external work",n);
    close(r.cumulativeSupportWork,0,2e-10,"fixed support work",n);close(r.cumulativeElectricalWork,s.electrical,2e-10,"electrical work",n);
    close(r.cumulativeRestLengthWork,s.restLength,2e-10,"rest length work",n);
    const auto &spec=m.getProblemSetup().getStructuralNativeControlSpec();
    require(c.available&&c.schemaVersion==1&&c.completedFE==n&&c.completedControlTicks==n&&c.fePhase==0&&c.samplePhase==0
        &&c.sourceFingerprint==r.sourceFingerprint&&c.providerRevision==spec.providerRevision,"Accepted controller identity/counters changed");
    const auto &f=c.lastFrame;require(f.completedFE==n-1&&f.controlTick==n-1&&f.fePhase==0&&f.samplePhase==0,"Left frame phase changed");
    require(f.sampleTime==double(n-1)*nominalDt&&f.nextFETime==double(n)*nominalDt&&f.nextControlTime==f.nextFETime
        &&f.physicalDt==f.nextFETime-f.sampleTime,"Left frame canonical clock changed");
    const std::array<double,9> samples{{(left.applied[2]+.2*left.q)/.5,left.a,left.cable,left.q,
        .5*left.applied[1]-.2*left.q,left.reaction,left.applied[2],left.applied[1],left.v}};
    const std::array<const char*,9> units{{"V","m/s^2","N","m","C","N","C","V","m/s"}};
    require(f.samples.size()==samples.size(),"Left sample catalog count changed");
    for(size_t i=0;i<samples.size();++i){require(f.samples[i].sensorId==spec.sensors[i].sensorId&&f.samples[i].quantity==spec.sensors[i].quantity,
        "Left sample identity/quantity changed");require(rStructuralNativeMeasurementUnit(f.samples[i].quantity)==units[i],"Measurement SI unit changed");
        close(f.samples[i].value,samples[i],2e-9,"left accepted sample",n);}
    require(c.lastActions.size()==4&&c.heldValues.size()==4&&c.lastAppliedValues.size()==4,"Action ledger/catalog count changed");
    const std::array<const char*,4> actionUnits{{"N","V","C","m"}};
    for(size_t i=0;i<4;++i){const auto &a=c.lastActions[i];require(a.actionId==spec.actions[i].actionId&&a.quantity==spec.actions[i].quantity,
        "Action identity/order changed");require(rStructuralNativeActionUnit(a.quantity)==actionUnits[i],"Action SI unit changed");
        close(a.previousApplied,s.previous[i],2e-12,"previous action",n);close(a.requested,s.requested[i],5e-11,"old-I PI request",n);
        close(a.conditioned,s.requested[i],5e-11,"conditioned request",n);close(a.applied,s.applied[i],2e-12,"limited action",n);
        close(c.heldValues[i],s.applied[i],2e-12,"held action",n);close(c.lastAppliedValues[i],s.applied[i],2e-12,"last applied action",n);
        const auto &limits=spec.actions[i];const double amplitude=std::max(limits.minimum,std::min(limits.maximum,a.requested));
        const double physicalLimited=std::max(a.previousApplied-limits.rateLimit*f.physicalDt,
            std::min(a.previousApplied+limits.rateLimit*f.physicalDt,amplitude));
        require(a.applied==physicalLimited,"Rate limiter did not use the physical FE interval exactly");
        require(a.limitFlags==s.flags[i],"Amplitude/rate flags differ from actual clamp changes");}
    const auto oldI=decodeIndependent(c.lastControllerInputState),nextI=decodeIndependent(c.controllerState);
    const std::array<size_t,4> loopOrder{{2,0,3,1}};
    for(size_t i=0;i<4;++i){close(oldI[i],s.oldI[loopOrder[i]],2e-12,"old integral envelope",n);
        close(nextI[i],s.integrals[loopOrder[i]],2e-12,"next integral envelope",n);}
    const auto stress=m.findVariable(R_VARIABLE_STRESS_NORMAL);require(stress!=RConstants::eod,"Fresh member axial results unavailable");
    close(m.getVariable(stress).getValue(0,3),s.cable,2e-9,"taut member609 result",n);
    close(m.getVariable(stress).getValue(0,4),0,2e-9,"slack member811 result",n);
    if(n==1)require(std::abs(f.samples[1].value-s.rightA)>1e-5,"Left acceleration was replaced by right action refresh");
}
RModel reference(const RModel &source,const QString &path,const std::vector<Expected> &values)
{auto m=source;RSolver solver(m,path,{});uint observed=0;solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){
    ++observed;require(record(accepted).completedSteps==observed,"Observer skipped/repeated accepted step");check(accepted,values);});
 solver.run();require(observed==12,"Reference real RSolver did not accept all steps");return m;}
}

#define REPORT_CONTROL_EXCEPTIONS \
    catch(const RError &e){QFAIL(qPrintable(e.getMessage()));} \
    catch(const std::exception &e){QFAIL(e.what());} \
    catch(...){QFAIL("Unknown native control run exception");}
class TestNativeControlRun:public QObject {
    Q_OBJECT
private slots:
    void sameSolverBudgetAndIndependentClosedLoop();
    void staleDisplayDoesNotSupplySamples();
    void amplitudeThenPhysicalRateLimit();
    void publicationRollbackSameSolverRetry();
    void preparedTamper_data();void preparedTamper();
    void restartTamper_data();void restartTamper();
    void sourceIdentityRejection_data();void sourceIdentityRejection();
    void freshRunIgnoresPreviousAcceptedHistory();
    void realCliSplitResume_data();void realCliSplitResume();
    void realCliRejectsTamperedRestart_data();void realCliRejectsTamperedRestart();
};
void TestNativeControlRun::sameSolverBudgetAndIndependentClosedLoop() try
{
    QTemporaryDir dir;const auto source=fixture();const auto values=history();const auto expected=reference(source,dir.filePath("full.rbm"),values);auto m=source;
    RStructuralTrussDynamicsModelAdapter adapter(source);QCOMPARE(adapter.system().nodalLumpedMass,std::vector<double>({mass,mass}));
    RSolver solver(m,dir.filePath("split.rbm"),{});solver.setNativeMixedStepBudget(4);std::vector<QByteArray> witnesses;
    solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){check(accepted,values);witnesses.push_back(combined(accepted));});
    solver.run();QCOMPARE(record(m).completedSteps,4u);QCOMPARE(control(m).completedControlTicks,quint64(4));
    QCOMPARE(record(m).runTargetSteps,12u);QCOMPARE(m.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),12u);
    solver.run();QCOMPARE(record(m).completedSteps,8u);solver.setNativeMixedStepBudget(0);solver.run();
    QCOMPARE(record(m).completedSteps,12u);QCOMPARE(witnesses.size(),size_t(12));QCOMPARE(combined(m),combined(expected));
    auto continuous=source;RSolver full(continuous,dir.filePath("witness-full.rbm"),{});size_t i=0;
    full.setAcceptedStructuralStepObserver([&](const RModel &accepted){require(i<witnesses.size()&&combined(accepted)==witnesses[i++],
        "Budget boundary changed complete physical/controller bits");});full.run();QCOMPARE(i,size_t(12));
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::staleDisplayDoesNotSupplySamples() try
{
    QTemporaryDir dir;auto source=fixture();const auto values=history();
    for(auto type:{R_VARIABLE_DISPLACEMENT,R_VARIABLE_VELOCITY,R_VARIABLE_ACCELERATION,R_VARIABLE_STRESS_NORMAL}){
        const auto id=source.addVariable(RVariable(type));auto &variable=source.getVariable(id);
        const bool member=type==R_VARIABLE_STRESS_NORMAL;variable.setApplyType(member?R_VARIABLE_APPLY_ELEMENT:R_VARIABLE_APPLY_NODE);
        variable.resize(member?1:3,member?source.getNElements():source.getNNodes());
        for(uint a=0;a<variable.getNVectors();++a)for(uint b=0;b<variable.getNValues();++b)variable.setValue(a,b,9000+a+b);}
    RSolver solver(source,dir.filePath("stale.rbm"),{});solver.setNativeMixedStepBudget(1);solver.run();check(source,values);
    QCOMPARE(control(source).lastFrame.samples[3].value,.035);QCOMPARE(control(source).lastFrame.samples[8].value,.08);
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::amplitudeThenPhysicalRateLimit() try
{
    auto source=fixture();auto &spec=source.getProblemSetup().getStructuralNativeControlSpec();
    for(auto &loop:spec.piLoops)if(loop.actionId=="F-delta")loop.bias=10;
    source.getProblemSetup().getStructuralTrussDynamicsSetup().bindToModel(source);
    RStructuralTrussNativeRun run(source);auto first=run.prepare(source);const auto &a=control(first->candidateModel()).lastActions[0];
    QCOMPARE(a.requested,10+2*(-.035)+3*.01);QCOMPARE(a.conditioned,a.requested);
    QCOMPARE(a.applied,.5*nominalDt);QCOMPARE(a.limitFlags,quint32(R_NATIVE_CONTROL_AMPLITUDE_LIMITED|R_NATIVE_CONTROL_RATE_LIMITED));
    run.validatePrepared(*first);run.commit(*first);
    auto accepted=first->candidateModel();double previous=a.applied;bool sawRoundedPhysicalInterval=false;
    for(uint n=2;n<=10;++n){auto step=run.prepare(accepted);const auto &c=control(step->candidateModel());const auto &b=c.lastActions[0];
        const double physicalH=double(n)*nominalDt-double(n-1)*nominalDt;QCOMPARE(c.lastFrame.physicalDt,physicalH);
        QCOMPARE(b.applied,previous+.5*physicalH);QCOMPARE(b.limitFlags,quint32(R_NATIVE_CONTROL_AMPLITUDE_LIMITED|R_NATIVE_CONTROL_RATE_LIMITED));
        sawRoundedPhysicalInterval|=physicalH!=nominalDt;run.validatePrepared(*step);run.commit(*step);accepted=step->candidateModel();previous=b.applied;}
    QVERIFY(sawRoundedPhysicalInterval);
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::publicationRollbackSameSolverRetry() try
{
    QTemporaryDir dir;const auto source=fixture();const auto values=history();const auto expected=reference(source,dir.filePath("reference.rbm"),values);auto m=source;
    const auto output=dir.filePath("actual.rbm"),checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m);
    RSolver solver(m,output,{});solver.setAcceptedStepParticipant(store);solver.setNativeMixedStepBudget(1);solver.run();solver.run();
    QCOMPARE(record(m).completedSteps,2u);check(m,values);
    const auto baseline=combined(m),results=resultsAndClock(m),oldLatest=raw(QDir(checkpointDir).filePath("latest.json")),
        oldLast=raw(QDir(checkpointDir).filePath("lastvalid.json")),oldLogical=raw(output);
    const auto sourceIdentity=m.getProblemSetup().getStructuralTrussDynamicsSetup().computeSourceFingerprint(m);
    m.getProblemSetup().setRestart(true);RStructuralTrussNativeRun resumed(m);m.getProblemSetup().setRestart(false);
    auto candidate=resumed.prepare(m);const auto pending=combined(candidate->candidateModel());check(candidate->candidateModel(),values);
    store->setPhaseHook(Store::Phase::AfterLatest,[](Store::Phase){throw std::runtime_error("native control publish failure after latest");});
    QVERIFY(!failure(solver).isEmpty());QVERIFY(solver.isNativeMixedRetryable());QVERIFY(store->isRetryable());
    QCOMPARE(combined(m),baseline);QCOMPARE(resultsAndClock(m),results);QCOMPARE(record(m).completedSteps,2u);
    QCOMPARE(m.getProblemSetup().getStructuralTrussDynamicsSetup().computeSourceFingerprint(m),sourceIdentity);
    QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),oldLatest);QCOMPARE(raw(QDir(checkpointDir).filePath("lastvalid.json")),oldLast);
    QCOMPARE(raw(output),oldLogical);QVERIFY(!QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(output,3)));
    QCOMPARE(combined(latest(checkpointDir)),baseline);
    solver.run();QCOMPARE(record(m).completedSteps,3u);QCOMPARE(combined(m),pending);check(m,values);
    solver.setNativeMixedStepBudget(0);solver.run();QCOMPARE(combined(m),combined(expected));QCOMPARE(combined(latest(checkpointDir)),combined(expected));
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::preparedTamper_data()
{
    QTest::addColumn<int>("mutation");const char *names[]={"action-requested","action-conditioned","action-applied","action-flags",
        "left-sample","frame-time","next-control-time","frame-phase","controller-after","controller-before","held-input",
        "last-applied","control-phase","control-tick","provider-sha","physical-input","accepted-clock"};
    for(int i=0;i<17;++i)QTest::newRow(names[i])<<i;
}
void TestNativeControlRun::preparedTamper() try
{
    QFETCH(int,mutation);auto source=fixture();RStructuralTrussNativeRun run(source);
    const auto baseline=combined(RStructuralTrussDynamicsRestartAdapter::build(run.adapter(),run.acceptedState(),0,0,run.acceptedControlState()),
        run.adapter().nativeControlSpec(),run.acceptedControlState());
    auto candidate=run.prepare(source);auto &m=candidate->candidateModel();auto &c=m.getProblemSetup().getStructuralNativeControlState();
    switch(mutation){case 0:c.lastActions[0].requested+=.01;break;case 1:c.lastActions[0].conditioned+=.01;break;
    case 2:c.lastActions[0].applied+=.01;break;case 3:c.lastActions[0].limitFlags^=1;break;
    case 4:c.lastFrame.samples[3].value+=.01;break;case 5:c.lastFrame.sampleTime+=1e-14;break;
    case 6:c.lastFrame.nextControlTime+=1e-14;break;case 7:c.lastFrame.samplePhase=1;break;
    case 8:c.controllerState.append('x');break;case 9:c.lastControllerInputState.append('x');break;
    case 10:c.heldValues[0]+=.01;break;case 11:c.lastAppliedValues[0]+=.01;break;case 12:c.fePhase=1;break;
    case 13:++c.completedControlTicks;break;case 14:m.getProblemSetup().getStructuralNativeControlSpec().implementationSha256=QString(64,'f');break;
    case 15:m.getProblemSetup().getStructuralTrussDynamicsRestartData().externalForce[3]+=.01;break;
    case 16:m.getTimeSolver().setComputedTime(m.getTimeSolver().getComputedTime()+1e-14);break;}
    QVERIFY(rejected([&]{run.validatePrepared(*candidate);}));QCOMPARE(run.completedSteps(),0u);QCOMPARE(run.nextCommandIndex(),0u);
    QCOMPARE(combined(RStructuralTrussDynamicsRestartAdapter::build(run.adapter(),run.acceptedState(),0,0,run.acceptedControlState()),
        run.adapter().nativeControlSpec(),run.acceptedControlState()),baseline);
    auto retry=run.prepare(source);run.validatePrepared(*retry);check(retry->candidateModel(),history());run.commit(*retry);
    QCOMPARE(run.completedSteps(),1u);QCOMPARE(run.acceptedControlState().completedControlTicks,quint64(1));
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::restartTamper_data()
{
    QTest::addColumn<int>("mutation");const char *names[]={"requested","conditioned","applied","previous-applied","flags","left-q",
        "left-clock","physical-dt","control-clock","opaque-before","opaque-after","phase","sample-phase","provider-revision",
        "source-fingerprint","ticks","held-and-applied","physical-force","physical-length","port-observation","record-clock","missing-control"};
    for(int i=0;i<22;++i)QTest::newRow(names[i])<<i;
}
void TestNativeControlRun::restartTamper() try
{
    QFETCH(int,mutation);auto source=fixture();RStructuralTrussNativeRun run(source);RModel accepted;
    for(uint i=0;i<3;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);accepted=step->candidateModel();}
    auto r=record(accepted);auto c=control(accepted);
    const auto baseline=combined(r,run.adapter().nativeControlSpec(),c);
    const auto restored=RStructuralTrussDynamicsRestartAdapter::restore(run.adapter(),r,c);
    QCOMPARE(combined(RStructuralTrussDynamicsRestartAdapter::build(run.adapter(),restored.state,restored.completedSteps,restored.nextCommandIndex,
        restored.controlState),run.adapter().nativeControlSpec(),restored.controlState),baseline);
    switch(mutation){case 0:c.lastActions[0].requested+=.01;break;case 1:c.lastActions[0].conditioned+=.01;break;
    case 2:c.lastActions[0].applied+=.01;break;case 3:c.lastActions[0].previousApplied+=.01;break;case 4:c.lastActions[0].limitFlags^=1;break;
    case 5:c.lastFrame.samples[3].value+=.01;break;case 6:c.lastFrame.sampleTime+=1e-14;break;case 7:c.lastFrame.physicalDt+=1e-14;break;
    case 8:c.lastFrame.nextControlTime+=1e-14;break;case 9:c.lastControllerInputState.append('x');break;
    case 10:c.controllerState.append('x');break;case 11:c.fePhase=1;break;case 12:c.samplePhase=1;break;
    case 13:c.providerRevision="unexpected";break;case 14:c.sourceFingerprint=QString(64,'f');break;case 15:++c.completedControlTicks;break;
    case 16:c.heldValues[0]+=.01;c.lastAppliedValues[0]+=.01;break;case 17:r.externalForce[3]+=.01;break;
    case 18:r.memberInputValues[3]+=.01;break;case 19:r.portVoltages[1]+=.01;break;case 20:r.time+=1e-14;break;case 21:c.clear();break;}
    QVERIFY(rejected([&]{RStructuralTrussDynamicsRestartAdapter::restore(run.adapter(),r,c);}));
    QCOMPARE(combined(record(accepted),run.adapter().nativeControlSpec(),control(accepted)),baseline);
    accepted.getProblemSetup().getStructuralTrussDynamicsRestartData()=r;
    // Install corrupt fixture data without the setter's earlier structural
    // validation, so this assertion exercises the actual restore constructor.
    accepted.getProblemSetup().getStructuralNativeControlState()=c;
    accepted.getProblemSetup().setRestart(true);QVERIFY(rejected([&]{RStructuralTrussNativeRun invalid(accepted);}));
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::sourceIdentityRejection_data()
{
    QTest::addColumn<int>("mutation");const char *names[]={"provider-sha","provider-revision","provider-id","environment-sha",
        "different-valid-environment","missing-node","duplicate-sensor","duplicate-action","missing-loop","unsupported-action",
        "unsupported-measurement","wrong-port-mode","wrong-initial-voltage","wrong-initial-length","force-nonzero-initial",
        "nonempty-frozen-schedule","multirate","sample-phase","delay","hold","noise","free-support-reaction"};
    for(int i=0;i<22;++i)QTest::newRow(names[i])<<i;
}
void TestNativeControlRun::sourceIdentityRejection() try
{
    QFETCH(int,mutation);auto source=fixture();RStructuralTrussNativeRun frozen(source);auto bad=source;
    auto &s=bad.getProblemSetup().getStructuralNativeControlSpec();
    switch(mutation){case 0:s.implementationSha256=QString(64,'f');break;case 1:s.providerRevision="unexpected";break;
    case 2:s.providerId="other-provider";break;case 3:s.dependencyEnvironmentSha256=QString(64,'f');break;
    case 4:s.dependencyEnvironmentManifest.append("different-environment\n");s.dependencyEnvironmentSha256=digest(s.dependencyEnvironmentManifest);break;
    case 5:s.sensors[3].physicalNodeId=999;break;case 6:s.sensors.push_back(s.sensors[0]);break;
    case 7:s.actions.push_back(s.actions[0]);break;case 8:s.piLoops.pop_back();break;
    case 9:s.actions[0].quantity=static_cast<Action>(2);break;case 10:s.sensors[0].quantity=static_cast<Measurement>(99);break;
    case 11:s.actions[1].portId="Q-port";break;case 12:s.actions[1].initialCommand=.8;break;
    case 13:s.actions[3].initialCommand=.9;break;case 14:s.actions[0].initialCommand=.1;break;
    case 15:bad.getProblemSetup().getStructuralTrussDynamicsSetup().heldCommands={{1,{}, {}, {{202,{{0,0,0}}}}}};break;
    case 16:s.controlEveryFESteps=2;break;case 17:s.samplePhase=1;break;case 18:s.actions[0].delayTicks=1;break;
    case 19:s.actions[0].holdTicks=2;break;case 20:s.actions[0].noiseAmplitude=.01;break;case 21:s.sensors[5].physicalNodeId=202;break;}
    QVERIFY(rejected([&]{RStructuralTrussNativeRun invalid(bad);}));
    QVERIFY(rejected([&]{frozen.validateCurrentSource(bad);}));QCOMPARE(frozen.completedSteps(),0u);
    auto valid=frozen.prepare(source);frozen.validatePrepared(*valid);check(valid->candidateModel(),history());
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::freshRunIgnoresPreviousAcceptedHistory() try
{
    QTemporaryDir dir;const auto source=fixture();const auto values=history();const auto expected=reference(source,dir.filePath("reference.rbm"),values);
    auto old=source;RSolver oldSolver(old,dir.filePath("old.rbm"),{});oldSolver.setNativeMixedStepBudget(4);oldSolver.run();
    old.getProblemSetup().setRestart(false);
    // Keep a structurally valid old control record, but replace both opaque envelopes
    // with a malformed provider payload. A fresh run must not interpret it.
    old.getProblemSetup().getStructuralNativeControlState().controllerState="old-history";
    old.getProblemSetup().getStructuralNativeControlState().lastControllerInputState="old-before";
    const auto persisted=dir.filePath("old-history.rbm");old.write(persisted,false);RModel loaded;loaded.read(persisted);
    RStructuralTrussNativeRun fresh(loaded),clean(source);QCOMPARE(fresh.completedSteps(),0u);
    QCOMPARE(combined(RStructuralTrussDynamicsRestartAdapter::build(fresh.adapter(),fresh.acceptedState(),0,0,fresh.acceptedControlState()),
        fresh.adapter().nativeControlSpec(),fresh.acceptedControlState()),
        combined(RStructuralTrussDynamicsRestartAdapter::build(clean.adapter(),clean.acceptedState(),0,0,clean.acceptedControlState()),
        clean.adapter().nativeControlSpec(),clean.acceptedControlState()));
    RSolver solver(loaded,dir.filePath("fresh.rbm"),{});solver.run();check(loaded,values);QCOMPARE(combined(loaded),combined(expected));
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::realCliSplitResume_data()
{QTest::addColumn<bool>("binary");QTest::newRow("ram-four-actions")<<false;QTest::newRow("rbm-four-actions")<<true;}
void TestNativeControlRun::realCliSplitResume() try
{
    QFETCH(bool,binary);QVERIFY(QFileInfo(executable()).isFile());QTemporaryDir dir;const auto source=fixture();const auto values=history();
    const auto ext=RModel::getDefaultFileExtension(binary),fullInput=dir.filePath("full."+ext),splitInput=dir.filePath("split."+ext);
    source.write(fullInput,false);source.write(splitInput,false);const auto fullHash=fileDigest(fullInput),splitHash=fileDigest(splitInput),runtime=fileDigest(executable());
    const auto fullDir=dir.filePath("full-checkpoints"),splitDir=dir.filePath("split-checkpoints");
    const auto fullContext=context(fullHash,runtime),splitContext=context(splitHash,runtime);
    const auto full=cli(fullInput,fullDir,fullContext);QVERIFY2(full.code==0,full.diagnostics.constData());
    const auto split=cli(splitInput,splitDir,splitContext,4);QVERIFY2(split.code==0,split.diagnostics.constData());
    const auto checkpoint=latest(splitDir);QCOMPARE(record(checkpoint).completedSteps,4u);QCOMPARE(record(checkpoint).runTargetSteps,12u);
    QCOMPARE(checkpoint.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),12u);QCOMPARE(control(checkpoint).completedControlTicks,quint64(4));check(checkpoint,values);
    const auto recovery=latestPath(splitDir),recoveryHash=fileDigest(recovery);const auto resumed=cli(recovery,splitDir,splitContext,0,true);
    QVERIFY2(resumed.code==0,resumed.diagnostics.constData());const auto expected=latest(fullDir),actual=latest(splitDir);
    QCOMPARE(record(actual).completedSteps,12u);QCOMPARE(control(actual).completedControlTicks,quint64(12));
    QCOMPARE(combined(actual),combined(expected));check(expected,values);check(actual,values);
    QCOMPARE(fileDigest(recovery),recoveryHash);QCOMPARE(fileDigest(fullInput),fullHash);QCOMPARE(fileDigest(splitInput),splitHash);
} REPORT_CONTROL_EXCEPTIONS
void TestNativeControlRun::realCliRejectsTamperedRestart_data()
{
    QTest::addColumn<int>("mutation");QTest::newRow("provider-code")<<0;QTest::newRow("declared-environment")<<1;
    QTest::newRow("controller-envelope")<<2;QTest::newRow("action-request")<<3;QTest::newRow("left-frame")<<4;
    QTest::newRow("physical-input")<<5;
}
void TestNativeControlRun::realCliRejectsTamperedRestart() try
{
    QFETCH(int,mutation);QVERIFY(QFileInfo(executable()).isFile());QTemporaryDir dir;auto source=fixture();const auto input=dir.filePath("source.rbm");
    source.write(input,false);const auto sourceHash=fileDigest(input),checkpointDir=dir.filePath("checkpoints");const auto ctx=context(sourceHash,fileDigest(executable()));
    const auto split=cli(input,checkpointDir,ctx,4);QVERIFY2(split.code==0,split.diagnostics.constData());
    const auto oldLatest=raw(QDir(checkpointDir).filePath("latest.json"));
    const auto lastValidPath=QDir(checkpointDir).filePath("lastvalid.json");
    const bool oldLastExists=QFileInfo::exists(lastValidPath);
    const auto oldLast=oldLastExists?raw(lastValidPath):QByteArray();
    auto bad=latest(checkpointDir);const auto baseline=combined(bad);auto &c=bad.getProblemSetup().getStructuralNativeControlState();
    auto &s=bad.getProblemSetup().getStructuralNativeControlSpec();
    switch(mutation){case 0:s.implementationSha256=QString(64,'f');break;
    case 1:s.dependencyEnvironmentManifest.append("different-environment\n");s.dependencyEnvironmentSha256=digest(s.dependencyEnvironmentManifest);break;
    case 2:c.controllerState.append('x');break;
    case 3:c.lastActions[0].requested+=.01;c.lastActions[0].conditioned+=.01;break;
    case 4:c.lastFrame.samples[3].value+=.01;break;case 5:bad.getProblemSetup().getStructuralTrussDynamicsRestartData().externalForce[3]+=.01;break;}
    const auto modified=bad.write(dir.filePath("tampered.rbm"),false);
    const auto modifiedHash=fileDigest(modified);
    const auto result=cli(modified,checkpointDir,ctx,0,true);QVERIFY2(result.code!=0,result.diagnostics.constData());
    QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),oldLatest);
    QCOMPARE(QFileInfo::exists(lastValidPath),oldLastExists);
    if(oldLastExists)QCOMPARE(raw(lastValidPath),oldLast);
    QCOMPARE(combined(latest(checkpointDir)),baseline);QCOMPARE(fileDigest(input),sourceHash);QCOMPARE(fileDigest(modified),modifiedHash);
} REPORT_CONTROL_EXCEPTIONS
#undef REPORT_CONTROL_EXCEPTIONS
QTEST_GUILESS_MAIN(TestNativeControlRun)
#include "tst_native_control_run.moc"
