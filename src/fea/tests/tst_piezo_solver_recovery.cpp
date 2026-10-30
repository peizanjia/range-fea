#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QSemaphore>
#include <QThreadPool>
#include <QEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QUuid>
#include <memory>
#include <rpiezo_axial_experiment_service.h>
#include <piezo_axial_backend_bridge.h>
#include <piezo_native_backend_bridge.h>
#include <piezo_runtime_identity.h>
#include <piezo_numeric_transport.h>
#include <piezo_accepted_trace.h>
#include <piezo_result_series.h>
#include <native_checkpoint_recovery.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include "application.h"
#include "session.h"
#include "solver_manager.h"
#include "model_io.h"
#include "model_command_host.h"

// Existing bounded Windows fixture verifies path, executable hash, direct-child
// ownership and child PID before terminating the paused solver.
bool terminateVerifiedOwnRecoverySolver(const QString &,QString *);
namespace {
class NativeQueuedLifecycleManager:public SolverManager
{
public:
    std::shared_ptr<QLockFile> originalRecoveryLease(const QString &jobId) const
    {
        return this->nativeRecoveryLogicalLocks.value(jobId);
    }
    QJsonObject persistenceDiagnostic(const QString &jobId) const
    {
        return this->nativeJobDiagnostics.value(jobId);
    }
    SolverTask *takeUnstartedTaskForDeletion(const QString &jobId)
    {
        QMutexLocker<QRecursiveMutex> guard(&this->jobsMutex);
        auto *task=this->nativeTasks.value(jobId).data();
        if(!task || !QThreadPool::globalInstance()->tryTake(task))return nullptr;
        this->runningJobs.removeAll(task);this->waitingJobs.removeAll(task);this->waitingParallelJobs.removeAll(task);
        this->jobIsStarting=false;return task;
    }
};
class NativeThreadPoolGateRunnable : public QRunnable
{
public:
    NativeThreadPoolGateRunnable(const std::shared_ptr<QSemaphore> &started,
                                 const std::shared_ptr<QSemaphore> &release)
        : started(started), release(release)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        started->release();
        release->tryAcquire(1, 120000);
    }

private:
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
};

class NativeThreadPoolGate
{
public:
    NativeThreadPoolGate()
        : pool(QThreadPool::globalInstance()), previousMaximum(pool->maxThreadCount()),
          started(std::make_shared<QSemaphore>()), release(std::make_shared<QSemaphore>())
    {
    }

    ~NativeThreadPoolGate()
    {
        release->release();
        pool->waitForDone(120000);
        if (maximumChanged) pool->setMaxThreadCount(previousMaximum);
    }

    bool start()
    {
        if (!pool->waitForDone(15000)) return false;
        pool->setMaxThreadCount(1);
        maximumChanged = true;
        pool->start(new NativeThreadPoolGateRunnable(started, release));
        return started->tryAcquire(1, 10000);
    }

    void releaseNow() { release->release(); }

private:
    QThreadPool *pool;
    int previousMaximum;
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
    bool maximumChanged = false;
};

QString fileSha256(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

bool checkpointDirectorySnapshot(const QString &directory, QMap<QString,QByteArray> *files,
                                 QString *error)
{
    if (!files || !QFileInfo(directory).isDir() || QFileInfo(directory).isSymLink())
    {
        if (error) *error = QStringLiteral("Checkpoint directory is unavailable or linked.");
        return false;
    }
    QMap<QString,QByteArray> snapshot;
    QDirIterator iterator(directory,QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const QString path=iterator.next();
        const QFileInfo info=iterator.fileInfo();
        if (info.isSymLink() || !info.isFile())
        {
            if (error) *error=QStringLiteral("Checkpoint snapshot encountered a linked or non-regular file.");
            return false;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            if (error) *error=file.errorString();
            return false;
        }
        const QByteArray bytes=file.readAll();
        if (file.error()!=QFileDevice::NoError)
        {
            if (error) *error=file.errorString();
            return false;
        }
        snapshot.insert(QDir(directory).relativeFilePath(path),bytes);
    }
    *files=std::move(snapshot);
    if (error) error->clear();
    return true;
}

QString checkpointSnapshotDifference(const QMap<QString,QByteArray> &expected,
                                     const QMap<QString,QByteArray> &actual)
{
    QStringList differences;
    for(auto it=expected.cbegin();it!=expected.cend();++it){
        if(!actual.contains(it.key()))differences.append(QStringLiteral("missing %1").arg(it.key()));
        else if(actual.value(it.key())!=it.value())differences.append(QStringLiteral("changed %1 %2->%3")
            .arg(it.key(),QString::fromLatin1(QCryptographicHash::hash(it.value(),QCryptographicHash::Sha256).toHex()),
                 QString::fromLatin1(QCryptographicHash::hash(actual.value(it.key()),QCryptographicHash::Sha256).toHex())));
    }
    for(auto it=actual.cbegin();it!=actual.cend();++it)if(!expected.contains(it.key()))differences.append(QStringLiteral("added %1").arg(it.key()));
    return differences.join(QLatin1Char(';'));
}

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"),QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"),QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"),operation},{QStringLiteral("parameters"),parameters}};
}

QJsonObject nativeRecoveryPreview(Session *session, const QString &jobId,
                                  const QJsonObject &checkpoint)
{
    return session->getModelCommandHost()->handle(hostRequest(QStringLiteral("command.preview"),
        {{QStringLiteral("command"),QStringLiteral("analysis.recover")},
         {QStringLiteral("commandVersion"),1},
         {QStringLiteral("arguments"),QJsonObject{
             {QStringLiteral("jobId"),jobId},
             {QStringLiteral("checkpointSha256"),checkpoint.value(QStringLiteral("checkpointSha256"))},
             {QStringLiteral("remainingSteps"),checkpoint.value(QStringLiteral("remainingSteps"))}}}}));
}

