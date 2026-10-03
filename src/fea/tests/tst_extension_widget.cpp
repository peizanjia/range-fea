#include "extension_widget.h"

#include "application.h"
#include "session.h"
#include "workflow_process.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QTemporaryDir>
#include <QTest>

#include <rml_model.h>

#include <functional>

namespace
{
bool spinUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        if (predicate()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
    return predicate();
}

bool requestWorkflow(WorkflowProcess *workflow, const QString &operation,
                     const QJsonObject &parameters, QJsonObject *result, QString *error,
                     int timeoutMs = 30000)
{
    if (!workflow || !workflow->isReady() || !result)
    {
        if (error) *error = QStringLiteral("Workflow service is not ready.");
        return false;
    }
    QSignalSpy replies(workflow, &WorkflowProcess::replyReceived);
    QSignalSpy failures(workflow, &WorkflowProcess::requestFailed);
    const QString requestId = workflow->request(operation, parameters);
    if (requestId.isEmpty())
    {
        if (error) *error = QStringLiteral("Could not send workflow request %1.").arg(operation);
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        for (const QList<QVariant> &arguments : replies)
        {
            if (arguments.size() < 3 || arguments.at(0).toString() != requestId) continue;
            if (arguments.at(1).toString() != operation)
            {
                if (error) *error = QStringLiteral("Reply operation did not match %1.").arg(operation);
                return false;
            }
            *result = arguments.at(2).toJsonObject();
            return true;
        }
        for (const QList<QVariant> &arguments : failures)
        {
            if (arguments.size() < 4 || arguments.at(0).toString() != requestId) continue;
            if (error) *error = QStringLiteral("%1: %2 (%3)").arg(operation,
                arguments.at(3).toString(), arguments.at(2).toString());
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(10);
    }
    if (error) *error = QStringLiteral("Timed out waiting for workflow operation %1.").arg(operation);
    return false;
}

bool runApprovedServiceCommand(WorkflowProcess *workflow, const QString &command,
                               const QJsonObject &parameters, QJsonObject *jobResult,
                               QString *error)
{
    QJsonObject project;
    if (!requestWorkflow(workflow, QStringLiteral("project.query"), {}, &project, error)) return false;
    const QJsonValue revision = project.value(QStringLiteral("revision"));
    if (!revision.isDouble())
    {
        if (error) *error = QStringLiteral("project.query did not return a revision.");
        return false;
    }

    QJsonObject preview;
    if (!requestWorkflow(workflow, QStringLiteral("command.preview"),
        {{QStringLiteral("command"), command}, {QStringLiteral("parameters"), parameters},
         {QStringLiteral("expectedRevision"), revision}}, &preview, error)) return false;
    const QString previewId = preview.value(QStringLiteral("previewId")).toString();
    const QString previewHash = preview.value(QStringLiteral("previewHash")).toString();
    if (previewId.isEmpty() || previewHash.isEmpty())
    {
        if (error) *error = QStringLiteral("The service did not return a complete preview for %1.").arg(command);
        return false;
    }

    QJsonObject approval;
    if (!requestWorkflow(workflow, QStringLiteral("host.approve"),
        {{QStringLiteral("previewId"), previewId}, {QStringLiteral("previewHash"), previewHash}},
        &approval, error)) return false;
    const QString approvalToken = approval.value(QStringLiteral("approvalToken")).toString();
    if (approvalToken.isEmpty())
    {
        if (error) *error = QStringLiteral("The service did not return an approval token for %1.").arg(command);
        return false;
    }

    QJsonObject execution;
    if (!requestWorkflow(workflow, QStringLiteral("command.execute"),
        {{QStringLiteral("previewId"), previewId}, {QStringLiteral("approvalToken"), approvalToken}},
        &execution, error)) return false;
    const QString jobId = execution.value(QStringLiteral("jobId")).toString();
    if (jobId.isEmpty())
    {
        if (error) *error = QStringLiteral("The service did not return a job identifier for %1.").arg(command);
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 90000)
    {
        QJsonObject response;
        if (!requestWorkflow(workflow, QStringLiteral("jobs.query"),
            {{QStringLiteral("jobId"), jobId}}, &response, error)) return false;
        const QJsonObject job = response.value(QStringLiteral("job")).toObject();
        const QString state = job.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("succeeded"))
        {
            if (jobResult) *jobResult = job.value(QStringLiteral("result")).toObject();
            return true;
        }
        if (state == QStringLiteral("failed") || state == QStringLiteral("cancelled"))
        {
            if (error) *error = QStringLiteral("%1 job %2: %3")
                .arg(command, state, QJsonDocument(job.value(QStringLiteral("error")).toObject())
                    .toJson(QJsonDocument::Compact));
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QTest::qWait(100);
    }
    if (error) *error = QStringLiteral("Timed out waiting for %1 job %2.").arg(command, jobId);
    return false;
}

QJsonObject manifest(const QString &version)
{
    const QJsonArray form{QJsonObject{{QStringLiteral("id"), QStringLiteral("series")},
        {QStringLiteral("type"), QStringLiteral("numeric-series")}, {QStringLiteral("required"), true}}};
    const QJsonArray columns{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("rms")}, {QStringLiteral("label"), QStringLiteral("RMS")},
                    {QStringLiteral("unit"), QStringLiteral("valueUnit")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("peakAbs")}, {QStringLiteral("label"), QStringLiteral("Absolute peak")},
                    {QStringLiteral("unit"), QStringLiteral("valueUnit")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("peakTime")}, {QStringLiteral("label"), QStringLiteral("Peak time")},
                    {QStringLiteral("unit"), QStringLiteral("timeUnit")}}};
    return {{QStringLiteral("manifestVersion"), 1}, {QStringLiteral("extensionId"), QStringLiteral("range-fea.gui-test")},
        {QStringLiteral("version"), version}, {QStringLiteral("sdkVersion"), QStringLiteral("range.extension/v1")},
        {QStringLiteral("kind"), QStringLiteral("numeric-series-statistics")},
        {QStringLiteral("entryPoint"), QStringLiteral("extension.py:process")},
        {QStringLiteral("dependencies"), QJsonArray{}},
        {QStringLiteral("inputSchema"), QStringLiteral("range.extension/v1/numeric-series")},
        {QStringLiteral("outputSchema"), QStringLiteral("range.extension/v1/statistics")},
        {QStringLiteral("gui"), QJsonObject{{QStringLiteral("form"), form},
            {QStringLiteral("table"), QJsonObject{{QStringLiteral("columns"), columns}}}}}};
}

