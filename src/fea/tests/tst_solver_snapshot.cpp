#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>
#include <algorithm>

#include <rbl_job.h>

#include "application.h"
#include "model_command_host.h"
#include "model_io.h"
#include "session.h"
#include "solver_manager.h"
#include "solver_task.h"

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

QByteArray fileSha256(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

QByteArray directorySha256(const QString &directoryName)
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
            if (chunk.isEmpty() && file.error() != QFile::NoError) return {};
            hash.addData(chunk);
        }
        hash.addData("\0", 1);
    }
    return hash.result();
}

bool waitForGate(const std::shared_ptr<QSemaphore> &started)
{
    return started->tryAcquire(1, 10000);
}

bool loadStage9Model(QTemporaryDir &directory, Model &model, QString &sourceFileName)
{
#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_WORKFLOW_SOURCE_ROOT "."
#endif
    const QString sourceExample = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT))
            .filePath(QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram"));
    sourceFileName = directory.filePath(QStringLiteral("stage9_truss4.ram"));
    if (!QFile::copy(sourceExample, sourceFileName)) return false;
    try
    {
        ModelIO::open(model, sourceFileName);
    }
    catch (const RError &)
    {
        return false;
    }
    // stage9_truss4.ram is the command-line controller benchmark. The generic
    // GUI SolverTask does not select an external controller executable, so use
    // its native dynamics model without that CLI-only controller requirement.
    model.getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    return model.getNNodes() > 0 && model.getNElements() > 0;
}

uint addStage9Model(Session &session, QTemporaryDir &directory, QString &sourceFileName)
{
    Model model;
    if (!loadStage9Model(directory, model, sourceFileName)) return RConstants::eod;
    session.addModel(model);
    return session.getNModels() - 1;
}

void removeTestModel(Session &session, uint modelID)
{
    if (modelID < session.getNModels()) session.removeModel(modelID);
}
}

class TestSolverSnapshot : public QObject
{
    Q_OBJECT

private slots:
    void coldSolverPublicationCreatesAuditRecord()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Session *session = Application::instance()->getSession();
        QString sourceFileName;
        const uint modelID = addStage9Model(*session, directory, sourceFileName);
        QVERIFY(modelID != RConstants::eod);
        const QPair<QString, quint64> sourceStamp = session->modelCommandStamp(modelID);
        QVERIFY(!sourceStamp.first.isEmpty());

        // Do not query ModelCommandHost before accepting. The publication path
        // must establish a pre-result journal snapshot for a cold document.
        Model result(session->getModel(modelID));
        result.setDescription(QStringLiteral("solver result accepted"));
        const QString resultFileName = directory.filePath(QStringLiteral("accepted-result.ram"));
        result.setFileName(resultFileName);
        ModelIO::save(result, resultFileName, false);
        QVERIFY(QFileInfo::exists(resultFileName));

        QSignalSpy events(session, &Session::solverResultAvailable);
        QString error;
        QVERIFY2(session->acceptSolverResult(sourceStamp, result, resultFileName, &error), qPrintable(error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(session->getModel(modelID).getDescription(), QStringLiteral("solver result accepted"));
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.last().at(3).toBool(), true);

        const QString sourcePath = QFileInfo(sourceFileName).absoluteFilePath();
        QFile journal(session->getModelCommandHost()->journalPath());
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const QJsonDocument journalDocument = QJsonDocument::fromJson(journal.readAll());
        QVERIFY(journalDocument.isObject());
        const QJsonArray records = journalDocument.object().value(QStringLiteral("records")).toArray();
        bool foundAcceptedResult = false;
        for (const QJsonValue &value : records)
        {
            const QJsonObject record = value.toObject();
            const QJsonObject selection = record.value(QStringLiteral("selectionSnapshot")).toObject();
            if (record.value(QStringLiteral("commandId")).toString() ==
                    QStringLiteral("model.external.external_edit") &&
                QFileInfo(selection.value(QStringLiteral("sourcePath")).toString()).absoluteFilePath() == sourcePath)
            {
                foundAcceptedResult = true;
                QVERIFY(!record.value(QStringLiteral("modelId")).toString().isEmpty());
                QCOMPARE(selection.value(QStringLiteral("reason")).toString(), QStringLiteral("external_edit"));
            }
        }
        QVERIFY(foundAcceptedResult);
        removeTestModel(*session, modelID);
    }

