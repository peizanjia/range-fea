#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <rbl_error.h>
#include <rml_model.h>
#include <rml_file_utils.h>
#include <rml_monitoring_point.h>
#include <rsolver.h>
#include <rstructural_truss_dynamics_model_adapter.h>
#include <rstructural_truss_dynamics_restart_adapter.h>
#include <rstructural_truss_native_run.h>
#include "native_accepted_checkpoint.h"

namespace {
using Store=NativeAcceptedCheckpointStore;using Phase=Store::Phase;using Record=RStructuralTrussDynamicsRestartData;
void set(RBoundaryCondition &bc,RVariableType type,double v,bool enabled=true)
{auto &c=bc.getComponent(bc.findComponentPosition(type));c.setEnabled(enabled);c.clear();if(enabled)c.add(0,v);}
RBoundaryCondition support(bool x)
{RBoundaryCondition bc(R_BOUNDARY_CONDITION_DISPLACEMENT);set(bc,R_VARIABLE_DISPLACEMENT_X,0,x);
 set(bc,R_VARIABLE_DISPLACEMENT_Y,0);set(bc,R_VARIABLE_DISPLACEMENT_Z,0);return bc;}
RModel fixture(double dt=.0034567890123456789,bool stationary=false)
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
    for(uint i=0;i<3;++i){const RVariableType qt[]={R_VARIABLE_DISPLACEMENT_X,R_VARIABLE_DISPLACEMENT_Y,R_VARIABLE_DISPLACEMENT_Z};
        const RVariableType vt[]={R_VARIABLE_VELOCITY_X,R_VARIABLE_VELOCITY_Y,R_VARIABLE_VELOCITY_Z};
        auto &qc=q.getComponent(q.findComponentPosition(qt[i]));qc.setEnabled(true);qc.clear();qc.add(0,(!stationary && i==0)?.035:0);
        auto &vc=v.getComponent(v.findComponentPosition(vt[i]));vc.setEnabled(true);vc.clear();vc.add(0,(!stationary && i==0)?.08:0);}
    m.getPoint(1).addInitialCondition(q);m.getPoint(1).addInitialCondition(v);
    RBoundaryCondition load(R_BOUNDARY_CONDITION_FORCE);set(load,R_VARIABLE_FORCE_X,stationary?0:1.2);
    set(load,R_VARIABLE_FORCE_Y,0,false);set(load,R_VARIABLE_FORCE_Z,0,false);m.getPoint(1).addBoundaryCondition(load);
    for(uint i=0;i<m.getNElementGroups();++i){auto *g=m.getElementGroupPtr(i);
        if(!g || !g->hasEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION))continue;
        auto &gravity=g->getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        for(auto type:{R_VARIABLE_G_ACCELERATION_X,R_VARIABLE_G_ACCELERATION_Y,R_VARIABLE_G_ACCELERATION_Z}){
            auto &c=gravity.getComponent(gravity.findComponentPosition(type));c.setEnabled(true);c.clear();c.add(0,0);}}
    m.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));auto &d=m.getProblemSetup().getStructuralDynamicsSetup();
    d.setEnabled(true);d.setTimeStep(dt);d.setNSteps(12);d.setOutputFrequency(1);
    auto &clock=m.getTimeSolver();clock.setEnabled(true);clock.setInputStartTime(dt);clock.setInputTimeStepSize(dt);clock.setInputNTimeSteps(12);clock.setOutputFrequency(1);
    std::vector<double> times;for(uint n=1;n<=12;++n)times.push_back(double(n)*dt);clock.setTimes(times);
    auto &s=m.getProblemSetup().getStructuralTrussDynamicsSetup();s.enabled=true;s.passiveMembers={{901,1}};
    s.absoluteForceTolerance=1e-10;s.relativeForceTolerance=1e-10;s.linearRelativeTolerance=1e-12;
    for(uint i=0;i<2;++i){RStructuralTrussActiveMember a;a.physicalElementId=ids[i+1];a.portId=i==0?"V-port":"Q-port";
        a.referenceLength=1;a.axialStiffness=10;a.coupling=.2;a.capacitance=.5;a.initialValue=stationary?0:(i==0?.7:.15);
        a.kind=i==0?RStructuralTrussElectricalKind::Voltage:RStructuralTrussElectricalKind::ConductorCharge;s.activeMembers.push_back(a);}
    s.cableMembers={{609,1,stationary?0.0:1.0},{811,3,0}};
    if(!stationary)s.heldCommands={{1,{{"Q-port",.2},{"V-port",.8}},{{609,.99}},{{202,{{.4,0,0}}}}},
        {3,{},{},{{202,{{2,0,0}}}}},{5,{{"V-port",-.3},{"Q-port",-.1}},{{609,1.02}},{} }};
    s.bindToModel(m);return m;
}