QString extensionSource(bool changed)
{
    return QStringLiteral(
        "import math\n"
        "%1"
        "def process(series):\n"
        "    values = series['values']\n"
        "    peak = max(abs(value) for value in values)\n"
        "    peak_index = next(index for index, value in enumerate(values) if abs(value) == peak)\n"
        "    rms = math.sqrt(sum(value * value for value in values) / len(values))\n"
        "    return {'rms': rms, 'peakAbs': peak, 'peakTime': series['times'][peak_index],\n"
        "            'timeUnit': series['timeUnit'], 'valueUnit': series['valueUnit']}\n")
        .arg(changed ? QStringLiteral("# version two source change for the service-computed diff\n") : QString());
}

QJsonObject extensionTests()
{
    const QJsonObject input{{QStringLiteral("times"), QJsonArray{0.0, 0.5, 1.0}},
        {QStringLiteral("values"), QJsonArray{3.0, 3.0, 3.0}},
        {QStringLiteral("timeUnit"), QStringLiteral("s")}, {QStringLiteral("valueUnit"), QStringLiteral("m")}};
    const QJsonObject expected{{QStringLiteral("rms"), 3.0}, {QStringLiteral("peakAbs"), 3.0},
        {QStringLiteral("peakTime"), 0.0}, {QStringLiteral("timeUnit"), QStringLiteral("s")},
        {QStringLiteral("valueUnit"), QStringLiteral("m")}};
    return {{QStringLiteral("cases"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("constant-series")},
        {QStringLiteral("input"), input}, {QStringLiteral("expected"), expected}}}}};
}

