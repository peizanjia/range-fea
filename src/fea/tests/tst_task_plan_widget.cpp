#include <QtTest>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <cmath>
#include "task_plan_widget.h"
#include "workflow_process.h"

// Real GUI/service/native acceptance. The bootstrap is deliberately a separate
// process: its SDK authority cannot approve any node in the new GUI session.
class TaskPlanWidgetTests : public QObject
{
    Q_OBJECT
    QTemporaryDir fixture;
    WorkflowProcess::Configuration config;
    QJsonObject source;
    QString error;

    template<class T> T *control(TaskPlanWidget &widget, const char *name)
    { return widget.findChild<T *>(QString::fromLatin1(name)); }
    QTreeWidgetItem *node(TaskPlanWidget &widget, const QString &id)
    {
        auto *tree = control<QTreeWidget>(widget, "taskPlanTree");
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == id) return tree->topLevelItem(i);
        return nullptr;
    }
    QString state(TaskPlanWidget &widget, const QString &id)
    { auto *item = node(widget, id); return item ? item->data(2, Qt::UserRole).toString() : QString(); }
    QJsonObject request(WorkflowProcess &process, const QString &operation, const QJsonObject &parameters = {})
    {
        QSignalSpy replies(&process, &WorkflowProcess::replyReceived);
        QSignalSpy failures(&process, &WorkflowProcess::requestFailed);
        const auto id = process.request(operation, parameters);
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 30000) {
            for (const auto &reply : replies)
                if (reply[0].toString() == id) return reply[2].toJsonObject();
            for (const auto &failure : failures)
                if (failure[0].toString() == id) {
                    error = failure[2].toString() + ": " + failure[3].toString(); return {};
                }
            QTest::qWait(20);
        }
        error = operation + " timed out"; return {};
    }
    void click(TaskPlanWidget &widget, const char *name)
    { QTest::mouseClick(control<QPushButton>(widget, name), Qt::LeftButton); }
    void screenshot(TaskPlanWidget &widget, const QString &name)
    {
        const QString directory = qEnvironmentVariable("RANGE_TASK_PLAN_EVIDENCE_DIRECTORY");
        if (directory.isEmpty()) return;
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(widget.grab().save(QDir(directory).filePath(name + ".png")));
    }