    void submittedRunUsesQueuedSnapshotAndDetachesStaleResult()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(RJobManager::getInstance().getNRunning() == 0 &&
                                 RJobManager::getInstance().getNWaiting() == 0, 15000);

        QString sourceFileName;
        const uint modelID = addStage9Model(*session, directory, sourceFileName);
        QVERIFY(modelID != RConstants::eod);
        const QByteArray sourceHashBefore = fileSha256(sourceFileName);
        QVERIFY(!sourceHashBefore.isEmpty());

        const QString originalName = session->getModel(modelID).getName();
        const QString originalDescription = session->getModel(modelID).getDescription();
        const QPair<QString, quint64> sourceStampBefore = session->modelCommandStamp(modelID);
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QVERIFY(!task->getSourceDocumentId().isEmpty());
        QVERIFY(QFileInfo::exists(Application::instance()->getApplicationSettings()->getSolverPath()));

        auto gateStarted = std::make_shared<QSemaphore>();
        auto gateRelease = std::make_shared<QSemaphore>();
        GateReleaseGuard gateGuard{gateRelease};
        manager.RJobManager::submit(new GateJob(gateStarted, gateRelease));
        QVERIFY(waitForGate(gateStarted));

        QSignalSpy taskFinished(task, &RJob::finished);
        QSignalSpy taskFailed(task, &RJob::failed);
        QSignalSpy blockingEvents(&manager, &RJobManager::jobBlocking);
        QSignalSpy resultEvents(session, &Session::solverResultAvailable);
        const qsizetype priorResultEvents = resultEvents.size();

        manager.submit(task);
        const QPair<QString, quint64> capturedStamp = task->getSourceStamp();
        const QString inputFileName = task->getInputSnapshotFileName();
        const QString resultFileName = task->getResultArtifactFileName();
        const QByteArray inputHash = task->getInputSnapshotSha256();
        const QByteArray inputPackageHash = task->getInputPackageSha256();
        QVERIFY(capturedStamp.first == sourceStampBefore.first);
        QCOMPARE(capturedStamp.second, sourceStampBefore.second);
        QVERIFY(!inputHash.isEmpty());
        QVERIFY(!inputPackageHash.isEmpty());
        QVERIFY(QFileInfo::exists(inputFileName));
        QVERIFY(resultFileName != inputFileName);
        QVERIFY(QFileInfo(inputFileName).absoluteFilePath() != QFileInfo(sourceFileName).absoluteFilePath());
        QCOMPARE(fileSha256(inputFileName), inputHash);
        QCOMPARE(directorySha256(QFileInfo(inputFileName).absolutePath()), inputPackageHash);

        Model serializedInput;
        ModelIO::open(serializedInput, inputFileName);
        QCOMPARE(serializedInput.getName(), originalName);
        QCOMPARE(serializedInput.getDescription(), originalDescription);

        // The gate keeps the solver queued until the live document is edited.
        // The worker must solve the already serialized snapshot and must not
        // write back over this newer document generation.
        session->storeCurentModelVersion(modelID, QStringLiteral("Edit while solver waits"));
        session->getModel(modelID).setDescription(QStringLiteral("edited while solver waited"));
        session->setProblemChanged(modelID);
        QCOMPARE(session->modelCommandStamp(modelID).first, capturedStamp.first);
        QCOMPARE(session->modelCommandStamp(modelID).second, capturedStamp.second + 1);
        QCOMPARE(fileSha256(sourceFileName), sourceHashBefore);
        QCOMPARE(manager.getNWaiting(), 1u);

