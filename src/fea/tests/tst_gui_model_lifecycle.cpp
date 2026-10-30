#include <QtTest>
#include <QFile>
#include <QLineEdit>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QSemaphore>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTextDocument>
#include <QTextCursor>
#include <QUuid>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include <rmodel_command_session.h>
#include <rtrusted_mechanics_service.h>
#if RANGE_ENABLE_AGENT
#include <ragent_conversation_store.h>
#endif
#include <rbl_utils.h>
#include <rgl_logger_handler.h>
#include "application.h"
#include "central_tab_widget.h"
#include "action.h"
#include "model_command_host.h"
#include "model_io.h"
#include "modal_setup_widget.h"
#include "problem_tree.h"
#include "solver_manager.h"
#include "workflow_widget.h"
#include "control_monitor_widget.h"
#include <QDockWidget>
#if RANGE_ENABLE_AGENT
#include "agent_widget.h"
#endif
#include "extension_widget.h"
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QDialog>
#include <QMenu>
#include <QTabBar>
#include <QVBoxLayout>
#include <QToolButton>
#include <QAccessible>
#include "gl_widget.h"
#include "model_records_selector.h"

namespace
{
void lifecycleBootTrace(const char *stage)
{
    if (std::getenv("RANGE_LIFECYCLE_BOOT_TRACE")) {
        std::fprintf(stderr,"LIFECYCLE_BOOT %s\n",stage);
        std::fflush(stderr);
    }
}
int &lifecycleBootArgc(int &argc)
{
    lifecycleBootTrace("before QApplication constructor");
    return argc;
}
// Only startup dependencies are prepared here. The Application constructor,
// initialize(), MainWindow, Session, ProblemTree, actions and IO jobs are real.
class WindowApplication : public Application
{
public:
    WindowApplication(int &argc, char **argv) : Application(lifecycleBootArgc(argc), argv)
    {
        lifecycleBootTrace("after Application constructor");
        disconnect(this, &RApplication::started, this, nullptr);
        cloudSessionManager = new RCloudSessionManager(this);
        cloudConnectionHandler = new RCloudConnectionHandler(cloudSessionManager, applicationSettings, this);
#if RANGE_ENABLE_AGENT
        aiAgentSettingsManager = new RAgentSettingsManager(this);
#endif
        // QSettings isolation does not isolate QStandardPaths-based native jobs.
        // Ordinary invocations must not scan previous test runs' durable ledgers.
        QCoreApplication::setApplicationName(QStringLiteral("RangeFEA-Integration-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QStringList testArguments = QCoreApplication::arguments();
        if (testArguments.contains(QStringLiteral("--recovery-interaction"))
            || testArguments.contains(QStringLiteral("--model-commit-recovery")))
            for (const QString &role : {QStringLiteral("--producer"), QStringLiteral("--consumer"),
                                       QStringLiteral("--contender")})
            {
                const int roleIndex = testArguments.indexOf(role);
                if (roleIndex >= 0 && roleIndex + 1 < testArguments.size())
                    QCoreApplication::setApplicationName(QStringLiteral("RangeFEA-WF05-")
                        + QFileInfo(testArguments.at(roleIndex + 1)).fileName());
            }
        lifecycleBootTrace("before initialize");
        initialize(false);
        lifecycleBootTrace("after initialize");
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

QString currentDockTabTitle(const QMainWindow *window,
                            const QDockWidget *dock,
                            const QDockWidget *companion)
{
    for (auto *tabBar : window->findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly))
    {
        int dockIndex = -1;
        bool hasCompanion = false;
        for (int index = 0; index < tabBar->count(); ++index)
        {
            if (tabBar->tabText(index) == dock->windowTitle())
                dockIndex = index;
            if (tabBar->tabText(index) == companion->windowTitle())
                hasCompanion = true;
        }
        if (dockIndex >= 0 && hasCompanion)
        {
            const int currentIndex = tabBar->currentIndex();
            return currentIndex >= 0 ? tabBar->tabText(currentIndex) : QString();
        }
    }
    return {};
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

QJsonObject trustedLiveRodProblemSpec()
{
    const auto q = [](double value, const char *unit) {
        return QJsonObject{{QStringLiteral("value"), value}, {QStringLiteral("unit"), QString::fromLatin1(unit)}};
    };
    return {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("modelType"), QStringLiteral("axialRod")},
        {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), q(1.0, "m")},
            {QStringLiteral("segments"), 4}}},
        {QStringLiteral("material"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Steel")},
            {QStringLiteral("source"), QStringLiteral("fixture")},
            {QStringLiteral("youngModulus"), q(210e9, "Pa")},
            {QStringLiteral("poissonRatio"), 0.3}, {QStringLiteral("density"), q(7850.0, "kg/m3")}}},
        {QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), q(1e-4, "m2")}}},
        {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}}},
        {QStringLiteral("supports"), QJsonArray{
            QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
                {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}},
            QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}}}},
        {QStringLiteral("loads"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("pointForce")},
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("components"), QJsonArray{q(1000.0, "N"), q(0.0, "N"), q(0.0, "N")}}}}},
        {QStringLiteral("initialConditions"), QJsonArray{}},
        {QStringLiteral("observables"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("component"), QStringLiteral("ux")}}}},
        {QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}}}};
}

bool closeReportedMetric(double actual, double reference)
{
    const double tolerance = qMax(1e-24, qAbs(reference) * 5e-4);
    return qIsFinite(actual) && qAbs(actual - reference) <= tolerance;
}

bool exactTrustedRodPreview(const QString &origin, const QJsonObject &preview,
                            const QJsonObject &normalizedProblem, const QJsonObject &budgets,
                            const QString &documentId, quint64 revision)
{
    const QJsonObject arguments = preview.value(QStringLiteral("arguments")).toObject();
    const QJsonObject target = preview.value(QStringLiteral("target")).toObject();
    const bool rangesUnchanged = !arguments.contains(QStringLiteral("allowedNumericalRanges"))
        || arguments.value(QStringLiteral("allowedNumericalRanges")).toArray().isEmpty();
    const QJsonObject after = preview.value(QStringLiteral("after")).toObject();
    return origin == QStringLiteral("experiment")
        && preview.value(QStringLiteral("command")) == QJsonValue(QStringLiteral("experiment.run"))
        && arguments.value(QStringLiteral("problemSpec")).toObject() == normalizedProblem
        && arguments.value(QStringLiteral("budgets")).toObject() == budgets
        && rangesUnchanged
        && target.value(QStringLiteral("documentId")).toString() == documentId
        && preview.value(QStringLiteral("expectedDocumentRevision")).toDouble(-1.0) == static_cast<double>(revision)
        && preview.value(QStringLiteral("requiresHostApproval")).toBool()
        && after.value(QStringLiteral("sourceDocumentModified")) == QJsonValue(false)
        && after.value(QStringLiteral("maximumAttemptsThisRun")).toInt(-1) == 1;
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

class RecordingSelectorFixture : public ModelRecordsSelector
{
public:
    void arm(uint count) { ++playbackGeneration; pendingRecordUpdates = count; markNextIndicator = true; recordIndicator = true; }
    void loaded(uint modelID) { onUpdateJobFinished(modelID, playbackGeneration); }
    void cancel() { stopPlayback(); }
    bool recording() const { return recordIndicator; }
    uint pending() const { return pendingRecordUpdates; }
};
}

