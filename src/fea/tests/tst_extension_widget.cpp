#include "extension_widget.h"

#include "application.h"
#include "session.h"
#include "workflow_process.h"
#include "series_plot_widget.h"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScrollArea>
#include <QTabWidget>

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
#include <QTimer>
#include <QTest>

#include <rml_model.h>

#include <functional>
#include <cmath>

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

void clickVisible(QPushButton *button)
{
    for (QWidget *ancestor = button->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(ancestor)) scroll->ensureWidgetVisible(button);
    QCoreApplication::processEvents();
    QTest::mouseClick(button, Qt::LeftButton);
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
    if (widget->isVisible()) clickVisible(action); else action->click();
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
    if (widget->isVisible()) clickVisible(approve); else approve->click();
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
    void scientificRuntimeRequiredFailureRetiresReadyReceiptAndApproval()
    {
        for(const auto &code:{QString("RUNTIME_VERIFICATION_REQUIRED"),QString("RUNTIME_VERIFICATION_IN_PROGRESS")})
        {
            WorkflowProcess service; ExtensionWidget widget(&service);
            widget.runtimeVerificationReady=true;
            widget.runtimeVerificationGeneration="old-generation";
            widget.runtimeVerificationId="old-receipt";
            widget.runtimeVerificationRequest="old-poll";
            widget.runtimeVerificationTimer->start();
            widget.currentPreview={{"previewId","old-approved-candidate"}};
            widget.commandInFlight=true;
            ExtensionWidget::PendingRequest context{ExtensionWidget::RequestKind::CommandPreview,{}};
            context.operation="command.preview";
            widget.pendingRequests.insert("failed-preview",context);
            widget.onRequestFailed("failed-preview","command.preview",code,"Private environment changed");
            QVERIFY(!widget.runtimeVerificationReady); QVERIFY(!widget.commandInFlight);
            QVERIFY(widget.runtimeVerificationGeneration.isEmpty()); QVERIFY(widget.runtimeVerificationId.isEmpty());
            QVERIFY(widget.runtimeVerificationRequest.isEmpty()); QVERIFY(!widget.runtimeVerificationTimer->isActive());
            QVERIFY(widget.currentPreview.isEmpty()); QVERIFY(!widget.approveButton->isEnabled());
        }
    }
    void scientificRuntimeReceiptRejectsStaleWorkspaceAndGeneration()
    {
        WorkflowProcess service;
        ExtensionWidget widget(&service);
        widget.runtimeVerificationGeneration = "current-generation";
        const QJsonObject receipt{{"verificationId", "receipt"}, {"state", "ready"}, {"isCurrent", true},
            {"runtimeHash", "runtime-hash"}, {"expectedEnvironmentStamp", "stamp"}, {"profile", "packaged"},
            {"verifiedAt", "2026-10-09T00:00:00Z"}, {"fullFileVerify", QJsonObject{{"verifiedFiles", true},
                {"runtimeHash", "runtime-hash"}, {"environmentStamp", "stamp"}}}};
        const auto deliver = [&widget](const QString &generation, const QString &workspace, const QJsonObject &value) {
            ExtensionWidget::PendingRequest context{ExtensionWidget::RequestKind::RuntimeVerifyPoll, workspace};
            context.intent = generation; context.operation = "extension.runtime.verification.query";
            widget.pendingRequests.insert("reply", context);
            widget.onReply("reply", context.operation, value);
        };
        deliver("old-generation", {}, receipt);
        QVERIFY(!widget.runtimeVerificationReady);
        widget.observedWorkspace = "old-workspace";
        deliver("current-generation", "old-workspace", receipt);
        QVERIFY(!widget.runtimeVerificationReady); // Service configuration belongs to another workspace.
        widget.observedWorkspace.clear();
        deliver("current-generation", {}, receipt);
        QVERIFY(widget.runtimeVerificationReady);
        QVERIFY(widget.runtimeVerificationMessage.contains("2026-10-09"));
        auto changed = receipt; changed.insert("isCurrent", false); changed.insert("state", "failed");
        changed.insert("error", QJsonObject{{"code", "RUNTIME_CHANGED"}, {"message", "stamp changed"}});
        widget.currentPreview = {{"previewId", "stale-preview"}};
        deliver("current-generation", {}, changed);
        QVERIFY(!widget.runtimeVerificationReady);
        QVERIFY(widget.currentPreview.isEmpty());
        QVERIFY(!widget.runtimeVerificationTimer->isActive());
    }

    void actualNativeScientificPreviewPlotsAndWorkspaceSwitch()
    {
        if (qEnvironmentVariable("RANGE_SCIENTIFIC_GUI_ACCEPTANCE") != "1")
            QSKIP("Explicit frozen-runtime/native GUI acceptance is enabled by RANGE_SCIENTIFIC_GUI_ACCEPTANCE=1.");
#if defined(RANGE_SCIENTIFIC_PYTHON) && defined(RANGE_SCIENTIFIC_PACKAGE) && defined(RANGE_SCIENTIFIC_SOLVER) && defined(RANGE_SCIENTIFIC_NATIVE) && defined(RANGE_SCIENTIFIC_EXAMPLES)
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString evidence = qEnvironmentVariable("RANGE_SCIENTIFIC_GUI_EVIDENCE_DIRECTORY");
        const QString reuse = qEnvironmentVariable("RANGE_SCIENTIFIC_GUI_REUSE_FIXTURE");
        const QString fixtureRoot = reuse.isEmpty() ? fixture.path() : QFileInfo(reuse).absoluteFilePath();
        if (reuse.isEmpty() && !evidence.isEmpty())
        {
            fixture.setAutoRemove(false);
            QVERIFY(QDir().mkpath(evidence));
            QFile location(evidence + "/scientific-fixture-location.json");
            QVERIFY(location.open(QIODevice::WriteOnly));
            location.write(QJsonDocument(QJsonObject{{"root", fixtureRoot}}).toJson());
        }
        auto config = WorkflowProcess::defaultConfiguration(fixtureRoot + "/workspace");
        const auto configuredPath = [](const char *environmentName, const char *compiledPath) {
            const QString overridePath = qEnvironmentVariable(environmentName);
            return overridePath.isEmpty() ? QString::fromUtf8(compiledPath) : overridePath;
        };
        config.python = configuredPath("RANGE_TEST_SCIENTIFIC_PYTHON", RANGE_SCIENTIFIC_PYTHON);
        config.packageDirectory = configuredPath("RANGE_TEST_SCIENTIFIC_PACKAGE", RANGE_SCIENTIFIC_PACKAGE);
        config.solver = configuredPath("RANGE_TEST_SCIENTIFIC_SOLVER", RANGE_SCIENTIFIC_SOLVER);
        config.native = configuredPath("RANGE_TEST_SCIENTIFIC_NATIVE", RANGE_SCIENTIFIC_NATIVE);
        config.examples = configuredPath("RANGE_TEST_SCIENTIFIC_EXAMPLES", RANGE_SCIENTIFIC_EXAMPLES);
        config.trustedExtensions = true;
        config.requestTimeoutMs = 30000;
        for (const auto &path : {config.python, config.solver, config.native})
        {
            const QFileInfo info(path);
            QVERIFY2(info.isAbsolute() && info.isFile(), qPrintable(path));
        }
        for (const auto &path : {config.packageDirectory, config.examples})
        {
            const QFileInfo info(path);
            QVERIFY2(info.isAbsolute() && info.isDir(), qPrintable(path));
        }
        QString helper = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_HELPER");
        if (helper.isEmpty())
            helper = QDir(QString::fromUtf8(RANGE_SCIENTIFIC_PACKAGE))
                .absoluteFilePath("../tests/scientific_widget_bootstrap.py");
        const QFileInfo helperInfo(helper);
        QVERIFY2(helperInfo.isAbsolute() && helperInfo.isFile(), qPrintable(helper));
        QProcess bootstrap;
        // -I follows the private runtime's _pth import policy; the helper adds
        // only the reviewed SDK/test directories, never ambient PYTHONPATH.
        if (!QFileInfo(fixtureRoot + "/fixture.json").isFile())
        {
        bootstrap.start(config.python, {"-I", "-B", "-c",
            "import runpy,sys; p=sys.argv.pop(1); runpy.run_path(p,run_name='__main__')",
            helper, "--root", fixtureRoot, "--solver", config.solver, "--native", config.native,
            "--examples", config.examples, "--python", config.python,
            "--package", config.packageDirectory});
        QVERIFY(bootstrap.waitForStarted(10000));
        QVERIFY2(spinUntil([&bootstrap] { return bootstrap.state() == QProcess::NotRunning; }, 360000),
                 "Real scientific bootstrap did not finish within six minutes.");
        const auto diagnostics = bootstrap.readAllStandardError();
        QCOMPARE(bootstrap.exitStatus(), QProcess::NormalExit);
        QVERIFY2(bootstrap.exitCode() == 0, diagnostics.constData());
        }
        QFile setupFile(fixtureRoot + "/fixture.json");
        QVERIFY(setupFile.open(QIODevice::ReadOnly));
        const auto setup = QJsonDocument::fromJson(setupFile.readAll()).object();
        QVERIFY(!setup.isEmpty());
        const QString nativeJob = setup.value("sourceJob").toString();
        const QString bundle = setup.value("first").toObject().value("bundleHash").toString();
        const QJsonArray expectedChannels = setup.value("expected").toObject().value("channels").toArray();
        QCOMPARE(expectedChannels.size(), 2);
        WorkflowProcess workflow;
        QSignalSpy serviceFailures(&workflow, &WorkflowProcess::requestFailed);
        QSignalSpy serviceErrors(&workflow, &WorkflowProcess::serviceError);
        QSignalSpy serviceReplies(&workflow, &WorkflowProcess::replyReceived);
        ExtensionWidget widget(&workflow);
        widget.resize(800, 600);
        widget.show();
        const auto diagnostic = [&] {
            QJsonObject parameters; QString inputError;
            const bool inputValid = widget.buildRunParameters(&parameters, &inputError);
            QJsonArray failures;
            for (const auto &failure : serviceFailures)
                failures.append(QJsonObject{{"operation", failure.value(1).toString()},
                    {"code", failure.value(2).toString()}, {"message", failure.value(3).toString()}});
            QJsonArray errors;
            for (const auto &failure : serviceErrors) errors.append(failure.value(0).toString());
            QJsonArray replies;
            for (const auto &reply : serviceReplies)
                if (reply.value(1).toString() == "command.preview") replies.append(reply.value(2).toJsonObject());
            const QJsonObject state{{"status", widget.status->text()}, {"preview", widget.currentPreview},
                {"result", widget.resultView->toPlainText()}, {"inputValid", inputValid}, {"inputError", inputError},
                {"draft", parameters}, {"failures", failures}, {"serviceErrors", errors}, {"previewReplies", replies},
                {"commandInFlight", widget.commandInFlight}, {"pendingCommand", widget.pendingCommand},
                {"pendingJobId", widget.pendingJobId}, {"selection", widget.currentSelectionKey()},
                {"fixtureRoot", fixtureRoot}};
            const auto bytes = QJsonDocument(state).toJson();
            if (!evidence.isEmpty())
            {
                QFile output(evidence + "/scientific-gui-diagnostics.json");
                if (output.open(QIODevice::WriteOnly)) output.write(bytes);
                widget.grab().save(evidence + "/scientific-gui-diagnostic.png");
            }
            return QString::fromUtf8(bytes);
        };
        QCoreApplication::processEvents();
        QVERIFY(widget.minimumSizeHint().height() <= 600);
        workflow.start(config);
        QVERIFY(spinUntil([&workflow] { return workflow.isReady(); }, 30000));
        QVERIFY2(spinUntil([&widget] { return widget.runtimeVerificationReady; }, 360000), qPrintable(diagnostic()));
        QVERIFY(spinUntil([&widget] { return versionItem(widget.extensionTree, "1.0.0", "active"); }, 30000));
        widget.extensionTree->setCurrentItem(versionItem(widget.extensionTree, "1.0.0", "active"));
        QVERIFY(spinUntil([&widget] { return widget.scientificFormSupported && widget.runButton->isEnabled(); }, 15000));
        QVERIFY(widget.scientificForm->isVisible());
        QCOMPARE(widget.selectedBundleHash, bundle);
        QVERIFY(spinUntil([&widget, &nativeJob] { return widget.sourceJob->findData(nativeJob) >= 0; }, 15000));
        widget.sourceMode->setCurrentIndex(widget.sourceMode->findData("native-active"));
        widget.sourceJob->setCurrentIndex(widget.sourceJob->findData(nativeJob));
        const auto selections = setup.value("sources").toArray();
        for (const auto &value : selections)
        {
            const QString channel = value.toObject().value("channelId").toString();
            QVERIFY(spinUntil([&widget, &channel] {
                return widget.sourceSignal->findData(channel) >= 0 && widget.addSourceButton->isEnabled();
            }, 30000));
            widget.sourceSignal->setCurrentIndex(widget.sourceSignal->findData(channel));
            clickVisible(widget.addSourceButton);
        }
        QCOMPARE(widget.sourcesTable->rowCount(), 2);
        widget.windowInput->setCurrentText("hann");
        widget.removeMeanInput->setChecked(true);
        widget.filterInput->setCurrentText("butterworth-lowpass");
        widget.cutoffInput->setValue(10.0);
        clickVisible(widget.runButton);
        QVERIFY2(spinUntil([&widget] { return widget.approveButton->isEnabled(); }, 30000), qPrintable(diagnostic()));
        auto preview = QJsonDocument::fromJson(widget.previewView->toPlainText().toUtf8()).object();
        QCOMPARE(preview.value("normalized").toObject().value("command").toString(), QString("extension.run"));
        QCOMPARE(preview.value("normalized").toObject().value("parameters").toObject().value("series"), setup.value("series"));
        QVERIFY(widget.scientificJobId.isEmpty()); // A preview is not an execution.
        widget.cutoffInput->setValue(11.0);
        QVERIFY(widget.currentPreview.isEmpty());
        QVERIFY(!widget.approveButton->isEnabled());
        widget.cutoffInput->setValue(10.0);
        clickVisible(widget.runButton);
        QVERIFY2(spinUntil([&widget] { return widget.approveButton->isEnabled(); }, 30000), qPrintable(diagnostic()));
        const auto screenshot = [&widget, &evidence](const QString &name) {
            if (evidence.isEmpty()) return true;
            if (auto *scroll = widget.findChild<QScrollArea *>("extensionFeatureScroll"))
            {
                if (name == "scientific-native-preview") scroll->ensureWidgetVisible(widget.approveButton);
                else if (name != "scientific-native-rollback" && widget.sciencePlots->count())
                    scroll->ensureWidgetVisible(widget.sciencePlots);
                QCoreApplication::processEvents();
            }
            return QDir().mkpath(evidence) && widget.grab().save(evidence + "/" + name + ".png");
        };
        QVERIFY(screenshot("scientific-native-preview"));
        clickVisible(widget.approveButton);
        QVERIFY2(spinUntil([&widget] {
            return !widget.scientificJobId.isEmpty() && widget.sciencePlots->count() == 2;
            }, 180000), qPrintable(diagnostic()));
        const QString scienceJob = widget.scientificJobId;
        const auto result = widget.displayedScientificResult;
        QCOMPARE(result.value("bundleHash").toString(), bundle);
        const auto artifact = result.value("scientificResult").toObject();
        const QString relativePath = artifact.value("path").toString();
        QCOMPARE(relativePath, "jobs/" + scienceJob + "/scientific-result.json");
        QFile actualFile(QDir(config.workspace).absoluteFilePath(relativePath));
        QVERIFY(actualFile.open(QIODevice::ReadOnly));
        const auto actualBytes = actualFile.readAll();
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(actualBytes, QCryptographicHash::Sha256).toHex()),
                 artifact.value("sha256").toString());
        const auto actual = QJsonDocument::fromJson(actualBytes).object();
        QCOMPARE(actual.value("inputHash"), result.value("seriesSha256"));
        QCOMPARE(actual.value("times"), setup.value("series").toObject().value("times"));
        QCOMPARE(actual.value("filter"), setup.value("options").toObject().value("filter"));
        QCOMPARE(actual.value("sampleCount").toInt(), 20);
        QCOMPARE(actual.value("binCount").toInt(), 11);
        const auto profile = result.value("verificationProfile").toObject();
        QVERIFY(!profile.value("id").toString().isEmpty());
        QVERIFY(profile.value("absoluteTolerance").toDouble() > 0);
        QVERIFY(profile.value("relativeTolerance").toDouble() > 0);
        QFile provenanceFile(QDir(config.workspace).absoluteFilePath("jobs/" + scienceJob + "/input-provenance.json"));
        QVERIFY(provenanceFile.open(QIODevice::ReadOnly));
        const auto provenanceBytes = provenanceFile.readAll();
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(provenanceBytes, QCryptographicHash::Sha256).toHex()),
                 result.value("inputProvenanceSha256").toString());
        const auto provenance = QJsonDocument::fromJson(provenanceBytes).object();
        for (const auto &key : {QString("seriesSha256"), QString("bundleHash"), QString("runtimeHash")})
            QCOMPARE(provenance.value(key), result.value(key));
        const auto origins = provenance.value("provenance").toObject().value("sources").toArray();
        QCOMPARE(origins.size(), 2);
        QCOMPARE(provenance.value("provenance").toObject().value("options"), setup.value("options"));
        for (int channel = 0; channel < origins.size(); ++channel)
        {
            const auto origin = origins[channel].toObject();
            const auto selector = origin.value("selector").toObject();
            QCOMPARE(selector.value("jobId").toString(), nativeJob);
            QCOMPARE(selector.value("mode").toString(), QString("active"));
            QCOMPARE(selector.value("channelId"), selections[channel].toObject().value("channelId"));
            QCOMPARE(selector.value("artifactId"), setup.value("nativeCatalog").toObject().value("artifactId"));
            QCOMPARE(origin.value("proofScope").toString(), QString("solver-and-declared-controller-runtime/v1"));
            QCOMPARE(origin.value("sourceKind").toString(), QString("native-active-fe"));
            QVERIFY(!selector.value("sourceHash").toString().isEmpty());
        }
        const auto near = [&profile](double value, double expected) {
            return std::isfinite(value) && std::abs(value - expected) <= profile.value("absoluteTolerance").toDouble()
                + profile.value("relativeTolerance").toDouble() * std::abs(expected);
        };
        // Compare the complete output arrays against the independent small-N
        // DFT/filter oracle built from every native raw sample, not screenshots.
        const auto actualChannels = actual.value("channels").toArray();
        QCOMPARE(actualChannels.size(), expectedChannels.size());
        for (int channel = 0; channel < actualChannels.size(); ++channel)
        {
            const auto observed = actualChannels[channel].toObject();
            const auto expected = expectedChannels[channel].toObject();
            QCOMPARE(observed.value("unit"), expected.value("unit"));
            for (const QString &key : {QString("filteredValues"), QString("amplitude"), QString("psd"), QString("fft")})
            {
                const auto values = observed.value(key).toArray();
                const auto oracle = expected.value(key).toArray();
                QCOMPARE(values.size(), oracle.size());
                for (int i = 0; i < values.size(); ++i)
                {
                    if (key == "fft")
                    {
                        QCOMPARE(values[i].toArray().size(), 2);
                        for (int part = 0; part < 2; ++part)
                            QVERIFY(near(values[i].toArray()[part].toDouble(), oracle[i].toArray()[part].toDouble()));
                    }
                    else QVERIFY(near(values[i].toDouble(), oracle[i].toDouble()));
                }
            }
        }
        for (const QString &view : {QString("filtered-time"), QString("amplitude"), QString("psd"),
                                   QString("real"), QString("imag"), QString("phase")})
        {
            widget.scienceView->setCurrentText(view);
            QVERIFY(spinUntil([&widget, &view] {
                auto *plot = qobject_cast<SeriesPlotWidget *>(widget.sciencePlots->widget(0));
                return plot && plot->dataset().title == view && widget.sciencePlots->count() == 2;
            }, 30000));
            for (int channel = 0; channel < 2; ++channel)
            {
                auto *plot = qobject_cast<SeriesPlotWidget *>(widget.sciencePlots->widget(channel));
                QVERIFY(plot);
                const auto &dataset = plot->dataset();
                const auto output = actualChannels[channel].toObject();
                const QString unit = output.value("unit").toString();
                QCOMPARE(dataset.xUnit, view == "filtered-time" ? QString("s") : QString("Hz"));
                QCOMPARE(dataset.yUnit, view == "phase" ? QString("rad") : view == "psd" ? unit + "^2/Hz" : unit);
                QCOMPARE(dataset.traces.size(), 1);
                const auto &trace = dataset.traces.first();
                QCOMPARE(trace.sourceId, scienceJob);
                QCOMPARE(trace.artifactSha256, artifact.value("sha256").toString());
                QVERIFY(trace.label.contains(selections[channel].toObject().value("channelId").toString()));
                const auto xs = actual.value(view == "filtered-time" ? "times" : "frequenciesHz").toArray();
                QCOMPARE(trace.points.size(), xs.size());
                QVERIFY(trace.points.size() <= 1000);
                for (int i = 0; i < trace.points.size(); ++i)
                {
                    QCOMPARE(trace.originalOrdinals[i], quint64(i));
                    QCOMPARE(trace.points[i].x(), xs[i].toDouble());
                    double y = 0;
                    if (view == "filtered-time") y = output.value("filteredValues").toArray()[i].toDouble();
                    else if (view == "amplitude" || view == "psd") y = output.value(view).toArray()[i].toDouble();
                    else
                    {
                        const auto pair = output.value("fft").toArray()[i].toArray();
                        y = view == "real" ? pair[0].toDouble() : view == "imag" ? pair[1].toDouble()
                            : std::atan2(pair[1].toDouble(), pair[0].toDouble());
                    }
                    QCOMPARE(trace.points[i].y(), y);
                }
                QCOMPARE(widget.metricsTable->item(channel, 1)->text(), unit);
                QCOMPARE(widget.metricsTable->item(channel, 2)->text(), QString("20"));
                QCOMPARE(widget.metricsTable->item(channel, 3)->text(), QString("11"));
            }
            QVERIFY(screenshot("scientific-native-" + view));
        }
        for (const auto &proof : {scienceJob, bundle, result.value("runtimeHash").toString(),
                                  result.value("seriesSha256").toString(), artifact.value("sha256").toString(),
                                  profile.value("id").toString(), QString("causal"), QString("10")})
            QVERIFY2(widget.scienceProvenance->text().contains(proof), qPrintable(proof));
        QVERIFY(widget.scienceProvenance->text().contains("accepted native frames"));
        QString error;
        QVERIFY(spinUntil([&widget] { return candidateItem(widget.extensionTree, "2.0.0", "validated"); }, 15000));
        widget.extensionTree->setCurrentItem(candidateItem(widget.extensionTree, "2.0.0", "validated"));
        QVERIFY2(previewAndApproveWidgetAction(&widget, "extensionActivate", "extension.activate", "active", &error), qPrintable(error));
        QVERIFY(spinUntil([&widget] { return versionItem(widget.extensionTree, "2.0.0", "active"); }, 15000));
        widget.extensionTree->setCurrentItem(widget.extensionTree->topLevelItem(0));
        QVERIFY(spinUntil([&widget, &bundle] { return widget.rollbackTarget->findData(bundle) >= 0; }, 15000));
        widget.rollbackTarget->setCurrentIndex(widget.rollbackTarget->findData(bundle));
        QVERIFY2(previewAndApproveWidgetAction(&widget, "extensionRollback", "extension.rollback", bundle.left(12), &error), qPrintable(error));
        QVERIFY(spinUntil([&widget] { return versionItem(widget.extensionTree, "1.0.0", "active") && versionItem(widget.extensionTree, "2.0.0", "retained"); }, 15000));
        QJsonObject historical;
        QVERIFY2(requestWorkflow(&workflow, "jobs.query", {{"jobId", scienceJob}}, &historical, &error), qPrintable(error));
        QCOMPARE(historical.value("job").toObject().value("result"), QJsonValue(result));
        actualFile.close();
        QVERIFY(actualFile.open(QIODevice::ReadOnly));
        QCOMPARE(actualFile.readAll(), actualBytes);
        QVERIFY(screenshot("scientific-native-rollback"));
        // Restore a real completed display before switching to B; merely
        // clearing an already empty widget cannot demonstrate this regression.
        widget.extensionTree->setCurrentItem(versionItem(widget.extensionTree, "1.0.0", "active"));
        QVERIFY(spinUntil([&widget] { return widget.scientificFormSupported; }, 15000));
        widget.sourceMode->setCurrentIndex(widget.sourceMode->findData("native-active"));
        widget.sourceJob->setCurrentIndex(widget.sourceJob->findData(nativeJob));
        for (const auto &value : selections)
        {
            const QString channel = value.toObject().value("channelId").toString();
            QVERIFY(spinUntil([&widget, &channel] {
                return widget.sourceSignal->findData(channel) >= 0 && widget.addSourceButton->isEnabled();
            }, 30000));
            widget.sourceSignal->setCurrentIndex(widget.sourceSignal->findData(channel));
            clickVisible(widget.addSourceButton);
        }
        QCOMPARE(widget.scientificSources.size(), 2);
        QVERIFY2(previewAndApproveWidgetAction(&widget, "extensionRun", "extension.run", "scientificResult", &error), qPrintable(error));
        QVERIFY(spinUntil([&widget] { return widget.sciencePlots->count() == 2 && !widget.scientificJobId.isEmpty(); }, 30000));
        workflow.stop();
        QVERIFY(spinUntil([&workflow] { return workflow.isStopped(); }, 10000));
        config.workspace = setup.value("secondWorkspace").toString();
        workflow.start(config);
        QVERIFY(spinUntil([&workflow] { return workflow.isReady(); }, 30000));
        QVERIFY(spinUntil([&widget] { return versionItem(widget.extensionTree, "1.0.0", "active"); }, 30000));
        widget.extensionTree->setCurrentItem(versionItem(widget.extensionTree, "1.0.0", "active"));
        QVERIFY(spinUntil([&widget, &bundle] { return widget.scientificFormSupported && widget.selectedBundleHash == bundle; }, 15000));
        QVERIFY(widget.scientificSources.isEmpty());
        QVERIFY(widget.scientificJobId.isEmpty());
        QVERIFY(widget.displayedScientificResult.isEmpty());
        QVERIFY(widget.scientificGeneration.isEmpty());
        QCOMPARE(widget.sourcesTable->rowCount(), 0);
        QCOMPARE(widget.sciencePlots->count(), 0);
        QCOMPARE(widget.pendingSciencePlots->count(), 0);
        QCOMPARE(widget.metricsTable->rowCount(), 0);
        QVERIFY(widget.scienceProvenance->text().isEmpty());
        QVERIFY(screenshot("scientific-same-bundle-workspace-cleared"));
        workflow.stop();
        QVERIFY(spinUntil([&workflow] { return workflow.isStopped(); }, 10000));