QJsonObject stageCandidate(WorkflowProcess *workflow, const QString &version, bool changed,
                           QString *error)
{
    QJsonObject result;
    const QJsonObject parameters{{QStringLiteral("manifest"), manifest(version)},
        {QStringLiteral("source"), extensionSource(changed)}, {QStringLiteral("tests"), extensionTests()}};
    if (!runApprovedServiceCommand(workflow, QStringLiteral("extension.stage"), parameters, &result, error)) return {};
    return result;
}

QTreeWidgetItem *candidateItem(QTreeWidget *tree, const QString &version, const QString &state = {})
{
    if (!tree) return nullptr;
    QTreeWidgetItemIterator it(tree);
    while (*it)
    {
        QTreeWidgetItem *item = *it;
        if (item->text(0) == QStringLiteral("Candidate v%1").arg(version) &&
            (state.isEmpty() || item->text(1) == state)) return item;
        ++it;
    }
    return nullptr;
}

QTreeWidgetItem *versionItem(QTreeWidget *tree, const QString &version, const QString &state)
{
    if (!tree) return nullptr;
    QTreeWidgetItemIterator it(tree);
    while (*it)
    {
        QTreeWidgetItem *item = *it;
        if (item->text(0) == QStringLiteral("Version %1").arg(version) && item->text(1) == state) return item;
        ++it;
    }
    return nullptr;
}

bool previewAndApproveWidgetAction(ExtensionWidget *widget, const QString &buttonName,
                                   const QString &expectedCommand, const QString &expectedResult,
                                   QString *error)
{
    auto *action = widget ? widget->findChild<QPushButton *>(buttonName) : nullptr;
    auto *approve = widget ? widget->findChild<QPushButton *>(QStringLiteral("extensionApprove")) : nullptr;
    auto *preview = widget ? widget->findChild<QPlainTextEdit *>(QStringLiteral("extensionPreview")) : nullptr;
    auto *result = widget ? widget->findChild<QPlainTextEdit *>(QStringLiteral("extensionResult")) : nullptr;
    if (!action || !approve || !preview || !result)
    {
        if (error) *error = QStringLiteral("The %1 GUI action is unavailable.").arg(expectedCommand);
        return false;
    }
    if (!spinUntil([action] { return action->isEnabled(); }, 15000))
    {
        if (error) *error = QStringLiteral("The %1 GUI action did not become available.").arg(expectedCommand);
        return false;
    }
    result->clear();
    action->click();
    if (!spinUntil([approve] { return approve->isEnabled(); }, 30000))
    {
        if (error) *error = QStringLiteral("The GUI did not produce an approval preview for %1.").arg(expectedCommand);
        return false;
    }
    const QJsonDocument previewDocument = QJsonDocument::fromJson(preview->toPlainText().toUtf8());
    const QString previewCommand = previewDocument.object().value(QStringLiteral("normalized")).toObject()
        .value(QStringLiteral("command")).toString();
    if (previewCommand != expectedCommand)
    {
        if (error) *error = QStringLiteral("Expected preview for %1 but got %2.").arg(expectedCommand, previewCommand);
        return false;
    }
    approve->click();
    if (!spinUntil([result, &expectedResult] { return result->toPlainText().contains(expectedResult); }, 90000))
    {
        if (error) *error = QStringLiteral("The GUI job result did not contain %1. Last result: %2")
            .arg(expectedResult, result->toPlainText());
        return false;
    }
    return true;
}

Model draftModel()
{
    Model model;
    model.setName(QStringLiteral("Unsaved extension GUI test draft"));
    model.setNNodes(2);
    model.setNode(0, RNode(0.0, 0.0, 0.0));
    model.setNode(1, RNode(1.0, 0.0, 0.0));
    model.setNElements(1);
    RElement element(R_ELEMENT_TRUSS1);
    element.setNodeId(0, 0);
    element.setNodeId(1, 1);
    model.setElement(0, element);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    model.getProblemSetup().getModalSetup().setMethod(R_MODAL_MULTIPLE_MODES);
    model.getProblemSetup().getModalSetup().setNIterations(44);
    model.getProblemSetup().getModalSetup().setNModesToExtract(2);
    return model;
}
}