class TestGuiModelLifecycle : public QObject
{
    Q_OBJECT
private slots:
    void recordVideoWaitsForEachRenderedAndSavedFrame()
    {
        auto *session = Application::instance()->getSession();
        auto *window = Application::instance()->getMainWindow();
        for (uint id = 0; id < 2; ++id)
        {
            auto model = fixture(QStringLiteral("record-frame-%1").arg(id));
            model.setFileName(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".rbm");
            session->addModel(model);
        }
        QMap<uint, GLWidget *> views;
        for (auto *subWindow : window->findChildren<SubWindow *>())
        {
            // This ordering test supplies the image writer explicitly; the
            // separate installed test exercises the actual OpenGL framebuffer.
            QObject::disconnect(session, nullptr, subWindow, nullptr);
            views.insert(subWindow->getModelID(), subWindow->getGlWidget());
        }
        QCOMPARE(views.size(), 2);
        QStringList files;
        for (uint id = 0; id < 2; ++id)
        {
            const auto path = session->getModel(id).buildScreenShotFileName();
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QImage stale(2, 2, QImage::Format_RGB32); stale.fill(Qt::black);
            QVERIFY(stale.save(path));
            files.append(path);
        }
        RecordingSelectorFixture selector;
        QSignalSpy completed(&selector, &ModelRecordsSelector::recordFinished);
        QSignalSpy captures(session, &Session::takeScreenShot);
        const auto writer = QObject::connect(session, &Session::takeScreenShot, &selector,
            [](uint modelID, const QString &path) {
                QImage image(2, 2, QImage::Format_RGB32);
                image.fill(modelID == 0 ? Qt::red : Qt::blue);
                image.save(path);
            });
        selector.arm(2);
        selector.loaded(0); selector.loaded(1);
        QCOMPARE(captures.count(), 0);
        QCOMPARE(completed.count(), 0);
        QVERIFY(!QFileInfo::exists(files.at(0)));
        QVERIFY(!QFileInfo::exists(files.at(1)));
        QVERIFY(QMetaObject::invokeMethod(views.value(0), "frameSwapped", Qt::DirectConnection));
        QCOMPARE(captures.count(), 1);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(selector.pending(), 1u);
        QCOMPARE(QImage(files.at(0)).pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(QMetaObject::invokeMethod(views.value(1), "frameSwapped", Qt::DirectConnection));
        QCOMPARE(captures.count(), 2);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(selector.pending(), 0u);
        QCOMPARE(QImage(files.at(1)).pixelColor(0, 0), QColor(Qt::blue));
        selector.cancel(); // Invalidate the queued advance before processing events.
        QCoreApplication::processEvents();
        QCOMPARE(completed.count(), 1);
        QObject::disconnect(writer);
    }

    void recordVideoTimeoutCannotAdvanceOrUseStaleFrame()
    {
        auto *session = Application::instance()->getSession();
        auto model = fixture("record-timeout");
        model.setFileName(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".rbm");
        session->addModel(model);
        auto *view = Application::instance()->getMainWindow()->findChild<GLWidget *>();
        QVERIFY(view);
        const QString screenshot = session->getModel(0).buildScreenShotFileName();
        QImage stale(2, 2, QImage::Format_RGB32);
        stale.fill(Qt::black);
        QVERIFY(stale.save(screenshot));
        RecordingSelectorFixture selector;
        QSignalSpy completed(&selector, &ModelRecordsSelector::recordFinished);
        QSignalSpy captures(session, &Session::takeScreenShot);
        selector.arm(1); selector.loaded(0);
        QVERIFY(!QFileInfo::exists(screenshot));
        auto *timeout = selector.findChild<QTimer *>("recordFrameTimeout");
        QVERIFY(timeout);
        QVERIFY(QMetaObject::invokeMethod(timeout, "timeout", Qt::DirectConnection));
        QVERIFY(!selector.recording());
        QCOMPARE(selector.pending(), 0u);
        QVERIFY(QMetaObject::invokeMethod(view, "frameSwapped", Qt::DirectConnection));
        QCOMPARE(captures.count(), 0);
        QCOMPARE(completed.count(), 0);
    }

    void recordVideoSaveFailureStopsBeforeAdvance()
    {
        auto *session = Application::instance()->getSession();
        auto model = fixture("record-save-failure");
        model.setFileName(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".rbm");
        session->addModel(model);
        GLWidget *view = nullptr;
        for (auto *subWindow : Application::instance()->getMainWindow()->findChildren<SubWindow *>())
        {
            QObject::disconnect(session, nullptr, subWindow, nullptr);
            view = subWindow->getGlWidget();
        }
        QVERIFY(view);
        const QString screenshot = session->getModel(0).buildScreenShotFileName();
        QImage stale(2, 2, QImage::Format_RGB32);
        stale.fill(Qt::black);
        QVERIFY(stale.save(screenshot));
        RecordingSelectorFixture selector;
        QSignalSpy completed(&selector, &ModelRecordsSelector::recordFinished);
        QSignalSpy captures(session, &Session::takeScreenShot);
        selector.arm(1); selector.loaded(0);
        QVERIFY(QMetaObject::invokeMethod(view, "frameSwapped", Qt::DirectConnection));
        QCOMPARE(captures.count(), 1);
        QCOMPARE(completed.count(), 0);
        QVERIFY(!selector.recording());
        QCOMPARE(selector.pending(), 0u);
        QVERIFY(!QFileInfo::exists(screenshot));
    }

    void inspectSavedVideoDisplacements()
    {
        const QString directory = qEnvironmentVariable("RANGE_VIDEO_FIXTURE_DIR");
        const QString outputPath = qEnvironmentVariable("RANGE_VIDEO_DISPLACEMENT_RECEIPT");
    if (directory.isEmpty() || outputPath.isEmpty())
        QSKIP("external saved-video displacement fixture pair is not configured");
        QVERIFY(!directory.isEmpty());
        QVERIFY(!outputPath.isEmpty());
        QJsonArray frames;
        QStringList physicalIds;
        for (uint record = 1; record <= 20; ++record)
        {
            Model model;
            const QString fileName = QDir(directory).filePath(
                QStringLiteral("trusted-candidate-%1.rbm").arg(record, 5, 10, QLatin1Char('0')));
            model.read(fileName);
            QCOMPARE(model.getNNodes(), 5u);
            QJsonArray nodes;
            uint displacementPosition = RConstants::eod;
            for (uint variable = 0; variable < model.getNVariables(); ++variable)
                if (model.getVariable(variable).getType() == R_VARIABLE_DISPLACEMENT)
                    displacementPosition = variable;
            QVERIFY(displacementPosition != RConstants::eod);
            const RVariable &displacement = model.getVariable(displacementPosition);
            QCOMPARE(displacement.getNVectors(), 3u);
            QCOMPARE(displacement.getNValues(), model.getNNodes());
            for (uint node = 0; node < model.getNNodes(); ++node)
            {
                const RNode &geometry = model.getNode(node);
                QCOMPARE(geometry.getPhysicalId(), node + 1);
                if (record == 1) physicalIds.append(QString::number(geometry.getPhysicalId()));
                RRVector value = displacement.getValueVector(node);
                QCOMPARE(value.size(), uint(3));
                nodes.append(QJsonObject{{QStringLiteral("physicalNodeId"), int(geometry.getPhysicalId())},
                    {QStringLiteral("x"), geometry.getX()}, {QStringLiteral("y"), geometry.getY()},
                    {QStringLiteral("z"), geometry.getZ()}, {QStringLiteral("ux"), value[0]},
                    {QStringLiteral("uy"), value[1]}, {QStringLiteral("uz"), value[2]}});
            }
            frames.append(QJsonObject{{QStringLiteral("recordNo"), int(record)},
                {QStringLiteral("variableType"), QStringLiteral("var-displacement")},
                {QStringLiteral("nodes"), nodes}});
        }
        QCOMPARE(physicalIds, QStringList({"1", "2", "3", "4", "5"}));
        const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("schema"),
            QStringLiteral("range.stage8.saved-video-displacements/v1")},
            {QStringLiteral("source"), directory}, {QStringLiteral("frames"), frames}})
            .toJson(QJsonDocument::Indented);
        QSaveFile file(outputPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        QVERIFY(file.commit());
    }

    void missingApplicationLogDirectoryDoesNotReenterLogger()
    {
        if (!qEnvironmentVariableIsSet("RANGE_TEST_MISSING_LOG_DIRECTORY_CHILD"))
        {
            QProcess child;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.remove("RANGE_APPLICATION_PROFILE_DIR");
            environment.insert("RANGE_TEST_MISSING_LOG_DIRECTORY_CHILD", "1");
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("missingApplicationLogDirectoryDoesNotReenterLogger"),
                         QStringLiteral("-silent")});
            QVERIFY(child.waitForStarted(5000));
            const bool finished = child.waitForFinished(30000);
            if (!finished)
            {
                child.kill();
                child.waitForFinished(5000);
            }
            const QByteArray diagnostic = child.readAllStandardOutput() + child.readAllStandardError();
            QVERIFY2(finished, "Isolated missing-log-directory child timed out (possible logger reentry)");
            QCOMPARE(child.exitStatus(), QProcess::NormalExit);
            QVERIFY2(child.exitCode() == 0, diagnostic.constData());
            return;
        }

        // main() gives this child its own temporary QSettings/test-runtime tree.
        auto *central = Application::instance()->getMainWindow()->findChild<CentralTabWidget *>();
        QVERIFY(central);
        auto *output = central->findChild<RTextBrowser *>("applicationOutputBrowser");
        QVERIFY(output);
        const QString configuredLog = Application::instance()->getApplicationSettings()->getLogFileName();
        const QString configuredDirectory = QFileInfo(configuredLog).absolutePath();
        const QSettings settings;
        const QString settingsRoot = QFileInfo(settings.fileName()).absolutePath();
        QCOMPARE(QDir::cleanPath(configuredDirectory),
                 QDir::cleanPath(QDir(settingsRoot).filePath("test-runtime/log")));
        const QString movedDirectory = configuredDirectory + "-moved";
        QVERIFY(!QFileInfo::exists(movedDirectory));
        auto &logger = RLogger::getInstance();
        struct RestoreLogger {
            RLogger &logger;
            QString file;
            RLogLevelMask level;
            bool halted;
            ~RestoreLogger() {
                logger.setFile(file);
                logger.setLevel(level);
                logger.setHalted(halted);
            }
        } restore{logger, logger.getFile(), logger.getLevel(), logger.getHalted()};
        logger.setFile(configuredLog);
        logger.setLevel(R_LOG_LEVEL_DETAIL);
        logger.setHalted(false);
        logger.print(QStringLiteral("original-file-line-before-directory-move\n"));
        // Close before moving, so this test does not depend on Windows file
        // share-delete behavior. Reopen the same physical file at its new path.
        logger.setFile(QString());
        QVERIFY(QDir().rename(configuredDirectory, movedDirectory));
        const QString movedLog = movedDirectory + QDir::separator() + QFileInfo(configuredLog).fileName();
        logger.setFile(movedLog);
        QElapsedTimer timer;
        timer.start();
        logger.print(QStringLiteral("next-gui-message-after-directory-move\n"));
        QVERIFY(timer.elapsed() < 1000);
        QVERIFY(!QFileInfo::exists(configuredDirectory));
        QVERIFY(output->toPlainText().contains("next-gui-message-after-directory-move"));
        logger.setFile(restore.file);
        QFile file(movedLog);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray fullLog = file.readAll();
        QVERIFY(fullLog.contains("original-file-line-before-directory-move"));
        QVERIFY(fullLog.contains("next-gui-message-after-directory-move"));
    }

    void liveOutputKeepsBoundedRecentDiagnostics()
    {
        auto *central = Application::instance()->getMainWindow()->findChild<CentralTabWidget *>();
        QVERIFY(central);
        auto *output = central->findChild<RTextBrowser *>("processOutputBrowser");
        QVERIFY(output);
        output->clear();
        QVERIFY(!output->document()->isUndoRedoEnabled());
        int peakCharacters = 0;
        int peakBlocks = 0;
        const auto connection = connect(output->document(), &QTextDocument::contentsChanged, output, [&] {
            peakCharacters = qMax(peakCharacters, output->document()->characterCount());
            peakBlocks = qMax(peakBlocks, output->document()->blockCount());
        });
        // Disconnect even when a QtTest assertion returns early.
        struct DisconnectOnExit {
            QMetaObject::Connection connection;
            ~DisconnectOnExit() { QObject::disconnect(connection); }
        } disconnectOnExit{connection};
        auto &manager = SolverManager::getInstance();
        QSignalSpy stdoutSpy(&manager, &SolverManager::readyReadStandardOutput);
        QSignalSpy stderrSpy(&manager, &SolverManager::readyReadStandardError);
        const QString padding(144, QLatin1Char('x'));
        for (int index = 0; index < 2400; ++index)
        {
            manager.readyReadStandardOutput(QString("stdout-%1 %2\n").arg(index).arg(padding));
            if (index % 8 == 0)
                manager.readyReadStandardError(QString("stderr-%1\n").arg(index));
        }
        QCOMPARE(stdoutSpy.count(), 2400);
        QCOMPARE(stderrSpy.count(), 300);
        QVERIFY(output->toPlainText().contains("stdout-2399"));
        QVERIFY(!output->toPlainText().contains("stdout-0 "));
        QCOMPARE(stdoutSpy.first().first().toString(), QString("stdout-0 %1\n").arg(padding));

        QString huge(2 * 1024 * 1024, QLatin1Char('x'));
        huge += "newest-huge-line";
        const qsizetype cutoff = huge.size() - (256 * 1024 - 1);
        huge[cutoff - 1] = QChar(0xd83d);
        huge[cutoff] = QChar(0xde00);
        manager.readyReadStandardOutput(huge);
        QVERIFY(output->toPlainText().endsWith("newest-huge-line"));
        QVERIFY(!output->toPlainText().front().isLowSurrogate());
        QCOMPARE(stdoutSpy.last().first().toString(), huge);

        // Also cover a surrogate pair straddling the old-document trim point.
        output->clear();
        QString existing = QString(QChar(0xd83d)) + QChar(0xde00);
        existing += QString(256 * 1024 - 3, QLatin1Char('x'));
        manager.readyReadStandardOutput(existing);
        manager.readyReadStandardOutput("x");
        QVERIFY(!output->toPlainText().front().isLowSurrogate());

        // Exercise every paragraph separator interpreted by QTextCursor, CRLF
        // pairs, and a huge multi-line chunk. Record every observable document
        // mutation as well as the final state.
        QString manyLines;
        manyLines.reserve(400000);
        for (int index = 0; index < 30000; ++index)
            manyLines += QString("line-%1\r\n").arg(index);
        manyLines += QChar::ParagraphSeparator;
        manyLines += QChar(0xfdd0);
        manyLines += QChar(0xfdd1);
        manyLines += "newest-many-lines";
        manager.readyReadStandardOutput(manyLines);
        QVERIFY(output->toPlainText().endsWith("newest-many-lines"));

        manager.readyReadStandardError("final-error-diagnostic");
        QTextCursor error = output->document()->find("final-error-diagnostic");
        QVERIFY(!error.isNull());
        QCOMPARE(error.charFormat().foreground().color(), QColor(255, 255, 255));
        QCOMPARE(error.charFormat().background().color(), QColor(170, 0, 0));
        QCOMPARE(output->verticalScrollBar()->value(), output->verticalScrollBar()->maximum());
        manager.readyReadStandardOutput(" final-normal-diagnostic");
        QTextCursor normal = output->document()->find("final-normal-diagnostic");
        QCOMPARE(normal.charFormat().foreground().color(), QApplication::palette().text().color());
        QCOMPARE(normal.charFormat().background().color(), QApplication::palette().base().color());
        QVERIFY(output->toPlainText().endsWith("final-error-diagnostic final-normal-diagnostic"));
        QVERIFY(peakCharacters <= 256 * 1024);
        QVERIFY(peakBlocks <= 2000);
        QVERIFY(output->toolTip().contains("Recent output only"));
    }

    void solverOutputAlertsPreserveSelectedView()
    {
        auto *central = Application::instance()->getMainWindow()->findChild<CentralTabWidget *>();
        QVERIFY(central);
        auto *modelView = central->findChild<QMdiArea *>("mdiArea");
        auto *output = central->findChild<RTextBrowser *>("processOutputBrowser");
        QVERIFY(modelView); QVERIFY(output);
        const int selectedIndex = central->indexOf(modelView);
        const int processOutputIndex = central->indexOf(output);
        QVERIFY(selectedIndex >= 0); QVERIFY(processOutputIndex >= 0);

        struct RestoreSelection {
            CentralTabWidget *tabs;
            int selectedIndex;
            int processOutputIndex;
            ~RestoreSelection() {
                tabs->setCurrentIndex(processOutputIndex); // Clear the pending tab notice.
                tabs->setCurrentIndex(selectedIndex);
            }
        } restore{central, selectedIndex, processOutputIndex};
        central->setCurrentIndex(processOutputIndex); // Reset unread notice from preceding tests.

        central->setCurrentIndex(selectedIndex);
        auto &manager = SolverManager::getInstance();
        manager.jobStarted();
        QCOMPARE(central->currentIndex(), selectedIndex);
        QVERIFY(central->tabText(processOutputIndex).startsWith("* "));
        manager.jobFinished();
        QCOMPARE(central->currentIndex(), selectedIndex);

        const QString diagnostic = QStringLiteral("selection-preserving-failure-sentinel");
        manager.readyReadStandardError(diagnostic);
        QCOMPARE(central->currentIndex(), selectedIndex);
        manager.jobFailed();
        QCOMPARE(central->currentIndex(), selectedIndex);
        QVERIFY(output->toPlainText().contains(diagnostic));
        QVERIFY(central->tabText(processOutputIndex).contains("!!!"));
    }

    void applicationOutputLimitPreservesCompleteLogFile()
    {
        auto *central = Application::instance()->getMainWindow()->findChild<CentralTabWidget *>();
        QVERIFY(central);
        auto *output = central->findChild<RTextBrowser *>("applicationOutputBrowser");
        QVERIFY(output);
        output->clear();
        QVERIFY(!output->document()->isUndoRedoEnabled());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto &logger = RLogger::getInstance();
        struct RestoreLogger {
            RLogger &logger;
            QString file;
            RLogLevelMask level;
            bool halted;
            ~RestoreLogger() {
                logger.setFile(file);
                logger.setLevel(level);
                logger.setHalted(halted);
            }
        } restore{logger, logger.getFile(), logger.getLevel(), logger.getHalted()};
        const QString fullLog = directory.filePath("complete-output.log");
        logger.setFile(fullLog);
        logger.setLevel(R_LOG_LEVEL_DETAIL);
        logger.setHalted(false);
        QSignalSpy infoSpy(&RLoggerHandler::getInstance(), &RLoggerHandler::infoPrinted);
        const QString padding(144, QLatin1Char('x'));
        for (int index = 0; index < 2400; ++index)
            logger.print(QString("bounded-live-record-%1 %2\n").arg(index).arg(padding));
        QCOMPARE(infoSpy.count(), 2400);
        QVERIFY(output->toPlainText().contains("bounded-live-record-2399"));
        QVERIFY(!output->toPlainText().contains("bounded-live-record-0 "));
        logger.print(QStringLiteral("application-warning-sentinel\n"), RMessage::Type::Warning);
        QTextCursor warning = output->document()->find("application-warning-sentinel");
        QVERIFY(!warning.isNull());
        QCOMPARE(warning.charFormat().foreground().color(), QColor(170, 0, 0));
        logger.print(QStringLiteral("application-error-sentinel\n"), RMessage::Type::Error);
        QTextCursor error = output->document()->find("application-error-sentinel");
        QVERIFY(!error.isNull());
        QCOMPARE(error.charFormat().foreground().color(), QColor(255, 255, 255));
        QCOMPARE(error.charFormat().background().color(), QColor(170, 0, 0));
        QVERIFY(output->document()->characterCount() <= 256 * 1024);
        QVERIFY(output->document()->blockCount() <= 2000);
        // setFile closes and flushes the fixture's persistent file handle.
        logger.setFile(restore.file);
        QFile file(fullLog);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray log = file.readAll();
        QCOMPARE(log.count("bounded-live-record-"), 2400);
        QVERIFY(log.contains("bounded-live-record-0 "));
        QVERIFY(log.contains("bounded-live-record-2399 "));
        QVERIFY(log.contains("application-warning-sentinel"));
        QVERIFY(log.contains("application-error-sentinel"));
    }

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
        QStringList featureDockNames{"controlDock"};
