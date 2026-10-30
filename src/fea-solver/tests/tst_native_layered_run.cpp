#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <rbl_error.h>
#include <rml_model.h>
#include <rml_file_utils.h>
#include <rml_monitoring_point.h>
#include <rsolver.h>
#include <rparallel_execution_context.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include "native_accepted_checkpoint.h"

namespace {
using Run=RStructuralPiezoLayeredNativeRun;
using Codec=RStructuralPiezoLayeredRestartCodec;
using Store=NativeAcceptedCheckpointStore;
using Phase=Store::Phase;
RModel scene(bool plate=false,bool configured=false) {
    RModel m;const double xy[4][2]={{0,0},{1,0},{1,1},{0,1}};
    for(uint i=0;i<(plate?4u:3u);++i){m.addNode(plate?RNode(xy[i][0],xy[i][1],0):RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<(plate?1u:2u);++i){RElement e(plate?R_ELEMENT_QUAD1:R_ELEMENT_BEAM1);for(uint j=0;j<(plate?4u:2u);++j)e.setNodeId(j,plate?j:i+j);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    // This fixture declares its complete loading through the layered setup.
    // removeAllEnvironmentConditions() would reinstate these legacy defaults.
    for(uint i=0;i<(plate?m.getNSurfaces():m.getNLines());++i){
        RElementGroup &group=plate?static_cast<RElementGroup &>(m.getSurface(i)):static_cast<RElementGroup &>(m.getLine(i));
        group.removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        group.removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_TEMPERATURE);
    }
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));m.getProblemSetup().setMassMatrixType(RMassMatrixType::Consistent);
    auto &d=m.getProblemSetup().getStructuralDynamicsSetup();d.setEnabled(true);d.setTimeStep(.01);d.setNSteps(8);m.getTimeSolver().setInputStartTime(.01);
    auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();s.enabled=true;if(plate)s.family=RStructuralPiezoLayeredModelFamily::FlatPlate;
    const auto native=RStructuralPiezoContinuumMaterial::isotropic(30,.2,7);
    RStructuralPiezoLayeredMaterial h;h.materialId="host";h.elasticityOrCompliance=native.elasticity;h.density=7;h.sourceId="independent-example";h.sourceSha256=QString(64,'a');
    auto p=h;p.materialId="piezo";p.piezoelectric=true;p.coupling[12]=2;p.coupling[13]=2;p.permittivity={{4,0,0,0,4,0,0,0,4}};s.materials={h,p};
    RStructuralPiezoLayeredPort drive;drive.readout.portId="drive";drive.readout.role=RStructuralPiezoPortRole::ActuatorSensor;drive.readout.readout=RStructuralPiezoReadoutMode::ActuatorCharge;drive.initialValue=.12;
    RStructuralPiezoLayeredPort sense;sense.readout.portId="sense";sense.readout.role=RStructuralPiezoPortRole::Sensor;sense.readout.readout=RStructuralPiezoReadoutMode::OpenCircuitVoltage;sense.initialValue=.03;s.ports={drive,sense};
    for(uint i=0;i<(plate?1u:2u);++i){RStructuralPiezoLayeredModelLayer host;host.materialId="host";host.lowerZ=.1;host.upperZ=.3;auto active=host;active.materialId="piezo";active.lowerZ=.3;active.upperZ=.4;active.portId="drive";auto sensor=active;sensor.lowerZ=.4;sensor.upperZ=.45;sensor.portId="sense";s.members.push_back({501+13*i,{host,active,sensor}});}
    using D=RStructuralPiezoPhysicalDof;
    for(uint node:plate?std::vector<uint>{101,134}:std::vector<uint>{101})for(D dof:plate?std::vector<D>{D::UX,D::UY,D::UZ,D::RX,D::RY}:std::vector<D>{D::UX,D::UZ,D::RY})s.supports.push_back({node,dof,0});
    s.backgroundForces={{123,{{.2,0,-.1,0,0,0}}}};s.initialVelocities={{123,{{.04,0,.08,0,0,0}}}};
    s.damping.kind=RStructuralPiezoDampingKind::Rayleigh;s.damping.rayleighAlpha=.06;s.damping.rayleighBeta=.005;
    if(configured){s.excitationConfiguration="ramp/v1";s.controllerConfiguration="counter+observer/v1";s.designConfiguration="native-experiment/v1";}
    s.bindToModel(m);return m;
}