class TestExtensionWidget : public QObject
{
    Q_OBJECT
private slots:
    void numericSeriesParserRejectsInvalidAndOversizedInput()
    {
        QJsonObject series;
        QString error;
        QVERIFY2(ExtensionWidget::parseNumericSeries(QStringLiteral("0, .5, 1"), QStringLiteral("1, 2, 3"),
            QStringLiteral("s"), QStringLiteral("m/s"), &series, &error), qPrintable(error));
        QCOMPARE(series.value(QStringLiteral("times")).toArray().size(), 3);
        QCOMPARE(series.value(QStringLiteral("values")).toArray().at(1).toDouble(), 2.0);
        QCOMPARE(series.value(QStringLiteral("timeUnit")).toString(), QStringLiteral("s"));

        QVERIFY(!ExtensionWidget::parseNumericSeries(QStringLiteral("0, 1"), QStringLiteral("1"),
            QStringLiteral("s"), QStringLiteral("m"), &series, &error));
        QVERIFY(!ExtensionWidget::parseNumericSeries(QStringLiteral("0, 1, 1"), QStringLiteral("1, 2, 3"),
            QStringLiteral("s"), QStringLiteral("m"), &series, &error));
        QVERIFY(!ExtensionWidget::parseNumericSeries(QStringLiteral("0, nan"), QStringLiteral("1, 2"),
            QStringLiteral("s"), QStringLiteral("m"), &series, &error));
        QVERIFY(!ExtensionWidget::parseNumericSeries(QStringLiteral("0, 1"), QStringLiteral("1, 2"),
            QStringLiteral("s!"), QStringLiteral("m"), &series, &error));

        QString manySamples;
        for (int i = 0; i < 513; ++i)
        {
            if (!manySamples.isEmpty()) manySamples += QLatin1Char(',');
            manySamples += QString::number(i);
        }
        QVERIFY(!ExtensionWidget::parseNumericSeries(manySamples, manySamples,
            QStringLiteral("s"), QStringLiteral("m"), &series, &error));
    }

    void widgetDoesNotOwnSharedWorkflowProcess()
    {
        QPointer<WorkflowProcess> workflow = new WorkflowProcess;
        auto *widget = new ExtensionWidget(workflow);
        QVERIFY(workflow->parent() == nullptr);
        QCOMPARE(widget->objectName(), QStringLiteral("extensionWidget"));
        QVERIFY(!widget->findChild<QPushButton *>(QStringLiteral("extensionValidate"))->isEnabled());
        delete widget;
        QVERIFY(!workflow.isNull());
        delete workflow.data();
    }

    void trustedServiceLifecycleHotRefreshAndPreservesDraft()
    {
        auto *session = Application::instance()->getSession();
        session->clear();
        session->addModel(draftModel());
        session->setModelSelected(0, true);
        const auto originalDocument = session->modelCommandStamp(0);
        QVERIFY(!originalDocument.first.isEmpty());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 44u);

        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        WorkflowProcess workflow;
        ExtensionWidget widget(&workflow);
        const auto config = [&workspace] {
            auto value = WorkflowProcess::defaultConfiguration(workspace.path());
            value.trustedExtensions = true;
            value.requestTimeoutMs = 60000;
            return value;
        }();
        workflow.start(config);
        QVERIFY2(spinUntil([&workflow] { return workflow.isReady(); }, 20000),
                 "Trusted workflow service did not start from the packaged/source runtime.");

        QJsonObject capabilities;
        QString error;
        QVERIFY2(requestWorkflow(&workflow, QStringLiteral("capabilities.query"), {}, &capabilities, &error), qPrintable(error));
        const QJsonObject runtime = capabilities.value(QStringLiteral("extensionRuntime")).toObject();
        QVERIFY(runtime.value(QStringLiteral("trustedExtensions")).toBool());
        QCOMPARE(runtime.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("range.extension/v1"));
        QCOMPARE(runtime.value(QStringLiteral("osSandbox")).toBool(), false);
        auto *trustLabel = widget.findChild<QLabel *>(QStringLiteral("extensionTrustStatus"));
        QVERIFY(trustLabel);
        QVERIFY2(spinUntil([trustLabel] { return trustLabel->text().contains(QStringLiteral("trusted developer mode")); }, 10000),
                 "ExtensionWidget did not display the service-reported trusted runtime capability.");