#if RANGE_ENABLE_AGENT
        featureDockNames.prepend("agentDock");
#endif
        for (const auto &name : featureDockNames)
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

#if RANGE_ENABLE_AGENT
    void savedDockLayoutWithoutFeatureVersionIsPreservedAndFreshProfileUsesDefaults()
    {
        auto *application = Application::instance();
        auto *settings = application->getApplicationSettings();
        auto *layoutSource = application->getMainWindow();
        QVERIFY(settings); QVERIFY(layoutSource);

        const QString geometryKey = QStringLiteral("mainWindow/geometry");
        const QString stateKey = QStringLiteral("mainWindow/windowState");
        const QString versionKey = QStringLiteral("mainWindow/featureLayoutVersion");
        struct SavedSetting { QString key; QVariant value; bool present; };
        const QVector<SavedSetting> savedSettings = {
            {geometryKey, settings->value(geometryKey), settings->contains(geometryKey)},
            {stateKey, settings->value(stateKey), settings->contains(stateKey)},
            {versionKey, settings->value(versionKey), settings->contains(versionKey)}};
        struct RestoreSettings
        {
            QSettings *settings;
            QVector<SavedSetting> saved;
            ~RestoreSettings()
            {
                for (const auto &entry : saved)
                {
                    if (entry.present) settings->setValue(entry.key, entry.value);
                    else settings->remove(entry.key);
                }
                settings->sync();
            }
        } restoreSettings{settings, savedSettings};

        const QByteArray originalGeometry = layoutSource->saveGeometry();
        const QByteArray originalState = layoutSource->saveState();
        const bool sourceWasVisible = layoutSource->isVisible();
        struct RestoreSourceLayout
        {
            MainWindow *window;
            QByteArray geometry;
            QByteArray state;
            bool wasVisible;
            ~RestoreSourceLayout()
            {
                window->restoreState(state);
                window->restoreGeometry(geometry);
                if (!wasVisible) window->hide();
            }
        } restoreSource{layoutSource, originalGeometry, originalState, sourceWasVisible};

        auto *sourceModel = layoutSource->findChild<QDockWidget *>("modelDock");
        auto *sourceSolver = layoutSource->findChild<QDockWidget *>("solverDock");
        auto *sourceAgent = layoutSource->findChild<QDockWidget *>("agentDock");
        auto *sourceControl = layoutSource->findChild<QDockWidget *>("controlDock");
        QVERIFY(sourceModel); QVERIFY(sourceSolver); QVERIFY(sourceAgent); QVERIFY(sourceControl);

        // Build a real, valid pre-feature layout with visible semantic differences
        // from the fresh-profile defaults: the model and solver are tabified on the
        // left, the assistant floats, and the control dock is hidden.
        layoutSource->addDockWidget(Qt::LeftDockWidgetArea, sourceModel);
        layoutSource->addDockWidget(Qt::LeftDockWidgetArea, sourceSolver);
        layoutSource->tabifyDockWidget(sourceModel, sourceSolver);
        sourceAgent->setFloating(true);
        sourceAgent->resize(420, 320);
        sourceAgent->show();
        sourceControl->hide();
        layoutSource->resize(1180, 760);
        layoutSource->show();
        QTRY_VERIFY_WITH_TIMEOUT(sourceSolver->isVisible(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(sourceAgent->isVisible(), 3000);
        sourceSolver->raise();
        QTRY_COMPARE_WITH_TIMEOUT(currentDockTabTitle(layoutSource, sourceSolver, sourceModel),
                                  sourceSolver->windowTitle(), 3000);
        QVERIFY(currentDockTabTitle(layoutSource, sourceSolver, sourceModel)
                != sourceModel->windowTitle());
        QVERIFY(sourceControl->isHidden());
        QVERIFY(layoutSource->tabifiedDockWidgets(sourceModel).contains(sourceSolver));

        const QByteArray legacyState = layoutSource->saveState();
        const QByteArray legacyGeometry = layoutSource->saveGeometry();
        const QSize legacyAssistantSize = sourceAgent->size();
        QVERIFY(!legacyState.isEmpty());
        QVERIFY(legacyAssistantSize.width() >= 340);
        QVERIFY(legacyAssistantSize.height() >= 160);

        settings->setValue(geometryKey, legacyGeometry);
        settings->setValue(stateKey, legacyState);
        settings->remove(versionKey);
        settings->sync();
        QVERIFY(!settings->contains(versionKey));

        {
            MainWindow restored(application->getSession(), application->getCloudSessionManager(),
                                settings, application->getActionList());
            auto *model = restored.findChild<QDockWidget *>("modelDock");
            auto *solver = restored.findChild<QDockWidget *>("solverDock");
            auto *agent = restored.findChild<QDockWidget *>("agentDock");
            auto *control = restored.findChild<QDockWidget *>("controlDock");
            QVERIFY(model); QVERIFY(solver); QVERIFY(agent); QVERIFY(control);
            QCOMPARE(settings->value(versionKey).toInt(), 1);

            restored.show();
            QTRY_VERIFY_WITH_TIMEOUT(solver->isVisible(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(agent->isVisible(), 3000);
            QTRY_COMPARE_WITH_TIMEOUT(currentDockTabTitle(&restored, solver, model),
                                      solver->windowTitle(), 3000);
            QVERIFY(currentDockTabTitle(&restored, solver, model) != model->windowTitle());
            QCOMPARE(restored.dockWidgetArea(model), Qt::LeftDockWidgetArea);
            QCOMPARE(restored.dockWidgetArea(solver), Qt::LeftDockWidgetArea);
            QVERIFY(restored.tabifiedDockWidgets(model).contains(solver));
            QVERIFY(!model->isFloating());
            QVERIFY(!solver->isFloating());
            QVERIFY(solver->isVisible());
            QVERIFY(agent->isFloating());
            QVERIFY(control->isHidden());
            QVERIFY(qAbs(agent->width() - legacyAssistantSize.width()) <= 40);
            QVERIFY(qAbs(agent->height() - legacyAssistantSize.height()) <= 40);
        }

        settings->remove(geometryKey);
        settings->remove(stateKey);
        settings->remove(versionKey);
        settings->sync();
        {
            MainWindow fresh(application->getSession(), application->getCloudSessionManager(),
                             settings, application->getActionList());
            auto *model = fresh.findChild<QDockWidget *>("modelDock");
            auto *solver = fresh.findChild<QDockWidget *>("solverDock");
            auto *agent = fresh.findChild<QDockWidget *>("agentDock");
            auto *control = fresh.findChild<QDockWidget *>("controlDock");
            QVERIFY(model); QVERIFY(solver); QVERIFY(agent); QVERIFY(control);
            QCOMPARE(settings->value(versionKey).toInt(), 1);

            fresh.resize(1280, 800);
            fresh.show();
            QTRY_VERIFY_WITH_TIMEOUT(solver->isVisible(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(control->isVisible(), 3000);
            QTRY_COMPARE_WITH_TIMEOUT(currentDockTabTitle(&fresh, agent, solver),
                                      agent->windowTitle(), 3000);
            QVERIFY(currentDockTabTitle(&fresh, agent, solver) != solver->windowTitle());
            QCOMPARE(fresh.dockWidgetArea(model), Qt::LeftDockWidgetArea);
            QCOMPARE(fresh.dockWidgetArea(solver), Qt::RightDockWidgetArea);
            QCOMPARE(fresh.dockWidgetArea(agent), Qt::RightDockWidgetArea);
            QCOMPARE(fresh.dockWidgetArea(control), Qt::BottomDockWidgetArea);
            QVERIFY(!agent->isFloating());
            QVERIFY(!control->isFloating());
            QVERIFY(!model->isHidden());
            QVERIFY(!agent->isHidden());
            QVERIFY(!control->isHidden());
            const auto sizeText = [](const QSize &size) {
                return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
            };
            const auto dockText = [&](const char *name, const QDockWidget *dock) {
                return QStringLiteral("%1 size=%2 geometry=%3 min=%4 minHint=%5")
                    .arg(QString::fromLatin1(name))
                    .arg(sizeText(dock->size()))
                    .arg(sizeText(dock->geometry().size()))
                    .arg(sizeText(dock->minimumSize()))
                    .arg(sizeText(dock->minimumSizeHint()));
            };
            const auto layoutDiagnostic = [&] {
                return QStringLiteral("window=%1; %2; %3; %4; %5")
                    .arg(sizeText(fresh.size()))
                    .arg(dockText("model", model))
                    .arg(dockText("solver", solver))
                    .arg(dockText("agent", agent))
                    .arg(dockText("control", control));
            };

            // Measure the active solver tab for the shared right-dock allocation;
            // the inactive assistant widget may retain stale geometry.
            solver->raise();
            QTRY_COMPARE_WITH_TIMEOUT(currentDockTabTitle(&fresh, solver, agent),
                                      solver->windowTitle(), 3000);
            const QString solverDiagnostic = layoutDiagnostic();
            QVERIFY2(qAbs(model->width() - 190) <= 70, qPrintable(solverDiagnostic));
            QVERIFY2(qAbs(solver->width() - 340) <= 70, qPrintable(solverDiagnostic));
            QVERIFY2(qAbs(control->height() - 220) <= 70, qPrintable(solverDiagnostic));

            agent->raise();
            QTRY_COMPARE_WITH_TIMEOUT(currentDockTabTitle(&fresh, agent, solver),
                                      agent->windowTitle(), 3000);
            const QString agentDiagnostic = layoutDiagnostic();
            QVERIFY2(qAbs(agent->width() - 340) <= 70, qPrintable(agentDiagnostic));

            solver->raise();
            QTRY_COMPARE_WITH_TIMEOUT(currentDockTabTitle(&fresh, solver, agent),
                                      solver->windowTitle(), 3000);
        }
    }

    void dockLayoutsShrinkWithoutLosingControls()
    {
        auto *window = Application::instance()->getMainWindow();
        auto *session = Application::instance()->getSession();
        auto *controlDock = window->findChild<QDockWidget *>("controlDock");
        auto *agentDock = window->findChild<QDockWidget *>("agentDock");
        auto *workflow = window->findChild<WorkflowWidget *>();
        auto *agent = window->findChild<AgentWidget *>();
        QVERIFY(controlDock); QVERIFY(agentDock); QVERIFY(workflow); QVERIFY(agent);
        auto *controlScroll = qobject_cast<QScrollArea *>(controlDock->widget());
        auto *historyScroll = agent->findChild<QScrollArea *>("agentHistoryScroll");
        auto *pages = workflow->findChild<QTabWidget *>("workflowPages");
        auto *goal = agent->findChild<QPlainTextEdit *>("agentGoal");
        auto *preview = workflow->findChild<QPushButton *>("piezoPreview");
        auto *run = workflow->findChild<QPushButton *>("piezoRun");
        auto *confirm = workflow->findChild<QPushButton *>("piezoConfirm");
        auto *previewPanel = workflow->findChild<QWidget *>("piezoPreviewPanel");
        QVERIFY(controlScroll); QCOMPARE(controlScroll->widget(), workflow);
        QVERIFY(historyScroll); QVERIFY(pages); QVERIFY(goal); QVERIFY(preview); QVERIFY(run); QVERIFY(confirm); QVERIFY(previewPanel);
        QVERIFY(!historyScroll->isAncestorOf(goal));
        QCOMPARE(goal->parentWidget(), agent);
        QVERIFY(pages->count() > 2);
        const auto savedGeometry = window->saveGeometry(), savedState = window->saveState();
        QWidget *inactivePage = pages->widget(1);
        const auto savedMinimum = inactivePage->minimumSize();
        const auto savedHistoryMinimum = historyScroll->widget()->minimumSize();
        const auto savedGoal = goal->toPlainText();
        struct Restore {
            MainWindow *window; QByteArray geometry, state; QWidget *page, *history;
            QSize pageMinimum, historyMinimum; QPlainTextEdit *goal; QString goalText; bool visible;
            ~Restore() {
                page->setMinimumSize(pageMinimum); history->setMinimumSize(historyMinimum);
                goal->setPlainText(goalText); window->restoreState(state); window->restoreGeometry(geometry);
                if (!visible) window->hide();
            }
        } restore{window, savedGeometry, savedState, inactivePage, historyScroll->widget(),
                  savedMinimum, savedHistoryMinimum, goal, savedGoal, window->isVisible()};
        // The real session creates its ordinary source viewport and refreshes
        // all feature pages; the inactive-page bound then reproduces growth
        // from a long form without depending on font/driver-specific hints.
        session->addModel(fixture("Responsive dock source"));
        session->setModelSelected(0, true);
        auto *piezoPage = workflow->findChild<QWidget *>("piezoWorkbench");
        QVERIFY(piezoPage); QVERIFY(pages->indexOf(piezoPage) >= 0);
        pages->setCurrentWidget(piezoPage);
        inactivePage->setMinimumSize(1700, 1500);
        historyScroll->widget()->setMinimumHeight(1500);
        window->show(); agentDock->show(); agentDock->raise();
        controlDock->setFloating(true); controlDock->show();
        // Retain enough width for the application's actual toolbars/composer;
        // both sizes remain well below the inactive page's forced minimum.
        for (const auto &size : {QSize(1000, 600), QSize(900, 480)}) {
            window->resize(size); controlDock->resize(size);
            QTRY_COMPARE_WITH_TIMEOUT(window->size(), size, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(controlDock->size(), size, 3000);
            QVERIFY(window->isVisible()); QVERIFY(controlDock->isVisible());
            QVERIFY(!window->isMinimized()); QVERIFY(!controlDock->isMinimized());
            QVERIFY(controlScroll->verticalScrollBar()->maximum() > 0);
            QVERIFY(historyScroll->verticalScrollBar()->maximum() > 0);
            QVERIFY(goal->isVisible());
            const auto goalRect = QRect(goal->mapTo(agent, QPoint()), goal->size());
            QVERIFY(agent->rect().contains(goalRect));
            for (const auto *name : {"agentSend", "agentStop", "agentAskReadOnly"}) {
                auto *button = agent->findChild<QPushButton *>(name); QVERIFY(button);
                QVERIFY(!historyScroll->isAncestorOf(button));
                // Stop is intentionally hidden while no assistant task runs.
                if (QByteArray(name) == "agentStop") { QVERIFY(button->isHidden()); continue; }
                QVERIFY(button->isVisible());
                QVERIFY(agent->rect().contains(QRect(button->mapTo(agent, QPoint()), button->size())));
            }
            const auto composerPosition = goal->mapTo(agent, QPoint());
            historyScroll->verticalScrollBar()->setValue(historyScroll->verticalScrollBar()->maximum());
            QCoreApplication::processEvents();
            QCOMPARE(goal->mapTo(agent, QPoint()), composerPosition);
            goal->setPlainText("Composer remains usable while history scrolls.");
            QCOMPARE(goal->toPlainText(), QString("Composer remains usable while history scrolls."));
            // Initial Preview and Run controls must be visible without a
            // scroll-to-button repair, even when another workflow page is wide.
            for (auto *button : {preview, run}) {
                QTRY_VERIFY_WITH_TIMEOUT(controlScroll->viewport()->rect().contains(
                    QRect(button->mapTo(controlScroll->viewport(), QPoint()), button->size())), 3000);
                QVERIFY(button->isVisible());
            }
            QCOMPARE(confirm->parentWidget(), previewPanel);
            QVERIFY(previewPanel->isHidden());
            QVERIFY(!confirm->isVisible());
            QCOMPARE(workflow->findChild<QPushButton *>("piezoPreview"), preview);
            QCOMPARE(workflow->findChild<QPushButton *>("piezoRun"), run);
            QCOMPARE(workflow->findChild<QPushButton *>("piezoConfirm"), confirm);
            auto *accessible = QAccessible::queryAccessibleInterface(run); QVERIFY(accessible);
            QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
        }
        for (const auto *name : {"modelDock", "solverDock"}) {
            auto *dock = window->findChild<QDockWidget *>(name); QVERIFY(dock);
            auto *tabs = dock->findChild<QTabWidget *>(); QVERIFY(tabs);
            for (int index = 0; index < tabs->count(); ++index) {
                auto *scroll = qobject_cast<QScrollArea *>(tabs->widget(index));
                QVERIFY(scroll); QVERIFY(scroll->widget()); QVERIFY(scroll->widgetResizable());
            }
        }
    }
#endif

    void workflowModuleMenuNavigatesEveryExistingPage()
    {
        auto *window = Application::instance()->getMainWindow();
        auto *workflow = window->findChild<WorkflowWidget *>();
        auto *controlDock = window->findChild<QDockWidget *>("controlDock");
        QVERIFY(workflow); QVERIFY(controlDock);
        auto *pages = workflow->findChild<QTabWidget *>("workflowPages");
        auto *moduleButton = workflow->findChild<QToolButton *>("workflowModuleMenu");
        auto *workspace = workflow->findChild<QLineEdit *>("workflowWorkspace");
        QVERIFY(pages); QVERIFY(moduleButton); QVERIFY(workspace);
        auto *menu = moduleButton->menu();
        QVERIFY(menu);
        QVERIFY(pages->count() > 1);
        QCOMPARE(menu->actions().size(), pages->count());
        const auto *accessible = QAccessible::queryAccessibleInterface(moduleButton);
        QVERIFY(accessible);
        QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
        QVERIFY(moduleButton->focusPolicy() != Qt::NoFocus);

        const QByteArray savedState = window->saveState();
        const QByteArray savedGeometry = window->saveGeometry();
        const int originalPage = pages->currentIndex();
        const QString originalWorkspace = workspace->text();
        const bool windowWasVisible = window->isVisible();
        const bool controlDockWasHidden = controlDock->isHidden();
        struct Restore {
            MainWindow *window;
            QByteArray state;
            QByteArray geometry;
            QTabWidget *pages;
            int page;
            QLineEdit *workspace;
            QString workspaceText;
            QDockWidget *controlDock;
            QMenu *menu;
            bool controlDockWasHidden;
            bool windowWasVisible;
            ~Restore() {
                menu->close();
                pages->setCurrentIndex(page);
                workspace->setText(workspaceText);
                window->restoreState(state);
                window->restoreGeometry(geometry);
                controlDock->setVisible(!controlDockWasHidden);
                if (!windowWasVisible) window->hide();
            }
        } restore{window, savedState, savedGeometry, pages, originalPage, workspace,
                  originalWorkspace, controlDock, menu, controlDockWasHidden, windowWasVisible};

        const QString stateMarker = QStringLiteral("module-menu-state-preserved");
        workspace->setText(stateMarker);
        window->show();
        window->activateWindow();
        controlDock->show();
        controlDock->raise();
        QTRY_VERIFY_WITH_TIMEOUT(moduleButton->isVisible(), 3000);

        moduleButton->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(moduleButton));

        bool popupAboutToShow = false;
        bool popupObserved = false;
        bool escapeDismissedPopup = false;
        bool popupWatchdogExpired = false;
        QTimer escapePoll;
        escapePoll.setSingleShot(false);
        escapePoll.setInterval(10);
        QTimer popupWatchdog;
        popupWatchdog.setSingleShot(true);
        popupWatchdog.setInterval(3000);
        connect(menu, &QMenu::aboutToShow, menu, [&popupAboutToShow] {
            popupAboutToShow = true;
        });
        connect(&escapePoll, &QTimer::timeout, menu, [&] {
            if (!menu->isVisible()) return;
            popupObserved = true;
            escapePoll.stop();
            QTest::keyClick(menu, Qt::Key_Escape);
            escapeDismissedPopup = !menu->isVisible();
            if (menu->isVisible()) menu->close();
            popupWatchdog.stop();
        });
        connect(&popupWatchdog, &QTimer::timeout, menu, [&] {
            popupWatchdogExpired = true;
            escapePoll.stop();
            menu->close();
        });
        escapePoll.start();
        popupWatchdog.start();
        // Let the repeating check observe the closed state before the Space
        // event starts QToolButton::showMenu's nested event loop.
        QTest::qWait(25);
        QTest::keyClick(moduleButton, Qt::Key_Space);
        escapePoll.stop();
        popupWatchdog.stop();
        QVERIFY2(popupAboutToShow, "Keyboard activation did not open the module menu");
        QVERIFY2(popupObserved, "The module popup was not observed before its watchdog");
        QVERIFY2(!popupWatchdogExpired, "The module popup needed the watchdog to close");
        QVERIFY2(escapeDismissedPopup, "Escape did not dismiss the keyboard-opened module menu");
        QVERIFY(!menu->isVisible());

        for (int index = 0; index < pages->count(); ++index)
        {
            auto *page = pages->widget(index);
            auto *action = menu->actions().at(index);
            QCOMPARE(action->text(), pages->tabText(index));
            const auto popupPoint = moduleButton->mapToGlobal(
                QPoint(moduleButton->width() / 2, moduleButton->height()));
            menu->popup(popupPoint);
            QTRY_VERIFY_WITH_TIMEOUT(menu->isVisible(), 3000);

            int checkedCount = 0;
            for (auto *candidate : menu->actions())
                if (candidate->isChecked()) ++checkedCount;
            QCOMPARE(checkedCount, 1);
            QCOMPARE(action->isChecked(), pages->currentWidget() == page);

            const QRect actionRect = menu->actionGeometry(action);
            QVERIFY(actionRect.isValid());
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, actionRect.center());
            QTRY_COMPARE_WITH_TIMEOUT(pages->currentWidget(), page, 3000);
            QVERIFY(action->isChecked());
            QCOMPARE(workspace->text(), stateMarker);
        }
        QCOMPARE(pages->count(), menu->actions().size());
    }

#if RANGE_ENABLE_AGENT
    void agentComposerFollowsVisualKeyboardTabOrder()
    {
        auto *window = Application::instance()->getMainWindow();
        auto *workflow = window->findChild<WorkflowWidget *>();
        auto *session = Application::instance()->getSession();
        QVERIFY(workflow); QVERIFY(session);

        QDialog dialog;
        auto *layout = new QVBoxLayout(&dialog);
        AgentWidget agent(session->getModelCommandHost(), workflow->transport(), &dialog,
                          nullptr, AgentTaskRunner::AccessPolicy::ReadWrite, false);
        layout->addWidget(&agent);
        dialog.resize(720, 640);
        dialog.show();

        auto *goal = agent.findChild<QPlainTextEdit *>("agentGoal");
        auto *send = agent.findChild<QPushButton *>("agentSend");
        auto *readOnly = agent.findChild<QPushButton *>("agentAskReadOnly");
        auto *model = agent.findChild<QComboBox *>("agentModel");
        auto *effort = agent.findChild<QComboBox *>("agentThinkingEffort");
        auto *stream = agent.findChild<QCheckBox *>("agentStreamResponses");
        auto *automatic = agent.findChild<QCheckBox *>("agentAutomaticChanges");
        QVERIFY(goal); QVERIFY(send); QVERIFY(readOnly); QVERIFY(model); QVERIFY(effort);
        QVERIFY(stream); QVERIFY(automatic);
        QVERIFY(goal->tabChangesFocus());
        const auto focusedModelWidget = [model]() -> QWidget * {
            QWidget *focus = QApplication::focusWidget();
            return focus == model || focus == model->lineEdit() ? focus : nullptr;
        };
        goal->setPlainText("Tab order acceptance draft.");

        goal->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(goal));
        QTest::keyClick(goal, Qt::Key_Tab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(send));
        QTest::keyClick(send, Qt::Key_Tab); // Hidden Stop is skipped.
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(readOnly));
        QTest::keyClick(readOnly, Qt::Key_Tab);
        QTRY_VERIFY(focusedModelWidget());
        QTest::keyClick(focusedModelWidget(), Qt::Key_Tab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(effort));
        QTest::keyClick(effort, Qt::Key_Tab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(stream));
        QTest::keyClick(stream, Qt::Key_Tab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(automatic));

        QTest::keyClick(automatic, Qt::Key_Backtab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(stream));
        QTest::keyClick(stream, Qt::Key_Backtab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(effort));
        QTest::keyClick(effort, Qt::Key_Backtab);
        QTRY_VERIFY(focusedModelWidget());
        QTest::keyClick(focusedModelWidget(), Qt::Key_Backtab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(readOnly));
        QTest::keyClick(readOnly, Qt::Key_Backtab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(send));
        QTest::keyClick(send, Qt::Key_Backtab);
        QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget *>(goal));
    }
#endif

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

#if RANGE_ENABLE_AGENT
    void trustedRodPreviewGateRejectsChangedInputs()
    {
        const QJsonObject input = trustedLiveRodProblemSpec();
        const auto validation = RTrustedMechanicsService::validate(input);
        QVERIFY(validation.valid);
        const QJsonObject budgets{{QStringLiteral("solves"), 1},
            {QStringLiteral("repairs"), 0}, {QStringLiteral("modelTurns"), 12}};
        const QString documentId = QStringLiteral("fixture-source-document");
        QJsonObject preview{{QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("experimentId"), QStringLiteral("fixture-experiment")},
            {QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                {QStringLiteral("sessionEpoch"), QStringLiteral("fixture-session")}}},
            {QStringLiteral("expectedDocumentRevision"), 3},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), validation.normalizedSpec},
                {QStringLiteral("allowedNumericalRanges"), QJsonArray{}}, {QStringLiteral("budgets"), budgets}}},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("after"), QJsonObject{{QStringLiteral("sourceDocumentModified"), false},
                {QStringLiteral("maximumAttemptsThisRun"), 1}}}};
        QVERIFY(exactTrustedRodPreview(QStringLiteral("experiment"), preview,
            validation.normalizedSpec, budgets, documentId, 3));
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("document"), preview,
            validation.normalizedSpec, budgets, documentId, 3));
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), preview,
            validation.normalizedSpec, budgets, documentId, 4));

        auto changed = preview;
        changed.insert(QStringLiteral("requiresHostApproval"), false);
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), changed,
            validation.normalizedSpec, budgets, documentId, 3));
        changed = preview;
        changed.insert(QStringLiteral("after"), QJsonObject{{QStringLiteral("sourceDocumentModified"), true},
            {QStringLiteral("maximumAttemptsThisRun"), 1}});
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), changed,
            validation.normalizedSpec, budgets, documentId, 3));
        changed = preview;
        changed.insert(QStringLiteral("after"), QJsonObject{{QStringLiteral("sourceDocumentModified"), false},
            {QStringLiteral("maximumAttemptsThisRun"), 2}});
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), changed,
            validation.normalizedSpec, budgets, documentId, 3));
        changed = preview;
        QJsonObject arguments = changed.value(QStringLiteral("arguments")).toObject();
        QJsonObject changedBudgets = budgets;
        changedBudgets.insert(QStringLiteral("solves"), 2);
        arguments.insert(QStringLiteral("budgets"), changedBudgets);
        changed.insert(QStringLiteral("arguments"), arguments);
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), changed,
            validation.normalizedSpec, budgets, documentId, 3));
        changed = preview;
        arguments = changed.value(QStringLiteral("arguments")).toObject();
        QJsonObject changedProblem = arguments.value(QStringLiteral("problemSpec")).toObject();
        QJsonObject material = changedProblem.value(QStringLiteral("material")).toObject();
        material.insert(QStringLiteral("youngModulus"), 200e9);
        changedProblem.insert(QStringLiteral("material"), material);
        arguments.insert(QStringLiteral("problemSpec"), changedProblem);
        changed.insert(QStringLiteral("arguments"), arguments);
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), changed,
            validation.normalizedSpec, budgets, documentId, 3));
        changed = preview;
        QJsonObject target = changed.value(QStringLiteral("target")).toObject();
        target.insert(QStringLiteral("documentId"), QStringLiteral("other-document"));
        changed.insert(QStringLiteral("target"), target);
        QVERIFY(!exactTrustedRodPreview(QStringLiteral("experiment"), changed,
            validation.normalizedSpec, budgets, documentId, 3));
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
        QTemporaryDir conversationWorkspace;
        QVERIFY(conversationWorkspace.isValid());
        QString workspaceError;
        QVERIFY2(session->openProject(conversationWorkspace.path(), &workspaceError), qPrintable(workspaceError));
        session->addModel(fixture("Remote model acceptance"));
        session->setModelSelected(0, true);
        auto *widget = Application::instance()->getMainWindow()->findChild<AgentWidget *>();
        QVERIFY(widget);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setModel(qEnvironmentVariable("RANGE_TEST_DEEPSEEK_MODEL", settings.getModel()));
        settings.setApiKey(apiKey);
        widget->setProviderSettings(settings);
        if (qEnvironmentVariableIsSet("RANGE_TEST_DEEPSEEK_STREAM"))
        {
            auto *streaming = widget->findChild<QCheckBox *>("agentStreamResponses");
            QVERIFY(streaming);
            streaming->setChecked(true);
        }
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
        if (qEnvironmentVariableIsSet("RANGE_TEST_DEEPSEEK_READONLY"))
        {
            const auto acceptedStamp = session->modelCommandStamp(0);
            const auto acceptedProposal = widget->taskRunner()->pendingProposal();
            auto *composer = widget->findChild<QPlainTextEdit *>("agentGoal");
            QVERIFY(composer);
            composer->setPlainText(QStringLiteral("Preserved main draft during the independent question."));
            widget->startReadOnlyQuestion(QStringLiteral("Using the selected GUI document's current supplied context or document_query, report its modal iteration count. Reply with the decimal integer only. Do not propose or execute changes."));
            auto *readOnly = widget->readOnlyWidget();
            QVERIFY(readOnly);
            QSignalSpy readOnlyFinished(readOnly->taskRunner(), &AgentTaskRunner::finished);
            QSignalSpy readOnlyActions(readOnly->taskRunner(), &AgentTaskRunner::actionCompleted);
            QTRY_COMPARE_WITH_TIMEOUT(readOnlyFinished.size(), 1, 180000);
            QCOMPARE(readOnlyFinished.last().at(0).toString(), QStringLiteral("completed"));
            QCOMPARE(readOnlyFinished.last().at(1).toString().trimmed(), QStringLiteral("53"));
            QCOMPARE(readOnlyActions.size(), 0);
            QCOMPARE(session->modelCommandStamp(0), acceptedStamp);
            QCOMPARE(widget->taskRunner()->pendingProposal(), acceptedProposal);
            QCOMPARE(composer->toPlainText(), QStringLiteral("Preserved main draft during the independent question."));
            QVERIFY(readOnly->conversationId() != widget->conversationId());
            RAgentConversationStore store;
            const auto identity = session->getModelCommandHost()->handle(
                {{"apiVersion", "range.host/v1"}, {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                 {"operation", "project.query"}, {"parameters", QJsonObject{}}}).value("result").toObject();
            QVERIFY2(store.configure(session->getModelCommandHost()->projectWorkspace(),
                                    identity.value("projectId").toString(), &workspaceError), qPrintable(workspaceError));
            const auto saved = store.exportConversation(readOnly->conversationId(), &workspaceError);
            QVERIFY2(!saved.isEmpty(), qPrintable(workspaceError));
            QVERIFY(!QJsonDocument(saved).toJson(QJsonDocument::Compact).contains(apiKey.toUtf8()));
            qInfo("DeepSeek live read-only answer: source unchanged, no action, independent conversation persisted.");
        }
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

    void deepSeekRunsTrustedRodExperiment()
    {
        const auto credentialsPath = qEnvironmentVariable("RANGE_TEST_DEEPSEEK_CREDENTIALS_FILE");
        if (credentialsPath.isEmpty()) QSKIP("Opt-in live DeepSeek trusted-experiment acceptance; credentials remain local.");
        const auto modelName = qEnvironmentVariable("RANGE_TEST_DEEPSEEK_MODEL");
        if (modelName != QStringLiteral("deepseek-v4-pro"))
            QSKIP("This acceptance requires the explicitly configured DeepSeek Pro model.");
        const auto evidencePath = qEnvironmentVariable("RANGE_TEST_EVIDENCE_EXPORT");
        if (evidencePath.isEmpty()) QSKIP("Set RANGE_TEST_EVIDENCE_EXPORT to preserve the Host and Agent evidence.");

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

        Session *session = Application::instance()->getSession();
        QTemporaryDir projectWorkspace;
        QVERIFY(projectWorkspace.isValid());
        QString workspaceError;
        qInfo("Trusted rod live acceptance: opening isolated project.");
        QVERIFY2(session->openProject(projectWorkspace.path(), &workspaceError), qPrintable(workspaceError));
        session->addModel(fixture("Disposable trusted rod source"));
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto sourceStamp = session->modelCommandStamp(0);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(0));
        qInfo("Trusted rod live acceptance: source fixture ready.");

        auto *widget = Application::instance()->getMainWindow()->findChild<AgentWidget *>();
        QVERIFY(widget);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setModel(modelName);
        settings.setApiKey(apiKey);
        widget->setProviderSettings(settings);
        const auto clearProviderKey = qScopeGuard([widget, settings]() mutable {
            settings.setApiKey({});
            widget->setProviderSettings(settings);
        });
        auto *effort = widget->findChild<QComboBox *>("agentThinkingEffort");
        QVERIFY(effort);
        const int effortIndex = effort->findData(qEnvironmentVariable("RANGE_TEST_DEEPSEEK_EFFORT", "off"));
        QVERIFY(effortIndex >= 0);
        effort->setCurrentIndex(effortIndex);
        auto *streaming = widget->findChild<QCheckBox *>("agentStreamResponses");
        QVERIFY(streaming);
        streaming->setChecked(false);

        AgentTaskRunner *runner = widget->taskRunner();
        QVERIFY(runner);
        const QJsonObject expectedProblem = trustedLiveRodProblemSpec();
        const QJsonObject expectedBudgets{{QStringLiteral("solves"), 1},
            {QStringLiteral("repairs"), 0}, {QStringLiteral("modelTurns"), 12}};
        QSignalSpy finished(runner, &AgentTaskRunner::finished);
        QSignalSpy actions(runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy activity(runner, &AgentTaskRunner::toolActivity);
        QSignalSpy observed(runner, &AgentTaskRunner::experimentObserved);
        QSignalSpy assistantMessages(runner, &AgentTaskRunner::assistantMessage);
        QSignalSpy questions(runner, &AgentTaskRunner::userInputRequested);
        QSignalSpy usage(widget, &AgentWidget::providerUsage);
        int proposalCount = 0;
        int acceptedProposalCount = 0;
        QString experimentId;
        QString proposalError;
        QJsonObject acceptedPreview;
        const auto expectedValidation = RTrustedMechanicsService::validate(expectedProblem);
        QVERIFY(expectedValidation.valid);
        const QJsonObject expectedNormalizedProblem = expectedValidation.normalizedSpec;
        QObject approvalScope;
        connect(runner, &AgentTaskRunner::userInputRequested, &approvalScope,
            [runner](const QString &, const QString &, const QJsonArray &) {
                // A live acceptance test cannot answer a physical clarification
                // on the user's behalf. Stop immediately and preserve that fact.
                qInfo("Trusted rod live acceptance: Agent requested clarification; cancelling without guessing.");
                QTimer::singleShot(0, runner, [runner] { runner->cancel(); });
            });
        connect(runner, &AgentTaskRunner::toolActivity, &approvalScope,
            [](const QString &, const QString &, const QString &toolName, const QString &phase,
               const QString &, const QJsonObject &) {
                // Log only fixed tool names and phases; never prompt or tool data.
                qInfo().noquote() << "Trusted rod live acceptance tool activity:" << toolName << phase;
            });
        const auto approvalConnection = connect(runner, &AgentTaskRunner::proposalReady,
            &approvalScope, [&, runner](const QString &origin, const QString &proposalId,
                                        const QJsonObject &preview) {
                ++proposalCount;
                const bool exact = proposalCount == 1 && exactTrustedRodPreview(origin, preview,
                    expectedNormalizedProblem, expectedBudgets, sourceStamp.first, sourceStamp.second);
                if (exact && actions.isEmpty())
                {
                    ++acceptedProposalCount;
                    acceptedPreview = preview;
                    experimentId = preview.value(QStringLiteral("experimentId")).toString();
                    if (experimentId.isEmpty())
                        proposalError = QStringLiteral("Host preview omitted experimentId.");
                }
                else
                {
                    proposalError = QStringLiteral("Rejected unexpected or changed experiment preview.");
                }
                const bool allowed = exact && actions.isEmpty() && !experimentId.isEmpty();
                QTimer::singleShot(0, runner, [runner, proposalId, allowed] {
                    runner->authorizePending(proposalId, allowed);
                    if (!allowed) runner->cancel();
                });
            });

        const auto cancelIfStillActive = qScopeGuard([runner] {
            if (runner->isActive()) runner->cancel();
        });
        qInfo("Trusted rod live acceptance: starting Agent task.");
        widget->startTask(QStringLiteral(
            "Run one fully specified trusted mechanics experiment on a detached candidate. "
            "First call trusted_problem_inspect and trusted_problem_diagnose with scope=candidate, "
            "using the current Host documentId and documentRevision. Use this exact SI ProblemSpec: "
            "axialRod; length 1 m; 4 segments; Steel, source fixture, E=210 GPa, nu=0.3, "
            "density 7850 kg/m3; area 1e-4 m2; static; fixedEnd ux,uy,uz and allNodes uy,uz; "
            "freeEnd point force [1000,0,0] N; no initial conditions; observe freeEnd ux displacement; "
            "requested relative displacement precision 0.01. Never change or solve the source document. "
            "Then call experiment_run with this exact ProblemSpec and budgets solves=1, repairs=0, modelTurns=12. "
            "Wait for the accepted experiment to finish, query it, and report only Host verification facts. "
            "Finish with one JSON object and no markdown: {\"schema\":\"range.agent.report/v1\","
            "\"experimentId\":string,\"result\":\"pass\",\"solves\":1,\"repairs\":0,"
            "\"displacementRelativeError\":number,\"staticResidual\":number,"
            "\"reactionImbalance\":number}. Copy the three numbers from the passing Host VerificationReport."), true);
        qInfo("Trusted rod live acceptance: Agent task submitted.");

        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 240000);
        if (!questions.isEmpty())
            QFAIL("The Agent requested a physical clarification; this test stopped without inferring an answer.");
        QVERIFY2(finished.last().at(0).toString() == QStringLiteral("completed"),
                 qPrintable(finished.last().at(1).toString()));
        QCOMPARE(proposalCount, 1);
        QCOMPARE(acceptedProposalCount, 1);
        QVERIFY2(proposalError.isEmpty(), qPrintable(proposalError));
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.first().at(0).toString(), QStringLiteral("experiment"));
        QCOMPARE(actions.first().at(1).toJsonObject().value(QStringLiteral("experimentId")).toString(), experimentId);
        QVERIFY(!experimentId.isEmpty());

        const QJsonObject queryResponse = host->handle({
            {QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), QStringLiteral("experiment.query")},
            {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("experimentId"), experimentId}}}});
        QCOMPARE(queryResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject hostSummary = queryResponse.value(QStringLiteral("result")).toObject();
        QCOMPARE(hostSummary.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
        QVERIFY(hostSummary.value(QStringLiteral("verificationPassed")).toBool());
        QCOMPARE(hostSummary.value(QStringLiteral("counters")).toObject().value(QStringLiteral("solves")).toInt(), 1);
        QCOMPARE(hostSummary.value(QStringLiteral("counters")).toObject().value(QStringLiteral("repairs")).toInt(), 0);
        bool sawWait = false;
        bool sawQuery = false;
        bool sawReadyObservation = false;
        for (const auto &entry : activity)
        {
            if (entry.at(3).toString() != QStringLiteral("completed")) continue;
            sawWait = sawWait || entry.at(2).toString() == QStringLiteral("experiment_wait");
            sawQuery = sawQuery || entry.at(2).toString() == QStringLiteral("experiment_query");
        }
        for (const auto &entry : observed)
            if (entry.at(1).toString() == experimentId
                && entry.at(2).toJsonObject().value(QStringLiteral("state")).toString()
                    == QStringLiteral("candidate_ready")) sawReadyObservation = true;
        QVERIFY(sawWait);
        QVERIFY(sawQuery);
        QVERIFY(sawReadyObservation);

        const QJsonObject evidence = host->evaluationEvidenceSnapshot(experimentId);
        QCOMPARE(evidence.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.trusted-experiment-evidence-snapshot/v1"));
        QCOMPARE(evidence.value(QStringLiteral("experimentId")).toString(), experimentId);
        QCOMPARE(evidence.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), sourceStamp.first);
        QCOMPARE(evidence.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("revision")).toString(), QString::number(sourceStamp.second));
        const QJsonObject childJob = evidence.value(QStringLiteral("childJob")).toObject();
        const QJsonObject solveIdentity = evidence.value(QStringLiteral("latestSolveIdentity")).toObject();
        const QJsonObject verification = evidence.value(QStringLiteral("verificationReport")).toObject();
        QCOMPARE(childJob.value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
        QCOMPARE(childJob.value(QStringLiteral("experimentId")).toString(), experimentId);
        QCOMPARE(childJob.value(QStringLiteral("jobId")).toString(), solveIdentity.value(QStringLiteral("jobId")).toString());
        QCOMPARE(childJob.value(QStringLiteral("attemptId")).toString(), evidence.value(QStringLiteral("attemptId")).toString());
        QCOMPARE(verification.value(QStringLiteral("status")).toString(), QStringLiteral("pass"));
        QVERIFY(verification.value(QStringLiteral("passed")).toBool());
        const QJsonObject verificationIdentity = verification.value(QStringLiteral("identity")).toObject();
        for (const QString &key : {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                                   QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256"),
                                   QStringLiteral("sourceSnapshotRef")})
            QCOMPARE(verificationIdentity.value(key), solveIdentity.value(key));
        QCOMPARE(verificationIdentity.value(QStringLiteral("manifestSha256")).toString(),
                 childJob.value(QStringLiteral("resultManifestSha256")).toString());
        const QByteArray verificationBytes = QJsonDocument(verification).toJson(QJsonDocument::Compact);
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(verificationBytes, QCryptographicHash::Sha256).toHex()),
                 evidence.value(QStringLiteral("verificationReportSha256")).toString());
        QCOMPARE(session->modelCommandStamp(0), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)), sourceFingerprint);

        const QJsonObject metrics = verification.value(QStringLiteral("metrics")).toObject();
        const double expectedDisplacement = metrics.value(QStringLiteral("displacement")).toObject()
            .value(QStringLiteral("evidence")).toObject().value(QStringLiteral("maximumRelativeError")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
        const double expectedResidual = metrics.value(QStringLiteral("staticResidual")).toObject()
            .value(QStringLiteral("evidence")).toObject().value(QStringLiteral("normalizedResidual")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
        const double expectedReaction = metrics.value(QStringLiteral("reactionBalance")).toObject()
            .value(QStringLiteral("evidence")).toObject().value(QStringLiteral("maximumNormalizedImbalance")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
        QVERIFY(qIsFinite(expectedDisplacement));
        QVERIFY(qIsFinite(expectedResidual));
        QVERIFY(qIsFinite(expectedReaction));
        QVERIFY(!assistantMessages.isEmpty());
        const QString finalReport = assistantMessages.last().at(0).toString();
        const QByteArray reportBytes = finalReport.toUtf8();
        const int reportStart = finalReport.indexOf(QLatin1Char('{'));
        const int reportEnd = finalReport.lastIndexOf(QLatin1Char('}'));
        QJsonParseError parseError;
        const QJsonDocument parsedReport = reportStart >= 0 && reportEnd > reportStart
            ? QJsonDocument::fromJson(finalReport.mid(reportStart, reportEnd - reportStart + 1).toUtf8(), &parseError)
            : QJsonDocument{};
        const QJsonObject modelReport = parsedReport.object();
        const bool reportMatchesHost = parsedReport.isObject()
            && modelReport.value(QStringLiteral("schema")) == QJsonValue(QStringLiteral("range.agent.report/v1"))
            && modelReport.value(QStringLiteral("experimentId")).toString() == experimentId
            && modelReport.value(QStringLiteral("result")) == QJsonValue(QStringLiteral("pass"))
            && modelReport.value(QStringLiteral("solves")).toInt(-1) == 1
            && modelReport.value(QStringLiteral("repairs")).toInt(-1) == 0
            && modelReport.value(QStringLiteral("displacementRelativeError")).isDouble()
            && modelReport.value(QStringLiteral("staticResidual")).isDouble()
            && modelReport.value(QStringLiteral("reactionImbalance")).isDouble()
            && closeReportedMetric(modelReport.value(QStringLiteral("displacementRelativeError")).toDouble(), expectedDisplacement)
            && closeReportedMetric(modelReport.value(QStringLiteral("staticResidual")).toDouble(), expectedResidual)
            && closeReportedMetric(modelReport.value(QStringLiteral("reactionImbalance")).toDouble(), expectedReaction);
        const bool reportContainsCredential = !apiKey.isEmpty() && reportBytes.contains(apiKey.toUtf8());
        QString sanitizedReport = finalReport;
        sanitizedReport.replace(apiKey, QStringLiteral("[REDACTED]"));
        int totalTokens = 0;
        for (const auto &entry : usage) totalTokens += entry.at(0).toJsonObject().value(QStringLiteral("totalTokens")).toInt();
        QJsonObject exported{{QStringLiteral("schema"), QStringLiteral("range.stage8.live-agent-host-evidence/v1")},
            {QStringLiteral("testCase"), QStringLiteral("deepSeekRunsTrustedRodExperiment")},
            {QStringLiteral("provider"), QStringLiteral("DeepSeek")}, {QStringLiteral("model"), modelName},
            {QStringLiteral("sourceUnchanged"), true}, {QStringLiteral("sourceStamp"), QJsonObject{
                {QStringLiteral("documentId"), sourceStamp.first},
                {QStringLiteral("revision"), QString::number(sourceStamp.second)}}},
            {QStringLiteral("approvalBoundary"), QStringLiteral("test harness approved one exact preview in a disposable project")},
            {QStringLiteral("preview"), acceptedPreview}, {QStringLiteral("hostSummary"), hostSummary},
            {QStringLiteral("hostEvidence"), evidence}, {QStringLiteral("agentFinalReport"), sanitizedReport},
            {QStringLiteral("agentReportSha256"), QString::fromLatin1(QCryptographicHash::hash(
                sanitizedReport.toUtf8(), QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("agentReportMatchesHost"), reportMatchesHost},
            {QStringLiteral("agentReportContainedCredential"), reportContainsCredential},
            {QStringLiteral("providerRequests"), usage.size()}, {QStringLiteral("providerTotalTokens"), totalTokens}};
        const QByteArray exportBytes = QJsonDocument(exported).toJson(QJsonDocument::Indented);
        QVERIFY(!exportBytes.contains(apiKey.toUtf8()));
        QSaveFile evidenceFile(evidencePath);
        QVERIFY2(evidenceFile.open(QIODevice::WriteOnly), qPrintable(evidenceFile.errorString()));
        QCOMPARE(evidenceFile.write(exportBytes), qint64(exportBytes.size()));
        QVERIFY2(evidenceFile.commit(), qPrintable(evidenceFile.errorString()));
        qInfo().noquote() << "DeepSeek trusted rod evidence exported; experiment=" << experimentId
                          << "providerRequests=" << usage.size() << "tokens=" << totalTokens
                          << "reportMatchesHost=" << reportMatchesHost;
        QVERIFY(!reportContainsCredential);
        QVERIFY2(reportMatchesHost, qPrintable(QStringLiteral("Agent final report did not match the Host VerificationReport; parse status: %1")
            .arg(parseError.errorString())));
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
#endif

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
        const QString sourcePath = directory.filePath("source.rbm");
        ModelIO::save(source, sourcePath, true);
        ModelIO::open(source, sourcePath);
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
#if RANGE_ENABLE_AGENT
            window->findChild<QDockWidget *>("agentDock")->raise();
#endif
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
#if RANGE_ENABLE_AGENT
        for (auto *agent : window->findChildren<AgentWidget *>()) agent->taskRunner()->cancel();
#endif
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
        auto *reloadJob = new ModelIO(MODEL_IO_UPDATE, disk.getFileName(), session->getModelPtr(0));
        QSignalSpy reloadFailed(reloadJob, &RJob::failed);
        QSignalSpy reloadFinished(reloadJob, &RJob::finished);
        RJobManager::getInstance().submit(reloadJob);
        QTRY_COMPARE_WITH_TIMEOUT(reloadFinished.count() + reloadFailed.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        auditLock.unlock();
        QCOMPARE(reloadFailed.count(), 1);
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
#if RANGE_ENABLE_AGENT
int runAgentTaskRunnerTests(int argc, char **argv);
int runAgentReadOnlyRunnerTests(int argc, char **argv);
int runAgentConversationHistoryWidgetTests(int argc, char **argv);
int runAgentObservationBoundaryTests(int argc, char **argv);
#endif
int runTaskPlanWidgetTests(int argc, char **argv);
int runRuntimeControlWidgetTests(int argc, char **argv);
int runSeriesWorkbenchWidgetTests(int argc, char **argv);
int runGeometryCommandWidgetTests(int argc, char **argv);
int runMeshJobControllerTests(int argc, char **argv);
int runMeshBindingWidgetTests(int argc, char **argv);
#if RANGE_ENABLE_AGENT
int runScientificProviderTests(int argc, char **argv);
#endif
int runSolverSnapshotTests(int argc, char **argv);
int runExtensionWidgetTests(int argc, char **argv);
#if RANGE_ENABLE_AGENT
int runAgentEventViewTests(int argc, char **argv);
int runAgentConversationWidgetTests(int argc, char **argv);
#endif
int runProjectSessionTests(int argc, char **argv);
int runNativeJobRegistryTests(int argc, char **argv);
int runGuiModelOperationsTests(int argc, char **argv);
int runNativeResultsWidgetTests(int argc, char **argv);
int runCurrentModelWorkflowTests(int argc, char **argv);
int runRecoveryInteractionTests(int argc, char **argv);
int runModelCommitRecoveryTests(int argc, char **argv);
int runPiezoExperimentHistoryTests(int argc, char **argv);
int runPiezoWorkbenchWidgetTests(int argc, char **argv);
int runPiezoSolverRecoveryTests(int argc, char **argv);

namespace
{
int runTrustedFailureSolver(int argc, char **argv)
{
    const QByteArray logArgumentPrefix = QByteArrayLiteral("--log-file=");
    QString logPath;
    for (int i = 1; i < argc; ++i)
    {
        const QByteArray argument(argv[i]);
        if (argument.startsWith(logArgumentPrefix))
        {
            logPath = QString::fromLocal8Bit(argument.mid(logArgumentPrefix.size()));
            break;
        }
    }
    if (logPath.isEmpty()) return 90;
    const QByteArray failureLogMode = qgetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
    if (failureLogMode == QByteArrayLiteral("missing")) return 23;

    QFile logFile(logPath);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append)) return 91;
    if (failureLogMode == QByteArrayLiteral("empty")) return 23;
    if (failureLogMode == QByteArrayLiteral("unicode-whitespace-tail"))
    {
        const QByteArray prefix = QByteArrayLiteral("log prefix before the captured tail\n");
        const QByteArray nonBreakingSpace = QByteArray::fromHex("c2a0");
        QByteArray whitespaceTail;
        whitespaceTail.reserve(256 * 1024);
        for (int i = 0; i < 128 * 1024; ++i) whitespaceTail.append(nonBreakingSpace);
        if (logFile.write(prefix) != prefix.size()
            || logFile.write(whitespaceTail) != whitespaceTail.size()
            || !logFile.flush()) return 92;
        return 23;
    }
    const QByteArray sentinel = QByteArrayLiteral("TRUSTED_FAILURE_LOG_SENTINEL\n");
    if (logFile.write(sentinel) != sentinel.size() || !logFile.flush()) return 92;
    return 23;
}
}

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]).startsWith("--file="))
        return runTrustedFailureSolver(argc, argv);
    lifecycleBootTrace("main entered");
    const QString runtimeTemplate = qEnvironmentVariable("RANGE_TEST_RUNTIME_TEMPLATE");
    QTemporaryDir settingsDirectory(runtimeTemplate);
    if (!settingsDirectory.isValid()) return 2;
    bool trustedRepairDiagnostics = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]).startsWith("trustedExperimentAuthorizedMeshRefinementConverges"))
            trustedRepairDiagnostics = true;
    if (qEnvironmentVariableIsSet("RANGE_TEST_PRESERVE_RUNTIME") || trustedRepairDiagnostics)
        settingsDirectory.setAutoRemove(false);
    if (trustedRepairDiagnostics)
    {
        qputenv("RANGE_TEST_TRUSTED_RUNTIME_DIRECTORY", settingsDirectory.path().toUtf8());
        std::fprintf(stderr, "Persistent trusted-repair runtime: %s\n", qPrintable(settingsDirectory.path()));
        std::fflush(stderr);
    }
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
        // Bound native child parallelism in this isolated test settings directory.
        settings.setValue("application/nThreads", 4);
        lifecycleBootTrace("before settings sync");
        settings.sync();
        lifecycleBootTrace("after settings sync");
    }
    // A caller's profile directory would override the isolated test-mode data root.
    qunsetenv("RANGE_APPLICATION_PROFILE_DIR");
    WindowApplication application(argc, argv);
    if (qEnvironmentVariableIsSet("RANGE_LIFECYCLE_TRACE")) RLogger::getInstance().setLevel(R_LOG_LEVEL_TRACE);
    TestGuiModelLifecycle test;
    const bool hostContext = argc > 1 && QByteArray(argv[1]) == "--host-context";
