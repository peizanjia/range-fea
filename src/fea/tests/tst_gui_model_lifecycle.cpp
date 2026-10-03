#include <QtTest>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QSemaphore>
#include <QSpinBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUuid>

#include <rmodel_command_session.h>
#include <rbl_utils.h>
#include "application.h"
#include "action.h"
#include "model_command_host.h"
#include "model_io.h"
#include "modal_setup_widget.h"
#include "problem_tree.h"
#include "solver_manager.h"

namespace
{
// Only startup dependencies are prepared here. The Application constructor,
// initialize(), MainWindow, Session, ProblemTree, actions and IO jobs are real.
class WindowApplication : public Application
{
public:
    WindowApplication(int &argc, char **argv) : Application(argc, argv)
    {
        disconnect(this, &RApplication::started, this, nullptr);
        cloudSessionManager = new RCloudSessionManager(this);
        cloudConnectionHandler = new RCloudConnectionHandler(cloudSessionManager, applicationSettings, this);
        aiAgentSettingsManager = new RAgentSettingsManager(this);
        initialize(false);
        setQuitOnLastWindowClosed(false);
    }
};

Model fixture(const QString &name)
{
    Model model;
    model.setName(name);
    model.setNNodes(2);
    model.setNode(0, RNode(0., 0., 0.));
    model.setNode(1, RNode(1., 0., 0.));
    model.setNElements(1);
    RElement element(R_ELEMENT_TRUSS1);
    element.setNodeId(0, 0);
    element.setNodeId(1, 1);
    model.setElement(0, element);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    auto &modal = model.getProblemSetup().getModalSetup();
    modal.setMethod(R_MODAL_MULTIPLE_MODES);
    modal.setNIterations(12);
    modal.setNModesToExtract(2);
    return model;
}

QJsonObject request(const QJsonObject &current, uint iterations = 31, bool dryRun = false)
{
    auto parameters = current.value("modal").toObject();
    parameters.insert("iterations", int(iterations));
    return {{"apiVersion", "range.application/v1"}, {"commandId", "analysis.modal.configure"}, {"commandVersion", 1},
        {"modelId", current.value("modelId")}, {"expectedRevision", current.value("revision")},
        {"requestId", QUuid::createUuid().toString()}, {"idempotencyKey", QUuid::createUuid().toString()},
        {"parameters", parameters}, {"dryRun", dryRun}};
}

class GateJob : public RJob
{
public:
    explicit GateJob(const std::shared_ptr<QSemaphore> &gate) : gate(gate) {}
protected:
    int perform() override { return gate->tryAcquire(1, 10000) ? 0 : 1; }
private:
    std::shared_ptr<QSemaphore> gate;
};
}