        const QJsonObject firstCandidate = stageCandidate(&workflow, QStringLiteral("0.1.0"), false, &error);
        QVERIFY2(!firstCandidate.isEmpty(), qPrintable(error));
        const QString firstCandidateId = firstCandidate.value(QStringLiteral("candidateId")).toString();
        const QString firstBundleHash = firstCandidate.value(QStringLiteral("bundleHash")).toString();
        QVERIFY(!firstCandidateId.isEmpty());
        QVERIFY(!firstBundleHash.isEmpty());

        auto *tree = widget.findChild<QTreeWidget *>(QStringLiteral("extensionList"));
        QVERIFY(tree);
        QVERIFY2(spinUntil([tree] { return candidateItem(tree, QStringLiteral("0.1.0"), QStringLiteral("staged")) != nullptr; }, 15000),
                 "The open widget did not hot-refresh the newly staged candidate.");
        QTreeWidgetItem *firstItem = candidateItem(tree, QStringLiteral("0.1.0"), QStringLiteral("staged"));
        tree->setCurrentItem(firstItem);
        auto *sourceView = widget.findChild<QPlainTextEdit *>(QStringLiteral("extensionSource"));
        QVERIFY(sourceView);
        QVERIFY2(spinUntil([sourceView] { return sourceView->toPlainText().contains(QStringLiteral("def process")); }, 10000),
                 "The GUI did not load candidate source from the service.");

        QJsonObject candidateBeforeApproval;
        QVERIFY2(requestWorkflow(&workflow, QStringLiteral("extension.candidate.get"),
            {{QStringLiteral("candidateId"), firstCandidateId}}, &candidateBeforeApproval, &error), qPrintable(error));
        QCOMPARE(candidateBeforeApproval.value(QStringLiteral("candidate")).toObject()
            .value(QStringLiteral("state")).toString(), QStringLiteral("staged"));
        QVERIFY2(previewAndApproveWidgetAction(&widget, QStringLiteral("extensionValidate"),
            QStringLiteral("extension.validate"), QStringLiteral("validated"), &error), qPrintable(error));
        QVERIFY2(spinUntil([tree] { return candidateItem(tree, QStringLiteral("0.1.0"), QStringLiteral("validated")) != nullptr; }, 15000),
                 "The validated candidate state did not hot-refresh in the widget.");
        QVERIFY2(spinUntil([tree] {
            const auto *validated = candidateItem(tree, QStringLiteral("0.1.0"), QStringLiteral("validated"));
            return validated && tree->currentItem() == validated;
        }, 15000), "The selected candidate was not restored after its registry details refreshed.");
        QVERIFY2(previewAndApproveWidgetAction(&widget, QStringLiteral("extensionActivate"),
            QStringLiteral("extension.activate"), QStringLiteral("active"), &error), qPrintable(error));
        QVERIFY2(spinUntil([tree] { return versionItem(tree, QStringLiteral("0.1.0"), QStringLiteral("active")) != nullptr; }, 15000),
                 "The activated extension version did not hot-refresh in the widget.");