RModel movingAnchorFixture()
{
    auto m=fixture();const double dt=m.getProblemSetup().getStructuralDynamicsSetup().getTimeStep();
    const double t0=m.getTimeSolver().getInputStartTime()-dt,finish=t0+12*dt,duration=finish-t0;
    m.getProblemSetup().getStructuralInteractionSetup().getPrescribedMotions().push_back(
        {"quadratic moving anchor",{0,RDofType::UX},{{t0,0,0,.4},{finish,.2*duration*duration,.4*duration,.4}}});
    auto &setup=m.getProblemSetup().getStructuralTrussDynamicsSetup();
    setup.heldCommands.push_back({7,{},{{609,1.2}}, {}});
    setup.heldCommands.push_back({9,{},{{609,.98}}, {}});
    setup.bindToModel(m);return m;
}
// Entirely independent scalar mechanics: no production assembly, motion evaluator,
// dynamics driver, or restart builder supplies this physical/work reference.
struct AnchorReference {
    double q=.035,v=.08,a=0,g=0,vg=0,voltage=.7,charge=.15,length=1,force=1.2;
    double cable=0,stored=0,kinetic=0,reaction=0,initial=0,external=0,support=0,electrical=0,restLength=0;
};
constexpr double anchorMass=2.5;
double anchorCable(double delta,double length)
{return std::max(0.0,1+(10/length)*(1+delta-length));}
double anchorCableEnergy(double delta,double length)
{const double n=anchorCable(delta,length);return .5*n*n/(10/length);}
double anchorElectricalEnergy(double delta,double voltage,double charge)
{return .25*voltage*voltage+.25*std::pow((charge+.2*delta)/.5,2);}
void observeAnchor(AnchorReference &s)
{
    const double delta=s.q-s.g;s.cable=anchorCable(delta,s.length);
    const double f=30.08*delta+.2*s.voltage+.4*s.charge+s.cable;
    s.a=(s.force-f)/anchorMass;s.reaction=anchorMass*.4-f;
    s.stored=.5*30*delta*delta+anchorElectricalEnergy(delta,s.voltage,s.charge)+anchorCableEnergy(delta,s.length);
    s.kinetic=.5*anchorMass*(s.v*s.v+s.vg*s.vg);
}
std::vector<AnchorReference> anchorHistory(double dt,double t0)
{
    std::vector<AnchorReference> history;AnchorReference state;observeAnchor(state);
    state.initial=state.stored+state.kinetic;history.push_back(state);
    for(uint step=1;step<=12;++step){
        const double oldDelta=state.q-state.g,oldVoltage=state.voltage,oldCharge=state.charge,oldLength=state.length;
        if(step==1){state.voltage=.8;state.charge=.2;state.length=.99;state.force=.4;}
        if(step==3)state.force=2;
        if(step==5){state.voltage=-.3;state.charge=-.1;state.length=1.02;}
        if(step==7)state.length=1.2;
        if(step==9)state.length=.98;
        state.electrical+=anchorElectricalEnergy(oldDelta,state.voltage,state.charge)-anchorElectricalEnergy(oldDelta,oldVoltage,oldCharge);
        state.restLength+=anchorCableEnergy(oldDelta,state.length)-anchorCableEnergy(oldDelta,oldLength);
        observeAnchor(state); // Right side of any held-command jump, at fixed geometry.
        const auto start=state;const double time=t0+double(step)*dt,previous=t0+double(step-1)*dt;
        const double h=time-previous,tau=time-t0,g=.2*tau*tau,vg=.4*tau;
        const double predictor=start.q+h*start.v+.25*h*h*start.a,coefficient=4*anchorMass/(h*h);
        // Solve both affine branches and require exactly one self-consistent root.
        double next=0;uint roots=0;
        for(bool taut:{false,true}){
            const double k=30.08+(taut?10/state.length:0);
            const double b=.2*state.voltage+.4*state.charge+(taut?1+10/state.length*(1-state.length):0);
            const double candidate=(coefficient*predictor+state.force+k*g-b)/(coefficient+k);
            const bool candidateTaut=1+10/state.length*(1+candidate-g-state.length)>0;
            if(candidateTaut==taut){next=candidate;++roots;}
        }
        if(roots!=1)throw std::runtime_error("Independent moving-anchor branch root is ambiguous");
        state.q=next;state.g=g;state.vg=vg;observeAnchor(state);
        state.v=start.v+.5*h*(start.a+state.a);observeAnchor(state);
        const bool expectedTaut=step<7 || step>=9;
        if((anchorCable(oldDelta,state.length)>0)!=expectedTaut || (state.cable>0)!=expectedTaut)
            throw std::runtime_error("Independent cable609 left/right taut-slack-retension schedule disagrees");
        if((10.0/3)*(1+oldDelta-3)>0 || (10.0/3)*(1+state.q-state.g-3)>0)
            throw std::runtime_error("Independent cable811 must remain slack at both interval ends");
        state.external+=state.force*(state.q-start.q);
        state.support+=.5*h*(start.reaction*start.vg+state.reaction*state.vg);
        state.electrical+=state.voltage*(-.2)*((state.q-state.g)-oldDelta);
        history.push_back(state);
    }
    return history;
}
void checkAnchorValue(double actual,double expected,double tolerance,const char *field,uint step)
{
    if(!std::isfinite(actual) || std::abs(actual-expected)>tolerance)
        throw std::runtime_error(qPrintable(QString("Moving anchor step %1 %2: actual %3, independent %4 (tolerance %5)")
            .arg(step).arg(QString::fromLatin1(field)).arg(actual,0,'g',17).arg(expected,0,'g',17).arg(tolerance,0,'g',8)));
}
void checkAnchorRecord(const RModel &m,const std::vector<AnchorReference> &history)
{
    const auto &r=m.getProblemSetup().getStructuralTrussDynamicsRestartData();const uint step=r.completedSteps;
    if(!r.available || step>=history.size() || r.q.size()!=6 || r.v.size()!=6 || r.a.size()!=6 || r.reactions.size()!=6)
        throw std::runtime_error("Moving anchor accepted record dimensions unavailable");
    const auto &e=history[step];
    checkAnchorValue(r.q[0],e.g,2e-12,"anchor displacement",step);
    checkAnchorValue(r.v[0],e.vg,2e-11,"anchor velocity",step);
    checkAnchorValue(r.a[0],.4,2e-10,"anchor acceleration",step);
    checkAnchorValue(r.q[3],e.q,2e-11,"free displacement",step);
    checkAnchorValue(r.v[3],e.v,2e-10,"free velocity",step);
    checkAnchorValue(r.a[3],e.a,2e-9,"free acceleration",step);
    checkAnchorValue(r.reactions[0],e.reaction,2e-9,"support reaction",step);
    checkAnchorValue(r.reactions[3],0,2e-9,"free residual reaction",step);
    for(uint dof:{1u,2u,4u,5u}){
        checkAnchorValue(r.q[dof],0,2e-12,"transverse displacement",step);
        checkAnchorValue(r.v[dof],0,2e-11,"transverse velocity",step);
        checkAnchorValue(r.a[dof],0,2e-10,"transverse acceleration",step);
        checkAnchorValue(r.reactions[dof],0,2e-9,"transverse reaction",step);
    }
    checkAnchorValue(r.storedEnergy,e.stored,2e-10,"stored energy",step);
    checkAnchorValue(r.kineticEnergy,e.kinetic,2e-10,"kinetic energy",step);
    checkAnchorValue(r.initialTotalEnergy,e.initial,2e-10,"initial total energy",step);
    checkAnchorValue(r.cumulativeExternalWork,e.external,2e-10,"external work",step);
    checkAnchorValue(r.cumulativeSupportWork,e.support,2e-10,"support work",step);
    checkAnchorValue(r.cumulativeElectricalWork,e.electrical,2e-10,"electrical work",step);
    checkAnchorValue(r.cumulativeRestLengthWork,e.restLength,2e-10,"rest-length work",step);
    const uint stress=m.findVariable(R_VARIABLE_STRESS_NORMAL);
    if(stress==RConstants::eod)throw std::runtime_error("Moving anchor cable axial display unavailable");
    checkAnchorValue(m.getVariable(stress).getValue(0,3),e.cable,2e-9,"cable609 axial force display",step);
    checkAnchorValue(m.getVariable(stress).getValue(0,4),0,2e-9,"cable811 slack axial force display",step);
}

