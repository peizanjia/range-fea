#include <QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSemaphore>
#include <QTemporaryDir>
#include <memory>

#include <algorithm>

#include <rbl_job.h>

#include "application.h"
#include "model_io.h"
#include "model_command_host.h"
#include "session.h"
#include "solver_manager.h"

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
    if (model.getNLines() == 0) return false;
    for (uint index = 0; index < model.getNPoints(); ++index)
        model.getPoint(index).setMaterial(model.getLine(0).getMaterial());
    ModelIO::save(model, target, false);
    if (sourceFileName) *sourceFileName = target;
    return model.getNNodes() > 0 && model.getNElements() > 0;
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
        const QString resultPathA = finishedA.value(QStringLiteral("resultArtifactRef")).toString();
        QCOMPARE(finishedA.value(QStringLiteral("resultArtifactSha256")).toString(), fileSha256(resultPathA));
    }
};

int runNativeJobRegistryTests(int argc, char **argv)
{
    TestNativeJobRegistry test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_native_job_registry.moc"