QByteArray opaque(uint proposal,uint observer){return QByteArray::number(proposal)+":"+QByteArray::number(observer);}
std::pair<uint,uint> parse(const QByteArray &state){const auto fields=state.split(':');if(fields.size()!=2)throw std::invalid_argument("bad opaque");bool a=false,b=false;const uint x=fields[0].toUInt(&a),y=fields[1].toUInt(&b);if(!a||!b||opaque(x,y)!=state)throw std::invalid_argument("bad opaque");return {x,y};}
RStructuralPiezoLayeredRuntimeCallbacks bundle(const RModel &m,uint stride=3){
    const auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();RStructuralPiezoLayeredRuntimeCallbacks b;b.sourceFingerprint=s.computeSourceFingerprint(m);b.excitationConfiguration=s.excitationConfiguration;b.controllerConfiguration=s.controllerConfiguration;b.designConfiguration=s.designConfiguration;
    b.backendIdentity="native-test-pure-callbacks/v1";b.forcingIdentity="physical-time-ramp/v1";b.controllerIdentity="counter-observer/v1";b.controlEveryFESteps=stride;b.initialOpaqueControllerState=opaque(0,0);b.envelopes={{-.5,.5,2}};
    const uint nodes=m.getNNodes();b.perturbationForce=[nodes](double t){std::vector<double> f(6*nodes);f[12]=.2*t;f[14]=-.1*t;return f;};
    b.propose=[](const RStructuralPiezoLayeredDynamicsState &physical,uint completed,uint tick,const QByteArray &old){const auto counters=parse(old);if(counters.first!=tick||counters.second!=completed||physical.physicalDisplacements.empty())throw std::invalid_argument("proposal physical/phase mismatch");RStructuralPiezoLayeredControllerProposal p;p.requestedVoltages={tick%2?-2.:2.};p.nextOpaqueState=opaque(tick+1,completed);p.lastFrameBytes=QByteArray::number(completed);return p;};
    b.postFE=[](const RStructuralPiezoLayeredDynamicsState &p,uint completed,uint tick,const std::vector<double> &applied,const QByteArray &old){const auto c=parse(old);if(c.first!=tick||c.second+1!=completed||p.heldActuatorVoltages!=applied)throw std::invalid_argument("observer applied mismatch");return opaque(tick,completed);};
    b.validateOpaque=[](const QByteArray &s,uint completed,uint tick,uint){if(parse(s)!=std::make_pair(tick,completed))throw std::invalid_argument("opaque semantic mismatch");};return b;
}

