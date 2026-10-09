#include <QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRunnable>
#include <QSaveFile>
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

#include <rbl_job.h>

#include "application.h"
#include "model_io.h"
#include "model_command_host.h"
#include "session.h"
#include "solver_manager.h"

bool terminateVerifiedOwnRecoverySolver(const QString &solverPath, QString *error);

namespace
{
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
                 int(checkpointSequence));
        QJsonObject recoveryCheckpointPointer;
        const QJsonObject recoveryCheckpointEnvelope =
                latestNativeCheckpointEnvelope(recoveryPauseRequest, &recoveryCheckpointPointer);
        QVERIFY(!recoveryCheckpointEnvelope.isEmpty());
        QCOMPARE(recoveryCheckpointEnvelope.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("checkpointEnvelopeSha256")).toString(),
                 recoveryCheckpointPointer.value(QStringLiteral("envelopeSha256")).toString());
        QCOMPARE(recoveryPauseAck.value(QStringLiteral("checkpointModelSha256")).toString(),
                 recoveryCheckpointEnvelope.value(QStringLiteral("checkpointModelSha256")).toString());
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
        QCOMPARE(latestRecoverySequence, 10.0);
        QVERIFY(persistAsInterruptedForTest(recovered, recoveryJobId, nullptr));

        SolverManager recoveredAgain;
        const QJsonObject secondQualification = recoveredAgain.nativeJobRecoveryQuery(recoveryJobId);
        QVERIFY2(secondQualification.value(QStringLiteral("eligibility")).toString() == QStringLiteral("recoverable"),
                 qPrintable(recoveryQualificationSummary(secondQualification)));
        const QJsonObject secondCheckpoint = secondQualification.value(QStringLiteral("checkpoint")).toObject();
        const QJsonObject secondSeedProof = secondQualification.value(QStringLiteral("nativeCheckpointSeed")).toObject();
        QCOMPARE(secondCheckpoint.value(QStringLiteral("acceptedSeq")).toInt(), int(checkpointSequence));
        QCOMPARE(secondCheckpoint.value(QStringLiteral("targetSteps")).toInt(), int(targetSteps));
        QCOMPARE(secondCheckpoint.value(QStringLiteral("remainingSteps")).toInt(),
                 int(targetSteps - checkpointSequence));
        QCOMPARE(secondCheckpoint.value(QStringLiteral("checkpointSha256")).toString(),
                 secondSeedProof.value(QStringLiteral("seedModelSha256")).toString());
        QCOMPARE(secondCheckpoint.value(QStringLiteral("envelopeSha256")).toString(),
                 secondSeedProof.value(QStringLiteral("seedEnvelopeSha256")).toString());
        const QJsonObject secondPartialResult = secondQualification.value(QStringLiteral("partialResult")).toObject();
        const int secondPartialAcceptedSequence = secondPartialResult.value(QStringLiteral("acceptedSeq")).toInt(-1);
        QVERIFY(secondPartialAcceptedSequence >= int(secondInterruptionSequence));
        QVERIFY(secondPartialAcceptedSequence < secondCheckpoint.value(QStringLiteral("targetSteps")).toInt());
        QVERIFY(secondPartialAcceptedSequence >= int(observedSecondPartialSequence));
        QCOMPARE(secondPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt(),
                 secondPartialAcceptedSequence - secondCheckpoint.value(QStringLiteral("acceptedSeq")).toInt());
        QVERIFY(secondPartialResult.value(QStringLiteral("discardedAcceptedSteps")).toInt() <= 10);
        QVERIFY(secondPartialResult.value(QStringLiteral("discardedSimulationSeconds")).isDouble());
        const double secondDiscardedSimulationSeconds =
                secondPartialResult.value(QStringLiteral("discardedSimulationSeconds")).toDouble();
        QVERIFY(secondDiscardedSimulationSeconds >= 0.0 && secondDiscardedSimulationSeconds <= 1.0);
        QCOMPARE(secondQualification.value(QStringLiteral("checkpoint")).toObject()
                     .value(QStringLiteral("checkpointSha256")).toString(),
                 seedProof.value(QStringLiteral("seedModelSha256")).toString());

        const QString completedRecoveryJobId = recoveredAgain.submitNativeRecovery(
            recoveryJobId, secondCheckpoint.value(QStringLiteral("checkpointSha256")).toString(),
            targetSteps - checkpointSequence, &error);
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

        QFile manifestFile(resumedJob.value(QStringLiteral("resultManifestRef")).toString());
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
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
        QCOMPARE(reusedHistory.size(), 10);
        for (const QJsonValue &historyValue : reusedHistory)
        {
            const QJsonObject history = historyValue.toObject();
            QCOMPARE(history.value(QStringLiteral("originKind")).toString(), QStringLiteral("legacy-history-unattributed"));
            QVERIFY(history.value(QStringLiteral("origin")).isNull());
            const QJsonObject origin = history.value(QStringLiteral("recoveryPrefixOrigin")).toObject();
            QCOMPARE(origin.value(QStringLiteral("sourceJobId")).toString(), interruptedJobId);
            QCOMPARE(origin.value(QStringLiteral("acceptedSeq")).toInt(), 10);
            QCOMPARE(history.value(QStringLiteral("recoveryInheritance")).toObject()
                         .value(QStringLiteral("sourceJobId")).toString(), recoveryJobId);
            QVERIFY(history.value(QStringLiteral("recordNo")).toInt() <= 10);
        }
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