QJsonObject executeNativeRecoveryPreview(Session *session, const QString &previewId)
{
    return session->getModelCommandHost()->handle(hostRequest(QStringLiteral("command.execute"),
        {{QStringLiteral("previewId"),previewId},
         {QStringLiteral("idempotencyKey"),QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
}

bool publishTestNativeJobRecord(QString *recordPath, QJsonObject *candidate)
{
    if (!recordPath || !candidate || recordPath->isEmpty()) return false;
    const QString registryDirectory = QFileInfo(*recordPath).absolutePath();
    const QString previousHash = candidate->value(QStringLiteral("nativeJobRegistrySha256")).toString();
    const int nextRevision = candidate->value(QStringLiteral("nativeJobRegistryRevision")).toInt(-1) + 1;
    if (nextRevision <= 0 || previousHash.isEmpty()) return false;
    const bool immutableStore = QFileInfo(*recordPath).fileName() != QStringLiteral("job.json") ||
            !QDir(registryDirectory).entryList({QStringLiteral("commit-r*.marker")}, QDir::Files).isEmpty();
    const QString blobUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLower();
    const QString publishedPath = immutableStore
            ? QDir(registryDirectory).filePath(QStringLiteral("job-record-r%1-%2.json")
                .arg(nextRevision, 8, 10, QChar('0')).arg(blobUuid))
            : *recordPath;
    candidate->remove(QStringLiteral("nativeJobRegistrySha256"));
    candidate->remove(QStringLiteral("nativeJobRecordCommitMarkerRef"));
    candidate->insert(QStringLiteral("nativeJobRegistrySchema"), QStringLiteral("range.native-job-record/v1"));
    candidate->insert(QStringLiteral("nativeJobRegistryRevision"), nextRevision);
    candidate->insert(QStringLiteral("nativeJobRegistryPreviousSha256"), previousHash);
    candidate->insert(QStringLiteral("nativeJobRecordRef"), publishedPath);
    candidate->insert(QStringLiteral("nativeJobRegistrySha256"),
                      QString::fromLatin1(QCryptographicHash::hash(
                          QJsonDocument(*candidate).toJson(QJsonDocument::Compact),
                          QCryptographicHash::Sha256).toHex()));
    const QByteArray bytes = QJsonDocument(*candidate).toJson(QJsonDocument::Compact);
    if (immutableStore)
    {
        QFile blob(publishedPath);
        if (!blob.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            blob.write(bytes) != bytes.size() || !blob.flush()) return false;
        blob.close();
        const QString blobSha = fileSha256(publishedPath);
        const QString markerName = QStringLiteral("commit-r%1-%2-%3-%4.marker")
                .arg(nextRevision, 8, 10, QChar('0'))
                .arg(blobUuid, previousHash, blobSha);
        QFile marker(QDir(registryDirectory).filePath(markerName));
        if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) || !marker.flush()) return false;
        marker.close();
    }
    else
    {
        QSaveFile record(publishedPath);
        record.setDirectWriteFallback(false);
        if (!record.open(QIODevice::WriteOnly) || record.write(bytes) != bytes.size() || !record.commit())
            return false;
    }
    *recordPath = publishedPath;
    return true;
}

QJsonObject readObject(const QString &path);
bool persistAsInterruptedForTest(SolverManager &manager, const QString &jobId,
                                QString *registryRecordPath)
{
    const QJsonObject terminal = manager.jobQuery(jobId);
    QString recordPath = terminal.value(QStringLiteral("nativeJobRecordRef")).toString();
    if (recordPath.isEmpty()) return false;
    QFile persistedRecord(recordPath);
    if (!persistedRecord.open(QIODevice::ReadOnly)) return false;
    QJsonObject interrupted = QJsonDocument::fromJson(persistedRecord.readAll()).object();
    persistedRecord.close();
    if (interrupted.isEmpty()) return false;
    interrupted.insert(QStringLiteral("state"), QStringLiteral("interrupted"));
    interrupted.insert(QStringLiteral("resultArtifactVerified"), false);
    interrupted.insert(QStringLiteral("resultSourceBindingVerified"), false);
    interrupted.insert(QStringLiteral("resultReadbackComplete"), false);
    interrupted.insert(QStringLiteral("resultManifestDescriptorReported"), false);
    interrupted.insert(QStringLiteral("resultManifestVerified"), false);
    interrupted.insert(QStringLiteral("nativeResultCatalogAvailable"), false);
    interrupted.insert(QStringLiteral("resultSourceBinding"), QJsonObject{});
    interrupted.insert(QStringLiteral("resultArtifactSha256"), QJsonValue(QJsonValue::Null));
    interrupted.insert(QStringLiteral("resultManifestRef"), QJsonValue(QJsonValue::Null));
    interrupted.insert(QStringLiteral("resultManifestSha256"), QJsonValue(QJsonValue::Null));
    interrupted.insert(QStringLiteral("resultArtifactId"), QJsonValue(QJsonValue::Null));
    const QString simulatedOwner = QUuid::createUuid().toString(QUuid::WithoutBraces);
    interrupted.insert(QStringLiteral("ownerInstanceId"), simulatedOwner);
    interrupted.insert(QStringLiteral("ownerPid"), 2147483647);
    const QString ownerRoot = QDir::cleanPath(QDir(QFileInfo(recordPath).absolutePath())
        .absoluteFilePath(QStringLiteral("../../../native-job-owners")));
    interrupted.insert(QStringLiteral("ownerLockRef"),
                       QDir(ownerRoot).filePath(simulatedOwner + QStringLiteral(".lock")));
    if (!publishTestNativeJobRecord(&recordPath, &interrupted)) return false;
    if (registryRecordPath) *registryRecordPath = recordPath;
    return true;
}


QJsonObject readObject(const QString &path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(f.readAll()).object();
}
QString details(const QJsonObject &o) {return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));}
Model sourceModel(bool axial) {
    Model m;m.setName("Recovery actual FE");
    for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<2;++i){RElement e(axial?R_ELEMENT_TRUSS1:R_ELEMENT_BEAM1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
    m.getTimeSolver().setInputStartTime(.001);return m;
}
Model experiment(const QString &family,bool controlled=true) {
    const bool axial=family!="layered";auto m=sourceModel(axial);
    auto recipe=axial?RPiezoAxialExperimentService::exampleRecipe(m,{}):RPiezoExperimentService::exampleRecipe(m);
    recipe["replaceLegacyConditions"]=true;recipe["steps"]=12;
    if(controlled)recipe["controller"]=QJsonObject{{"kind","PI"},{"kp",.3},{"ki",.02},
        {"selectedOutputs",QJsonArray{"tip_displacement"}},{"bias",QJsonArray{.3}},
        {"controlEveryFESteps",3},{"voltageLower",QJsonArray{-.5}},{"voltageUpper",QJsonArray{.5}},{"deltaVoltageLimits",QJsonArray{.06}}};
    const auto result=axial?RPiezoAxialExperimentService::preview(m,recipe):RPiezoExperimentService::preview(m,recipe);
    if(!result.valid)throw RError(RError::Type::InvalidInput,R_ERROR_REF,"%s",qPrintable(result.message));
    static_cast<RModel&>(m)=result.candidate;
    if(family=="mixed")m.getProblemSetup().setStructuralPiezoAxialSetup({});
    return m;
}
void saveSource(Model &m,const QString &path){m.setFileName(path);ModelIO::save(m,path,false);}
void verifyPrivateNativeOutput(const QJsonObject &record,const QString &family)
{
    const QString inputDirectory=record["inputPackageRef"].toString();
    const QString runRoot=QDir::cleanPath(QDir(inputDirectory).absoluteFilePath(".."));
    QCOMPARE(QDir::cleanPath(record["nativeCheckpointDirectory"].toString()),QDir(runRoot).filePath("checkpoints"));
    QCOMPARE(QDir::cleanPath(record["resultArtifactRef"].toString()),QDir(runRoot).filePath("checkpoints/run-results/native-"+family+".rbm"));
    QCOMPARE(QDir::cleanPath(QFileInfo(record["inputSnapshotRef"].toString()).absolutePath()),QDir(runRoot).filePath("input"));
    QVERIFY(record["inputSnapshotRef"]!=record["resultArtifactRef"]);
    QCOMPARE(QDir::cleanPath(record["logArtifactRefs"].toObject()["nativeResultJournal"].toString()),QDir(runRoot).filePath("events/native-results.ndjson"));
    QCOMPARE(fileSha256(record["inputSnapshotRef"].toString()),record["inputSnapshotSha256"].toString());
    QCOMPARE(fileSha256(record["nativeCheckpointInputModelRef"].toString()),record["nativeCheckpointInputModelSha256"].toString());
    QCOMPARE(record["nativeCheckpointContext"].toObject()["packageSha256"],record["inputPackageSha256"]);
    const QString resultDirectory=QFileInfo(record["resultArtifactRef"].toString()).absolutePath();
    QVERIFY(QFileInfo(resultDirectory).isDir());QVERIFY(!QFileInfo(resultDirectory).isSymLink());
    QCOMPARE(QDir::cleanPath(QFileInfo(resultDirectory).canonicalFilePath()),QDir::cleanPath(resultDirectory));
}
void verifyCompactTrace(const QJsonObject &record,const QString &family,uint baseSeq,uint terminalSeq,const QJsonObject &pauseReceipt={})
{
    const QString directory=record["nativeCheckpointDirectory"].toString();const QString pointerPath=QDir(directory).filePath("latest.json");
    const auto pointer=readObject(pointerPath);QCOMPARE(pointer.size(),3);QCOMPARE(pointer["schema"].toString(),QString("range.native-checkpoint-pointer/v2"));
    const QString envelopePath=QDir(directory).filePath(pointer["envelopeFile"].toString());
    QCOMPARE(fileSha256(envelopePath),pointer["envelopeSha256"].toString());const auto envelope=readObject(envelopePath);
    QCOMPARE(envelope.size(),19);QCOMPARE(envelope["schema"].toString(),QString("range.native-checkpoint/v2"));QCOMPARE(envelope["acceptedSeq"].toInt(),int(terminalSeq));
    const auto serialized=envelope["traceCursor"].toObject();QCOMPARE(serialized.size(),5);
    PiezoAcceptedTrace::Cursor cursor;cursor.byteEnd=qint64(serialized["byteEnd"].toDouble());cursor.lastRecordOffset=qint64(serialized["lastRecordOffset"].toDouble());
    cursor.sequence=quint64(serialized["sequence"].toDouble());cursor.recordSha32=QByteArray::fromHex(serialized["recordSha256"].toString().toLatin1());
    cursor.stateSha32=QByteArray::fromHex(serialized["stateSha256"].toString().toLatin1());QCOMPARE(cursor.sequence,quint64(terminalSeq));
    QCOMPARE(QString::fromLatin1(cursor.stateSha32.toHex()),envelope["stateFingerprintSha256"].toString());
    QFile trace(QDir(directory).filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,cursor);
    QCOMPARE(prefix.records,quint64(terminalSeq-baseSeq+1));QCOMPARE(prefix.header.baseSeq,quint64(baseSeq));QCOMPARE(prefix.header.family,family);
    QCOMPARE(prefix.header.targetSteps,quint64(12));QCOMPARE(prefix.cursor.byteEnd,trace.size());
    QCOMPARE(QCryptographicHash::hash(prefix.terminal.payload,QCryptographicHash::Sha256),cursor.stateSha32);
    RModel original;original.read(record["nativeCheckpointInputModelRef"].toString());const auto source=rPiezoRuntimeCertify(original);
    QCOMPARE(source.completedSteps,quint64(baseSeq));QCOMPARE(source.sourceFingerprint,prefix.header.sourceFingerprint);
    const auto first=PiezoAcceptedTrace::readRecord(trace,prefix.header,PiezoAcceptedTrace::initialCursor(prefix.header),cursor.byteEnd);
    QCOMPARE(first.cursor.stateSha32,source.stateIdentity);
    const auto context=record["nativeCheckpointContext"].toObject();QCOMPARE(prefix.header.contextSha256,QCryptographicHash::hash(QJsonDocument(context).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256));
    QCOMPARE(QString::fromLatin1(prefix.header.headerSha32.toHex()),envelope["traceHeaderSha256"].toString());
    const QString modelPath=QDir(directory).filePath(envelope["modelFile"].toString());QVERIFY(QFileInfo(modelPath).isFile());
    QCOMPARE(fileSha256(modelPath),envelope["checkpointModelSha256"].toString());RModel full;full.read(modelPath);const auto fullState=rPiezoRuntimeCertify(full,true);
    QCOMPARE(fullState.completedSteps,quint64(envelope["modelAcceptedSeq"].toDouble()));
    QCOMPARE(fullState.stateIdentity,cursor.stateSha32); // Pause and final predicates persist this terminal full MODEL.
    if(!pauseReceipt.isEmpty()){
        QCOMPARE(pauseReceipt["state"].toString(),QString("paused"));QVERIFY(pauseReceipt["checkpointAvailable"].toBool());
        QCOMPARE(pauseReceipt["acceptedSeq"].toInt(),int(terminalSeq));QCOMPARE(pauseReceipt["checkpointAcceptedSeq"].toInt(),int(terminalSeq));
        QCOMPARE(pauseReceipt["checkpointModelSha256"],envelope["checkpointModelSha256"]);QCOMPARE(pauseReceipt["checkpointEnvelopeSha256"],pointer["envelopeSha256"]);
    }
    PiezoResultSeries::TraceProof proof;proof.baseModelPath=QDir(directory).filePath(prefix.header.baseModelFile);proof.tracePath=trace.fileName();
    proof.baseModelSha256=prefix.header.baseModelSha256;proof.expectedHeaderSha256=prefix.header.headerSha32;proof.expectedContextSha256=prefix.header.contextSha256;
    proof.expectedSourceFingerprint=source.sourceFingerprint;proof.cursor=cursor;
    const auto certified=PiezoResultSeries::readAcceptedTrace(proof,{"node:123:displacement:X"});QVERIFY2(certified.valid,qPrintable(certified.message));QVERIFY(certified.dataset);
    QCOMPARE(certified.dataset->completedFE.size(),int(prefix.records));QCOMPARE(certified.dataset->completedFE.first(),baseSeq);QCOMPARE(certified.dataset->completedFE.last(),terminalSeq);
    QCOMPARE(certified.dataset->channels.size(),1);QCOMPARE(certified.dataset->channels.first().channelId,QString("node:123:displacement:X"));
    QVERIFY(!certified.dataset->readouts.isEmpty());double endpoint=0;
    if(family=="axial")endpoint=RStructuralPiezoAxialRestartCodec::decode(prefix.terminal.payload).physical.displacements.at(6);
    else endpoint=RStructuralPiezoLayeredRestartCodec::decode(prefix.terminal.payload).physical.physicalDisplacements.at(12);
    const double actual=certified.dataset->columns.first().last();QCOMPARE(QByteArray(reinterpret_cast<const char *>(&actual),8),QByteArray(reinterpret_cast<const char *>(&endpoint),8));
    for(int i=0;i<certified.dataset->completedFE.size();++i){
        QCOMPARE(certified.dataset->completedFE[i],baseSeq+uint(i));const uint step=baseSeq+uint(i);
        const double expected=step?prefix.header.initialTime+double(step)*prefix.header.dt:prefix.header.initialTime;
        const double observed=certified.dataset->times[i];QCOMPARE(QByteArray(reinterpret_cast<const char *>(&observed),8),QByteArray(reinterpret_cast<const char *>(&expected),8));
    }
}
void verifyFinishedNativeBundle(SolverManager &manager,const QString &job,const QString &family)
{
    const auto record=manager.jobQuery(job);verifyPrivateNativeOutput(record,family);
    QVERIFY(record["resultArtifactVerified"].toBool());QVERIFY(record["resultManifestVerified"].toBool());
    const QString directory=QFileInfo(record["resultArtifactRef"].toString()).absolutePath();
    QCOMPARE(QDir::cleanPath(record["resultManifestRef"].toString()),QDir(directory).filePath("native-result.manifest.json"));
    const auto binding=record["resultSourceBinding"].toObject();
    QCOMPARE(QDir::cleanPath(binding["logicalSourcePath"].toString()),QDir::cleanPath(record["resultArtifactRef"].toString()));
    QCOMPARE(QDir::cleanPath(binding["resolvedSourcePath"].toString()),QDir(directory).filePath("native-"+family+"-00012.rbm"));
    QCOMPARE(binding["sourceArtifacts"].toArray().size(),2);
    QCOMPARE(fileSha256(record["resultArtifactRef"].toString()),record["resultArtifactSha256"].toString());
    const auto manifest=readObject(record["resultManifestRef"].toString());const auto frames=manifest["frames"].toArray();
    if(family!="mixed"){
        QCOMPARE(frames.size(),1);QCOMPARE(frames.first().toObject()["recordNo"].toInt(),12);
        QCOMPARE(fileSha256(binding["resolvedSourcePath"].toString()),frames.first().toObject()["sha256"].toString());
        const uint base=record["recovery"].toObject().isEmpty()?0u:uint(record["recovery"].toObject()["acceptedSeq"].toInt());
        verifyCompactTrace(record,family,base,12);
        const auto snapshot=manager.nativeResultQuery(job,"snapshot",{{"channelIds",QJsonArray{"node:123:displacement:X"}}});
        QVERIFY2(snapshot["valid"].toBool(),qPrintable(details(snapshot)));QCOMPARE(snapshot["frames"].toArray().size(),1);QCOMPARE(snapshot["frames"].toArray().first().toObject()["recordNo"].toInt(),12);
        QCOMPARE(snapshot["sourceSnapshotRef"].toObject(),record["nativeResultProvenance"].toObject()["sourceSnapshotRef"].toObject());
        return;
    }
    QCOMPARE(frames.size(),12);
    for(int i=0;i<frames.size();++i){const auto frame=frames[i].toObject();QCOMPARE(frame["recordNo"].toInt(),i+1);
        const QString name=QString("native-%1-%2.rbm").arg(family).arg(i+1,5,10,QChar('0'));
        QCOMPARE(frame["relativePath"].toString(),name);QCOMPARE(fileSha256(QDir(directory).filePath(name)),frame["sha256"].toString());}
    const auto snapshot=manager.nativeResultQuery(job,"snapshot",{{"channelIds",QJsonArray{"node:123:displacement:X"}}});
    QVERIFY2(snapshot["valid"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(snapshot).toJson(QJsonDocument::Compact))));
    QCOMPARE(snapshot["frames"].toArray().size(),12);
    for(int i=0;i<12;++i)QCOMPARE(snapshot["frames"].toArray()[i].toObject()["recordNo"].toInt(),i+1);
}
}