#if RANGE_ENABLE_AGENT
    const bool agentRunner = argc > 1 && QByteArray(argv[1]) == "--agent-runner";
    const bool agentReadOnly = argc > 1 && QByteArray(argv[1]) == "--agent-readonly-runner";
    const bool agentHistory = argc > 1 && QByteArray(argv[1]) == "--agent-conversation-history";
    const bool observationBoundaries = argc > 1 && QByteArray(argv[1]) == "--agent-observation-boundaries";
#endif
    const bool taskPlans = argc > 1 && QByteArray(argv[1]) == "--task-plans-widget";
    const bool runtimeControl = argc > 1 && QByteArray(argv[1]) == "--runtime-control-widget";
    const bool seriesWorkbench = argc > 1 && QByteArray(argv[1]) == "--series-workbench-widget";
    const bool geometryCommands = argc > 1 && QByteArray(argv[1]) == "--geometry-command-widget";
    const bool meshJobs = argc > 1 && QByteArray(argv[1]) == "--mesh-job-controller";
    const bool meshBinding = argc > 1 && QByteArray(argv[1]) == "--mesh-binding-widget";
#if RANGE_ENABLE_AGENT
    const bool scientificProvider = argc > 1 && QByteArray(argv[1]) == "--scientific-provider";
#endif
    const bool solverSnapshot = argc > 1 && QByteArray(argv[1]) == "--solver-snapshot";
    const bool extensionWidget = argc > 1 && QByteArray(argv[1]) == "--extension-widget";
