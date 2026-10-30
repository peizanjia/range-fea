#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QDataStream>
#include <QScopeGuard>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <rbl_error.h>
#include <rml_model.h>
#include <rml_file_utils.h>
#include <rml_monitoring_point.h>
#include <rsolver.h>
#include <rparallel_execution_context.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include "native_accepted_checkpoint.h"
#include "piezo_axial_backend_bridge.h"
#include "piezo_numeric_transport.h"
#include <stdexcept>

namespace {
using Run=RStructuralPiezoAxialNativeRun;
using Codec=RStructuralPiezoAxialRestartCodec;
using Bundle=RStructuralPiezoAxialRuntimeCallbacks;
using Store=NativeAcceptedCheckpointStore;
using Phase=Store::Phase;
using ReadoutMode=RStructuralPiezoReadoutMode;
QByteArray json(const QJsonObject &o) {return QJsonDocument(o).toJson(QJsonDocument::Compact);}
void component(RBoundaryCondition &bc,RVariableType type,double value,bool enabled=true) {
    auto &c=bc.getComponent(bc.findComponentPosition(type));c.setEnabled(enabled);c.clear();if(enabled)c.add(0,value);
}
RBoundaryCondition support(bool fixed) {
    RBoundaryCondition bc(R_BOUNDARY_CONDITION_DISPLACEMENT);
    component(bc,R_VARIABLE_DISPLACEMENT_X,0,fixed);
    component(bc,R_VARIABLE_DISPLACEMENT_Y,0);component(bc,R_VARIABLE_DISPLACEMENT_Z,0);return bc;
}
// Three physical nodes, one voltage replacement rod, a passive rod and an
// open-circuit sensor rod. The actual native scalar FE remains the plant.
RModel scene(bool controlled=true) {
    RModel m;for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<3;++i) {
        RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,i==0?0:1);e.setNodeId(1,i==0?1:2);
        m.addElement(e,true,i);m.setElementPhysicalId(i,501+13*i);
        RMaterial material(RMaterial::Solid);
        RMaterialProperty young(RMaterialProperty::ModulusOfElasiticity);young.add(293.15,30);material.add(young);
        RMaterialProperty density(RMaterialProperty::Density);density.add(293.15,1);material.add(density);
        auto &line=m.getLine(i);line.setCrossArea(1);line.setMaterial(material);line.addBoundaryCondition(support(false));
    }
    for(uint i:{0u,2u}) {RElement e(R_ELEMENT_POINT);e.setNodeId(0,i);m.addElement(e,true,i==0?0:1);}
    m.getPoint(0).addBoundaryCondition(support(true));
    RInitialCondition initial(R_INITIAL_CONDITION_DISPLACEMENT);
    for(auto type:{R_VARIABLE_DISPLACEMENT_X,R_VARIABLE_DISPLACEMENT_Y,R_VARIABLE_DISPLACEMENT_Z}) {
        auto &c=initial.getComponent(initial.findComponentPosition(type));c.setEnabled(true);c.clear();c.add(0,type==R_VARIABLE_DISPLACEMENT_X?.02:0);
    }
    m.getPoint(1).addInitialCondition(initial);
    for(uint i=0;i<m.getNElementGroups();++i) {
        auto *group=m.getElementGroupPtr(i);if(!group)continue;
        group->removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        group->removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_TEMPERATURE);
    }
    m.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));m.getProblemSetup().setMassMatrixType(RMassMatrixType::Lumped);
    auto &d=m.getProblemSetup().getStructuralDynamicsSetup();d.setEnabled(true);d.setTimeStep(.01);d.setNSteps(8);d.setOutputFrequency(1);
    auto &time=m.getTimeSolver();time.setEnabled(true);time.setInputStartTime(.01);time.setInputTimeStepSize(.01);
    time.setInputNTimeSteps(8);time.setTimes({.01,.02,.03,.04,.05,.06,.07,.08});
    auto &truss=m.getProblemSetup().getStructuralTrussDynamicsSetup();truss.enabled=true;truss.passiveMembers={{514,1}};
    truss.absoluteForceTolerance=1e-11;truss.relativeForceTolerance=1e-11;truss.linearRelativeTolerance=1e-12;
    RStructuralTrussActiveMember drive;drive.physicalElementId=501;drive.portId="drive";drive.referenceLength=1;
    drive.axialStiffness=100;drive.coupling=2;drive.capacitance=.5;drive.kind=RStructuralTrussElectricalKind::Voltage;
    drive.massSource=RStructuralTrussMassSource::FixedMemberMass;drive.fixedMass=7;
    auto sensor=drive;sensor.physicalElementId=527;sensor.portId="sense";sensor.axialStiffness=10;sensor.coupling=-.4;
    sensor.kind=RStructuralTrussElectricalKind::ConductorCharge;sensor.fixedMass=1;truss.activeMembers={drive,sensor};
    auto &readout=m.getProblemSetup().getStructuralPiezoReadoutSetup();
    readout.ports={{"drive",RStructuralPiezoPortRole::ActuatorSensor,RStructuralPiezoReadoutMode::ActuatorCharge},
                   {"sense",RStructuralPiezoPortRole::Sensor,RStructuralPiezoReadoutMode::OpenCircuitVoltage}};
    // Initial displacement is free vibration about the independently certified
    // zero-load reference, rather than an asserted equilibrated initial state.
    readout.workpoint.available=true;
    for(uint i=0;i<m.getNNodes();++i)readout.workpoint.nodes.push_back({m.getNodePhysicalId(i),{}, {}});
    readout.workpoint.ports={{"drive",0,0},{"sense",0,0}};
    truss.bindToModel(m);
    QJsonObject controller{{"kind",controlled?"PI":"none"},{"controlEveryFESteps",3}};
    if(controlled) {
        controller["kp"]=.4;controller["ki"]=.02;controller["selectedOutputs"]=QJsonArray{"tip:q"};
        controller["bias"]=QJsonArray{.3};controller["voltageLower"]=QJsonArray{-.5};
        controller["voltageUpper"]=QJsonArray{.5};controller["deltaVoltageLimits"]=QJsonArray{.06};
    }
    auto &axial=m.getProblemSetup().getStructuralPiezoAxialSetup();axial.enabled=true;
    axial.recipeConfiguration=json({{"family","axialRod"},{"controller",controller},
        {"damping",QJsonObject{{"kind","rayleigh"},{"rayleighAlpha",.04},{"rayleighBeta",.002}}},
        {"design",QJsonObject{{"modeIndices",QJsonArray{0}},{"outputs",QJsonArray{
            QJsonObject{{"id","tip:q"},{"physicalNodeId",123},{"dof","X"},{"quantity","displacement"}},
            QJsonObject{{"id","tip:v"},{"physicalNodeId",123},{"dof","X"},{"quantity","velocity"}}}}}},
        {"excitation",QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId","node:123:X"},{"unit","N"},
            {"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","0.01*t"}}}}}}}}}});
    axial.bindToModel(m);return m;
}
RModel sensorReadoutScene(ReadoutMode mode) {
    auto m=scene(true);
    auto &truss=m.getProblemSetup().getStructuralTrussDynamicsSetup();
    auto &sensor=truss.activeMembers[1];
    sensor.kind=mode==ReadoutMode::OpenCircuitVoltage ? RStructuralTrussElectricalKind::ConductorCharge : RStructuralTrussElectricalKind::Voltage;
    sensor.initialValue=0.0;
    auto &readout=m.getProblemSetup().getStructuralPiezoReadoutSetup();
    readout.ports[1].readout=mode;
    readout.ports[1].Cf=mode==ReadoutMode::IdealChargeAmplifier?.5:0.0;
    readout.ports[1].amplifierPolarity=mode==ReadoutMode::IdealChargeAmplifier?1:-1;
    readout.ports[1].Qref=mode==ReadoutMode::IdealChargeAmplifier?.02:0.0;
    readout.ports[1].VoutBias=mode==ReadoutMode::IdealChargeAmplifier?.3:0.0;
    readout.workpoint.ports={{"drive",0.0,0.0},{"sense",0.0,0.0}};
    truss.bindToModel(m);
    m.getProblemSetup().getStructuralPiezoAxialSetup().bindToModel(m);
    return m;
}
Bundle callbacks(const RModel &m) {return PiezoAxialBackendBridge::create(m).callbacks;}
QByteArray raw(const QString &path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error(qPrintable(f.errorString()));return f.readAll();
}
QString digest(const QString &path) {return QString::fromLatin1(QCryptographicHash::hash(raw(path),QCryptographicHash::Sha256).toHex());}
QByteArray bytes(const RModel &m) {return m.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState;}
RStructuralPiezoAxialRunState state(const RModel &m) {return Codec::decode(bytes(m));}
QString controlHash(const Bundle &cb) {return QString::fromLatin1(QCryptographicHash::hash(cb.backendIdentity.toUtf8(),QCryptographicHash::Sha256).toHex());}
NativeAcceptedCheckpointStateView view(const RModel &m,const Bundle &cb) {
    RModel certified(m);certified.getProblemSetup().setRestart(certified.getProblemSetup().getStructuralPiezoAxialRestart().available);
    const Run run(certified,cb);const auto &s=run.acceptedRuntime();
    return {s.completedFE,s.physical.time,s.dt,QCryptographicHash::hash(Codec::encode(s),QCryptographicHash::Sha256)};
}
QByteArray context(const QString &input,const QString &runtime,const Bundle &cb) {
    return json({{"schema","range.native-checkpoint-context/v1"},{"sourceSha256",input},{"modelSha256",input},
        {"controlSha256",controlHash(cb)},{"runtimeSha256",runtime},{"packageSha256",QString(64,'d')},
        {"policy","accepted-steps-or-sim-time-v1"},{"maxAcceptedSteps",10},{"maxSimulationSeconds",1.0},{"dtS",.01}});
}
std::shared_ptr<Store> participant(const QString &dir,const RModel &m,const Bundle &cb) {
    const QString input(64,'a'),runtime(64,'c');return std::make_shared<Store>(dir,context(input,runtime,cb),
        input,controlHash(cb),runtime,false,m,[cb](const RModel &candidate){return view(candidate,cb);});
}
QString failure(const std::function<void()> &f) {try{f();return {};}catch(const RError &e){return e.getMessage();}catch(const std::exception &e){return QString::fromUtf8(e.what());}}
QByteArray reference(const RModel &source,const Bundle &cb) {
    Run run(source,cb);while(run.hasRemainingSteps()){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);}return Codec::encode(run.acceptedRuntime());
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
    QProcess p;p.setProcessChannelMode(QProcess::MergedChannels);auto env=QProcessEnvironment::systemEnvironment();
    for(const auto &name:env.keys())if(name.startsWith("RANGE_NATIVE_") || name.startsWith("RANGE_RUNTIME_"))env.remove(name);
    p.setProcessEnvironment(env);p.start(executable(),args);
    if(!p.waitForStarted(10000))throw std::runtime_error(qPrintable(p.errorString()));
    if(!p.waitForFinished(60000)){p.kill();p.waitForFinished(5000);throw std::runtime_error("Native axial CLI timed out");}
    const auto diagnostics=p.readAll();if(p.exitStatus()!=QProcess::NormalExit)throw std::runtime_error(diagnostics.constData());return {p.exitCode(),diagnostics};
}
ProcessResult cli(const QString &input,const QString &dir,const QByteArray &ctx,uint budget=0,bool restart=false,const QStringList &extra={}) {
    QStringList args={"--file="+input,"--native-checkpoint-dir="+dir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx),
                      "--native-axial-steps="+QString::number(budget)};
    if(restart)args.append("--restart");args.append(extra);return cli(args);
}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestNativeAxialRun:public QObject {
    Q_OBJECT
private slots:
    void externalSampledTransaction();
    void sameSolverBudgetAndFrames_data();void sameSolverBudgetAndFrames();
    void everyDiskSplit_data();void everyDiskSplit();
    void checkpointFailures_data();void checkpointFailures();
    void checkpointIsLastPublication();
    void incompleteRollbackDisablesRetry();
    void outputFailureAndPostCommitRetry();
    void sourceMutationAndUnsupportedModes();
    void axialElectricalReadouts_data();void axialElectricalReadouts();
    void realCliSplitResume_data();void realCliSplitResume();
    void realCliPauseResume_data();void realCliPauseResume();
    void realCliInvalidInputs_data();void realCliInvalidInputs();
    void realCliOutputFailureAudit();
    void realCliFreshIgnoresAcceptedDisplay();
    void realCliCompleteAcceptedTrace();
};
void TestNativeAxialRun::sameSolverBudgetAndFrames_data() {
    QTest::addColumn<bool>("controlled");QTest::newRow("PI")<<true;QTest::newRow("control-off")<<false;
}
void TestNativeAxialRun::sameSolverBudgetAndFrames() try {
    QFETCH(bool,controlled);auto m=scene(controlled);QTemporaryDir dir;QVERIFY(dir.isValid());
    auto &policy=m.getProblemSetup().getParallelExecutionConfig();policy.setMode(RParallelExecutionMode::FixedThreads);policy.setRequestedThreads(4);
    m.getProblemSetup().getStructuralTrussDynamicsSetup().bindToModel(m);
    m.getProblemSetup().getStructuralPiezoAxialSetup().bindToModel(m);
    const auto cb=callbacks(m);const auto expected=reference(m,cb);
    RSolver solver(m,dir.filePath("result.rbm"),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);solver.setNativeAxialStepBudget(3);
    uint accepted=0,frames=0,reports=0;
    solver.setParallelExecutionReportObserver([&](const auto &r){
        if(r.appliedThreads!=1 || r.actualThreads!=1 || !r.deterministicReduction || r.requestedThreads!=4 || !r.fallback)
            throw std::runtime_error("Axial report did not expose deterministic serial physics");++reports;
    });
    solver.setResultFrameObserver([&](const RSolverResultFrame &frame){
        if(!QFileInfo(frame.actualRecordPath).isFile() || frame.recordNo!=++frames || frame.nativeAnalysis!="transient")
            throw std::runtime_error("Actual axial result frame mismatch");
        RModel disk;disk.read(frame.actualRecordPath);if(bytes(disk)!=bytes(m))throw std::runtime_error("Frame lacks complete axial runtime");
    });
    solver.setAcceptedStructuralStepObserver([&](const RModel &model){
        const auto s=state(model);if(s.completedFE!=++accepted || s.targetSteps!=8 || s.controlPhase!=accepted%3 ||
            s.controlTick!=(accepted+2)/3 || s.lastApplied.size()!=1 || s.readouts.size()!=2 || s.physical.reactions.size()!=9)
            throw std::runtime_error("Accepted physical/controller/electrical semantics mismatch");
    });
    solver.run();QCOMPARE(state(m).completedFE,3u);solver.run();QCOMPARE(state(m).completedFE,6u);
    solver.setNativeAxialStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);QCOMPARE(frames,8u);QCOMPARE(accepted,8u);QCOMPARE(reports,3u);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::everyDiskSplit_data() {
    QTest::addColumn<QString>("extension");QTest::newRow("ram")<<QString("ram");QTest::newRow("rbm")<<QString("rbm");
}
void TestNativeAxialRun::everyDiskSplit() try {
    QFETCH(QString,extension);const auto source=scene();const auto cb=callbacks(source);const auto expected=reference(source,cb);QTemporaryDir dir;
    for(uint split=0;split<=8;++split) {
        auto m=source;if(split){RSolver first(m,dir.filePath(QString("first-%1.rbm").arg(split)),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);first.setNativeAxialStepBudget(split);first.run();}
        else {const Run initialized(m,cb);m=initialized.acceptedModel(m);}
        const auto accepted=bytes(m);const auto path=dir.filePath(QString("restore-%1.%2").arg(split).arg(extension));
        const auto actual=m.nativeWritePaths(path,false).first();m.write(path,false);RModel disk;disk.read(actual);QCOMPARE(bytes(disk),accepted);
        disk.getProblemSetup().setRestart(true);RSolver resumed(disk,dir.filePath(QString("remaining-%1.rbm").arg(split)),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);resumed.run();
        QCOMPARE(bytes(disk),expected);QCOMPARE(state(disk).completedFE,8u);QCOMPARE(state(disk).controlTick,3u);
        QCOMPARE(disk.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),8u);
    }
} REPORT_EXCEPTIONS
void TestNativeAxialRun::checkpointFailures_data() {
    QTest::addColumn<int>("phase");
    for(Phase p:{Phase::BeforeModelWrite,Phase::AfterModelWrite,Phase::AfterModelReadBack,Phase::BeforeEnvelopeWrite,
        Phase::AfterEnvelopeWrite,Phase::AfterEnvelopeReadBack,Phase::BeforeLastValid,Phase::AfterLastValid,
        Phase::BeforeLatest,Phase::AfterLatest,Phase::AfterLatestReadBack,Phase::BeforeMarkerWrite,Phase::AfterMarkerWrite,Phase::BeforeMarkerRemove})
        QTest::newRow(qPrintable(QString::number(int(p))))<<int(p);
}
void TestNativeAxialRun::checkpointFailures() try {
    QFETCH(int,phase);QTemporaryDir dir;auto m=scene();const auto cb=callbacks(m);const auto expected=reference(m,cb);
    const auto output=dir.filePath("actual.rbm"),checkpointDir=dir.filePath("checkpoints");auto store=participant(checkpointDir,m,cb);
    RSolver solver(m,output,{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);solver.setAcceptedStepParticipant(store);solver.setNativeAxialStepBudget(1);solver.run();
    const auto old=bytes(m),oldLatest=raw(QDir(checkpointDir).filePath("latest.json")),oldOutput=raw(output);
    const auto lastPath=QDir(checkpointDir).filePath("lastvalid.json");const bool lastExisted=QFileInfo::exists(lastPath);
    const auto oldLast=lastExisted?raw(lastPath):QByteArray();const auto recovery=latestPath(checkpointDir),recoveryHash=digest(recovery);
    store->setPhaseHook(Phase(phase),[](Phase){throw std::runtime_error("injected axial checkpoint failure");});
    const auto diagnostic=failure([&]{solver.run();});QVERIFY2(diagnostic.contains("injected axial"),qPrintable(diagnostic));
    QCOMPARE(bytes(m),old);QCOMPARE(bytes(latest(checkpointDir)),old);QCOMPARE(raw(output),oldOutput);
    QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),oldLatest);QCOMPARE(QFileInfo::exists(lastPath),lastExisted);if(lastExisted)QCOMPARE(raw(lastPath),oldLast);
    QCOMPARE(digest(recovery),recoveryHash);QVERIFY(!QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(output,2)));
    QVERIFY(solver.isNativeAxialRetryable());QVERIFY(store->isRetryable());solver.run();QCOMPARE(state(m).completedFE,2u);
    solver.setNativeAxialStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);QCOMPARE(bytes(latest(checkpointDir)),expected);QCOMPARE(digest(recovery),recoveryHash);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::checkpointIsLastPublication() try {
    QTemporaryDir dir;auto m=scene();const auto cb=callbacks(m);const auto output=dir.filePath("result.rbm");
    auto store=participant(dir.filePath("checkpoints"),m,cb);RSolver solver(m,output,{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);
    solver.setAcceptedStepParticipant(store);solver.setNativeAxialStepBudget(1);solver.run();bool resultWasPublished=false,notified=false;
    store->setPhaseHook(Phase::BeforeLatest,[&](Phase){
        RModel written;written.read(output);if(state(written).completedFE!=2 || bytes(written)!=bytes(m))throw std::runtime_error("Recovery authority preceded actual result publication");
        const auto *old=store->committedReceipt();if(!old || old->envelope.value("acceptedSeq").toDouble()!=1)throw std::runtime_error("Participant promoted before LAST");
        resultWasPublished=true;
    });
    solver.setAcceptedStructuralStepObserver([&](const RModel &accepted){
        const auto *receipt=store->committedReceipt();if(!resultWasPublished || !receipt || receipt->envelope.value("acceptedSeq").toDouble()!=2 || state(accepted).completedFE!=2)
            throw std::runtime_error("Accepted observer preceded complete publication/promotions");notified=true;
    });
    solver.run();QVERIFY(resultWasPublished);QVERIFY(notified);QCOMPARE(state(m).completedFE,2u);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::incompleteRollbackDisablesRetry() try {
    QTemporaryDir dir;auto m=scene();const auto cb=callbacks(m);auto store=participant(dir.filePath("checkpoints"),m,cb);
    RSolver solver(m,dir.filePath("result.rbm"),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);
    solver.setAcceptedStepParticipant(store);solver.setNativeAxialStepBudget(1);solver.run();const auto old=bytes(m);
    store->setPhaseHook(Phase::AfterLatest,[&](Phase){
        store->setPhaseHook(Phase::BeforeRollbackLatest,[](Phase){throw std::runtime_error("injected rollback failure");});
        throw std::runtime_error("injected publication failure");
    });
    QVERIFY(failure([&]{solver.run();}).contains("incomplete rollback"));QCOMPARE(bytes(m),old);
    QVERIFY(!solver.isNativeAxialRetryable());QVERIFY(!store->isRetryable());
    QVERIFY(failure([&]{solver.run();}).contains("cannot retry"));QCOMPARE(bytes(m),old);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::outputFailureAndPostCommitRetry() try {
    QTemporaryDir dir;auto m=scene();const auto cb=callbacks(m);const auto expected=reference(m,cb);const auto output=dir.filePath("output.rbm");
    RSolver solver(m,output,{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);solver.setNativeAxialStepBudget(1);solver.run();const auto old=bytes(m),oldOutput=raw(output);
    const auto blocked=RFileUtils::getFileNameWithTimeStep(output,2);QVERIFY(QDir().mkpath(blocked));QVERIFY(!failure([&]{solver.run();}).isEmpty());
    QCOMPARE(bytes(m),old);QCOMPARE(raw(output),oldOutput);QVERIFY(solver.isNativeAxialRetryable());QVERIFY(QDir().rmdir(blocked));
    solver.setAcceptedStructuralStepObserver([](const RModel &){throw std::runtime_error("postcommit observer failure");});
    QVERIFY(failure([&]{solver.run();}).contains("postcommit"));QCOMPARE(state(m).completedFE,2u);
    solver.setAcceptedStructuralStepObserver({});solver.setNativeAxialStepBudget(0);solver.run();QCOMPARE(bytes(m),expected);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::sourceMutationAndUnsupportedModes() try {
    QTemporaryDir dir;const auto source=scene();const auto cb=callbacks(source);auto m=source;const auto output=dir.filePath("mutation.rbm");
    RSolver solver(m,output,{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);
    solver.setParallelExecutionReportObserver([&](const auto &){m.getProblemSetup().getStructuralTrussDynamicsSetup().activeMembers[0].coupling+=1;});
    QVERIFY(!failure([&]{solver.run();}).isEmpty());QVERIFY(bytes(m).isEmpty());QVERIFY(!QFileInfo::exists(output));m=source;solver.setParallelExecutionReportObserver({});
    QVERIFY(!failure([&]{solver.setStructuralSupportResponseObserver([](const auto &){});}).isEmpty());
    QVERIFY(!failure([&]{solver.setStructuralControlCallback("legacy","revision",[](const auto &){return RStructuralControlDecision{};});}).isEmpty());
    QVERIFY(!failure([&]{solver.setStructuralControlStepObserver([](const auto &){});}).isEmpty());
    QVERIFY(!failure([&]{solver.setNativeMixedStepBudget(1);}).isEmpty());QVERIFY(!failure([&]{solver.setNativeLayeredStepBudget(1);}).isEmpty());
    auto unsupported=source;unsupported.getMonitoringPointManager().push_back(RMonitoringPoint());
    QVERIFY(!failure([&]{RSolver rejected(unsupported,dir.filePath("monitoring.rbm"),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);}).isEmpty());
    unsupported=source;unsupported.getProblemSetup().getStructuralPiezoLayeredSetup().enabled=true;
    QVERIFY(!failure([&]{RSolver rejected(unsupported,dir.filePath("layered.rbm"),{},RSolver::ExecutionPolicy::AllRegisteredSolvers,{},cb);}).isEmpty());
    solver.setNativeAxialStepBudget(1);solver.run();QCOMPARE(state(m).completedFE,1u);
    m.getProblemSetup().getStructuralPiezoAxialSetup().enabled=false;QVERIFY(!failure([&]{solver.run();}).isEmpty());
} REPORT_EXCEPTIONS
void TestNativeAxialRun::axialElectricalReadouts_data() {
    QTest::addColumn<int>("modeValue");
    QTest::newRow("open-circuit-voltage")<<int(ReadoutMode::OpenCircuitVoltage);
    QTest::newRow("short-circuit-charge")<<int(ReadoutMode::ShortCircuitCharge);
    QTest::newRow("ideal-charge-amplifier")<<int(ReadoutMode::IdealChargeAmplifier);
}
void TestNativeAxialRun::axialElectricalReadouts() try {
    QFETCH(int,modeValue);const auto mode=ReadoutMode(modeValue);auto source=sensorReadoutScene(mode);
    QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("axial-readout.rbm");
    source.write(path,false);RModel model;model.read(path);
    const auto &persisted=model.getProblemSetup().getStructuralPiezoReadoutSetup();
    QCOMPARE(persisted.ports.size(),size_t(2));QVERIFY(persisted.ports[1].readout==mode);
    QCOMPARE(persisted.ports[1].Cf,mode==ReadoutMode::IdealChargeAmplifier?.5:0.0);
    QCOMPARE(model.getProblemSetup().getStructuralTrussDynamicsSetup().sourceFingerprint,
             source.getProblemSetup().getStructuralTrussDynamicsSetup().sourceFingerprint);

    auto cb=callbacks(model);Run run(model,cb);
    const auto actuator=run.adapter().maps().memberByPortId.at("drive");
    const auto sensorIndex=run.adapter().maps().memberByPortId.at("sense");
    for(int step=0;step<3;++step) {
        auto prepared=run.prepare(model);run.validatePrepared(*prepared);run.commit(*prepared);
        const auto &state=run.acceptedRuntime();QCOMPARE(state.readouts.size(),size_t(2));
        const auto &appliedDrive=std::get<RStructuralTrussVoltage>(state.physical.appliedInputs.memberInputs.at(actuator));
        QVERIFY(std::abs(appliedDrive.value)>1e-10);
        const auto &appliedSensor=state.physical.appliedInputs.memberInputs.at(sensorIndex);
        if(mode==ReadoutMode::OpenCircuitVoltage) QVERIFY(std::holds_alternative<RStructuralTrussCharge>(appliedSensor));
        else {QVERIFY(std::holds_alternative<RStructuralTrussVoltage>(appliedSensor));QCOMPARE(std::get<RStructuralTrussVoltage>(appliedSensor).value,0.0);}

        const auto found=std::find_if(state.readouts.begin(),state.readouts.end(),[](const auto &r){return r.portId=="sense";});
        QVERIFY(found!=state.readouts.end());const auto &actual=*found;
        const double dx=1.0+state.physical.displacements[6]-state.physical.displacements[3];
        const double dy=state.physical.displacements[7]-state.physical.displacements[4];
        const double dz=state.physical.displacements[8]-state.physical.displacements[5];
        const double extension=std::hypot(dx,std::hypot(dy,dz))-1.0;
        QVERIFY(std::abs(extension)>1e-10);
        // Independent one-port equations for the frozen rod: Q = C*V - G*dl,
        // with sensor C=.5 F and G=-.4 C/m; the source workpoint is zero.
        const double expectedCharge=-(-.4)*extension;
        const double expectedVoltage=(-.4)*extension/.5;
        QCOMPARE(actual.unit,mode==ReadoutMode::ShortCircuitCharge?QStringLiteral("C"):QStringLiteral("V"));
        QVERIFY(actual.mode==mode);
        const auto close=[](double a,double b){return std::isfinite(a)&&std::abs(a-b)<=2e-9*std::max({1.0,std::abs(a),std::abs(b)});};
        if(mode==ReadoutMode::OpenCircuitVoltage) {
            QVERIFY2(close(actual.value,expectedVoltage),qPrintable(QString("open V actual=%1 expected=%2").arg(actual.value,0,'g',17).arg(expectedVoltage,0,'g',17)));
            QVERIFY(close(actual.deltaValue,expectedVoltage));QVERIFY(close(actual.voltage,expectedVoltage));
            QVERIFY(close(actual.charge,0.0));QVERIFY(close(actual.deltaCharge,0.0));
        } else if(mode==ReadoutMode::ShortCircuitCharge) {
            QVERIFY2(close(actual.value,expectedCharge),qPrintable(QString("short Q actual=%1 expected=%2").arg(actual.value,0,'g',17).arg(expectedCharge,0,'g',17)));
            QVERIFY(close(actual.deltaValue,expectedCharge));QVERIFY(close(actual.charge,expectedCharge));
            QVERIFY(close(actual.deltaCharge,expectedCharge));QVERIFY(close(actual.voltage,0.0));
        } else {
            const double expectedOutput=.3+(expectedCharge-.02)/.5;
            const double expectedDelta=expectedCharge/.5;
            QVERIFY2(close(actual.value,expectedOutput),qPrintable(QString("amplifier V actual=%1 expected=%2").arg(actual.value,0,'g',17).arg(expectedOutput,0,'g',17)));
            QVERIFY(close(actual.deltaValue,expectedDelta));QVERIFY(close(actual.charge,expectedCharge));
            QVERIFY(close(actual.deltaCharge,expectedCharge));QVERIFY(close(actual.voltage,0.0));
        }
    }
    // A frozen FE run must reject a persisted source whose readout identity changed.
    model.getProblemSetup().getStructuralPiezoReadoutSetup().ports[1].VoutBias+=.01;
    QVERIFY(!failure([&]{auto stale=run.prepare(model);}).isEmpty());
} REPORT_EXCEPTIONS
void TestNativeAxialRun::realCliSplitResume_data() {everyDiskSplit_data();}
void TestNativeAxialRun::realCliSplitResume() try {
    QFETCH(QString,extension);QTemporaryDir dir;const auto source=scene();const auto cb=callbacks(source);const auto expected=reference(source,cb);
    for(uint split:{1u,2u,3u,5u,7u}) {
        const auto input=dir.filePath(QString("source-%1.%2").arg(split).arg(extension));source.write(input,false);const auto inputHash=digest(input);
        const auto checkpointDir=dir.filePath(QString("checkpoints-%1").arg(split));const auto ctx=context(inputHash,digest(executable()),cb);
        const auto first=cli(input,checkpointDir,ctx,split);QVERIFY2(first.code==0,first.diagnostics.constData());QCOMPARE(digest(input),inputHash);
        const auto firstModel=latest(checkpointDir);QCOMPARE(state(firstModel).completedFE,split);QCOMPARE(state(firstModel).targetSteps,8u);QCOMPARE(state(firstModel).controlPhase,split%3);
        const auto recovery=latestPath(checkpointDir),recoveryHash=digest(recovery);const auto resumed=cli(recovery,checkpointDir,ctx,0,true);QVERIFY2(resumed.code==0,resumed.diagnostics.constData());
        const auto final=latest(checkpointDir);QCOMPARE(bytes(final),expected);QCOMPARE(digest(input),inputHash);QCOMPARE(digest(recovery),recoveryHash);
        const auto last=state(final);QCOMPARE(last.completedFE,8u);QCOMPARE(last.controlTick,3u);QCOMPARE(last.targetSteps,8u);QCOMPARE(last.physical.time,.08);
        RModel work;work.read(QDir(checkpointDir).filePath("run-results/native-axial.rbm"));QCOMPARE(bytes(work),expected);
    }
} REPORT_EXCEPTIONS
void TestNativeAxialRun::realCliPauseResume_data() {
    QTest::addColumn<uint>("baseline");QTest::newRow("fresh")<<0u;QTest::newRow("restored")<<3u;
}
void TestNativeAxialRun::realCliPauseResume() try {
    QFETCH(uint,baseline);QTemporaryDir dir;const auto source=scene();const auto cb=callbacks(source);const auto expected=reference(source,cb);
    const auto input=dir.filePath("source.rbm"),checkpointDir=dir.filePath("checkpoints"),executionState=dir.filePath("execution.json");source.write(input,false);
    const auto ctx=context(digest(input),digest(executable()),cb);QString activeInput=input;
    if(baseline){const auto first=cli(input,checkpointDir,ctx,baseline);QVERIFY2(first.code==0,first.diagnostics.constData());activeInput=latestPath(checkpointDir);}
    const QString taskId="axial-pause-"+QUuid::createUuid().toString(QUuid::Id128),serverName=taskId+"-server";
    QLocalServer server;QVERIFY(server.listen(serverName));QProcess process;
    const auto cleanup=qScopeGuard([&]{if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(5000);}});
    auto env=QProcessEnvironment::systemEnvironment();for(const auto &name:env.keys())if(name.startsWith("RANGE_NATIVE_") || name.startsWith("RANGE_RUNTIME_"))env.remove(name);
    process.setProcessEnvironment(env);process.setProcessChannelMode(QProcess::MergedChannels);
    const uint pauseAt=baseline?5u:2u;
    QStringList args={"--file="+activeInput,"--native-checkpoint-dir="+checkpointDir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx),
        "--native-axial-steps=0","--pause-at-accepted-seq="+QString::number(pauseAt),"--native-execution-state="+executionState,
        "--task-id="+taskId,"--task-server="+serverName};if(baseline)args.append("--restart");
    process.start(executable(),args);QVERIFY(process.waitForStarted(10000));QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(),10000);
    auto *socket=server.nextPendingConnection();QVERIFY(socket);QJsonObject gate;QElapsedTimer timer;timer.start();
    while(timer.elapsed()<15000){if(QFileInfo::exists(executionState))gate=QJsonDocument::fromJson(raw(executionState)).object();
        if(gate.value("state").toString()=="paused" || process.state()==QProcess::NotRunning)break;QTest::qWait(10);}
    const auto diagnostic=process.readAll();QVERIFY2(gate.value("state").toString()=="paused",diagnostic.constData());
    QCOMPARE(gate.value("acceptedSeq").toDouble(),double(pauseAt));QCOMPARE(gate.value("timeS").toDouble(),double(pauseAt)*.01);
    QCOMPARE(gate.value("checkpointAcceptedSeq").toDouble(),double(pauseAt));QCOMPARE(gate.value("controlSha256").toString(),controlHash(cb));
    QCOMPARE(state(latest(checkpointDir)).completedFE,pauseAt);QCOMPARE(state(latest(checkpointDir)).targetSteps,8u);
    const auto pausedBytes=raw(executionState);
    auto command=[&](const QString &id){const QString message=id+":RESUME";QByteArray frame;QDataStream out(&frame,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_8);out<<quint32(message.size())<<message;
        return out.status()==QDataStream::Ok && socket->write(frame)==frame.size() && socket->waitForBytesWritten(5000);};
    QVERIFY(command(taskId+"-wrong"));QTest::qWait(50);QCOMPARE(raw(executionState),pausedBytes);
    QVERIFY(command(taskId));QVERIFY(process.waitForFinished(60000));const auto finished=process.readAll();QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode()==0,finished.constData());QCOMPARE(bytes(latest(checkpointDir)),expected);
    const auto resumed=QJsonDocument::fromJson(raw(executionState)).object();QCOMPARE(resumed.value("state").toString(),QString("resumed"));
    QCOMPARE(resumed.value("acceptedSeq").toDouble(),double(pauseAt));
} REPORT_EXCEPTIONS
void TestNativeAxialRun::realCliInvalidInputs_data() {
    QTest::addColumn<QString>("kind");for(const auto *name:{"bad-decimal","negative","overflow","mixed-budget","layered-budget","thermal-budget","missing-restart","changed-source","tampered-runtime","legacy-monitoring","wrong-control-hash","reserved-source"})QTest::newRow(name)<<QString::fromLatin1(name);
}
void TestNativeAxialRun::realCliInvalidInputs() try {
    QFETCH(QString,kind);QTemporaryDir dir;auto source=scene();const auto cb=callbacks(source);const auto input=dir.filePath("source.rbm");QStringList extra;
    if(kind=="legacy-monitoring")source.getMonitoringPointManager().push_back(RMonitoringPoint());
    if(kind=="changed-source" || kind=="tampered-runtime") {
        const Run initialized(source,cb);source=initialized.acceptedModel(source);
        if(kind=="changed-source") {
            auto &setup=source.getProblemSetup().getStructuralPiezoAxialSetup();auto recipe=QJsonDocument::fromJson(setup.recipeConfiguration).object();recipe["damping"]=QJsonObject{{"kind","none"}};setup.recipeConfiguration=json(recipe);setup.bindToModel(source);
            source.getProblemSetup().getStructuralPiezoAxialRestart().sourceFingerprint=setup.sourceFingerprint;
        } else {auto s=state(source);s.backendIdentity="changed-backend";source.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState=Codec::encode(s);}
        extra.append("--restart");
    }
    source.write(input,false);const auto before=digest(input),checkpointDir=dir.filePath("checkpoints");auto ctx=context(before,digest(executable()),cb);QString budget="1";
    if(kind=="bad-decimal")budget="01";if(kind=="negative")budget="-1";if(kind=="overflow")budget="4294967296";
    if(kind=="mixed-budget")extra.append("--native-mixed-steps=0");if(kind=="layered-budget")extra.append("--native-layered-steps=0");
    if(kind=="thermal-budget")extra.append("--thermal-steps=1");if(kind=="missing-restart")extra.append("--restart");
    if(kind=="wrong-control-hash"){auto parsed=QJsonDocument::fromJson(ctx).object();parsed["controlSha256"]=NativeAcceptedCheckpointContext::noControlSha256();ctx=json(parsed);}
    if(kind=="reserved-source") {
        const auto first=cli(input,checkpointDir,ctx,1);QVERIFY2(first.code==0,first.diagnostics.constData());const auto pointer=raw(QDir(checkpointDir).filePath("latest.json"));
        const auto result=cli(QDir(checkpointDir).filePath("run-results/native-axial.rbm"),checkpointDir,ctx,1,true);QVERIFY2(result.code!=0,result.diagnostics.constData());
        QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),pointer);QCOMPARE(digest(input),before);return;
    }
    QStringList args={"--file="+input,"--native-checkpoint-dir="+checkpointDir,"--native-checkpoint-context-json="+QString::fromUtf8(ctx),"--native-axial-steps="+budget};args.append(extra);
    const auto result=cli(args);QVERIFY2(result.code!=0,result.diagnostics.constData());QCOMPARE(digest(input),before);QVERIFY(!QFileInfo::exists(QDir(checkpointDir).filePath("latest.json")));
} REPORT_EXCEPTIONS
void TestNativeAxialRun::realCliOutputFailureAudit() try {
    QTemporaryDir dir;const auto source=scene();const auto cb=callbacks(source);const auto expected=reference(source,cb);
    const auto input=dir.filePath("source.rbm"),checkpointDir=dir.filePath("checkpoints");source.write(input,false);const auto ctx=context(digest(input),digest(executable()),cb);
    const auto first=cli(input,checkpointDir,ctx,1);QVERIFY2(first.code==0,first.diagnostics.constData());
    const auto recovery=latestPath(checkpointDir);const auto old=bytes(latest(checkpointDir)),pointer=raw(QDir(checkpointDir).filePath("latest.json"));
    const auto output=QDir(checkpointDir).filePath("run-results/native-axial.rbm");const auto oldOutput=raw(output);const auto blocked=RFileUtils::getFileNameWithTimeStep(output,2);QVERIFY(QDir().mkpath(blocked));
    const auto failed=cli(recovery,checkpointDir,ctx,1,true);QVERIFY2(failed.code!=0,failed.diagnostics.constData());QCOMPARE(bytes(latest(checkpointDir)),old);
    QCOMPARE(raw(output),oldOutput);QCOMPARE(raw(QDir(checkpointDir).filePath("latest.json")),pointer);
    const auto journals=QDir(QFileInfo(recovery).absolutePath()).entryList({QFileInfo(recovery).fileName()+".*.events.jsonl"},QDir::Files);
    QCOMPARE(journals.size(),1);QJsonObject failedPayload;
    for(const auto &line:raw(QDir(QFileInfo(recovery).absolutePath()).filePath(journals.first())).split('\n')) {
        const auto event=QJsonDocument::fromJson(line).object();if(event.value("type").toString()=="RunFailed")failedPayload=event.value("payload").toObject();
    }
    QVERIFY(failedPayload.value("nativeAxialAcceptedStateAvailable").toBool());QCOMPARE(failedPayload.value("lastAcceptedSequence").toDouble(),1.);
    QCOMPARE(failedPayload.value("lastAcceptedTimeS").toDouble(),.01);QVERIFY(QDir().rmdir(blocked));
    const auto resumed=cli(recovery,checkpointDir,ctx,0,true);QVERIFY2(resumed.code==0,resumed.diagnostics.constData());QCOMPARE(bytes(latest(checkpointDir)),expected);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::realCliFreshIgnoresAcceptedDisplay() try {
    QTemporaryDir dir;const auto source=scene();const auto cb=callbacks(source);const auto expected=reference(source,cb);
    auto accepted=source;{Run run(source,cb);for(uint i=0;i<3;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);}accepted=run.acceptedModel(source);}
    QCOMPARE(state(accepted).completedFE,3u);const auto input=dir.filePath("accepted-input.rbm"),checkpointDir=dir.filePath("fresh-checkpoints");accepted.write(input,false);
    const auto before=digest(input);const auto ctx=context(before,digest(executable()),cb);const auto fresh=cli(input,checkpointDir,ctx);
    QVERIFY2(fresh.code==0,fresh.diagnostics.constData());QCOMPARE(bytes(latest(checkpointDir)),expected);QCOMPARE(digest(input),before);
} REPORT_EXCEPTIONS
void TestNativeAxialRun::realCliCompleteAcceptedTrace() try {
    QTemporaryDir dir;QVERIFY(dir.isValid());const auto source=scene();const auto cb=callbacks(source);
    const auto expected=reference(source,cb);const auto input=dir.filePath("source.rbm");source.write(input,false);
    const auto originalHash=digest(input);const auto checkpoints=dir.filePath("compact-checkpoints");
    auto declared=QJsonDocument::fromJson(context(originalHash,digest(executable()),cb)).object();
    declared["maxAcceptedSteps"]=10;const auto ctx=json(declared);
    const auto first=cli(input,checkpoints,ctx,3,false,{"--native-accepted-trace","--log-quiet"});
    QVERIFY2(first.code==0,first.diagnostics.constData());QCOMPARE(digest(input),originalHash);
    const auto recovery=latestPath(checkpoints);const auto recoveryHash=digest(recovery);
    const auto resumed=cli(recovery,checkpoints,ctx,0,true,{"--native-accepted-trace","--log-quiet"});
    QVERIFY2(resumed.code==0,resumed.diagnostics.constData());QCOMPARE(digest(input),originalHash);
    QCOMPARE(digest(recovery),recoveryHash);QCOMPARE(bytes(latest(checkpoints)),expected);
    const auto pointer=QJsonDocument::fromJson(raw(QDir(checkpoints).filePath("latest.json"))).object();
    QCOMPARE(pointer["schema"].toString(),QString("range.native-checkpoint-pointer/v2"));
    const auto envelope=QJsonDocument::fromJson(raw(QDir(checkpoints).filePath(pointer["envelopeFile"].toString()))).object();
    QCOMPARE(envelope["acceptedSeq"].toInt(),8);QCOMPARE(envelope["modelAcceptedSeq"].toInt(),8);
    const auto c=envelope["traceCursor"].toObject();PiezoAcceptedTrace::Cursor cursor;
    cursor.byteEnd=qint64(c["byteEnd"].toDouble());cursor.lastRecordOffset=qint64(c["lastRecordOffset"].toDouble());
    cursor.sequence=quint64(c["sequence"].toDouble());cursor.recordSha32=QByteArray::fromHex(c["recordSha256"].toString().toLatin1());
    cursor.stateSha32=QByteArray::fromHex(c["stateSha256"].toString().toLatin1());
    QFile trace(QDir(checkpoints).filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));
    const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,cursor);QCOMPARE(prefix.records,quint64(9));
    QCOMPARE(prefix.terminal.payload,expected);QCOMPARE(prefix.header.baseSeq,quint64(0));
    const auto workingDirectory=QDir(QDir(checkpoints).filePath("run-results"));
    QVERIFY(QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(workingDirectory.filePath("native-axial.rbm"),3)));
    QVERIFY(QFileInfo::exists(RFileUtils::getFileNameWithTimeStep(workingDirectory.filePath("native-axial.rbm"),8)));
    QCOMPARE(workingDirectory.entryList({"*.rbm"},QDir::Files).size(),3);
    QVERIFY(!QFileInfo::exists(QDir(checkpoints).filePath("pending-publication.json")));
} REPORT_EXCEPTIONS

