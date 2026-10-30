#include <QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QDateTime>
#include <QEvent>
#include <QPointer>
#include <QLockFile>
#include <QScopeGuard>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRunnable>
#include <QSaveFile>
#include <QSignalSpy>
#include <QSet>
#include <QSemaphore>
#include <QThreadPool>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>
#include <atomic>

#include <algorithm>
#include <cmath>

#include <rml_time_solver.h>
#include <rnative_result_broker.h>
#include <rmodel_command_service.h>
#include <rtrusted_experiment_service.h>
#include <rtrusted_mechanics_service.h>

#include <rbl_job.h>

#include "application.h"
#include "model_io.h"
#include "model_command_host.h"
#include "session.h"
#include "solver_manager.h"

bool terminateVerifiedOwnRecoverySolver(const QString &solverPath, QString *error);

namespace
{
QJsonObject trustedTestQuantity(double value, const char *unit)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("unit"), QString::fromLatin1(unit)}};
}

QJsonObject trustedRodProblemSpec()
{
    const auto vector = [](double x, double y, double z) {
        return QJsonArray{trustedTestQuantity(x, "N"), trustedTestQuantity(y, "N"), trustedTestQuantity(z, "N")};
    };
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("modelType"), QStringLiteral("axialRod")},
            {QStringLiteral("name"), QStringLiteral("trusted detached solver regression")},
            {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), trustedTestQuantity(1.0, "m")},
                                                       {QStringLiteral("segments"), 4}}},
            {QStringLiteral("material"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Steel")},
                                                       {QStringLiteral("source"), QStringLiteral("fixture")},
                                                       {QStringLiteral("youngModulus"), trustedTestQuantity(210e9, "Pa")},
                                                       {QStringLiteral("poissonRatio"), 0.3},
                                                       {QStringLiteral("density"), trustedTestQuantity(7850.0, "kg/m3")}}},
            {QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), trustedTestQuantity(1e-4, "m2")}}},
            {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}}},
            {QStringLiteral("supports"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}},
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}}}},
            {QStringLiteral("loads"), QJsonArray{
                 QJsonObject{{QStringLiteral("kind"), QStringLiteral("pointForce")},
                             {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("components"), vector(1000.0, 0.0, 0.0)}}}},
            {QStringLiteral("initialConditions"), QJsonArray{}},
            {QStringLiteral("observables"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("quantity"), QStringLiteral("displacement")},
                             {QStringLiteral("component"), QStringLiteral("ux")}}}},
            {QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}}}};
}

class GateJob : public RJob
{
public:
    GateJob(const std::shared_ptr<QSemaphore> &started,
            const std::shared_ptr<QSemaphore> &release)
        : started(started), release(release)
    {
        setBlocking(false);
    }

protected:
    int perform() override
    {
        started->release();
        return release->tryAcquire(1, 30000) ? 0 : 1;
    }

private:
    std::shared_ptr<QSemaphore> started;
    std::shared_ptr<QSemaphore> release;
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

struct GateReleaseGuard
{
    std::shared_ptr<QSemaphore> release;
    ~GateReleaseGuard() { if (release) release->release(); }
};

// Isolated queued-terminal evidence fixture; this never executes an FE task
// and does not claim process or numerical qualification from injected signals.
class DeletedSenderTerminalProbe : public SolverManager
{
public:
    void retainFixtureLease(const QString &jobId, const std::shared_ptr<QLockFile> &lease)
    {
        nativeRecoveryLogicalLocks.insert(jobId, lease);
    }
    void holdSenderOutsideWorkerQueue(SolverTask *task)
    {
        QMutexLocker<QRecursiveMutex> locker(&jobsMutex);
        waitingJobs.removeAll(task);
        waitingParallelJobs.removeAll(task);
        runningJobs.append(task);
    }
    void detachSenderBeforeDeletion(SolverTask *task)
    {
        QMutexLocker<QRecursiveMutex> locker(&jobsMutex);
        waitingJobs.removeAll(task);
        waitingParallelJobs.removeAll(task);
        runningJobs.removeAll(task);
    }
    bool retainsFixtureLease(const QString &jobId) const
    {
        return nativeRecoveryLogicalLocks.contains(jobId);
    }
    void cleanupFixtureLease(const QString &jobId)
    {
        const auto lease = nativeRecoveryLogicalLocks.take(jobId);
        if (lease && lease->isLocked()) lease->unlock();
    }
};

class SessionModelGuard
{
public:
    explicit SessionModelGuard(Session *session) : session(session) {}
    ~SessionModelGuard()
    {
        for (const QString &identity : identities)
        {
            const uint modelID = session->findModelByCommandIdentity(identity);
            if (modelID != RConstants::eod) session->removeModel(modelID);
        }
    }

    uint add(Model model)
    {
        session->addModel(model);
        const uint modelID = session->getNModels() - 1;
        identities.append(session->modelCommandStamp(modelID).first);
        return modelID;
    }

    QString identity(uint modelID) const
    {
        return session->modelCommandStamp(modelID).first;
    }

private:
    Session *session;
    QStringList identities;
};

bool loadTinyTruss(QTemporaryDir &directory, const QString &stem, Model &model, QString *sourceFileName = nullptr)
{
#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_WORKFLOW_SOURCE_ROOT "."
#endif
    const QString fixture = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT))
            .filePath(QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram"));
    const QString target = directory.filePath(stem + QStringLiteral(".ram"));
    if (!QFile::copy(fixture, target)) return false;
    try
    {
        ModelIO::open(model, target);
    }
    catch (const RError &)
    {
        return false;
    }
    // The tiny native GUI run uses the structural model; the fixture's
    // controller binding belongs to the separate workflow CLI benchmark.
    model.getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    model.getProblemSetup().getStructuralDynamicsSetup().setEnabled(false);
    model.getProblemSetup().setRestart(false);
    model.getTimeSolver().setEnabled(false);
    if (model.getNLines() == 0) return false;
    for (uint index = 0; index < model.getNPoints(); ++index)
        model.getPoint(index).setMaterial(model.getLine(0).getMaterial());
    ModelIO::save(model, target, false);
    if (sourceFileName) *sourceFileName = target;
    return model.getNNodes() > 0 && model.getNElements() > 0;
}

void configureSingleOutputDynamics(Model &model, uint steps = 400, uint outputFrequency = 0)
{
    constexpr double timeStep = 0.001;
    if (outputFrequency == 0) outputFrequency = steps;
    RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
    dynamics.setTimeStep(timeStep);
    dynamics.setNSteps(steps);
    dynamics.setOutputFrequency(outputFrequency);

    RTimeSolver &time = model.getTimeSolver();
    time.setEnabled(true);
    time.setInputStartTime(0.0);
    time.setInputNTimeSteps(steps);
    time.setInputTimeStepSize(timeStep);
    time.setOutputFrequency(outputFrequency);
    time.setComputedTime(0.0);
    time.setTimes(RTimeSolver::findTimesVector(steps, 0.0, timeStep));
    time.setCurrentTimeStep(0);
}

QTemporaryDir &linkedProjectWorkspace()
{
    static QTemporaryDir directory;
    return directory;
}

bool waitForGate(const std::shared_ptr<QSemaphore> &started)
{
    return started->tryAcquire(1, 10000);
}

QString fileSha256(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

QString boundedFileTail(const QString &fileName, qint64 maximumBytes = 4096)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly) || maximumBytes <= 0) return {};
    const qint64 start = qMax<qint64>(0, file.size() - maximumBytes);
    if (!file.seek(start)) return {};
    return QString::fromUtf8(file.read(maximumBytes));
}

QJsonObject nativeExecutionStateObject(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > 64 * 1024) return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject()
            ? document.object() : QJsonObject{};
}

QJsonObject persistedNativeJobRecord(const QJsonObject &jobRecord)
{
    const QString recordPath = jobRecord.value(QStringLiteral("nativeJobRecordRef")).toString();
    QFile file(recordPath);
    if (recordPath.isEmpty() || !file.open(QIODevice::ReadOnly) ||
        file.size() < 0 || file.size() > 32 * 1024 * 1024) return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject()
            ? document.object() : QJsonObject{};
}

QJsonObject latestNativeCheckpointEnvelope(const QJsonObject &jobRecord,
                                           QJsonObject *pointerOut = nullptr)
{
    const QString directory = jobRecord.value(QStringLiteral("nativeCheckpointDirectory")).toString();
    QFile pointerFile(QDir(directory).filePath(QStringLiteral("latest.json")));
    if (!pointerFile.open(QIODevice::ReadOnly)) return {};
    const QJsonObject pointer = QJsonDocument::fromJson(pointerFile.readAll()).object();
    const QString envelopeName = pointer.value(QStringLiteral("envelopeFile")).toString();
    if (envelopeName.isEmpty() || QFileInfo(envelopeName).fileName() != envelopeName) return {};
    QFile envelopeFile(QDir(directory).filePath(envelopeName));
    if (!envelopeFile.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = envelopeFile.readAll();
    const QJsonObject envelope = QJsonDocument::fromJson(bytes).object();
    if (bytes.isEmpty() ||
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
            pointer.value(QStringLiteral("envelopeSha256")).toString())
        return {};
    if (pointerOut) *pointerOut = pointer;
    return envelope;
}

QString jsonObjectSha256(const QJsonObject &object)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
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

bool latestAcceptedSequence(const QJsonObject &jobRecord, double *acceptedSequence)
{
    if (!acceptedSequence) return false;
    const QString directory = jobRecord.value(QStringLiteral("nativeCheckpointDirectory")).toString();
    QFile pointer(QDir(directory).filePath(QStringLiteral("latest.json")));
    if (!pointer.open(QIODevice::ReadOnly)) return false;
    const QJsonObject pointerObject = QJsonDocument::fromJson(pointer.readAll()).object();
    const QString envelopeName = pointerObject.value(QStringLiteral("envelopeFile")).toString();
    if (QFileInfo(envelopeName).fileName() != envelopeName || envelopeName.isEmpty()) return false;
    QFile envelope(QDir(directory).filePath(envelopeName));
    if (!envelope.open(QIODevice::ReadOnly)) return false;
    const QJsonValue sequence = QJsonDocument::fromJson(envelope.readAll()).object()
            .value(QStringLiteral("acceptedSeq"));
    if (!sequence.isDouble()) return false;
    *acceptedSequence = sequence.toDouble();
    return true;
}

QString recoveryQualificationSummary(const QJsonObject &qualification)
{
    const QJsonObject checkpoint = qualification.value(QStringLiteral("checkpoint")).toObject();
    const QJsonObject partial = qualification.value(QStringLiteral("partialResult")).toObject();
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("eligibility"), qualification.value(QStringLiteral("eligibility"))},
        {QStringLiteral("reason"), qualification.value(QStringLiteral("reason"))},
        {QStringLiteral("checkpointAcceptedSeq"), checkpoint.value(QStringLiteral("acceptedSeq"))},
        {QStringLiteral("targetSteps"), checkpoint.value(QStringLiteral("targetSteps"))},
        {QStringLiteral("remainingSteps"), checkpoint.value(QStringLiteral("remainingSteps"))},
        {QStringLiteral("partialAcceptedSeq"), partial.value(QStringLiteral("acceptedSeq"))},
        {QStringLiteral("discardedAcceptedSteps"), partial.value(QStringLiteral("discardedAcceptedSteps"))}
    }).toJson(QJsonDocument::Compact));
}

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

bool nearlyEqualVectors(const std::vector<double> &left, const std::vector<double> &right,
                        double tolerance = 1.0e-11)
{
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (std::abs(left[index] - right[index]) > tolerance * std::max({1.0, std::abs(left[index]), std::abs(right[index])}))
            return false;
    return true;
}

QString directorySha256(const QString &directoryName)
{
    const QDir root(directoryName);
    QStringList relativeFileNames;
    QDirIterator iterator(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) relativeFileNames.append(root.relativeFilePath(iterator.next()));
    std::sort(relativeFileNames.begin(), relativeFileNames.end());
    if (relativeFileNames.isEmpty()) return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &relativeFileName : relativeFileNames)
    {
        hash.addData(relativeFileName.toUtf8());
        hash.addData("\0", 1);
        QFile file(root.filePath(relativeFileName));
        if (!file.open(QIODevice::ReadOnly)) return {};
        while (!file.atEnd())
        {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return {};
            hash.addData(chunk);
        }
        hash.addData("\0", 1);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool waitForManagerIdle(SolverManager &manager)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000)
    {
        if (manager.getNRunning() == 0 && manager.getNWaiting() == 0 &&
            RJobManager::getInstance().getNRunning() == 0 &&
            RJobManager::getInstance().getNWaiting() == 0)
            return true;
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
    return false;
}
}