class TestGuiModelLifecycle : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        Application::instance()->getSession()->clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void windowEditHistorySaveReopen()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto *app = Application::instance();
        QVERIFY(app->getMainWindow());
        QVERIFY(!app->getMainWindow()->isVisible());
        auto *session = app->getSession();
        auto *host = session->getModelCommandHost();
        auto first = fixture("first");
        auto second = fixture("second");
        first.write(directory.filePath("first.rbm"));
        second.write(directory.filePath("second.rbm"));
        session->readModels({first.getFileName(), second.getFileName()});
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        QCOMPARE(session->getNModels(), 2u);
        session->setModelSelected(0, true);
        session->setModelSelected(1, false);
        auto *tree = app->getMainWindow()->findChild<ProblemTree *>();
        QVERIFY(tree);
        auto *iterations = tree->findChild<QSpinBox *>("modalIterations");
        QVERIFY(iterations);
        const auto before = host->query(0);
        QCOMPARE(before.value("modelId"), host->query(0).value("modelId"));
        iterations->setValue(24);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 24u);
        QCOMPARE(session->getModel(1).getProblemSetup().getModalSetup().getNIterations(), 12u);
        const auto edited = host->query(0);
        QCOMPARE(edited.value("modelId"), before.value("modelId"));
        QVERIFY(edited.value("revision") != before.value("revision"));
        auto stale = request(edited);
        auto preview = stale;
        preview.insert("dryRun", true);
        QCOMPARE(host->execute(0, preview).value("status").toString(), QString("preview"));

        app->getActionList()->processAvailability();
        auto *undo = app->getActionList()->getAction(Action::ACTION_GEOMETRY_UNDO);
        QVERIFY(undo->isEnabled());
        undo->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
        QCOMPARE(host->execute(0, stale).value("status").toString(), QString("rejected"));
        const auto undone = host->query(0);
        QVERIFY(undone.value("modelId") != edited.value("modelId"));
        app->getActionList()->processAvailability();
        auto *redo = app->getActionList()->getAction(Action::ACTION_GEOMETRY_REDO);
        QVERIFY(redo->isEnabled());
        redo->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 24u);
        QCOMPARE(host->execute(0, request(undone)).value("status").toString(), QString("rejected"));

        session->setModelSelected(1, true);
        iterations = tree->findChild<QSpinBox *>("modalIterations");
        QVERIFY(iterations); // Actual multiple-selection editor is present.
        iterations->setValue(42);
        for (uint id = 0; id < 2; ++id)
            QCOMPARE(session->getModel(id).getProblemSetup().getModalSetup().getNIterations(), 42u);
        app->getActionList()->processAvailability();
        undo->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 24u);
        QCOMPARE(session->getModel(1).getProblemSetup().getModalSetup().getNIterations(), 12u);
        app->getActionList()->processAvailability();
        redo->trigger();
        for (uint id = 0; id < 2; ++id)
            QCOMPARE(session->getModel(id).getProblemSetup().getModalSetup().getNIterations(), 42u);

        session->setModelSelected(1, false);
        stale = request(host->query(0));
        const QString saved = directory.filePath("saved.rbm");
        session->getModel(0).setFileName(saved);
        app->getActionList()->processAvailability();
        QSignalSpy failed(&RJobManager::getInstance(), &RJobManager::jobFailed);
        QSignalSpy finished(&RJobManager::getInstance(), &RJobManager::jobFinished);
        app->getActionList()->getAction(Action::ACTION_MODEL_SAVE)->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count() + failed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(QFileInfo::exists(saved));
        QCOMPARE(host->execute(0, stale).value("status").toString(), QString("rejected"));
        const auto savedState = host->query(0);
        session->removeModel(0); // Positional ID 0 is now the other document.
        QCOMPARE(host->execute(0, request(savedState)).value("status").toString(), QString("rejected"));
        session->readModels({saved});
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        QCOMPARE(session->getNModels(), 2u);
        QCOMPARE(session->getModel(1).getProblemSetup().getModalSetup().getNIterations(), 42u);
        QCOMPARE(host->execute(1, request(savedState)).value("status").toString(), QString("rejected"));
        QVERIFY(host->query(1).value("modelId") != savedState.value("modelId"));

        QFile journal(host->journalPath());
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const auto records = QJsonDocument::fromJson(journal.readAll()).object().value("records").toArray();
        bool published = false, publishedModelBytes = false, auditedUndo = false, auditedRedo = false;
        for (const auto &record : records)
        {
            published |= record.toObject().value("commandId").toString() == "model.artifact.published";
            if (record.toObject().value("commandId").toString() == "model.artifact.published")
            {
                const auto artifact = record.toObject().value("selectionSnapshot").toObject();
                const QString path = artifact.value("outputPath").toString();
                if (QFileInfo(path).canonicalFilePath() != QFileInfo(saved).canonicalFilePath())
                {
                    QFile bytes(path);
                    QVERIFY(bytes.open(QIODevice::ReadOnly));
                    QCOMPARE(artifact.value("outputSha256").toString(),
                        QString::fromLatin1(QCryptographicHash::hash(bytes.readAll(), QCryptographicHash::Sha256).toHex()));
                    publishedModelBytes = true;
                }
            }
            auditedUndo |= record.toObject().value("commandId").toString() == "model.external.undo";
            auditedRedo |= record.toObject().value("commandId").toString() == "model.external.redo";
        }
        QVERIFY(published);
        QVERIFY(publishedModelBytes);
        QVERIFY(auditedUndo);
        QVERIFY(auditedRedo);
    }

    void legacyReloadAndBatchValidation()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(fixture("compatible"));
        auto other = fixture("incompatible");
        other.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
        session->addModel(other);
        auto setup = session->getModel(0).getProblemSetup().getModalSetup();
        setup.setNIterations(70);
        QString failure;
        QVERIFY(!host->configureModal({0, 1}, setup, &failure));
        QVERIFY(!failure.isEmpty());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 0u);
        session->setModelSelected(0, true);
        session->setModelSelected(1, true);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!Application::instance()->getMainWindow()->findChild<ProblemTree *>()->findChild<ModalSetupWidget *>());

        auto old = request(host->query(0));
        session->getModel(0).getProblemSetup().getModalSetup().setNIterations(18);
        session->setProblemChanged(0);
        QCOMPARE(host->execute(0, old).value("status").toString(), QString("rejected"));
        old = request(host->query(0));
        session->setModelChanged(0);
        QCOMPARE(host->execute(0, old).value("status").toString(), QString("rejected"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto disk = fixture("reload");
        disk.getProblemSetup().getModalSetup().setNIterations(77);
        disk.write(directory.filePath("reload.rbm"));
        old = request(host->query(0));
        QSignalSpy failed(&RJobManager::getInstance(), &RJobManager::jobFailed);
        QSignalSpy finished(&RJobManager::getInstance(), &RJobManager::jobFinished);
        RJobManager::getInstance().submit(new ModelIO(MODEL_IO_UPDATE, disk.getFileName(), session->getModelPtr(0)));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count() + failed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 77u);
        QCOMPARE(host->execute(0, old).value("status").toString(), QString("rejected"));

        // Reload already changed memory if auditing fails: the job must report
        // failure and retire its old revision instead of claiming full success.
        old = request(host->query(0));
        QLockFile auditLock(host->journalPath() + ".lock");
        auditLock.setStaleLockTime(0);
        QVERIFY(auditLock.tryLock(0));
        failed.clear();
        finished.clear();
        RJobManager::getInstance().submit(new ModelIO(MODEL_IO_UPDATE, disk.getFileName(), session->getModelPtr(0)));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count() + failed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        auditLock.unlock();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(host->execute(0, old).value("status").toString(), QString("rejected"));
    }

    void failedCloseAuditIsReportedAndRetried()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(fixture("removed"));
        session->addModel(fixture("remaining"));
        const auto removed = host->query(0);
        const auto remaining = host->query(1);
        QSignalSpy failed(host, &ModelCommandHost::commandRejected);
        QLockFile lock(host->journalPath() + ".lock");
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock(0));
        session->removeModel(0);
        QVERIFY(!failed.isEmpty());
        lock.unlock();
        QCOMPARE(host->query(0).value("guiModelIdentity"), remaining.value("guiModelIdentity"));
        host->query(0); // A successful retry must not duplicate the close.
        QFile journal(host->journalPath());
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const auto records = QJsonDocument::fromJson(journal.readAll()).object()["records"].toArray();
        int closes = 0;
        for (const auto &value : records)
        {
            const auto record = value.toObject();
            if (record["commandId"] == QJsonValue("model.session.close")
                && QUuid(record["modelId"].toString()) == QUuid(removed["modelId"].toString())) ++closes;
        }
        QCOMPARE(closes, 1);
    }

    void bothManagersRejectBusyAndQueuedEdits()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(fixture("busy"));
        for (auto *manager : {&RJobManager::getInstance(), static_cast<RJobManager *>(&SolverManager::getInstance())})
        {
            const auto command = request(host->query(0), 91);
            auto gate = std::make_shared<QSemaphore>();
            manager->submit(new GateJob(gate));
            manager->submit(new GateJob(gate));
            QVERIFY(manager->getNWaiting() > 0);
            const auto result = host->execute(0, command);
            gate->release(2); // Release before assertions, so failure cannot strand a worker.
            QCOMPARE(result.value("status").toString(), QString("rejected"));
            QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
            QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
            QCOMPARE(host->query(0).value("busy").toBool(), false);
        }
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) return 2;
    QStandardPaths::setTestModeEnabled(true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, RVendor::familyName(), RVendor::shortName());
        settings.setValue("version", RVendor::version().toString());
        settings.setValue("cloud/softwareCheckUpdates", false);
        settings.setValue("cloud/softwareSendUsageInfo", false);
        settings.setValue("cloud/syncDataDirectory", false);
        settings.sync();
    }
    WindowApplication application(argc, argv);
    if (qEnvironmentVariableIsSet("RANGE_LIFECYCLE_TRACE")) RLogger::getInstance().setLevel(R_LOG_LEVEL_TRACE);
    TestGuiModelLifecycle test;
    const int result = QTest::qExec(&test, argc, argv);
    application.getSession()->clear();
    delete application.getMainWindow();
    SolverManager::getInstance().stopServer();
    return result;
}

#include "tst_gui_model_lifecycle.moc"