#if RANGE_ENABLE_AGENT
    const bool agentEventView = argc > 1 && QByteArray(argv[1]) == "--agent-event-view";
    const bool agentConversation = argc > 1 && QByteArray(argv[1]) == "--agent-conversation";
#endif
    const bool projectSession = argc > 1 && QByteArray(argv[1]) == "--project-session";
    const bool nativeJobs = argc > 1 && QByteArray(argv[1]) == "--native-jobs";
    const bool modelOperations = argc > 1 && QByteArray(argv[1]) == "--model-operations";
    const bool nativeResults = argc > 1 && QByteArray(argv[1]) == "--native-results-widget";
    const bool currentModelWorkflow = argc > 1 && QByteArray(argv[1]) == "--current-model-workflow";
    const bool recoveryInteraction = argc > 1 && QByteArray(argv[1]) == "--recovery-interaction";
    const bool modelCommitRecovery = argc > 1 && QByteArray(argv[1]) == "--model-commit-recovery";
    const bool piezoHistory = argc > 1 && QByteArray(argv[1]) == "--piezo-experiment-history";
    const bool piezoWorkbench = argc > 1 && QByteArray(argv[1]) == "--piezo-workbench-widget";
    const bool piezoSolverRecovery = argc > 1 && QByteArray(argv[1]) == "--piezo-solver-recovery";
    bool selectedSuite = hostContext || taskPlans || runtimeControl || seriesWorkbench || geometryCommands
        || meshJobs || meshBinding || solverSnapshot || extensionWidget || projectSession
        || nativeJobs || modelOperations || nativeResults || currentModelWorkflow || recoveryInteraction
        || modelCommitRecovery || piezoHistory || piezoWorkbench || piezoSolverRecovery;
