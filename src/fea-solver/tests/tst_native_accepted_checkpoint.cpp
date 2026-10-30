#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QBuffer>
#include <QSysInfo>
#include <cstring>
#include <cmath>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <rbl_error.h>
#include "rml_model.h"
#include "native_accepted_checkpoint.h"

#ifdef RANGE_NATIVE_CHECKPOINT_TEST_HOOKS
namespace NativeAcceptedCheckpointTestHooks
{
using RenameFailureHook = void (*)(const QString &, QFileDevice::FileError);
void setRenameFailureHook(RenameFailureHook hook);
}
#endif

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
#ifdef Q_OS_WIN
class HeldRenameBlocker
{
public:
    explicit HeldRenameBlocker(const QString &path)
    {
        const std::wstring nativePath=QDir::toNativeSeparators(path).toStdWString();
        this->handle=CreateFileW(nativePath.c_str(),GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    ~HeldRenameBlocker(){this->close();}
    bool isValid() const{return this->handle!=INVALID_HANDLE_VALUE;}
    void close(){if(this->isValid()){CloseHandle(this->handle);this->handle=INVALID_HANDLE_VALUE;}}
private:
    HANDLE handle=INVALID_HANDLE_VALUE;
};
HeldRenameBlocker *renameBlockerToRelease=nullptr;
int observedLatestRenameFailures=0;
QFileDevice::FileError observedLatestRenameError=QFileDevice::NoError;
void releaseLatestAfterRenameFailure(const QString &path,QFileDevice::FileError error)
{
    if(QFileInfo(path).fileName()!=QStringLiteral("latest.json"))return;
    ++observedLatestRenameFailures;observedLatestRenameError=error;
    if(error==QFileDevice::RenameError && renameBlockerToRelease)renameBlockerToRelease->close();
}
#endif
void replaceBytes(const QString &path,const QByteArray &b)
{QFile f(path);if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate) || f.write(b)!=b.size() || !f.flush())throw std::runtime_error("Cannot replace test artifact");}
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
// Protocol-only witness; genuine axial/layered codec/physics coverage belongs
// to tst_native_piezo_accepted_trace, not this synthetic transaction fixture.
RModel traceModel(uint seq){auto m=model(seq,.01,12);m.getTimeSolver().setInputNTimeSteps(12);m.getTimeSolver().setInputTimeStepSize(.01);return m;}
QByteArray tracePayload(const RModel &m){const auto &r=m.getProblemSetup().getStructuralDynamicsRestartData();const auto seq=r.available?r.completedSteps:0;
    return QJsonDocument(QJsonObject{{"seq",double(seq)},{"time",double(seq)*.01},{"dt",.01},{"opaque","valid"}}).toJson(QJsonDocument::Compact);}
NativeAcceptedCheckpointStateView traceView(const QByteArray &payload){const auto o=QJsonDocument::fromJson(payload).object();
    const double seq=o.value("seq").toDouble(-1);if(o.size()!=4 || seq<0 || seq>12 || std::floor(seq)!=seq || o.value("time").toDouble(-1)!=seq*.01 ||
        o.value("dt").toDouble()!=.01 || o.value("opaque").toString()!="valid")throw std::runtime_error("Invalid synthetic accepted payload");
    return {quint64(seq),seq*.01,.01,QCryptographicHash::hash(payload,QCryptographicHash::Sha256)};}
Store::TraceOptions traceOptions(){Store::TraceOptions o;o.enabled=true;o.header.family="axial";o.header.sourceFingerprint=hash('a');
    o.header.configurationIdentity=hash('b');o.header.backendIdentity="protocol-only-witness/v1";o.header.targetSteps=12;o.header.dt=.01;
    o.extractPayload=tracePayload;o.certifyPayload=traceView;o.rebuildAcceptedModel=[](const RModel &,const QByteArray &payload){return traceModel(uint(traceView(payload).sequence));};return o;}
std::unique_ptr<Store> traceStore(const QString &path,Store::TraceOptions options=traceOptions(),bool restart=false,uint seq=0){return std::make_unique<Store>(path,context(.01),hash('b'),
    NativeAcceptedCheckpointContext::noControlSha256(),hash('c'),restart,traceModel(seq),[](const RModel &m){return traceView(tracePayload(m));},std::move(options));}