QByteArray raw(const QString &path)
{QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read test artifact");return f.readAll();}
void write(const QString &path,const QByteArray &bytes)
{QFile f(path);if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate)||f.write(bytes)!=bytes.size())throw std::runtime_error("Cannot write test artifact");}
QString digest(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
QString fileDigest(const QString &p){return digest(raw(p));}
QByteArray recordBytes(const Record &r)
{
    // Independent, bit-exact witness of every record field; not production FileIO or stateFingerprint.
    QByteArray b;QDataStream out(&b,QIODevice::WriteOnly);
    const auto real=[&](double x){quint64 bits;std::memcpy(&bits,&x,sizeof(bits));out<<bits;};
    const auto vec=[&](const std::vector<double>&v){out<<quint64(v.size());for(double x:v)real(x);};
    const auto ids=[&](const std::vector<uint>&v){out<<quint64(v.size());for(uint x:v)out<<x;};
    out<<r.available<<r.completedSteps<<r.runTargetSteps<<r.nextCommandIndex;real(r.initialTime);real(r.time);real(r.fixedTimeStep);
    ids(r.orderedPhysicalNodeIds);ids(r.orderedMemberPhysicalIds);out<<quint64(r.orderedPortIds.size());for(const auto &id:r.orderedPortIds)out<<id;
    out<<r.sourceFingerprint<<r.systemIdentity;vec(r.q);vec(r.v);vec(r.a);vec(r.reactions);vec(r.externalForce);ids(r.memberInputKinds);vec(r.memberInputValues);
    vec(r.portVoltages);vec(r.portCharges);vec(r.portEnthalpies);real(r.storedEnergy);real(r.kineticEnergy);real(r.initialTotalEnergy);
    real(r.cumulativeExternalWork);real(r.cumulativeSupportWork);real(r.cumulativeElectricalWork);real(r.cumulativeRestLengthWork);return b;
}
const Record &record(const RModel &m){return m.getProblemSetup().getStructuralTrussDynamicsRestartData();}
NativeAcceptedCheckpointStateView view(const RModel &m)
{
    RStructuralTrussDynamicsModelAdapter a(m);auto r=record(m);
    if(r.available)RStructuralTrussDynamicsRestartAdapter::restore(a,r);
    else r=RStructuralTrussDynamicsRestartAdapter::build(a,a.initialize(),0,0);
    return {r.completedSteps,r.time,r.fixedTimeStep,QCryptographicHash::hash(recordBytes(r),QCryptographicHash::Sha256)};
}
QByteArray context(const QString &inputHash,const QString &runtimeHash,double dt)
{
    return QJsonDocument(QJsonObject{{"schema","range.native-checkpoint-context/v1"},{"sourceSha256",inputHash},{"modelSha256",inputHash},
        {"controlSha256",NativeAcceptedCheckpointContext::noControlSha256()},{"runtimeSha256",runtimeHash},
        {"packageSha256",QString(64,'d')},{"policy","accepted-steps-or-sim-time-v1"},
        {"maxAcceptedSteps",10},{"maxSimulationSeconds",1.0},{"dtS",dt}}).toJson(QJsonDocument::Compact);
}
std::shared_ptr<Store> participant(const QString &dir,const RModel &initial)
{
    const QString input=QString(64,'a'),runtime=QString(64,'c');
    return std::make_shared<Store>(dir,context(input,runtime,initial.getProblemSetup().getStructuralDynamicsSetup().getTimeStep()),
        input,NativeAcceptedCheckpointContext::noControlSha256(),runtime,false,initial,view);
}
QString failure(RSolver &s)
{try{s.run();return {};}catch(const RError &e){return e.getMessage();}catch(const std::exception &e){return QString::fromUtf8(e.what());}}
template<class F> bool rejected(F f)
{try{f();}catch(const RError &){return true;}catch(const std::exception &){return true;}return false;}
Record reference(const RModel &source,const QString &path)
{auto m=source;RSolver solver(m,path,{});solver.run();return record(m);}
RModel latest(const QString &dir)
{
    const auto pointer=QJsonDocument::fromJson(raw(QDir(dir).filePath("latest.json"))).object();
    const auto envelopePath=QDir(dir).filePath(pointer.value("envelopeFile").toString());
    if(fileDigest(envelopePath)!=pointer.value("envelopeSha256").toString())throw std::runtime_error("Envelope hash mismatch");
    const auto envelope=QJsonDocument::fromJson(raw(envelopePath)).object();const auto path=QDir(dir).filePath(envelope.value("modelFile").toString());
    if(fileDigest(path)!=envelope.value("checkpointModelSha256").toString())throw std::runtime_error("MODEL hash mismatch");
    RModel m;m.read(path);return m;
}
QString latestPath(const QString &dir)
{const auto p=QJsonDocument::fromJson(raw(QDir(dir).filePath("latest.json"))).object();
 const auto e=QJsonDocument::fromJson(raw(QDir(dir).filePath(p.value("envelopeFile").toString()))).object();return QDir(dir).filePath(e.value("modelFile").toString());}
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
    QStringList args={"--file="+input,"--native-checkpoint-dir="+dir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx)};
    args.append("--native-mixed-steps="+QString::number(budget));if(restart)args.append("--restart");
    QProcess process;process.setProcessChannelMode(QProcess::MergedChannels);process.start(executable(),args);
    if(!process.waitForStarted(10000))throw std::runtime_error(qPrintable(process.errorString()));
    if(!process.waitForFinished(60000)){process.kill();process.waitForFinished(5000);throw std::runtime_error("Native mixed CLI timed out");}
    const auto out=process.readAll();if(process.exitStatus()!=QProcess::NormalExit)throw std::runtime_error(out.constData());return {process.exitCode(),out};
}
}
#define REPORT_RUN_EXCEPTIONS \
    catch(const RError &e){QFAIL(qPrintable(e.getMessage()));} \
    catch(const std::exception &e){QFAIL(e.what());} \
    catch(...){QFAIL("Unknown native mixed run exception");}