        QTreeWidgetItem *activeVersion = versionItem(tree, QStringLiteral("0.1.0"), QStringLiteral("active"));
        tree->setCurrentItem(activeVersion);
        auto *times = widget.findChild<QPlainTextEdit *>(QStringLiteral("extensionSeriesTimes"));
        auto *values = widget.findChild<QPlainTextEdit *>(QStringLiteral("extensionSeriesValues"));
        auto *timeUnit = widget.findChild<QComboBox *>(QStringLiteral("extensionTimeUnit"));
        auto *valueUnit = widget.findChild<QComboBox *>(QStringLiteral("extensionValueUnit"));
        auto *runButton = widget.findChild<QPushButton *>(QStringLiteral("extensionRun"));
        QVERIFY(times && values && timeUnit && valueUnit && runButton);
        times->setPlainText(QStringLiteral("0, 0.5, 1"));
        values->setPlainText(QStringLiteral("3, 3, 3"));
        timeUnit->setCurrentText(QStringLiteral("s"));
        valueUnit->setCurrentText(QStringLiteral("m"));
        QVERIFY2(spinUntil([runButton] { return runButton->isEnabled(); }, 10000),
                 "The declared numeric-series form did not become available for the active version.");
        QVERIFY2(previewAndApproveWidgetAction(&widget, QStringLiteral("extensionRun"),
            QStringLiteral("extension.run"), QStringLiteral("metrics"), &error), qPrintable(error));
        auto *metrics = widget.findChild<QTableWidget *>(QStringLiteral("extensionMetrics"));
        QVERIFY(metrics);
        QCOMPARE(metrics->rowCount(), 3);
        QCOMPARE(metrics->item(0, 1)->text(), QStringLiteral("3"));
        QCOMPARE(metrics->item(0, 2)->text(), QStringLiteral("m"));
        QCOMPARE(metrics->item(2, 2)->text(), QStringLiteral("s"));
        QTest::qWait(2300);
        QCOMPARE(times->toPlainText(), QStringLiteral("0, 0.5, 1"));
        QCOMPARE(values->toPlainText(), QStringLiteral("3, 3, 3"));
        QCOMPARE(metrics->item(0, 1)->text(), QStringLiteral("3"));

        QJsonObject configurationResult;
        QVERIFY2(runApprovedServiceCommand(&workflow, QStringLiteral("workflow.configure"),
            {{QStringLiteral("model"), QStringLiteral("four-rod")}, {QStringLiteral("dt"), 0.005},
             {QStringLiteral("steps"), 20}, {QStringLiteral("targetDamping"), 0.1}},
            &configurationResult, &error), qPrintable(error));
        QJsonObject referenceResult;
        QVERIFY2(runApprovedServiceCommand(&workflow, QStringLiteral("workflow.run"),
            {{QStringLiteral("mode"), QStringLiteral("both")}, {QStringLiteral("independentReference"), true}},
            &referenceResult, &error), qPrintable(error));
        const QString referenceJobId = referenceResult.value(QStringLiteral("jobId")).toString();
        QVERIFY(!referenceJobId.isEmpty());
        auto *sourceMode = widget.findChild<QComboBox *>(QStringLiteral("extensionSourceMode"));
        auto *sourceJob = widget.findChild<QComboBox *>(QStringLiteral("extensionSourceJob"));
        auto *approve = widget.findChild<QPushButton *>(QStringLiteral("extensionApprove"));
        auto *preview = widget.findChild<QPlainTextEdit *>(QStringLiteral("extensionPreview"));
        auto *result = widget.findChild<QPlainTextEdit *>(QStringLiteral("extensionResult"));
        QVERIFY(sourceMode && sourceJob && approve && preview && result);
        QVERIFY2(spinUntil([sourceJob, &referenceJobId] { return sourceJob->findData(referenceJobId) >= 0; }, 15000),
                 "The completed independent workflow job did not appear as a feature input.");
        sourceMode->setCurrentIndex(sourceMode->findData(QStringLiteral("provenance")));
        sourceJob->setCurrentIndex(sourceJob->findData(referenceJobId));
        result->clear();
        runButton->click();
        QVERIFY2(spinUntil([approve] { return approve->isEnabled(); }, 15000),
                 "The provenance-based extension run did not create a pending preview.");
        auto previewObject = QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object();
        QCOMPARE(previewObject.value(QStringLiteral("normalized")).toObject()
            .value(QStringLiteral("parameters")).toObject().value(QStringLiteral("provenance")).toObject()
            .value(QStringLiteral("jobId")).toString(), referenceJobId);
        QTest::qWait(2300);
        QVERIFY2(approve->isEnabled(), "An unchanged source-job refresh cleared the pending provenance preview.");
        previewObject = QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object();
        QCOMPARE(previewObject.value(QStringLiteral("normalized")).toObject()
            .value(QStringLiteral("parameters")).toObject().value(QStringLiteral("provenance")).toObject()
            .value(QStringLiteral("jobId")).toString(), referenceJobId);
        approve->click();
        QVERIFY2(spinUntil([result] { return result->toPlainText().contains(QStringLiteral("reference.active.sensorVelocity")); }, 90000),
                 "The approved provenance run did not report its selected reference signal.");