void traceCommit(Store &s,uint seq){auto p=s.stage(traceModel(seq),{seq,seq*.01,.01,seq==12});p->publish();p->commit();}
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
    void rollbackTamperedEvidenceFailsClosed();
    void recoveryPlanIsIdempotent_data();void recoveryPlanIsIdempotent();
    void corruptLatestFailsClosed();
    void directoryTamperRefused();
    void traceEveryStepAndPeriodicModel();
    void tracePublicationFailure_data();void tracePublicationFailure();
    void traceTruncateFailurePoisonsRecovery_data();void traceTruncateFailurePoisonsRecovery();
    void traceAdmissionAndFullPrefixGuards();
    void traceCertifiedPrefixRejectsRehashedOpaque();
    void traceRecoverySourceAndReadProofs_data();void traceRecoverySourceAndReadProofs();
    void traceImportedBaseSequenceAndAcceptedRestart();
    void traceImmutableModelPins_data();void traceImmutableModelPins();
    void traceRebuilderLateFileMutation_data();void traceRebuilderLateFileMutation();
    void traceLongPathBaselinePeriodicAndReopen();
    void checkpointWriterLockAdmission_data();void checkpointWriterLockAdmission();
    void checkpointAtomicRenameRetry();
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
void TestNativeAcceptedCheckpoint::traceEveryStepAndPeriodicModel()
{
    QTemporaryDir dir;auto s=traceStore(dir.path());QCOMPARE(s->readLatest().envelope["acceptedSeq"].toInt(),0);
    const auto baselinePath=s->readLatest().modelPath;
    for(uint i=1;i<=12;++i){traceCommit(*s,i);const auto *receipt=s->committedReceipt();QVERIFY(receipt);
        QCOMPARE(receipt->envelope["acceptedSeq"].toInt(),int(i));QCOMPARE(traceView(receipt->acceptedRuntime).sequence,quint64(i));
        QCOMPARE(receipt->modelSequence,i<10?quint64(0):i<12?quint64(10):quint64(12));
        if(i<10)QCOMPARE(receipt->modelPath,baselinePath);
        if(i!=10 && i!=12)QVERIFY(!receipt->acceptedModel);
    }
    const auto latest=s->readLatest();QVERIFY(latest.acceptedModel);QCOMPARE(latest.modelSequence,quint64(12));
    QCOMPARE(traceView(tracePayload(*latest.acceptedModel)).sequence,quint64(12));
    QCOMPARE(QDir(dir.path()).entryList({"checkpoint-*.rbm"},QDir::Files).size(),2);
    QFile trace(dir.filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));
    QCOMPARE(PiezoAcceptedTrace::verifyPrefix(trace,latest.traceCursor).records,quint64(13));
}
void TestNativeAcceptedCheckpoint::traceLongPathBaselinePeriodicAndReopen()
try {
    QTemporaryDir dir;QVERIFY(dir.isValid());QString path=dir.path();
    for(uint i=0;i<6;++i){const QString part=QStringLiteral("segment-%1-").arg(i)+QString(50,QLatin1Char('a'));
        QVERIFY(part.size()<255);path=QDir(path).filePath(part);}
    QVERIFY(path.size()>320);QVERIFY(QDir().mkpath(path));
    auto s=traceStore(path);const auto baseline=s->readLatest();QVERIFY(baseline.modelPath.size()>260);
    QCOMPARE(baseline.modelSequence,quint64(0));
    const QString lockPath=QDir(path).filePath(".native-checkpoint.lock");const auto ownerBytes=bytes(lockPath);
    QVERIFY_EXCEPTION_THROWN(traceStore(path,traceOptions(),true),std::exception);QCOMPARE(bytes(lockPath),ownerBytes);
    for(uint i=1;i<=10;++i)traceCommit(*s,i);const auto ten=s->readLatest();QVERIFY(ten.modelPath.size()>260);
    QCOMPARE(ten.modelSequence,quint64(10));QCOMPARE(traceView(ten.acceptedRuntime).sequence,quint64(10));
    s.reset();QVERIFY(!QFileInfo::exists(lockPath));s=traceStore(path,traceOptions(),true,10);QCOMPARE(s->readLatest().acceptedRuntime,ten.acceptedRuntime);
    traceCommit(*s,11);QCOMPARE(s->committedReceipt()->modelSequence,quint64(10));QVERIFY(!s->committedReceipt()->acceptedModel);
    traceCommit(*s,12);const auto final=s->readLatest();QCOMPARE(final.modelSequence,quint64(12));
    QCOMPARE(final.envelope["acceptedSeq"].toInt(),12);QVERIFY(final.modelPath.size()>260);
    QFile trace(QDir(path).filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));
    const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,final.traceCursor);QCOMPARE(prefix.records,quint64(13));
    QCOMPARE(prefix.terminal.payload,final.acceptedRuntime);
}
catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}
catch(const std::exception &error){QFAIL(error.what());}
void TestNativeAcceptedCheckpoint::checkpointWriterLockAdmission_data()
{
    QTest::addColumn<bool>("longPath");QTest::addColumn<QString>("owner");
    for(bool longPath:{false,true})for(const auto &owner:{"live-store","unrecognized-lock","foreign-host-lock"})
        QTest::newRow(qPrintable(QString("%1-%2").arg(longPath?"long":"short",owner)))<<longPath<<QString::fromLatin1(owner);
}
void TestNativeAcceptedCheckpoint::checkpointWriterLockAdmission()
{
    QFETCH(bool,longPath);QFETCH(QString,owner);QTemporaryDir dir;QVERIFY(dir.isValid());QString path=dir.path();
    if(longPath){for(uint i=0;i<6;++i)path=QDir(path).filePath(QString("segment-%1-").arg(i)+QString(50,QLatin1Char('a')));QVERIFY(path.size()>320);}
    QVERIFY(QDir().mkpath(path));const QString lockPath=QDir(path).filePath(".native-checkpoint.lock");
    if(owner=="live-store"){
        auto first=store(path);const auto original=bytes(lockPath);QVERIFY_EXCEPTION_THROWN(store(path),std::exception);
        QCOMPARE(bytes(lockPath),original);QVERIFY(first->isRetryable());first.reset();QVERIFY(!QFileInfo::exists(lockPath));
        auto next=store(path);QVERIFY(next->isRetryable());next.reset();QVERIFY(!QFileInfo::exists(lockPath));return;
    }
    const QByteArray original=owner=="unrecognized-lock"?QByteArray("unrecognized lock owner\n"):
        QByteArray("1\nfixture-owner\n")+(QSysInfo::machineHostName()+"-foreign-checkpoint-host").toUtf8()+"\n\n\n";
    replaceBytes(lockPath,original);QVERIFY_EXCEPTION_THROWN(store(path),std::exception);QCOMPARE(bytes(lockPath),original);
    QVERIFY(!QFileInfo::exists(QDir(path).filePath("latest.json")));QVERIFY(!QFileInfo::exists(QDir(path).filePath("pending-publication.json")));
}
void TestNativeAcceptedCheckpoint::checkpointAtomicRenameRetry()
{
#ifndef Q_OS_WIN
    QSKIP("Windows sharing-violation behavior only.");
#else
    QTemporaryDir transientDir;QVERIFY(transientDir.isValid());auto transient=store(transientDir.path());commit(*transient,1);
    HeldRenameBlocker transientBlocker(transientDir.filePath("latest.json"));QVERIFY(transientBlocker.isValid());
    observedLatestRenameFailures=0;observedLatestRenameError=QFileDevice::NoError;renameBlockerToRelease=&transientBlocker;
    NativeAcceptedCheckpointTestHooks::setRenameFailureHook(releaseLatestAfterRenameFailure);
    std::exception_ptr transientFailure;
    try{commit(*transient,2);}catch(...){transientFailure=std::current_exception();}
    NativeAcceptedCheckpointTestHooks::setRenameFailureHook(nullptr);renameBlockerToRelease=nullptr;
    if(transientFailure)std::rethrow_exception(transientFailure);
    QCOMPARE(observedLatestRenameFailures,1);QVERIFY(observedLatestRenameError==QFileDevice::RenameError);
    QVERIFY(!transientBlocker.isValid());
    QCOMPARE(transient->readLatest().envelope.value("acceptedSeq").toInt(),2);

    QTemporaryDir persistentDir;QVERIFY(persistentDir.isValid());auto persistent=store(persistentDir.path());commit(*persistent,1);
    const QString latestPath=persistentDir.filePath("latest.json");const auto originalLatest=bytes(latestPath);
    HeldRenameBlocker persistentBlocker(latestPath);QVERIFY(persistentBlocker.isValid());
    auto publication=persistent->stage(model(2),identity(2));
    QVERIFY_EXCEPTION_THROWN(publication->publish(),std::exception);
    QVERIFY(!publication->rollback());
    QCOMPARE(bytes(latestPath),originalLatest);
    QVERIFY(!persistent->isRetryable());
    QVERIFY(QFileInfo::exists(persistentDir.filePath("pending-publication.json")));
    persistentBlocker.close();
#endif
}
void TestNativeAcceptedCheckpoint::tracePublicationFailure_data()
{
    QTest::addColumn<int>("phase");
    for(auto phase:{Phase::BeforeEvidenceWrite,Phase::AfterEvidenceReadBack,Phase::BeforeMarkerWrite,Phase::AfterMarkerWrite,
        Phase::BeforeTraceAppend,Phase::AfterTraceAppend,Phase::AfterTraceRecordReadBack,Phase::BeforeModelWrite,Phase::AfterModelWrite,Phase::AfterModelReadBack,
        Phase::BeforeEnvelopeWrite,Phase::AfterEnvelopeWrite,Phase::AfterEnvelopeReadBack,Phase::BeforeLastValid,Phase::AfterLastValid,
        Phase::BeforeLatest,Phase::AfterLatest,Phase::AfterLatestReadBack,Phase::BeforeMarkerRemove})
        QTest::newRow(qPrintable(QString::number(int(phase))))<<int(phase);
}
void TestNativeAcceptedCheckpoint::tracePublicationFailure()
{
    QFETCH(int,phase);QTemporaryDir dir;auto s=traceStore(dir.path());for(uint i=1;i<10;++i)traceCommit(*s,i);
    const auto latest=bytes(dir.filePath("latest.json")),last=bytes(dir.filePath("lastvalid.json")),trace=bytes(dir.filePath("accepted-trace.rpt"));
    s->setPhaseHook(static_cast<Phase>(phase),[](Phase){throw std::runtime_error("Trace injected failure");});
    bool threw=false;std::unique_ptr<RAcceptedStepPublication> p;
    try{p=s->stage(traceModel(10),{10,.1,.01,false});p->publish();}catch(const std::exception &){threw=true;}
    QVERIFY(threw);if(p)QVERIFY(p->rollback());p.reset();QVERIFY(s->isRetryable());
    QCOMPARE(bytes(dir.filePath("latest.json")),latest);QCOMPARE(bytes(dir.filePath("lastvalid.json")),last);QCOMPARE(bytes(dir.filePath("accepted-trace.rpt")),trace);
    QVERIFY(!QFileInfo::exists(dir.filePath("pending-publication.json")));traceCommit(*s,10);QCOMPARE(s->readLatest().modelSequence,quint64(10));
}
void TestNativeAcceptedCheckpoint::traceTruncateFailurePoisonsRecovery_data()
{
    QTest::addColumn<int>("phase");for(auto phase:{Phase::BeforeTraceTruncate,Phase::AfterTraceTruncate,Phase::AfterTraceRollbackReadBack})
        QTest::newRow(qPrintable(QString::number(int(phase))))<<int(phase);
}
void TestNativeAcceptedCheckpoint::traceTruncateFailurePoisonsRecovery()
{
    QFETCH(int,phase);
    QTemporaryDir dir;auto s=traceStore(dir.path());
    const auto oldLatest=bytes(dir.filePath("latest.json"));
    const bool hadLastValid=QFileInfo::exists(dir.filePath("lastvalid.json"));
    const auto oldLastValid=hadLastValid?bytes(dir.filePath("lastvalid.json")):QByteArray();
    const auto oldTrace=bytes(dir.filePath("accepted-trace.rpt"));
    auto p=s->stage(traceModel(1),{1,.01,.01,false});
    s->setPhaseHook(Phase::AfterLatest,[&](Phase){s->setPhaseHook(static_cast<Phase>(phase),[](Phase){throw std::runtime_error("Trace truncate fault");});throw std::runtime_error("Publication fault");});
    QVERIFY_EXCEPTION_THROWN(p->publish(),std::exception);QVERIFY(!p->rollback());p.reset();QVERIFY(!s->isRetryable());
    QVERIFY(QFileInfo::exists(dir.filePath("pending-publication.json")));QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);
    QVERIFY(!s->rollbackEvidencePaths().isEmpty());s.reset();
    auto reopened=traceStore(dir.path(),traceOptions(),true,0);
    QVERIFY(!QFileInfo::exists(dir.filePath("pending-publication.json")));
    QCOMPARE(bytes(dir.filePath("latest.json")),oldLatest);
    QCOMPARE(QFileInfo::exists(dir.filePath("lastvalid.json")),hadLastValid);
    if(hadLastValid)QCOMPARE(bytes(dir.filePath("lastvalid.json")),oldLastValid);
    QCOMPARE(bytes(dir.filePath("accepted-trace.rpt")),oldTrace);
    QCOMPARE(reopened->readLatest().envelope.value("acceptedSeq").toInt(),0);
    traceCommit(*reopened,1);QCOMPARE(reopened->readLatest().envelope.value("acceptedSeq").toInt(),1);
}
void TestNativeAcceptedCheckpoint::traceAdmissionAndFullPrefixGuards()
{
    QTemporaryDir wrong,header,tail,payload;auto o=traceOptions();o.header.targetSteps=20001;
    auto oversized=traceModel(0);std::vector<double> times;for(uint i=1;i<=20001;++i)times.push_back(i*.01);
    oversized.getTimeSolver().setTimes(times);oversized.getTimeSolver().setInputNTimeSteps(20001);
    const auto oversizedStore=[&]{return std::make_unique<Store>(wrong.path(),context(.01),hash('b'),NativeAcceptedCheckpointContext::noControlSha256(),
        hash('c'),false,oversized,[](const RModel &m){return traceView(tracePayload(m));},o);};
    QVERIFY_EXCEPTION_THROWN(oversizedStore(),std::exception);QVERIFY(!QFileInfo::exists(wrong.filePath("accepted-trace.rpt")));
    o=traceOptions();o.header.contextSha256=QByteArray(32,'x');QVERIFY_EXCEPTION_THROWN(traceStore(wrong.path(),o),std::exception);
    o=traceOptions();o.initialHookPhase=Phase::AfterTraceHeaderReadBack;o.initialHook=[](Phase){throw std::runtime_error("Header readback fault");};
    QVERIFY_EXCEPTION_THROWN(traceStore(header.path(),o),std::exception);QVERIFY(!QFileInfo::exists(header.filePath("latest.json")));
    auto s=traceStore(tail.path());traceCommit(*s,1);QFile file(tail.filePath("accepted-trace.rpt"));QVERIFY(file.open(QIODevice::Append));QCOMPARE(file.write("tail"),qint64(4));file.close();
    QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);QVERIFY_EXCEPTION_THROWN(s->stage(traceModel(2),{2,.02,.01,false}),std::exception);
    QVERIFY(bytes(tail.filePath("accepted-trace.rpt")).endsWith("tail"));
    auto bad=traceOptions();bad.certifyPayload=[](const QByteArray &b){auto v=traceView(b);++v.sequence;return v;};
    QVERIFY_EXCEPTION_THROWN(traceStore(payload.path(),bad),std::exception);
}
void TestNativeAcceptedCheckpoint::traceCertifiedPrefixRejectsRehashedOpaque()
{
    QTemporaryDir dir;auto s=traceStore(dir.path());traceCommit(*s,1);traceCommit(*s,2);
    const auto latest=s->readLatest();QFile original(dir.filePath("accepted-trace.rpt"));QVERIFY(original.open(QIODevice::ReadOnly));
    const auto header=PiezoAcceptedTrace::readHeader(original);auto cursor=PiezoAcceptedTrace::initialCursor(header);
    QList<PiezoAcceptedTrace::Record> records;
    while(cursor.byteEnd<latest.traceCursor.byteEnd){const auto record=PiezoAcceptedTrace::readRecord(original,header,cursor,latest.traceCursor.byteEnd);records.append(record);cursor=record.cursor;}
    original.close();QCOMPARE(records.size(),3);
    // Rebuild the entire byte-valid chain and pointer proofs, while retaining
    // the unchanged base/full MODEL and terminal accepted runtime 2. This
    // reaches the per-record semantic certificate for historical runtime 1.
    QByteArray changed;QBuffer buffer(&changed);QVERIFY(buffer.open(QIODevice::ReadWrite));
    const auto rebuiltHeader=PiezoAcceptedTrace::writeHeader(buffer,header);QCOMPARE(rebuiltHeader.headerSha32,header.headerSha32);
    cursor=PiezoAcceptedTrace::initialCursor(rebuiltHeader);
    for(auto record:records){if(record.sequence==1){auto o=QJsonDocument::fromJson(record.payload).object();o["opaque"]="invalid";
            record.payload=QJsonDocument(o).toJson(QJsonDocument::Compact);}
        cursor=PiezoAcceptedTrace::append(buffer,rebuiltHeader,cursor,record.sequence,record.time,record.payload);}
    const auto prefix=PiezoAcceptedTrace::verifyPrefix(buffer,cursor);QCOMPARE(prefix.records,quint64(3));
    QCOMPARE(prefix.terminal.payload,latest.acceptedRuntime);buffer.close();replaceBytes(dir.filePath("accepted-trace.rpt"),changed);
    auto pointer=QJsonDocument::fromJson(bytes(dir.filePath("latest.json"))).object();const auto envelopePath=dir.filePath(pointer["envelopeFile"].toString());
    auto envelope=QJsonDocument::fromJson(bytes(envelopePath)).object();envelope["traceCursor"]=QJsonObject{{"byteEnd",double(cursor.byteEnd)},
        {"lastRecordOffset",double(cursor.lastRecordOffset)},{"sequence",double(cursor.sequence)},
        {"recordSha256",QString::fromLatin1(cursor.recordSha32.toHex())},{"stateSha256",QString::fromLatin1(cursor.stateSha32.toHex())}};
    const auto envelopeBytes=QJsonDocument(envelope).toJson(QJsonDocument::Compact);replaceBytes(envelopePath,envelopeBytes);
    pointer["envelopeSha256"]=QString::fromLatin1(QCryptographicHash::hash(envelopeBytes,QCryptographicHash::Sha256).toHex());
    replaceBytes(dir.filePath("latest.json"),QJsonDocument(pointer).toJson(QJsonDocument::Compact));
    bool semanticRejected=false;try{(void)s->readLatest();}catch(const std::exception &e){semanticRejected=QByteArray(e.what()).contains("Invalid synthetic accepted payload");}
    QVERIFY(semanticRejected);s.reset();QVERIFY_EXCEPTION_THROWN(traceStore(dir.path(),traceOptions(),true),std::exception);
}
void TestNativeAcceptedCheckpoint::traceRecoverySourceAndReadProofs_data()
{
    QTest::addColumn<QString>("mutation");
    for(const auto &name:{"source","configuration","backend","trace-header","trace-prefix","pointer-during-read","envelope-during-read","pending-during-read"})
        QTest::newRow(name)<<QString::fromLatin1(name);
}
void TestNativeAcceptedCheckpoint::traceRecoverySourceAndReadProofs()
{
    QFETCH(QString,mutation);QTemporaryDir dir;auto s=traceStore(dir.path());traceCommit(*s,1);
    if(mutation=="source" || mutation=="configuration" || mutation=="backend") {
        auto options=traceOptions();if(mutation=="source")options.header.sourceFingerprint=hash('e');
        if(mutation=="configuration")options.header.configurationIdentity=hash('e');if(mutation=="backend")options.header.backendIdentity="other-backend/v1";
        s.reset();QVERIFY_EXCEPTION_THROWN(traceStore(dir.path(),options,true),std::exception);return;
    }
    if(mutation=="trace-header" || mutation=="trace-prefix"){
        auto content=bytes(dir.filePath("accepted-trace.rpt"));QFile file(dir.filePath("accepted-trace.rpt"));QVERIFY(file.open(QIODevice::ReadOnly));
        const auto header=PiezoAcceptedTrace::readHeader(file);file.close();const auto index=mutation=="trace-header"?qint64(0):header.byteEnd+8;
        QVERIFY(index>=0 && index<content.size());content[int(index)]=char(content[int(index)]^1);replaceBytes(dir.filePath("accepted-trace.rpt"),content);
        QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);return;
    }
    s->setPhaseHook(Phase::AfterTracePrefixScan,[&](Phase){
        if(mutation=="pointer-during-read")replaceBytes(dir.filePath("latest.json"),bytes(dir.filePath("lastvalid.json")));
        if(mutation=="envelope-during-read"){auto p=QJsonDocument::fromJson(bytes(dir.filePath("latest.json"))).object();
            const auto path=dir.filePath(p["envelopeFile"].toString());replaceBytes(path,bytes(path)+" ");}
        if(mutation=="pending-during-read")replaceBytes(dir.filePath("pending-publication.json"),"{}");
    });
    QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);
}
void TestNativeAcceptedCheckpoint::traceImportedBaseSequenceAndAcceptedRestart()
{
    QTemporaryDir dir;auto options=traceOptions();options.header.baseSeq=5;
    auto s=traceStore(dir.path(),options,true,5);const auto base=s->readLatest();QCOMPARE(base.envelope["acceptedSeq"].toInt(),5);
    QCOMPARE(base.modelSequence,quint64(5));traceCommit(*s,6);const auto six=s->readLatest();
    QCOMPARE(six.modelSequence,quint64(5));QVERIFY(six.acceptedModel);s.reset();
    // Existing v2 may be opened with its current accepted MODEL, even though
    // immutable trace/header baseline remains 5 and periodic full MODEL is 5.
    s=traceStore(dir.path(),options,true,6);QCOMPARE(s->readLatest().acceptedRuntime,six.acceptedRuntime);
    traceCommit(*s,7);const auto latest=s->readLatest();QFile trace(dir.filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));
    const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,latest.traceCursor);QCOMPARE(prefix.header.baseSeq,quint64(5));QCOMPARE(prefix.records,quint64(3));
}
void TestNativeAcceptedCheckpoint::traceImmutableModelPins_data()
{
    QTest::addColumn<bool>("base");QTest::newRow("immutable-original-baseline")<<true;QTest::newRow("immutable-current-periodic")<<false;
}
void TestNativeAcceptedCheckpoint::traceImmutableModelPins()
{
    QFETCH(bool,base);QTemporaryDir dir;auto s=traceStore(dir.path());const QString basePath=s->readLatest().modelPath;
    for(uint i=1;i<=10;++i)traceCommit(*s,i);const QString target=base?basePath:s->readLatest().modelPath;
    const auto original=bytes(target),oldLatest=bytes(dir.filePath("latest.json")),oldTrace=bytes(dir.filePath("accepted-trace.rpt"));
    auto p=s->stage(traceModel(11),{11,.11,.01,false});bool changed=false;
    s->setPhaseHook(Phase::AfterLatest,[&](Phase){QFile f(target);if(!f.open(QIODevice::ReadWrite))throw std::runtime_error("Pinned write denied");
        const auto first=f.read(1);if(first.size()!=1 || !f.seek(0) || f.write(QByteArray(1,char(first[0]^1)))!=1 || !f.flush())throw std::runtime_error("Pinned mutation test failed");changed=true;});
    QVERIFY_EXCEPTION_THROWN(p->publish(),std::exception);const bool restored=p->rollback();p.reset();
    QCOMPARE(bytes(dir.filePath("latest.json")),oldLatest);QCOMPARE(bytes(dir.filePath("accepted-trace.rpt")),oldTrace);
#ifdef Q_OS_WIN
    QVERIFY(!changed);QVERIFY(restored);QVERIFY(s->isRetryable());QCOMPARE(bytes(target),original);
    QVERIFY(!QFileInfo::exists(dir.filePath("pending-publication.json")));traceCommit(*s,11);QCOMPARE(s->readLatest().modelSequence,quint64(10));
#else
    QVERIFY(changed);QVERIFY(!restored);QVERIFY(!s->isRetryable());QVERIFY(QFileInfo::exists(dir.filePath("pending-publication.json")));
    QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);