#else
        QFAIL("Actual WF11 GUI acceptance requires explicit RANGE_SCIENTIFIC_* build paths.");
#endif
    }

    void scientificFormPreservesDraftAndRejectsExpressions()
    {
        ExtensionWidget widget(nullptr);
        const QJsonObject gui{{"form", QJsonArray{
            QJsonObject{{"id", "sources"}, {"type", "channel-select"}, {"minimum", 1}, {"maximum", 4}},
            QJsonObject{{"id", "window"}, {"type", "enum"}, {"values", QJsonArray{"rectangular", "hann"}}},
            QJsonObject{{"id", "removeMean"}, {"type", "enum"}, {"values", QJsonArray{false, true}}},
            QJsonObject{{"id", "filter"}, {"type", "enum"}, {"values", QJsonArray{"none", "butterworth-lowpass"}}},
            QJsonObject{{"id", "cutoffHz"}, {"type", "number"}, {"unit", "Hz"}}}},
            {"table", QJsonObject{{"columns", QJsonArray{"channel", "unit", "sampleCount", "binCount"}}}},
            {"plots", QJsonArray{"filtered-time", "amplitude", "psd", "real", "imag", "phase"}}};
        widget.scientificGui = gui; widget.selectedExtensionId = "science"; widget.selectedBundleHash = "bundle";
        QJsonObject manifest{{"sdkVersion", "range.extension/v2"}, {"kind", "multichannel-spectral-filter"}, {"gui", gui}};
        widget.renderFormAndMetrics(manifest); QVERIFY(widget.scientificFormSupported);
        auto *featureScroll = widget.findChild<QScrollArea *>("extensionFeatureScroll");
        QVERIFY(featureScroll); QVERIFY(featureScroll->widgetResizable());
        widget.resize(800, 600); widget.show();
        widget.scienceProvenance->setText(QString("Runtime: %1\nSource: %1\nResult: %1\n%2")
            .arg(QString(64, 'a'), QString("Complete numerical contract and accepted frame provenance.\n").repeated(12)));
        QCoreApplication::processEvents();
        QVERIFY(widget.minimumSizeHint().height() <= 600);
        featureScroll->ensureWidgetVisible(widget.approveButton);
        QCoreApplication::processEvents();
        QVERIFY(featureScroll->viewport()->rect().contains(widget.approveButton->mapTo(featureScroll->viewport(), widget.approveButton->rect().center())));
        widget.windowInput->setCurrentText("hann"); widget.removeMeanInput->setChecked(true);
        widget.currentPreview = {{"previewId", "stale"}};
        widget.filterInput->setCurrentText("butterworth-lowpass"); QVERIFY(widget.currentPreview.isEmpty());
        widget.sourceJob->addItem("Native", "native-job"); widget.sourceSignal->addItem("node:1", "node:1");
        widget.nativeSourceDescriptor = {{"artifactId", "frames"}};
        widget.addScientificSource(); widget.addScientificSource(); QCOMPARE(widget.scientificSources.size(), 1);
        widget.renderFormAndMetrics(manifest);
        QCOMPARE(widget.windowInput->currentText(), QString("hann")); QVERIFY(widget.removeMeanInput->isChecked());
        QCOMPARE(widget.scientificSources.size(), 1);
        widget.selectedVersion = {{"state", "active"}, {"bundleHash", "bundle"}, {"manifest", manifest}};
        widget.extensionListResult = {{"extensions", QJsonArray{QJsonObject{{"extensionId", "science"}, {"activeBundleHash", "bundle"}}}}};
        QJsonObject parameters; QString error;
        QVERIFY2(widget.buildRunParameters(&parameters, &error), qPrintable(error));
        QCOMPARE(parameters.value("sources").toArray().size(), 1);
        const auto options = parameters.value("options").toObject();
        QCOMPARE(options.value("window").toString(), QString("hann")); QVERIFY(options.value("removeMean").toBool());
        QCOMPARE(options.value("filter").toObject().value("order").toInt(), 1);
        QCOMPARE(options.value("filter").toObject().value("phase").toString(), QString("causal"));
        widget.observedWorkspace = "workspace-A";
        widget.scientificJobId = "old-job"; widget.displayedScientificResult = {{"jobId", "old-job"}};
        widget.scienceProvenance->setText("old-source-proof");
        widget.sciencePlots->addTab(new SeriesPlotWidget, "Old channel");
        widget.pendingSciencePlots->addTab(new SeriesPlotWidget, "Old pending channel");
        widget.resetScientificWorkspace(); widget.observedWorkspace = "workspace-B";
        widget.renderFormAndMetrics(manifest); // Same immutable bundle in a different workspace.
        QVERIFY(widget.scientificSources.isEmpty()); QVERIFY(widget.scientificJobId.isEmpty());
        QVERIFY(widget.displayedScientificResult.isEmpty()); QVERIFY(widget.scientificGeneration.isEmpty());
        QVERIFY(widget.scienceProvenance->text().isEmpty()); QCOMPARE(widget.sciencePlots->count(), 0);
        QCOMPARE(widget.pendingSciencePlots->count(), 0);
        widget.scienceProvenance->setText("previous-bundle-source-proof");
        widget.scientificJobId = "previous-bundle-job";
        widget.scientificPoints = QJsonArray{QJsonObject{{"x",1},{"y",2}}};
        widget.scientificPageIdentity = {{"resultSha256","old-result"}};
        widget.pendingScienceRows = QJsonArray{QJsonObject{{"channel","old-channel"}}};
        widget.pendingSciencePlots->addTab(new SeriesPlotWidget, "Old pending channel");
        widget.selectedBundleHash = "other-bundle"; widget.renderFormAndMetrics(manifest);
        QVERIFY(widget.scienceProvenance->text().isEmpty()); QVERIFY(widget.scientificJobId.isEmpty());
        QVERIFY(widget.scientificPoints.isEmpty()); QVERIFY(widget.scientificPageIdentity.isEmpty());
        QVERIFY(widget.pendingScienceRows.isEmpty()); QCOMPARE(widget.pendingSciencePlots->count(),0);
        QCOMPARE(widget.scientificSources.size(), 0); QCOMPARE(widget.windowInput->currentText(), QString("rectangular"));
        QJsonObject maliciousGui = gui; maliciousGui.insert("expression", "execute()");
        manifest.insert("gui", maliciousGui); widget.scientificGui = maliciousGui;
        widget.renderFormAndMetrics(manifest); QVERIFY(!widget.featureFormSupported);
    }

    void scientificPagingPreservesExtremaAndMixedUnits()
    {
        ExtensionWidget widget(nullptr); widget.scientificJobId = "science-job"; widget.selectedBundleHash = "bundle";
        widget.scienceView->setCurrentText("amplitude");
        widget.metricsTable->setColumnCount(4);
        auto page = [](int channel, int offset, int count, const QString &unit) {
            QJsonArray points;
            for (int i = offset; i < offset + count; ++i)
                points.append(QJsonObject{{"x", double(i)}, {"y", i == 1555 ? 99999.0 : double(i % 3)}, {"rawOrdinal", i}});
            return QJsonObject{{"jobId", "science-job"}, {"bundleHash", "bundle"}, {"view", "amplitude"},
                {"runtimeHash", "runtime"}, {"sourceSha256", "source"}, {"resultSha256", QString(64, QChar('a'))},
                {"channel", QJsonObject{{"id", QString("channel-%1").arg(channel)}, {"unit", unit}}},
                {"channelCount", 2}, {"sampleCount", 3998}, {"binCount", 2000}, {"pointCount", 2000},
                {"offset", offset}, {"nextOffset", offset + count < 2000 ? QJsonValue(offset + count) : QJsonValue(QJsonValue::Null)},
                {"domain", "frequency"}, {"xUnit", "Hz"}, {"xLabel", "Frequency"}, {"yUnit", unit}, {"yLabel", "Amplitude"}, {"points", points}};
        };
        widget.consumeScientificPage(page(0, 0, 1000, "m/s")); QCOMPARE(widget.sciencePlots->count(), 0);
        widget.consumeScientificPage(page(0, 1000, 1000, "m/s")); QCOMPARE(widget.sciencePlots->count(), 0);
        widget.consumeScientificPage(page(1, 0, 1000, "N")); widget.consumeScientificPage(page(1, 1000, 1000, "N"));
        auto *first = qobject_cast<SeriesPlotWidget *>(widget.sciencePlots->widget(0)); QVERIFY(first);
        const auto &trace = first->dataset().traces.first(); QVERIFY(trace.points.size() <= 1000);
        QVERIFY(trace.originalOrdinals.contains(1555)); QCOMPARE(first->dataset().yUnit, QString("m/s"));
        QCOMPARE(widget.sciencePlots->count(), 2);
        auto *second = qobject_cast<SeriesPlotWidget *>(widget.sciencePlots->widget(1)); QVERIFY(second);
        QCOMPARE(second->dataset().yUnit, QString("N")); QCOMPARE(first->dataset().traces.size(), 1);
        widget.scientificChannel = 0; widget.scientificPoints = {}; widget.scientificPageIdentity = {};
        widget.consumeScientificPage(page(0, 0, 1000, "m/s"));
        auto altered = page(0, 1000, 1000, "m/s"); altered.insert("runtimeHash", "tampered");
        widget.consumeScientificPage(altered);
        QCOMPARE(widget.sciencePlots->widget(0), first); QVERIFY(widget.scientificPoints.isEmpty());
    }

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