void TestNativeAxialRun::externalSampledTransaction() try {
    const auto runtime=PiezoNumericTransport::defaultRuntime();
    auto source=scene(true);auto &setup=source.getProblemSetup().getStructuralPiezoAxialSetup();auto recipe=QJsonDocument::fromJson(setup.recipeConfiguration).object();
    const auto script=QDir(runtime.second).absoluteFilePath("../../fea-solver/examples/external_io/pure_provider.py");
    const auto sdk=QDir(QFileInfo(script).absolutePath()).filePath("range_external_sdk.py");QVERIFY(QFileInfo::exists(script));QVERIFY(QFileInfo::exists(sdk));
    const QJsonObject connection{{"transport","stdio"},{"executable",runtime.first},
        {"args",QJsonArray{"-u",script}},{"dependencies",QJsonArray{sdk}},
        {"algorithm",QJsonObject{{"id","range-example-pure-provider"},{"version","1"}}},{"requestTimeout",3},{"handshakeTimeout",3}};
    auto controller=recipe["controller"].toObject();for(const auto *key:{"kp","ki","bias"})controller.remove(key);
    controller["kind"]="external";controller["stateSource"]="sensor_only";controller["selectedOutputs"]=QJsonArray{"tip:v"};
    controller["external"]=QJsonObject{{"connection",connection},{"initialState",QJsonObject{{"integral",0.}}}};recipe["controller"]=controller;
    recipe["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalSources",QJsonArray{QJsonObject{
        {"connection",connection},{"channelIds",QJsonArray{"node:123:X"}},{"sampleEveryFESteps",2},{"initialState",QJsonObject{{"samples","0"}}}}}}};
    setup.recipeConfiguration=json(recipe);setup.bindToModel(source);
    auto result=PiezoAxialBackendBridge::create(source,runtime.first,runtime.second);
    const auto post=result.callbacks.postFE;bool failOnce=true;result.callbacks.postFE=[&](const auto &physical,const auto &samples,uint step,uint tick,const auto &applied,const QByteArray &old){
        auto candidate=post(physical,samples,step,tick,applied,old);if(failOnce){failOnce=false;throw std::runtime_error("injected axial external postFE failure");}return candidate;
    };
    Run run(source,result.callbacks);const auto before=Codec::encode(run.acceptedRuntime());bool failed=false;
    try{run.prepare(source);}catch(const std::runtime_error &){failed=true;}QVERIFY(failed);QCOMPARE(Codec::encode(run.acceptedRuntime()),before);
    for(uint i=0;i<4;++i){
        const auto old=Codec::encode(run.acceptedRuntime());auto first=run.prepare(source);auto second=run.prepare(source);
        QCOMPARE(Codec::encode(first->runtime()),Codec::encode(second->runtime()));QCOMPARE(Codec::encode(run.acceptedRuntime()),old);
        const auto opaque=QJsonDocument::fromJson(second->runtime().opaqueControllerState).object();
        QCOMPARE(opaque["excitationStates"].toArray()[0].toObject()["samples"].toString(),QString::number(i/2+1));
        const double expected=std::sin(2*std::acos(-1.)*double((i/2)*2)*.01);
        QVERIFY(std::abs(second->result().appliedInputs.externalForces[6]-expected)<1e-14);
        run.validatePrepared(*second);run.commit(*second);run.certifyAcceptedRuntime(run.acceptedRuntime());
        auto saved=run.acceptedModel(source);saved.getProblemSetup().setRestart(true);
        auto resumedBridge=PiezoAxialBackendBridge::create(saved,runtime.first,runtime.second);Run resumed(saved,resumedBridge.callbacks);
        QCOMPARE(Codec::encode(resumed.acceptedRuntime()),Codec::encode(run.acceptedRuntime()));
        auto continued=run.prepare(source),restarted=resumed.prepare(saved);QCOMPARE(Codec::encode(continued->runtime()),Codec::encode(restarted->runtime()));
    }
} REPORT_EXCEPTIONS

QTEST_GUILESS_MAIN(TestNativeAxialRun)
#include "tst_native_axial_run.moc"
