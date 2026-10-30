#include "runtime_control_widget.h"
#include "workflow_process.h"
#if RANGE_ENABLE_AGENT
#include "agent_task_runner.h"
#include "application.h"
#include <rai_agent.h>
#endif
#include "control_monitor_widget.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QApplication>
#include <QTimer>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class RuntimeControlWidgetTests : public QObject
{
    Q_OBJECT
    QTemporaryDir fixture;
    WorkflowProcess::Configuration config;
    template<class T> T *control(RuntimeControlWidget &widget, const char *name)
    { return widget.findChild<T *>(QString::fromLatin1(name)); }
    void click(RuntimeControlWidget &widget, const char *name)
    { QTest::mouseClick(control<QPushButton>(widget, name), Qt::LeftButton); }
    QJsonObject request(WorkflowProcess &process, const QString &op, const QJsonObject &params = {})
    {
        QSignalSpy replies(&process, &WorkflowProcess::replyReceived);
        const auto id = process.request(op, params); QElapsedTimer clock; clock.start();
        while (clock.elapsed() < 10000) {
            for (const auto &reply : replies) if (reply[0].toString() == id) return reply[2].toJsonObject();
            QTest::qWait(10);
        }
        return {};
    }
    void start(WorkflowProcess &process, RuntimeControlWidget &widget)
    {
        widget.resize(420, 720); widget.show(); process.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(control<QComboBox>(widget, "runtimeJobChoice")->count(), 2, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(control<QPushButton>(widget, "runtimePrepare")->isEnabled(), 10000);
    }
private slots:
    void initTestCase()
    {
        QVERIFY(fixture.isValid());
        config = WorkflowProcess::defaultConfiguration(fixture.path() + "/workspace");
#ifdef RANGE_RUNTIME_PYTHON
        config.python = QString::fromUtf8(RANGE_RUNTIME_PYTHON);
#endif
        QVERIFY(!config.python.isEmpty());
        config.solver = config.python; config.native = config.python;
        config.packageDirectory = fixture.path() + "/package"; config.examples = fixture.path();
        QVERIFY(QDir().mkpath(config.packageDirectory + "/range_workflow"));
        QFile init(config.packageDirectory + "/range_workflow/__init__.py");
        QVERIFY(init.open(QIODevice::WriteOnly)); init.close();
#ifdef RANGE_RUNTIME_WIDGET_BOOTSTRAP
        QVERIFY(QFile::copy(QString::fromUtf8(RANGE_RUNTIME_WIDGET_BOOTSTRAP),
            config.packageDirectory + "/range_workflow/__main__.py"));
#else
        QFAIL("RANGE_RUNTIME_WIDGET_BOOTSTRAP must identify the isolated transport fixture");
#endif
    }
    void explicitApprovalAndActualReceiptAreSeparate()
    {
        WorkflowProcess process; RuntimeControlWidget widget(&process); start(process, widget);
        auto *approve = control<QPushButton>(widget, "runtimeApproveSubmit");
        auto *status = control<QLabel>(widget, "runtimeStatus");
        QVERIFY(!approve->isEnabled());
        control<QDoubleSpinBox>(widget, "runtimeGain")->setValue(3);
        click(widget, "runtimePrepare"); QTRY_VERIFY(approve->isEnabled());
        QCOMPARE(request(process, "fixture.query").value("counts").toObject().value("approve").toInt(), 0);
        QCOMPARE(request(process, "fixture.query").value("counts").toObject().value("submit").toInt(), 0);
        const QString before = control<QPlainTextEdit>(widget, "runtimePreview")->toPlainText();
        QVERIFY(before.contains("2 → 3")); QVERIFY(before.contains("N*s/m"));
        click(widget, "runtimeRefresh");
        QTest::qWait(100);
        QCOMPARE(control<QDoubleSpinBox>(widget, "runtimeGain")->value(), 3.0);
        QVERIFY(approve->isEnabled());
        QSignalSpy replies(&process, &WorkflowProcess::replyReceived);
        click(widget, "runtimeApproveSubmit"); QTRY_VERIFY(!approve->isEnabled());
        QTRY_COMPARE(request(process, "fixture.query").value("counts").toObject().value("submit").toInt(), 1);
        QVERIFY(!status->text().startsWith("Accepted at"));
        QString proposalId;
        for (const auto &reply : replies)
            if (reply[1].toString() == "host.runtime.parameters.approve") proposalId = reply[2].toJsonObject().value("proposalId").toString();
        QVERIFY(!proposalId.isEmpty());
        request(process, "fixture.accept", {{"proposalId", proposalId}}); click(widget, "runtimeRefresh");
        QTRY_VERIFY(status->text().startsWith("Accepted at sequence 7, time 0.07 s"));
        QTRY_VERIFY(control<QPlainTextEdit>(widget, "runtimeAcceptedHistory")->toPlainText().contains("accepted sequence 7"));
        QVERIFY(!control<QPlainTextEdit>(widget, "runtimePreview")->toPlainText().contains("fixture-secret-token"));
        process.stop(); QTRY_VERIFY(process.isStopped());
    }
    void jobAndWorkspaceChangesDiscardLateApproval()
    {
        WorkflowProcess process; RuntimeControlWidget widget(&process); start(process, widget);
        request(process, "fixture.defer"); click(widget, "runtimePrepare");
        QTRY_VERIFY(control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        click(widget, "runtimeApproveSubmit");
        QTRY_COMPARE(request(process, "fixture.query").value("deferred").toArray().size(), 1);
        QSignalSpy replies(&process, &WorkflowProcess::replyReceived);
        control<QComboBox>(widget, "runtimeJobChoice")->setCurrentIndex(1);
        request(process, "fixture.release");
        QCOMPARE(request(process, "fixture.query").value("counts").toObject().value("submit").toInt(), 0);
        QList<QVariant> late;
        for (const auto &reply : replies) if (reply[1].toString() == "host.runtime.parameters.approve") late = reply;
        QVERIFY(!late.isEmpty());
        process.stop(); QTRY_VERIFY(process.isStopped());
        auto other = config; other.workspace = fixture.path() + "/other-workspace";
        process.start(other); QTRY_VERIFY(process.isReady());
        QTRY_VERIFY(control<QPushButton>(widget, "runtimePrepare")->isEnabled());
        process.replyReceived(late[0].toString(), late[1].toString(), late[2].toJsonObject());
        QCOMPARE(request(process, "fixture.query").value("counts").toObject().value("submit").toInt(), 0);
        QVERIFY(!control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        process.stop(); QTRY_VERIFY(process.isStopped());
    }
    void revisionInvalidatesPreviewAndPauseNeedsObservation()
    {
        WorkflowProcess process; RuntimeControlWidget widget(&process); start(process, widget);
        click(widget, "runtimePrepare"); QTRY_VERIFY(control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        const auto job = control<QComboBox>(widget, "runtimeJobChoice")->currentData().toString();
        request(process, "fixture.revision", {{"jobId", job}}); click(widget, "runtimeRefresh");
        QTRY_VERIFY(!control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        QCOMPARE(control<QDoubleSpinBox>(widget, "runtimeGain")->value(), 2.0);
        click(widget, "runtimePause");
        QTRY_COMPARE(request(process, "fixture.query").value("counts").toObject().value("pause").toInt(), 1);
        auto *state = control<QLabel>(widget, "runtimeExecutionState");
        QVERIFY(!state->text().contains("paused"));
        request(process, "fixture.execution", {{"state", "paused"}}); click(widget, "runtimeRefresh");
        QTRY_VERIFY(state->text().contains("paused"));
        process.stop(); QTRY_VERIFY(process.isStopped());
    }
    void changedRevisionDiscardsDeferredApprovalToken()
    {
        WorkflowProcess process; RuntimeControlWidget widget(&process); start(process, widget);
        request(process, "fixture.defer"); click(widget, "runtimePrepare");
        QTRY_VERIFY(control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        click(widget, "runtimeApproveSubmit");
        QTRY_COMPARE(request(process, "fixture.query").value("deferred").toArray().size(), 1);
        const auto job = control<QComboBox>(widget, "runtimeJobChoice")->currentData().toString();
        request(process, "fixture.revision", {{"jobId", job}}); click(widget, "runtimeRefresh");
        QTRY_VERIFY(control<QLabel>(widget, "runtimeIdentity")->text().contains("Current parameter revision: 1"));
        request(process, "fixture.release");
        QCOMPARE(request(process, "fixture.query").value("counts").toObject().value("submit").toInt(), 0);
        QVERIFY(!control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        process.stop(); QTRY_VERIFY(process.isStopped());
    }

#if RANGE_ENABLE_AGENT
    void actualNativeAgentPreviewAndVisibleApprovalApplyAtAcceptedBoundary()
    {
#if defined(RANGE_RUNTIME_PACKAGE) && defined(RANGE_RUNTIME_SOLVER) && defined(RANGE_RUNTIME_NATIVE) && defined(RANGE_RUNTIME_EXAMPLES)
        QTemporaryDir actualFixture;
        QVERIFY(actualFixture.isValid());
        auto actual = WorkflowProcess::defaultConfiguration(actualFixture.path() + "/workspace");
        actual.python = QString::fromUtf8(RANGE_RUNTIME_PYTHON);
        actual.packageDirectory = QString::fromUtf8(RANGE_RUNTIME_PACKAGE);
        actual.solver = QString::fromUtf8(RANGE_RUNTIME_SOLVER);
        actual.native = QString::fromUtf8(RANGE_RUNTIME_NATIVE);
        actual.examples = QString::fromUtf8(RANGE_RUNTIME_EXAMPLES);
        QProcess bootstrap;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONPATH", actual.packageDirectory); env.insert("PYTHONNOUSERSITE", "1");
        bootstrap.setProcessEnvironment(env);
        bootstrap.start(actual.python, {QDir(actual.packageDirectory).absoluteFilePath("../tests/task_widget_bootstrap.py"),
            "--root", actualFixture.path(), "--solver", actual.solver, "--native", actual.native,
            "--examples", actual.examples, "--python", actual.python});
        QVERIFY(bootstrap.waitForStarted(10000)); QVERIFY(bootstrap.waitForFinished(120000));
        QVERIFY2(bootstrap.exitCode() == 0, bootstrap.readAllStandardError().constData());
        WorkflowProcess process; RuntimeControlWidget widget(&process);
        widget.resize(700, 1050); widget.show(); process.start(actual);
        QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 30000);
        const auto project = request(process, "project.query");
        auto preview = request(process, "command.preview", {{"command", "workflow.run"},
            {"expectedRevision", project.value("revision")},
            {"parameters", QJsonObject{{"mode", "active"}, {"independentReference", true},
                {"runtimeControl", true}, {"pauseAtAcceptedSeq", 2}}}});
        QVERIFY(!preview.isEmpty());
        const auto approval = request(process, "host.approve", {{"previewId", preview.value("previewId")},
            {"previewHash", preview.value("previewHash")}});
        const auto launched = request(process, "command.execute", {{"previewId", preview.value("previewId")},
            {"approvalToken", approval.value("approvalToken")}});
        const QString job = launched.value("jobId").toString(); QVERIFY(!job.isEmpty());
        widget.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(widget, "runtimeExecutionState")->text().contains("paused"), 90000);
        QVERIFY(control<QPushButton>(widget, "runtimePrepare")->isEnabled());
        ControlMonitorWidget monitor(&process);
        monitor.resize(700, 350); monitor.show(); monitor.setJob(job, "running");
        QTRY_COMPARE_WITH_TIMEOUT(monitor.sampleCount(), 2, 15000);
        const QString pausedCsv = actualFixture.path() + "/paused-native-samples.csv";
        const bool previousDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QTimer::singleShot(100, &monitor, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                dialog->selectFile(pausedCsv); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        QTest::mouseClick(monitor.findChild<QPushButton *>("controlExport"), Qt::LeftButton);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previousDialogs);
        QFile exported(pausedCsv); QVERIFY(exported.open(QIODevice::ReadOnly));
        const auto pausedData = exported.readAll(); exported.close();
        QCOMPARE(pausedData.split('\n').size(), 4); // Header, two actual rows, final newline.
        QCOMPARE(request(process, "runtime.parameters.query", {{"jobId", job}}).value("execution").toObject().value("acceptedSeq").toInt(), 2);
        auto *displayHold = monitor.findChild<QPushButton *>("controlHold");
        QVERIFY(displayHold); displayHold->setChecked(true);
        const auto state = request(process, "runtime.parameters.query", {{"jobId", job}});
        const auto attempts = state.value("runtimeControl").toObject().value("attempts").toObject();
        QVERIFY(!attempts.isEmpty());
        const auto before = attempts.begin().value().toObject().value("acceptedState").toObject().value("parameters").toObject();
        const auto limits = before.value("actuatorLimits").toArray();
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &process);
        QSignalSpy turns(&runner, &AgentTaskRunner::turnRequested);
        runner.startTask("Preview a finite gain update for this paused native run.", true);
        QTRY_COMPARE_WITH_TIMEOUT(turns.size(), 1, 15000);
        const QJsonObject arguments{{"jobId", job}, {"velocityGainNsm", before.value("velocityGainNsm").toDouble() * 1.2},
                                    {"actuatorLimits", limits}};
        const QJsonObject call{{"id", "native-runtime-preview"}, {"type", "function"},
            {"function", QJsonObject{{"name", "runtime_parameters_propose"},
                {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}};
        const QJsonObject body{{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"role", "assistant"},
            {"content", ""}, {"tool_calls", QJsonArray{call}}}}, {"finish_reason", "tool_calls"}}}}};
        QJsonObject turn; QString parseError;
        QVERIFY2(RAgent::parseToolResponse(QJsonDocument(body).toJson(QJsonDocument::Compact), &turn, &parseError), qPrintable(parseError));
        runner.receiveTurn(turns.first()[0].toString(), turn);
        QTRY_COMPARE_WITH_TIMEOUT(turns.size(), 2, 15000);
        widget.refresh();
        auto *saved = control<QComboBox>(widget, "runtimeProposalChoice");
        QTRY_COMPARE_WITH_TIMEOUT(saved->count(), 2, 15000);
        QVERIFY(!control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        saved->setCurrentIndex(1);
        QTRY_VERIFY(control<QPushButton>(widget, "runtimeApproveSubmit")->isEnabled());
        const QString proposalId = saved->currentData().toString();
        const auto unapproved = request(process, "runtime.parameters.query", {{"jobId", job}, {"proposalId", proposalId}});
        QCOMPARE(unapproved.value("status").toString(), QString("previewed")); QVERIFY(!unapproved.contains("receipt"));
        QCOMPARE(request(process, "runtime.parameters.query", {{"jobId", job}}).value("execution").toObject().value("acceptedSeq").toInt(), 2);
        click(widget, "runtimeApproveSubmit");
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(widget, "runtimeStatus")->text().contains("queued"), 15000);
        click(widget, "runtimeResume");
        QJsonObject completed;
        QElapsedTimer clock; clock.start();
        do {
            completed = request(process, "jobs.query", {{"jobId", job}}).value("job").toObject();
            if (completed.value("state") != "running" && completed.value("state") != "queued") break;
            QTest::qWait(50);
        } while (clock.elapsed() < 90000);
        QCOMPARE(completed.value("state").toString(), QString("succeeded"));
        QTRY_COMPARE_WITH_TIMEOUT(monitor.sampleCount(), 20, 15000);
        QVERIFY(displayHold->isChecked());
        QVERIFY(monitor.findChild<QLabel *>("controlTelemetryStatus")->text().contains("DISPLAY HELD"));
        QVERIFY(monitor.findChild<QLabel *>("controlTelemetryStatus")->text().contains("(2 steps)"));
        const auto accepted = request(process, "runtime.parameters.query", {{"jobId", job}, {"proposalId", proposalId}});
        QCOMPARE(accepted.value("status").toString(), QString("accepted"));
        QCOMPARE(accepted.value("receipt").toObject().value("appliedSeq").toString(), QString("3"));
        QVERIFY(completed.value("result").toObject().value("active").toObject().value("maxDisplacementErrorM").toDouble() <= 1e-8);
        widget.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(widget, "runtimeStatus")->text().startsWith("Accepted at sequence 3"), 15000);
        const QString evidence = qEnvironmentVariable("RANGE_RUNTIME_GUI_EVIDENCE_DIRECTORY");
        if (!evidence.isEmpty()) { QVERIFY(QDir().mkpath(evidence)); QVERIFY(widget.grab().save(evidence + "/runtime-agent-native-accepted.png")); }
        runner.cancel(); process.stop(); QTRY_VERIFY(process.isStopped());
#else
        QFAIL("Actual native runtime paths must be configured by CMake");
#endif
    }
#endif
};

int runRuntimeControlWidgetTests(int argc, char **argv)
{
    RuntimeControlWidgetTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_runtime_control_widget.moc"