        const QJsonObject secondCandidate = stageCandidate(&workflow, QStringLiteral("0.2.0"), true, &error);
        QVERIFY2(!secondCandidate.isEmpty(), qPrintable(error));
        const QString secondBundleHash = secondCandidate.value(QStringLiteral("bundleHash")).toString();
        QVERIFY(!secondBundleHash.isEmpty());
        QVERIFY2(spinUntil([tree] { return candidateItem(tree, QStringLiteral("0.2.0"), QStringLiteral("staged")) != nullptr; }, 15000),
                 "The widget did not hot-refresh a second staged version.");
        QTreeWidgetItem *secondItem = candidateItem(tree, QStringLiteral("0.2.0"), QStringLiteral("staged"));
        tree->setCurrentItem(secondItem);
        auto *diffView = widget.findChild<QPlainTextEdit *>(QStringLiteral("extensionDiff"));
        QVERIFY(diffView);
        QVERIFY2(spinUntil([diffView] { return diffView->toPlainText().contains(QStringLiteral("version two source change")); }, 10000),
                 "The GUI did not display the service-computed source diff against the active version.");
        QVERIFY2(previewAndApproveWidgetAction(&widget, QStringLiteral("extensionValidate"),
            QStringLiteral("extension.validate"), QStringLiteral("validated"), &error), qPrintable(error));
        QVERIFY2(spinUntil([tree] { return candidateItem(tree, QStringLiteral("0.2.0"), QStringLiteral("validated")) != nullptr; }, 15000),
                 "The second candidate validation state did not refresh.");
        QVERIFY2(previewAndApproveWidgetAction(&widget, QStringLiteral("extensionActivate"),
            QStringLiteral("extension.activate"), QStringLiteral("active"), &error), qPrintable(error));
        QVERIFY2(spinUntil([tree] { return versionItem(tree, QStringLiteral("0.2.0"), QStringLiteral("active")) != nullptr; }, 15000),
                 "The second version did not become active in the widget.");
        QCOMPARE(times->toPlainText(), QStringLiteral("0, 0.5, 1"));
        QCOMPARE(values->toPlainText(), QStringLiteral("3, 3, 3"));

        auto *rollbackTarget = widget.findChild<QComboBox *>(QStringLiteral("extensionRollbackTarget"));
        auto *rollbackButton = widget.findChild<QPushButton *>(QStringLiteral("extensionRollback"));
        QVERIFY(rollbackTarget && rollbackButton);
        QTreeWidgetItem *extensionRoot = tree->topLevelItem(0);
        QVERIFY(extensionRoot);
        tree->setCurrentItem(extensionRoot);
        QVERIFY2(spinUntil([rollbackTarget] { return rollbackTarget->count() == 1; }, 10000),
                 "The retained first version was not offered as a rollback target.");
        QCOMPARE(rollbackTarget->currentData().toString(), firstBundleHash);
        QVERIFY2(spinUntil([rollbackButton] { return rollbackButton->isEnabled(); }, 5000),
                 "Rollback was unavailable after a second version became active.");
        QVERIFY2(previewAndApproveWidgetAction(&widget, QStringLiteral("extensionRollback"),
            QStringLiteral("extension.rollback"), firstBundleHash.left(12), &error), qPrintable(error));
        QVERIFY2(spinUntil([tree] { return versionItem(tree, QStringLiteral("0.1.0"), QStringLiteral("active")) != nullptr &&
            versionItem(tree, QStringLiteral("0.2.0"), QStringLiteral("retained")) != nullptr; }, 15000),
                 "Rollback did not restore the first version and retain the newer version.");
        QCOMPARE(times->toPlainText(), QStringLiteral("0, 0.5, 1"));
        QCOMPARE(values->toPlainText(), QStringLiteral("3, 3, 3"));

        QCOMPARE(session->modelCommandStamp(0).first, originalDocument.first);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 44u);
        session->clear();
        workflow.stop();
        QVERIFY2(spinUntil([&workflow] { return workflow.isStopped(); }, 5000),
                 "The workflow process did not stop before its temporary workspace was removed.");
    }
};

int runExtensionWidgetTests(int argc, char **argv)
{
    TestExtensionWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_extension_widget.moc"