#if RANGE_ENABLE_AGENT
    selectedSuite = selectedSuite || agentRunner || agentReadOnly || agentHistory || observationBoundaries
        || agentEventView || agentConversation || scientificProvider;
#endif
    if (selectedSuite)
    {
        for (int i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
        --argc; argv[argc] = nullptr;
    }
    const int result = hostContext ? runGuiHostContextTests(argc, argv)
#if RANGE_ENABLE_AGENT
        : agentRunner ? runAgentTaskRunnerTests(argc, argv)
        : agentReadOnly ? runAgentReadOnlyRunnerTests(argc, argv)
        : agentHistory ? runAgentConversationHistoryWidgetTests(argc, argv)
        : observationBoundaries ? runAgentObservationBoundaryTests(argc, argv)
#endif
        : taskPlans ? runTaskPlanWidgetTests(argc, argv)
        : runtimeControl ? runRuntimeControlWidgetTests(argc, argv)
        : seriesWorkbench ? runSeriesWorkbenchWidgetTests(argc, argv)
        : geometryCommands ? runGeometryCommandWidgetTests(argc, argv)
        : meshJobs ? runMeshJobControllerTests(argc, argv)
        : meshBinding ? runMeshBindingWidgetTests(argc, argv)
#if RANGE_ENABLE_AGENT
        : scientificProvider ? runScientificProviderTests(argc, argv)
#endif
        : solverSnapshot ? runSolverSnapshotTests(argc, argv)
        : extensionWidget ? runExtensionWidgetTests(argc, argv)
#if RANGE_ENABLE_AGENT
        : agentEventView ? runAgentEventViewTests(argc, argv)
        : agentConversation ? runAgentConversationWidgetTests(argc, argv)
#endif
        : projectSession ? runProjectSessionTests(argc, argv)
        : nativeJobs ? runNativeJobRegistryTests(argc, argv)
        : modelOperations ? runGuiModelOperationsTests(argc, argv)
        : nativeResults ? runNativeResultsWidgetTests(argc, argv)
        : currentModelWorkflow ? runCurrentModelWorkflowTests(argc, argv)
        : recoveryInteraction ? runRecoveryInteractionTests(argc, argv)
        : modelCommitRecovery ? runModelCommitRecoveryTests(argc, argv)
        : piezoHistory ? runPiezoExperimentHistoryTests(argc, argv)
        : piezoWorkbench ? runPiezoWorkbenchWidgetTests(argc, argv)
        : piezoSolverRecovery ? runPiezoSolverRecoveryTests(argc, argv) : QTest::qExec(&test, argc, argv);
    application.getSession()->clear();
    delete application.getMainWindow();
    SolverManager::getInstance().stopServer();
    return result;
}

#include "tst_gui_model_lifecycle.moc"
