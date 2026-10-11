#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <cstring>
#include <stdexcept>
#include "rml_model.h"
#include "native_accepted_checkpoint.h"

namespace {
using Store=NativeAcceptedCheckpointStore;
using Phase=Store::Phase;
QString hash(char c) { return QString(64,QLatin1Char(c)); }
QByteArray context(double dt=1.0)
{
    // Deliberately specify the public context keys independently of toJsonObject().
    return QJsonDocument(QJsonObject{{"schema","range.native-checkpoint-context/v1"},
        {"sourceSha256",hash('a')},{"modelSha256",hash('b')},
        {"controlSha256",NativeAcceptedCheckpointContext::noControlSha256()},
        {"runtimeSha256",hash('c')},{"packageSha256",hash('d')},
        {"policy","accepted-steps-or-sim-time-v1"},{"maxAcceptedSteps",10},
        {"maxSimulationSeconds",1.0},{"dtS",dt}}).toJson(QJsonDocument::Compact);
}
RModel model(uint seq,double dt=1.0,uint target=30)
{
    RModel m; m.addNode(RNode(0,0,0));m.addNode(RNode(1,0,0));
    RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,0);e.setNodeId(1,1);m.addElement(e,true,0);
    auto &d=m.getProblemSetup().getStructuralDynamicsSetup();d.setEnabled(true);d.setTimeStep(dt);d.setNSteps(target);
    auto &clock=m.getTimeSolver();std::vector<double> times;
    for(uint i=1;i<=target;++i)times.push_back(i*dt);
    clock.setEnabled(true);clock.setTimes(times);clock.setCurrentTimeStep(seq?seq-1:0);clock.setComputedTime(seq*dt);
    if(seq) {
        auto &s=m.getProblemSetup().getStructuralDynamicsRestartData();s.available=true;s.completedSteps=seq;
        s.time=seq*dt;s.fixedTimeStep=dt;s.meshNodeCount=2;s.meshElementCount=1;s.physicalDofCount=6;
        s.meshFingerprint=hash('a');s.dofFingerprint=hash('b');s.systemFingerprint=hash('c');
        s.q={1.2345678901234567+seq};s.v={-2.345678901234567};s.a={3.456789012345678};s.externalForce={4.567890123456789};
        s.initialMechanicalEnergy=5.67890123456789;s.cumulativeExternalWork=-6.789012345678901;
        s.cumulativeDampingDissipation=0.12345678901234567;s.validate();
    }
    return m;
}
RAcceptedStepIdentity identity(uint seq,double dt=1.0,uint target=30)
{ return {seq,seq*dt,dt,seq==target}; }
std::unique_ptr<Store> store(const QString &path,double dt=1.0,Store::StateViewExtractor extractor={})
{
    return std::make_unique<Store>(path,context(dt),hash('b'),NativeAcceptedCheckpointContext::noControlSha256(),
                                   hash('c'),false,model(0,dt),std::move(extractor));
}
QByteArray bytes(const QString &path)
{ QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read test artifact");return f.readAll(); }
void commit(Store &s,uint seq,double dt=1.0)
{ auto m=model(seq,dt);auto p=s.stage(m,identity(seq,dt));p->publish();p->commit(); }
NativeAcceptedCheckpointStateView independentView(const RModel &m)
{
    const auto &r=m.getProblemSetup().getStructuralDynamicsRestartData();r.validate();
    QByteArray encoded;
    // Simple independent state witness, unrelated to the production legacy serializer.
    for(double value:r.q) { char raw[sizeof(double)];std::memcpy(raw,&value,sizeof(value));encoded.append(raw,sizeof(raw)); }
    return {r.available?r.completedSteps:0,r.available?r.time:0,
            m.getProblemSetup().getStructuralDynamicsSetup().getTimeStep(),
            QCryptographicHash::hash(encoded,QCryptographicHash::Sha256)};
}
}
class TestNativeAcceptedCheckpoint : public QObject {
    Q_OBJECT
private slots:
    void contextContract();
    void tinyTimeStepContextMismatch();
    void intermediateInvocationFinalStep();
    void stagePublishRollback_data();void stagePublishRollback();
    void commitAndSequence();
    void injectedFailure_data();void injectedFailure();
    void noDueCommitAndStrictFirst();
    void legacyFirstCallback();
    void extractorFreezesCaller();
    void wrapperRetriesSameSequence();
    void rollbackFailureEvidence();
    void corruptLatestFailsClosed();
    void directoryTamperRefused();
};
void TestNativeAcceptedCheckpoint::contextContract()
{
    const auto c=NativeAcceptedCheckpointContext::parse(context());QCOMPARE(c.dtS,1.0);QCOMPARE(c.maxAcceptedSteps,10u);
    const auto original=QJsonDocument::fromJson(context()).object();QCOMPARE(c.toJsonObject(),original);
    auto modified=original;modified.insert("unknown",1);
    QVERIFY_EXCEPTION_THROWN(NativeAcceptedCheckpointContext::parse(QJsonDocument(modified).toJson(QJsonDocument::Compact)),std::exception);
    modified=original;modified.remove("packageSha256");
    QVERIFY_EXCEPTION_THROWN(NativeAcceptedCheckpointContext::parse(QJsonDocument(modified).toJson(QJsonDocument::Compact)),std::exception);
}
void TestNativeAcceptedCheckpoint::tinyTimeStepContextMismatch()
{
    constexpr double candidateDt=1.0e-14,contextDt=1.0e-13;
    QTemporaryDir constructorDir;QVERIFY(constructorDir.isValid());
    auto constructMismatch=[&] {
        return std::make_unique<Store>(constructorDir.path(),context(contextDt),hash('b'),
            NativeAcceptedCheckpointContext::noControlSha256(),hash('c'),false,
            model(0,candidateDt),independentView);
    };
    QVERIFY_EXCEPTION_THROWN(constructMismatch(),std::exception);
    QVERIFY(!QFileInfo::exists(constructorDir.filePath("latest.json")));
    QVERIFY(!QFileInfo::exists(constructorDir.filePath("lastvalid.json")));
    QTemporaryDir stageDir;QVERIFY(stageDir.isValid());auto s=store(stageDir.path(),contextDt,independentView);
    auto mismatched=model(1,candidateDt);
    QVERIFY_EXCEPTION_THROWN(s->stage(mismatched,identity(1,candidateDt)),std::exception);
    QVERIFY(!QFileInfo::exists(stageDir.filePath("latest.json")));
    QVERIFY(!QFileInfo::exists(stageDir.filePath("lastvalid.json")));
    QVERIFY(s->isRetryable());
    // Matching same-sequence stage still succeeds after the rejection.
    commit(*s,1,contextDt);
}
void TestNativeAcceptedCheckpoint::intermediateInvocationFinalStep()
{
    QTemporaryDir dir;QVERIFY(dir.isValid());constexpr double dt=.01;auto s=store(dir.path(),dt);
    auto candidate=model(1,dt,30);auto endOfInvocation=identity(1,dt,30);endOfInvocation.finalStep=true;
    // The invocation stops early, while the frozen physical run still targets 30 steps.
    auto p=s->stage(candidate,endOfInvocation);
    QVERIFY(!QFileInfo::exists(dir.filePath("latest.json")));p->publish();p->commit();p.reset();
    const auto verified=s->readLatest();QCOMPARE(verified.envelope.value("acceptedSeq").toInt(),1);
    RModel disk;disk.read(verified.modelPath);QCOMPARE(disk.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),30u);
    QCOMPARE(disk.getProblemSetup().getStructuralDynamicsRestartData().completedSteps,1u);
    const auto originalPointer=bytes(dir.filePath("latest.json"));
    auto beyond=model(2,dt,30);beyond.getTimeSolver().setTimes({dt});
    auto invalid=identity(2,dt,30);invalid.finalStep=true;
    QVERIFY_EXCEPTION_THROWN(s->stage(beyond,invalid),std::exception);
    auto zero=model(0,dt,30);QVERIFY_EXCEPTION_THROWN(s->stage(zero,{0,0,dt,true}),std::exception);
    QCOMPARE(bytes(dir.filePath("latest.json")),originalPointer);
    // At the actual last physical step, omitting finalStep must be rejected.
    QTemporaryDir finalDir;Store finalStore(finalDir.path(),context(dt),hash('b'),
        NativeAcceptedCheckpointContext::noControlSha256(),hash('c'),false,model(29,dt,30));
    auto physicalFinal=model(30,dt,30);auto missingFinal=identity(30,dt,30);missingFinal.finalStep=false;
    QVERIFY_EXCEPTION_THROWN(finalStore.stage(physicalFinal,missingFinal),std::exception);
    auto finalPublication=finalStore.stage(physicalFinal,identity(30,dt,30));
    finalPublication->publish();finalPublication->commit();
    QCOMPARE(finalStore.readLatest().envelope.value("acceptedSeq").toInt(),30);
}
void TestNativeAcceptedCheckpoint::stagePublishRollback_data()
{QTest::addColumn<int>("primed");QTest::newRow("both-missing")<<0;QTest::newRow("lastvalid-missing")<<1;QTest::newRow("both-present")<<2;}
void TestNativeAcceptedCheckpoint::stagePublishRollback()
{
    QFETCH(int,primed);QTemporaryDir dir;QVERIFY(dir.isValid());auto s=store(dir.path());
    for(int i=1;i<=primed;++i)commit(*s,uint(i));
    const auto latest=dir.filePath("latest.json"),last=dir.filePath("lastvalid.json");
    const bool hadLatest=QFileInfo::exists(latest),hadLast=QFileInfo::exists(last);
    const auto oldLatest=hadLatest?bytes(latest):QByteArray(),oldLast=hadLast?bytes(last):QByteArray();
    auto candidate=model(primed+1);auto p=s->stage(candidate,identity(primed+1));
    QCOMPARE(QFileInfo::exists(latest),hadLatest);if(hadLatest)QCOMPARE(bytes(latest),oldLatest);
    p->publish();QVERIFY(QFileInfo::exists(latest));QVERIFY(p->rollback());p.reset();
    QCOMPARE(QFileInfo::exists(latest),hadLatest);QCOMPARE(QFileInfo::exists(last),hadLast);
    if(hadLatest)QCOMPARE(bytes(latest),oldLatest);if(hadLast)QCOMPARE(bytes(last),oldLast);
    QVERIFY(s->isRetryable());commit(*s,primed+1);
    QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),primed+1);
}
void TestNativeAcceptedCheckpoint::commitAndSequence()
{
    QTemporaryDir dir;auto s=store(dir.path());auto candidate=model(1);auto p=s->stage(candidate,identity(1));
    QVERIFY(s->committedReceipt()==nullptr);p->publish();QVERIFY(s->committedReceipt()==nullptr);
    // Publication is visible on disk, but public recovery authority is guarded
    // until the caller crosses the accepted-state commit boundary.
    QVERIFY(QFileInfo::exists(dir.filePath("latest.json")));
    const auto pointer=QJsonDocument::fromJson(bytes(dir.filePath("latest.json"))).object();
    QVERIFY(!pointer.value("envelopeFile").toString().isEmpty());
    QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);
    p->commit();p.reset();QVERIFY(s->committedReceipt()!=nullptr);
    QCOMPARE(s->committedReceipt()->envelope.value("acceptedSeq").toInt(),1);
    QVERIFY_EXCEPTION_THROWN(s->stage(candidate,identity(1)),std::exception);
    commit(*s,2);QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),2);
    s.reset();
    Store reopened(dir.path(),context(),hash('b'),NativeAcceptedCheckpointContext::noControlSha256(),
                   hash('c'),true,model(2));
    QCOMPARE(reopened.readLatest().envelope.value("acceptedSeq").toInt(),2);
}
void TestNativeAcceptedCheckpoint::injectedFailure_data()
{
    QTest::addColumn<int>("phase");QTest::addColumn<int>("primed");
    const Phase phases[]={Phase::BeforeModelWrite,Phase::AfterModelWrite,Phase::AfterModelReadBack,
        Phase::BeforeEnvelopeWrite,Phase::AfterEnvelopeWrite,Phase::AfterEnvelopeReadBack,
        Phase::BeforeLastValid,Phase::AfterLastValid,Phase::BeforeLatest,Phase::AfterLatest,Phase::AfterLatestReadBack};
    const char *names[]={"before-model","after-model","model-readback","before-envelope","after-envelope","envelope-readback",
        "before-lastvalid","after-lastvalid","before-latest","after-latest","latest-readback"};
    for(int i=0;i<11;++i)for(int prime:{0,2}) {
        // lastvalid is only written when an older latest actually exists.
        if(prime==0 && (phases[i]==Phase::BeforeLastValid || phases[i]==Phase::AfterLastValid))continue;
        QTest::newRow(qPrintable(QString("%1-%2").arg(names[i]).arg(prime)))<<int(phases[i])<<prime;
    }
}
void TestNativeAcceptedCheckpoint::injectedFailure()
{
    QFETCH(int,phase);QFETCH(int,primed);QTemporaryDir dir;auto s=store(dir.path());
    for(int i=1;i<=primed;++i)commit(*s,uint(i));
    const auto latest=dir.filePath("latest.json"),last=dir.filePath("lastvalid.json");
    const bool hadLatest=QFileInfo::exists(latest),hadLast=QFileInfo::exists(last);
    const auto oldLatest=hadLatest?bytes(latest):QByteArray(),oldLast=hadLast?bytes(last):QByteArray();
    RModel accepted=model(primed),candidate=model(primed+1);
    const auto acceptedQ=accepted.getProblemSetup().getStructuralDynamicsRestartData().q;
    const auto candidateQ=candidate.getProblemSetup().getStructuralDynamicsRestartData().q;
    s->setPhaseHook(static_cast<Phase>(phase),[](Phase){throw std::runtime_error("injected one-shot publication failure");});
    bool threw=false;std::unique_ptr<RAcceptedStepPublication> p;
    try {p=s->stage(candidate,identity(primed+1));p->publish();}
    catch(const std::exception &){threw=true;}
    QVERIFY(threw);if(p){QVERIFY(p->rollback());p.reset();}
    QCOMPARE(QFileInfo::exists(latest),hadLatest);QCOMPARE(QFileInfo::exists(last),hadLast);
    if(hadLatest)QCOMPARE(bytes(latest),oldLatest);if(hadLast)QCOMPARE(bytes(last),oldLast);
    QVERIFY(accepted.getProblemSetup().getStructuralDynamicsRestartData().q==acceptedQ);
    QVERIFY(candidate.getProblemSetup().getStructuralDynamicsRestartData().q==candidateQ);
    QVERIFY(s->isRetryable());commit(*s,primed+1);
    QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),primed+1);
}
void TestNativeAcceptedCheckpoint::noDueCommitAndStrictFirst()
{
    QTemporaryDir dir;auto s=store(dir.path(),0.01);
    auto gap=model(3,0.01);QVERIFY_EXCEPTION_THROWN(s->stage(gap,identity(3,0.01)),std::exception);
    auto one=model(1,0.01);auto p=s->stage(one,identity(1,0.01));p->publish();
    QVERIFY(!QFileInfo::exists(dir.filePath("latest.json")));QVERIFY(p->rollback());p.reset();
    commit(*s,1,0.01);QVERIFY(!QFileInfo::exists(dir.filePath("latest.json")));
    QVERIFY_EXCEPTION_THROWN(s->stage(one,identity(1,0.01)),std::exception);
    for(uint i=2;i<=10;++i)commit(*s,i,0.01);
    QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),10);
}
void TestNativeAcceptedCheckpoint::legacyFirstCallback()
{
    QTemporaryDir dir;auto s=store(dir.path(),0.01);s->accept(model(10,0.01));
    QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),10);
    QVERIFY_EXCEPTION_THROWN(s->accept(model(12,0.01)),std::exception);
}
void TestNativeAcceptedCheckpoint::extractorFreezesCaller()
{
    QTemporaryDir dir;auto s=store(dir.path(),1.0,independentView);auto candidate=model(1);
    const auto expected=independentView(candidate).stateFingerprint32;auto p=s->stage(candidate,identity(1));
    candidate.getProblemSetup().getStructuralDynamicsRestartData().q[0]=999;
    p->publish();p->commit();p.reset();RModel restored;restored.read(s->readLatest().modelPath);
    QCOMPARE(independentView(restored).stateFingerprint32,expected);
    QCOMPARE(restored.getProblemSetup().getStructuralDynamicsRestartData().q[0],2.2345678901234567);
}
void TestNativeAcceptedCheckpoint::wrapperRetriesSameSequence()
{
    QTemporaryDir dir;auto s=store(dir.path());s->setPhaseHook(Phase::AfterLatest,[](Phase){throw std::runtime_error("wrapper injection");});
    auto one=model(1);QVERIFY_EXCEPTION_THROWN(s->accept(one),std::exception);QVERIFY(s->isRetryable());
    s->accept(one);QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),1);
}
void TestNativeAcceptedCheckpoint::rollbackFailureEvidence()
{
    QTemporaryDir dir;auto s=store(dir.path());commit(*s,1);commit(*s,2);
    const auto oldLatest=bytes(dir.filePath("latest.json")),oldLast=bytes(dir.filePath("lastvalid.json"));
    auto p=s->stage(model(3),identity(3));
    s->setPhaseHook(Phase::AfterLatest,[&s](Phase){
        s->setPhaseHook(Phase::BeforeRollbackLatest,[](Phase){throw std::runtime_error("rollback injection");});
        throw std::runtime_error("publish injection");
    });
    QVERIFY_EXCEPTION_THROWN(p->publish(),std::exception);QVERIFY(!p->rollback());p.reset();
    QVERIFY(!s->isRetryable());QVERIFY(!s->rollbackDiagnostic().isEmpty());
    QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);
    QVERIFY_EXCEPTION_THROWN(s->stage(model(3),identity(3)),std::exception);
    const auto paths=s->rollbackEvidencePaths();QVERIFY(paths.size()>=3);
    bool foundLatest=false,foundLast=false;
    for(const auto &path:paths){QVERIFY(QFileInfo(path).isFile());
        if(path.endsWith("-latest.pointer")){QCOMPARE(bytes(path),oldLatest);foundLatest=true;}
        if(path.endsWith("-lastvalid.pointer")){QCOMPARE(bytes(path),oldLast);foundLast=true;}
    }
    QVERIFY(foundLatest);QVERIFY(foundLast);
    s.reset();
    // The newly published sequence 3 bytes are internally valid. Reopening
    // them must nevertheless reject the durable unresolved-transaction marker.
    auto reopen=[&] {
        return std::make_unique<Store>(dir.path(),context(),hash('b'),
            NativeAcceptedCheckpointContext::noControlSha256(),hash('c'),true,model(3));
    };
    QVERIFY_EXCEPTION_THROWN(reopen(),std::exception);
    for(const auto &path:paths)QVERIFY(QFileInfo(path).isFile());
}
void TestNativeAcceptedCheckpoint::corruptLatestFailsClosed()
{
    QTemporaryDir dir;auto s=store(dir.path());commit(*s,1);commit(*s,2);
    {QFile orphan(dir.filePath("checkpoint-9999999999-orphan.rbm"));QVERIFY(orphan.open(QIODevice::WriteOnly));orphan.write("unverified orphan");}
    QCOMPARE(s->readLatest().envelope.value("acceptedSeq").toInt(),2);
    {QFile f(dir.filePath("latest.json"));QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write("{broken");}
    QVERIFY(QFileInfo::exists(dir.filePath("lastvalid.json")));QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);
}
void TestNativeAcceptedCheckpoint::directoryTamperRefused()
{
    QTemporaryDir dir;auto s=store(dir.path());
    QVERIFY(QDir().mkpath(dir.filePath("latest.json")));
    QVERIFY_EXCEPTION_THROWN(s->stage(model(1),identity(1)),std::exception);
    QVERIFY(QFileInfo(dir.filePath("latest.json")).isDir());
}
QTEST_APPLESS_MAIN(TestNativeAcceptedCheckpoint)
#include "tst_native_accepted_checkpoint.moc"