class TestNativeMixedRun:public QObject {
    Q_OBJECT
private slots:
    void sameSolverBudgetAndIndependentPhysics();
    void movingAnchorIndependentPhysics();
    void movingAnchorPublicationRollback();
    void realCliMovingAnchor_data();void realCliMovingAnchor();
    void nonlinearNewtonFailureKeepsInitialState();
    void preparedCandidateTamper_data();void preparedCandidateTamper();
    void checkpointFailures_data();void checkpointFailures();
    void outputFailures_data();void outputFailures();
    void postCommitObserverContinues();
    void failedRollbackPoisonsSolver();
    void configuredMonitoringAndUpfrontRejection();
    void legacyCallbacksRejected();
    void realCliSplitResume_data();void realCliSplitResume();
    void realCliRejectsReservedSource_data();void realCliRejectsReservedSource();
    void realCliRejectsTamperedRestart_data();void realCliRejectsTamperedRestart();
};
void TestNativeMixedRun::sameSolverBudgetAndIndependentPhysics() try
{
    QTemporaryDir dir;auto source=fixture();const auto expected=reference(source,dir.filePath("continuous.rbm"));auto m=source;
    RSolver solver(m,dir.filePath("split.rbm"),{});solver.setNativeMixedStepBudget(4);std::vector<uint> observed;
    solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){observed.push_back(record(accepted).completedSteps);});
    solver.run();QCOMPARE(record(m).completedSteps,4u);QCOMPARE(record(m).runTargetSteps,12u);
    QCOMPARE(m.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),12u);QCOMPARE(record(m).nextCommandIndex,2u);
    solver.run();QCOMPARE(record(m).completedSteps,8u);solver.setNativeMixedStepBudget(0);solver.run();
    QCOMPARE(recordBytes(record(m)),recordBytes(expected));QCOMPARE(observed.size(),size_t(12));
    for(uint i=0;i<12;++i)QCOMPARE(observed[i],i+1);
    // Independent one-free-DOF Newmark recurrence. Both cables are classified;
    // the second stays slack and contributes mass but no force or stiffness.
    double q=.035,v=.08,voltage=.7,charge=.15,length=1,force=1.2;
    constexpr double mass=2.5;const double dt=source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep();
    for(uint step=1;step<=12;++step){if(step==1){voltage=.8;charge=.2;length=.99;force=.4;}if(step==3)force=2;
        if(step==5){voltage=-.3;charge=-.1;length=1.02;}
        const double k=30.08+10/length,offset=.2*voltage+.4*charge+1+10/length*(1-length);
        const double a=(force-offset-k*q)/mass,h=double(step)*dt-double(step-1)*dt;
        const double predictor=q+h*v+.25*h*h*a;
        const double next=(4*mass/(h*h)*predictor+force-offset)/(4*mass/(h*h)+k);
        const double nextA=(force-offset-k*next)/mass;v+=.5*h*(a+nextA);q=next;}
    QVERIFY(std::abs(record(m).q[3]-q)<2e-11);QVERIFY(std::abs(record(m).v[3]-v)<2e-10);
    QCOMPARE(record(m).memberInputKinds,std::vector<uint>({0,1,2,3,3}));QCOMPARE(record(m).orderedPortIds,std::vector<QString>({"V-port","Q-port"}));
} REPORT_RUN_EXCEPTIONS