class TestPiezoSolverRecovery:public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        auto &manager=SolverManager::getInstance();
        manager.stopRunningTasks();
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,30000);
        Application::instance()->getSession()->clear();
    }
    void runtimeIdentityMatchesRealCallback_data() {
        QTest::addColumn<QString>("family");QTest::newRow("axial")<<QString("axial");QTest::newRow("layered")<<QString("layered");
    }
    void runtimeIdentityMatchesRealCallback() {
        QFETCH(QString,family);const auto m=experiment(family,false);
        const auto identity=family=="axial"?PiezoAxialBackendBridge::create(m).callbacks.backendIdentity:PiezoNativeBackendBridge::create(m).callbacks.backendIdentity;
        QCOMPARE(rPiezoRuntimeControlSha256(m),QCryptographicHash::hash(identity.toUtf8(),QCryptographicHash::Sha256));
        QVERIFY(rPiezoRuntimeControlSha256(m)!=QCryptographicHash::hash(QByteArrayLiteral("range.native-checkpoint.no-control.v1"),QCryptographicHash::Sha256));
        auto plain=experiment("layered",false);auto &s=plain.getProblemSetup().getStructuralPiezoLayeredSetup();
        s.excitationConfiguration.clear();s.controllerConfiguration.clear();s.designConfiguration.clear();s.bindToModel(plain);
        QCOMPARE(rPiezoRuntimeControlSha256(plain),QCryptographicHash::hash(QByteArrayLiteral("range.native-checkpoint.no-control.v1"),QCryptographicHash::Sha256));
        QCOMPARE(rPiezoRuntimeCertify(plain).completedSteps,quint64(0));
    }
    void unavailableExternalFunctionRejectsBeforeQueue_data() {runtimeIdentityMatchesRealCallback_data();}
    void unavailableExternalFunctionRejectsBeforeQueue() {
        QFETCH(QString,family);auto m=experiment(family,false);
        const auto paths=PiezoNumericTransport::defaultRuntime();
        PiezoNumericTransport transport(paths.first,paths.second);
        const auto runtime=transport.runtimeMetadata();
        QTemporaryDir directory;QVERIFY(directory.isValid());
        const QJsonObject excitation{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{QJsonObject{
            {"sourcePath",directory.filePath("missing_force.py")},{"functionName","force"},{"sourceSha256",QString(64,'0')},
            {"dependencyFiles",QJsonArray{}},{"environment",QJsonObject{{"pythonVersion",runtime["python"]},{"numpyVersion",runtime["numpy"]},{"scipyVersion",runtime["scipy"]}}},
            {"channels",QJsonArray{QJsonObject{{"channelId","node:123:X"},{"unit","N"}}}},{"missingPolicy","error"},{"cacheExactTime",false}}}}};
        if(family=="axial"){
            auto &s=m.getProblemSetup().getStructuralPiezoAxialSetup();auto r=QJsonDocument::fromJson(s.recipeConfiguration).object();r["excitation"]=excitation;
            s.recipeConfiguration=QJsonDocument(r).toJson(QJsonDocument::Compact);s.bindToModel(m);
        }else {auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();s.excitationConfiguration=QJsonDocument(excitation).toJson(QJsonDocument::Compact);s.bindToModel(m);}
        auto *session=Application::instance()->getSession();session->addModel(m);const uint id=session->getNModels()-1;
        const auto stamp=session->modelCommandStamp(id);auto &manager=SolverManager::getInstance();const int before=manager.jobsQuery().value("total").toInt();
        const auto validation=manager.validateRun(id);QVERIFY2(!validation["valid"].toBool(),qPrintable(details(validation)));
        const auto errors=details(validation);QVERIFY2(errors.contains("missing_force")||errors.contains("source")||errors.contains("file"),qPrintable(errors));
        QString error;QVERIFY(manager.submitValidated(id,{},true,&error).isEmpty());QVERIFY(!error.isEmpty());
        QCOMPARE(manager.jobsQuery().value("total").toInt(),before);QCOMPARE(session->modelCommandStamp(id),stamp);
        QVERIFY(!session->getModel(id).getProblemSetup().getStructuralPiezoAxialRestart().available);
        QVERIFY(!session->getModel(id).getProblemSetup().getStructuralPiezoLayeredRestart().available);
    }
    void terminalReportPersistsAfterQueuedTaskDestruction(){
        QTemporaryDir directory;QVERIFY(directory.isValid());auto source=experiment("axial",false);saveSource(source,directory.filePath("queued-close.rbm"));
        auto *session=Application::instance()->getSession();session->addModel(source);const uint id=session->getNModels()-1;
        NativeThreadPoolGate gate;QVERIFY(gate.start());NativeQueuedLifecycleManager manager;QString error;
        const QString job=manager.submitValidated(id,{},true,&error);QVERIFY2(!job.isEmpty(),qPrintable(error));
        auto *task=manager.takeUnstartedTaskForDeletion(job);QVERIFY(task);QPointer<SolverTask> lifetime(task);
        // The queued report owns immutable outcome fields. Destruction of its
        // sender must not prevent durable persistence or access freed lock data.
        task->terminalReported(job,"cancelled",false,false,-1,false,{}, {},false,"cancelled before child start");
        delete task;QVERIFY(lifetime.isNull());QCoreApplication::sendPostedEvents(&manager,QEvent::MetaCall);
        const auto terminal=manager.jobQuery(job);QCOMPARE(terminal["state"].toString(),QString("cancelled"));
        QVERIFY(!terminal["processStarted"].toBool());QVERIFY(!terminal["nativeJobRecordRef"].toString().isEmpty());
        QCOMPARE(readObject(terminal["nativeJobRecordRef"].toString())["state"].toString(),QString("cancelled"));
        QCOMPARE(manager.getNRunning(),0u);QCoreApplication::processEvents();QCOMPARE(manager.jobQuery(job)["state"].toString(),QString("cancelled"));
    }
    void recoveryLeaseSurvivesSenderDestructionUntilDurableTerminal_data(){
        QTest::addColumn<bool>("failPersistence");QTest::newRow("durable-terminal-releases-original-lease")<<false;
        QTest::newRow("blocked-terminal-retains-original-lease")<<true;
    }
    void recoveryLeaseSurvivesSenderDestructionUntilDurableTerminal(){
#ifndef Q_OS_WIN
        QSKIP("Verified paused direct-child interruption fixture is Windows-specific.");
#endif
        QFETCH(bool,failPersistence);QTemporaryDir directory;QVERIFY(directory.isValid());QString error;
        auto source=experiment("axial",true);saveSource(source,directory.filePath("lease-parent.rbm"));
        auto *session=Application::instance()->getSession();session->addModel(source);const uint sourceId=session->getNModels()-1;
        const auto sourceStamp=session->modelCommandStamp(sourceId);const QString sourceSha=fileSha256(source.getFileName());
        auto &producer=SolverManager::getInstance();QString parentJob;
        {
            NativeThreadPoolGate parentGate;QVERIFY(parentGate.start());
            parentJob=producer.submitValidated(sourceId,{},true,&error);QVERIFY2(!parentJob.isEmpty(),qPrintable(error));
            QVERIFY2(producer.requestNativeAcceptedBoundaryPause(parentJob,2,&error),qPrintable(error));
            const QString executionState=producer.jobQuery(parentJob)["nativeExecutionStateRef"].toString();parentGate.releaseNow();
            QTRY_COMPARE_WITH_TIMEOUT(readObject(executionState)["state"].toString(),QString("paused"),60000);
            QCOMPARE(readObject(executionState)["jobId"].toString(),parentJob);
            verifyCompactTrace(producer.jobQuery(parentJob),"axial",0,2,readObject(executionState));
            // Deliberate process loss is required to qualify recovery. This
            // fixture checks executable, direct parent, PID, nonce and pause file.
            QVERIFY2(terminateVerifiedOwnRecoverySolver(Application::instance()->getApplicationSettings()->getSolverPath(),&error),qPrintable(error));
            QTRY_COMPARE_WITH_TIMEOUT(producer.jobQuery(parentJob)["state"].toString(),QString("failed"),30000);
            QTRY_VERIFY_WITH_TIMEOUT(producer.getNRunning()==0 && producer.getNWaiting()==0,30000);
        }
        QVERIFY(persistAsInterruptedForTest(producer,parentJob,nullptr));
        NativeThreadPoolGate recoveryGate;QVERIFY(recoveryGate.start());
        auto manager=std::make_unique<NativeQueuedLifecycleManager>();const auto preview=manager->nativeJobRecoveryQuery(parentJob);
        QCOMPARE(preview["eligibility"].toString(),QString("recoverable"));const auto checkpoint=preview["checkpoint"].toObject();
        QCOMPARE(checkpoint["acceptedSeq"].toInt(),2);QCOMPARE(checkpoint["remainingSteps"].toInt(),10);
        const QString recoveryJob=manager->submitNativeRecovery(parentJob,checkpoint["checkpointSha256"].toString(),10,&error);
        QVERIFY2(!recoveryJob.isEmpty(),qPrintable(error));const auto queued=manager->jobQuery(recoveryJob);
        QCOMPARE(queued["recovery"].toObject()["recoverySourceJobId"].toString(),parentJob);
        auto originalLease=manager->originalRecoveryLease(recoveryJob);QVERIFY(originalLease);QVERIFY(originalLease->isLocked());
        const QString originalLockPath=originalLease->fileName();QLockFile peer(originalLockPath);peer.setStaleLockTime(0);QVERIFY(!peer.tryLock(0));
        auto *task=manager->takeUnstartedTaskForDeletion(recoveryJob);QVERIFY(task);QVERIFY(task->isNativeRecoveryTask());QVERIFY(task->hasNativeRecoveryLogicalLock());
        QPointer<SolverTask> taskLifetime(task);const QString recordPath=queued["nativeJobRecordRef"].toString();
        const QString initialRecordSha=fileSha256(recordPath);const auto initialDiskRecord=readObject(recordPath);QCOMPARE(initialDiskRecord["state"].toString(),QString("queued"));
        std::unique_ptr<QLockFile> publicationBarrier;
        if(failPersistence){
            publicationBarrier=std::make_unique<QLockFile>(QDir(QFileInfo(recordPath).absolutePath()).filePath(".job-record.lock"));
            publicationBarrier->setStaleLockTime(0);QVERIFY(publicationBarrier->tryLock(0));
        }
        task->terminalReported(recoveryJob,"cancelled",false,false,-1,false,{}, {},false,"cancelled before recovery child start");
        delete task;QVERIFY(taskLifetime.isNull());
        QCOMPARE(manager->originalRecoveryLease(recoveryJob).get(),originalLease.get());QVERIFY(originalLease->isLocked());
        QCoreApplication::sendPostedEvents(manager.get(),QEvent::MetaCall);
        if(failPersistence){
            QCOMPARE(manager->persistenceDiagnostic(recoveryJob)["code"].toString(),QString("NATIVE_JOB_PERSISTENCE_FAILED"));
            QCOMPARE(fileSha256(recordPath),initialRecordSha);QCOMPARE(readObject(recordPath),initialDiskRecord);
            QCOMPARE(manager->originalRecoveryLease(recoveryJob).get(),originalLease.get());QVERIFY(originalLease->isLocked());QVERIFY(!peer.tryLock(0));
            publicationBarrier.reset();
            // No successful terminal record is fabricated to clean up the
            // failed writer. Closing its owner releases its remaining lease.
            manager.reset();originalLease.reset();QVERIFY(peer.tryLock(0));peer.unlock();
            QCOMPARE(fileSha256(recordPath),initialRecordSha);QCOMPARE(readObject(recordPath)["state"].toString(),QString("queued"));
        }else{
            const auto terminal=manager->jobQuery(recoveryJob);QCOMPARE(terminal["state"].toString(),QString("cancelled"));
            QCOMPARE(readObject(terminal["nativeJobRecordRef"].toString())["state"].toString(),QString("cancelled"));
            QVERIFY(!manager->originalRecoveryLease(recoveryJob));QVERIFY(!originalLease->isLocked());
            QVERIFY(peer.tryLock(0));peer.unlock();QVERIFY(originalLease->tryLock(0));originalLease->unlock();
            manager.reset();originalLease.reset();
        }
        QCOMPARE(session->modelCommandStamp(sourceId),sourceStamp);QCOMPARE(fileSha256(source.getFileName()),sourceSha);
        verifyCompactTrace(producer.jobQuery(parentJob),"axial",0,2);
    }
    void frozenJobRetainsArtifactsAcrossSourceCloseAndNewProject(){
        QTemporaryDir oldProject,newProject;QVERIFY(oldProject.isValid());QVERIFY(newProject.isValid());QString error;
        auto *session=Application::instance()->getSession();QVERIFY2(session->openProject(oldProject.path(),&error),qPrintable(error));
        auto source=experiment("axial",true);saveSource(source,oldProject.filePath("original.rbm"));const QString sourceSha=fileSha256(source.getFileName());
        session->addModel(source);const uint sourceId=session->getNModels()-1;const auto sourceStamp=session->modelCommandStamp(sourceId);
        const auto frozen=session->createDocumentSnapshot(sourceId,&error);QVERIFY2(!frozen.isEmpty(),qPrintable(error));
        NativeThreadPoolGate gate;QVERIFY(gate.start());auto &manager=SolverManager::getInstance();
        const QString job=manager.submitValidated(sourceId,frozen,true,&error);QVERIFY2(!job.isEmpty(),qPrintable(error));
        const auto queued=manager.jobQuery(job);QVERIFY(queued["frozenSnapshotVerified"].toBool());
        QCOMPARE(queued["projectId"],frozen["projectId"]);QCOMPARE(queued["projectSnapshotRef"].toObject(),frozen);
        QCOMPARE(queued["sourceSnapshotRef"].toObject(),frozen);QVERIFY(frozen.contains("relativePath"));
        const auto publicSource=queued["nativeResultProvenance"].toObject()["sourceSnapshotRef"].toObject();QCOMPARE(publicSource.size(),6);
        QCOMPARE(publicSource["kind"].toString(),QString("project-native-input-snapshot/v1"));
        for(const auto &field:QStringList{"projectId","persistentDocumentId","sourceRevision","modelSha256","instanceRef"})QCOMPARE(publicSource[field],frozen[field]);
        for(auto field=publicSource.constBegin();field!=publicSource.constEnd();++field)QVERIFY(!field.key().contains("path",Qt::CaseInsensitive));
        QVERIFY(!QJsonDocument(publicSource).toJson(QJsonDocument::Compact).contains(frozen["relativePath"].toString().toUtf8()));
        QSignalSpy available(session,&Session::solverResultAvailable);
        // Change projects while the independent frozen task is still waiting
        // on the real worker pool; queued completion must use its old authority.
        session->clear();QVERIFY2(session->openProject(newProject.path(),&error),qPrintable(error));
        auto replacement=experiment("axial",false);saveSource(replacement,newProject.filePath("replacement.rbm"));session->addModel(replacement);
        const uint replacementId=session->getNModels()-1;const auto replacementStamp=session->modelCommandStamp(replacementId);
        QVERIFY(replacementStamp.first!=sourceStamp.first);const auto replacementState=rPiezoRuntimeCertify(session->getModel(replacementId));
        const uint replacementUndo=session->getModel(replacementId).getUndoStackSize();gate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(job)["state"].toString(),QString("succeeded"),120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,30000);QCoreApplication::processEvents();
        const auto finished=manager.jobQuery(job);QCOMPARE(finished["projectId"],queued["projectId"]);QCOMPARE(finished["nativeResultProvenance"],queued["nativeResultProvenance"]);
        QVERIFY(finished["resultArtifactVerified"].toBool());QVERIFY(finished["resultManifestVerified"].toBool());
        QVERIFY(QFileInfo(finished["resultArtifactRef"].toString()).isFile());QVERIFY(QFileInfo(finished["resultManifestRef"].toString()).isFile());
        QCOMPARE(session->modelCommandStamp(replacementId),replacementStamp);QCOMPARE(session->getModel(replacementId).getUndoStackSize(),replacementUndo);
        QVERIFY(rPiezoRuntimeSameAcceptedState(replacementState,rPiezoRuntimeCertify(session->getModel(replacementId))));
        QCOMPARE(fileSha256(source.getFileName()),sourceSha);QVERIFY(!available.isEmpty());
        bool retainedClosedSource=false;for(const auto &signal:available)if(signal[0].toString()==sourceStamp.first && !signal[3].toBool())retainedClosedSource=true;
        QVERIFY(retainedClosedSource);verifyFinishedNativeBundle(manager,job,"axial");
    }
    void guiCancellationReapsPausedCompactSolver_data(){runtimeIdentityMatchesRealCallback_data();}
    void guiCancellationReapsPausedCompactSolver(){
#ifndef Q_OS_WIN
        QSKIP("Native paused child qualification is Windows-specific.");
#endif
        QFETCH(QString,family);QTemporaryDir directory;QVERIFY(directory.isValid());auto model=experiment(family,true);
        saveSource(model,directory.filePath("cancel-paused.rbm"));const QString sourceSha=fileSha256(model.getFileName());
        auto *session=Application::instance()->getSession();session->addModel(model);const uint id=session->getNModels()-1;const auto stamp=session->modelCommandStamp(id);
        auto &manager=SolverManager::getInstance();QString error;NativeThreadPoolGate gate;QVERIFY(gate.start());
        const QString job=manager.submitValidated(id,{},true,&error);QVERIFY2(!job.isEmpty(),qPrintable(error));
        QVERIFY2(manager.requestNativeAcceptedBoundaryPause(job,2,&error),qPrintable(error));
        const QString pausePath=manager.jobQuery(job)["nativeExecutionStateRef"].toString();gate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(readObject(pausePath)["state"].toString(),QString("paused"),60000);
        // The child's pause file can precede delivery of its queued started
        // signal to the GUI. Observe the durable process identity before taking
        // the snapshot used to prove cancellation reaps that exact child.
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(job)["processStarted"].toBool()
                                 && manager.jobQuery(job)["nativeProcessId"].toString().toULongLong() > 0,30000);
        const auto paused=manager.jobQuery(job);verifyCompactTrace(paused,family,0,2,readObject(pausePath));
        const auto pointerBefore=readObject(QDir(paused["nativeCheckpointDirectory"].toString()).filePath("latest.json"));
        QVERIFY2(manager.cancelJob(job,&error),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(job)["state"].toString(),QString("cancelled"),30000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,30000);
        const auto cancelled=manager.jobQuery(job);QVERIFY(cancelled["processStarted"].toBool());QVERIFY(cancelled["processExited"].toBool());
        QVERIFY(cancelled["cancellationRequested"].toBool());QCOMPARE(cancelled["nativeProcessId"],paused["nativeProcessId"]);
        QCOMPARE(readObject(QDir(cancelled["nativeCheckpointDirectory"].toString()).filePath("latest.json")),pointerBefore);
        verifyCompactTrace(cancelled,family,0,2);QCOMPARE(session->modelCommandStamp(id),stamp);QCOMPARE(fileSha256(model.getFileName()),sourceSha);
        QVERIFY(!QFileInfo(cancelled["resultArtifactRef"].toString()).exists());
    }
    void guiCheckpointRecoveryPreservesOriginalTargetAndCompleteState_data() {
        QTest::addColumn<QString>("family");QTest::addColumn<bool>("controlled");QTest::addColumn<bool>("pending");QTest::addColumn<bool>("baselinePending");
        QTest::newRow("axial-open-loop")<<QString("axial")<<false<<false<<false;
        QTest::newRow("axial-open-loop-pending")<<QString("axial")<<false<<true<<false;
        QTest::newRow("axial-open-loop-pending-baseline")<<QString("axial")<<false<<true<<true;
        QTest::newRow("axial-PI")<<QString("axial")<<true<<false<<false;
        QTest::newRow("layered-PI")<<QString("layered")<<true<<false<<false;
        QTest::newRow("mixed")<<QString("mixed")<<false<<false<<false;
    }
    void guiCheckpointRecoveryPreservesOriginalTargetAndCompleteState() {
#ifndef Q_OS_WIN
        QSKIP("Verified paused direct-child termination fixture is Windows-specific.");
#endif
        QFETCH(QString,family);QFETCH(bool,controlled);QFETCH(bool,pending);QFETCH(bool,baselinePending);QTemporaryDir directory;QVERIFY(directory.isValid());
        const uint pauseSequence=baselinePending?1u:5u;
        auto continuous=experiment(family,controlled);auto interrupted=continuous;
        saveSource(continuous,directory.filePath("continuous.rbm"));saveSource(interrupted,directory.filePath("interrupted.rbm"));
        const QString continuousSourceSha=fileSha256(continuous.getFileName()),interruptedSourceSha=fileSha256(interrupted.getFileName());
        auto *session=Application::instance()->getSession();session->addModel(continuous);const uint referenceId=session->getNModels()-1;
        session->addModel(interrupted);const uint sourceId=session->getNModels()-1;
        auto &manager=SolverManager::getInstance();QString error;
        const auto validation=manager.validateRun(sourceId);QVERIFY2(validation["valid"].toBool(),qPrintable(details(validation)));
        const QString referenceJob=manager.submitValidated(referenceId,{},true,&error);QVERIFY2(!referenceJob.isEmpty(),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(referenceJob)["state"].toString(),QString("succeeded"),120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,30000);
        verifyFinishedNativeBundle(manager,referenceJob,family);
        QCOMPARE(fileSha256(directory.filePath("continuous.rbm")),continuousSourceSha);
        NativeThreadPoolGate gate;QVERIFY(gate.start());
        const QString job=manager.submitValidated(sourceId,{},true,&error);QVERIFY2(!job.isEmpty(),qPrintable(error));
        const auto queued=manager.jobQuery(job);QVERIFY2(queued["nativeCheckpointEnabled"].toBool(),qPrintable(details(queued)));
        verifyPrivateNativeOutput(queued,family);
        const auto context=queued["nativeCheckpointContext"].toObject();
        if(family!="mixed"){
            const auto authority=queued["nativeResultProvenance"].toObject()["executionContext"].toObject()["nativeAcceptedTrace"].toObject();
            QCOMPARE(authority.size(),4);QCOMPARE(authority["context"].toObject(),context);
            QCOMPARE(authority["inputModelPath"],queued["nativeCheckpointInputModelRef"]);
            QCOMPARE(authority["inputModelSha256"],queued["nativeCheckpointInputModelSha256"]);
        }
        QCOMPARE(context["controlSha256"].toString(),QString::fromLatin1(rPiezoRuntimeControlSha256(interrupted).toHex()));
        QVERIFY2(manager.requestNativeAcceptedBoundaryPause(job,pauseSequence,&error),qPrintable(error));
        const QString pausePath=manager.jobQuery(job)["nativeExecutionStateRef"].toString();gate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(readObject(pausePath)["state"].toString(),QString("paused"),60000);
        QCOMPARE(readObject(pausePath)["acceptedSeq"].toInt(),int(pauseSequence));QVERIFY(readObject(pausePath)["checkpointAvailable"].toBool());
        if(family=="mixed")QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(job)["resultFrames"].toArray().size(),int(pauseSequence),30000);
        const auto paused=manager.jobQuery(job);verifyPrivateNativeOutput(paused,family);
        QCOMPARE(paused["inputSnapshotSha256"],queued["inputSnapshotSha256"]);QCOMPARE(paused["inputPackageSha256"],queued["inputPackageSha256"]);
        if(family=="mixed"){
            const auto live=manager.nativeResultQuery(job,"catalog",{});
            QVERIFY2(live["valid"].toBool(),qPrintable(details(live)));QVERIFY(live["live"].toBool());QCOMPARE(live["frameCount"].toInt(),5);QCOMPARE(live["frames"].toArray().size(),5);
            QCOMPARE(live["sourceSnapshotRef"].toObject(),paused["nativeResultProvenance"].toObject()["sourceSnapshotRef"].toObject());
            const QString outputDirectory=QFileInfo(paused["resultArtifactRef"].toString()).absolutePath();
            for(int i=0;i<5;++i){const auto frame=live["frames"].toArray()[i].toObject();QCOMPARE(frame["recordNo"].toInt(),i+1);
                QCOMPARE(fileSha256(QDir(outputDirectory).filePath(frame["relativePath"].toString())),frame["sha256"].toString());}
            const auto series=manager.nativeResultQuery(job,"series",{{"channelId","node:123:displacement:X"}});
            QVERIFY2(series["valid"].toBool(),qPrintable(details(series)));QVERIFY(series["live"].toBool());QCOMPARE(series["samples"].toArray().size(),5);
            for(int i=0;i<5;++i){const auto sample=series["samples"].toArray()[i].toObject();QCOMPARE(sample["recordNo"].toInt(),i+1);
                RModel frameModel;frameModel.read(QDir(outputDirectory).filePath(live["frames"].toArray()[i].toObject()["relativePath"].toString()));
                QCOMPARE(rPiezoRuntimeCertify(frameModel,true).completedSteps,quint64(i+1));QVERIFY(sample["value"].isDouble());QVERIFY(std::isfinite(sample["value"].toDouble()));
                QCOMPARE(sample["value"].toDouble(),frameModel.getProblemSetup().getStructuralTrussDynamicsRestartData().q[6]);}
        }else{
            QCOMPARE(paused["resultFrames"].toArray().size(),0);QVERIFY(!QFileInfo(paused["resultArtifactRef"].toString()).exists());
            verifyCompactTrace(paused,family,0,pauseSequence,readObject(pausePath));
        }
        QCOMPARE(fileSha256(directory.filePath("interrupted.rbm")),interruptedSourceSha);
        QVERIFY2(terminateVerifiedOwnRecoverySolver(Application::instance()->getApplicationSettings()->getSolverPath(),&error),qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(job)["state"].toString(),QString("failed"),30000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning()==0 && manager.getNWaiting()==0,30000);
        QVERIFY(persistAsInterruptedForTest(manager,job,nullptr));
        const QString checkpointDirectory=queued["nativeCheckpointDirectory"].toString();
        if(pending){
            {
                NativeCheckpointRecovery::WriterLease staleLeaseCleanup(checkpointDirectory);
                QVERIFY(staleLeaseCleanup.ownsLease());
            }
            QVERIFY(!QFileInfo::exists(QDir(checkpointDirectory).filePath(".native-checkpoint.lock")));
        }
        QString previousCheckpointSha;
        QByteArray pendingMarkerBytes;
        QString pendingManifestPath;
        QByteArray pendingManifestOriginal,previousLatestPointerBytes,previousLastValidPointerBytes;
        bool previousLastValidExists=false;
        QMap<QString,QByteArray> pendingCheckpointSnapshot;
        if(pending){
            QCOMPARE(family,QString("axial"));QVERIFY(!QFileInfo::exists(QDir(checkpointDirectory).filePath("pending-publication.json")));
            const auto currentPointer=readObject(QDir(checkpointDirectory).filePath("latest.json"));
            QCOMPARE(currentPointer["schema"].toString(),QString("range.native-checkpoint-pointer/v2"));
            const QString currentEnvelopeName=currentPointer["envelopeFile"].toString();
            const QString paddedSequence=QStringLiteral("%1").arg(pauseSequence,10,10,QChar('0'));
            const QString envelopePattern=QStringLiteral("^envelope-%1-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.json$").arg(paddedSequence);
            QVERIFY2(QRegularExpression(envelopePattern).match(currentEnvelopeName).hasMatch(),qPrintable(currentEnvelopeName));
            QCOMPARE(readObject(QDir(checkpointDirectory).filePath(currentEnvelopeName))["acceptedSeq"].toInt(),int(pauseSequence));
            const auto checkpointContext=queued["nativeCheckpointContext"].toObject();
            const QString contextSha=QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(checkpointContext).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
            QString manifestName;QByteArray manifestBytes;QJsonObject rollbackManifest;
            const QStringList evidenceNames=QDir(checkpointDirectory).entryList({QStringLiteral("rollback-evidence-*.json")},QDir::Files,QDir::Name);
            for(const QString &candidateName:evidenceNames){
                QFile candidate(QDir(checkpointDirectory).filePath(candidateName));if(!candidate.open(QIODevice::ReadOnly))continue;
                const QByteArray bytes=candidate.readAll();const QJsonObject object=QJsonDocument::fromJson(bytes).object();
                if(object["schema"].toString()==QString("range.native-checkpoint-rollback-evidence/v2") &&
                   object["candidateSeq"].toInt(-1)==int(pauseSequence) && object["contextSha256"].toString()==contextSha &&
                   object["sourceSha256"].toString()==checkpointContext["sourceSha256"].toString()){
                    QVERIFY(manifestName.isEmpty());manifestName=candidateName;manifestBytes=bytes;rollbackManifest=object;
                }
            }
            QVERIFY2(!manifestName.isEmpty(),"The interrupted publication rollback manifest was not found.");
            QCOMPARE(QJsonDocument(rollbackManifest).toJson(QJsonDocument::Compact),manifestBytes);
            const QString token=manifestName.mid(QStringLiteral("rollback-evidence-").size(),36);
            const auto previousLatest=rollbackManifest["latest"].toObject();QVERIFY(previousLatest["exists"].toBool());
            QCOMPARE(previousLatest["file"].toString(),QString("rollback-%1-latest.pointer").arg(token));
            const QString previousLatestPath=QDir(checkpointDirectory).filePath(previousLatest["file"].toString());
            QCOMPARE(fileSha256(previousLatestPath),previousLatest["sha256"].toString());
            QFile previousLatestFile(previousLatestPath);QVERIFY(previousLatestFile.open(QIODevice::ReadOnly));
            previousLatestPointerBytes=previousLatestFile.readAll();const auto previousLatestPointer=QJsonDocument::fromJson(previousLatestPointerBytes).object();
            QCOMPARE(previousLatestPointer["schema"].toString(),QString("range.native-checkpoint-pointer/v2"));
            const auto previousEnvelope=readObject(QDir(checkpointDirectory).filePath(previousLatestPointer["envelopeFile"].toString()));
            QCOMPARE(previousEnvelope["acceptedSeq"].toInt(),int(pauseSequence-1));
            previousCheckpointSha=previousEnvelope["checkpointModelSha256"].toString();
            const auto previousLastValid=rollbackManifest["lastvalid"].toObject();previousLastValidExists=previousLastValid["exists"].toBool();
            if(previousLastValidExists){
                const QString expectedName=QString("rollback-%1-lastvalid.pointer").arg(token);
                QCOMPARE(previousLastValid["file"].toString(),expectedName);
                const QString previousLastValidPath=QDir(checkpointDirectory).filePath(expectedName);
                QCOMPARE(fileSha256(previousLastValidPath),previousLastValid["sha256"].toString());
                QFile previousLastValidFile(previousLastValidPath);QVERIFY(previousLastValidFile.open(QIODevice::ReadOnly));
                previousLastValidPointerBytes=previousLastValidFile.readAll();
            }
            const QByteArray manifestHash=QCryptographicHash::hash(manifestBytes,QCryptographicHash::Sha256).toHex();
            const QJsonObject marker{{"schema","range.native-checkpoint-pending-publication/v2"},
                {"candidateSeq",int(pauseSequence)},{"contextSha256",contextSha},{"rollbackManifestFile",manifestName},
                {"rollbackManifestSha256",QString::fromLatin1(manifestHash)}};
            pendingMarkerBytes=QJsonDocument(marker).toJson(QJsonDocument::Compact);
            pendingManifestPath=QDir(checkpointDirectory).filePath(manifestName);pendingManifestOriginal=manifestBytes;
            QSaveFile markerFile(QDir(checkpointDirectory).filePath("pending-publication.json"));markerFile.setDirectWriteFallback(false);
            QVERIFY(markerFile.open(QIODevice::WriteOnly));QCOMPARE(markerFile.write(pendingMarkerBytes),qint64(pendingMarkerBytes.size()));QVERIFY(markerFile.commit());
            QCOMPARE(fileSha256(QDir(checkpointDirectory).filePath("rollback-evidence-%1.json").arg(token)),QString::fromLatin1(manifestHash));
            QString snapshotError;QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&pendingCheckpointSnapshot,&snapshotError),qPrintable(snapshotError));
        }
        if(family!="mixed" && !pending){
            QFile trace(QDir(checkpointDirectory).filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));
            const auto header=PiezoAcceptedTrace::readHeader(trace);trace.close();
            const QString pointerPath=QDir(checkpointDirectory).filePath("latest.json");auto pointer=readObject(pointerPath);
            QCOMPARE(pointer["schema"].toString(),QString("range.native-checkpoint-pointer/v2"));
            const QString previousEnvelopePath=QDir(checkpointDirectory).filePath(pointer["envelopeFile"].toString());
            auto terminalEnvelope=readObject(previousEnvelopePath);QCOMPARE(terminalEnvelope["acceptedSeq"].toInt(),5);
            // The forced pause writes a full MODEL at step 5. Pin its original
            // base MODEL instead to exercise the valid between-write state:
            // latest raw runtime remains step 5 while the full MODEL is step 0.
            terminalEnvelope["modelFile"]=header.baseModelFile;terminalEnvelope["checkpointModelSha256"]=QString::fromLatin1(header.baseModelSha256.toHex());
            terminalEnvelope["modelAcceptedSeq"]=double(header.baseSeq);
            const QString envelopeName="recovery-between-model-intervals.json",envelopePath=QDir(checkpointDirectory).filePath(envelopeName);
            const auto publish=[&](const QString &path,const QJsonObject &object){QSaveFile file(path);file.setDirectWriteFallback(false);
                const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();};
            QVERIFY(publish(envelopePath,terminalEnvelope));pointer["envelopeFile"]=envelopeName;pointer["envelopeSha256"]=fileSha256(envelopePath);QVERIFY(publish(pointerPath,pointer));
            const auto beforeStamp=session->modelCommandStamp(sourceId);const auto beforeState=rPiezoRuntimeCertify(session->getModel(sourceId));
            auto corrupted=terminalEnvelope;corrupted["stateFingerprintSha256"]=QString(64,'a');QVERIFY(publish(envelopePath,corrupted));
            auto corruptedPointer=pointer;corruptedPointer["envelopeSha256"]=fileSha256(envelopePath);QVERIFY(publish(pointerPath,corruptedPointer));
            SolverManager rejected;QCOMPARE(rejected.nativeJobRecoveryQuery(job)["eligibility"].toString(),QString("conflict"));
            QCOMPARE(session->modelCommandStamp(sourceId),beforeStamp);QVERIFY(rPiezoRuntimeSameAcceptedState(beforeState,rPiezoRuntimeCertify(session->getModel(sourceId))));
            QVERIFY(publish(envelopePath,terminalEnvelope));QVERIFY(publish(pointerPath,pointer));
        }
        std::unique_ptr<SolverManager> reopened;
        SolverManager *recoveryManager=&manager;
        if(!pending){reopened=std::make_unique<SolverManager>();recoveryManager=reopened.get();}
        QJsonObject qualification=recoveryManager->nativeJobRecoveryQuery(job);
        if(baselinePending){
            QCOMPARE(qualification["eligibility"].toString(),QString("recompute_required"));
            const QJsonObject projectedBaseline=qualification["checkpoint"].toObject();
            const QString baselineCheckpointSha=projectedBaseline["checkpointSha256"].toString();
            QCOMPARE(projectedBaseline["acceptedSeq"].toInt(),0);
            QCOMPARE(projectedBaseline["targetSteps"].toInt(),12);
            QCOMPARE(projectedBaseline["remainingSteps"].toInt(),12);
            QCOMPARE(baselineCheckpointSha,previousCheckpointSha);
            QMap<QString,QByteArray> observedSnapshot;QString snapshotError;
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY2(observedSnapshot==pendingCheckpointSnapshot,qPrintable(checkpointSnapshotDifference(pendingCheckpointSnapshot,observedSnapshot)));
            QString baselineSubmitError;
            QCOMPARE(baselineCheckpointSha.size(),64);
            QVERIFY(manager.submitNativeRecovery(job,baselineCheckpointSha,12,&baselineSubmitError).isEmpty());
            QVERIFY(!baselineSubmitError.isEmpty());
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY2(observedSnapshot==pendingCheckpointSnapshot,qPrintable(checkpointSnapshotDifference(pendingCheckpointSnapshot,observedSnapshot)));
            const QJsonObject recoveryRequestCheckpoint{{"checkpointSha256",baselineCheckpointSha},{"remainingSteps",12}};
            const QJsonObject rejectedPreview=nativeRecoveryPreview(session,job,recoveryRequestCheckpoint);
            QCOMPARE(rejectedPreview["status"].toString(),QString("rejected"));
            QCOMPARE(rejectedPreview["code"].toString(),QString("RECOVERY_CHANGED"));
            QVERIFY(!rejectedPreview.contains("result"));
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);
            return;
        }
        QCOMPARE(qualification["eligibility"].toString(),QString("recoverable"));
        QJsonObject selected=qualification["checkpoint"].toObject();
        const int selectedSeq=pending?4:5,remainingSteps=12-selectedSeq;
        const QString initialCheckpointSha=selected["checkpointSha256"].toString();
        QCOMPARE(selected["targetSteps"].toInt(),12);QCOMPARE(selected["acceptedSeq"].toInt(),selectedSeq);
        QCOMPARE(selected["remainingSteps"].toInt(),remainingSteps);
        if(family!="mixed"){
            QVERIFY(!selected["nativeAcceptedTrace"].toObject().isEmpty());
            QCOMPARE(readObject(selected["envelopePath"].toString())["modelAcceptedSeq"].toInt(),0);
            RModel periodic;periodic.read(selected["checkpointPath"].toString());QCOMPARE(rPiezoRuntimeCertify(periodic,true).completedSteps,quint64(0));
        }
        QString recoveredJob;
        if(pending){
            const QString markerPath=QDir(checkpointDirectory).filePath("pending-publication.json");
            const QString pendingPlanSha=selected["pendingRecoveryPlanSha256"].toString();
            QCOMPARE(pendingPlanSha.size(),64);
            QMap<QString,QByteArray> observedSnapshot;QString snapshotError;
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY2(observedSnapshot==pendingCheckpointSnapshot,qPrintable(checkpointSnapshotDifference(pendingCheckpointSnapshot,observedSnapshot)));
            const auto repeated=manager.nativeJobRecoveryQuery(job);QCOMPARE(repeated["eligibility"].toString(),QString("recoverable"));
            QCOMPARE(repeated["checkpoint"].toObject()["pendingRecoveryPlanSha256"].toString(),pendingPlanSha);
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);

            QString missingPlanError;
            QVERIFY(manager.submitNativeRecovery(job,selected["checkpointSha256"].toString(),uint(remainingSteps),&missingPlanError,QString()).isEmpty());
            QVERIFY(!missingPlanError.isEmpty());
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);
            QString wrongPlanError;
            QVERIFY(manager.submitNativeRecovery(job,selected["checkpointSha256"].toString(),uint(remainingSteps),&wrongPlanError,QString(64,'0')).isEmpty());
            QVERIFY(!wrongPlanError.isEmpty());
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);

            auto *host=session->getModelCommandHost();
            const QJsonObject pendingPreview=nativeRecoveryPreview(session,job,selected);
            QCOMPARE(pendingPreview["status"].toString(),QString("preview"));
            QCOMPARE(pendingPreview["result"].toObject()["requiresHostApproval"].toBool(),true);
            QCOMPARE(pendingPreview["result"].toObject()["before"].toObject()["acceptedSeq"].toInt(),4);
            const QJsonObject unapproved=executeNativeRecoveryPreview(session,pendingPreview["result"].toObject()["previewId"].toString());
            QCOMPARE(unapproved["status"].toString(),QString("rejected"));QCOMPARE(unapproved["code"].toString(),QString("AUTHORIZATION_REQUIRED"));
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);

            // Qualify and approve the real, fully published accepted-5 state.
            // Restoring the same pending marker then makes this approved preview stale.
            QVERIFY(QFile::remove(markerPath));
            QMap<QString,QByteArray> noMarkerSnapshot;
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&noMarkerSnapshot,&snapshotError),qPrintable(snapshotError));
            const auto changedQualification=manager.nativeJobRecoveryQuery(job);
            QCOMPARE(changedQualification["eligibility"].toString(),QString("recoverable"));
            const QJsonObject changedCheckpoint=changedQualification["checkpoint"].toObject();
            QCOMPARE(changedCheckpoint["acceptedSeq"].toInt(),5);QCOMPARE(changedCheckpoint["remainingSteps"].toInt(),7);
            QVERIFY(changedCheckpoint["pendingRecoveryPlanSha256"].toString().isEmpty());
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==noMarkerSnapshot);
            const QJsonObject oldPreview=nativeRecoveryPreview(session,job,changedCheckpoint);
            QCOMPARE(oldPreview["status"].toString(),QString("preview"));
            const QString oldPreviewId=oldPreview["result"].toObject()["previewId"].toString();
            QVERIFY(host->approvePreview(oldPreviewId));
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==noMarkerSnapshot);
            QSaveFile restoreMarker(markerPath);restoreMarker.setDirectWriteFallback(false);
            QVERIFY(restoreMarker.open(QIODevice::WriteOnly));QCOMPARE(restoreMarker.write(pendingMarkerBytes),qint64(pendingMarkerBytes.size()));QVERIFY(restoreMarker.commit());
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);
            const QJsonObject stale=executeNativeRecoveryPreview(session,oldPreviewId);
            QCOMPARE(stale["status"].toString(),QString("rejected"));QCOMPARE(stale["code"].toString(),QString("RECOVERY_CHANGED"));
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);

            qualification=manager.nativeJobRecoveryQuery(job);QCOMPARE(qualification["eligibility"].toString(),QString("recoverable"));
            selected=qualification["checkpoint"].toObject();QCOMPARE(selected["acceptedSeq"].toInt(),4);
            QCOMPARE(selected["remainingSteps"].toInt(),8);QCOMPARE(selected["pendingRecoveryPlanSha256"].toString(),pendingPlanSha);
            QCOMPARE(selected["checkpointSha256"].toString(),initialCheckpointSha);
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);
            const QJsonObject freshPreview=nativeRecoveryPreview(session,job,selected);
            QCOMPARE(freshPreview["status"].toString(),QString("preview"));
            const QString freshPreviewId=freshPreview["result"].toObject()["previewId"].toString();
            QVERIFY(host->approvePreview(freshPreviewId));
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&observedSnapshot,&snapshotError),qPrintable(snapshotError));
            QVERIFY(observedSnapshot==pendingCheckpointSnapshot);
            const QJsonObject accepted=executeNativeRecoveryPreview(session,freshPreviewId);
            QCOMPARE(accepted["status"].toString(),QString("accepted"));QCOMPARE(accepted["code"].toString(),QString("OK"));
            recoveredJob=accepted["result"].toObject()["jobId"].toString();QVERIFY(!recoveredJob.isEmpty());

            QVERIFY(!QFileInfo::exists(markerPath));
            QFile restoredLatest(QDir(checkpointDirectory).filePath("latest.json"));QVERIFY(restoredLatest.open(QIODevice::ReadOnly));
            QCOMPARE(restoredLatest.readAll(),previousLatestPointerBytes);
            const QString lastValidPath=QDir(checkpointDirectory).filePath("lastvalid.json");
            QCOMPARE(QFileInfo::exists(lastValidPath),previousLastValidExists);
            if(previousLastValidExists){QFile restoredLastValid(lastValidPath);QVERIFY(restoredLastValid.open(QIODevice::ReadOnly));QCOMPARE(restoredLastValid.readAll(),previousLastValidPointerBytes);}
            QFile unchangedManifest(pendingManifestPath);QVERIFY(unchangedManifest.open(QIODevice::ReadOnly));
            QCOMPARE(unchangedManifest.readAll(),pendingManifestOriginal);
            QMap<QString,QByteArray> restoredSnapshot;
            QVERIFY2(checkpointDirectorySnapshot(checkpointDirectory,&restoredSnapshot,&snapshotError),qPrintable(snapshotError));
            for(auto it=pendingCheckpointSnapshot.cbegin();it!=pendingCheckpointSnapshot.cend();++it)
                if(it.key().startsWith(QStringLiteral("rollback-")))QCOMPARE(restoredSnapshot.value(it.key()),it.value());

            const auto restoredPointer=readObject(QDir(checkpointDirectory).filePath("latest.json"));
            const auto restoredEnvelope=readObject(QDir(checkpointDirectory).filePath(restoredPointer["envelopeFile"].toString()));
            QCOMPARE(restoredEnvelope["acceptedSeq"].toInt(),4);
            const auto serialized=restoredEnvelope["traceCursor"].toObject();QCOMPARE(serialized["sequence"].toInt(),4);
            QFile acceptedTrace(QDir(checkpointDirectory).filePath("accepted-trace.rpt"));QVERIFY(acceptedTrace.open(QIODevice::ReadOnly));
            const auto traceHeader=PiezoAcceptedTrace::readHeader(acceptedTrace);
            PiezoAcceptedTrace::Cursor cursor;cursor.byteEnd=qint64(serialized["byteEnd"].toDouble());
            cursor.lastRecordOffset=qint64(serialized["lastRecordOffset"].toDouble());cursor.sequence=quint64(serialized["sequence"].toDouble());
            cursor.recordSha32=QByteArray::fromHex(serialized["recordSha256"].toString().toLatin1());
            cursor.stateSha32=QByteArray::fromHex(serialized["stateSha256"].toString().toLatin1());
            const auto strictPrefix=PiezoAcceptedTrace::verifyPrefix(acceptedTrace,cursor);
            QCOMPARE(strictPrefix.records,quint64(5));QCOMPARE(strictPrefix.cursor.sequence,quint64(4));
            QCOMPARE(strictPrefix.cursor.byteEnd,acceptedTrace.size());QCOMPARE(acceptedTrace.size(),cursor.byteEnd);
            QCOMPARE(QString::fromLatin1(traceHeader.headerSha32.toHex()),restoredEnvelope["traceHeaderSha256"].toString());
        }else{
            recoveredJob=recoveryManager->submitNativeRecovery(job,selected["checkpointSha256"].toString(),uint(remainingSteps),&error);
            QVERIFY2(!recoveredJob.isEmpty(),qPrintable(error));
        }
        const auto recoveredQueued=recoveryManager->jobQuery(recoveredJob);RModel recoveredInput;recoveredInput.read(recoveredQueued["nativeCheckpointInputModelRef"].toString());
        verifyPrivateNativeOutput(recoveredQueued,family);
        QVERIFY(recoveredQueued["resultArtifactRef"]!=queued["resultArtifactRef"]);
        QCOMPARE(recoveredQueued["recovery"].toObject()["sourceResultArtifactRef"],queued["resultArtifactRef"]);
        QCOMPARE(recoveredInput.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),uint(12));
        QCOMPARE(rPiezoRuntimeCertify(recoveredInput).targetSteps,quint64(12));
        QCOMPARE(rPiezoRuntimeCertify(recoveredInput).completedSteps,quint64(selectedSeq));
        if(family!="mixed"){
            QCOMPARE(recoveredQueued["nativeCheckpointSeed"].toObject()["schema"].toString(),QString("range.native-checkpoint-trace-seed/v2"));
            QCOMPARE(QString::fromLatin1(rPiezoRuntimeCertify(recoveredInput,true).stateIdentity.toHex()),selected["stateFingerprintSha256"].toString());
        }
        QTRY_COMPARE_WITH_TIMEOUT(recoveryManager->jobQuery(recoveredJob)["state"].toString(),QString("succeeded"),120000);
        QTRY_VERIFY_WITH_TIMEOUT(recoveryManager->getNRunning()==0 && recoveryManager->getNWaiting()==0,30000);
        verifyFinishedNativeBundle(*recoveryManager,recoveredJob,family);
        const auto recoveredTerminal=recoveryManager->jobQuery(recoveredJob);
        QCOMPARE(recoveredTerminal["inputSnapshotSha256"],recoveredQueued["inputSnapshotSha256"]);
        QCOMPARE(recoveredTerminal["inputPackageSha256"],recoveredQueued["inputPackageSha256"]);
        verifyPrivateNativeOutput(manager.jobQuery(job),family);
        RModel referenceResult,recoveredResult;
        referenceResult.read(manager.jobQuery(referenceJob)["resultArtifactRef"].toString());recoveredResult.read(recoveryManager->jobQuery(recoveredJob)["resultArtifactRef"].toString());
        const auto a=rPiezoRuntimeCertify(referenceResult,true),b=rPiezoRuntimeCertify(recoveredResult,true);
        QCOMPARE(a.completedSteps,quint64(12));QVERIFY(rPiezoRuntimeSameAcceptedState(a,b));
        // Stored result variables cannot replace complete restore authority.
        auto invalid=recoveredResult;
        if(family=="axial"){auto &bytes=invalid.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState;bytes[0]=char(bytes.at(0)^1);}
        else if(family=="layered"){auto &bytes=invalid.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes;bytes[0]=char(bytes.at(0)^1);}
        else invalid.getProblemSetup().getStructuralTrussDynamicsRestartData().q[0]+=.01;
        bool rejected=false;try {rPiezoRuntimeCertify(invalid,true);}catch(const RError &){rejected=true;}QVERIFY(rejected);
    }
};
int runPiezoSolverRecoveryTests(int argc,char **argv){TestPiezoSolverRecovery test;return QTest::qExec(&test,argc,argv);}
#include "tst_piezo_solver_recovery.moc"
