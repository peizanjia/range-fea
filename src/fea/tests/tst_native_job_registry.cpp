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
#include <QSemaphore>
#include <QTemporaryDir>
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

void configureSingleOutputDynamics(Model &model)
{
    constexpr uint steps = 400;
    constexpr double timeStep = 0.001;
    RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
    dynamics.setTimeStep(timeStep);
    dynamics.setNSteps(steps);
    dynamics.setOutputFrequency(steps);

    RTimeSolver &time = model.getTimeSolver();
    time.setEnabled(true);
    time.setInputStartTime(0.0);
    time.setInputNTimeSteps(steps);
    time.setInputTimeStepSize(timeStep);
    time.setOutputFrequency(steps);
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
};

int runNativeJobRegistryTests(int argc, char **argv)
{
    TestNativeJobRegistry test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_native_job_registry.moc"