void TestNativeMixedRun::movingAnchorIndependentPhysics() try
{
    QTemporaryDir dir;auto source=movingAnchorFixture();const double dt=source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep();
    const double t0=source.getTimeSolver().getInputStartTime()-dt;const auto independent=anchorHistory(dt,t0);
    // Explicit initial energies are computed from physical initial q/v and source inputs.
    const auto &initial=independent.front();
    checkAnchorValue(initial.kinetic,.5*anchorMass*.08*.08,1e-15,"reference initial kinetic",0);
    const RStructuralTrussNativeRun initialized(source);const auto &initialActual=initialized.acceptedState();
    checkAnchorValue(initialActual.storedEnergy,initial.stored,2e-10,"initial stored energy",0);
    checkAnchorValue(initialActual.kineticEnergy,initial.kinetic,2e-10,"initial kinetic energy",0);
    checkAnchorValue(initialActual.initialTotalEnergy,initial.initial,2e-10,"initial total energy",0);
    auto m=source;RSolver solver(m,dir.filePath("moving.rbm"),{});std::vector<uint> observed;
    solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){checkAnchorRecord(accepted,independent);
        observed.push_back(record(accepted).completedSteps);});
    solver.run();QCOMPARE(observed.size(),size_t(12));for(uint i=0;i<12;++i)QCOMPARE(observed[i],i+1);
    QCOMPARE(record(m).runTargetSteps,12u);QCOMPARE(record(m).nextCommandIndex,5u);
    QCOMPARE(record(m).orderedPhysicalNodeIds,std::vector<uint>({101,202}));
    QCOMPARE(record(m).orderedMemberPhysicalIds,std::vector<uint>({901,503,777,609,811}));
    QCOMPARE(record(m).memberInputKinds,std::vector<uint>({0,1,2,3,3}));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::movingAnchorPublicationRollback() try
{
    QTemporaryDir dir;auto source=movingAnchorFixture();const auto expected=reference(source,dir.filePath("reference.rbm"));auto m=source;
    const auto output=dir.filePath("actual.rbm"),checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m);
    const auto sourceFingerprint=source.getProblemSetup().getStructuralTrussDynamicsSetup().computeSourceFingerprint(source);
    RSolver solver(m,output,{});solver.setAcceptedStepParticipant(store);solver.setNativeMixedStepBudget(1);
    solver.run(); // The attempted accepted step 2 fails after publishing latest.
    const auto baseline=recordBytes(record(m)),oldLatest=raw(QDir(checkpointDir).filePath("latest.json"));
    const auto lastValidPath=QDir(checkpointDir).filePath("lastvalid.json");
    const bool oldLastExists=QFileInfo::exists(lastValidPath);
    const auto oldLast=oldLastExists?raw(lastValidPath):QByteArray(),oldLogical=raw(output);
    const auto recoveryPath=latestPath(checkpointDir),recoveryHash=fileDigest(recoveryPath);
    const double oldClock=m.getTimeSolver().getComputedTime();
    store->setPhaseHook(Phase::AfterLatest,[](Phase){throw std::runtime_error("moving anchor accepted-2 AfterLatest publication failure");});
    const auto diagnostic=failure(solver);QVERIFY2(diagnostic.contains("accepted-2"),qPrintable(diagnostic));
    QCOMPARE(recordBytes(record(m)),baseline);QCOMPARE(record(m).completedSteps,1u);
    QCOMPARE(recordBytes(record(latest(checkpointDir))),baseline);
    QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),oldLatest);
    QCOMPARE(QFileInfo::exists(lastValidPath),oldLastExists);
    if(oldLastExists)QCOMPARE(raw(lastValidPath),oldLast);
    QCOMPARE(raw(output),oldLogical);
    QCOMPARE(fileDigest(recoveryPath),recoveryHash);QCOMPARE(m.getTimeSolver().getComputedTime(),oldClock);
    QCOMPARE(m.getProblemSetup().getStructuralTrussDynamicsSetup().computeSourceFingerprint(m),sourceFingerprint);
    QVERIFY(!QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(output,2)));
    QVERIFY(solver.isNativeMixedRetryable());QVERIFY(store->isRetryable());
    solver.run();QCOMPARE(record(m).completedSteps,2u);solver.setNativeMixedStepBudget(0);solver.run();
    QCOMPARE(recordBytes(record(m)),recordBytes(expected));QCOMPARE(recordBytes(record(latest(checkpointDir))),recordBytes(expected));
    QCOMPARE(fileDigest(recoveryPath),recoveryHash);
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::realCliMovingAnchor_data()
{QTest::addColumn<bool>("binary");QTest::newRow("moving-anchor-ram")<<false;QTest::newRow("moving-anchor-rbm")<<true;}
void TestNativeMixedRun::realCliMovingAnchor() try
{
    QFETCH(bool,binary);QVERIFY(QFileInfo(executable()).isFile());QTemporaryDir dir;auto source=movingAnchorFixture();
    const double dt=source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep(),t0=source.getTimeSolver().getInputStartTime()-dt;
    const auto independent=anchorHistory(dt,t0);const auto ext=RModel::getDefaultFileExtension(binary);
    const auto fullInput=dir.filePath("moving-full."+ext),splitInput=dir.filePath("moving-split."+ext);
    source.write(fullInput,false);source.write(splitInput,false);
    const auto fullHash=fileDigest(fullInput),splitHash=fileDigest(splitInput),runtime=fileDigest(executable());
    const auto fullDir=dir.filePath("full-checkpoints"),splitDir=dir.filePath("split-checkpoints");
    const auto fullContext=context(fullHash,runtime,dt),splitContext=context(splitHash,runtime,dt);
    const auto full=cli(fullInput,fullDir,fullContext);QVERIFY2(full.code==0,full.diagnostics.constData());
    const auto split=cli(splitInput,splitDir,splitContext,4);QVERIFY2(split.code==0,split.diagnostics.constData());
    const auto checkpoint=latest(splitDir);QCOMPARE(record(checkpoint).completedSteps,4u);
    QCOMPARE(record(checkpoint).runTargetSteps,12u);QCOMPARE(checkpoint.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),12u);
    QCOMPARE(record(checkpoint).nextCommandIndex,2u);checkAnchorRecord(checkpoint,independent);
    const auto recoveryPath=latestPath(splitDir),recoveryHash=fileDigest(recoveryPath);
    const auto resumed=cli(recoveryPath,splitDir,splitContext,0,true);QVERIFY2(resumed.code==0,resumed.diagnostics.constData());
    const auto expected=latest(fullDir),actual=latest(splitDir);
    QCOMPARE(record(actual).completedSteps,12u);QCOMPARE(record(actual).runTargetSteps,12u);
    QCOMPARE(recordBytes(record(actual)),recordBytes(record(expected)));checkAnchorRecord(expected,independent);checkAnchorRecord(actual,independent);
    QCOMPARE(fileDigest(recoveryPath),recoveryHash);QCOMPARE(fileDigest(fullInput),fullHash);QCOMPARE(fileDigest(splitInput),splitHash);
} REPORT_RUN_EXCEPTIONS

void TestNativeMixedRun::nonlinearNewtonFailureKeepsInitialState() try
{
    QTemporaryDir dir;auto m=fixture(.1);m.getNode(1).setY(1);
    auto &setup=m.getProblemSetup().getStructuralTrussDynamicsSetup();
    const double chord=std::sqrt(2.0);
    setup.passiveMembers[0].referenceLength=chord;
    for(auto &member:setup.activeMembers) member.referenceLength=chord;
    setup.cableMembers[0].initialReferenceLength=chord;
    setup.heldCommands.clear();setup.maxIterations=1;setup.bindToModel(m);
    const auto sourceFingerprint=setup.computeSourceFingerprint(m);
    const auto initialRecord=recordBytes(record(m));
    const double initialClock=m.getTimeSolver().getComputedTime();
    const auto output=dir.filePath("failed.rbm"),checkpointDir=dir.filePath("checkpoints");
    auto store=participant(checkpointDir,m);RSolver solver(m,output,{});
    solver.setAcceptedStepParticipant(store);
    for(int retry=0;retry<2;++retry)
    {
        const auto diagnostic=failure(solver);
        QVERIFY2(diagnostic.contains("Newton iteration limit"),qPrintable(diagnostic));
        QVERIFY(solver.isNativeMixedRetryable());QVERIFY(store->isRetryable());
        QCOMPARE(recordBytes(record(m)),initialRecord);
        QCOMPARE(setup.computeSourceFingerprint(m),sourceFingerprint);
        QCOMPARE(m.getTimeSolver().getComputedTime(),initialClock);
        QVERIFY(!QFileInfo::exists(output));
        QVERIFY(!QFileInfo::exists(QDir(checkpointDir).filePath("latest.json")));
    }
} REPORT_RUN_EXCEPTIONS