QByteArray raw(const QString &path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error(qPrintable(f.errorString()));return f.readAll();
}
QString digest(const QString &path) {return QString::fromLatin1(QCryptographicHash::hash(raw(path),QCryptographicHash::Sha256).toHex());}
QByteArray bytes(const RModel &m) {return m.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes;}
RStructuralPiezoLayeredRunState state(const RModel &m) {return Codec::decode(bytes(m));}
NativeAcceptedCheckpointStateView view(const RModel &m) {
    RModel certified(m);certified.getProblemSetup().setRestart(certified.getProblemSetup().getStructuralPiezoLayeredRestart().available);
    const Run run(certified);const auto &s=run.acceptedRuntime();
    return {s.completedFE,s.physical.integrator.state.time,s.dt,QCryptographicHash::hash(Codec::encode(s),QCryptographicHash::Sha256)};
}
QByteArray context(const QString &input,const QString &runtime,double dt) {
    return QJsonDocument(QJsonObject{{"schema","range.native-checkpoint-context/v1"},{"sourceSha256",input},{"modelSha256",input},
        {"controlSha256",NativeAcceptedCheckpointContext::noControlSha256()},{"runtimeSha256",runtime},{"packageSha256",QString(64,'d')},
        {"policy","accepted-steps-or-sim-time-v1"},{"maxAcceptedSteps",10},{"maxSimulationSeconds",1.0},{"dtS",dt}}).toJson(QJsonDocument::Compact);
}
std::shared_ptr<Store> participant(const QString &dir,const RModel &m) {
    const QString input(64,'a'),runtime(64,'c');return std::make_shared<Store>(dir,context(input,runtime,.01),
        input,NativeAcceptedCheckpointContext::noControlSha256(),runtime,false,m,view);
}
QString failure(const std::function<void()> &f) {try{f();return {};}catch(const RError &e){return e.getMessage();}catch(const std::exception &e){return QString::fromUtf8(e.what());}}
QByteArray reference(const RModel &source) {
    Run run(source);while(run.hasRemainingSteps()){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);}return Codec::encode(run.acceptedRuntime());
}
QString latestPath(const QString &dir) {
    const auto p=QJsonDocument::fromJson(raw(QDir(dir).filePath("latest.json"))).object();
    const QString envelopePath=QDir(dir).filePath(p.value("envelopeFile").toString());
    if(digest(envelopePath)!=p.value("envelopeSha256").toString())throw std::runtime_error("Envelope hash mismatch");
    const auto envelope=QJsonDocument::fromJson(raw(envelopePath)).object();const QString path=QDir(dir).filePath(envelope.value("modelFile").toString());
    if(digest(path)!=envelope.value("checkpointModelSha256").toString())throw std::runtime_error("MODEL hash mismatch");return path;
}
RModel latest(const QString &dir) {RModel m;m.read(latestPath(dir));return m;}
QString executable() {
#ifdef RANGE_FEA_SOLVER_EXECUTABLE
    return QString::fromUtf8(RANGE_FEA_SOLVER_EXECUTABLE);
#elif defined(Q_OS_WIN)
    return QDir(QCoreApplication::applicationDirPath()).filePath("fea-solver.exe");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("fea-solver");
#endif
}
struct ProcessResult {int code;QByteArray diagnostics;};
ProcessResult cli(const QStringList &args) {
    QProcess p;p.setProcessChannelMode(QProcess::MergedChannels);
    auto env=QProcessEnvironment::systemEnvironment();
    for(const auto &name:env.keys())if(name.startsWith("RANGE_NATIVE_") || name.startsWith("RANGE_RUNTIME_"))env.remove(name);
    p.setProcessEnvironment(env);p.start(executable(),args);
    if(!p.waitForStarted(10000))throw std::runtime_error(qPrintable(p.errorString()));
    if(!p.waitForFinished(60000)){p.kill();p.waitForFinished(5000);throw std::runtime_error("Native layered CLI timed out");}
    const auto diagnostics=p.readAll();if(p.exitStatus()!=QProcess::NormalExit)throw std::runtime_error(diagnostics.constData());return {p.exitCode(),diagnostics};
}
ProcessResult cli(const QString &input,const QString &dir,const QByteArray &ctx,uint budget=0,bool restart=false) {
    QStringList args={"--file="+input,"--native-checkpoint-dir="+dir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx),
                      "--native-layered-steps="+QString::number(budget)};
    if(restart)args.append("--restart");return cli(args);
}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestNativeLayeredRun:public QObject {
    Q_OBJECT
private slots:
    void sameSolverBudgetAndFrames_data();void sameSolverBudgetAndFrames();
    void everyDiskSplit_data();void everyDiskSplit();
    void checkpointFailures_data();void checkpointFailures();
    void outputFailureAndPostCommitRetry();
    void callbackBundleRealRun();
    void sourceMutationAndUnsupportedModes();
    void realCliSplitResume_data();void realCliSplitResume();
    void realCliInvalidInputs_data();void realCliInvalidInputs();
};
void TestNativeLayeredRun::sameSolverBudgetAndFrames_data() {
    QTest::addColumn<bool>("plate");QTest::newRow("beam")<<false;QTest::newRow("plate")<<true;
}
void TestNativeLayeredRun::sameSolverBudgetAndFrames() try {
    QFETCH(bool,plate);auto m=scene(plate);const auto expected=reference(m);QTemporaryDir dir;QVERIFY(dir.isValid());
    auto &policy=m.getProblemSetup().getParallelExecutionConfig();policy.setMode(RParallelExecutionMode::FixedThreads);policy.setRequestedThreads(4);
    RSolver solver(m,dir.filePath("result.rbm"),{},RSolver::ExecutionPolicy::StructuralProduct);solver.setNativeLayeredStepBudget(3);
    uint acceptedCount=0,frames=0,reports=0;
    solver.setParallelExecutionReportObserver([&](const RParallelExecutionReport &r){
        if(r.appliedThreads!=1 || r.actualThreads!=1 || !r.deterministicReduction || r.requestedThreads!=4 || !r.fallback)
            throw std::runtime_error("Layered report did not expose actual deterministic serial physics");++reports;
    });
    solver.setResultFrameObserver([&](const RSolverResultFrame &frame){
        if(!QFileInfo(frame.actualRecordPath).isFile() || frame.recordNo!=frames+1 || frame.nativeAnalysis!="transient" ||
           frame.checkpointTime!=double(frame.recordNo)*.01)throw std::runtime_error("Actual native result frame mismatch");
        RModel disk;disk.read(frame.actualRecordPath);if(bytes(disk)!=bytes(m))throw std::runtime_error("Native frame is not complete accepted runtime");++frames;
    });
    solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){
        const auto s=state(accepted);if(s.completedFE!=++acceptedCount || s.targetSteps!=8 || s.controlTick!=0 || s.lastApplied.size()!=1 ||
            s.physical.readouts.size()!=2 || s.physical.physicalReactions.size()!=6*accepted.getNNodes())throw std::runtime_error("Accepted runtime semantics mismatch");
    });
    solver.run();QCOMPARE(state(m).completedFE,3u);solver.run();QCOMPARE(state(m).completedFE,6u);
    solver.setNativeLayeredStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);QCOMPARE(frames,8u);QCOMPARE(acceptedCount,8u);QCOMPARE(reports,3u);
    QVERIFY(solver.isNativeLayeredRetryable());QVERIFY(!solver.isNativeMixedRetryable());
    QVERIFY(!failure([&]{solver.setNativeMixedStepBudget(1);}).isEmpty());
    solver.run();QCOMPARE(bytes(m),expected);QCOMPARE(frames,8u);
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::everyDiskSplit_data() {
    QTest::addColumn<bool>("plate");QTest::addColumn<QString>("extension");
    for(bool plate:{false,true})for(const auto &ext:{QString("ram"),QString("rbm")})QTest::newRow(qPrintable(QString("%1-%2").arg(plate?"plate":"beam",ext)))<<plate<<ext;
}
void TestNativeLayeredRun::everyDiskSplit() try {
    QFETCH(bool,plate);QFETCH(QString,extension);const auto source=scene(plate);const auto expected=reference(source);QTemporaryDir dir;QVERIFY(dir.isValid());
    for(uint split=0;split<=8;++split){auto m=source;
        if(split){RSolver first(m,dir.filePath(QString("first-%1.rbm").arg(split)),{});first.setNativeLayeredStepBudget(split);first.run();}
        else {const Run initialized(m);m=initialized.acceptedModel(m);}
        const auto acceptedBytes=bytes(m);const QString path=dir.filePath(QString("restore-%1.%2").arg(split).arg(extension));
        const auto actualPath=m.nativeWritePaths(path,false).first();m.write(path,false);RModel disk;disk.read(actualPath);QCOMPARE(bytes(disk),acceptedBytes);
        disk.getProblemSetup().setRestart(true);RSolver resumed(disk,dir.filePath(QString("remaining-%1.rbm").arg(split)),{});resumed.run();
        QCOMPARE(bytes(disk),expected);QCOMPARE(state(disk).completedFE,8u);QCOMPARE(disk.getTimeSolver().getTimes().size(),size_t(8));
        QCOMPARE(disk.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),8u);
    }
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::checkpointFailures_data() {
    QTest::addColumn<int>("phase");
    for(Phase p:{Phase::BeforeModelWrite,Phase::AfterModelWrite,Phase::AfterModelReadBack,Phase::BeforeEnvelopeWrite,
        Phase::AfterEnvelopeWrite,Phase::AfterEnvelopeReadBack,Phase::BeforeLastValid,Phase::AfterLastValid,
        Phase::BeforeLatest,Phase::AfterLatest,Phase::AfterLatestReadBack,Phase::BeforeMarkerWrite,Phase::AfterMarkerWrite,Phase::BeforeMarkerRemove})
        QTest::newRow(qPrintable(QString::number(int(p))))<<int(p);
}
void TestNativeLayeredRun::checkpointFailures() try {
    QFETCH(int,phase);QTemporaryDir dir;QVERIFY(dir.isValid());auto m=scene();const auto expected=reference(m);
    const auto output=dir.filePath("actual.rbm"),checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m);
    RSolver solver(m,output,{});solver.setAcceptedStepParticipant(store);solver.setNativeLayeredStepBudget(1);solver.run();
    const auto oldBytes=bytes(m),oldLatest=raw(QDir(checkpointDir).filePath("latest.json")),oldOutput=raw(output);
    const auto lastPath=QDir(checkpointDir).filePath("lastvalid.json");const bool lastExisted=QFileInfo::exists(lastPath);
    const auto oldLast=lastExisted?raw(lastPath):QByteArray();const auto recoveryPath=latestPath(checkpointDir),recoveryHash=digest(recoveryPath);
    const auto oldTime=m.getTimeSolver().getComputedTime();
    store->setPhaseHook(Phase(phase),[](Phase){throw std::runtime_error("injected layered checkpoint publication failure");});
    const auto diagnostic=failure([&]{solver.run();});QVERIFY2(diagnostic.contains("injected layered"),qPrintable(diagnostic));
    QCOMPARE(bytes(m),oldBytes);QCOMPARE(m.getTimeSolver().getComputedTime(),oldTime);QCOMPARE(bytes(latest(checkpointDir)),oldBytes);
    QCOMPARE(raw(output),oldOutput);QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),oldLatest);
    QCOMPARE(QFileInfo::exists(lastPath),lastExisted);if(lastExisted)QCOMPARE(raw(lastPath),oldLast);
    QCOMPARE(digest(recoveryPath),recoveryHash);QVERIFY(!QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(output,2)));
    QVERIFY(solver.isNativeLayeredRetryable());QVERIFY(store->isRetryable());solver.run();QCOMPARE(state(m).completedFE,2u);
    solver.setNativeLayeredStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);QCOMPARE(bytes(latest(checkpointDir)),expected);
    QCOMPARE(digest(recoveryPath),recoveryHash);
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::outputFailureAndPostCommitRetry() try {
    QTemporaryDir dir;auto m=scene();const auto expected=reference(m);const auto output=dir.filePath("output.rbm");
    RSolver solver(m,output,{});solver.setNativeLayeredStepBudget(1);solver.run();const auto oldBytes=bytes(m),oldOutput=raw(output);
    const auto blocked=RFileUtils::getFileNameWithTimeStep(output,2);QVERIFY(QDir().mkpath(blocked));QVERIFY(!failure([&]{solver.run();}).isEmpty());
    QCOMPARE(bytes(m),oldBytes);QCOMPARE(raw(output),oldOutput);QVERIFY(solver.isNativeLayeredRetryable());QVERIFY(QDir().rmdir(blocked));
    solver.setAcceptedStructuralStepObserver([](const RModel &){throw std::runtime_error("postcommit observer failure");});
    QVERIFY(failure([&]{solver.run();}).contains("postcommit"));QCOMPARE(state(m).completedFE,2u);
    solver.setAcceptedStructuralStepObserver({});solver.setNativeLayeredStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::callbackBundleRealRun() try {
    QTemporaryDir dir;auto m=scene(false,true);const auto cb=bundle(m);Run independent(m,cb);
    while(independent.hasRemainingSteps()){auto step=independent.prepare(m);independent.validatePrepared(*step);independent.commit(*step);}
    const auto expected=Codec::encode(independent.acceptedRuntime());
    RSolver solver(m,dir.filePath("controlled.rbm"),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,cb);
    solver.setNativeLayeredStepBudget(2);solver.run();QCOMPARE(state(m).completedFE,2u);QCOMPARE(state(m).controlTick,1u);
    solver.run();QCOMPARE(state(m).completedFE,4u);QCOMPARE(state(m).controlTick,2u);
    solver.setNativeLayeredStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);QCOMPARE(state(m).opaqueControllerState,opaque(3,8));
    const auto path=m.nativeWritePaths(dir.filePath("controlled-restart.rbm"),false).first();m.write(path,false);
    RModel disk;disk.read(path);disk.getProblemSetup().setRestart(true);
    RSolver restored(disk,dir.filePath("restored.rbm"),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,cb);restored.run();QCOMPARE(bytes(disk),expected);
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::sourceMutationAndUnsupportedModes() try {
    QTemporaryDir dir;const auto source=scene();auto m=source;const auto output=dir.filePath("mutation.rbm");RSolver solver(m,output,{});
    solver.setParallelExecutionReportObserver([&](const auto &){m.getProblemSetup().getStructuralPiezoLayeredSetup().materials[0].density+=1;});
    QVERIFY(!failure([&]{solver.run();}).isEmpty());QVERIFY(!QFileInfo::exists(output));QVERIFY(bytes(m).isEmpty());
    m=source;solver.setParallelExecutionReportObserver({});
    QVERIFY(!failure([&]{solver.setStructuralSupportResponseObserver([](const auto &){});}).isEmpty());
    QVERIFY(!failure([&]{solver.setStructuralControlCallback("legacy","revision",[](const auto &){return RStructuralControlDecision{};});}).isEmpty());
    QVERIFY(!failure([&]{solver.setStructuralControlStepObserver([](const auto &){});}).isEmpty());
    auto unsupported=source;unsupported.getMonitoringPointManager().push_back(RMonitoringPoint());
    QVERIFY(!failure([&]{RSolver rejected(unsupported,dir.filePath("monitoring.rbm"),{});}).isEmpty());
    unsupported=scene(false,true);QVERIFY(!failure([&]{RSolver rejected(unsupported,dir.filePath("runtime.rbm"),{});}).isEmpty());
    unsupported=source;unsupported.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_HEAT|R_PROBLEM_STRESS));
    QVERIFY(!failure([&]{RSolver rejected(unsupported,dir.filePath("mixed.rbm"),{});}).isEmpty());
    QVERIFY(!failure([&]{solver.setNativeMixedStepBudget(1);}).isEmpty());
    solver.setNativeLayeredStepBudget(1);solver.run();QCOMPARE(state(m).completedFE,1u);
    m.getProblemSetup().getStructuralPiezoLayeredSetup().enabled=false;QVERIFY(!failure([&]{solver.run();}).isEmpty());
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::realCliSplitResume_data() {everyDiskSplit_data();}
void TestNativeLayeredRun::realCliSplitResume() try {
    QFETCH(bool,plate);QFETCH(QString,extension);QVERIFY(QFileInfo(executable()).isFile());QTemporaryDir dir;const auto source=scene(plate);
    const auto expected=reference(source);const auto runtimeHash=digest(executable());
    for(uint split:{1u,3u,7u}){
        const auto input=dir.filePath(QString("source-%1.%2").arg(split).arg(extension));source.write(input,false);const auto inputHash=digest(input);
        const auto checkpointDir=dir.filePath(QString("checkpoints-%1").arg(split));const auto ctx=context(inputHash,runtimeHash,.01);
        const auto first=cli(input,checkpointDir,ctx,split);QVERIFY2(first.code==0,first.diagnostics.constData());QCOMPARE(digest(input),inputHash);
        const auto firstModel=latest(checkpointDir);QCOMPARE(state(firstModel).completedFE,split);QCOMPARE(state(firstModel).targetSteps,8u);
        const auto recoveryPath=latestPath(checkpointDir),recoveryHash=digest(recoveryPath);
        const auto resumed=cli(recoveryPath,checkpointDir,ctx,0,true);QVERIFY2(resumed.code==0,resumed.diagnostics.constData());
        const auto final=latest(checkpointDir);QCOMPARE(bytes(final),expected);QCOMPARE(digest(input),inputHash);QCOMPARE(digest(recoveryPath),recoveryHash);
        const auto last=state(final);QCOMPARE(last.completedFE,8u);QCOMPARE(last.targetSteps,8u);QCOMPARE(last.physical.integrator.state.time,.08);
        const QString workPath=QDir(checkpointDir).filePath("run-results/native-layered.rbm");QVERIFY(QFileInfo::exists(workPath));RModel work;work.read(workPath);QCOMPARE(bytes(work),expected);
    }
} REPORT_EXCEPTIONS
void TestNativeLayeredRun::realCliInvalidInputs_data() {
    QTest::addColumn<QString>("kind");for(const auto *name:{"bad-decimal","negative","overflow","mixed-budget","both-budgets","runtime-config","missing-restart","changed-source","tampered-runtime","legacy-monitoring","reserved-source"})QTest::newRow(name)<<QString::fromLatin1(name);
}
void TestNativeLayeredRun::realCliInvalidInputs() try {
    QFETCH(QString,kind);QTemporaryDir dir;auto source=scene();const auto input=dir.filePath("source.rbm");
    QStringList extra;
    if(kind=="runtime-config")source=scene(false,true);
    if(kind=="legacy-monitoring")source.getMonitoringPointManager().push_back(RMonitoringPoint());
    if(kind=="changed-source" || kind=="tampered-runtime"){
        const Run initialized(source);source=initialized.acceptedModel(source);
        if(kind=="changed-source"){
            auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();setup.materials[0].density+=1;setup.bindToModel(source);
            // Keep the MODEL envelope structurally valid so the actual CLI
            // must reject the old full runtime bound to a different source.
            source.getProblemSetup().getStructuralPiezoLayeredRestart().sourceFingerprint=setup.sourceFingerprint;
        }
        else {auto s=state(source);s.backendIdentity="changed-backend";source.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes=Codec::encode(s);}
        extra.append("--restart");
    }
    source.write(input,false);const auto before=digest(input);const auto checkpointDir=dir.filePath("checkpoints");const auto ctx=context(before,digest(executable()),.01);
    QString budget="1";
    if(kind=="bad-decimal")budget="01";if(kind=="negative")budget="-1";if(kind=="overflow")budget="4294967296";
    if(kind=="mixed-budget")extra.append("--native-mixed-steps=1");
    if(kind=="both-budgets")extra.append("--native-mixed-steps=0");
    if(kind=="missing-restart")extra.append("--restart");
    if(kind=="reserved-source"){
        const auto first=cli(input,checkpointDir,ctx,1);QVERIFY2(first.code==0,first.diagnostics.constData());
        const auto pointer=raw(QDir(checkpointDir).filePath("latest.json"));const auto reserved=QDir(checkpointDir).filePath("run-results/native-layered.rbm");
        const auto result=cli(reserved,checkpointDir,ctx,1,true);QVERIFY2(result.code!=0,result.diagnostics.constData());
        QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),pointer);QCOMPARE(digest(input),before);return;
    }
    QStringList args={"--file="+input,"--native-checkpoint-dir="+checkpointDir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx)};
    if(kind!="mixed-budget")args.append("--native-layered-steps="+budget);args.append(extra);
    const auto result=cli(args);QVERIFY2(result.code!=0,result.diagnostics.constData());QCOMPARE(digest(input),before);
    QVERIFY(!QFileInfo::exists(QDir(checkpointDir).filePath("latest.json")));
} REPORT_EXCEPTIONS
QTEST_APPLESS_MAIN(TestNativeLayeredRun)
#include "tst_native_layered_run.moc"