class TestNativeJobRegistry : public QObject
{
    Q_OBJECT

private slots:
    void trustedDetachedCandidateSolvesWithoutPublishingToSource()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("trusted-detached-source"), source));
        const uint sourceModelID = models.add(source);
        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(sourceModelID);
        const QString sourceSha256 = RModelCommandService::stateFingerprint(session->getModel(sourceModelID));
        const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
                ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        const QString experimentStorage = QDir(dataRoot).filePath(QStringLiteral("trusted-experiments"));
        RTrustedExperimentService experiments(experimentStorage);
        RTrustedExperimentService::SourceIdentity sourceIdentity;
        sourceIdentity.workspaceId = session->getModelCommandHost()->projectWorkspace();
        sourceIdentity.sessionEpoch = session->getModelCommandHost()->sessionEpoch();
        sourceIdentity.sessionId = session->getID();
        sourceIdentity.documentId = sourceStamp.first;
        sourceIdentity.documentRevision = QString::number(sourceStamp.second);
        sourceIdentity.snapshotSha256 = sourceSha256;
        const QJsonObject spec = trustedRodProblemSpec();
        const auto issued = experiments.authorize({{QStringLiteral("schemaVersion"), 1},
                                                     {QStringLiteral("sourceBinding"), sourceIdentity.toJson()},
                                                     {QStringLiteral("problemSpec"), spec},
                                                     {QStringLiteral("budgets"), QJsonObject{
                                                         {QStringLiteral("solves"), 1},
                                                         {QStringLiteral("repairs"), 0},
                                                         {QStringLiteral("modelTurns"), 1}}}}, sourceIdentity);
        QVERIFY2(issued.ok, qPrintable(issued.message));
        const QString experimentId = issued.data.value(QStringLiteral("experimentId")).toString();
        const QJsonObject authorization = issued.data.value(QStringLiteral("authorization")).toObject();
        QVERIFY(experiments.create(experimentId, sourceIdentity).ok);
        QVERIFY(experiments.run(experimentId, sourceIdentity).ok);
        const auto candidate = RTrustedMechanicsService::createCandidate(spec, sourceStamp.first,
                                                                          QString::number(sourceStamp.second));
        QVERIFY2(candidate.valid, qPrintable(candidate.message));
        Model detached;
        static_cast<RModel&>(detached) = candidate.model;
        const QString artifactRoot = QDir(dataRoot).filePath(QStringLiteral("trusted-experiments/") + experimentId);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(),
                                    detached, sourceStamp,
                                    authorization, artifactRoot,
                                    manager.getTaskServerName(), SolverTask::TrustedExperimentCandidateTag{});
        QString error;
        QVERIFY2(task->captureTrustedCandidateSnapshot(detached, &error), qPrintable(error));
        const auto started = experiments.beginSolve(experimentId, sourceIdentity, spec,
            QString::fromLatin1(task->getInputSnapshotSha256().toHex()),
            QString::fromLatin1(task->getInputPackageSha256().toHex()));
        QVERIFY2(started.ok, qPrintable(started.message));
        const QString attemptId = started.data.value(QStringLiteral("attemptId")).toString();
        QVERIFY(task->setTrustedExperimentAttemptId(attemptId));
        const QString jobId = manager.submitTrustedDetached(task, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));

        SolverManager restartedManager;
        Model replayCandidate;
        static_cast<RModel&>(replayCandidate) = candidate.model;
        auto *replayedTask = new SolverTask(Application::instance()->getApplicationSettings(),
                                            replayCandidate, sourceStamp, authorization, artifactRoot,
                                            restartedManager.getTaskServerName(), SolverTask::TrustedExperimentCandidateTag{});
        QString replayError;
        QVERIFY2(replayedTask->captureTrustedCandidateSnapshot(replayCandidate, &replayError),
                 qPrintable(replayError));
        QVERIFY(replayedTask->setTrustedExperimentAttemptId(attemptId));
        QVERIFY(restartedManager.submitTrustedDetached(replayedTask, &replayError).isEmpty());
        QVERIFY(replayError.contains(QStringLiteral("already submitted and cannot be replayed")));

        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);
        QVERIFY(waitForManagerIdle(manager));

        const QJsonObject job = manager.jobQuery(jobId);
        QCOMPARE(job.value(QStringLiteral("resultPublication")).toString(), QStringLiteral("detached-candidate"));
        QCOMPARE(job.value(QStringLiteral("modelIdAtSubmit")).type(), QJsonValue::Null);
        QCOMPARE(job.value(QStringLiteral("trustedExperimentId")).toString(), experimentId);
        QCOMPARE(job.value(QStringLiteral("trustedExperimentAttemptId")).toString(), attemptId);
        QCOMPARE(job.value(QStringLiteral("trustedAuthorizationDigest")).toString(),
                 authorization.value(QStringLiteral("authorizationDigest")).toString());
        QVERIFY(job.value(QStringLiteral("resultManifestVerified")).toBool());
        QVERIFY(manager.nativeResultQuery(jobId, QStringLiteral("catalog"), {})
                    .value(QStringLiteral("valid")).toBool());
        QCOMPARE(session->modelCommandStamp(sourceModelID), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(sourceModelID)), sourceSha256);
    }

    void trustedDetachedRejectsCallerMintedAuthorization()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("trusted-detached-forged"), source));
        const uint sourceModelID = models.add(source);
        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(sourceModelID);
        const auto *host = session->getModelCommandHost();
        const QString sourceSha256 = RModelCommandService::stateFingerprint(session->getModel(sourceModelID));
        const QString experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        QJsonObject authorization{
            {QStringLiteral("schema"), QStringLiteral("range.trusted-experiment-authorization/v1")},
            {QStringLiteral("experimentId"), experimentId},
            {QStringLiteral("sourceBinding"), QJsonObject{
                {QStringLiteral("sessionEpoch"), host->sessionEpoch()},
                {QStringLiteral("sessionId"), session->getID()},
                {QStringLiteral("workspaceId"), host->projectWorkspace()},
                {QStringLiteral("documentId"), sourceStamp.first},
                {QStringLiteral("documentRevision"), QString::number(sourceStamp.second)},
                {QStringLiteral("snapshotSha256"), sourceSha256}}},
            {QStringLiteral("problemSpecDigest"), QString(64, QLatin1Char('a'))},
            {QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('b'))},
            {QStringLiteral("numericalConfigDigest"), QString(64, QLatin1Char('c'))},
            {QStringLiteral("experimentPolicy"), QJsonObject{}}};
        authorization.insert(QStringLiteral("authorizationDigest"), QString::fromLatin1(
            QCryptographicHash::hash(QJsonDocument(authorization).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256).toHex()));
        const auto candidate = RTrustedMechanicsService::createCandidate(trustedRodProblemSpec(),
            sourceStamp.first, QString::number(sourceStamp.second));
        QVERIFY2(candidate.valid, qPrintable(candidate.message));
        Model detached;
        static_cast<RModel&>(detached) = candidate.model;
        const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
                ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        const QString artifactRoot = QDir(dataRoot).filePath(QStringLiteral("trusted-experiments/") + experimentId);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), detached, sourceStamp,
                                    authorization, artifactRoot, manager.getTaskServerName(),
                                    SolverTask::TrustedExperimentCandidateTag{});
        QString error;
        QVERIFY2(task->captureTrustedCandidateSnapshot(detached, &error), qPrintable(error));
        QVERIFY(task->setTrustedExperimentAttemptId(QUuid::createUuid().toString(QUuid::WithoutBraces).toLower()));
        QVERIFY(manager.submitTrustedDetached(task, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("persisted active experiment authorization attempt")));
    }

    void trustedDetachedAuthorizationRejectsChangedSource()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("trusted-detached-stale"), source));
        const uint sourceModelID = models.add(source);
        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(sourceModelID);
        const QString sourceSha256 = RModelCommandService::stateFingerprint(session->getModel(sourceModelID));
        const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
                ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        const QString experimentStorage = QDir(dataRoot).filePath(QStringLiteral("trusted-experiments"));
        RTrustedExperimentService experiments(experimentStorage);
        RTrustedExperimentService::SourceIdentity sourceIdentity;
        sourceIdentity.workspaceId = session->getModelCommandHost()->projectWorkspace();
        sourceIdentity.sessionEpoch = session->getModelCommandHost()->sessionEpoch();
        sourceIdentity.sessionId = session->getID();
        sourceIdentity.documentId = sourceStamp.first;
        sourceIdentity.documentRevision = QString::number(sourceStamp.second);
        sourceIdentity.snapshotSha256 = sourceSha256;
        const QJsonObject spec = trustedRodProblemSpec();
        const auto issued = experiments.authorize({{QStringLiteral("schemaVersion"), 1},
                                                     {QStringLiteral("sourceBinding"), sourceIdentity.toJson()},
                                                     {QStringLiteral("problemSpec"), spec},
                                                     {QStringLiteral("budgets"), QJsonObject{
                                                         {QStringLiteral("solves"), 1},
                                                         {QStringLiteral("repairs"), 0},
                                                         {QStringLiteral("modelTurns"), 1}}}}, sourceIdentity);
        QVERIFY2(issued.ok, qPrintable(issued.message));
        const QString experimentId = issued.data.value(QStringLiteral("experimentId")).toString();
        const QJsonObject authorization = issued.data.value(QStringLiteral("authorization")).toObject();
        QVERIFY(experiments.create(experimentId, sourceIdentity).ok);
        QVERIFY(experiments.run(experimentId, sourceIdentity).ok);
        const auto candidate = RTrustedMechanicsService::createCandidate(spec, sourceStamp.first,
                                                                          QString::number(sourceStamp.second));
        QVERIFY2(candidate.valid, qPrintable(candidate.message));
        Model detached;
        static_cast<RModel&>(detached) = candidate.model;
        const QString artifactRoot = QDir(dataRoot).filePath(QStringLiteral("trusted-experiments/") + experimentId);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(),
                                    detached, sourceStamp,
                                    authorization, artifactRoot,
                                    manager.getTaskServerName(), SolverTask::TrustedExperimentCandidateTag{});
        QString error;
        QVERIFY2(task->captureTrustedCandidateSnapshot(detached, &error), qPrintable(error));
        const auto started = experiments.beginSolve(experimentId, sourceIdentity, spec,
            QString::fromLatin1(task->getInputSnapshotSha256().toHex()),
            QString::fromLatin1(task->getInputPackageSha256().toHex()));
        QVERIFY2(started.ok, qPrintable(started.message));
        QVERIFY(task->setTrustedExperimentAttemptId(started.data.value(QStringLiteral("attemptId")).toString()));
        session->getModel(sourceModelID).setDescription(QStringLiteral("Changed after trusted authorization"));
        QVERIFY(session->getModelCommandHost()->recordExternalChange(
                    sourceModelID, QStringLiteral("trusted experiment stale-source regression"), &error));
        const QString rejected = manager.submitTrustedDetached(task, &error);
        QVERIFY(rejected.isEmpty());
        QVERIFY(error.contains(QStringLiteral("source document or revision")));
    }

    void trustedDetachedAuthorizationRejectsMismatchedSessionIdAndEpoch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("trusted-detached-session-identity"), source));
        const uint sourceModelID = models.add(source);
        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(sourceModelID);
        const QString sourceSha256 = RModelCommandService::stateFingerprint(session->getModel(sourceModelID));
        const auto *host = session->getModelCommandHost();
        const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
                ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        const QString experimentStorage = QDir(dataRoot).filePath(QStringLiteral("trusted-experiments"));
        RTrustedExperimentService experiments(experimentStorage);
        const QJsonObject spec = trustedRodProblemSpec();

        for (const QString &mismatchedField : {QStringLiteral("sessionId"), QStringLiteral("sessionEpoch")})
        {
            RTrustedExperimentService::SourceIdentity sourceIdentity;
            sourceIdentity.workspaceId = host->projectWorkspace();
            sourceIdentity.sessionEpoch = host->sessionEpoch();
            sourceIdentity.sessionId = session->getID();
            if (mismatchedField == QStringLiteral("sessionId"))
                sourceIdentity.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            else
                sourceIdentity.sessionEpoch = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
            sourceIdentity.documentId = sourceStamp.first;
            sourceIdentity.documentRevision = QString::number(sourceStamp.second);
            sourceIdentity.snapshotSha256 = sourceSha256;

            const auto issued = experiments.authorize({{QStringLiteral("schemaVersion"), 1},
                                                         {QStringLiteral("sourceBinding"), sourceIdentity.toJson()},
                                                         {QStringLiteral("problemSpec"), spec},
                                                         {QStringLiteral("budgets"), QJsonObject{
                                                             {QStringLiteral("solves"), 1},
                                                             {QStringLiteral("repairs"), 0},
                                                             {QStringLiteral("modelTurns"), 1}}}}, sourceIdentity);
            QVERIFY2(issued.ok, qPrintable(issued.message));
            const QString experimentId = issued.data.value(QStringLiteral("experimentId")).toString();
            const QJsonObject authorization = issued.data.value(QStringLiteral("authorization")).toObject();
            QVERIFY(experiments.create(experimentId, sourceIdentity).ok);
            QVERIFY(experiments.run(experimentId, sourceIdentity).ok);

            const auto candidate = RTrustedMechanicsService::createCandidate(
                spec, sourceStamp.first, QString::number(sourceStamp.second));
            QVERIFY2(candidate.valid, qPrintable(candidate.message));
            Model detached;
            static_cast<RModel&>(detached) = candidate.model;
            const QString artifactRoot = QDir(dataRoot).filePath(
                QStringLiteral("trusted-experiments/") + experimentId);
            auto *task = new SolverTask(Application::instance()->getApplicationSettings(),
                                        detached, sourceStamp, authorization, artifactRoot,
                                        manager.getTaskServerName(), SolverTask::TrustedExperimentCandidateTag{});
            QString error;
            QVERIFY2(task->captureTrustedCandidateSnapshot(detached, &error), qPrintable(error));
            const auto started = experiments.beginSolve(experimentId, sourceIdentity, spec,
                QString::fromLatin1(task->getInputSnapshotSha256().toHex()),
                QString::fromLatin1(task->getInputPackageSha256().toHex()));
            QVERIFY2(started.ok, qPrintable(started.message));
            QVERIFY(task->setTrustedExperimentAttemptId(
                started.data.value(QStringLiteral("attemptId")).toString()));

            const QString rejected = manager.submitTrustedDetached(task, &error);
            QVERIFY(rejected.isEmpty());
            QVERIFY2(error.contains(QStringLiteral("source session changed")), qPrintable(error));
            const auto stopped = experiments.stop(experimentId, sourceIdentity,
                QStringLiteral("SESSION_IDENTITY_MISMATCH_TEST"), error);
            QVERIFY2(stopped.ok, qPrintable(stopped.message));
            const QJsonObject record = experiments.query(experimentId).data
                .value(QStringLiteral("experiment")).toObject();
            QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
            QCOMPARE(record.value(QStringLiteral("terminalCode")).toString(),
                     QStringLiteral("SESSION_IDENTITY_MISMATCH_TEST"));
        }
    }

    void restartCapturePreservesCompactOriginsAcrossCatalogPages()
    {
        // Metadata/capture fixture only: these persisted MODELs exercise the
        // broker and staging boundary, without claiming a numerical FE solve.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        QVERIFY(waitForManagerIdle(SolverManager::getInstance()));
        SessionModelGuard models(session);
        Model model;
        QString logicalPath;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("paged-restart-origin"), model, &logicalPath));
        constexpr uint frameCount = 201;
        configureSingleOutputDynamics(model, frameCount, 1);
        RVariable displacement(R_VARIABLE_DISPLACEMENT, R_VARIABLE_APPLY_NODE);
        displacement.resize(3, model.getNNodes());
        model.addVariable(displacement);
        RStructuralDynamicsRestartData checkpoint;
        checkpoint.available = true;
        checkpoint.fixedTimeStep = 0.001;
        checkpoint.meshNodeCount = model.getNNodes();
        checkpoint.meshElementCount = model.getNElements();
        checkpoint.physicalDofCount = model.getNNodes();
        checkpoint.meshFingerprint = RStructuralDynamicsRestartData::computeMeshFingerprint(model);
        checkpoint.dofFingerprint = QString(64, QLatin1Char('a'));
        checkpoint.systemFingerprint = QString(64, QLatin1Char('b'));
        checkpoint.q.assign(model.getNNodes(), 0.0);
        checkpoint.v.assign(model.getNNodes(), 0.0);
        checkpoint.a.assign(model.getNNodes(), 0.0);
        checkpoint.externalForce.assign(model.getNNodes(), 0.0);
        for (uint record = 1; record <= frameCount; ++record)
        {
            model.getTimeSolver().setCurrentTimeStep(record - 1);
            checkpoint.completedSteps = record;
            checkpoint.time = model.getTimeSolver().getCurrentTime();
            model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
            model.write(logicalPath, true);
        }
        const QString fixtureHash = fileSha256(logicalPath);
        const QJsonObject declared{
            {QStringLiteral("jobId"), QStringLiteral("metadata-fixture-paged-restart")},
            {QStringLiteral("proofScope"), QStringLiteral("solver-and-declared-controller-runtime/v1")},
            {QStringLiteral("sourceRuntimeStamp"), QJsonObject{
                {QStringLiteral("documentId"), QStringLiteral("metadata-fixture")},
                {QStringLiteral("revision"), QStringLiteral("1")}}},
            {QStringLiteral("sourceSnapshotRef"), QJsonObject{
                {QStringLiteral("projectId"), QStringLiteral("metadata-fixture")},
                {QStringLiteral("sourceRevision"), QStringLiteral("1")}}},
            {QStringLiteral("inputSnapshotSha256"), fixtureHash},
            {QStringLiteral("inputPackageSha256"), fixtureHash},
            {QStringLiteral("runtime"), QJsonObject{
                {QStringLiteral("fingerprintSha256"), fixtureHash},
                {QStringLiteral("scope"), QStringLiteral("metadata-fixture/v1")}}},
            {QStringLiteral("control"), QJsonObject{
                {QStringLiteral("enabled"), false},
                {QStringLiteral("controllerId"), QString()},
                {QStringLiteral("controllerVersionSha256"), QString()}}}};
        const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, declared);
        QVERIFY2(published.valid, qPrintable(published.message));
        const QJsonObject firstPage = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            QStringLiteral("catalog"), {{QStringLiteral("limit"), 200}});
        QVERIFY(firstPage.value(QStringLiteral("valid")).toBool());
        QCOMPARE(firstPage.value(QStringLiteral("frames")).toArray().size(), 200);
        QVERIFY(!firstPage.value(QStringLiteral("nextCursor")).toString().isEmpty());
        QFile manifest(published.manifestPath);
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const QJsonObject compact = QJsonDocument::fromJson(manifest.readAll()).object();
        QVERIFY(compact.value(QStringLiteral("originTable")).isObject());
        manifest.close();

        Model restart;
        ModelIO::open(restart, logicalPath);
        restart.getProblemSetup().setRestart(true);
        const uint modelID = models.add(restart);
        SolverTask capture(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(capture.captureInputSnapshot(restart, session->modelCommandStamp(modelID), modelID));
        QFile index(QDir(QFileInfo(capture.getInputSnapshotFileName()).absolutePath())
                    .filePath(QStringLiteral("history/origin-index.json")));
        QVERIFY(index.open(QIODevice::ReadOnly));
        const QJsonArray captured = QJsonDocument::fromJson(index.readAll()).array();
        const QJsonArray originals = published.manifest.value(QStringLiteral("frames")).toArray();
        QCOMPARE(captured.size(), int(frameCount));
        QCOMPARE(originals.size(), int(frameCount));
        for (int offset = 0; offset < captured.size(); ++offset)
        {
            const QJsonObject entry = captured.at(offset).toObject();
            const QJsonObject original = originals.at(offset).toObject();
            QCOMPARE(entry.value(QStringLiteral("recordNo")).toInt(), offset + 1);
            QCOMPARE(entry.value(QStringLiteral("originalFrameSha256")), original.value(QStringLiteral("sha256")));
            QCOMPARE(entry.value(QStringLiteral("origin")), original.value(QStringLiteral("origin")));
            QCOMPARE(entry.value(QStringLiteral("originManifestSha256")).toString(), published.manifestSha256);
            QCOMPARE(entry.value(QStringLiteral("originManifestPath")).toString(), QFileInfo(published.manifestPath).canonicalFilePath());
        }

        // The last frame lies on page two; a changed source must be rejected
        // even when all first-page frame bytes remain intact.
        const QJsonObject lastOriginal = originals.last().toObject();
        QFile changed(directory.filePath(lastOriginal.value(QStringLiteral("relativePath")).toString()));
        QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(changed.write("\n"), qint64(1));
        changed.close();
        SolverTask rejected(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(!rejected.captureInputSnapshot(restart, session->modelCommandStamp(modelID), modelID));
    }

    void managedRestartHistoryKeepsOriginalJobAndRejectsChangedAncestorManifest()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("restart-origin"), model));
        configureSingleOutputDynamics(model);
        const uint originalID = models.add(model);
        QString error;
        const QString originalJob = manager.submitValidated(originalID, {}, true, &error);
        QVERIFY2(!originalJob.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(originalJob).value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"), 120000);
        QVERIFY(waitForManagerIdle(manager));
        const QJsonObject original = manager.jobQuery(originalJob);
        QFile originalManifest(original.value(QStringLiteral("resultManifestRef")).toString());
        QVERIFY(originalManifest.open(QIODevice::ReadOnly));
        const QJsonObject rawOriginalManifest = QJsonDocument::fromJson(originalManifest.readAll()).object();
        originalManifest.close();
        QVERIFY(rawOriginalManifest.value(QStringLiteral("originTable")).isObject());
        const QJsonArray originalFrames = rawOriginalManifest.value(QStringLiteral("frames")).toArray();
        QVERIFY(!originalFrames.isEmpty());
        QVERIFY(originalFrames.first().toObject().contains(QStringLiteral("originRef")));
        QVERIFY(!originalFrames.first().toObject().contains(QStringLiteral("origin")));
        Model restart;
        ModelIO::open(restart, original.value(QStringLiteral("resultArtifactRef")).toString());
        restart.getProblemSetup().setRestart(true);
        // Native restart input step counts are additional steps: harmonize
        // retains accepted steps 1..400 and appends 600, ending at record 1000.
        restart.getProblemSetup().getStructuralDynamicsSetup().setNSteps(600);
        restart.getProblemSetup().getStructuralDynamicsSetup().setOutputFrequency(200);
        restart.getTimeSolver().setInputNTimeSteps(600);
        restart.getTimeSolver().setTimes(RTimeSolver::findTimesVector(600, 0.0, 0.001));
        restart.getTimeSolver().setOutputFrequency(200);
        const uint restartID = models.add(restart);
        const QString restartedJob = manager.submitValidated(restartID, {}, true, &error);
        QVERIFY2(!restartedJob.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(restartedJob).value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"), 120000);
        QVERIFY(waitForManagerIdle(manager));
        const QJsonObject catalog = manager.nativeResultQuery(restartedJob, QStringLiteral("catalog"), {});
        QVERIFY2(catalog.value(QStringLiteral("valid")).toBool(), qPrintable(catalog.value(QStringLiteral("message")).toString()));
        const QJsonArray frames = catalog.value(QStringLiteral("frames")).toArray();
        const QJsonObject restarted = manager.jobQuery(restartedJob);
        QFile originIndex(QDir(restarted.value(QStringLiteral("inputPackageRef")).toString())
                          .filePath(QStringLiteral("history/origin-index.json")));
        QVERIFY(originIndex.open(QIODevice::ReadOnly));
        const QJsonArray capturedOrigins = QJsonDocument::fromJson(originIndex.readAll()).array();
        QCOMPARE(capturedOrigins.size(), originalFrames.size());
        for (int index = 0; index < capturedOrigins.size(); ++index)
        {
            const QJsonObject captured = capturedOrigins.at(index).toObject();
            const QJsonObject originalFrame = originalFrames.at(index).toObject();
            QCOMPARE(captured.value(QStringLiteral("recordNo")), originalFrame.value(QStringLiteral("recordNo")));
            QCOMPARE(captured.value(QStringLiteral("originalFrameSha256")), originalFrame.value(QStringLiteral("sha256")));
            QCOMPARE(captured.value(QStringLiteral("origin")).toObject(),
                     original.value(QStringLiteral("nativeResultProvenance")).toObject());
            QCOMPARE(captured.value(QStringLiteral("originManifestSha256")), original.value(QStringLiteral("resultManifestSha256")));
        }
        QCOMPARE(frames.size(), 4);
        for (int index = 0; index < frames.size(); ++index)
        {
            const QJsonObject frame = frames.at(index).toObject();
            const int recordNo = 400 + 200 * index;
            QCOMPARE(frame.value(QStringLiteral("recordNo")).toInt(), recordNo);
            QVERIFY(std::fabs(frame.value(QStringLiteral("timeS")).toDouble() - double(recordNo) * 0.001) < 1e-10);
            QCOMPARE(frame.value(QStringLiteral("originKind")).toString(), index == 0
                     ? QStringLiteral("reused-history") : QStringLiteral("produced"));
            // Compare the complete origin, including input, source and runtime
            // hashes; prior history must never inherit the continuation job.
            QCOMPARE(frame.value(QStringLiteral("origin")).toObject(),
                     (index == 0 ? original : restarted).value(QStringLiteral("nativeResultProvenance")).toObject());
        }
        const QJsonObject reused = frames.first().toObject();
        const QJsonObject proof = reused.value(QStringLiteral("historyProof")).toObject();
        QCOMPARE(proof.value(QStringLiteral("sha256")), reused.value(QStringLiteral("sha256")));
        QCOMPARE(proof.value(QStringLiteral("originalFrameSha256")).toString(),
                 original.value(QStringLiteral("resultSourceBinding")).toObject().value(QStringLiteral("savedArtifactHash")).toString());
        QCOMPARE(proof.value(QStringLiteral("originManifestSha256")), original.value(QStringLiteral("resultManifestSha256")));
        QCOMPARE(proof.value(QStringLiteral("originManifestPath")), original.value(QStringLiteral("resultManifestRef")));
        const QJsonArray journal = restarted.value(QStringLiteral("resultFrames")).toArray();
        QCOMPARE(journal.size(), 3);
        for (int index = 0; index < journal.size(); ++index)
        {
            QCOMPARE(journal.at(index).toObject().value(QStringLiteral("seq")).toInt(), index + 1);
            QCOMPARE(journal.at(index).toObject().value(QStringLiteral("recordNo")).toInt(), 600 + 200 * index);
            QCOMPARE(journal.at(index).toObject().value(QStringLiteral("sha256")), frames.at(index + 1).toObject().value(QStringLiteral("sha256")));
        }

        // Even a semantically unchanged replacement must fail the retained
        // registry hash; it cannot fall back to unattributed legacy history.
        QFile ancestor(original.value(QStringLiteral("resultManifestRef")).toString());
        QVERIFY(ancestor.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(ancestor.write("\n"), qint64(1));
        ancestor.close();
        auto rejected = std::make_unique<SolverTask>(Application::instance()->getApplicationSettings(), restartID);
        QVERIFY(!rejected->captureInputSnapshot(restart, session->modelCommandStamp(restartID), restartID));
        QVERIFY(!manager.nativeResultQuery(restartedJob, QStringLiteral("catalog"), {}).value(QStringLiteral("valid")).toBool());
    }

    void terminalModelReplacementAfterReadbackCannotSucceed()
    {
        for (const bool replaceAfterManifest : {false, true})
        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            Session *session = Application::instance()->getSession();
            SolverManager &manager = SolverManager::getInstance();
            QVERIFY(waitForManagerIdle(manager));
            SessionModelGuard models(session);
            Model model;
            QVERIFY(loadTinyTruss(directory, QStringLiteral("replacement-gate"), model));
            configureSingleOutputDynamics(model);
            const uint modelID = models.add(model);
            auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
            QVERIFY(task->captureInputSnapshot(session->getModel(modelID), session->modelCommandStamp(modelID), modelID));
            auto started = std::make_shared<QSemaphore>();
            auto release = std::make_shared<QSemaphore>();
            GateReleaseGuard releaseGuard{release};
            manager.RJobManager::submit(new GateJob(started, release));
            QVERIFY(waitForGate(started));
            auto replaced = std::make_shared<std::atomic_bool>(false);
            const auto replace = [replaced](const QString &path)
            {
                try
                {
                    RModel replacement;
                    replacement.read(path);
                    replacement.setDescription(QStringLiteral("MODEL Y replaced readback MODEL X"));
                    replacement.write(path, true);
                    replaced->store(true);
                }
                catch (const RError &) {}
            };
            if (!replaceAfterManifest)
                QObject::connect(task, &SolverTask::resultArtifactCreated, task,
                                 [replace](const QString &, const QString &, const QJsonObject &binding)
                {
                    replace(binding.value(QStringLiteral("resolvedSourcePath")).toString());
                }, Qt::DirectConnection);
            QString error;
            const QString jobId = manager.submitValidated(task, {}, true, &error);
            QVERIFY2(!jobId.isEmpty(), qPrintable(error));
            if (replaceAfterManifest)
                // Connected after registration: the manager first verifies the
                // descriptor, then this queued replacement runs before its
                // terminal evidence handler. This exercises the success gate.
                QObject::connect(task, &SolverTask::nativeResultManifestPublished, &manager,
                                 [replace](const QString &, const QJsonObject &descriptor)
                {
                    const QString root = QFileInfo(descriptor.value(QStringLiteral("resultManifestRef")).toString()).absolutePath();
                    replace(QDir(root).filePath(descriptor.value(QStringLiteral("resultManifest")).toObject()
                                               .value(QStringLiteral("terminalModel")).toObject()
                                               .value(QStringLiteral("relativePath")).toString()));
                }, Qt::QueuedConnection);
            release->release();
            releaseGuard.release.reset();
            QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                      QStringLiteral("failed"), 120000);
            QVERIFY(replaced->load());
            QVERIFY(!manager.jobQuery(jobId).value(QStringLiteral("resultManifestVerified")).toBool());
            QVERIFY(!manager.nativeResultQuery(jobId, QStringLiteral("catalog"), {}).value(QStringLiteral("valid")).toBool());
            QVERIFY(waitForManagerIdle(manager));
        }
    }

    void supportedFrozenInputCannotUseUnsupportedSignalAsSuccessEvidence()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("unsupported-signal-gate"), model));
        configureSingleOutputDynamics(model);
        // Native RSolver derives its clock from Newmark configuration. This
        // valid input remains supported even before that clock is enabled.
        model.getTimeSolver().setEnabled(false);
        const uint modelID = models.add(model);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(task->captureInputSnapshot(session->getModel(modelID), session->modelCommandStamp(modelID), modelID));
        auto started = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        GateReleaseGuard releaseGuard{release};
        manager.RJobManager::submit(new GateJob(started, release));
        QVERIFY(waitForGate(started));
        QString error;
        const QString jobId = manager.submitValidated(task, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QVERIFY(manager.jobQuery(jobId).value(QStringLiteral("nativeResultCatalogExpected")).toBool());
        const QString path = task->getResultArtifactFileName();
        ModelIO::save(model, path, false);
        RProjectCatalog::SourceBinding binding;
        Model saved;
        ModelIO::open(saved, path, &binding);
        QJsonArray artifacts;
        for (const auto &artifact : binding.sourceArtifacts)
            artifacts.append(QJsonObject{{QStringLiteral("path"), artifact.path}, {QStringLiteral("sha256"), artifact.sha256}});
        const QJsonObject bindingJson{{QStringLiteral("logicalSourcePath"), binding.logicalSourcePath},
                                      {QStringLiteral("resolvedSourcePath"), binding.resolvedSourcePath},
                                      {QStringLiteral("savedArtifactHash"), binding.savedArtifactHash},
                                      {QStringLiteral("sourceArtifacts"), artifacts}};
        const QString hash = fileSha256(path);
        task->resultArtifactCreated(path, hash, bindingJson);
        task->nativeResultCatalogUnavailable(jobId, QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"), QStringLiteral("Injected unsupported result"));
        task->terminalReported(jobId, QStringLiteral("succeeded"), true, true, 0, true, path, hash, true, {});
        QTRY_COMPARE(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QVERIFY(manager.jobQuery(jobId).value(QStringLiteral("resultArtifactVerified")).toBool());
        task->kill();
        release->release();
        releaseGuard.release.reset();
        QVERIFY(waitForManagerIdle(manager));
    }

    void invalidSetupCreatesNoJob()
    {
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model invalidSource;
        invalidSource.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
        const uint modelID = models.add(invalidSource);
        const int priorTotal = manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();
        QCOMPARE(manager.jobsQuery(0, 0).value(QStringLiteral("error")).toString(), QStringLiteral("invalid_pagination"));
        QCOMPARE(manager.jobsQuery(-1, 1).value(QStringLiteral("error")).toString(), QStringLiteral("invalid_pagination"));
        QCOMPARE(manager.jobsQuery(0, 201).value(QStringLiteral("error")).toString(), QStringLiteral("invalid_pagination"));

        const QJsonObject validation = manager.validateRun(modelID);
        QVERIFY(!validation.value(QStringLiteral("valid")).toBool());
        QVERIFY(!validation.value(QStringLiteral("errors")).toArray().isEmpty());

        QString error;
        const QString jobId = manager.submitValidated(modelID, QJsonObject(), true, &error);
        QVERIFY(jobId.isEmpty());
        QVERIFY(!error.isEmpty());
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), priorTotal);
        QCOMPARE(manager.getNWaiting(), 0u);
        QCOMPARE(manager.getNRunning(), 0u);
    }

    void sourceSnapshotSurvivesEditAndCloseWithoutTouchingOtherDocument()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model sourceA;
        QString sourcePathA;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("source-a"), sourceA, &sourcePathA));
        configureSingleOutputDynamics(sourceA);
        sourceA.setDescription(QStringLiteral("source A before submit"));
        const uint modelA = models.add(sourceA);
        const QString identityA = models.identity(modelA);
        const QPair<QString,quint64> stampA = session->modelCommandStamp(modelA);

        Model otherDocument;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("other-document"), otherDocument));
        otherDocument.setDescription(QStringLiteral("other document remains untouched"));
        models.add(otherDocument);
        const QString otherIdentity = models.identity(session->getNModels() - 1);

        // Manual GUI submission carries an explicitly empty project ref. The
        // input package is captured before this call returns, while result
        // publication remains queued on the GUI thread.
        QString error;
        const QString jobId = manager.submitValidated(modelA, QJsonObject(), true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        const QJsonObject queued = manager.jobQuery(jobId);
        QVERIFY(queued.value(QStringLiteral("found")).toBool());
        QVERIFY(queued.value(QStringLiteral("provenanceFrozen")).toBool());
        const QJsonObject queuedProvenance = queued.value(QStringLiteral("nativeResultProvenance")).toObject();
        QCOMPARE(queuedProvenance.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(queuedProvenance.value(QStringLiteral("inputSnapshotSha256")).toString(),
                 queued.value(QStringLiteral("inputSnapshotSha256")).toString());
        QCOMPARE(queuedProvenance.value(QStringLiteral("inputPackageSha256")).toString(),
                 queued.value(QStringLiteral("inputPackageSha256")).toString());
        QCOMPARE(queued.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), identityA);
        QCOMPARE(queued.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("revision")).toString(), QString::number(stampA.second));
        QCOMPARE(queued.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("sessionEpoch")).toString(),
                 session->getModelCommandHost()->sessionEpoch());
        QCOMPARE(queued.value(QStringLiteral("projectLink")).toString(), QStringLiteral("unlinked"));
        QCOMPARE(queued.value(QStringLiteral("inputDerivedFrom")).toString(), QStringLiteral("live_unlinked"));
        QVERIFY(!queued.value(QStringLiteral("frozenSnapshotVerified")).toBool());

        const QString inputPath = queued.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString resultPath = queued.value(QStringLiteral("resultArtifactRef")).toString();
        QVERIFY(QFileInfo::exists(inputPath));
        QVERIFY(QFileInfo::exists(sourcePathA));

        uint currentA = session->findModelByCommandIdentity(identityA);
        QVERIFY(currentA != RConstants::eod);
        session->storeCurentModelVersion(currentA, QStringLiteral("Edit source A while solver waits"));
        session->getModel(currentA).setDescription(QStringLiteral("source A edited after submit"));
        session->setProblemChanged(currentA);
        QCOMPARE(session->modelCommandStamp(currentA).second, stampA.second + 1);

        currentA = session->findModelByCommandIdentity(identityA);
        QVERIFY(currentA != RConstants::eod);
        session->removeModel(currentA);
        QVERIFY(session->findModelByCommandIdentity(identityA) == RConstants::eod);

        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);
        const QJsonObject finished = manager.jobQuery(jobId);
        QCOMPARE(finished.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), identityA);
        QCOMPARE(finished.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("revision")).toString(), QString::number(stampA.second));
        QCOMPARE(finished.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("sessionEpoch")).toString(),
                 session->getModelCommandHost()->sessionEpoch());
        QVERIFY(finished.value(QStringLiteral("processStarted")).toBool());
        QVERIFY(finished.value(QStringLiteral("processExited")).toBool());
        QCOMPARE(finished.value(QStringLiteral("processExitCode")).toInt(), 0);
        QVERIFY(finished.value(QStringLiteral("resultReadbackComplete")).toBool());
        QVERIFY2(finished.value(QStringLiteral("progressAvailable")).toBool(),
                 qPrintable(finished.value(QStringLiteral("progressUnavailableReason")).toString()));
        QCOMPARE(finished.value(QStringLiteral("progressStatus")).toString(), QStringLiteral("available"));
        QVERIFY(finished.value(QStringLiteral("progressSequence")).toDouble() > 0);
        const QJsonArray progressEvents = finished.value(QStringLiteral("progressEvents")).toArray();
        QVERIFY(!progressEvents.isEmpty());
        double priorSequence = 0;
        for (const QJsonValue &value : progressEvents)
        {
            const QJsonObject event = value.toObject();
            QCOMPARE(event.value(QStringLiteral("jobId")).toString(), jobId);
            QCOMPARE(event.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("range.native-progress/v1"));
            QVERIFY(event.value(QStringLiteral("seq")).toDouble() > priorSequence);
            priorSequence = event.value(QStringLiteral("seq")).toDouble();
            if (event.value(QStringLiteral("pulse")).toBool())
                QVERIFY(event.value(QStringLiteral("fraction")).isNull());
            else
            {
                const double fraction = event.value(QStringLiteral("fraction")).toDouble(-1.0);
                QVERIFY(fraction >= 0.0 && fraction <= 1.0);
            }
        }
        QVERIFY(QFileInfo::exists(resultPath));
        QCOMPARE(finished.value(QStringLiteral("resultArtifactRef")).toString(),
                 QFileInfo(resultPath).absoluteFilePath());
        QVERIFY(finished.value(QStringLiteral("resultArtifactVerified")).toBool());
        QCOMPARE(finished.value(QStringLiteral("resultArtifactSha256")).toString(), fileSha256(resultPath));
        QVERIFY(finished.value(QStringLiteral("resultSourceBindingVerified")).toBool());
        const QJsonObject resultSourceBinding = finished.value(QStringLiteral("resultSourceBinding")).toObject();
        QCOMPARE(resultSourceBinding.value(QStringLiteral("logicalSourcePath")).toString(),
                 QFileInfo(resultPath).canonicalFilePath());
        const QJsonArray resultSourceArtifacts = resultSourceBinding.value(QStringLiteral("sourceArtifacts")).toArray();
        QVERIFY(!resultSourceArtifacts.isEmpty());
        QCOMPARE(resultSourceArtifacts.first().toObject().value(QStringLiteral("path")).toString(),
                 resultSourceBinding.value(QStringLiteral("logicalSourcePath")).toString());
        QCOMPARE(resultSourceArtifacts.first().toObject().value(QStringLiteral("sha256")).toString(),
                 finished.value(QStringLiteral("resultArtifactSha256")).toString());
        QCOMPARE(resultSourceArtifacts.last().toObject().value(QStringLiteral("path")).toString(),
                 resultSourceBinding.value(QStringLiteral("resolvedSourcePath")).toString());
        QCOMPARE(resultSourceArtifacts.last().toObject().value(QStringLiteral("sha256")).toString(),
                 resultSourceBinding.value(QStringLiteral("savedArtifactHash")).toString());

        QVERIFY(finished.value(QStringLiteral("nativeResultCatalogExpected")).toBool());
        QVERIFY(finished.value(QStringLiteral("provenanceFrozen")).toBool());
        QVERIFY(finished.value(QStringLiteral("resultManifestDescriptorReported")).toBool());
        QVERIFY(finished.value(QStringLiteral("resultManifestVerified")).toBool());
        QVERIFY(finished.value(QStringLiteral("nativeResultCatalogAvailable")).toBool());
        const QJsonObject provenance = finished.value(QStringLiteral("nativeResultProvenance")).toObject();
        QCOMPARE(provenance, queuedProvenance);
        QCOMPARE(provenance.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(provenance.value(QStringLiteral("sourceRuntimeStamp")).toObject(),
                 finished.value(QStringLiteral("sourceRuntimeStamp")).toObject());
        QCOMPARE(provenance.value(QStringLiteral("sourceSnapshotRef")).toObject(),
                 finished.value(QStringLiteral("sourceSnapshotRef")).toObject());
        QCOMPARE(provenance.value(QStringLiteral("inputSnapshotSha256")).toString(),
                 finished.value(QStringLiteral("inputSnapshotSha256")).toString());
        QCOMPARE(provenance.value(QStringLiteral("inputPackageSha256")).toString(),
                 finished.value(QStringLiteral("inputPackageSha256")).toString());
        QCOMPARE(provenance.value(QStringLiteral("inputSnapshotSha256")).toString(), fileSha256(inputPath));
        QCOMPARE(provenance.value(QStringLiteral("inputPackageSha256")).toString(),
                 directorySha256(QFileInfo(inputPath).absolutePath()));
        const QJsonObject runtime = provenance.value(QStringLiteral("runtime")).toObject();
        QCOMPARE(runtime.value(QStringLiteral("scope")).toString(),
                 QStringLiteral("solver-and-declared-controller-runtime/v1"));
        QCOMPARE(runtime.value(QStringLiteral("fingerprintSha256")).toString().size(), 64);
        QCOMPARE(provenance.value(QStringLiteral("proofScope")).toString(),
                 QStringLiteral("solver-and-declared-controller-runtime/v1"));
        const QJsonObject control = provenance.value(QStringLiteral("control")).toObject();
        QVERIFY(!control.value(QStringLiteral("enabled")).toBool());
        QVERIFY(control.value(QStringLiteral("controllerId")).toString().isEmpty());
        QVERIFY(control.value(QStringLiteral("controllerVersionSha256")).toString().isEmpty());

        const QString manifestPath = finished.value(QStringLiteral("resultManifestRef")).toString();
        const QString resultDirectory = QFileInfo(resultPath).absolutePath();
        QCOMPARE(QFileInfo(manifestPath).fileName(), QStringLiteral("native-result.manifest.json"));
        QCOMPARE(QFileInfo(manifestPath).absolutePath(), resultDirectory);
        QVERIFY(QFileInfo::exists(manifestPath));
        QVERIFY(!QFileInfo(manifestPath).isSymLink());
        QDir privateRunDirectory(resultDirectory);
        QCOMPARE(privateRunDirectory.dirName(), QStringLiteral("result"));
        QVERIFY(privateRunDirectory.cdUp());
        QVERIFY(privateRunDirectory.dirName().startsWith(jobId + QLatin1Char('-')));
        QVERIFY(privateRunDirectory.absolutePath().contains(QStringLiteral(".range-runs")));
        QCOMPARE(finished.value(QStringLiteral("resultManifestSha256")).toString(), fileSha256(manifestPath));

        QFile manifestFile(manifestPath);
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestFile.readAll());
        QVERIFY(manifestDocument.isObject());
        const QJsonObject manifest = manifestDocument.object();
        QCOMPARE(manifest.value(QStringLiteral("apiVersion")).toString(), QStringLiteral("range.native-results/v1"));
        QCOMPARE(manifest.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(manifest.value(QStringLiteral("artifactId")).toString(),
                 finished.value(QStringLiteral("resultArtifactId")).toString());
        QCOMPARE(manifest.value(QStringLiteral("sourceRuntimeStamp")).toObject(),
                 provenance.value(QStringLiteral("sourceRuntimeStamp")).toObject());
        QCOMPARE(manifest.value(QStringLiteral("sourceSnapshotRef")).toObject(),
                 provenance.value(QStringLiteral("sourceSnapshotRef")).toObject());
        QCOMPARE(manifest.value(QStringLiteral("inputSnapshotSha256")).toString(),
                 provenance.value(QStringLiteral("inputSnapshotSha256")).toString());
        QCOMPARE(manifest.value(QStringLiteral("inputPackageSha256")).toString(),
                 provenance.value(QStringLiteral("inputPackageSha256")).toString());
        QCOMPARE(manifest.value(QStringLiteral("runtime")).toObject(), runtime);
        QCOMPARE(manifest.value(QStringLiteral("control")).toObject(), control);
        QCOMPARE(manifest.value(QStringLiteral("proofScope")).toString(),
                 provenance.value(QStringLiteral("proofScope")).toString());
        QCOMPARE(manifest.value(QStringLiteral("analysis")).toObject()
                     .value(QStringLiteral("type")).toString(), QStringLiteral("structural-dynamics"));
        QCOMPARE(manifest.value(QStringLiteral("logicalLatest")).toObject()
                     .value(QStringLiteral("sha256")).toString(), fileSha256(resultPath));
        QCOMPARE(manifest.value(QStringLiteral("logicalLatest")).toObject()
                     .value(QStringLiteral("type")).toString(), QStringLiteral("LINK"));
        QCOMPARE(manifest.value(QStringLiteral("sampleScope")).toString(),
                 QStringLiteral("all-persisted-numbered-model-records"));
        const QJsonArray availableVariables = manifest.value(QStringLiteral("availableVariables")).toArray();
        QVERIFY(std::any_of(availableVariables.cbegin(), availableVariables.cend(), [](const QJsonValue &value)
        {
            return value.toString() == QStringLiteral("var-displacement");
        }));

        const QJsonArray frames = manifest.value(QStringLiteral("frames")).toArray();
        QCOMPARE(frames.size(), 1);
        const QJsonObject frame = frames.first().toObject();
        QCOMPARE(frame.value(QStringLiteral("recordNo")).toInt(), 400);
        QVERIFY(std::fabs(frame.value(QStringLiteral("timeS")).toDouble() - 0.4) < 1e-12);
        const QString frameRelativePath = frame.value(QStringLiteral("relativePath")).toString();
        QCOMPARE(QFileInfo(frameRelativePath).fileName(), frameRelativePath);
        const QString framePath = QDir(resultDirectory).filePath(frameRelativePath);
        QCOMPARE(QFileInfo(framePath).absolutePath(), resultDirectory);
        QVERIFY(QFileInfo::exists(framePath));
        QVERIFY(!QFileInfo(framePath).isSymLink());
        QCOMPARE(frame.value(QStringLiteral("sha256")).toString(), fileSha256(framePath));
        QCOMPARE(frame.value(QStringLiteral("nodePhysicalIdMapSha256")).toString(),
                 manifest.value(QStringLiteral("nodePhysicalIdMapSha256")).toString());
        QCOMPARE(manifest.value(QStringLiteral("terminalModel")).toObject()
                     .value(QStringLiteral("relativePath")).toString(), frameRelativePath);
        QCOMPARE(manifest.value(QStringLiteral("terminalModel")).toObject()
                     .value(QStringLiteral("sha256")).toString(), frame.value(QStringLiteral("sha256")).toString());

        QVERIFY(finished.value(QStringLiteral("resultFramesAvailable")).toBool());
        QCOMPARE(finished.value(QStringLiteral("resultFrameSequence")).toInt(), 1);
        const QJsonArray journalFrames = finished.value(QStringLiteral("resultFrames")).toArray();
        QCOMPARE(journalFrames.size(), 1);
        QCOMPARE(journalFrames.first().toObject().value(QStringLiteral("recordNo")).toInt(), 400);
        QCOMPARE(journalFrames.first().toObject().value(QStringLiteral("sha256")).toString(), frame.value(QStringLiteral("sha256")).toString());
        const QJsonObject completedCatalog = manager.nativeResultQuery(jobId, QStringLiteral("catalog"), {});
        QVERIFY2(completedCatalog.value(QStringLiteral("valid")).toBool(), qPrintable(completedCatalog.value(QStringLiteral("message")).toString()));
        const QString liveId = finished.value(QStringLiteral("resultArtifactId")).toString() + QStringLiteral("-live-1");
        const QJsonObject liveCatalog = manager.nativeResultQuery(jobId, QStringLiteral("catalog"), {{QStringLiteral("artifactId"), liveId}});
        QVERIFY2(liveCatalog.value(QStringLiteral("valid")).toBool(), qPrintable(liveCatalog.value(QStringLiteral("message")).toString()));
        QVERIFY(liveCatalog.value(QStringLiteral("live")).toBool());
        QVERIFY(!liveCatalog.value(QStringLiteral("completed")).toBool());
        QCOMPARE(liveCatalog.value(QStringLiteral("artifactId")).toString(), liveId);
        QVERIFY(!manager.nativeResultQuery(jobId, QStringLiteral("catalog"), {{QStringLiteral("artifactId"), QStringLiteral("artifact-wrong-job")}})
                .value(QStringLiteral("valid")).toBool());

        Model capturedInput;
        ModelIO::open(capturedInput, inputPath);
        QCOMPARE(capturedInput.getDescription(), QStringLiteral("source A before submit"));
        const uint otherID = session->findModelByCommandIdentity(otherIdentity);
        QVERIFY(otherID != RConstants::eod);
        QCOMPARE(session->getModel(otherID).getDescription(), QStringLiteral("other document remains untouched"));
    }

    void linkedProjectInputUsesVerifiedFrozenBodyAndRejectsTampering()
    {
        QTemporaryDir sourceDirectory;
        QVERIFY(sourceDirectory.isValid());
        QTemporaryDir &workspace = linkedProjectWorkspace();
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        QString projectError;
        QVERIFY2(session->openProject(workspace.path(), &projectError), qPrintable(projectError));

        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(sourceDirectory, QStringLiteral("linked-frozen"), source));
        source.setDescription(QStringLiteral("frozen project body"));
        const double frozenNodeX = source.getNode(0).getX();
        const uint modelID = models.add(source);
        QString error;
        const QJsonObject projectSnapshotRef = session->createDocumentSnapshot(modelID, &error);
        QVERIFY2(!projectSnapshotRef.isEmpty(), qPrintable(error));
        const QPair<QString,quint64> frozenStamp = session->modelCommandStamp(modelID);

        // Simulate an unnotified live data edit and a GUI selection change. The
        // runtime stamp still matches the frozen project reference, so the
        // solver input must come from the managed snapshot, not this live view.
        Model &liveModel = session->getModel(modelID);
        liveModel.setDescription(QStringLiteral("unnotified live edit"));
        RNode changedNode = liveModel.getNode(0);
        changedNode.setX(frozenNodeX + 0.125);
        liveModel.setNode(0, changedNode);
        liveModel.setSelected(true);
        QCOMPARE(session->modelCommandStamp(modelID), frozenStamp);

        const QString jobId = manager.submitValidated(modelID, projectSnapshotRef, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        const QJsonObject queued = manager.jobQuery(jobId);
        QCOMPARE(queued.value(QStringLiteral("projectLink")).toString(), QStringLiteral("linked"));
        QCOMPARE(queued.value(QStringLiteral("inputDerivedFrom")).toString(), QStringLiteral("frozen_project_snapshot"));
        QVERIFY(queued.value(QStringLiteral("frozenSnapshotVerified")).toBool());
        QCOMPARE(queued.value(QStringLiteral("frozenSnapshotSha256")).toString(),
                 projectSnapshotRef.value(QStringLiteral("modelSha256")).toString());
        QCOMPARE(queued.value(QStringLiteral("frozenSnapshotPath")).toString(),
                 QFileInfo(QDir(session->projectWorkspace())
                               .filePath(projectSnapshotRef.value(QStringLiteral("relativePath")).toString())
                 ).canonicalFilePath());
        QCOMPARE(queued.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("sessionEpoch")).toString(),
                 session->getModelCommandHost()->sessionEpoch());
        QVERIFY(!queued.value(QStringLiteral("inputSnapshotSha256")).toString().isEmpty());
        QVERIFY(!queued.value(QStringLiteral("inputPackageSha256")).toString().isEmpty());
        const QString inputPath = queued.value(QStringLiteral("inputSnapshotRef")).toString();
        QCOMPARE(queued.value(QStringLiteral("inputSnapshotSha256")).toString(), fileSha256(inputPath));
        QCOMPARE(queued.value(QStringLiteral("inputPackageSha256")).toString(),
                 directorySha256(QFileInfo(inputPath).absolutePath()));
        Model capturedInput;
        ModelIO::open(capturedInput, inputPath);
        QCOMPARE(capturedInput.getDescription(), QStringLiteral("frozen project body"));
        QCOMPARE(capturedInput.getNode(0).getX(), frozenNodeX);
        QVERIFY(!capturedInput.getSelected());

        session->removeModel(modelID);
        QVERIFY(session->findModelByCommandIdentity(frozenStamp.first) == RConstants::eod);
        QVERIFY2(manager.cancelJob(jobId, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 15000);
        QVERIFY(waitForManagerIdle(manager));
        const QJsonObject afterCloseCandidate = manager.jobQuery(jobId);
        QCOMPARE(afterCloseCandidate.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), frozenStamp.first);
        QCOMPARE(afterCloseCandidate.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("sessionEpoch")).toString(),
                 session->getModelCommandHost()->sessionEpoch());

        Model tamperedSource;
        QVERIFY(loadTinyTruss(sourceDirectory, QStringLiteral("linked-tamper"), tamperedSource));
        const uint tamperedModelID = models.add(tamperedSource);
        const QJsonObject tamperedRef = session->createDocumentSnapshot(tamperedModelID, &error);
        QVERIFY2(!tamperedRef.isEmpty(), qPrintable(error));
        const QString tamperedPath = QDir(session->projectWorkspace())
                .filePath(tamperedRef.value(QStringLiteral("relativePath")).toString());
        QFile tamperedFile(tamperedPath);
        QVERIFY(tamperedFile.open(QIODevice::ReadWrite));
        const QByteArray firstByte = tamperedFile.read(1);
        QCOMPARE(firstByte.size(), 1);
        QVERIFY(tamperedFile.seek(0));
        const char changedByte = char(firstByte.at(0) ^ 0x01);
        QCOMPARE(tamperedFile.write(&changedByte, 1), qint64(1));
        QVERIFY(tamperedFile.flush());
        tamperedFile.close();

        const int priorJobTotal = manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();
        const QString rejectedJob = manager.submitValidated(tamperedModelID, tamperedRef, true, &error);
        QVERIFY(rejectedJob.isEmpty());
        QVERIFY(error.contains(QStringLiteral("modelSha256")) || error.contains(QStringLiteral("snapshot bytes")));
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), priorJobTotal);
    }

    void legacyJobBlocksValidationBeforeModelInspection()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("legacy-validation-gate"), model));
        const uint modelID = models.add(model);

        auto gateStarted = std::make_shared<QSemaphore>();
        auto gateRelease = std::make_shared<QSemaphore>();
        GateReleaseGuard gateGuard{gateRelease};
        RJobManager::getInstance().submit(new GateJob(gateStarted, gateRelease));
        QVERIFY(waitForGate(gateStarted));

        const int priorJobTotal = manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();
        const QJsonObject validation = manager.validateRun(modelID);
        QVERIFY(!validation.value(QStringLiteral("valid")).toBool());
        QCOMPARE(validation.value(QStringLiteral("errors")).toArray().first().toString(),
                 QStringLiteral("Solver validation is unavailable while a legacy or live-model job is active or queued."));
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), priorJobTotal);

        gateRelease->release();
        gateGuard.release.reset();
        QVERIFY(waitForManagerIdle(manager));
    }

    void preCapturedDetachedTaskCanQueueBehindUnknownManagerJob()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("pre-captured-gate"), model));
        const uint modelID = models.add(model);
        const QPair<QString,quint64> sourceStamp = session->modelCommandStamp(modelID);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(task->captureInputSnapshot(session->getModel(modelID), sourceStamp, modelID));

        auto gateStarted = std::make_shared<QSemaphore>();
        auto gateRelease = std::make_shared<QSemaphore>();
        GateReleaseGuard gateGuard{gateRelease};
        manager.RJobManager::submit(new GateJob(gateStarted, gateRelease));
        QVERIFY(waitForGate(gateStarted));

        QString error;
        const QString jobId = manager.submitValidated(task, QJsonObject(), true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        const QJsonObject queued = manager.jobQuery(jobId);
        QCOMPARE(queued.value(QStringLiteral("state")).toString(), QStringLiteral("queued"));
        QCOMPARE(queued.value(QStringLiteral("inputDerivedFrom")).toString(), QStringLiteral("live_unlinked"));
        QVERIFY(QFileInfo::exists(queued.value(QStringLiteral("inputSnapshotRef")).toString()));

        QVERIFY2(manager.cancelJob(jobId, &error), qPrintable(error));
        QCOMPARE(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        gateRelease->release();
        gateGuard.release.reset();
        QVERIFY(waitForManagerIdle(manager));
    }

    void cancellingOneQueuedJobDoesNotCancelTheNextJob()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model modelA;
        Model modelB;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("cancel-a"), modelA));
        QVERIFY(loadTinyTruss(directory, QStringLiteral("cancel-b"), modelB));
        modelA.setDescription(QStringLiteral("job A"));
        modelB.setDescription(QStringLiteral("job B"));
        const uint modelAId = models.add(modelA);
        const uint modelBId = models.add(modelB);

        QString error;
        const QString jobA = manager.submitValidated(modelAId, QJsonObject(), true, &error);
        QVERIFY2(!jobA.isEmpty(), qPrintable(error));
        const QString jobB = manager.submitValidated(modelBId, QJsonObject(), true, &error);
        QVERIFY2(!jobB.isEmpty(), qPrintable(error));

        QVERIFY2(manager.cancelJob(jobB, &error), qPrintable(error));
        const QString cancellingState = manager.jobQuery(jobB).value(QStringLiteral("state")).toString();
        QVERIFY(cancellingState == QStringLiteral("cancelling") || cancellingState == QStringLiteral("cancelled"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobB).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobA).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);
        const QJsonObject cancelledB = manager.jobQuery(jobB);
        QVERIFY(cancelledB.value(QStringLiteral("cancellationRequested")).toBool());
        QVERIFY(cancelledB.value(QStringLiteral("state")).toString() != QStringLiteral("succeeded"));
        if (cancelledB.value(QStringLiteral("resultArtifactVerified")).toBool())
        {
            const QString cancelledResult = cancelledB.value(QStringLiteral("resultArtifactRef")).toString();
            QVERIFY(QFileInfo::exists(cancelledResult));
            QCOMPARE(cancelledB.value(QStringLiteral("resultArtifactSha256")).toString(), fileSha256(cancelledResult));
        }
        const QJsonObject finishedA = manager.jobQuery(jobA);
        QVERIFY(finishedA.value(QStringLiteral("resultReadbackComplete")).toBool());
        QVERIFY(finishedA.value(QStringLiteral("resultArtifactVerified")).toBool());
        QVERIFY(finishedA.value(QStringLiteral("resultManifestVerified")).toBool());
        QVERIFY(finishedA.value(QStringLiteral("nativeResultCatalogAvailable")).toBool());
        const QString resultPathA = finishedA.value(QStringLiteral("resultArtifactRef")).toString();
        QCOMPARE(finishedA.value(QStringLiteral("resultArtifactSha256")).toString(), fileSha256(resultPathA));
        QFile staticManifestFile(finishedA.value(QStringLiteral("resultManifestRef")).toString());
        QVERIFY(staticManifestFile.open(QIODevice::ReadOnly));
        const QJsonObject staticManifest = QJsonDocument::fromJson(staticManifestFile.readAll()).object();
        QCOMPARE(staticManifest.value(QStringLiteral("analysis")).toObject()
                     .value(QStringLiteral("type")).toString(), QStringLiteral("static"));
        QVERIFY(staticManifest.value(QStringLiteral("frames")).toArray().isEmpty());
    }

    void nativeCheckpointReservedBasenameDoesNotCollideWithUserModel()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model model;
        QString sourcePath;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("native-checkpoint-input"), model, &sourcePath));
        QCOMPARE(QFileInfo(sourcePath).fileName(), QStringLiteral("native-checkpoint-input.ram"));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelId = models.add(model);
        QString error;
        const QString jobId = manager.submitValidated(modelId, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);

        const QJsonObject record = manager.jobQuery(jobId);
        QVERIFY(record.value(QStringLiteral("nativeCheckpointEnabled")).toBool());
        const QString inputSnapshotPath = record.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString checkpointInputPath = record.value(QStringLiteral("nativeCheckpointInputModelRef")).toString();
        QVERIFY(QFileInfo(inputSnapshotPath).isFile());
        QVERIFY(QFileInfo(checkpointInputPath).isFile());
        QVERIFY(inputSnapshotPath != checkpointInputPath);
        QCOMPARE(QFileInfo(record.value(QStringLiteral("sourceModelFileRef")).toString()).canonicalFilePath(),
                 QFileInfo(sourcePath).canonicalFilePath());
        QVERIFY(QFileInfo(inputSnapshotPath).fileName() != QStringLiteral("native-checkpoint-input.ram"));
        QCOMPARE(QFileInfo(checkpointInputPath).fileName(), QStringLiteral("native-checkpoint-input.ram"));

        SolverTask detachedRecoveryTask(Application::instance()->getApplicationSettings(), model,
                                        {QStringLiteral("test-source-document"), quint64(0)},
                                        {{QStringLiteral("recoverySourceJobId"), QStringLiteral("test-source-job")}},
                                        directory.path(), QStringLiteral("test-recovery-task-server"));
        const QString recoveryCommand = detachedRecoveryTask.getCommandLine();
        QCOMPARE(recoveryCommand.count(QStringLiteral("\"--restart\"")), 1);
        QCOMPARE(QFileInfo(detachedRecoveryTask.getInputSnapshotFileName()).suffix(),
                 RModel::getDefaultFileExtension(true));
    }

    void gracefulRegisteredStopRetainsAcceptedPrefix_data()
    {
        QTest::addColumn<bool>("staleSource");
        QTest::newRow("current-source") << false;
        QTest::newRow("stale-source") << true;
    }

    void gracefulRegisteredStopRetainsAcceptedPrefix()
    {
        QFETCH(bool, staleSource);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("graceful-stop"), model));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelID = models.add(model);
        NativeThreadPoolGate gate;
        QVERIFY(gate.start());
        QString error;
        const QString jobId = manager.submitValidated(modelID, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QVERIFY2(manager.requestNativeAcceptedBoundaryPause(jobId, 10, &error), qPrintable(error));
        const auto queued = manager.jobQuery(jobId);
        const QString executionPath = queued.value(QStringLiteral("nativeExecutionStateRef")).toString();
        gate.releaseNow();
        const auto cleanup = qScopeGuard([&] {
            try { manager.stopRunningTasks(); } catch (const RError &) {}
            waitForManagerIdle(manager);
        });
        QTRY_COMPARE_WITH_TIMEOUT(nativeExecutionStateObject(executionPath).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("paused"), 30000);
        QCOMPARE(nativeExecutionStateObject(executionPath).value(QStringLiteral("acceptedSeq")).toInt(), 10);
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("processStarted")).toBool(), 10000);
        const auto before = manager.jobQuery(jobId);
        const QString inputPath = before.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString inputHash = fileSha256(inputPath);
        const QString inputPackage = before.value(QStringLiteral("inputPackageRef")).toString();
        const QString packageHash = directorySha256(inputPackage);
        if (staleSource)
        {
            session->storeCurentModelVersion(modelID, QStringLiteral("Edit source while paused"));
            session->getModel(modelID).setDescription(QStringLiteral("Source changed after native input freeze"));
            session->setProblemChanged(modelID);
        }
        bool durableBeforeStopSignal = false;
        QString firstCancellationTime;
        const auto duplicate = QObject::connect(&manager, &SolverManager::nativeJobUpdated, &manager,
            [&](const QString &updatedId, const QJsonObject &fields) {
                if (updatedId != jobId || !fields.value(QStringLiteral("cancellationRequested")).toBool()) return;
                const auto durable = persistedNativeJobRecord(manager.jobQuery(jobId));
                firstCancellationTime = durable.value(QStringLiteral("cancelRequestedAt")).toString();
                durableBeforeStopSignal = durable.value(QStringLiteral("state")).toString() == QStringLiteral("cancelling") &&
                    durable.value(QStringLiteral("cancellationRequested")).toBool() &&
                    !firstCancellationTime.isEmpty() &&
                    nativeExecutionStateObject(executionPath).value(QStringLiteral("state")).toString() == QStringLiteral("paused");
                // Direct reentry must not replace the first durable request time.
                manager.stopRunningTasks();
            }, Qt::DirectConnection);
        const QString stopFailure = [&] {
            try { manager.stopRunningTasks(); return QString(); }
            catch (const RError &e) { return e.getMessage(); }
        }();
        QObject::disconnect(duplicate);
        QVERIFY2(stopFailure.isEmpty(), qPrintable(stopFailure));
        QVERIFY(durableBeforeStopSignal);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        QVERIFY(waitForManagerIdle(manager));
        const auto stopped = manager.jobQuery(jobId);
        QVERIFY(stopped.value(QStringLiteral("cancellationRequested")).toBool());
        QCOMPARE(stopped.value(QStringLiteral("cancelRequestedAt")).toString(), firstCancellationTime);
        QVERIFY(QDateTime::fromString(firstCancellationTime, Qt::ISODateWithMs).isValid());
        QVERIFY(stopped.value(QStringLiteral("processStarted")).toBool());
        QVERIFY(stopped.value(QStringLiteral("processExited")).toBool());
        QCOMPARE(stopped.value(QStringLiteral("processExitCode")).toInt(-1), 0);
        QVERIFY(stopped.value(QStringLiteral("resultReadbackComplete")).toBool());
        QVERIFY(stopped.value(QStringLiteral("resultArtifactVerified")).toBool());
        QVERIFY(stopped.value(QStringLiteral("resultManifestVerified")).toBool());
        QCOMPARE(persistedNativeJobRecord(stopped).value(QStringLiteral("cancelRequestedAt")).toString(), firstCancellationTime);
        QCOMPARE(fileSha256(inputPath), inputHash);
        QCOMPARE(directorySha256(inputPackage), packageHash);
        RModel accepted;
        accepted.read(stopped.value(QStringLiteral("resultArtifactRef")).toString());
        QCOMPARE(accepted.getProblemSetup().getStructuralDynamicsRestartData().completedSteps, 10u);
        QCOMPARE(accepted.getProblemSetup().getStructuralDynamicsSetup().getNSteps(), 20u);
        // Explicit Stop is a terminal cancellation, not an accidental interruption.
        // Its accepted prefix remains intact without silently making it resumable.
        const QString cancelledRecordHash = fileSha256(stopped.value(QStringLiteral("nativeJobRecordRef")).toString());
        const auto recovery = manager.nativeJobRecoveryQuery(jobId);
        QCOMPARE(recovery.value(QStringLiteral("eligibility")).toString(), QStringLiteral("recompute_required"));
        QCOMPARE(recovery.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(persistedNativeJobRecord(stopped).value(QStringLiteral("cancelRequestedAt")).toString(), firstCancellationTime);
        QCOMPARE(fileSha256(stopped.value(QStringLiteral("nativeJobRecordRef")).toString()), cancelledRecordHash);
        const auto checkpoint = latestNativeCheckpointEnvelope(stopped);
        QVERIFY(!checkpoint.isEmpty());
        QCOMPARE(checkpoint.value(QStringLiteral("acceptedSeq")).toInt(), 10);
        const QString checkpointName = checkpoint.value(QStringLiteral("modelFile")).toString();
        QVERIFY(!checkpointName.isEmpty());
        QCOMPARE(QFileInfo(checkpointName).fileName(), checkpointName);
        const QString checkpointPath = QDir(stopped.value(QStringLiteral("nativeCheckpointDirectory")).toString()).filePath(checkpointName);
        QCOMPARE(fileSha256(checkpointPath), checkpoint.value(QStringLiteral("checkpointModelSha256")).toString());
        RModel checkpointModel;
        checkpointModel.read(checkpointPath);
        QCOMPARE(checkpointModel.getProblemSetup().getStructuralDynamicsRestartData().completedSteps, 10u);
        QCOMPARE(checkpointModel.getProblemSetup().getStructuralDynamicsSetup().getNSteps()
                 - checkpointModel.getProblemSetup().getStructuralDynamicsRestartData().completedSteps, 10u);
        if (staleSource)
            QCOMPARE(session->getModel(modelID).getDescription(), QStringLiteral("Source changed after native input freeze"));
        const QString finalRecordHash = fileSha256(stopped.value(QStringLiteral("nativeJobRecordRef")).toString());
        manager.stopRunningTasks();
        QCOMPARE(fileSha256(stopped.value(QStringLiteral("nativeJobRecordRef")).toString()), finalRecordHash);
    }

    void gracefulStopPersistenceFailureSendsNoStop()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("graceful-stop-persistence"), model));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelID = models.add(model);
        NativeThreadPoolGate gate;
        QVERIFY(gate.start());
        QString error;
        const QString jobId = manager.submitValidated(modelID, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QVERIFY2(manager.requestNativeAcceptedBoundaryPause(jobId, 10, &error), qPrintable(error));
        const QString executionPath = manager.jobQuery(jobId).value(QStringLiteral("nativeExecutionStateRef")).toString();
        gate.releaseNow();
        QLockFile writerLock(QDir(QFileInfo(manager.jobQuery(jobId).value(QStringLiteral("nativeJobRecordRef")).toString()).absolutePath())
                             .filePath(QStringLiteral(".job-record.lock")));
        writerLock.setStaleLockTime(0);
        const auto cleanup = qScopeGuard([&] {
            writerLock.unlock();
            try { manager.stopRunningTasks(); } catch (const RError &) {}
            waitForManagerIdle(manager);
        });
        QTRY_COMPARE_WITH_TIMEOUT(nativeExecutionStateObject(executionPath).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("paused"), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("processStarted")).toBool(), 10000);
        const auto before = manager.jobQuery(jobId);
        const QString recordPath = before.value(QStringLiteral("nativeJobRecordRef")).toString();
        const QString recordHash = fileSha256(recordPath);
        const QString pausedHash = fileSha256(executionPath);
        QVERIFY(writerLock.tryLock(0));
        qint64 liveLockPid = 0;
        QString liveLockHost;
        QString liveLockApp;
        QVERIFY(writerLock.getLockInfo(&liveLockPid, &liveLockHost, &liveLockApp));
        QCOMPARE(liveLockPid, QCoreApplication::applicationPid());
        bool rejected = false;
        try { manager.stopRunningTasks(); } catch (const RError &) { rejected = true; }
        QVERIFY(rejected);
        QTest::qWait(100);
        const auto failedRequest = manager.jobQuery(jobId);
        QCOMPARE(failedRequest.value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        QVERIFY(!failedRequest.value(QStringLiteral("cancellationRequested")).toBool());
        QVERIFY(!failedRequest.contains(QStringLiteral("cancelRequestedAt")));
        QCOMPARE(failedRequest.value(QStringLiteral("diagnostic")).toObject().value(QStringLiteral("code")).toString(),
                 QStringLiteral("NATIVE_JOB_PERSISTENCE_FAILED"));
        const QString lockDiagnostic = failedRequest.value(QStringLiteral("diagnostic")).toObject()
                                           .value(QStringLiteral("message")).toString();
        QVERIFY(lockDiagnostic.contains(QStringLiteral("PID %1").arg(liveLockPid)));
        if (!liveLockHost.isEmpty()) QVERIFY(lockDiagnostic.contains(liveLockHost));
        if (!liveLockApp.isEmpty()) QVERIFY(lockDiagnostic.contains(liveLockApp));
        QCOMPARE(fileSha256(recordPath), recordHash);
        QCOMPARE(fileSha256(executionPath), pausedHash);
        QVERIFY(!failedRequest.value(QStringLiteral("processExited")).toBool());
        writerLock.unlock();

        // Exercise QLockFile's stale-owner decisions through the same
        // registry persistence path: an aged same-process owner remains live,
        // while a dead same-host PID is recoverable.
        const QString recordDirectory = QFileInfo(recordPath).absolutePath();
        const QString recordLockPath = QDir(recordDirectory).filePath(QStringLiteral(".job-record.lock"));
        const QByteArray liveOwnerLockContents = QByteArray::number(liveLockPid) + '\n' + liveLockApp.toUtf8() + '\n' +
                                                 liveLockHost.toUtf8() + '\n';
        QFile liveOwnerLockFile(recordLockPath);
        QVERIFY(liveOwnerLockFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(liveOwnerLockFile.write(liveOwnerLockContents), qint64(liveOwnerLockContents.size()));
        QVERIFY(liveOwnerLockFile.flush());
        liveOwnerLockFile.close();
        QFile agedLiveLockAge(recordLockPath);
        QVERIFY(agedLiveLockAge.open(QIODevice::ReadWrite));
        QVERIFY(agedLiveLockAge.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-300),
                                            QFileDevice::FileModificationTime));
        agedLiveLockAge.close();
        const QString agedLiveLockHash = fileSha256(recordLockPath);
        bool agedLiveLockRejected = false;
        try { manager.stopRunningTasks(); } catch (const RError &) { agedLiveLockRejected = true; }
        QVERIFY(agedLiveLockRejected);
        QTest::qWait(100);
        const auto agedLiveLockRequest = manager.jobQuery(jobId);
        QCOMPARE(agedLiveLockRequest.value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        QVERIFY(!agedLiveLockRequest.value(QStringLiteral("cancellationRequested")).toBool());
        QVERIFY(!agedLiveLockRequest.contains(QStringLiteral("cancelRequestedAt")));
        QCOMPARE(agedLiveLockRequest.value(QStringLiteral("diagnostic")).toObject().value(QStringLiteral("code")).toString(),
                 QStringLiteral("NATIVE_JOB_PERSISTENCE_FAILED"));
        const QString agedLockDiagnostic = agedLiveLockRequest.value(QStringLiteral("diagnostic")).toObject()
                                               .value(QStringLiteral("message")).toString();
        QVERIFY(agedLockDiagnostic.contains(QStringLiteral("PID %1").arg(liveLockPid)));
        if (!liveLockHost.isEmpty()) QVERIFY(agedLockDiagnostic.contains(liveLockHost));
        if (!liveLockApp.isEmpty()) QVERIFY(agedLockDiagnostic.contains(liveLockApp));
        QCOMPARE(fileSha256(recordPath), recordHash);
        QCOMPARE(fileSha256(executionPath), pausedHash);
        QCOMPARE(fileSha256(recordLockPath), agedLiveLockHash);
        QVERIFY(!agedLiveLockRequest.value(QStringLiteral("processExited")).toBool());

        constexpr qint64 deadOwnerPid = 2147483647;
        const QByteArray staleLockContents = QByteArray::number(deadOwnerPid) + '\n' + liveLockApp.toUtf8() + '\n' +
                                             liveLockHost.toUtf8() + '\n';
        QFile staleLockFile(recordLockPath);
        QVERIFY(staleLockFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(staleLockFile.write(staleLockContents), qint64(staleLockContents.size()));
        QVERIFY(staleLockFile.flush());
        staleLockFile.close();
        QFile staleLockAge(recordLockPath);
        QVERIFY(staleLockAge.open(QIODevice::ReadWrite));
        QVERIFY(staleLockAge.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-300),
                                         QFileDevice::FileModificationTime));
        staleLockAge.close();
        QLockFile staleLockProbe(recordLockPath);
        qint64 observedDeadPid = 0;
        QString observedDeadHost;
        QString observedDeadApp;
        QVERIFY(staleLockProbe.getLockInfo(&observedDeadPid, &observedDeadHost, &observedDeadApp));
        QCOMPARE(observedDeadPid, deadOwnerPid);
        QCOMPARE(observedDeadHost, liveLockHost);
        QCOMPARE(observedDeadApp, liveLockApp);

        const QDateTime retryTime = QDateTime::currentDateTimeUtc();
        manager.stopRunningTasks();
        QVERIFY(!QFileInfo::exists(recordLockPath));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        const auto stopped = manager.jobQuery(jobId);
        QCOMPARE(stopped.value(QStringLiteral("processExitCode")).toInt(-1), 0);
        QVERIFY(QDateTime::fromString(stopped.value(QStringLiteral("cancelRequestedAt")).toString(), Qt::ISODateWithMs) >= retryTime);
        QVERIFY(waitForManagerIdle(manager));
    }

    void gracefulStopWinsAlreadyReportedSuccessRace()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("graceful-stop-success-race"), model));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelID = models.add(model);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(task->captureInputSnapshot(session->getModel(modelID), session->modelCommandStamp(modelID), modelID));
        bool observedRealSuccess = false;
        QString stopError;
        // Registered before the manager's terminal handler: the real worker has
        // already selected succeeded, but cannot queue it until Stop is durable.
        QObject::connect(task, &SolverTask::terminalReported, &manager,
            [&](const QString &, const QString &reportedState) {
                observedRealSuccess = reportedState == QStringLiteral("succeeded");
                if (observedRealSuccess)
                    try { manager.stopRunningTasks(); }
                    catch (const RError &e) { stopError = e.getMessage(); }
            }, Qt::BlockingQueuedConnection);
        QString error;
        const QString jobId = manager.submitValidated(task, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        QVERIFY(observedRealSuccess);
        QVERIFY2(stopError.isEmpty(), qPrintable(stopError));
        const auto stopped = manager.jobQuery(jobId);
        QVERIFY(stopped.value(QStringLiteral("cancellationRequested")).toBool());
        QVERIFY(stopped.value(QStringLiteral("resultManifestVerified")).toBool());
        QCOMPARE(stopped.value(QStringLiteral("processExitCode")).toInt(-1), 0);
        QVERIFY(waitForManagerIdle(manager));
    }

    void gracefulStopCannotHideChangedResult()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("graceful-stop-result-tamper"), model));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelID = models.add(model);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(task->captureInputSnapshot(session->getModel(modelID), session->modelCommandStamp(modelID), modelID));
        bool changed = false;
        QString changeError;
        QObject::connect(task, &SolverTask::resultArtifactCreated, &manager,
            [&](const QString &, const QString &, const QJsonObject &binding) {
                try
                {
                    manager.stopRunningTasks();
                    RModel replacement;
                    const QString path = binding.value(QStringLiteral("resolvedSourcePath")).toString();
                    replacement.read(path);
                    replacement.setDescription(QStringLiteral("Invalid result after graceful Stop request"));
                    replacement.write(path, true);
                    changed = true;
                }
                catch (const RError &e) { changeError = e.getMessage(); }
            }, Qt::BlockingQueuedConnection);
        QString error;
        const QString jobId = manager.submitValidated(task, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        QVERIFY2(changeError.isEmpty(), qPrintable(changeError));
        QVERIFY(changed);
        const auto failed = manager.jobQuery(jobId);
        QVERIFY(failed.value(QStringLiteral("cancellationRequested")).toBool());
        QVERIFY(!failed.value(QStringLiteral("resultManifestVerified")).toBool());
        QVERIFY(waitForManagerIdle(manager));
    }

    void gracefulDeletedSenderCannotBypassTerminalEvidence_data()
    {
        QTest::addColumn<bool>("denyTerminalPersistence");
        QTest::newRow("durable-failed-before-lease-release") << false;
        QTest::newRow("failed-persistence-retains-lease") << true;
    }

    void gracefulDeletedSenderCannotBypassTerminalEvidence()
    {
        QFETCH(bool, denyTerminalPersistence);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        QVERIFY(waitForManagerIdle(SolverManager::getInstance()));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("deleted-terminal-sender"), model));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelID = models.add(model);
        DeletedSenderTerminalProbe manager;
        auto started = std::make_shared<QSemaphore>();
        auto release = std::make_shared<QSemaphore>();
        GateReleaseGuard releaseGuard{release};
        manager.RJobManager::submit(new GateJob(started, release));
        QVERIFY(waitForGate(started));
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(task->captureInputSnapshot(session->getModel(modelID), session->modelCommandStamp(modelID), modelID));
        const QString jobId = task->getTaskID().toString();
        auto lease = std::make_shared<QLockFile>(directory.filePath(QStringLiteral("terminal-evidence-lease.lock")));
        lease->setStaleLockTime(0);
        QVERIFY(lease->tryLock(0));
        manager.retainFixtureLease(jobId, lease);
        QString error;
        QCOMPARE(manager.submitValidated(task, {}, true, &error), jobId);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        manager.holdSenderOutsideWorkerQueue(task);
        manager.stopRunningTasks();
        const auto cancelling = manager.jobQuery(jobId);
        QCOMPARE(cancelling.value(QStringLiteral("state")).toString(), QStringLiteral("cancelling"));
        const QString cancelTime = cancelling.value(QStringLiteral("cancelRequestedAt")).toString();
        const QString recordDirectory = QFileInfo(cancelling.value(QStringLiteral("nativeJobRecordRef")).toString()).absolutePath();
        QLockFile writerLock(QDir(recordDirectory).filePath(QStringLiteral(".job-record.lock")));
        writerLock.setStaleLockTime(0);
        const auto cleanup = qScopeGuard([&] {
            writerLock.unlock();
            manager.cleanupFixtureLease(jobId);
            release->release();
            waitForManagerIdle(manager);
        });
        bool observedDurableFailureWhileLeaseHeld = false;
        const auto committed = QObject::connect(&manager, &SolverManager::nativeJobUpdated, &manager,
            [&](const QString &updatedId, const QJsonObject &fields) {
                if (updatedId != jobId || fields.value(QStringLiteral("state")).toString() != QStringLiteral("failed")) return;
                const auto durable = persistedNativeJobRecord(manager.jobQuery(jobId));
                observedDurableFailureWhileLeaseHeld = lease->isLocked() &&
                    durable.value(QStringLiteral("state")).toString() == QStringLiteral("failed") &&
                    durable.value(QStringLiteral("cancelRequestedAt")).toString() == cancelTime;
            }, Qt::DirectConnection);
        if (denyTerminalPersistence) QVERIFY(writerLock.tryLock(0));
        QPointer<SolverTask> senderGuard(task);
        // Negative metadata injection only: an unverified prefix is never valid
        // terminal evidence. Queue the production handler, then destroy sender.
        task->terminalReported(jobId, QStringLiteral("cancelled"), true, true, 0,
                               false, {}, {}, false, QStringLiteral("Stopped by the user; verified prefix retained."));
        manager.detachSenderBeforeDeletion(task);
        delete task;
        QVERIFY(senderGuard.isNull());
        QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
        QObject::disconnect(committed);
        const auto terminal = manager.jobQuery(jobId);
        QCOMPARE(terminal.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QCOMPARE(terminal.value(QStringLiteral("cancelRequestedAt")).toString(), cancelTime);
        QVERIFY(!terminal.value(QStringLiteral("resultArtifactVerified")).toBool());
        QVERIFY(terminal.value(QStringLiteral("message")).toString().contains(QStringLiteral("failed")));
        const auto durable = persistedNativeJobRecord(terminal);
        if (denyTerminalPersistence)
        {
            QCOMPARE(durable.value(QStringLiteral("state")).toString(), QStringLiteral("cancelling"));
            QVERIFY(!observedDurableFailureWhileLeaseHeld);
            QVERIFY(lease->isLocked());
            QVERIFY(manager.retainsFixtureLease(jobId));
            QCOMPARE(terminal.value(QStringLiteral("diagnostic")).toObject().value(QStringLiteral("code")).toString(),
                     QStringLiteral("NATIVE_JOB_PERSISTENCE_FAILED"));
        }
        else
        {
            QCOMPARE(durable.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
            QVERIFY(observedDurableFailureWhileLeaseHeld);
            QVERIFY(!lease->isLocked());
            QVERIFY(!manager.retainsFixtureLease(jobId));
        }
        writerLock.unlock();
        release->release();
        releaseGuard.release.reset();
        QVERIFY(waitForManagerIdle(manager));
    }

    void gracefulStopBeforeRealChildConnectionIsDelivered()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("stop-before-real-connection"), model));
        configureSingleOutputDynamics(model, 20, 1);
        const uint modelID = models.add(model);
        NativeThreadPoolGate gate;
        QVERIFY(gate.start());
        QString error;
        const QString jobId = manager.submitValidated(modelID, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QVERIFY2(manager.requestNativeAcceptedBoundaryPause(jobId, 10, &error), qPrintable(error));
        const auto queued = manager.jobQuery(jobId);
        QVERIFY(!queued.value(QStringLiteral("processStarted")).toBool());
        // Pause bounds even a late socket delivery before the original target.
        // The manager has started its RJob, while its real worker and command
        // socket are still held by the unrelated thread-pool gate.
        manager.stopRunningTasks();
        const auto requested = manager.jobQuery(jobId);
        QCOMPARE(requested.value(QStringLiteral("state")).toString(), QStringLiteral("cancelling"));
        QVERIFY(persistedNativeJobRecord(requested).value(QStringLiteral("cancellationRequested")).toBool());
        const QString cancelTime = requested.value(QStringLiteral("cancelRequestedAt")).toString();
        gate.releaseNow();
        const auto cleanup = qScopeGuard([&] {
            try { manager.stopRunningTasks(); } catch (const RError &) {}
            waitForManagerIdle(manager);
        });
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        QVERIFY(waitForManagerIdle(manager));
        const auto terminal = manager.jobQuery(jobId);
        QVERIFY(terminal.value(QStringLiteral("processStarted")).toBool());
        QVERIFY(terminal.value(QStringLiteral("processExited")).toBool());
        QCOMPARE(terminal.value(QStringLiteral("processExitCode")).toInt(-1), 0);
        QCOMPARE(terminal.value(QStringLiteral("cancelRequestedAt")).toString(), cancelTime);
        QVERIFY(terminal.value(QStringLiteral("resultManifestVerified")).toBool());
        RModel accepted;
        accepted.read(terminal.value(QStringLiteral("resultArtifactRef")).toString());
        QVERIFY(accepted.getProblemSetup().getStructuralDynamicsRestartData().completedSteps < 20u);
        QCOMPARE(accepted.getProblemSetup().getStructuralDynamicsSetup().getNSteps(), 20u);
    }

    void nativeRecoveryContinuesFromAcceptedCheckpointAndKeepsPrefixProvenance()
    {
#ifndef Q_OS_WIN
        QSKIP("The deterministic interrupted-process fixture requires verified Windows child termination.");
#endif
        const QString auditRoot = qEnvironmentVariable("RANGE_NATIVE_JOB_AUDIT_DIR");
        if (!auditRoot.isEmpty()) QVERIFY(QDir().mkpath(auditRoot));
        const QString directoryTemplate = auditRoot.isEmpty()
                ? QString() : QDir(auditRoot).filePath(QStringLiteral("native-recovery-XXXXXX"));
        QTemporaryDir directory(directoryTemplate);
        // An explicitly requested audit root retains the fixture even when a QtTest assertion exits early.
        if (!auditRoot.isEmpty()) directory.setAutoRemove(false);
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));
        constexpr uint targetSteps = 20;
        constexpr uint checkpointSequence = 10;
        constexpr uint secondInterruptionSequence = 15;

        SessionModelGuard models(session);
        Model continuousModel;
        Model interruptedModel;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("recovery-continuous"), continuousModel));
        QVERIFY(loadTinyTruss(directory, QStringLiteral("recovery-interrupted"), interruptedModel));
        configureSingleOutputDynamics(continuousModel, targetSteps, 1);
        configureSingleOutputDynamics(interruptedModel, targetSteps, 1);
        const uint continuousModelId = models.add(continuousModel);
        const uint interruptedModelId = models.add(interruptedModel);
        const QPair<QString,quint64> interruptedSourceStamp = session->modelCommandStamp(interruptedModelId);

        QString error;
        const QString continuousJobId = manager.submitValidated(continuousModelId, {}, true, &error);
        QVERIFY2(!continuousJobId.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(continuousJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);

        NativeThreadPoolGate initialGate;
        QVERIFY2(initialGate.start(), "Could not start the non-RJob global thread-pool gate.");
        const QString interruptedJobId = manager.submitValidated(interruptedModelId, {}, true, &error);
        QVERIFY2(!interruptedJobId.isEmpty(), qPrintable(error));
        const QJsonObject initialQueuedRecord = manager.jobQuery(interruptedJobId);
        const QString initialInputSnapshotPath = initialQueuedRecord.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString initialInputSnapshotSha = initialQueuedRecord.value(QStringLiteral("inputSnapshotSha256")).toString();
        const QString initialInputPackagePath = initialQueuedRecord.value(QStringLiteral("inputPackageRef")).toString();
        const QString initialInputPackageSha = initialQueuedRecord.value(QStringLiteral("inputPackageSha256")).toString();
        bool reentrantPauseObserverCalled = false;
        bool reentrantPauseAccepted = true;
        QString reentrantPauseError;
        const QMetaObject::Connection pauseReentryConnection = QObject::connect(
            &manager, &SolverManager::nativeJobUpdated, &manager,
            [&manager, &reentrantPauseObserverCalled, &reentrantPauseAccepted, &reentrantPauseError,
             interruptedJobId](const QString &updatedJobId, const QJsonObject &fields)
            {
                if (updatedJobId != interruptedJobId ||
                    !fields.contains(QStringLiteral("nativeExecutionPauseRequestedSeq"))) return;
                reentrantPauseObserverCalled = true;
                reentrantPauseAccepted = manager.requestNativeAcceptedBoundaryPause(
                    interruptedJobId, 10, &reentrantPauseError);
            }, Qt::DirectConnection);
        const bool initialPauseRequested = manager.requestNativeAcceptedBoundaryPause(
            interruptedJobId, checkpointSequence, &error);
        QObject::disconnect(pauseReentryConnection);
        if (!initialPauseRequested)
        {
            QString cancelError;
            manager.cancelJob(interruptedJobId, &cancelError);
            initialGate.releaseNow();
            QVERIFY2(initialPauseRequested, qPrintable(error));
        }
        QVERIFY(reentrantPauseObserverCalled);
        QVERIFY(!reentrantPauseAccepted);
        QVERIFY(reentrantPauseError.contains(QStringLiteral("configuration is busy")));
        const QJsonObject initialPauseRequest = manager.jobQuery(interruptedJobId);
        QCOMPARE(initialPauseRequest.value(QStringLiteral("state")).toString(), QStringLiteral("queued"));
        QCOMPARE(initialPauseRequest.value(QStringLiteral("inputSnapshotSha256")).toString(), initialInputSnapshotSha);
        QCOMPARE(initialPauseRequest.value(QStringLiteral("inputPackageSha256")).toString(), initialInputPackageSha);
        QCOMPARE(fileSha256(initialInputSnapshotPath), initialInputSnapshotSha);
        QCOMPARE(directorySha256(initialInputPackagePath), initialInputPackageSha);
        QCOMPARE(initialPauseRequest.value(QStringLiteral("nativeExecutionPauseRequestedSeq")).toInt(),
                 int(checkpointSequence));
        const QString initialExecutionStatePath =
                initialPauseRequest.value(QStringLiteral("nativeExecutionStateRef")).toString();
        QVERIFY(!initialExecutionStatePath.isEmpty());
        initialGate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(nativeExecutionStateObject(initialExecutionStatePath)
                                      .value(QStringLiteral("state")).toString(),
                                  QStringLiteral("paused"), 30000);
        const QJsonObject initialPauseAck = nativeExecutionStateObject(initialExecutionStatePath);
        QCOMPARE(initialPauseAck.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.native-execution-state/v1"));
        QCOMPARE(initialPauseAck.value(QStringLiteral("generation")).toInt(), 1);
        QCOMPARE(initialPauseAck.value(QStringLiteral("jobId")).toString(), interruptedJobId);
        QCOMPARE(initialPauseAck.value(QStringLiteral("taskId")).toString(), interruptedJobId);
        QCOMPARE(initialPauseAck.value(QStringLiteral("pauseAtAcceptedSeq")).toInt(), int(checkpointSequence));
        QCOMPARE(initialPauseAck.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
        QVERIFY(initialPauseAck.value(QStringLiteral("checkpointAvailable")).toBool());
        QCOMPARE(initialPauseAck.value(QStringLiteral("checkpointAcceptedSeq")).toInt(), int(checkpointSequence));
        QJsonObject initialCheckpointPointer;
        const QJsonObject initialCheckpointEnvelope =
                latestNativeCheckpointEnvelope(initialPauseRequest, &initialCheckpointPointer);
        QVERIFY(!initialCheckpointEnvelope.isEmpty());
        QCOMPARE(initialCheckpointEnvelope.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
        QCOMPARE(initialPauseAck.value(QStringLiteral("checkpointEnvelopeSha256")).toString(),
                 initialCheckpointPointer.value(QStringLiteral("envelopeSha256")).toString());
        QCOMPARE(initialPauseAck.value(QStringLiteral("checkpointModelSha256")).toString(),
                 initialCheckpointEnvelope.value(QStringLiteral("checkpointModelSha256")).toString());
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(interruptedJobId)
                                     .value(QStringLiteral("processStarted")).toBool(), 10000);
        const QJsonObject initialLiveRecord = manager.jobQuery(interruptedJobId);
        QCOMPARE(initialLiveRecord.value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        QVERIFY(!initialLiveRecord.value(QStringLiteral("processExited")).toBool());
        bool initialPidOk = false;
        QVERIFY(initialLiveRecord.value(QStringLiteral("nativeProcessId")).toString()
                    .toLongLong(&initialPidOk) > 0);
        QVERIFY(initialPidOk);
        const QJsonObject initialDurableLiveRecord = persistedNativeJobRecord(initialLiveRecord);
        QVERIFY(!initialDurableLiveRecord.isEmpty());
        QVERIFY(initialDurableLiveRecord.value(QStringLiteral("processStarted")).toBool());
        QVERIFY(!initialDurableLiveRecord.value(QStringLiteral("processExited")).toBool());
        QCOMPARE(initialDurableLiveRecord.value(QStringLiteral("nativeProcessId")).toString(),
                 initialLiveRecord.value(QStringLiteral("nativeProcessId")).toString());
        const QJsonObject initialPauseContext =
                initialPauseRequest.value(QStringLiteral("nativeCheckpointContext")).toObject();
        QCOMPARE(initialPauseAck.value(QStringLiteral("contextSha256")).toString(),
                 jsonObjectSha256(initialPauseContext));
        for (const QString &hashField : {QStringLiteral("sourceSha256"), QStringLiteral("modelSha256"),
                                         QStringLiteral("controlSha256"), QStringLiteral("runtimeSha256"),
                                         QStringLiteral("packageSha256")})
            QCOMPARE(initialPauseAck.value(hashField).toString(), initialPauseContext.value(hashField).toString());
        QVERIFY2(terminateVerifiedOwnRecoverySolver(
                     Application::instance()->getApplicationSettings()->getSolverPath(), &error),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(interruptedJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        QVERIFY(waitForManagerIdle(manager));
        double interruptedCheckpointSequence = -1.0;
        QVERIFY(latestAcceptedSequence(manager.jobQuery(interruptedJobId), &interruptedCheckpointSequence));
        QCOMPARE(interruptedCheckpointSequence, double(checkpointSequence));

        const QJsonObject interruptedTerminal = manager.jobQuery(interruptedJobId);
        const QString originalResultPath = interruptedTerminal.value(QStringLiteral("resultArtifactRef")).toString();
        QVERIFY(!originalResultPath.isEmpty());
        QVERIFY(QFileInfo(originalResultPath).isFile());
        const QString oldManifestPath = interruptedTerminal.value(QStringLiteral("resultManifestRef")).toString();
        QVERIFY(oldManifestPath.isEmpty() || !QFileInfo::exists(oldManifestPath));
        const QString originalInputSnapshotPath = interruptedTerminal.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString originalInputSnapshotSha = interruptedTerminal.value(QStringLiteral("inputSnapshotSha256")).toString();
        const QString originalInputPackagePath = interruptedTerminal.value(QStringLiteral("inputPackageRef")).toString();
        const QString originalInputPackageSha = interruptedTerminal.value(QStringLiteral("inputPackageSha256")).toString();
        QVERIFY(!originalInputSnapshotPath.isEmpty());
        QVERIFY(!originalInputPackagePath.isEmpty());
        QCOMPARE(fileSha256(originalInputSnapshotPath), originalInputSnapshotSha);
        QCOMPARE(directorySha256(originalInputPackagePath), originalInputPackageSha);

        QVERIFY(persistAsInterruptedForTest(manager, interruptedJobId, nullptr));

        SolverManager recovered;
        const QJsonObject qualification = recovered.nativeJobRecoveryQuery(interruptedJobId);
        QVERIFY2(qualification.value(QStringLiteral("eligibility")).toString() == QStringLiteral("recoverable"),
                 qPrintable(recoveryQualificationSummary(qualification)));
        const QJsonObject selectedCheckpoint = qualification.value(QStringLiteral("checkpoint")).toObject();
        QCOMPARE(selectedCheckpoint.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
        QCOMPARE(selectedCheckpoint.value(QStringLiteral("targetSteps")).toInt(), int(targetSteps));
        QCOMPARE(selectedCheckpoint.value(QStringLiteral("remainingSteps")).toInt(),
                 int(targetSteps - checkpointSequence));
        QVERIFY(!selectedCheckpoint.value(QStringLiteral("usedLastValid")).toBool());
        const QJsonObject firstPartialResult = qualification.value(QStringLiteral("partialResult")).toObject();
        const int firstPartialAcceptedSequence = firstPartialResult.value(QStringLiteral("acceptedSeq")).toInt(-1);
        QVERIFY(firstPartialAcceptedSequence >= selectedCheckpoint.value(QStringLiteral("acceptedSeq")).toInt());
        QVERIFY(firstPartialAcceptedSequence < selectedCheckpoint.value(QStringLiteral("targetSteps")).toInt());
        QCOMPARE(firstPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt(),
                 firstPartialAcceptedSequence - selectedCheckpoint.value(QStringLiteral("acceptedSeq")).toInt());
        QVERIFY(firstPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt() <= 10);
        QVERIFY(firstPartialResult.value(QStringLiteral("discardedSimulationSeconds")).isDouble());
        const double firstDiscardedSimulationSeconds =
                firstPartialResult.value(QStringLiteral("discardedSimulationSeconds")).toDouble();
        QVERIFY(firstDiscardedSimulationSeconds >= 0.0 && firstDiscardedSimulationSeconds <= 1.0);

        const int jobsBeforeBadRequest = recovered.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();
        QVERIFY(recovered.submitNativeRecovery(
                    interruptedJobId, selectedCheckpoint.value(QStringLiteral("checkpointSha256")).toString(),
                    9, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("remaining step count")));
        QCOMPARE(recovered.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), jobsBeforeBadRequest);

        NativeThreadPoolGate recoveryGate;
        QVERIFY2(recoveryGate.start(), "Could not start the non-RJob recovery thread-pool gate.");
        const QString recoveryJobId = recovered.submitNativeRecovery(
            interruptedJobId, selectedCheckpoint.value(QStringLiteral("checkpointSha256")).toString(),
            targetSteps - checkpointSequence, &error);
        QVERIFY2(!recoveryJobId.isEmpty(), qPrintable(error));
        QCOMPARE(fileSha256(originalInputSnapshotPath), originalInputSnapshotSha);
        QCOMPARE(directorySha256(originalInputPackagePath), originalInputPackageSha);
        const QJsonObject seededRecoveryJob = recovered.jobQuery(recoveryJobId);
        const QString firstRecoveryInputSnapshotPath = seededRecoveryJob.value(QStringLiteral("inputSnapshotRef")).toString();
        const QString firstRecoveryInputSnapshotSha = seededRecoveryJob.value(QStringLiteral("inputSnapshotSha256")).toString();
        const QString firstRecoveryInputPackagePath = seededRecoveryJob.value(QStringLiteral("inputPackageRef")).toString();
        const QString firstRecoveryInputPackageSha = seededRecoveryJob.value(QStringLiteral("inputPackageSha256")).toString();
        const QString firstRecoveryCheckpointInputPath =
                seededRecoveryJob.value(QStringLiteral("nativeCheckpointInputModelRef")).toString();
        QVERIFY(firstRecoveryInputSnapshotPath != firstRecoveryCheckpointInputPath);
        QCOMPARE(QFileInfo(firstRecoveryInputSnapshotPath).fileName(), QStringLiteral("recovery-solver-input.rbm"));
        QCOMPARE(QFileInfo(firstRecoveryCheckpointInputPath).fileName(), QStringLiteral("native-checkpoint-input.rbm"));
        const QJsonObject seedProof = seededRecoveryJob.value(QStringLiteral("nativeCheckpointSeed")).toObject();
        QCOMPARE(seedProof.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.native-checkpoint-seed/v1"));
        QCOMPARE(seedProof.value(QStringLiteral("sourceJobId")).toString(), interruptedJobId);
        QCOMPARE(seedProof.value(QStringLiteral("sourceEnvelopeSha256")).toString(),
                 selectedCheckpoint.value(QStringLiteral("envelopeSha256")).toString());
        QCOMPARE(seedProof.value(QStringLiteral("sourceCheckpointModelSha256")).toString(),
                 selectedCheckpoint.value(QStringLiteral("checkpointSha256")).toString());
        QCOMPARE(seedProof.value(QStringLiteral("inputModelSha256")).toString(),
                 seededRecoveryJob.value(QStringLiteral("nativeCheckpointContext")).toObject()
                     .value(QStringLiteral("modelSha256")).toString());
        QCOMPARE(fileSha256(seedProof.value(QStringLiteral("seedModelRef")).toString()),
                 seedProof.value(QStringLiteral("seedModelSha256")).toString());
        QCOMPARE(fileSha256(seedProof.value(QStringLiteral("seedEnvelopeRef")).toString()),
                 seedProof.value(QStringLiteral("seedEnvelopeSha256")).toString());
        const bool recoveryPauseRequested = recovered.requestNativeAcceptedBoundaryPause(
            recoveryJobId, secondInterruptionSequence, &error);
        if (!recoveryPauseRequested)
        {
            QString cancelError;
            recovered.cancelJob(recoveryJobId, &cancelError);
            recoveryGate.releaseNow();
            QVERIFY2(recoveryPauseRequested, qPrintable(error));
        }
        const QJsonObject recoveryPauseRequest = recovered.jobQuery(recoveryJobId);
        QCOMPARE(recoveryPauseRequest.value(QStringLiteral("state")).toString(), QStringLiteral("queued"));
        QCOMPARE(recoveryPauseRequest.value(QStringLiteral("inputSnapshotSha256")).toString(),
                 firstRecoveryInputSnapshotSha);
        QCOMPARE(recoveryPauseRequest.value(QStringLiteral("inputPackageSha256")).toString(),
                 firstRecoveryInputPackageSha);
        QCOMPARE(fileSha256(firstRecoveryInputSnapshotPath), firstRecoveryInputSnapshotSha);
        QCOMPARE(directorySha256(firstRecoveryInputPackagePath), firstRecoveryInputPackageSha);
        QCOMPARE(recoveryPauseRequest.value(QStringLiteral("nativeExecutionPauseRequestedSeq")).toInt(),
                 int(secondInterruptionSequence));
        const QString recoveryExecutionStatePath =
                recoveryPauseRequest.value(QStringLiteral("nativeExecutionStateRef")).toString();
        QVERIFY(!recoveryExecutionStatePath.isEmpty());
        recoveryGate.releaseNow();
        QTRY_COMPARE_WITH_TIMEOUT(nativeExecutionStateObject(recoveryExecutionStatePath)
                                      .value(QStringLiteral("state")).toString(),
                                  QStringLiteral("paused"), 30000);
        const QJsonObject recoveryPauseAck = nativeExecutionStateObject(recoveryExecutionStatePath);
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.native-execution-state/v1"));
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("generation")).toInt(), 1);
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("jobId")).toString(), recoveryJobId);
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("taskId")).toString(), recoveryJobId);
        QTRY_VERIFY_WITH_TIMEOUT(recovered.jobQuery(recoveryJobId)
                                     .value(QStringLiteral("processStarted")).toBool(), 10000);
        const QJsonObject recoveryLiveRecord = recovered.jobQuery(recoveryJobId);
        QCOMPARE(recoveryLiveRecord.value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        QVERIFY(!recoveryLiveRecord.value(QStringLiteral("processExited")).toBool());
        bool recoveryPidOk = false;
        QVERIFY(recoveryLiveRecord.value(QStringLiteral("nativeProcessId")).toString()
                    .toLongLong(&recoveryPidOk) > 0);
        QVERIFY(recoveryPidOk);
        const QJsonObject recoveryDurableLiveRecord = persistedNativeJobRecord(recoveryLiveRecord);
        QVERIFY(!recoveryDurableLiveRecord.isEmpty());
        QVERIFY(recoveryDurableLiveRecord.value(QStringLiteral("processStarted")).toBool());
        QVERIFY(!recoveryDurableLiveRecord.value(QStringLiteral("processExited")).toBool());
        QCOMPARE(recoveryDurableLiveRecord.value(QStringLiteral("nativeProcessId")).toString(),
                 recoveryLiveRecord.value(QStringLiteral("nativeProcessId")).toString());
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("pauseAtAcceptedSeq")).toInt(),
                 int(secondInterruptionSequence));
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("acceptedSeq")).toInt(),
                 int(secondInterruptionSequence));
        QVERIFY(recoveryPauseAck.value(QStringLiteral("checkpointAvailable")).toBool());
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("checkpointAcceptedSeq")).toInt(),
                 int(secondInterruptionSequence));
        QJsonObject recoveryCheckpointPointer;
        const QJsonObject recoveryCheckpointEnvelope =
                latestNativeCheckpointEnvelope(recoveryPauseRequest, &recoveryCheckpointPointer);
        QVERIFY(!recoveryCheckpointEnvelope.isEmpty());
        QCOMPARE(recoveryCheckpointEnvelope.value(QStringLiteral("acceptedSeq")).toInt(),
                 int(secondInterruptionSequence));
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("checkpointEnvelopeSha256")).toString(),
                 recoveryCheckpointPointer.value(QStringLiteral("envelopeSha256")).toString());
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("checkpointModelSha256")).toString(),
                 recoveryCheckpointEnvelope.value(QStringLiteral("checkpointModelSha256")).toString());
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("checkpointTimeS")).toDouble(),
                 recoveryCheckpointEnvelope.value(QStringLiteral("timeS")).toDouble());
        const QJsonObject recoveryPauseContext =
                recoveryPauseRequest.value(QStringLiteral("nativeCheckpointContext")).toObject();
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("contextSha256")).toString(),
                 jsonObjectSha256(recoveryPauseContext));
        for (const QString &hashField : {QStringLiteral("sourceSha256"), QStringLiteral("modelSha256"),
                                         QStringLiteral("controlSha256"), QStringLiteral("runtimeSha256"),
                                         QStringLiteral("packageSha256")})
            QCOMPARE(recoveryPauseAck.value(hashField).toString(), recoveryPauseContext.value(hashField).toString());
        const double observedSecondPartialSequence =
                recoveryPauseAck.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
        QVERIFY2(terminateVerifiedOwnRecoverySolver(
                     Application::instance()->getApplicationSettings()->getSolverPath(), &error),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(recovered.jobQuery(recoveryJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        QVERIFY(waitForManagerIdle(recovered));
        double latestRecoverySequence = -1.0;
        QVERIFY(latestAcceptedSequence(recovered.jobQuery(recoveryJobId), &latestRecoverySequence));
        QCOMPARE(latestRecoverySequence, double(secondInterruptionSequence));
        QVERIFY(persistAsInterruptedForTest(recovered, recoveryJobId, nullptr));

        SolverManager recoveredAgain;
        const QJsonObject secondQualification = recoveredAgain.nativeJobRecoveryQuery(recoveryJobId);
        QVERIFY2(secondQualification.value(QStringLiteral("eligibility")).toString() == QStringLiteral("recoverable"),
                 qPrintable(recoveryQualificationSummary(secondQualification)));
        const QJsonObject secondCheckpoint = secondQualification.value(QStringLiteral("checkpoint")).toObject();
        const QJsonObject secondSeedProof = secondQualification.value(QStringLiteral("nativeCheckpointSeed")).toObject();
        // A pause between periodic checkpoints durably commits its accepted
        // boundary. The immutable seed remains at 10; recovery now selects 15.
        QCOMPARE(secondCheckpoint.value(QStringLiteral("acceptedSeq")).toInt(),
                 int(secondInterruptionSequence));
        QCOMPARE(secondCheckpoint.value(QStringLiteral("targetSteps")).toInt(), int(targetSteps));
        QCOMPARE(secondCheckpoint.value(QStringLiteral("remainingSteps")).toInt(),
                 int(targetSteps - secondInterruptionSequence));
        QCOMPARE(secondSeedProof, seedProof);
        QCOMPARE(secondSeedProof.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
        QCOMPARE(secondCheckpoint.value(QStringLiteral("checkpointSha256")).toString(),
                 recoveryCheckpointEnvelope.value(QStringLiteral("checkpointModelSha256")).toString());
        QCOMPARE(secondCheckpoint.value(QStringLiteral("envelopeSha256")).toString(),
                 recoveryCheckpointPointer.value(QStringLiteral("envelopeSha256")).toString());
        QVERIFY(secondCheckpoint.value(QStringLiteral("checkpointSha256")).toString() !=
                secondSeedProof.value(QStringLiteral("seedModelSha256")).toString());
        const QJsonObject secondPartialResult = secondQualification.value(QStringLiteral("partialResult")).toObject();
        const int secondPartialAcceptedSequence = secondPartialResult.value(QStringLiteral("acceptedSeq")).toInt(-1);
        QCOMPARE(secondPartialAcceptedSequence, int(secondInterruptionSequence));
        QVERIFY(secondPartialAcceptedSequence < secondCheckpoint.value(QStringLiteral("targetSteps")).toInt());
        QVERIFY(secondPartialAcceptedSequence >= int(observedSecondPartialSequence));
        QCOMPARE(secondPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt(),
                 secondPartialAcceptedSequence - secondCheckpoint.value(QStringLiteral("acceptedSeq")).toInt());
        QCOMPARE(secondPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt(), 0);
        QVERIFY(secondPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt() <= 10);
        QVERIFY(secondPartialResult.value(QStringLiteral("discardedSimulationSeconds")).isDouble());
        const double secondDiscardedSimulationSeconds =
                secondPartialResult.value(QStringLiteral("discardedSimulationSeconds")).toDouble();
        QVERIFY(secondDiscardedSimulationSeconds >= 0.0 && secondDiscardedSimulationSeconds <= 1.0);
        QCOMPARE(secondDiscardedSimulationSeconds, 0.0);
        QCOMPARE(secondQualification.value(QStringLiteral("checkpoint")).toObject()
                     .value(QStringLiteral("checkpointSha256")).toString(),
                 recoveryCheckpointEnvelope.value(QStringLiteral("checkpointModelSha256")).toString());

        const QString completedRecoveryJobId = recoveredAgain.submitNativeRecovery(
            recoveryJobId, secondCheckpoint.value(QStringLiteral("checkpointSha256")).toString(),
            targetSteps - secondInterruptionSequence, &error);
        QVERIFY2(!completedRecoveryJobId.isEmpty(), qPrintable(error));
        QCOMPARE(fileSha256(firstRecoveryInputSnapshotPath), firstRecoveryInputSnapshotSha);
        QCOMPARE(directorySha256(firstRecoveryInputPackagePath), firstRecoveryInputPackageSha);
        QTRY_COMPARE_WITH_TIMEOUT(recoveredAgain.jobQuery(completedRecoveryJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);

        const QJsonObject resumedJob = recoveredAgain.jobQuery(completedRecoveryJobId);
        QCOMPARE(resumedJob.value(QStringLiteral("logicalJobId")).toString(), interruptedJobId);
        QCOMPARE(resumedJob.value(QStringLiteral("recoverySourceJobId")).toString(), recoveryJobId);
        QCOMPARE(resumedJob.value(QStringLiteral("selectedEnvelopeSha256")).toString(),
                 secondCheckpoint.value(QStringLiteral("envelopeSha256")).toString());
        QCOMPARE(resumedJob.value(QStringLiteral("recovery")).toObject()
                     .value(QStringLiteral("sourcePartialResult")).toObject()
                     .value(QStringLiteral("acceptedSeq")).toInt(), secondPartialAcceptedSequence);
        QVERIFY(resumedJob.value(QStringLiteral("resultArtifactRef")).toString() != originalResultPath);
        QCOMPARE(manager.jobQuery(interruptedJobId).value(QStringLiteral("state")).toString(),
                 QStringLiteral("failed"));
        QCOMPARE(session->modelCommandStamp(interruptedModelId).first, interruptedSourceStamp.first);
        QCOMPARE(session->modelCommandStamp(interruptedModelId).second, interruptedSourceStamp.second);

        Model continuousResult;
        Model resumedResult;
        continuousResult.read(manager.jobQuery(continuousJobId).value(QStringLiteral("resultArtifactRef")).toString());
        resumedResult.read(resumedJob.value(QStringLiteral("resultArtifactRef")).toString());
        const RStructuralDynamicsRestartData &continuousState =
                continuousResult.getProblemSetup().getStructuralDynamicsRestartData();
        const RStructuralDynamicsRestartData &resumedState =
                resumedResult.getProblemSetup().getStructuralDynamicsRestartData();
        QVERIFY(continuousState.available);
        QVERIFY(resumedState.available);
        QCOMPARE(resumedState.completedSteps, targetSteps);
        QVERIFY(nearlyEqualVectors(continuousState.q, resumedState.q));
        QVERIFY(nearlyEqualVectors(continuousState.v, resumedState.v));
        QVERIFY(nearlyEqualVectors(continuousState.a, resumedState.a));
        QVERIFY(nearlyEqualVectors(continuousState.externalForce, resumedState.externalForce));
        QVERIFY(std::abs(continuousState.cumulativeExternalWork - resumedState.cumulativeExternalWork) < 1.0e-11);
        QVERIFY(std::abs(continuousState.cumulativeDampingDissipation - resumedState.cumulativeDampingDissipation) < 1.0e-11);
        QVERIFY(std::abs(continuousState.initialMechanicalEnergy - resumedState.initialMechanicalEnergy) < 1.0e-11);

        // Query the verified public catalog so compact originRef entries are
        // expanded before checking the inherited explicit null origins.
        const QJsonObject manifest = recoveredAgain.nativeResultQuery(completedRecoveryJobId,
            QStringLiteral("catalog"), {{QStringLiteral("limit"), 200}});
        QVERIFY2(manifest.value(QStringLiteral("valid")).toBool(),
                 qPrintable(manifest.value(QStringLiteral("message")).toString()));
        QCOMPARE(manifest.value(QStringLiteral("jobId")).toString(), completedRecoveryJobId);
        QCOMPARE(manifest.value(QStringLiteral("frameCount")).toInt(), int(targetSteps));
        QVERIFY(!manifest.contains(QStringLiteral("nextCursor")));
        const QJsonArray frames = manifest.value(QStringLiteral("frames")).toArray();
        QSet<uint> recordNumbers;
        for (const QJsonValue &frameValue : frames)
        {
            const double recordNumber = frameValue.toObject().value(QStringLiteral("recordNo")).toDouble(-1.0);
            QVERIFY(recordNumber >= 1.0 && std::floor(recordNumber) == recordNumber);
            QVERIFY(!recordNumbers.contains(uint(recordNumber)));
            recordNumbers.insert(uint(recordNumber));
        }
        QCOMPARE(recordNumbers.size(), int(targetSteps));
        const QJsonArray reusedHistory = manifest.value(QStringLiteral("reusedHistory")).toArray();
        QCOMPARE(reusedHistory.size(), int(secondInterruptionSequence));
        QSet<int> originalPrefixRecords;
        QSet<int> recoveryPrefixRecords;
        for (const QJsonValue &historyValue : reusedHistory)
        {
            const QJsonObject history = historyValue.toObject();
            QCOMPARE(history.value(QStringLiteral("originKind")).toString(), QStringLiteral("legacy-history-unattributed"));
            QVERIFY(history.value(QStringLiteral("origin")).isNull());
            const QJsonObject origin = history.value(QStringLiteral("recoveryPrefixOrigin")).toObject();
            const int recordNo = history.value(QStringLiteral("recordNo")).toInt();
            QVERIFY(recordNo > 0 && recordNo <= int(secondInterruptionSequence));
            QCOMPARE(origin.value(QStringLiteral("sourceRecordNo")).toInt(), recordNo);
            if (recordNo <= int(checkpointSequence))
            {
                QVERIFY(!originalPrefixRecords.contains(recordNo));
                originalPrefixRecords.insert(recordNo);
                QCOMPARE(origin.value(QStringLiteral("sourceJobId")).toString(), interruptedJobId);
                QCOMPARE(origin.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
                QCOMPARE(origin.value(QStringLiteral("selectedEnvelopeSha256")).toString(),
                         selectedCheckpoint.value(QStringLiteral("envelopeSha256")).toString());
                const QJsonObject inheritance = history.value(QStringLiteral("recoveryInheritance")).toObject();
                QCOMPARE(inheritance.value(QStringLiteral("sourceJobId")).toString(), recoveryJobId);
                QCOMPARE(inheritance.value(QStringLiteral("acceptedSeq")).toInt(), int(secondInterruptionSequence));
                QCOMPARE(inheritance.value(QStringLiteral("selectedEnvelopeSha256")).toString(),
                         secondCheckpoint.value(QStringLiteral("envelopeSha256")).toString());
            }
            else
            {
                QVERIFY(!recoveryPrefixRecords.contains(recordNo));
                recoveryPrefixRecords.insert(recordNo);
                QCOMPARE(origin.value(QStringLiteral("sourceJobId")).toString(), recoveryJobId);
                QCOMPARE(origin.value(QStringLiteral("acceptedSeq")).toInt(), int(secondInterruptionSequence));
                QCOMPARE(origin.value(QStringLiteral("selectedEnvelopeSha256")).toString(),
                         secondCheckpoint.value(QStringLiteral("envelopeSha256")).toString());
                QVERIFY(!history.contains(QStringLiteral("recoveryInheritance")));
            }
        }
        QCOMPARE(originalPrefixRecords.size(), int(checkpointSequence));
        QCOMPARE(recoveryPrefixRecords.size(), int(secondInterruptionSequence - checkpointSequence));
    }

    void persistentJobRecordReopensAndExternalReplacementIsIsolated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QVERIFY(waitForManagerIdle(manager));

        SessionModelGuard models(session);
        Model model;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("durable-registry"), model));
        configureSingleOutputDynamics(model);
        const uint modelId = models.add(model);
        QString error;
        const QString jobId = manager.submitValidated(modelId, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);

        const QJsonObject original = manager.jobQuery(jobId);
        QVERIFY(!original.value(QStringLiteral("cancellationRequested")).toBool());
        QVERIFY(!original.contains(QStringLiteral("cancelRequestedAt")));
        QVERIFY(original.value(QStringLiteral("resultManifestVerified")).toBool());
        QString recordPath = original.value(QStringLiteral("nativeJobRecordRef")).toString();
        QVERIFY(!recordPath.isEmpty());
        QVERIFY(QFileInfo(recordPath).isFile());

        SolverManager reopened;
        const QJsonObject loaded = reopened.jobQuery(jobId);
        QVERIFY(loaded.value(QStringLiteral("found")).toBool());
        QCOMPARE(loaded.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(loaded.value(QStringLiteral("projectId")).toString(), QStringLiteral("unlinked"));
        QCOMPARE(loaded.value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
        QVERIFY(loaded.value(QStringLiteral("resultManifestVerified")).toBool());
        QCOMPARE(reopened.nativeJobRecoveryQuery(jobId).value(QStringLiteral("eligibility")).toString(),
                 QStringLiteral("recompute_required"));

        QFile durableRecord(recordPath);
        QVERIFY(durableRecord.open(QIODevice::ReadOnly));
        const QByteArray durableBytesBeforeFailedCommit = durableRecord.readAll();
        durableRecord.close();
        QVERIFY(!durableBytesBeforeFailedCommit.isEmpty());
        const QJsonObject managerRecordBeforeFailedCommit = manager.nativeJobs.value(jobId);
        const QJsonObject managerDiagnosticBeforeFailedCommit = manager.nativeJobDiagnostics.value(jobId);
        QLockFile writeFailureLock(QDir(QFileInfo(recordPath).absolutePath())
                                       .filePath(QStringLiteral(".job-record.lock")));
        writeFailureLock.setStaleLockTime(0);
        QVERIFY(writeFailureLock.tryLock(0));
        QSignalSpy nativeJobUpdatedSpy(&manager, &SolverManager::nativeJobUpdated);
        QVERIFY(!manager.updateNativeJob(jobId,
            {{QStringLiteral("trustedFailureDiagnosis"),
              QJsonObject{{QStringLiteral("schemaVersion"), 1},
                          {QStringLiteral("status"), QStringLiteral("complete")},
                          {QStringLiteral("packageSha256"), QStringLiteral("fixture-digest")}}}}));
        QCOMPARE(nativeJobUpdatedSpy.count(), 0);
        QCOMPARE(manager.nativeJobDiagnostics.value(jobId).value(QStringLiteral("code")).toString(),
                 QStringLiteral("NATIVE_JOB_PERSISTENCE_FAILED"));
        QFile durableRecordAfterFailure(recordPath);
        QVERIFY(durableRecordAfterFailure.open(QIODevice::ReadOnly));
        QCOMPARE(durableRecordAfterFailure.readAll(), durableBytesBeforeFailedCommit);
        durableRecordAfterFailure.close();
        SolverManager reopenedAfterFailedCommit;
        const QJsonObject afterFailedCommit = reopenedAfterFailedCommit.jobQuery(jobId);
        QVERIFY(afterFailedCommit.value(QStringLiteral("found")).toBool());
        QCOMPARE(afterFailedCommit.value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
        QVERIFY(!afterFailedCommit.contains(QStringLiteral("trustedFailureDiagnosis")));
        writeFailureLock.unlock();
        manager.nativeJobs.insert(jobId, managerRecordBeforeFailedCommit);
        if (managerDiagnosticBeforeFailedCommit.isEmpty()) manager.nativeJobDiagnostics.remove(jobId);
        else manager.nativeJobDiagnostics.insert(jobId, managerDiagnosticBeforeFailedCommit);

        QVERIFY(loaded.value(QStringLiteral("nativeCheckpointEnabled")).toBool());
        const QJsonObject checkpointContext = loaded.value(QStringLiteral("nativeCheckpointContext")).toObject();
        QCOMPARE(checkpointContext.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.native-checkpoint-context/v1"));
        QCOMPARE(checkpointContext.value(QStringLiteral("policy")).toString(),
                 QStringLiteral("accepted-steps-or-sim-time-v1"));
        const QString checkpointDirectory = loaded.value(QStringLiteral("nativeCheckpointDirectory")).toString();
        QVERIFY(QFileInfo(QDir(checkpointDirectory).filePath(QStringLiteral("latest.json"))).isFile());
        QVERIFY(QFileInfo(QDir(checkpointDirectory).filePath(QStringLiteral("lastvalid.json"))).isFile());

        const QString resultDirectory = QFileInfo(loaded.value(QStringLiteral("resultArtifactRef")).toString()).absolutePath();
        QVERIFY(QDir(directory.path()).relativeFilePath(resultDirectory).startsWith(QStringLiteral(".range-runs")));
        QVERIFY(QDir(resultDirectory).removeRecursively());
        QFile storedRecord(recordPath);
        QVERIFY(storedRecord.open(QIODevice::ReadOnly));
        QJsonObject simulated = QJsonDocument::fromJson(storedRecord.readAll()).object();
        storedRecord.close();
        simulated.insert(QStringLiteral("state"), QStringLiteral("running"));
        simulated.insert(QStringLiteral("resultArtifactVerified"), false);
        simulated.insert(QStringLiteral("resultSourceBindingVerified"), false);
        simulated.insert(QStringLiteral("resultReadbackComplete"), false);
        simulated.insert(QStringLiteral("resultManifestDescriptorReported"), false);
        simulated.insert(QStringLiteral("resultManifestVerified"), false);
        simulated.insert(QStringLiteral("nativeResultCatalogAvailable"), false);
        simulated.insert(QStringLiteral("resultSourceBinding"), QJsonObject{});
        simulated.insert(QStringLiteral("resultArtifactSha256"), QJsonValue(QJsonValue::Null));
        simulated.insert(QStringLiteral("resultManifestRef"), QJsonValue(QJsonValue::Null));
        simulated.insert(QStringLiteral("resultManifestSha256"), QJsonValue(QJsonValue::Null));
        simulated.insert(QStringLiteral("resultArtifactId"), QJsonValue(QJsonValue::Null));
        QVERIFY(publishTestNativeJobRecord(&recordPath, &simulated));

        SolverManager liveOwnerProbe;
        const QJsonObject liveOwnerQualification = liveOwnerProbe.nativeJobRecoveryQuery(jobId);
        QCOMPARE(liveOwnerQualification.value(QStringLiteral("eligibility")).toString(),
                 QStringLiteral("reconcile_required"));
        QFile liveRecordFile(recordPath);
        QVERIFY(liveRecordFile.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(liveRecordFile.readAll()).object()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("running"));
        liveRecordFile.close();

        QJsonObject interrupted = simulated;
        interrupted.insert(QStringLiteral("state"), QStringLiteral("interrupted"));
        interrupted.insert(QStringLiteral("interruptedAt"), QStringLiteral("2026-10-09T00:00:00.000Z"));
        const QString simulatedOwner = QUuid::createUuid().toString(QUuid::WithoutBraces);
        interrupted.insert(QStringLiteral("ownerInstanceId"), simulatedOwner);
        interrupted.insert(QStringLiteral("ownerPid"), 2147483647);
        interrupted.insert(QStringLiteral("ownerLockRef"),
                           QDir(QFileInfo(loaded.value(QStringLiteral("ownerLockRef")).toString()).absolutePath())
                               .filePath(simulatedOwner + QStringLiteral(".lock")));
        QVERIFY(publishTestNativeJobRecord(&recordPath, &interrupted));

        SolverManager recovered;
        const QJsonObject qualification = recovered.nativeJobRecoveryQuery(jobId);
        QCOMPARE(qualification.value(QStringLiteral("eligibility")).toString(), QStringLiteral("reconcile_required"));
        QCOMPARE(qualification.value(QStringLiteral("state")).toString(), QStringLiteral("interrupted"));
        QCOMPARE(qualification.value(QStringLiteral("checkpoint")).toObject()
                     .value(QStringLiteral("targetSteps")).toInt(), 400);
        QCOMPARE(qualification.value(QStringLiteral("checkpoint")).toObject()
                     .value(QStringLiteral("remainingSteps")).toInt(), 0);
        QCOMPARE(qualification.value(QStringLiteral("checkpoint")).toObject()
                     .value(QStringLiteral("checkpointSha256")).toString(),
                 fileSha256(qualification.value(QStringLiteral("checkpoint")).toObject()
                                .value(QStringLiteral("checkpointPath")).toString()));
        QString recoveryError;
        QVERIFY(recovered.submitNativeRecovery(jobId,
                    qualification.value(QStringLiteral("checkpoint")).toObject()
                        .value(QStringLiteral("checkpointSha256")).toString(),
                    0, &recoveryError).isEmpty());
        QVERIFY(recoveryError.contains(QStringLiteral("reconcile_required")));

        const QString latestCheckpointPointer = QDir(checkpointDirectory).filePath(QStringLiteral("latest.json"));
        QFile pointerFile(latestCheckpointPointer);
        QVERIFY(pointerFile.open(QIODevice::ReadOnly));
        const QByteArray latestPointerBytes = pointerFile.readAll();
        pointerFile.close();
        QVERIFY(!latestPointerBytes.isEmpty());
        QVERIFY(pointerFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(pointerFile.write("{corrupt latest pointer"), qint64(23));
        QVERIFY(pointerFile.flush());
        pointerFile.close();
        const QJsonObject badLatest = recovered.nativeJobRecoveryQuery(jobId);
        QCOMPARE(badLatest.value(QStringLiteral("eligibility")).toString(), QStringLiteral("conflict"));
        QVERIFY(badLatest.value(QStringLiteral("reason")).toString().contains(QStringLiteral("current native checkpoint pointer")));
        QVERIFY(pointerFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(pointerFile.write(latestPointerBytes), qint64(latestPointerBytes.size()));
        QVERIFY(pointerFile.flush());
        pointerFile.close();

        QFile recordFile(recordPath);
        QVERIFY(recordFile.open(QIODevice::ReadWrite));
        QVERIFY(recordFile.resize(0));
        const QByteArray corruptBytes("{corrupt native job record");
        QCOMPARE(recordFile.write(corruptBytes), qint64(corruptBytes.size()));
        QVERIFY(recordFile.flush());
        recordFile.close();

        const QJsonObject conflict = recovered.nativeJobRecoveryQuery(jobId);
        QCOMPARE(conflict.value(QStringLiteral("eligibility")).toString(), QStringLiteral("conflict"));
        QFile preserved(recordPath);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), corruptBytes);
        preserved.close();
        QVERIFY(QDir(QFileInfo(recordPath).absolutePath()).removeRecursively());
    }
};

int runNativeJobRegistryTests(int argc, char **argv)
{
    TestNativeJobRegistry test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_native_job_registry.moc"