        gateRelease->release();
        gateGuard.release.reset();
        QTRY_VERIFY_WITH_TIMEOUT(taskFinished.size() + taskFailed.size() > 0, 120000);
        QCOMPARE(taskFailed.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(resultEvents.size(), priorResultEvents + 1, 15000);

        QCOMPARE(fileSha256(sourceFileName), sourceHashBefore);
        QCOMPARE(fileSha256(inputFileName), inputHash);
        QCOMPARE(directorySha256(QFileInfo(inputFileName).absolutePath()), inputPackageHash);
        QVERIFY(QFileInfo::exists(resultFileName));
        const QList<QVariant> resultEvent = resultEvents.last();
        QCOMPARE(resultEvent.at(0).toString(), capturedStamp.first);
        QCOMPARE(resultEvent.at(1).toULongLong(), capturedStamp.second);
        QCOMPARE(QFileInfo(resultEvent.at(2).toString()).absoluteFilePath(),
                 QFileInfo(resultFileName).absoluteFilePath());
        QCOMPARE(resultEvent.at(3).toBool(), false);
        QCOMPARE(session->getModel(modelID).getDescription(), QStringLiteral("edited while solver waited"));
        for (const QList<QVariant> &event : blockingEvents)
        {
            QVERIFY(!event.at(0).toBool());
        }

        Model completedResult;
        ModelIO::open(completedResult, resultFileName);
        QCOMPARE(completedResult.getName(), originalName);
        QCOMPARE(completedResult.getDescription(), originalDescription);

        removeTestModel(*session, modelID);
    }

    void queuedCancellationLeavesSnapshotAndSourceUntouched()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        Session *session = Application::instance()->getSession();
        SolverManager &manager = SolverManager::getInstance();
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(RJobManager::getInstance().getNRunning() == 0 &&
                                 RJobManager::getInstance().getNWaiting() == 0, 15000);

        QString sourceFileName;
        const uint modelID = addStage9Model(*session, directory, sourceFileName);
        QVERIFY(modelID != RConstants::eod);
        const QByteArray sourceHashBefore = fileSha256(sourceFileName);
        QVERIFY(!sourceHashBefore.isEmpty());

        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        QSignalSpy taskFinished(task, &RJob::finished);
        QSignalSpy taskFailed(task, &RJob::failed);
        auto gateStarted = std::make_shared<QSemaphore>();
        auto gateRelease = std::make_shared<QSemaphore>();
        GateReleaseGuard gateGuard{gateRelease};
        manager.RJobManager::submit(new GateJob(gateStarted, gateRelease));
        QVERIFY(waitForGate(gateStarted));

        manager.submit(task);
        const QString inputFileName = task->getInputSnapshotFileName();
        const QString resultFileName = task->getResultArtifactFileName();
        const QByteArray inputHash = task->getInputSnapshotSha256();
        const QByteArray inputPackageHash = task->getInputPackageSha256();
        QVERIFY(QFileInfo::exists(inputFileName));
        QVERIFY(!inputHash.isEmpty());
        QVERIFY(!inputPackageHash.isEmpty());
        QCOMPARE(fileSha256(inputFileName), inputHash);
        QCOMPARE(directorySha256(QFileInfo(inputFileName).absolutePath()), inputPackageHash);

        task->kill();
        // The running generic gate is harmless; manager cancellation must
        // tolerate it and the queued task must not touch QProcess cross-thread.
        manager.killRunningTasks();
        gateRelease->release();
        gateGuard.release.reset();
        QTRY_VERIFY_WITH_TIMEOUT(taskFinished.size() + taskFailed.size() > 0, 30000);
        QCOMPARE(taskFinished.size(), 0);
        QCOMPARE(taskFailed.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        QCOMPARE(fileSha256(sourceFileName), sourceHashBefore);
        QCOMPARE(fileSha256(inputFileName), inputHash);
        QCOMPARE(directorySha256(QFileInfo(inputFileName).absolutePath()), inputPackageHash);
        QVERIFY(!QFileInfo::exists(resultFileName));

        removeTestModel(*session, modelID);
    }
};

int runSolverSnapshotTests(int argc, char **argv)
{
    TestSolverSnapshot test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_solver_snapshot.moc"
