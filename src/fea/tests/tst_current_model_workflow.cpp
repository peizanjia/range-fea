#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>

#include <rbl_error.h>
#include <rml_material_property.h>

#include "application.h"
#include "main_window.h"
#include "model_io.h"
#include "model_tree.h"
#include "native_results_widget.h"
#include "session.h"
#include "workflow_process.h"
#include "workflow_widget.h"

namespace {
bool clickControl(QWidget *widget)
{
    if (!widget || !widget->isEnabled()) return false;
    for (QWidget *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) scroll->ensureWidgetVisible(widget);
    QCoreApplication::processEvents();
    if (!widget->isVisible()) return false;
    QTest::mouseClick(widget, Qt::LeftButton); return true;
}

bool clickTab(QTabWidget *tabs, int index)
{
    if (!tabs || index < 0 || index >= tabs->count()) return false;
    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(index).center());
    QCoreApplication::processEvents(); return tabs->currentIndex() == index;
}

void enterNumber(QAbstractSpinBox *widget, const QString &text)
{
    for (QWidget *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) scroll->ensureWidgetVisible(widget);
    QTest::mouseClick(widget, Qt::LeftButton); QTest::keyClick(widget, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(widget, text); QTest::keyClick(widget, Qt::Key_Tab);
}

QJsonObject lastResponse(const QSignalSpy &replies, const QString &operation)
{
    for (int index = replies.size() - 1; index >= 0; --index)
        if (replies[index][1].toString() == operation) return replies[index][2].toJsonObject();
    return {};
}

QJsonObject readService(WorkflowProcess *transport, const QString &operation, const QJsonObject &args)
{
    QSignalSpy replies(transport, &WorkflowProcess::replyReceived);
    QSignalSpy failures(transport, &WorkflowProcess::requestFailed);
    const QString id = transport->request(operation, args);
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 30000)
    {
        for (const auto &reply : replies) if (reply[0].toString() == id) return reply[2].toJsonObject();
        for (const auto &failure : failures)
            if (failure[0].toString() == id) { qWarning("Read failed: %s", qPrintable(failure[3].toString())); return {}; }
        QCoreApplication::processEvents(); QTest::qWait(10);
    }
    return {};
}

QStringList csvRow(const QByteArray &bytes)
{
    const QString line = QString::fromUtf8(bytes); QStringList fields; QString field; bool quoted = false;
    for (int index = 0; index < line.size(); ++index)
    {
        const QChar character = line[index];
        if (character == '"')
        {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') { field += '"'; ++index; }
            else quoted = !quoted;
        }
        else if (character == ',' && !quoted) { fields.append(field); field.clear(); }
        else field += character;
    }
    fields.append(field); return fields;
}

struct DialogAttributeGuard
{
    bool previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    DialogAttributeGuard() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true); }
    ~DialogAttributeGuard() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); }
};
}

class TestCurrentModelWorkflow : public QObject
{
    Q_OBJECT
    QString evidenceDirectory;
    WorkflowWidget *workflow = nullptr;
    bool screenshot(const QString &name)
    {
        auto *window = Application::instance()->getMainWindow();
        QCoreApplication::processEvents();
        return window && window->grab().save(QDir(evidenceDirectory).filePath(name + ".png"));
    }
private slots:
    void initTestCase()
    {
        // This acceptance case is deliberately a native Windows gate, not an
        // offscreen substitute or an opt-in test that silently skips by default.
        QCOMPARE(QApplication::platformName(), QStringLiteral("windows"));
        auto *window = Application::instance()->getMainWindow(); QVERIFY(window);
        window->resize(1600, 1100); window->show(); window->raise(); window->activateWindow();
        QVERIFY2(QTest::qWaitForWindowExposed(window, 10000), "The native main window was not exposed.");
        evidenceDirectory = qEnvironmentVariable("RANGE_NATIVE_GUI_EVIDENCE_DIRECTORY");
#ifdef RANGE_WORKFLOW_SOURCE_ROOT
        if (evidenceDirectory.isEmpty()) evidenceDirectory = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT))
            .absoluteFilePath("../build/audit-20261008/WF-03-resume/gui-current-model");