private slots:
    void initTestCase()
    {
        QVERIFY(fixture.isValid());
        config = WorkflowProcess::defaultConfiguration(fixture.path() + "/workspace");
#ifdef RANGE_TASK_WIDGET_PYTHON
        config.python = QString::fromUtf8(RANGE_TASK_WIDGET_PYTHON);
#endif
#ifdef RANGE_TASK_WIDGET_PACKAGE
        config.packageDirectory = QString::fromUtf8(RANGE_TASK_WIDGET_PACKAGE);
#endif
#ifdef RANGE_TASK_WIDGET_SOLVER
        config.solver = QString::fromUtf8(RANGE_TASK_WIDGET_SOLVER);
#endif
#ifdef RANGE_TASK_WIDGET_NATIVE
        config.native = QString::fromUtf8(RANGE_TASK_WIDGET_NATIVE);
#endif
#ifdef RANGE_TASK_WIDGET_EXAMPLES
        config.examples = QString::fromUtf8(RANGE_TASK_WIDGET_EXAMPLES);
#endif
        for (const auto &file : {config.python, config.solver, config.native})
            QVERIFY2(QFileInfo(file).isFile(), qPrintable("Required executable missing: " + file));
        QVERIFY(QFileInfo(config.examples + "/stage9_truss4.ram").isFile());
        const QString helper = QDir(config.packageDirectory).absoluteFilePath("../tests/task_widget_bootstrap.py");
        QVERIFY2(QFileInfo(helper).isFile(), qPrintable(helper));
        QProcess bootstrap;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PYTHONPATH", config.packageDirectory);
        environment.insert("PYTHONNOUSERSITE", "1");
        bootstrap.setProcessEnvironment(environment);
        bootstrap.start(config.python, {helper, "--root", fixture.path(), "--solver", config.solver,
            "--native", config.native, "--examples", config.examples, "--python", config.python});
        QVERIFY(bootstrap.waitForStarted(10000));
        QVERIFY(bootstrap.waitForFinished(120000));
        const QByteArray diagnostics = bootstrap.readAllStandardError();
        QCOMPARE(bootstrap.exitStatus(), QProcess::NormalExit);
        QVERIFY2(bootstrap.exitCode() == 0, diagnostics.constData());
        QFile manifest(fixture.path() + "/fixture.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        source = QJsonDocument::fromJson(manifest.readAll()).object();
        QVERIFY(!source.value("sourceId").toString().isEmpty());
    }
    void visibleScanCompletesWithSeparateApprovalsAndNativeEvidence()
    {
        WorkflowProcess process;
        TaskPlanWidget widget(&process); widget.resize(1200, 720); widget.show();
        process.start(config); QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 30000);
        auto *tree = control<QTreeWidget>(widget, "taskPlanTree");
        auto *choice = control<QComboBox>(widget, "taskPlanChoice");
        auto *approve = control<QPushButton>(widget, "taskPlanApproveNode");
        control<QSpinBox>(widget, "taskScanSteps")->setValue(8);
        click(widget, "taskPlanCreateScan");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 6, 30000);
        QVERIFY(control<QLabel>(widget, "taskPlanStatus")->text().contains("Ready to start"));
        QVERIFY(!approve->isEnabled());
        const QString planId = choice->currentData().toString();
        QVERIFY(!planId.isEmpty());
        auto staged = request(process, "task.plan.query", {{"planId", planId}});
        QCOMPARE(staged.value("counts").toObject().value("toolCalls").toInt(), 0);
        for (const auto &entry : staged.value("nodes").toArray()) QVERIFY(entry.toObject().value("jobId").isUndefined());
        screenshot(widget, "task-plan-staged");
        click(widget, "taskPlanstart");
        const QStringList approvalOrder{"point1Configure", "point1Run", "point2Configure", "point2Run"};
        for (const auto &id : approvalOrder) {
            QTRY_COMPARE_WITH_TIMEOUT(state(widget, id), QString("awaiting_approval"), 90000);
            tree->setCurrentItem(node(widget, id));
            QTRY_VERIFY(approve->isEnabled());
            const auto preview = control<QPlainTextEdit>(widget, "taskPlanNodePreview")->toPlainText();
            QVERIFY(preview.contains(source.value("sourceId").toString()));
            QVERIFY(preview.contains("Time step")); QVERIFY(preview.contains("Accepted steps"));
            QVERIFY(preview.contains("Target damping ratio"));
            QVERIFY(preview.contains("[1]"));
            // Refresh must preserve both selected node and the next-scan draft.
            control<QDoubleSpinBox>(widget, "taskScanFirstDamping")->setValue(.123);
            control<QSpinBox>(widget, "taskScanSteps")->setValue(4);
            QSignalSpy replies(&process, &WorkflowProcess::replyReceived);
            click(widget, "taskPlanRefresh");
            QTRY_VERIFY_WITH_TIMEOUT(!replies.isEmpty(), 30000);
            QCOMPARE(tree->currentItem()->text(0), id);
            QCOMPARE(choice->currentData().toString(), planId);
            QCOMPARE(control<QSpinBox>(widget, "taskScanSteps")->value(), 4);
            QCOMPARE(control<QDoubleSpinBox>(widget, "taskScanFirstDamping")->value(), .123);
            screenshot(widget, "task-plan-preview-" + id);
            QTest::mouseClick(approve, Qt::LeftButton);
            QVERIFY(!approve->isEnabled());
            QTRY_COMPARE_WITH_TIMEOUT(state(widget, id), QString("completed"), 90000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(state(widget, "point2Verify"), QString("completed"), 90000);
        auto completed = request(process, "task.plan.query", {{"planId", planId}});
        QVERIFY2(!completed.isEmpty(), qPrintable(error));
        QCOMPARE(completed.value("state").toString(), QString("completed"));
        QCOMPARE(completed.value("counts").toObject().value("modelCalls").toInt(), 0);
        for (int point = 1; point <= 2; ++point) {
            const auto runId = QString("point%1Run").arg(point);
            const auto jobId = node(widget, runId)->text(4); QVERIFY(!jobId.isEmpty());
            auto job = request(process, "jobs.query", {{"jobId", jobId}}).value("job").toObject();
            QCOMPARE(job.value("state").toString(), QString("succeeded"));
            QCOMPARE(job.value("taskPlanRef").toObject().value("planId").toString(), planId);
            QCOMPARE(job.value("taskPlanRef").toObject().value("nodeId").toString(), runId);
            QCOMPARE(job.value("configuration").toObject().value("steps").toInt(), 8);
            QCOMPARE(job.value("configuration").toObject().value("targetDamping").toDouble(), point == 1 ? .05 : .10);
            for (const auto &variant : {QString("active"), QString("passive")}) {
                const auto result = job.value("result").toObject().value(variant).toObject();
                QCOMPARE(result.value("acceptedSeq").toInt(), 8);
                const QStringList keys{"maxDisplacementErrorM", "maxVelocityErrorMPerSecond", "maxRotationErrorRad", "maxAngularVelocityErrorRadPerSecond"};
                for (int index = 0; index < keys.size(); ++index) {
                    QVERIFY(result.value(keys[index]).isDouble()); const double value = result.value(keys[index]).toDouble();
                    QVERIFY(std::isfinite(value) && value >= 0 && value <= (index % 2 ? 1e-7 : 1e-8));
                }
                auto catalog = request(process, "result.catalog.query", {{"jobId", jobId}, {"mode", variant}, {"limit", 1}});
                QCOMPARE(catalog.value("sourceKind").toString(), "native-" + variant + "-fe");
                QCOMPARE(catalog.value("sourceSnapshotRef").toObject().value("modelSha256"), source.value("modelSha256"));
            }
        }
        screenshot(widget, "task-plan-completed");
        process.stop(); QTRY_VERIFY_WITH_TIMEOUT(process.isStopped(), 10000);
    }
    void singleModeRunPreviewsShowOnlySelectedVariant()
    {
        WorkflowProcess process;
        TaskPlanWidget widget(&process); widget.resize(1200, 720); widget.show();
        process.start(config); QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 30000);
        auto *choice = control<QComboBox>(widget, "taskPlanChoice");
        auto *tree = control<QTreeWidget>(widget, "taskPlanTree");
        auto *approve = control<QPushButton>(widget, "taskPlanApproveNode");

        for (const QString &mode : {QString("active"), QString("passive")}) {
            const QJsonObject plan{{"schema", "range.task-plan/v1"},
                {"title", "Single " + mode + " preview"},
                {"nodes", QJsonArray{QJsonObject{{"id", "run"}, {"kind", "command"},
                    {"tool", "workflow.run"}, {"successPredicate", "native.run.verified"},
                    {"parameters", QJsonObject{{"mode", mode}, {"independentReference", true}}}}}}};
            const auto staged = request(process, "task.plan.propose", {{"plan", plan}});
            QVERIFY2(!staged.isEmpty(), qPrintable(error));
            const QString planId = staged.value("planId").toString();
            QVERIFY(!planId.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(choice->findData(planId) >= 0, 30000);
            choice->setCurrentIndex(choice->findData(planId));
            click(widget, "taskPlanstart");
            QTRY_COMPARE_WITH_TIMEOUT(state(widget, "run"), QString("awaiting_approval"), 60000);
            tree->setCurrentItem(node(widget, "run"));
            QTRY_VERIFY_WITH_TIMEOUT(approve->isEnabled(), 30000);

            const QString visible = control<QPlainTextEdit>(widget, "taskPlanNodePreview")->toPlainText();
            const QString expected = mode == "active"
                ? "Run active FE and independently verify its accepted result."
                : "Run passive FE and independently verify its accepted result.";
            const QString other = mode == "active"
                ? "Run passive FE and independently verify its accepted result."
                : "Run active FE and independently verify its accepted result.";
            QVERIFY2(visible.contains(expected), qPrintable(visible));
            QVERIFY2(!visible.contains(other), qPrintable(visible));
            QVERIFY2(!visible.contains("Run active and passive FE and independently verify their accepted results."),
                     qPrintable(visible));

            const auto observed = request(process, "task.plan.query", {{"planId", planId}});
            QCOMPARE(observed.value("state").toString(), QString("awaiting_approval"));
            QCOMPARE(observed.value("counts").toObject().value("toolCalls").toInt(), 1);
            QCOMPARE(observed.value("counts").toObject().value("modelCalls").toInt(), 0);
            const auto nodeState = observed.value("nodes").toArray().first().toObject();
            QCOMPARE(nodeState.value("state").toString(), QString("awaiting_approval"));
            QVERIFY(nodeState.value("jobId").isUndefined());
            QCOMPARE(nodeState.value("parameters").toObject().value("mode").toString(), mode);
            QCOMPARE(nodeState.value("preview").toObject().value("normalized").toObject()
                         .value("parameters").toObject().value("mode").toString(), mode);
            const auto cancelled = request(process, "task.plan.cancel", {{"planId", planId},
                {"expectedPlanSha256", staged.value("planSha256")}});
            QCOMPARE(cancelled.value("state").toString(), QString("cancelled"));
        }

        process.stop(); QTRY_VERIFY_WITH_TIMEOUT(process.isStopped(), 10000);
    }
    void reopenAndCancelKeepAcceptedJobsAndWorkspaceIsolation()
    {
        WorkflowProcess process;
        TaskPlanWidget widget(&process); widget.resize(1200, 720); widget.show();
        process.start(config); QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 30000);
        auto *choice = control<QComboBox>(widget, "taskPlanChoice");
        auto *tree = control<QTreeWidget>(widget, "taskPlanTree");
        auto *approve = control<QPushButton>(widget, "taskPlanApproveNode");
        QTRY_VERIFY_WITH_TIMEOUT(choice->count() >= 1, 30000);
        const int oldCount = choice->count();
        click(widget, "taskPlanCreateScan"); QTRY_COMPARE_WITH_TIMEOUT(choice->count(), oldCount + 1, 30000);
        choice->setCurrentIndex(choice->count() - 1);
        const QString planId = choice->currentData().toString();
        click(widget, "taskPlanstart");
        QTRY_COMPARE_WITH_TIMEOUT(state(widget, "point1Configure"), QString("awaiting_approval"), 30000);
        tree->setCurrentItem(node(widget, "point1Configure")); QVERIFY(approve->isEnabled());
        click(widget, "taskPlanApproveNode");
        QTRY_COMPARE_WITH_TIMEOUT(state(widget, "point1Configure"), QString("completed"), 30000);
        const QString configuredJob = node(widget, "point1Configure")->text(4);
        QVERIFY(!configuredJob.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(state(widget, "point1Run"), QString("awaiting_approval"), 30000);
        tree->setCurrentItem(node(widget, "point1Run")); QVERIFY(approve->isEnabled());
        const auto oldPlan = request(process, "task.plan.query", {{"planId", planId}});
        QJsonObject oldNode;
        for (const auto &entry : oldPlan.value("nodes").toArray())
            if (entry.toObject().value("id") == QJsonValue("point1Run")) oldNode = entry.toObject();
        QVERIFY(!oldNode.value("previewHash").toString().isEmpty());
        click(widget, "taskPlanpause");
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(widget, "taskPlanStatus")->text().contains("Paused"), 30000);
        process.stop(); QTRY_VERIFY_WITH_TIMEOUT(process.isStopped(), 10000);
        QVERIFY(!approve->isEnabled()); QCOMPARE(tree->topLevelItemCount(), 0);
        process.start(config); QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(choice->findData(planId) >= 0, 30000); choice->setCurrentIndex(choice->findData(planId));
        const auto reopened = request(process, "task.plan.query", {{"planId", planId}});
        QCOMPARE(reopened.value("state").toString(), QString("paused"));
        for (const auto &entry : reopened.value("nodes").toArray()) {
            const auto item = entry.toObject();
            QVERIFY(item.value("preview").isUndefined());
            if (item.value("id") == QJsonValue("point1Run")) QVERIFY(item.value("jobId").isUndefined());
        }
        QSignalSpy failed(&process, &WorkflowProcess::requestFailed);
        const auto staleId = process.request("host.task.node.approve", {{"planId", planId},
            {"expectedPlanSha256", oldPlan.value("planSha256")}, {"nodeId", "point1Run"}, {"previewHash", oldNode.value("previewHash")}});
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 30000);
        QCOMPARE(failed.last()[0].toString(), staleId); QCOMPARE(failed.last()[2].toString(), QString("STALE_PLAN"));
        click(widget, "taskPlanresume");
        QTRY_COMPARE_WITH_TIMEOUT(state(widget, "point1Run"), QString("awaiting_approval"), 30000);
        tree->setCurrentItem(node(widget, "point1Run"));
        QTRY_VERIFY_WITH_TIMEOUT(approve->isEnabled(), 30000);
        QVERIFY(control<QPlainTextEdit>(widget, "taskPlanNodePreview")->toPlainText().contains(source.value("sourceId").toString()));
        click(widget, "taskPlanApproveNode");
        QTRY_VERIFY_WITH_TIMEOUT(node(widget, "point1Run") && !node(widget, "point1Run")->text(4).isEmpty(), 30000);
        const QString acceptedJob = node(widget, "point1Run")->text(4);
        click(widget, "taskPlanpause");
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(widget, "taskPlanStatus")->text().contains("Paused"), 30000);
        click(widget, "taskPlancancel");
        QTRY_VERIFY_WITH_TIMEOUT(control<QLabel>(widget, "taskPlanStatus")->text().contains("Cancelled"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(request(process, "jobs.query", {{"jobId", acceptedJob}}).value("job").toObject().value("state").toString(), QString("succeeded"), 90000);
        const auto job = request(process, "jobs.query", {{"jobId", acceptedJob}}).value("job").toObject();
        QCOMPARE(job.value("state").toString(), QString("succeeded"));
        QCOMPARE(job.value("taskPlanRef").toObject().value("planId").toString(), planId);
        const auto cancelled = request(process, "task.plan.query", {{"planId", planId}});
        QCOMPARE(cancelled.value("state").toString(), QString("cancelled"));
        for (const auto &entry : cancelled.value("nodes").toArray())
            if (entry.toObject().value("id") == QJsonValue("point2Configure")) QVERIFY(entry.toObject().value("jobId").isUndefined());
        screenshot(widget, "task-plan-cancelled-independent-job");
        QSignalSpy oldReplies(&process, &WorkflowProcess::replyReceived);
        click(widget, "taskPlanRefresh"); QTRY_VERIFY_WITH_TIMEOUT(!oldReplies.isEmpty(), 30000);
        const auto late = oldReplies.last();
        process.stop(); QTRY_VERIFY_WITH_TIMEOUT(process.isStopped(), 10000);
        auto other = config; other.workspace = fixture.path() + "/other-workspace";
        process.start(other); QTRY_VERIFY_WITH_TIMEOUT(process.isReady(), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(choice->count(), 0, 30000);
        // Replay a genuine old response after the session generation changed.
        process.replyReceived(late[0].toString(), late[1].toString(), late[2].toJsonObject());
        QCOMPARE(choice->count(), 0); QCOMPARE(tree->topLevelItemCount(), 0); QVERIFY(!approve->isEnabled());
        QCOMPARE(control<QPlainTextEdit>(widget, "taskPlanNodePreview")->toPlainText(), QString());
        process.stop(); QTRY_VERIFY_WITH_TIMEOUT(process.isStopped(), 10000);
    }
};

int runTaskPlanWidgetTests(int argc, char **argv)
{
    TaskPlanWidgetTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_task_plan_widget.moc"
