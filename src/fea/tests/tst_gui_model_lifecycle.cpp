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
#include "workflow_widget.h"
#include "control_monitor_widget.h"
#include <QDockWidget>
#include "agent_widget.h"
#include "extension_widget.h"
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QPushButton>
#include <QAccessible>
#include "gl_widget.h"

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
    void visibleViewportRenders()
    {
        const auto output = qEnvironmentVariable("RANGE_TEST_GUI_RENDER_OUTPUT");
        if (output.isEmpty()) QSKIP("Opt-in real Windows/OpenGL rendering acceptance.");
        QVERIFY(QGuiApplication::platformName() != "offscreen");
        auto *app = Application::instance();
        Model model;
        model.read(WorkflowProcess::defaultConfiguration(QDir::tempPath()).examples + "/stage10_beam.ram");
        app->getSession()->addModel(model);
        app->getSession()->setModelSelected(0, true);
        auto *window = app->getMainWindow();
        window->resize(1440, 900); window->show();
        QTRY_VERIFY_WITH_TIMEOUT(window->findChild<GLWidget *>() != nullptr, 10000);
        auto *viewport = window->findChild<GLWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(viewport->isValid(), 10000);
        const auto frame = viewport->grabFramebuffer();
        QVERIFY(!frame.isNull()); QVERIFY(frame.width() > 100); QVERIFY(frame.height() > 100);
        QVERIFY(window->grab().save(output));
        for (const auto &name : {"agentDock", "controlDock"})
        {
            auto *featureDock = window->findChild<QDockWidget *>(name);
            QVERIFY(featureDock); featureDock->show(); featureDock->raise();
            QTest::qWait(100);
            QVERIFY(window->grab().save(output + QString(".%1.png").arg(name)));
        }
        auto *dock = window->findChild<QWidget *>("solverDock");
        QVERIFY(dock);
        qobject_cast<QDockWidget *>(dock)->raise();
        auto *tabs = dock->findChild<QTabWidget *>();
        QVERIFY(tabs);
        for (int index = 6; index < tabs->count(); ++index)
        {
            tabs->setCurrentIndex(index);
            QTest::qWait(100);
            QVERIFY(tabs->currentWidget()->isVisible());
            QVERIFY(window->grab().save(output + QString(".panel-%1.png").arg(index)));
        }
        window->hide();
    }

    void workflowControlsExposeAccessibleNames()
    {
        auto *window = Application::instance()->getMainWindow();
        for (const auto &name : {"workflowWorkspace", "workflowJobs", "workflowResults", "workflowAudit"})
        {
            auto *widget = window->findChild<QWidget *>(QString::fromLatin1(name));
            QVERIFY(widget);
            auto *accessible = QAccessible::queryAccessibleInterface(widget);
            QVERIFY(accessible);
            QVERIFY2(!accessible->text(QAccessible::Name).isEmpty(), name);
        }
    }

    void deepSeekAppliesLiveGuiCommand()
    {
        const auto credentialsPath = qEnvironmentVariable("RANGE_TEST_DEEPSEEK_CREDENTIALS_FILE");
        if (credentialsPath.isEmpty()) QSKIP("Opt-in remote-provider acceptance; credentials remain local.");
        QFile credentials(credentialsPath);
        QVERIFY(credentials.open(QIODevice::ReadOnly));
        QVERIFY(credentials.size() < 65536);
        QString apiKey;
        for (auto line : QString::fromUtf8(credentials.readAll()).split('\n'))
        {
            line = line.trimmed();
            if (line.startsWith("DEEPSEEK_API_KEY=")) apiKey = line.mid(17).trimmed();
        }
        QVERIFY(!apiKey.isEmpty());
        auto *session = Application::instance()->getSession();
        session->addModel(fixture("Remote model acceptance"));
        session->setModelSelected(0, true);
        auto *widget = Application::instance()->getMainWindow()->findChild<AgentWidget *>();
        QVERIFY(widget);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiKey(apiKey);
        widget->setProviderSettings(settings);
        auto *effort = widget->findChild<QComboBox *>("agentThinkingEffort");
        QVERIFY(effort);
        const auto effortName = qEnvironmentVariable("RANGE_TEST_DEEPSEEK_EFFORT", "off");
        const int effortIndex = effort->findData(effortName);
        QVERIFY(effortIndex >= 0); effort->setCurrentIndex(effortIndex);
        QSignalSpy finished(widget->taskRunner(), &AgentTaskRunner::finished);
        QSignalSpy applied(widget->taskRunner(), &AgentTaskRunner::actionCompleted);
        QSignalSpy usage(widget, &AgentWidget::providerUsage);
        widget->startTask("For the selected open GUI document, use document_propose to set only modal iterations to 53, keeping method, modes, tolerance and unit exactly as in the supplied live context. This task authorizes that edit. After a successful host result, query context once to verify, then finish with a short summary. Do not use workflow tools or propose additional changes.", true);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 180000);
        QVERIFY2(finished.last().at(0).toString() == "completed", qPrintable(finished.last().at(1).toString()));
        QCOMPARE(finished.last().at(0).toString(), QString("completed"));
        QCOMPARE(applied.size(), 1);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 53u);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNModesToExtract(), 2u);
        QVERIFY(session->getModel(0).getUndoStackSize() > 0);
        if (qEnvironmentVariableIsSet("RANGE_TEST_DEEPSEEK_FOLLOWUP"))
        {
            finished.clear();
            auto *composer = widget->findChild<QPlainTextEdit *>("agentGoal");
            auto *send = widget->findChild<QPushButton *>("agentSend");
            QVERIFY(composer); QVERIFY(send);
            composer->setPlainText("Using our preceding conversation only, what modal iteration count did you just set? Reply with that decimal integer only. Do not call any tools or make changes.");
            send->click();
            QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 180000);
            QCOMPARE(finished.last().at(0).toString(), QString("completed"));
            QCOMPARE(finished.last().at(1).toString().trimmed(), QString("53"));
            QCOMPARE(applied.size(), 1);
        }
        QVERIFY(!usage.isEmpty());
        int tokens = 0;
        for (const auto &entry : usage) tokens += entry.at(0).toJsonObject().value("totalTokens").toInt();
        qInfo("DeepSeek live GUI acceptance: requests=%d total_tokens=%d", int(usage.size()), tokens);
        settings.setApiKey({}); widget->setProviderSettings(settings);
    }

    void deepSeekGeneratesValidatedExtension()
    {
        const auto credentialsPath = qEnvironmentVariable("RANGE_TEST_DEEPSEEK_CREDENTIALS_FILE");
        if (credentialsPath.isEmpty()) QSKIP("Opt-in remote extension acceptance; credentials remain local.");
        QFile credentials(credentialsPath);
        QVERIFY(credentials.open(QIODevice::ReadOnly));
        QVERIFY(credentials.size() < 65536);
        QString apiKey;
        for (auto line : QString::fromUtf8(credentials.readAll()).split('\n'))
            if (line.trimmed().startsWith("DEEPSEEK_API_KEY=")) apiKey = line.trimmed().mid(17).trimmed();
        QVERIFY(!apiKey.isEmpty());
        QTemporaryDir directory(qEnvironmentVariable("RANGE_TEST_EXTENSION_OUTPUT",
            QDir::tempPath() + "/range-agent-extension-XXXXXX"));
        QVERIFY(directory.isValid());
        if (qEnvironmentVariableIsSet("RANGE_TEST_EXTENSION_OUTPUT")) directory.setAutoRemove(false);
        auto *window = Application::instance()->getMainWindow();
        auto *workflow = window->findChild<WorkflowWidget *>();
        auto *widget = window->findChild<AgentWidget *>();
        QVERIFY(workflow); QVERIFY(widget);
        auto config = WorkflowProcess::defaultConfiguration(directory.path());
        config.trustedExtensions = true;
        workflow->openWorkspace(config);
        auto *service = workflow->transport();
        QTRY_VERIFY_WITH_TIMEOUT(service->isReady(), 15000);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiKey(apiKey); widget->setProviderSettings(settings);
        QSignalSpy finished(widget->taskRunner(), &AgentTaskRunner::finished);
        QSignalSpy usage(widget, &AgentWidget::providerUsage);
        int approvals = 0;
        QObject approvalScope;
        // This opt-in test explicitly authorizes staging in its disposable workspace.
        // Production proposals still require the user's review button.
        const auto approvalConnection = connect(widget->taskRunner(), &AgentTaskRunner::proposalReady,
            &approvalScope, [&, runner = widget->taskRunner()](const QString &origin, const QString &id, const QJsonObject &preview) {
                const bool allowed = origin == "extension" && preview.value("normalized").toObject().value("command") == "extension.stage";
                if (allowed) ++approvals;
                QTimer::singleShot(0, runner, [runner, id, allowed] { runner->authorizePending(id, allowed); });
            });
        widget->startTask("Create one new Python numeric-series-statistics extension named acceptance.generated, version 1.0.0, in the connected workspace. First read workflow_capabilities, schema_get for range.extension/v1/manifest, and knowledge_query for extension. Generate your own process(series) source to compute sample RMS, peakAbs and earliest peakTime with explicit unchanged units; use scaled RMS to avoid overflow. Generate at least one numerical test case (tests is an object with cases, each case has id, input and expected). Propose only extension.stage with manifest, source and tests. Wait for the staged job using workflow_wait, then finish. Do not validate, activate, run, rollback or change GUI documents; the acceptance host will do those separately. No third-party imports, file access, network or subprocesses.", false);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 300000);
        disconnect(approvalConnection);
        QVERIFY2(finished.last().at(0).toString() == "completed", qPrintable(finished.last().at(1).toString()));
        QCOMPARE(finished.last().at(0).toString(), QString("completed"));
        QCOMPARE(approvals, 1);
        qInfo().noquote() << "Generated extension workspace:" << directory.path();
        settings.setApiKey({}); widget->setProviderSettings(settings);
        int tokens = 0;
        for (const auto &entry : usage) tokens += entry.at(0).toJsonObject().value("totalTokens").toInt();
        qInfo("DeepSeek generated extension: requests=%d total_tokens=%d", int(usage.size()), tokens);

        QString rpcError;
        const auto rpc = [&](const QString &operation, const QJsonObject &parameters = QJsonObject{}) {
            QEventLoop loop;
            QJsonObject result;
            QString id;
            rpcError.clear();
            const auto success = connect(service, &WorkflowProcess::replyReceived, &loop,
                [&](const QString &received, const QString &, const QJsonObject &value) {
                    if (received == id) { result = value; loop.quit(); }
                });
            const auto failure = connect(service, &WorkflowProcess::requestFailed, &loop,
                [&](const QString &received, const QString &, const QString &code, const QString &message) {
                    if (received == id) { rpcError = code + ": " + message; loop.quit(); }
                });
            QTimer timer;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, [&] { rpcError = "RPC timeout"; loop.quit(); });
            id = service->request(operation, parameters); timer.start(30000); loop.exec();
            disconnect(success); disconnect(failure);
            return result;
        };
        const auto listing = rpc("extension.list", {{"extensionId", "acceptance.generated"}});
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        const auto entries = listing.value("extensions").toArray();
        QCOMPARE(entries.size(), 1);
        const auto candidates = entries.first().toObject().value("candidates").toArray();
        QCOMPARE(candidates.size(), 1);
        const auto candidate = candidates.first().toObject();
        const QJsonObject identity{{"candidateId", candidate.value("candidateId")}, {"bundleHash", candidate.value("bundleHash")}};
        const auto execute = [&](const QString &command, const QJsonObject &parameters) {
            const auto project = rpc("project.query");
            if (!rpcError.isEmpty()) return QJsonObject{};
            const auto preview = rpc("command.preview", {{"command", command}, {"parameters", parameters}, {"expectedRevision", project.value("revision")}});
            if (!rpcError.isEmpty()) return QJsonObject{};
            const auto approval = rpc("host.approve", {{"previewId", preview.value("previewId")}, {"previewHash", preview.value("previewHash")}});
            if (!rpcError.isEmpty()) return QJsonObject{};
            const auto submitted = rpc("command.execute", {{"previewId", preview.value("previewId")}, {"approvalToken", approval.value("approvalToken")}});
            if (!rpcError.isEmpty()) return QJsonObject{};
            QElapsedTimer deadline; deadline.start();
            QJsonObject job;
            while (deadline.elapsed() < 30000) {
                job = rpc("jobs.query", {{"jobId", submitted.value("jobId")}}).value("job").toObject();
                if (!rpcError.isEmpty()) return QJsonObject{};
                const auto state = job.value("state").toString();
                if (state != "queued" && state != "running" && state != "cancelling") return job;
                QTest::qWait(30);
            }
            rpcError = "Job timeout"; return job;
        };
        const auto validation = execute("extension.validate", identity);
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QVERIFY2(validation.value("state") == "succeeded", QJsonDocument(validation).toJson().constData());
        const auto activation = execute("extension.activate", identity);
        QVERIFY2(activation.value("state") == "succeeded", QJsonDocument(activation).toJson().constData());
        const QJsonObject series{{"times", QJsonArray{0, 1}}, {"values", QJsonArray{3, 4}}, {"timeUnit", "s"}, {"valueUnit", "m"}};
        const auto run = execute("extension.run", {{"extensionId", "acceptance.generated"}, {"bundleHash", candidate.value("bundleHash")}, {"series", series}});
        QVERIFY2(run.value("state") == "succeeded", QJsonDocument(run).toJson().constData());
        const auto metrics = run.value("result").toObject().value("metrics").toObject();
        QVERIFY(qAbs(metrics.value("rms").toDouble() - 3.5355339059327378) < 1e-12);
        QCOMPARE(metrics.value("peakAbs").toDouble(), 4.0);
        QCOMPARE(metrics.value("peakTime").toDouble(), 1.0);
        QCOMPARE(metrics.value("valueUnit").toString(), QString("m"));
        qInfo("Generated extension independently validated, activated and executed successfully.");
        service->stop();
        QTRY_VERIFY_WITH_TIMEOUT(service->isStopped(), 5000);
    }

    void generatedExtensionRunsThroughGui()
    {
        const auto workspace = qEnvironmentVariable("RANGE_TEST_GENERATED_EXTENSION_WORKSPACE");
        if (workspace.isEmpty()) QSKIP("Opt-in GUI acceptance of the retained real-model-generated candidate.");
        auto *window = Application::instance()->getMainWindow();
        auto *workflow = window->findChild<WorkflowWidget *>();
        auto *widget = window->findChild<ExtensionWidget *>();
        QVERIFY(workflow); QVERIFY(widget);
        auto config = WorkflowProcess::defaultConfiguration(workspace);
        config.trustedExtensions = true;
        workflow->openWorkspace(config);
        QTRY_VERIFY_WITH_TIMEOUT(workflow->transport()->isReady(), 15000);
        auto *tree = widget->findChild<QTreeWidget *>("extensionList");
        QVERIFY(tree);
        const auto activeItem = [&]() -> QTreeWidgetItem * {
            QTreeWidgetItemIterator it(tree);
            while (*it) {
                auto *item = *it;
                if (item->text(0) == "Version 1.0.0" && item->text(1) == "active") return item;
                ++it;
            }
            return nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(activeItem(), 15000);
        tree->setCurrentItem(activeItem());
        auto *times = widget->findChild<QPlainTextEdit *>("extensionSeriesTimes");
        auto *values = widget->findChild<QPlainTextEdit *>("extensionSeriesValues");
        auto *unit = widget->findChild<QComboBox *>("extensionValueUnit");
        auto *run = widget->findChild<QPushButton *>("extensionRun");
        auto *approve = widget->findChild<QPushButton *>("extensionApprove");
        auto *metrics = widget->findChild<QTableWidget *>("extensionMetrics");
        QVERIFY(times && values && unit && run && approve && metrics);
        times->setPlainText("0, 1"); values->setPlainText("3, 4"); unit->setCurrentText("m");
        QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 15000);
        run->click();
        QTRY_VERIFY_WITH_TIMEOUT(approve->isEnabled(), 15000);
        approve->click();
        QTRY_VERIFY_WITH_TIMEOUT(metrics->rowCount() == 3 && metrics->item(0, 1)
            && qAbs(metrics->item(0, 1)->text().toDouble() - 3.5355339059327378) < 1e-9, 30000);
        QCOMPARE(metrics->item(0, 2)->text(), QString("m"));
        QCOMPARE(metrics->item(1, 1)->text().toDouble(), 4.0);
        QCOMPARE(metrics->item(2, 1)->text().toDouble(), 1.0);
        qInfo("Real-model-generated extension ran through the GUI preview, approval and result table.");
        workflow->transport()->stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow->transport()->isStopped(), 5000);
    }

    void solverPublicationDoesNotOverwriteNewerEdits()
    {
        QTemporaryDir directory;
        auto *session = Application::instance()->getSession();
        auto source = fixture("publication");
        source.setFileName(directory.filePath("source.rbm"));
        session->addModel(source);
        session->addModel(fixture("Other document with history"));
        auto *host = session->getModelCommandHost();
        QCOMPARE(host->execute(0, request(host->query(0), 33)).value("status").toString(), QString("accepted"));
        QCOMPARE(host->execute(1, request(host->query(1), 44)).value("status").toString(), QString("accepted"));
        const auto previousHistory = session->getModel(0).getUndoStackSize();
        const auto otherHistory = session->getModel(1).getUndoStackSize();
        const auto stamp = session->modelCommandStamp(0);
        auto result = source;
        result.getProblemSetup().getModalSetup().setNIterations(58);
        result.setFileName(directory.filePath("result.rbm"));
        QSignalSpy events(session, &Session::solverResultAvailable);
        QString error;
        QVERIFY(session->acceptSolverResult(stamp, result, result.getFileName(), &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 58u);
        QCOMPARE(session->getModel(0).getFileName(), source.getFileName());
        QVERIFY(session->getModel(0).getUndoStackSize() > 0);
        QCOMPARE(session->getModel(0).getUndoStackSize(), previousHistory + 1);
        QCOMPARE(session->getModel(1).getUndoStackSize(), otherHistory);
        QCOMPARE(session->getModel(1).getProblemSetup().getModalSetup().getNIterations(), 44u);
        QVERIFY(!session->acceptSolverResult(stamp, source, source.getFileName(), &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 58u);
        QCOMPARE(events.count(), 2);
        QCOMPARE(events.last().at(3).toBool(), false);
        session->setModelSelected(0, true);
        auto *actions = Application::instance()->getActionList();
        actions->processAvailability();
        actions->getAction(Action::ACTION_GEOMETRY_UNDO)->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 33u);
        actions->processAvailability();
        actions->getAction(Action::ACTION_GEOMETRY_REDO)->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 58u);
        session->clear();
        session->addModel(fixture("new incarnation"));
        QVERIFY(!session->acceptSolverResult(stamp, result, result.getFileName(), &error));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
    }

    void workflowPanelRunsAndResumesRealBeam()
    {
        QTemporaryDir directory;
        const int testSteps = qEnvironmentVariableIntValue("RANGE_TEST_CONTROL_STEPS") > 0
            ? qBound(20, qEnvironmentVariableIntValue("RANGE_TEST_CONTROL_STEPS"), 100) : 20;
        auto *widget = Application::instance()->getMainWindow()->findChild<WorkflowWidget *>();
        QVERIFY(widget);
        const auto config = WorkflowProcess::defaultConfiguration(directory.path());
        QSignalSpy errors(widget->transport(), &WorkflowProcess::serviceError);
        QSignalSpy accepted(widget, &WorkflowWidget::jobAccepted);
        QSignalSpy results(widget, &WorkflowWidget::resultDisplayed);
        auto *monitor = widget->findChild<ControlMonitorWidget *>();
        QVERIFY(monitor);
        QSignalSpy samples(monitor, &ControlMonitorWidget::samplesUpdated);
        widget->openWorkspace(config);
        QTRY_VERIFY_WITH_TIMEOUT(widget->transport()->isReady(), 15000);
        widget->previewCommand("workflow.configure", {{"model", "cantilever-beam"},
            {"dt", .005}, {"steps", testSteps}, {"targetDamping", .1}});
        QTRY_VERIFY_WITH_TIMEOUT(!widget->currentPreview().isEmpty(), 15000);
        widget->applyPreview();
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 15000);
        results.clear();
        widget->previewCommand("workflow.run", {{"mode", "both"}, {"independentReference", true}});
        QTRY_VERIFY_WITH_TIMEOUT(!widget->currentPreview().isEmpty(), 15000);
        widget->applyPreview();
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 60000);
        auto first = results.last().at(0).toJsonObject();
        QCOMPARE(first.value("totalSteps").toInt(), testSteps);
        QVERIFY(first.contains("active")); QVERIFY(first.contains("passive"));
        QVERIFY(first.value("active").toObject().value("maxDisplacementErrorM").toDouble() < 1e-8);
        QTRY_VERIFY_WITH_TIMEOUT(monitor->sampleCount() >= testSteps, 5000);
        QCOMPARE(monitor->currentJob(), first.value("jobId").toString());
        bool sawRunningSamples = false;
        for (const auto &event : samples)
            sawRunningSamples |= event.at(1).toInt() > 0 && event.at(2).toString() == "running";
        QVERIFY2(sawRunningSamples, "Native samples must arrive before the job finishes");
        auto *table = widget->findChild<QTableWidget *>("workflowResults");
        QVERIFY(table); QCOMPARE(table->rowCount(), 5);
        results.clear();
        widget->previewCommand("workflow.resume", {{"jobId", first.value("jobId")}, {"additionalSteps", testSteps}});
        QTRY_VERIFY_WITH_TIMEOUT(!widget->currentPreview().isEmpty(), 15000);
        widget->applyPreview();
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 60000);
        const auto resumed = results.last().at(0).toJsonObject();
        QCOMPARE(resumed.value("totalSteps").toInt(), 2 * testSteps);
        QVERIFY(resumed.value("active").toObject().value("maxRotationErrorRad").toDouble() < 1e-8);
        QTRY_VERIFY_WITH_TIMEOUT(monitor->sampleCount() >= testSteps, 5000);
        QCOMPARE(monitor->currentJob(), resumed.value("jobId").toString());
        auto *holdDisplay = monitor->findChild<QPushButton *>("controlHold");
        QVERIFY(holdDisplay); holdDisplay->setChecked(true);
        QVERIFY(holdDisplay->text().contains("Resume")); holdDisplay->setChecked(false);
        const auto render = qEnvironmentVariable("RANGE_TEST_CONTROL_RENDER_OUTPUT");
        if (!render.isEmpty())
        {
            auto *window = Application::instance()->getMainWindow();
            auto *session = Application::instance()->getSession();
            if (session->getNModels() == 0)
            {
                Model model; model.read(config.examples + "/stage10_beam.ram");
                session->addModel(model); session->setModelSelected(0, true);
            }
            window->resize(1600, 1000); window->show();
            window->findChild<QDockWidget *>("agentDock")->raise();
            window->findChild<QDockWidget *>("controlDock")->show();
            widget->findChild<QTabWidget *>("workflowPages")->setCurrentIndex(0);
            QTest::qWait(200);
            QVERIFY(window->grab().save(render));
            window->hide();
        }
        QCOMPARE(accepted.count(), 3);
        QCOMPARE(errors.count(), 0);
        widget->transport()->stop();
        QTRY_VERIFY_WITH_TIMEOUT(widget->transport()->isStopped(), 5000);
    }

    void cleanup()
    {
        auto *window = Application::instance()->getMainWindow();
        for (auto *agent : window->findChildren<AgentWidget *>()) agent->taskRunner()->cancel();
        for (auto *workflow : window->findChildren<WorkflowWidget *>())
        {
            workflow->transport()->stop();
            QTRY_VERIFY_WITH_TIMEOUT(workflow->transport()->isStopped(), 5000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        Application::instance()->getSession()->clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void documentListChangesPreserveEachHistory()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(fixture("First history"));
        QCOMPARE(host->execute(0, request(host->query(0), 31)).value("status").toString(), QString("accepted"));
        const auto firstDepth = session->getModel(0).getUndoStackSize();
        QVERIFY(firstDepth > 0);
        session->addModel(fixture("Second history"));
        QCOMPARE(session->getModel(0).getUndoStackSize(), firstDepth);
        QCOMPARE(host->execute(1, request(host->query(1), 42)).value("status").toString(), QString("accepted"));
        QCOMPARE(host->execute(1, request(host->query(1), 43)).value("status").toString(), QString("accepted"));
        const auto secondDepth = session->getModel(1).getUndoStackSize();
        QVERIFY(secondDepth > firstDepth);
        session->addModel(fixture("Third document"));
        QCOMPARE(session->getModel(1).getUndoStackSize(), secondDepth);
        session->removeModel(0);
        QCOMPARE(session->getModel(0).getName(), QString("Second history"));
        QCOMPARE(session->getModel(0).getUndoStackSize(), secondDepth);
        session->getModel(0).undo(0);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 42u);
        QCOMPARE(session->getModel(1).getUndoStackSize(), 0u);
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

int runGuiHostContextTests(int argc, char **argv);
int runAgentTaskRunnerTests(int argc, char **argv);
int runSolverSnapshotTests(int argc, char **argv);
int runExtensionWidgetTests(int argc, char **argv);
int runAgentEventViewTests(int argc, char **argv);
int runAgentConversationWidgetTests(int argc, char **argv);

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
    const bool hostContext = argc > 1 && QByteArray(argv[1]) == "--host-context";
    const bool agentRunner = argc > 1 && QByteArray(argv[1]) == "--agent-runner";
    const bool solverSnapshot = argc > 1 && QByteArray(argv[1]) == "--solver-snapshot";
    const bool extensionWidget = argc > 1 && QByteArray(argv[1]) == "--extension-widget";
    const bool agentEventView = argc > 1 && QByteArray(argv[1]) == "--agent-event-view";
    const bool agentConversation = argc > 1 && QByteArray(argv[1]) == "--agent-conversation";
    if (hostContext || agentRunner || solverSnapshot || extensionWidget || agentEventView || agentConversation)
    {
        for (int i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
        --argc; argv[argc] = nullptr;
    }
    const int result = hostContext ? runGuiHostContextTests(argc, argv)
        : agentRunner ? runAgentTaskRunnerTests(argc, argv)
        : solverSnapshot ? runSolverSnapshotTests(argc, argv)
        : extensionWidget ? runExtensionWidgetTests(argc, argv)
        : agentEventView ? runAgentEventViewTests(argc, argv)
        : agentConversation ? runAgentConversationWidgetTests(argc, argv) : QTest::qExec(&test, argc, argv);
    application.getSession()->clear();
    delete application.getMainWindow();
    SolverManager::getInstance().stopServer();
    return result;
}

#include "tst_gui_model_lifecycle.moc"