#endif
        QVERIFY2(!evidenceDirectory.isEmpty(), "A native window evidence directory is required.");
        QVERIFY(QDir().mkpath(evidenceDirectory));
        workflow = window->findChild<WorkflowWidget *>("workflowWidget"); QVERIFY(workflow);
    }

    void cleanup()
    {
        if (workflow) { workflow->transport()->stop(); QTRY_VERIFY_WITH_TIMEOUT(workflow->transport()->isStopped(), 10000); }
        Application::instance()->getSession()->clear();
    }

    void nondefaultBeamSnapshotToNativeFeTableAndFrozenCsv()
    {
        auto *session = Application::instance()->getSession(); session->clear();
        QTemporaryDir project(QDir(evidenceDirectory).filePath("beam-project-XXXXXX"));
        QTemporaryDir workspace(QDir(evidenceDirectory).filePath("beam-workflow-XXXXXX"));
        QVERIFY(project.isValid()); QVERIFY(workspace.isValid());
        // Preserve the actual raw outputs alongside screenshots/CSV for review.
        project.setAutoRemove(false); workspace.setAutoRemove(false);
        QString error; QVERIFY2(session->openProject(project.path(), &error), qPrintable(error));
#ifdef RANGE_CURRENT_MODEL_TEST_FIXTURE
        const QString generator = QStringLiteral(RANGE_CURRENT_MODEL_TEST_FIXTURE);
#else
        const QString generator = QDir(QCoreApplication::applicationDirPath()).filePath("range-workflow-native-import-fixture.exe");
#endif
        QVERIFY2(QFileInfo(generator).isFile(), qPrintable(generator));
        const QString input = QDir(project.path()).filePath("nondefault-beam-A.ram");
        QProcess fixture; fixture.start(generator, {"beam", input});
        QVERIFY(fixture.waitForFinished(30000));
        QCOMPARE(fixture.exitStatus(), QProcess::NormalExit); QCOMPARE(fixture.exitCode(), 0);
        Model model;
        try { ModelIO::open(model, input); }
        catch (const RError &exception) { QFAIL(qPrintable(exception.getMessage())); }
        model.setName("WF03 nondefault beam A");
        QCOMPARE(model.getNode(1).getX(), 3.3);
        const auto youngPosition = model.getLine(0).getMaterial().findPosition(RMaterialProperty::ModulusOfElasiticity);
        QCOMPARE(model.getLine(0).getMaterial().get(youngPosition).getValue(0), 5.1e9);
        session->addModel(model); const uint modelId = session->getNModels() - 1;
        const auto stampA = session->modelCommandStamp(modelId);
        auto *window = Application::instance()->getMainWindow();
        auto *modelDock = window->findChild<QDockWidget *>("modelDock"); QVERIFY(modelDock); modelDock->show(); modelDock->raise();
        auto *modelTree = window->findChild<ModelTree *>(); QVERIFY(modelTree);
        QTRY_VERIFY(modelTree->topLevelItemCount() > int(modelId));
        auto *documentRow = modelTree->topLevelItem(int(modelId)); modelTree->scrollToItem(documentRow);
        QTest::mouseClick(modelTree->viewport(), Qt::LeftButton, Qt::NoModifier, modelTree->visualItemRect(documentRow).center());
        QCOMPARE(session->getSelectedModelIDs(), QList<uint>{modelId});
        auto *dock = window->findChild<QDockWidget *>("controlDock"); QVERIFY(dock); dock->show(); dock->raise();
        window->resizeDocks({dock}, {700}, Qt::Vertical);
        auto *pages = workflow->findChild<QTabWidget *>("workflowPages"); QVERIFY(clickTab(pages, 1));
        auto *workspaceEdit = workflow->findChild<QLineEdit *>("workflowWorkspace"); QVERIFY(workspaceEdit);
        QVERIFY(clickControl(workspaceEdit)); QTest::keyClick(workspaceEdit, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(workspaceEdit, workspace.path());
        QVERIFY(clickControl(workflow->findChild<QPushButton *>("workflowOpen")));
        QTRY_VERIFY_WITH_TIMEOUT(workflow->transport()->isReady(), 30000);
        QSignalSpy replies(workflow->transport(), &WorkflowProcess::replyReceived);
        QSignalSpy accepted(workflow, &WorkflowWidget::jobAccepted);
        QVERIFY(clickControl(workflow->findChild<QPushButton *>("workflowUseDocumentSnapshot")));
        QTRY_VERIFY_WITH_TIMEOUT(!lastResponse(replies, "model.source.register").isEmpty(), 30000);
        const auto sourceA = lastResponse(replies, "model.source.register");
        const QString sourceId = sourceA.value("sourceId").toString(); QVERIFY(!sourceId.isEmpty());
        const auto descriptor = sourceA.value("descriptor").toObject();
        QCOMPARE(descriptor.value("modelKind").toString(), QString("cantilever-beam"));
        const auto nodes = descriptor.value("nodes").toArray(); QCOMPARE(nodes.size(), 2);
        QCOMPARE(nodes[1].toObject().value("coordinates").toArray()[0].toDouble(), 3.3);
        QCOMPARE(nodes[1].toObject().value("initialDisplacementM").toArray()[1].toDouble(), .0042);
        QCOMPARE(nodes[1].toObject().value("initialVelocityMS").toArray()[1].toDouble(), -.031);
        QCOMPARE(descriptor.value("elements").toArray()[0].toObject().value("material").toObject().value("youngModulusPa").toDouble(), 5.1e9);
        const auto port = descriptor.value("ports").toObject().value("force").toObject();
        QCOMPARE(port.value("nodePhysicalId").toInt(), 117);
        QCOMPARE(port.value("globalDirection").toArray(), QJsonArray({0, 1, 0}));
        QCOMPARE(port.value("minimum").toDouble(), -1e6); QCOMPARE(port.value("maximum").toDouble(), 1e6);
        auto *choice = workflow->findChild<QComboBox *>("workflowModel"); QVERIFY(choice);
        QCOMPARE(choice->currentData(Qt::UserRole + 1).toString(), sourceId);
        QVERIFY(choice->currentText().contains(sourceA.value("registeredModelSha256").toString().left(12)));
        QVERIFY(screenshot("01-nondefault-source-A"));
        auto *dt = workflow->findChild<QDoubleSpinBox *>("workflowTimeStep");
        auto *steps = workflow->findChild<QSpinBox *>("workflowSteps"); QVERIFY(dt); QVERIFY(steps);
        enterNumber(dt, "0.001"); enterNumber(steps, "80"); QCOMPARE(dt->value(), .001); QCOMPARE(steps->value(), 80);
        auto *configure = workflow->findChild<QPushButton *>("workflowConfigure");
        auto *apply = workflow->findChild<QPushButton *>("workflowApply"); QVERIFY(configure); QVERIFY(apply);
        QVERIFY(clickControl(configure));
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 30000);
        const auto previewA = workflow->currentPreview();
        QCOMPARE(previewA.value("normalized").toObject().value("parameters").toObject().value("sourceId").toString(), sourceId);
        QVERIFY(workflow->findChild<QTextBrowser *>("workflowPreview")->toPlainText().contains("117"));
        // A pending editor draft invalidates GUI approval. This injects the
        // draft state only; approval itself is attempted through the real button.
        session->setNumericDraft(modelId, "wf03-test", "initialVelocity", "invalid", "1e-");
        QTRY_VERIFY(!apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton); QTest::qWait(100);
        QCOMPARE(accepted.count(), 0); QVERIFY(lastResponse(replies, "host.approve").isEmpty());
        QVERIFY(screenshot("02-invalid-draft-blocks-approval"));
        session->clearNumericDraft(modelId, "wf03-test", "initialVelocity");
        QTRY_VERIFY(apply->isEnabled()); QVERIFY(clickControl(apply));
        QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 1, 30000);
        const QString configJob = accepted[0][0].toString();
        QTRY_VERIFY_WITH_TIMEOUT(workflow->findChild<QPushButton *>("workflowRun")->isEnabled(), 30000);
        // Wait for the persisted configure task rather than racing its worker.
        QJsonObject configured;
        QElapsedTimer configureWait; configureWait.start();
        do { configured = readService(workflow->transport(), "jobs.query", {{"jobId", configJob}}).value("job").toObject(); if (configured.value("state") == "succeeded") break; QTest::qWait(20); }
        while (configureWait.elapsed() < 30000);
        QCOMPARE(configured.value("state").toString(), QString("succeeded"));
        QVERIFY(clickTab(pages, 1));
        QVERIFY(clickControl(workflow->findChild<QPushButton *>("workflowRun")));
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 30000);
        QCOMPARE(workflow->currentPreview().value("normalized").toObject().value("command").toString(), QString("workflow.run"));
        session->setNumericDraft(modelId, "wf03-test", "runVelocity", "invalid", "1e-");
        QTRY_VERIFY(!apply->isEnabled());
        QTest::mouseClick(apply, Qt::LeftButton); QTest::qWait(100);
        QCOMPARE(accepted.count(), 1);
        session->clearNumericDraft(modelId, "wf03-test", "runVelocity");
        QTRY_VERIFY(apply->isEnabled());
        QVERIFY(screenshot("03-active-passive-preview-A"));
        QJsonObject runResult;
        const auto resultConnection = connect(workflow, &WorkflowWidget::resultDisplayed, this, [&](const QJsonObject &result) {
            if (result.contains("nativeResults")) runResult = result;
        });
        QVERIFY(clickControl(apply)); QTRY_COMPARE_WITH_TIMEOUT(accepted.count(), 2, 30000);
        const QString runJob = accepted[1][0].toString();
        // After acceptance, change the open model's real material and close it.
        // The calculation must continue using the already copied source A.
        auto &materialB = session->getModel(modelId).getLine(0).getMaterial();
        materialB.get(youngPosition).clear(); materialB.get(youngPosition).add(293.15, 9.9e9);
        session->getModel(modelId).setName("WF03 edited beam B"); session->setModelChanged(modelId);
        QVERIFY(session->modelCommandStamp(modelId).second > stampA.second);
        session->removeModel(modelId); QCOMPARE(session->findModelByCommandIdentity(stampA.first), RConstants::eod);
        QTRY_VERIFY_WITH_TIMEOUT(runResult.value("jobId").toString() == runJob, 120000);
        disconnect(resultConnection);
        QCOMPARE(runResult.value("modelBinding").toObject().value("sourceId").toString(), sourceId);
        QCOMPARE(runResult.value("modelBinding").toObject().value("registeredModelSha256"), sourceA.value("registeredModelSha256"));
        QCOMPARE(runResult.value("totalSteps").toInt(), 80);
        QVERIFY(runResult.contains("active")); QVERIFY(runResult.contains("passive"));
        const auto sourceAfterClose = readService(workflow->transport(), "model.source.query", {{"sourceId", sourceId}});
        QCOMPARE(sourceAfterClose, sourceA);
        QVERIFY(clickTab(pages, 3));
        auto *native = workflow->findChild<NativeResultsWidget *>("workflowNativeResults"); QVERIFY(native);
        QTRY_VERIFY_WITH_TIMEOUT(!native->displaySnapshot().value("summary").toObject().isEmpty(), 30000);
        auto *nativeChannels = native->findChild<QComboBox *>("nativeResultChannel"); QVERIFY(nativeChannels);
        const int tipY = nativeChannels->findData("node:117:displacement:Y"); QVERIFY(tipY >= 0);
        QVERIFY(clickControl(nativeChannels)); QTest::keyClick(nativeChannels, Qt::Key_Home);
        for (int index = 0; index < tipY; ++index) QTest::keyClick(nativeChannels, Qt::Key_Down);
        QTest::keyClick(nativeChannels, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!native->displaySnapshot().value("summary").toObject().isEmpty(), 30000);
        QCOMPARE(native->displaySnapshot().value("channelId").toString(), QString("node:117:displacement:Y"));
        const auto activeSnapshot = native->displaySnapshot();
        QCOMPARE(activeSnapshot.value("jobId").toString(), runJob);
        QCOMPARE(activeSnapshot.value("samples").toArray().size(), 80);
        QJsonObject expectedResultOrigin = sourceA.value("originRef").toObject();
        expectedResultOrigin.insert("usesOpenDocument", true); expectedResultOrigin.insert("sourceId", sourceId);
        expectedResultOrigin.insert("registeredModelSha256", sourceA.value("registeredModelSha256"));
        QCOMPARE(activeSnapshot.value("provenance").toObject().value("sourceSnapshotRef").toObject(), expectedResultOrigin);
        QCOMPARE(activeSnapshot.value("provenance").toObject().value("sourceRuntimeStamp"),
                 sourceA.value("originRef").toObject().value("instanceRef"));
        QCOMPARE(activeSnapshot.value("provenance").toObject().value("mode").toString(), QString("active"));
        QVERIFY(screenshot("04-native-active-A-chart-table"));
        auto *modeChoice = workflow->findChild<QComboBox *>("workflowNativeMode"); QVERIFY(modeChoice);
        QVERIFY(clickControl(modeChoice)); QTest::keyClick(modeChoice, Qt::Key_End); QTest::keyClick(modeChoice, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(native->displaySnapshot().value("provenance").toObject().value("mode").toString(), QString("passive"), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!native->displaySnapshot().value("summary").toObject().isEmpty(), 30000);
        // Switch modes through the GUI, then explicitly choose the same physical channel.
        const int passiveTipY = nativeChannels->findData("node:117:displacement:Y"); QVERIFY(passiveTipY >= 0);
        QVERIFY(clickControl(nativeChannels)); QTest::keyClick(nativeChannels, Qt::Key_Home);
        for (int index = 0; index < passiveTipY; ++index) QTest::keyClick(nativeChannels, Qt::Key_Down);
        QTest::keyClick(nativeChannels, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!native->displaySnapshot().value("summary").toObject().isEmpty(), 30000);
        auto *hold = native->findChild<QPushButton *>("nativeResultHold"); QVERIFY(clickControl(hold));
        const auto frozen = native->displaySnapshot(); QVERIFY(hold->isChecked());
        QCOMPARE(frozen.value("channelId").toString(), QString("node:117:displacement:Y"));
        QCOMPARE(frozen.value("unit").toString(), QString("m"));
        QCOMPARE(frozen.value("samples").toArray().size(), 80);
        const QString csvPath = QDir(evidenceDirectory).filePath("nondefault-beam-passive-A.csv");
        QFile::remove(csvPath); // Explicit test-owned output from a previous run.
        DialogAttributeGuard dialogGuard; QTimer dialogDriver; dialogDriver.setInterval(25); bool dialogAccepted = false;
        QElapsedTimer dialogWait; dialogWait.start();
        connect(&dialogDriver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            if (dialogWait.elapsed() > 10000) { dialogDriver.stop(); dialog->reject(); return; }
            auto *filename = dialog->findChild<QLineEdit *>("fileNameEdit");
            auto *buttons = dialog->findChild<QDialogButtonBox *>(); if (!filename || !buttons) return;
            QTest::mouseClick(filename, Qt::LeftButton); QTest::keyClick(filename, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(filename, csvPath);
            auto *save = buttons->button(QDialogButtonBox::Save); if (!save || !save->isEnabled()) return;
            dialogDriver.stop(); dialogAccepted = true; QTest::mouseClick(save, Qt::LeftButton);
        });
        dialogDriver.start(); QVERIFY(clickControl(native->findChild<QPushButton *>("nativeResultExport"))); dialogDriver.stop();
        QVERIFY(dialogAccepted); QFile csv(csvPath); QVERIFY(csv.open(QIODevice::ReadOnly));
        const auto csvBytes = csv.readAll(); const auto lines = csvBytes.split('\n');
        const auto samples = frozen.value("samples").toArray(); QCOMPARE(lines.size(), samples.size() + 2);
        auto *table = native->findChild<QTableWidget *>("nativeResultTable"); QCOMPARE(table->rowCount(), samples.size());
        for (int index = 0; index < samples.size(); ++index)
        {
            const auto row = csvRow(lines[index + 1]); QCOMPARE(row.size(), 15);
            QCOMPARE(row[0], runJob); QCOMPARE(row[1], frozen.value("artifactId").toString());
            QCOMPARE(row[3], frozen.value("channelId").toString()); QCOMPARE(row[4], QString("m"));
            QCOMPARE(QJsonDocument::fromJson(row[6].toUtf8()).object(), frozen.value("provenance").toObject());
            QCOMPARE(row[9], table->item(index, 0)->text()); QCOMPARE(row[10], table->item(index, 1)->text()); QCOMPARE(row[11], table->item(index, 2)->text());
        }
        QCOMPARE(native->displaySnapshot(), frozen); QVERIFY(screenshot("05-native-passive-A-frozen-csv"));
        const QJsonObject evidence{{"platform", QApplication::platformName()}, {"projectWorkspace", project.path()},
            {"workflowWorkspace", workspace.path()}, {"sourceA", sourceA}, {"previewA", previewA},
            {"runResult", runResult}, {"activeSnapshot", activeSnapshot}, {"frozenPassiveSnapshot", frozen},
            {"csvPath", csvPath}, {"csvSha256", QString::fromLatin1(QCryptographicHash::hash(csvBytes, QCryptographicHash::Sha256).toHex())},
            {"invalidDraftBlockedApproval", true}, {"editedThenClosedOriginPreservedSourceA", true}};
        QFile report(QDir(evidenceDirectory).filePath("nondefault-beam-current-model.json"));
        QVERIFY(report.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray bytes = QJsonDocument(evidence).toJson(QJsonDocument::Indented); QCOMPARE(report.write(bytes), qint64(bytes.size()));
    }
};

int runCurrentModelWorkflowTests(int argc, char **argv)
{
    TestCurrentModelWorkflow test; return QTest::qExec(&test, argc, argv);
}

#include "tst_current_model_workflow.moc"