void TestNativeMixedRun::preparedCandidateTamper_data()
{
    QTest::addColumn<int>("mutation");QTest::newRow("result-vector")<<0;QTest::newRow("display-metadata")<<1;
    QTest::newRow("restart-record")<<2;QTest::newRow("accepted-clock")<<3;
}
void TestNativeMixedRun::preparedCandidateTamper() try
{
    QFETCH(int,mutation);auto source=fixture();RStructuralTrussNativeRun run(source);
    const auto initial=recordBytes(RStructuralTrussDynamicsRestartAdapter::build(run.adapter(),run.acceptedState(),0,0));
    auto prepared=run.prepare(source);auto &candidate=prepared->candidateModel();
    auto &displacement=candidate.getVariable(candidate.findVariable(R_VARIABLE_DISPLACEMENT));
    switch(mutation){case 0:displacement.setValue(0,1,999);break;
    case 1:displacement.getVariableData().setMinMaxDisplayValue(0,999);break;
    case 2:candidate.getProblemSetup().getStructuralTrussDynamicsRestartData().portEnthalpies[0]+=.01;break;
    case 3:candidate.getTimeSolver().setComputedTime(candidate.getTimeSolver().getComputedTime()+1e-14);break;}
    QVERIFY(rejected([&]{run.validatePrepared(*prepared);}));QCOMPARE(run.completedSteps(),0u);QCOMPARE(run.nextCommandIndex(),0u);
    QCOMPARE(recordBytes(RStructuralTrussDynamicsRestartAdapter::build(run.adapter(),run.acceptedState(),0,0)),initial);
    auto retry=run.prepare(source);run.validatePrepared(*retry);run.commit(*retry);QCOMPARE(run.completedSteps(),1u);QCOMPARE(run.nextCommandIndex(),1u);
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::checkpointFailures_data()
{
    QTest::addColumn<int>("phase");const Phase phases[]={Phase::BeforeModelWrite,Phase::AfterModelReadBack,Phase::BeforeEnvelopeWrite,
        Phase::AfterEnvelopeReadBack,Phase::BeforeLastValid,Phase::AfterLastValid,Phase::BeforeLatest,Phase::AfterLatest,Phase::AfterLatestReadBack};
    for(int i=0;i<9;++i)QTest::newRow(qPrintable(QString("phase-%1").arg(i)))<<int(phases[i]);
}
void TestNativeMixedRun::checkpointFailures() try
{
    QFETCH(int,phase);QTemporaryDir dir;auto source=fixture();const auto expected=reference(source,dir.filePath("reference.rbm"));auto m=source;
    const auto checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m);
    RSolver solver(m,dir.filePath("actual.rbm"),{});solver.setAcceptedStepParticipant(store);solver.setNativeMixedStepBudget(1);solver.run();solver.run();
    const auto baseline=recordBytes(record(m)),oldLatest=raw(QDir(checkpointDir).filePath("latest.json")),oldLast=raw(QDir(checkpointDir).filePath("lastvalid.json"));
    const auto oldLogical=raw(dir.filePath("actual.rbm"));
    store->setPhaseHook(static_cast<Phase>(phase),[](Phase){throw std::runtime_error("actual RSolver checkpoint failure");});
    QVERIFY(!failure(solver).isEmpty());QCOMPARE(recordBytes(record(m)),baseline);QCOMPARE(record(m).completedSteps,2u);
    QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),oldLatest);QCOMPARE(raw(QDir(checkpointDir).filePath("lastvalid.json")),oldLast);
    QCOMPARE(raw(dir.filePath("actual.rbm")),oldLogical);QVERIFY(!QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(dir.filePath("actual.rbm"),3)));
    QVERIFY(solver.isNativeMixedRetryable());QVERIFY(store->isRetryable());solver.run();QCOMPARE(record(m).completedSteps,3u);
    solver.setNativeMixedStepBudget(0);solver.run();QCOMPARE(recordBytes(record(m)),recordBytes(expected));
    QCOMPARE(recordBytes(record(latest(checkpointDir))),recordBytes(expected));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::outputFailures_data()
{QTest::addColumn<bool>("logical");QTest::newRow("record-directory-collision")<<false;QTest::newRow("link-directory-collision")<<true;}
void TestNativeMixedRun::outputFailures() try
{
    QFETCH(bool,logical);QTemporaryDir dir;auto source=fixture();const auto expected=reference(source,dir.filePath("reference.rbm"));auto m=source;
    const auto output=dir.filePath("actual.rbm"),checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m);
    RSolver solver(m,output,{});solver.setAcceptedStepParticipant(store);solver.setNativeMixedStepBudget(1);solver.run();
    const auto baseline=recordBytes(record(m)),pointer=raw(QDir(checkpointDir).filePath("latest.json"));
    const auto target=logical?RFileUtils::getFileNameWithOutTimeStep(output):RFileUtils::getFileNameWithTimeStep(output,2);
    const auto oldLink=logical?raw(target):QByteArray();if(logical)QVERIFY(QFile::remove(target));QVERIFY(QDir().mkpath(target));
    QVERIFY(!failure(solver).isEmpty());QCOMPARE(recordBytes(record(m)),baseline);QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),pointer);
    QVERIFY(solver.isNativeMixedRetryable());QVERIFY(QDir().rmdir(target));if(logical)write(target,oldLink);
    solver.run();QCOMPARE(record(m).completedSteps,2u);solver.setNativeMixedStepBudget(0);solver.run();
    QCOMPARE(recordBytes(record(m)),recordBytes(expected));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::postCommitObserverContinues() try
{
    QTemporaryDir dir;auto source=fixture();const auto expected=reference(source,dir.filePath("reference.rbm"));auto m=source;
    RSolver solver(m,dir.filePath("actual.rbm"),{});bool inject=true;std::vector<uint> steps;
    solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){steps.push_back(record(accepted).completedSteps);
        if(inject){inject=false;throw std::runtime_error("postcommit notification failure");}});
    QVERIFY(!failure(solver).isEmpty());QCOMPARE(record(m).completedSteps,1u);QVERIFY(solver.isNativeMixedRetryable());solver.run();
    QCOMPARE(recordBytes(record(m)),recordBytes(expected));QCOMPARE(steps.size(),size_t(12));for(uint i=0;i<12;++i)QCOMPARE(steps[i],i+1);
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::failedRollbackPoisonsSolver() try
{
    QTemporaryDir dir;auto m=fixture();const auto checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m);
    auto solver=std::make_unique<RSolver>(m,dir.filePath("actual.rbm"),QString());solver->setAcceptedStepParticipant(store);solver->setNativeMixedStepBudget(1);solver->run();
    const auto baseline=recordBytes(record(m));store->setPhaseHook(Phase::AfterLatest,[&](Phase){
        store->setPhaseHook(Phase::BeforeRollbackLatest,[](Phase){throw std::runtime_error("actual rollback failure");});throw std::runtime_error("actual publish failure");});
    QVERIFY(!failure(*solver).isEmpty());QCOMPARE(recordBytes(record(m)),baseline);QVERIFY(!solver->isNativeMixedRetryable());QVERIFY(!store->isRetryable());
    QVERIFY(!failure(*solver).isEmpty());QVERIFY_EXCEPTION_THROWN(store->readLatest(),std::exception);QVERIFY(!store->rollbackEvidencePaths().empty());
    solver.reset();store.reset();
    const auto input=QString(64,'a'),runtime=QString(64,'c');
    QVERIFY_EXCEPTION_THROWN(Store(checkpointDir,context(input,runtime,record(m).fixedTimeStep),input,
        NativeAcceptedCheckpointContext::noControlSha256(),runtime,true,m,view),std::exception);
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::configuredMonitoringAndUpfrontRejection() try
{
    QTemporaryDir dir;auto m=fixture();const auto monitorPath=dir.filePath("tip-monitor.txt");
    RMonitoringPoint monitor(R_VARIABLE_DISPLACEMENT,RR3Vector(1,0,0));monitor.setOutputFileName(monitorPath);m.getMonitoringPointManager().push_back(monitor);
    RSolver solver(m,dir.filePath("monitored.rbm"),{});solver.setNativeMixedStepBudget(1);solver.run();
    RFile in(monitorPath,RFile::ASCII);QVERIFY(in.open(QIODevice::ReadOnly));RMonitoringPoint read;uint iteration=0;RRVector values;
    read.readRecordFromFile(in,iteration,values);QCOMPARE(iteration,1u);QCOMPARE(values.getNRows(),3u);QVERIFY(std::abs(values[0]-record(m).q[3])<5e-8);
    auto invalid=fixture();const auto pos=invalid.addVariable(RVariable(R_VARIABLE_DISPLACEMENT));invalid.getVariable(pos).resize(3,2);invalid.getVariable(pos).setValue(0,1,999);
    RMonitoringPoint missing(R_VARIABLE_TEMPERATURE,RR3Vector(1,0,0));missing.setOutputFileName(dir.filePath("invalid-monitor.txt"));invalid.getMonitoringPointManager().push_back(missing);
    RSolver rejected(invalid,dir.filePath("invalid.rbm"),{});QVERIFY(!failure(rejected).isEmpty());QVERIFY(!record(invalid).available);
    QCOMPARE(invalid.getVariable(invalid.findVariable(R_VARIABLE_DISPLACEMENT)).getValue(0,1),999.0);QVERIFY(!QFileInfo::exists(dir.filePath("invalid.rbm")));
    auto staleElectrical=fixture();const auto electric=staleElectrical.addVariable(RVariable(R_VARIABLE_ELECTRIC_FIELD));
    auto &oldField=staleElectrical.getVariable(electric);oldField.setApplyType(R_VARIABLE_APPLY_NODE);oldField.resize(3,2);oldField.setValue(0,1,123);
    const auto staleMonitorPath=dir.filePath("stale-electric-monitor.txt"),staleOutput=dir.filePath("stale-electric.rbm");
    RMonitoringPoint staleMonitor(R_VARIABLE_ELECTRIC_FIELD,RR3Vector(1,0,0));staleMonitor.setOutputFileName(staleMonitorPath);
    staleElectrical.getMonitoringPointManager().push_back(staleMonitor);RSolver staleRejected(staleElectrical,staleOutput,{});
    QVERIFY(!failure(staleRejected).isEmpty());QVERIFY(!record(staleElectrical).available);
    QCOMPARE(staleElectrical.getVariable(staleElectrical.findVariable(R_VARIABLE_ELECTRIC_FIELD)).getValue(0,1),123.0);
    QVERIFY(!QFileInfo::exists(staleMonitorPath));QVERIFY(!QFileInfo::exists(staleOutput));
    auto ambiguous=fixture();const auto ambiguousPath=dir.filePath("ambiguous-stress.txt");
    RMonitoringPoint stress(R_VARIABLE_STRESS_NORMAL,RR3Vector(1,0,0));stress.setOutputFileName(ambiguousPath);
    ambiguous.getMonitoringPointManager().push_back(stress);
    RSolver ambiguousSolver(ambiguous,dir.filePath("ambiguous.rbm"),{});
    QVERIFY(failure(ambiguousSolver).contains("ambiguous"));
    QVERIFY(!record(ambiguous).available);QVERIFY(!QFileInfo::exists(ambiguousPath));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::legacyCallbacksRejected() try
{
    QTemporaryDir dir;auto m=fixture();RSolver solver(m,dir.filePath("legacy.rbm"),{});
    QVERIFY(rejected([&]{solver.setStructuralControlCallback("legacy","revision",[](const RStructuralControlObservation &){return RStructuralControlDecision{};});}));
    QVERIFY(!record(m).available);QVERIFY(!QFileInfo::exists(dir.filePath("legacy.rbm")));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::realCliSplitResume_data()
{
    QTest::addColumn<bool>("binary");QTest::addColumn<bool>("tiny");QTest::addColumn<bool>("nonzeroStart");
    for(bool binary:{false,true}) {
        QTest::newRow(qPrintable(QString("%1-decimal-held-inputs").arg(binary?"rbm":"ram")))<<binary<<false<<false;
        QTest::newRow(qPrintable(QString("%1-tiny-stationary").arg(binary?"rbm":"ram")))<<binary<<true<<false;
        QTest::newRow(qPrintable(QString("%1-decimal-nonzero-start").arg(binary?"rbm":"ram")))<<binary<<false<<true;
    }
}
void TestNativeMixedRun::realCliSplitResume() try
{
    QFETCH(bool,binary);QFETCH(bool,tiny);QFETCH(bool,nonzeroStart);QVERIFY(QFileInfo(executable()).isFile());QTemporaryDir dir;
    const double dt=tiny?1e-14:.0034567890123456789;auto source=fixture(dt,tiny);
    if(nonzeroStart){auto &clock=source.getTimeSolver();clock.setInputStartTime(.12523456789012345+dt);
        const double initial=clock.getInputStartTime()-dt;std::vector<double> times;
        for(uint step=1;step<=12;++step)times.push_back(initial+double(step)*dt);
        clock.setTimes(times);source.getProblemSetup().getStructuralTrussDynamicsSetup().bindToModel(source);}
    const double expectedInitial=source.getTimeSolver().getInputStartTime()-dt;
    const auto ext=RModel::getDefaultFileExtension(binary),fullInput=dir.filePath("full."+ext),splitInput=dir.filePath("split."+ext);
    source.write(fullInput,false);source.write(splitInput,false);
    const auto fullDir=dir.filePath("full-checkpoints"),splitDir=dir.filePath("split-checkpoints");
    const auto runtime=fileDigest(executable());const auto fullContext=context(fileDigest(fullInput),runtime,dt),splitContext=context(fileDigest(splitInput),runtime,dt);
    auto full=cli(fullInput,fullDir,fullContext);QVERIFY2(full.code==0,full.diagnostics.constData());
    auto split=cli(splitInput,splitDir,splitContext,4);QVERIFY2(split.code==0,split.diagnostics.constData());
    auto checkpoint=latest(splitDir);QCOMPARE(record(checkpoint).completedSteps,4u);QCOMPARE(record(checkpoint).runTargetSteps,12u);
    QCOMPARE(record(checkpoint).fixedTimeStep,dt);QCOMPARE(record(checkpoint).initialTime,expectedInitial);
    QCOMPARE(record(checkpoint).time,expectedInitial+4*dt);QCOMPARE(record(checkpoint).nextCommandIndex,tiny?0u:2u);
    const auto recoveryPath=latestPath(splitDir),recoveryHash=fileDigest(recoveryPath);
    auto resumed=cli(recoveryPath,splitDir,splitContext,0,true);QVERIFY2(resumed.code==0,resumed.diagnostics.constData());
    QCOMPARE(fileDigest(recoveryPath),recoveryHash); // The verified immutable recovery MODEL remains diagnostic authority.
    const auto expected=latest(fullDir),actual=latest(splitDir);QCOMPARE(record(actual).completedSteps,12u);
    QCOMPARE(recordBytes(record(actual)),recordBytes(record(expected)));QCOMPARE(record(actual).time,expectedInitial+12*dt);
    QCOMPARE(record(actual).orderedPortIds,std::vector<QString>({"V-port","Q-port"}));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::realCliRejectsReservedSource_data()
{
    QTest::addColumn<bool>("numbered");QTest::newRow("reserved-logical-source")<<false;
    QTest::newRow("reserved-numbered-source")<<true;
}
void TestNativeMixedRun::realCliRejectsReservedSource() try
{
    QFETCH(bool,numbered);QTemporaryDir dir;QVERIFY(dir.isValid());const auto checkpointDir=dir.filePath("checkpoints");
    const auto reserved=QDir(checkpointDir).filePath("run-results");QVERIFY(QDir().mkpath(reserved));
    const auto logical=QDir(reserved).filePath("native-mixed.rbm");
    const auto input=numbered?RFileUtils::getFileNameWithTimeStep(logical,1):logical;
    auto source=fixture();const auto written=source.write(input,false);
    if (written != input) write(input,raw(written));
    const auto before=raw(input);
    const auto ctx=context(fileDigest(input),fileDigest(executable()),source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep());
    const auto result=cli(input,checkpointDir,ctx,0,false);QVERIFY2(result.code!=0,result.diagnostics.constData());
    QCOMPARE(raw(input),before);QVERIFY(!QFileInfo::exists(QDir(checkpointDir).filePath("latest.json")));
    QVERIFY(!QFileInfo::exists(QDir(checkpointDir).filePath("lastvalid.json")));
} REPORT_RUN_EXCEPTIONS
void TestNativeMixedRun::realCliRejectsTamperedRestart_data()
{QTest::addColumn<bool>("sourceMutation");QTest::newRow("source-geometry")<<true;QTest::newRow("authoritative-restart-q")<<false;}
void TestNativeMixedRun::realCliRejectsTamperedRestart() try
{
    QFETCH(bool,sourceMutation);QTemporaryDir dir;auto source=fixture();const auto input=dir.filePath("input.rbm");source.write(input,false);
    const auto checkpointDir=dir.filePath("checkpoints");const auto ctx=context(fileDigest(input),fileDigest(executable()),.0034567890123456789);
    const auto initial=cli(input,checkpointDir,ctx,4);QVERIFY2(initial.code==0,initial.diagnostics.constData());const auto pointer=raw(QDir(checkpointDir).filePath("latest.json"));
    auto bad=latest(checkpointDir);if(sourceMutation){bad.getNode(1).setX(1.001);bad.getProblemSetup().getStructuralTrussDynamicsSetup().bindToModel(bad);}
    else bad.getProblemSetup().getStructuralTrussDynamicsRestartData().q[3]+=.01;
    const auto modified=dir.filePath("tampered.rbm");bad.write(modified,false);
    const auto failed=cli(modified,checkpointDir,ctx,0,true);QVERIFY2(failed.code!=0,failed.diagnostics.constData());
    QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),pointer);QCOMPARE(record(latest(checkpointDir)).completedSteps,4u);
} REPORT_RUN_EXCEPTIONS
#undef REPORT_RUN_EXCEPTIONS
QTEST_GUILESS_MAIN(TestNativeMixedRun)
#include "tst_native_mixed_run.moc"