#endif
}
void TestNativeAcceptedCheckpoint::traceRebuilderLateFileMutation_data()
{
    QTest::addColumn<QString>("target");for(const auto &name:{"trace","base","current"})QTest::newRow(name)<<QString::fromLatin1(name);
}
void TestNativeAcceptedCheckpoint::traceRebuilderLateFileMutation()
{
    QFETCH(QString,target);QTemporaryDir dir;bool mutate=false,changed=false;QString path;auto options=traceOptions();const auto pure=options.rebuildAcceptedModel;
    options.rebuildAcceptedModel=[&](const RModel &m,const QByteArray &b){auto rebuilt=pure(m,b);if(mutate){QFile f(path);
            if(target=="trace"){if(!f.open(QIODevice::Append) || f.write("late")!=4 || !f.flush())throw std::runtime_error("Late trace mutation failed");changed=true;}
            else if(f.open(QIODevice::ReadWrite)){const auto first=f.read(1);if(first.size()!=1 || !f.seek(0) || f.write(QByteArray(1,char(first[0]^1)))!=1 || !f.flush())
                    throw std::runtime_error("Late MODEL mutation failed");changed=true;}}
        return rebuilt;};
    auto s=traceStore(dir.path(),options);const auto basePath=s->readLatest().modelPath;for(uint i=1;i<=10;++i)traceCommit(*s,i);
    const auto accepted=s->readLatest();path=target=="trace"?dir.filePath("accepted-trace.rpt"):target=="base"?basePath:accepted.modelPath;
    const auto original=bytes(path),pointer=bytes(dir.filePath("latest.json"));mutate=true;
#ifdef Q_OS_WIN
    if(target=="trace"){QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);QVERIFY(changed);}
    else {const auto verified=s->readLatest();QCOMPARE(verified.acceptedRuntime,accepted.acceptedRuntime);QVERIFY(!changed);QCOMPARE(bytes(path),original);}
#else
    QVERIFY_EXCEPTION_THROWN(s->readLatest(),std::exception);QVERIFY(changed);
#endif
    QCOMPARE(bytes(dir.filePath("latest.json")),pointer);QCOMPARE(s->committedReceipt()->acceptedRuntime,accepted.acceptedRuntime);
    if(target=="trace")QVERIFY(bytes(dir.filePath("accepted-trace.rpt")).endsWith("late"));QVERIFY(!QFileInfo::exists(dir.filePath("pending-publication.json")));
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
    // The fresh store validates both old immutable pointers before it removes
    // the marker; the candidate sequence 3 never becomes restart authority.
    auto reopened=std::make_unique<Store>(dir.path(),context(),hash('b'),
        NativeAcceptedCheckpointContext::noControlSha256(),hash('c'),true,model(2));
    QVERIFY(!QFileInfo::exists(dir.filePath("pending-publication.json")));
    QCOMPARE(bytes(dir.filePath("latest.json")),oldLatest);
    QCOMPARE(bytes(dir.filePath("lastvalid.json")),oldLast);
    QCOMPARE(reopened->readLatest().envelope.value("acceptedSeq").toInt(),2);
    for(const auto &path:paths) {
        if (QFileInfo(path).fileName() == QStringLiteral("pending-publication.json"))
            QVERIFY(!QFileInfo::exists(path));
        else
            QVERIFY(QFileInfo(path).isFile());
    }
}
void TestNativeAcceptedCheckpoint::rollbackTamperedEvidenceFailsClosed()
{
    QTemporaryDir dir;auto s=store(dir.path());commit(*s,1);commit(*s,2);
    auto p=s->stage(model(3),identity(3));
    s->setPhaseHook(Phase::AfterLatest,[&s](Phase){
        s->setPhaseHook(Phase::BeforeRollbackLatest,[](Phase){throw std::runtime_error("leave pending marker");});
        throw std::runtime_error("publication fault");
    });
    QVERIFY_EXCEPTION_THROWN(p->publish(),std::exception);QVERIFY(!p->rollback());p.reset();
    const auto marker=bytes(dir.filePath("pending-publication.json"));
    const auto candidateLatest=bytes(dir.filePath("latest.json"));
    const auto paths=s->rollbackEvidencePaths();QString latestBackup;
    for(const auto &path:paths)if(path.endsWith("-latest.pointer"))latestBackup=path;
    QVERIFY(!latestBackup.isEmpty());s.reset();
    const auto originalBackup=bytes(latestBackup);replaceBytes(latestBackup,originalBackup+" ");
    auto reopen=[&]{return std::make_unique<Store>(dir.path(),context(),hash('b'),
        NativeAcceptedCheckpointContext::noControlSha256(),hash('c'),true,model(3));};
    QVERIFY_EXCEPTION_THROWN(reopen(),std::exception);
    QCOMPARE(bytes(dir.filePath("pending-publication.json")),marker);
    QCOMPARE(bytes(dir.filePath("latest.json")),candidateLatest);
    QCOMPARE(bytes(latestBackup),originalBackup+" ");
}
void TestNativeAcceptedCheckpoint::recoveryPlanIsIdempotent_data()
{
    QTest::addColumn<int>("rollbackPhase");
    QTest::newRow("pointer-rollback-interrupted")<<int(Phase::BeforeRollbackLatest);
    QTest::newRow("trace-rollback-interrupted")<<int(Phase::AfterTraceTruncate);
}
void TestNativeAcceptedCheckpoint::recoveryPlanIsIdempotent()
{
    try {
    QFETCH(int,rollbackPhase);QTemporaryDir dir;auto s=traceStore(dir.path());
    const auto oldLatest=bytes(dir.filePath("latest.json"));
    const auto oldTrace=bytes(dir.filePath("accepted-trace.rpt"));
    auto publication=s->stage(traceModel(1),{1,.01,.01,false});
    s->setPhaseHook(Phase::AfterLatest,[&](Phase){
        s->setPhaseHook(static_cast<Phase>(rollbackPhase),[](Phase){throw std::runtime_error("interrupt rollback");});
        throw std::runtime_error("interrupt publication");
    });
    QVERIFY_EXCEPTION_THROWN(publication->publish(),std::exception);
    QVERIFY(!publication->rollback());publication.reset();s.reset();
    QVERIFY(QFileInfo::exists(dir.filePath("pending-publication.json")));

    NativeCheckpointRecovery::WriterLease lease(dir.path());QString error;
    auto plan=NativeCheckpointRecovery::Plan::inspect(dir.path(),context(.01),&error);
    QVERIFY2(plan.has_value(),qPrintable(error));QCOMPARE(plan->candidateSequence(),quint64(1));
    QCOMPARE(plan->contextSha256(),QString::fromLatin1(QCryptographicHash::hash(context(.01),QCryptographicHash::Sha256).toHex()));
    if (rollbackPhase == int(Phase::BeforeRollbackLatest)) {
        QVERIFY(!plan->finish(lease,&error));
        QVERIFY(QFileInfo::exists(dir.filePath("pending-publication.json")));
    }
    QVERIFY(plan->restore(lease,&error));
    QVERIFY(QFileInfo::exists(dir.filePath("pending-publication.json")));
    QCOMPARE(bytes(dir.filePath("latest.json")),oldLatest);
    QCOMPARE(bytes(dir.filePath("accepted-trace.rpt")),oldTrace);
    QVERIFY(plan->restore(lease,&error));
    QVERIFY(QFileInfo::exists(dir.filePath("pending-publication.json")));
    QCOMPARE(bytes(dir.filePath("latest.json")),oldLatest);
    QCOMPARE(bytes(dir.filePath("accepted-trace.rpt")),oldTrace);
    QVERIFY(plan->finish(lease,&error));
    QVERIFY(!QFileInfo::exists(dir.filePath("pending-publication.json")));
    auto acquireSecondLease=[&]{return NativeCheckpointRecovery::WriterLease(dir.path());};
    QVERIFY_EXCEPTION_THROWN(acquireSecondLease(),std::exception);
    } catch (const std::exception &exception) {
        QFAIL(exception.what());
    }
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
