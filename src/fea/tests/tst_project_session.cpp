#include <QtTest>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QLockFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QDockWidget>
#include <QUuid>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <thread>

#include <rbl_job_manager.h>
#include <rmodel_command_session.h>

#include "application.h"
#include "action.h"
#include "bc_manager_tree.h"
#include "ec_manager_tree.h"
#include "ec_tree.h"
#include "geometry_command_widget.h"
#include "ic_manager_tree.h"
#include "model_action.h"
#include "model_action_input.h"
#include "model_command_host.h"
#include "model_io.h"
#include "modal_setup_widget.h"
#include "problem_tree.h"
#include "session.h"
#include "value_line_edit.h"
#include "workflow_widget.h"

namespace
{
Model modalFixture(const QString &name = QStringLiteral("Project session fixture"))
{
    Model model;
    model.setName(name);
    model.setNNodes(2);
    model.setNode(0, RNode(0.0, 0.0, 0.0));
    model.setNode(1, RNode(1.0, 0.0, 0.0));
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
    modal.setConvergenceValue(0.001);
    return model;
}

bool createSourceModel(const QString &path)
{
    Model model = modalFixture();
    try
    {
        ModelIO::save(model, path, true);
        return QFileInfo(path).isFile();
    }
    catch (...)
    {
        return false;
    }
}

QString nativeModelPath(const QString &directory, const QString &baseName)
{
    return QDir(directory).filePath(baseName + QLatin1Char('.') + RModel::getDefaultFileExtension(true));
}

bool openProject(Session *session, const QString &workspace)
{
    QString error;
    const bool opened = session->openProject(workspace, &error);
    if (!opened) qWarning("Could not open test project: %s", qPrintable(error));
    return opened;
}

bool waitForJob(RJob *job)
{
    if (!job) return false;
    job->setAutoDelete(true);
    QSignalSpy finished(job, &RJob::finished);
    QSignalSpy failed(job, &RJob::failed);
    RJobManager::getInstance().submit(job);
    QElapsedTimer timer;
    timer.start();
    while (((finished.isEmpty() && failed.isEmpty()) || ModelCommandHost::isBusy())
           && timer.elapsed() < 30000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QTest::qWait(10);
    }
    return !finished.isEmpty() && failed.isEmpty() && !ModelCommandHost::isBusy();
}

bool saveModelJob(Session *session, uint modelID, const QString &path)
{
    auto *job = new ModelIO(MODEL_IO_SAVE, path, session->getModelPtr(modelID));
    return waitForJob(job);
}

QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void captureWindowEvidence(const QString &name)
{
    const QString directory = qEnvironmentVariable("RANGE_PROJECT_GUI_EVIDENCE_DIRECTORY");
    if (directory.isEmpty()) return;
    auto *window = Application::instance()->getMainWindow();
    auto *problemTree = window->findChild<ProblemTree *>();
    for (auto *tabs : window->findChildren<QTabWidget *>())
    {
        const int index = tabs->indexOf(problemTree);
        if (index >= 0) tabs->setCurrentIndex(index);
    }
    if (auto *dock = window->findChild<QDockWidget *>(QStringLiteral("solverDock")))
    { dock->show(); dock->raise(); }
    QCoreApplication::processEvents();
    QVERIFY(window->isVisible());
    QVERIFY(problemTree->isVisible());
    QVERIFY(window->grab().save(QDir(directory).filePath(name + QStringLiteral(".png"))));
}

void verifyCatalogAcrossClients(Session *session, const QJsonObject &snapshot)
{
#ifndef RANGE_WORKFLOW_DEFAULT_PYTHON
    Q_UNUSED(session);
    Q_UNUSED(snapshot);
    QFAIL("Catalog SDK parity requires the configured Python interpreter.");
#else
    const QString persistentId = snapshot.value(QStringLiteral("persistentDocumentId")).toString();
    QProcess catalogCli;
    catalogCli.start(QStringLiteral(RANGE_PROJECT_TEST_CLI),
                     {QStringLiteral("--workspace"), session->projectWorkspace(), QStringLiteral("--query")});
    QVERIFY(catalogCli.waitForFinished(20000));
    QCOMPARE(catalogCli.exitStatus(), QProcess::NormalExit);
    QVERIFY2(catalogCli.exitCode() == 0, catalogCli.readAllStandardError().constData());
    const auto nativeCatalog = QJsonDocument::fromJson(catalogCli.readAllStandardOutput()).object();
    QJsonObject nativeDocument;
    for (const auto &value : nativeCatalog.value(QStringLiteral("documents")).toArray())
        if (value.toObject().value(QStringLiteral("persistentDocumentId")) == QJsonValue(persistentId))
            nativeDocument = value.toObject();
    QVERIFY(!nativeDocument.isEmpty());

    QProcess client;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONPATH"), QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT "/range-workflow/python"));
    environment.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    client.setProcessEnvironment(environment);
    const QString script = QStringLiteral(
        "import json,sys\nsys.path.insert(0,sys.argv.pop(1))\nfrom range_workflow import WorkflowClient\n"
        "with WorkflowClient(sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4],python=sys.executable) as c:\n"
        " print(json.dumps({'catalog':c.project_catalog(sys.argv[5]),'snapshot':c.project_snapshot(sys.argv[5])}))\n");
    client.start(QStringLiteral(RANGE_WORKFLOW_DEFAULT_PYTHON),
                 {QStringLiteral("-I"), QStringLiteral("-B"), QStringLiteral("-c"), script,
                  QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT "/range-workflow/python"),
                  session->projectWorkspace(),
                  QStringLiteral(RANGE_PROJECT_TEST_SOLVER),
                  QStringLiteral(RANGE_PROJECT_TEST_NATIVE),
                  QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT "/fea-solver/examples/stage9"), persistentId});
    QVERIFY(client.waitForFinished(20000));
    QCOMPARE(client.exitStatus(), QProcess::NormalExit);
    QVERIFY2(client.exitCode() == 0, client.readAllStandardError().constData());
    const auto response = QJsonDocument::fromJson(client.readAllStandardOutput()).object();
    const auto sdkCatalog = response.value(QStringLiteral("catalog")).toObject();
    QCOMPARE(sdkCatalog.value(QStringLiteral("projectId")), nativeCatalog.value(QStringLiteral("projectId")));
    QCOMPARE(sdkCatalog.value(QStringLiteral("documents")).toArray().size(), 1);
    QCOMPARE(sdkCatalog.value(QStringLiteral("documents")).toArray().first().toObject(), nativeDocument);
    QCOMPARE(sdkCatalog.value(QStringLiteral("unitConvention")),
             nativeCatalog.value(QStringLiteral("capability")).toObject().value(QStringLiteral("unitConvention")));
    const auto sdkSnapshot = response.value(QStringLiteral("snapshot")).toObject();
    for (const auto &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
                              QStringLiteral("sourceRevision"), QStringLiteral("modelSha256"), QStringLiteral("instanceRef")})
        QCOMPARE(sdkSnapshot.value(field), snapshot.value(field));
    QVERIFY(sdkSnapshot.value(QStringLiteral("snapshotBytesVerified")).toBool());
#endif
}

QJsonObject queryHostDocument(Session *session, uint id)
{
    auto *host = session->getModelCommandHost();
    const QJsonObject request{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
        {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("operation"), QStringLiteral("context.query")},
        {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("target"), QJsonObject{
            {QStringLiteral("documentId"), session->modelCommandStamp(id).first},
            {QStringLiteral("sessionEpoch"), host->sessionEpoch()}}}}}};
    const auto documents = host->handle(request).value(QStringLiteral("result")).toObject()
                               .value(QStringLiteral("documents")).toArray();
    return documents.isEmpty() ? QJsonObject{} : documents.first().toObject();
}

template<typename Widget> Widget *activeProblemChild(ProblemTree *tree, const QString &name)
{
    // QTreeWidget retires old item widgets with deleteLater. Query the widget
    // currently attached to a row instead of finding a retired QObject child.
    for (int row = 0; row < tree->topLevelItemCount(); ++row)
        if (auto *editor = tree->itemWidget(tree->topLevelItem(row), 0))
            if (auto *child = editor->findChild<Widget *>(name)) return child;
    return nullptr;
}
}

class TestProjectSession : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        auto *session = Application::instance()->getSession();
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        session->clear();
    }

    void closeReplayObservesOriginalOperationBeforeCompletingQueue()
    {
        for (bool observedBeforeExit : {false, true})
        {
            QTemporaryDir workspace;
            QVERIFY(workspace.isValid());
            QString error;
            auto catalog = RProjectCatalog::open(workspace.path(), &error);
            QVERIFY2(catalog, qPrintable(error));
            const QString retryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto operation = catalog->beginOperation(retryId, {{"command", "model.session.close"}}, {}, {}, &error);
            QVERIFY2(!operation.operation.operationId.isEmpty(), qPrintable(error));
            const QString operationId = operation.operation.operationId;
            QVERIFY2(catalog->advanceOperation(operationId, RProjectCatalog::OperationPhase::Accepted, {}, {}, &error), qPrintable(error));
            RModelCommandSession::JournalOptions options;
            options.path = QDir(workspace.path()).filePath("closed-session.json");
            RModelCommandSession editing(modalFixture(), options);
            const auto payload = editing.prepareCloseRetry(retryId, operationId, &error);
            QVERIFY2(!payload.isEmpty(), qPrintable(error));
            QVERIFY2(catalog->queueCloseRetry(retryId, operationId, "close_record_pending", payload, {}, &error), qPrintable(error));
            QVERIFY2(editing.closeWithRetryPayload(payload, &error), qPrintable(error));
            if (observedBeforeExit)
            {
                RProjectCatalog::OperationAssociation observation;
                observation.observation = {{"status", "ok"}, {"code", "OK"}, {"result", QJsonObject{
                    {"operationId", operationId}, {"command", "model.session.close"},
                    {"editingSessionId", payload.value("record").toObject().value("sessionId")},
                    {"recordId", payload.value("record").toObject().value("recordId")}, {"persistence", "durable"}}}};
                QVERIFY2(catalog->advanceOperation(operationId, RProjectCatalog::OperationPhase::Observed, observation, {}, &error), qPrintable(error));
            }
            catalog.reset(); // Both process-exit windows leave the durable queue.
            auto *session = Application::instance()->getSession();
            QVERIFY2(openProject(session, workspace.path()), qPrintable(error));
            const auto restored = session->getProjectCatalog()->queryOperation(operationId, {}, &error);
            QVERIFY2(restored.has_value(), qPrintable(error));
            QCOMPARE(int(restored->phase), int(RProjectCatalog::OperationPhase::Observed));
            QCOMPARE(restored->association.observation.value("result").toObject().value("recordId"),
                     payload.value("record").toObject().value("recordId"));
            QVERIFY(session->getProjectCatalog()->pendingCloseRetries({}, &error).isEmpty());
            QVERIFY(openProject(session, QDir(workspace.path()).filePath("next-project")));
        }
    }

    void persistentIdentitySurvivesCloseAndReopen()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("reopen"));
        QVERIFY(createSourceModel(sourcePath));
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);
        const QString runtimeIdBefore = session->modelCommandStamp(0).first;
        const QString persistentId = session->documentProjectContext(0)
                                         .value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!runtimeIdBefore.isEmpty());
        QVERIFY(!persistentId.isEmpty());
        QVERIFY(runtimeIdBefore != persistentId);

        session->clear();
        QCOMPARE(session->getNModels(), 0u);
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));

        Model reopened;
        ModelIO::open(reopened, sourcePath);
        session->addModel(reopened);
        const QString runtimeIdAfter = session->modelCommandStamp(0).first;
        const QJsonObject context = session->documentProjectContext(0);
        QCOMPARE(context.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QVERIFY(runtimeIdAfter != runtimeIdBefore);
        QVERIFY(runtimeIdAfter != persistentId);

        QTemporaryDir otherWorkspace;
        QVERIFY(otherWorkspace.isValid());
        QVERIFY(!session->openProject(otherWorkspace.path(), &error));
        QCOMPARE(session->projectWorkspace(), QFileInfo(workspace.path()).canonicalFilePath());
    }

    void managerConditionControlsOnlyInvalidateAcceptedChanges()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        Model source;
        source.setName(QStringLiteral("Manager checkbox regression"));
        source.setNNodes(2);
        source.setNode(0, RNode(0.0, 0.0, 0.0));
        source.setNode(1, RNode(1.0, 0.0, 0.0));
        source.setNElements(1);
        RElement truss(R_ELEMENT_TRUSS1);
        truss.setNodeId(0, 0);
        truss.setNodeId(1, 1);
        source.setElement(0, truss);
        source.setNLines(1);
        source.getLine(0).add(0);
        source.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));

        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("manager-checkbox"));
        bool saved = false;
        try
        {
            ModelIO::save(source, sourcePath, true);
            saved = QFileInfo(sourcePath).isFile();
        }
        catch (...)
        {
        }
        QVERIFY(saved);

        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);
        QCOMPARE(session->getNModels(), 1u);

        auto *window = Application::instance()->getMainWindow();
        QVERIFY(window);
        auto *bcTree = window->findChild<BCManagerTree *>();
        auto *ecTree = window->findChild<ECManagerTree *>();
        auto *icTree = window->findChild<ICManagerTree *>();
        QVERIFY(bcTree && ecTree && icTree);

        auto expectedStamp = session->modelCommandStamp(0);
        QVERIFY(!expectedStamp.first.isEmpty());
        int expectedSourceRevision = 0;
        const auto checkCatalogState = [&](bool dirty) {
            const auto context = session->documentProjectContext(0);
            QCOMPARE(context.value(QStringLiteral("sourceRevision")).toString(),
                     QStringLiteral("r%1").arg(expectedSourceRevision));
            QCOMPARE(context.value(QStringLiteral("unsaved")).toObject()
                         .value(QStringLiteral("dirty")).toBool(), dirty);
        };
        checkCatalogState(false);

        // Populate the real MainWindow manager trees with only the saved truss line selected.
        session->setModelSelected(0, false);
        session->setEntitySelected(0, R_ENTITY_GROUP_LINE, 0, true);
        QCoreApplication::processEvents();
        QCOMPARE(session->modelCommandStamp(0), expectedStamp);
        checkCatalogState(false);
        QVERIFY(bcTree->topLevelItemCount() > 0);
        QVERIFY(ecTree->topLevelItemCount() > 0);
        QVERIFY(icTree->topLevelItemCount() > 0);

        Model &accepted = session->getModel(0);
        RLine &line = accepted.getLine(0);
        const auto findRow = [](QTreeWidget *tree, int type) -> QTreeWidgetItem * {
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
            {
                auto *item = tree->topLevelItem(row);
                if (item->data(0, Qt::UserRole).isValid()
                    && item->data(0, Qt::UserRole).toInt() == type)
                    return item;
            }
            return nullptr;
        };
        const auto runCheckboxRoundTrip = [&](QTreeWidget *tree, auto supportsLine,
                                              auto containsType, const char *treeName) {
            QTreeWidgetItem *item = nullptr;
            int type = -1;
            for (int row = 0; row < tree->topLevelItemCount(); ++row)
            {
                auto *candidate = tree->topLevelItem(row);
                if (!candidate->flags().testFlag(Qt::ItemIsUserCheckable)
                    || !candidate->data(0, Qt::UserRole).isValid())
                    continue;
                const int candidateType = candidate->data(0, Qt::UserRole).toInt();
                if (supportsLine(candidateType))
                {
                    item = candidate;
                    type = candidateType;
                    break;
                }
            }
            QVERIFY2(item, treeName);
            QVERIFY2(supportsLine(type), treeName);
            QVERIFY2(item->flags().testFlag(Qt::ItemIsUserCheckable), treeName);
            QCOMPARE(item->checkState(0), Qt::Unchecked);
            QVERIFY(!containsType(type));

            const auto beforeSelectionStamp = session->modelCommandStamp(0);
            const auto beforeSelectionContext = session->documentProjectContext(0);
            item->setSelected(true);
            QCoreApplication::processEvents();
            QCOMPARE(session->modelCommandStamp(0), beforeSelectionStamp);
            const auto afterSelectionContext = session->documentProjectContext(0);
            QCOMPARE(afterSelectionContext.value(QStringLiteral("sourceRevision")),
                     beforeSelectionContext.value(QStringLiteral("sourceRevision")));
            QCOMPARE(afterSelectionContext.value(QStringLiteral("unsaved")).toObject()
                         .value(QStringLiteral("dirty")),
                     beforeSelectionContext.value(QStringLiteral("unsaved")).toObject()
                         .value(QStringLiteral("dirty")));

            item = findRow(tree, type);
            QVERIFY(item);
            item->setCheckState(0, Qt::Checked);
            ++expectedStamp.second;
            ++expectedSourceRevision;
            QCOMPARE(session->modelCommandStamp(0), expectedStamp);
            checkCatalogState(true);
            QVERIFY2(containsType(type), treeName);
            item = findRow(tree, type);
            QVERIFY(item);
            QCOMPARE(item->checkState(0), Qt::Checked);

            const auto beforeDuplicateStamp = session->modelCommandStamp(0);
            const QString beforeDuplicateRevision = session->documentProjectContext(0)
                .value(QStringLiteral("sourceRevision")).toString();
            QVERIFY(QMetaObject::invokeMethod(tree, "onItemChanged", Qt::DirectConnection,
                Q_ARG(QTreeWidgetItem*, item), Q_ARG(int, 0)));
            QCOMPARE(session->modelCommandStamp(0), beforeDuplicateStamp);
            QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("sourceRevision")).toString(),
                     beforeDuplicateRevision);
            QVERIFY2(containsType(type), treeName);
            checkCatalogState(true);

            item = findRow(tree, type);
            QVERIFY(item);
            item->setCheckState(0, Qt::Unchecked);
            ++expectedStamp.second;
            ++expectedSourceRevision;
            QCOMPARE(session->modelCommandStamp(0), expectedStamp);
            checkCatalogState(true);
            QVERIFY2(!containsType(type), treeName);
            item = findRow(tree, type);
            QVERIFY(item);
            QCOMPARE(item->checkState(0), Qt::Unchecked);
        };

        const auto supportsBoundary = [](int rawType) {
            if (rawType <= int(R_BOUNDARY_CONDITION_NONE)
                || rawType >= int(R_BOUNDARY_CONDITION_N_TYPES)) return false;
            const auto type = RBoundaryConditionType(rawType);
            return RBoundaryCondition::getApplyOnLine(type)
                && (RBoundaryCondition::getProblemTypeMask(type) & R_PROBLEM_STRESS);
        };
        const auto hasBoundaryType = [&line](int rawType) {
            const auto type = RBoundaryConditionType(rawType);
            if (!line.hasBoundaryCondition(type)) return false;
            for (uint index = 0; index < line.getNBoundaryConditions(); ++index)
                if (line.getBoundaryCondition(index).getType() == type) return true;
            return false;
        };
        const auto supportsInitial = [](int rawType) {
            if (rawType <= int(R_INITIAL_CONDITION_NONE)
                || rawType >= int(R_INITIAL_CONDITION_N_TYPES)) return false;
            const auto type = RInitialConditionType(rawType);
            return (RInitialCondition::getProblemTypeMask(type) & R_PROBLEM_STRESS) != 0;
        };
        const auto hasInitialType = [&line](int rawType) {
            const auto type = RInitialConditionType(rawType);
            if (!line.hasInitialCondition(type)) return false;
            for (uint index = 0; index < line.getNInitialConditions(); ++index)
                if (line.getInitialCondition(index).getType() == type) return true;
            return false;
        };
        const auto supportsEnvironment = [](int rawType) {
            if (rawType <= int(R_ENVIRONMENT_CONDITION_NONE)
                || rawType >= int(R_ENVIRONMENT_CONDITION_N_TYPES)) return false;
            const auto type = REnvironmentConditionType(rawType);
            return (REnvironmentCondition::getProblemTypeMask(type) & R_PROBLEM_STRESS) != 0;
        };
        const auto hasEnvironmentType = [&line](int rawType) {
            const auto type = REnvironmentConditionType(rawType);
            if (!line.hasEnvironmentCondition(type)) return false;
            for (uint index = 0; index < line.getNEnvironmentConditions(); ++index)
                if (line.getEnvironmentCondition(index).getType() == type) return true;
            return false;
        };

        runCheckboxRoundTrip(bcTree, supportsBoundary, hasBoundaryType, "Boundary condition row");
        runCheckboxRoundTrip(icTree, supportsInitial, hasInitialType, "Initial condition row");

        // Environment conditions are seeded and displayed without removal
        // checkboxes. Exercise their actual value editor, preserving defaults.
        QTreeWidgetItem *environmentRow = nullptr;
        int environmentType = -1;
        for (int row = 0; row < ecTree->topLevelItemCount(); ++row)
        {
            auto *candidate = ecTree->topLevelItem(row);
            const int type = candidate->data(0, Qt::UserRole).toInt();
            if (supportsEnvironment(type) && hasEnvironmentType(type))
            { environmentRow = candidate; environmentType = type; break; }
        }
        QVERIFY(environmentRow);
        const auto beforeEnvironmentSelection = session->modelCommandStamp(0);
        environmentRow->setSelected(true);
        QCOMPARE(session->modelCommandStamp(0), beforeEnvironmentSelection);
        auto *environmentEditor = window->findChild<ECTree *>();
        QVERIFY(environmentEditor && environmentEditor->topLevelItemCount() > 0);
        const auto *property = environmentEditor->topLevelItem(0);
        const auto variableType = RVariableType(property->data(3, Qt::DisplayRole).toInt());
        auto *valueWidget = environmentEditor->itemWidget(environmentEditor->topLevelItem(0), 1);
        QVERIFY(valueWidget);
        auto *valueEdit = valueWidget->findChild<ValueLineEdit *>();
        QVERIFY(valueEdit);
        valueEdit->setText(QStringLiteral("2.5"));
        // The existing editor emits a specific environment notification and
        // a problem refresh; both invalidate the runtime command stamp.
        expectedStamp.second += 2;
        ++expectedSourceRevision;
        QCOMPARE(session->modelCommandStamp(0), expectedStamp);
        checkCatalogState(true);
        const auto &condition = line.getEnvironmentCondition(REnvironmentConditionType(environmentType));
        bool foundValue = false;
        for (uint index = 0; index < condition.size(); ++index)
            if (condition.getComponent(index).getType() == variableType)
                foundValue = condition.getComponent(index).getValue(0) == 2.5;
        QVERIFY(foundValue);
    }

    void duplicateOpenCreatesCopyAndSaveAsPreservesItsIdentity()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && sources.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("origin"));
        QVERIFY(createSourceModel(sourcePath));
        Model first;
        ModelIO::open(first, sourcePath);
        session->addModel(first);
        Model duplicate;
        ModelIO::open(duplicate, sourcePath);
        session->addModel(duplicate);

        const QJsonObject origin = session->documentProjectContext(0);
        const QJsonObject copy = session->documentProjectContext(1);
        const QString originId = origin.value(QStringLiteral("persistentDocumentId")).toString();
        const QString copyId = copy.value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!originId.isEmpty() && !copyId.isEmpty());
        QVERIFY(originId != copyId);
        QCOMPARE(copy.value(QStringLiteral("originDocumentId")).toString(), originId);
        QVERIFY(copy.value(QStringLiteral("sourcePath")).toString().isEmpty());
        QVERIFY(session->getModel(1).getFileName().isEmpty());

        const QString copyPath = nativeModelPath(outputs.path(), QStringLiteral("copy"));
        QString error;
        QVERIFY2(session->canSaveModel(1, copyPath, &error), qPrintable(error));
        Model copyToSave = session->getModel(1);
        ModelIO::save(copyToSave, copyPath, false);
        QVERIFY(copyToSave.getLoadedSourceBinding().has_value());
        session->getModel(1).setFileName(copyPath);
        QVERIFY2(session->recordModelSaved(1, copyPath, *copyToSave.getLoadedSourceBinding(), &error),
                 qPrintable(error));

        const QJsonObject savedCopy = session->documentProjectContext(1);
        QCOMPARE(savedCopy.value(QStringLiteral("persistentDocumentId")).toString(), copyId);
        QCOMPARE(savedCopy.value(QStringLiteral("originDocumentId")).toString(), originId);
        QCOMPARE(savedCopy.value(QStringLiteral("sourcePath")).toString(), QFileInfo(copyPath).canonicalFilePath());
        QVERIFY(!savedCopy.value(QStringLiteral("savedArtifactHash")).toString().isEmpty());
        QCOMPARE(savedCopy.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool(), false);

        session->removeModel(0);
        QCOMPARE(session->getNModels(), 1u);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(), copyId);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("originDocumentId")).toString(), originId);
    }

    void snapshotIsFrozenAndBoundToRuntimeEpoch()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture());
        const QString runtimeId = session->modelCommandStamp(0).first;
        const QPair<QString, quint64> baselineStamp = session->modelCommandStamp(0);
        const QJsonObject baselineContext = session->documentProjectContext(0);
        const QString baselineSourceRevision = baselineContext.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(baselineSourceRevision, QStringLiteral("r0"));
        QCOMPARE(baselineContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);

        QString error;
        const QJsonObject firstArtifact = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!firstArtifact.isEmpty(), qPrintable(error));
        QCOMPARE(firstArtifact.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
        const QJsonObject instanceRef = firstArtifact.value(QStringLiteral("instanceRef")).toObject();
        QCOMPARE(instanceRef.value(QStringLiteral("documentId")).toString(), runtimeId);
        QCOMPARE(instanceRef.value(QStringLiteral("sessionEpoch")).toString(),
                 session->getModelCommandHost()->sessionEpoch());
        QCOMPARE(instanceRef.value(QStringLiteral("documentRevision")).toDouble(),
                 double(baselineStamp.second));

        const QString frozenPath = QDir(session->projectWorkspace()).filePath(
            firstArtifact.value(QStringLiteral("relativePath")).toString());
        Model frozen;
        ModelIO::open(frozen, frozenPath);
        QCOMPARE(frozen.getProblemSetup().getModalSetup().getNIterations(), 12u);

        session->getModel(0).getProblemSetup().getModalSetup().setNIterations(29);
        session->setProblemChanged(0);
        const QJsonObject updatedContext = session->documentProjectContext(0);
        QCOMPARE(updatedContext.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QCOMPARE(session->modelCommandStamp(0).second, baselineStamp.second + 1);

        Model rereadFrozen;
        ModelIO::open(rereadFrozen, frozenPath);
        QCOMPARE(rereadFrozen.getProblemSetup().getModalSetup().getNIterations(), 12u);
        const QJsonObject secondArtifact = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!secondArtifact.isEmpty(), qPrintable(error));
        QCOMPARE(secondArtifact.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QVERIFY(secondArtifact.value(QStringLiteral("modelSha256")).toString()
                != firstArtifact.value(QStringLiteral("modelSha256")).toString());
    }

    void reopeningSavedSourceReconcilesDirtySnapshotAsOrphan()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("discarded-edit"));
        QVERIFY(createSourceModel(sourcePath));
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);
        const QString persistentId = session->documentProjectContext(0)
                                         .value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!persistentId.isEmpty());
        const QJsonObject baselineContext = session->documentProjectContext(0);
        QCOMPARE(baselineContext.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
        QCOMPARE(baselineContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);
        session->setModelSelectionChanged(0);
        const QJsonObject refreshedContext = session->documentProjectContext(0);
        QCOMPARE(refreshedContext.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
        QCOMPARE(refreshedContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);

        session->getModel(0).getProblemSetup().getModalSetup().setNIterations(41);
        session->setProblemChanged(0);
        QString error;
        const QString editedSourceRevision = session->documentProjectContext(0)
                                                 .value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(editedSourceRevision, QStringLiteral("r1"));
        const QJsonObject discarded = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!discarded.isEmpty(), qPrintable(error));
        QCOMPARE(discarded.value(QStringLiteral("sourceRevision")).toString(), editedSourceRevision);
        session->clear();

        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        Model reopened;
        ModelIO::open(reopened, sourcePath);
        session->addModel(reopened);
        const QJsonObject recovered = session->documentProjectContext(0);
        QCOMPARE(recovered.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(recovered.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r2"));
        QVERIFY(recovered.value(QStringLiteral("reopenedSavedSource")).toBool());
        QCOMPARE(recovered.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool(), false);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);

        const QJsonArray orphans = recovered.value(QStringLiteral("orphanSnapshots")).toArray();
        QVERIFY(!orphans.isEmpty());
        const QJsonObject orphan = orphans.last().toObject();
        QCOMPARE(orphan.value(QStringLiteral("sourceRevision")).toString(), editedSourceRevision);
        QCOMPARE(orphan.value(QStringLiteral("modelSha256")).toString(),
                 discarded.value(QStringLiteral("modelSha256")).toString());
        QVERIFY(QFileInfo::exists(QDir(session->projectWorkspace())
                                      .filePath(orphan.value(QStringLiteral("relativePath")).toString())));

        const QJsonObject current = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!current.isEmpty(), qPrintable(error));
        QCOMPARE(current.value(QStringLiteral("sourceRevision")).toString(),
                 QStringLiteral("r2"));
        QVERIFY(current.value(QStringLiteral("modelSha256")).toString()
                != orphan.value(QStringLiteral("modelSha256")).toString());
    }

    void activeDocumentLeaseForksSecondSessionAndKeepsOwnerWritable()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *sessionA = Application::instance()->getSession();
        QVERIFY(openProject(sessionA, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("shared-session"));
        QVERIFY(createSourceModel(sourcePath));

        Model sourceA;
        ModelIO::open(sourceA, sourcePath);
        sessionA->addModel(sourceA);
        const QString persistentId = sessionA->documentProjectContext(0)
                                         .value(QStringLiteral("persistentDocumentId")).toString();
        const QJsonObject ownerBaseline = sessionA->documentProjectContext(0);
        const QString baselineSourceRevision = ownerBaseline.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(baselineSourceRevision, QStringLiteral("r0"));
        QCOMPARE(ownerBaseline.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);

        Session sessionB;
        QVERIFY(openProject(&sessionB, workspace.path()));
        Model sourceB;
        ModelIO::open(sourceB, sourcePath);
        sessionB.addModel(sourceB);
        const QJsonObject secondContext = sessionB.documentProjectContext(0);
        const QString secondId = secondContext.value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!secondId.isEmpty());
        QVERIFY(secondId != persistentId);
        QCOMPARE(secondContext.value(QStringLiteral("originDocumentId")).toString(), persistentId);
        QVERIFY(secondContext.value(QStringLiteral("sourcePath")).toString().isEmpty());

        sessionA->getModel(0).getProblemSetup().getModalSetup().setNIterations(52);
        sessionA->setProblemChanged(0);
        const QJsonObject editedContext = sessionA->documentProjectContext(0);
        const QString editedSourceRevision = editedContext.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(editedSourceRevision, QStringLiteral("r1"));

        QString error;
        const QJsonObject snapshot = sessionA->createDocumentSnapshot(0, &error);
        QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
        QCOMPARE(snapshot.value(QStringLiteral("sourceRevision")).toString(), editedSourceRevision);
        QVERIFY2(saveModelJob(sessionA, 0, sourcePath), qPrintable(error));
        const QJsonObject savedOwner = sessionA->documentProjectContext(0);
        QCOMPARE(savedOwner.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(savedOwner.value(QStringLiteral("sourceRevision")).toString(), editedSourceRevision);
        QCOMPARE(sessionB.documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 secondId);
        QVERIFY(!sessionB.documentProjectContext(0).value(QStringLiteral("sourceConflict")).toBool());
        sessionB.clear();
        QCOMPARE(sessionB.getNModels(), 0u);
        QCOMPARE(sessionA->getNModels(), 1u);
        QCOMPARE(sessionA->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 persistentId);
    }

    void transientProjectLockRetainsAndRetriesAcceptedRevision()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture());
        const QString lockPath = QDir(workspace.path()).filePath(QStringLiteral(".project.lock"));
        QLockFile heldLock(lockPath);
        QVERIFY(heldLock.tryLock(0));

        session->getModel(0).getProblemSetup().getModalSetup().setNIterations(46);
        session->setProblemChanged(0);
        const QJsonObject unavailable = session->documentProjectContext(0);
        QCOMPARE(unavailable.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
        QVERIFY(!unavailable.value(QStringLiteral("sourceConflict")).toBool());
        QVERIFY(!session->canSaveModel(0, nativeModelPath(workspace.path(), QStringLiteral("busy-save"))));

        heldLock.unlock();
        QTRY_VERIFY_WITH_TIMEOUT(session->documentProjectContext(0)
                                     .value(QStringLiteral("sourceRevision")).toString() == QStringLiteral("r1"),
                                 10000);
        const QJsonObject recovered = session->documentProjectContext(0);
        QVERIFY(!recovered.value(QStringLiteral("sourceConflict")).toBool());
        QCOMPARE(recovered.value(QStringLiteral("persistence")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("available"));
        QVERIFY(session->canSaveModel(0, nativeModelPath(workspace.path(), QStringLiteral("recovered-save"))));
    }

    void modelActionWorkerExportRemainsSaveable()
    {
        QTemporaryDir workspace;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        Model sliver;
        sliver.setName(QStringLiteral("Worker sliver export source"));
        sliver.setNNodes(3);
        sliver.setNode(0, RNode(0.0, 0.0, 0.0));
        sliver.setNode(1, RNode(1.0, 0.0, 0.0));
        sliver.setNode(2, RNode(0.001, 0.000001, 0.0));
        sliver.setNElements(1);
        sliver.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        RElement triangle(R_ELEMENT_TRI1);
        triangle.setNodeId(0, 0);
        triangle.setNodeId(1, 1);
        triangle.setNodeId(2, 2);
        sliver.setElement(0, triangle);
        sliver.updateSliverElements(Model::SliverElementEdgeRatio);
        QVERIFY(sliver.getNSlivers() > 0);
        session->addModel(sliver);

        auto *action = new ModelAction;
        ModelActionInput exportAction(0);
        exportAction.setExportSliverElements();
        action->addAction(exportAction);
        // The worker export creates a new none-task model. Drive its real task
        // dialog so this test stays focused on Session publication and Save.
        auto *mainWindow = Application::instance()->getMainWindow();
        QTimer taskDialogResolver;
        taskDialogResolver.setInterval(20);
        bool taskDialogHandled = false;
        const auto ensureStressTask = [](QDialog *dialog) {
            auto *tree = dialog->findChild<QTreeWidget *>();
            if (!tree || tree->topLevelItemCount() == 0) return false;
            QTreeWidgetItem *root = tree->topLevelItem(0);
            for (int i = 0; i < root->childCount(); ++i)
                if (root->child(i)->data(0, Qt::UserRole).toInt() == int(R_PROBLEM_STRESS_MODAL))
                    return true;
            auto *item = new QTreeWidgetItem(root);
            item->setText(0, QStringLiteral("Modal analysis"));
            item->setData(0, Qt::UserRole, int(R_PROBLEM_STRESS_MODAL));
            item->setData(1, Qt::UserRole, 1);
            item->setFirstColumnSpanned(true);
            return true;
        };
        QObject::connect(&taskDialogResolver, &QTimer::timeout, mainWindow, [&] {
            const auto dialogs = mainWindow->findChildren<QDialog *>();
            QDialog *taskDialog = nullptr;
            for (QDialog *dialog : dialogs)
                if (dialog->windowTitle() == QStringLiteral("Problem task flow"))
                {
                    taskDialog = dialog;
                    break;
            }
            if (!taskDialog) return;
            if (session->getNModels() == 0) return;
            const uint exportedModel = session->getNModels() - 1;
            session->getModel(exportedModel).setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
            if (ensureStressTask(taskDialog))
            {
                taskDialog->accept();
                taskDialogHandled = true;
                taskDialogResolver.stop();
            }
        });
        taskDialogResolver.start();
        const bool jobFinished = waitForJob(action);
        QTRY_VERIFY_WITH_TIMEOUT(taskDialogHandled, 5000);
        taskDialogResolver.stop();
        QVERIFY(jobFinished);
        QVERIFY(taskDialogHandled);
        QTRY_COMPARE_WITH_TIMEOUT(session->getNModels(), 2u, 5000);
        const QJsonObject exportedContext = session->documentProjectContext(1);
        const QString exportId = exportedContext.value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!exportId.isEmpty());
        QVERIFY(exportedContext.value(QStringLiteral("sourcePath")).toString().isEmpty());

        const QString savePath = nativeModelPath(outputs.path(), QStringLiteral("worker-export"));
        QVERIFY(saveModelJob(session, 1, savePath));
        const QJsonObject saved = session->documentProjectContext(1);
        QCOMPARE(saved.value(QStringLiteral("persistentDocumentId")).toString(), exportId);
        QCOMPARE(saved.value(QStringLiteral("sourcePath")).toString(), QFileInfo(savePath).canonicalFilePath());
        QVERIFY(!saved.value(QStringLiteral("savedArtifactHash")).toString().isEmpty());
    }

    void externallyChangedSourceNeedsExplicitReloadAcceptance()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("external-change"));
        QVERIFY(createSourceModel(sourcePath));
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);
        const QString persistentId = session->documentProjectContext(0)
                                         .value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!persistentId.isEmpty());

        Model externallyChanged = modalFixture();
        externallyChanged.getProblemSetup().getModalSetup().setNIterations(37);
        ModelIO::save(externallyChanged, sourcePath, true);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistence")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("conflict"));
        QString error;
        QVERIFY(!session->canSaveModel(0, sourcePath, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(session->createDocumentSnapshot(0, &error).isEmpty());

        Model reloaded = session->getModel(0);
        ModelIO::update(reloaded, sourcePath);
        session->setModel(0, reloaded);
        QVERIFY2(session->recordModelReloaded(0, sourcePath, &error), qPrintable(error));
        const QJsonObject accepted = session->documentProjectContext(0);
        QCOMPARE(accepted.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(accepted.value(QStringLiteral("persistence")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("available"));
        QCOMPARE(accepted.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 37u);
    }

    void sourceReadBeforeExternalWriteBecomesDetachedInsteadOfRebound()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("read-barrier"));
        QVERIFY(createSourceModel(sourcePath));

        Model loadedA;
        ModelIO::open(loadedA, sourcePath);
        session->addModel(loadedA);
        const QString originalId = session->documentProjectContext(0)
                                       .value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!originalId.isEmpty());
        session->removeModel(0);

        Model externalB = modalFixture();
        externalB.getProblemSetup().getModalSetup().setNIterations(39);
        ModelIO::save(externalB, sourcePath, true);
        QCOMPARE(session->getNModels(), 0u);

        session->addModel(loadedA);
        const QJsonObject detached = session->documentProjectContext(0);
        QVERIFY(detached.value(QStringLiteral("persistentDocumentId")).toString() != originalId);
        QCOMPARE(detached.value(QStringLiteral("originDocumentId")).toString(), originalId);
        QVERIFY(detached.value(QStringLiteral("sourcePath")).toString().isEmpty());
        QVERIFY(detached.value(QStringLiteral("sourceUnverified")).toBool());
        QVERIFY(detached.value(QStringLiteral("detachedFromChangedSource")).toBool());
        QCOMPARE(detached.value(QStringLiteral("sourceLinkStatus")).toString(),
                 QStringLiteral("changedSinceRead"));
        QVERIFY(detached.value(QStringLiteral("savedArtifactHash")).toString().isEmpty());
        QString error;
        const QJsonObject snapshot = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
        const QString snapshotPath = QDir(session->projectWorkspace()).filePath(
            snapshot.value(QStringLiteral("relativePath")).toString());
        Model snapshotModel;
        ModelIO::open(snapshotModel, snapshotPath);
        QCOMPARE(snapshotModel.getProblemSetup().getModalSetup().getNIterations(), 12u);
        QVERIFY(session->getModel(0).getFileName().isEmpty());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);

        const QByteArray currentSourceBytes = fileBytes(sourcePath);
        const QString copySavePath = nativeModelPath(sources.path(), QStringLiteral("read-barrier-copy"));
        QVERIFY(saveModelJob(session, 0, copySavePath));
        const QJsonObject savedCopy = session->documentProjectContext(0);
        QVERIFY(savedCopy.value(QStringLiteral("sourcePath")).toString()
                == QFileInfo(copySavePath).canonicalFilePath());
        QVERIFY(!savedCopy.value(QStringLiteral("detachedFromChangedSource")).toBool());
        QVERIFY(!saveModelJob(session, 0, sourcePath));
        QCOMPARE(fileBytes(sourcePath), currentSourceBytes);
    }

    void reloadBindingChangedAfterReadIsRejectedBeforePublication()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("reload-barrier"));
        QVERIFY(createSourceModel(sourcePath));
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);

        Model externalB = modalFixture();
        externalB.getProblemSetup().getModalSetup().setNIterations(31);
        ModelIO::save(externalB, sourcePath, true);
        Model reloaded = session->getModel(0);
        RProjectCatalog::SourceBinding sourceBindingB;
        ModelIO::update(reloaded, sourcePath, &sourceBindingB);

        Model externalC = modalFixture();
        externalC.getProblemSetup().getModalSetup().setNIterations(41);
        ModelIO::save(externalC, sourcePath, true);

        QString error;
        QVERIFY(!session->validateReloadSource(0, sourceBindingB, &error));
        QVERIFY(!error.isEmpty());
        const QJsonObject rejected = session->documentProjectContext(0);
        QVERIFY(rejected.value(QStringLiteral("sourceConflict")).toBool());
        QCOMPARE(rejected.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
    }

    void publicationQueriesStayCachedUntilCommit()
    {
        QTemporaryDir workspace;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture());

        const QString outputPath = nativeModelPath(outputs.path(), QStringLiteral("publishing"));
        const QStringList outputPaths = session->getModel(0).nativeWritePaths(outputPath, true);
        QString error;
        auto lease = session->reserveModelSave(0, outputPath, outputPaths, &error);
        QVERIFY2(lease, qPrintable(error));
        const QPair<QString, quint64> stamp = session->modelCommandStamp(0);
        const QJsonObject beforeWrite = session->documentProjectContext(0);
        QCOMPARE(beforeWrite.value(QStringLiteral("persistence")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("publishing"));
        QVERIFY(!beforeWrite.value(QStringLiteral("sourceConflict")).toBool());

        Model frozen = session->getModel(0);
        ModelIO::save(frozen, outputPath, true);
        const QJsonObject afterWrite = session->documentProjectContext(0);
        QCOMPARE(afterWrite.value(QStringLiteral("persistence")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("publishing"));
        QVERIFY(!afterWrite.value(QStringLiteral("sourceConflict")).toBool());
        QVERIFY(session->createDocumentSnapshot(0, &error).isEmpty());
        QVERIFY(!error.isEmpty());

        QVERIFY(frozen.getLoadedSourceBinding().has_value());
        QVERIFY2(session->recordModelSaved(0, frozen.getFileName(), *frozen.getLoadedSourceBinding(),
                                           lease, stamp, &error), qPrintable(error));
        const QJsonObject committed = session->documentProjectContext(0);
        QCOMPARE(committed.value(QStringLiteral("persistence")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(committed.value(QStringLiteral("sourcePath")).toString(), QFileInfo(outputPath).canonicalFilePath());
        QVERIFY(!committed.value(QStringLiteral("savedArtifactHash")).toString().isEmpty());
    }

    void publicationRejectsReplacementAfterNativeWriteAndKeepsModelDirty()
    {
        QTemporaryDir workspace;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture());
        session->getModel(0).getProblemSetup().getModalSetup().setNIterations(55);
        session->setProblemChanged(0);
        const QJsonObject beforeSave = session->documentProjectContext(0);
        QCOMPARE(beforeSave.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);

        const QString outputPath = nativeModelPath(outputs.path(), QStringLiteral("write-replaced"));
        const QStringList outputPaths = session->getModel(0).nativeWritePaths(outputPath, true);
        QString error;
        auto lease = session->reserveModelSave(0, outputPath, outputPaths, &error);
        QVERIFY2(lease, qPrintable(error));
        const QPair<QString, quint64> stamp = session->modelCommandStamp(0);

        Model writtenB = session->getModel(0);
        ModelIO::save(writtenB, outputPath, true);
        QVERIFY(writtenB.getLoadedSourceBinding().has_value());
        const RProjectCatalog::SourceBinding expectedB = *writtenB.getLoadedSourceBinding();
        Model replacementC = modalFixture();
        replacementC.getProblemSetup().getModalSetup().setNIterations(91);
        ModelIO::save(replacementC, outputPath, true);
        const auto actualC = RProjectCatalog::resolveSourceBinding(outputPath);
        QVERIFY(actualC.has_value());
        QVERIFY(actualC->savedArtifactHash != expectedB.savedArtifactHash);

        QVERIFY(!session->recordModelSaved(0, expectedB.logicalSourcePath, expectedB,
                                           lease, stamp, &error));
        QVERIFY(!error.isEmpty());
        lease.reset();
        const QJsonObject rejected = session->documentProjectContext(0);
        QVERIFY(rejected.value(QStringLiteral("sourceConflict")).toBool());
        QVERIFY(rejected.value(QStringLiteral("sourcePath")).toString().isEmpty());
        QVERIFY(rejected.value(QStringLiteral("savedArtifactHash")).toString().isEmpty());
        QCOMPARE(rejected.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);
        QVERIFY(session->getModel(0).getFileName().isEmpty());
        const auto stillC = RProjectCatalog::resolveSourceBinding(outputPath);
        QVERIFY(stillC.has_value());
        QCOMPARE(stillC->savedArtifactHash, actualC->savedArtifactHash);
    }

    void nativeSaveRevalidatesEachTargetImmediatelyBeforeCommit()
    {
        QTemporaryDir workspace;
        QTemporaryDir outputs;
        QTemporaryDir staging;
        QVERIFY(workspace.isValid() && outputs.isValid() && staging.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        const QString outputPath = nativeModelPath(outputs.path(), QStringLiteral("replace-during-save"));
        Model previous = modalFixture(QStringLiteral("Existing output"));
        ModelIO::save(previous, outputPath, true);
        session->addModel(modalFixture(QStringLiteral("Tracked output")));
        Model staged = session->getModel(0);
        staged.getProblemSetup().getModalSetup().setNIterations(73);
        const QString stagedRequestedPath = QDir(staging.path()).filePath(QFileInfo(outputPath).fileName());
        ModelIO::save(staged, stagedRequestedPath, true);
        QVERIFY(staged.getLoadedSourceBinding().has_value());

        const QStringList publicPaths = session->getModel(0).nativeWritePaths(outputPath, true);
        const QStringList stagedPaths = staged.nativeWritePaths(stagedRequestedPath, true);
        QCOMPARE(publicPaths.size(), stagedPaths.size());
        QVERIFY(publicPaths.size() >= 2);

        const QString stageRoot = QDir::cleanPath(QDir::fromNativeSeparators(staging.path()));
        const QString outputRoot = QDir::cleanPath(QDir::fromNativeSeparators(outputs.path()));
        const auto mapStagedPath = [&stageRoot, &outputRoot](const QString &path) {
            const QString absolute = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
            const QString relative = QDir::fromNativeSeparators(QDir(stageRoot).relativeFilePath(absolute));
            return QDir::cleanPath(QDir::fromNativeSeparators(QDir(outputRoot).filePath(relative)));
        };
        RProjectCatalog::SourceBinding expectedWrittenBinding = *staged.getLoadedSourceBinding();
        expectedWrittenBinding.logicalSourcePath = mapStagedPath(expectedWrittenBinding.logicalSourcePath);
        expectedWrittenBinding.resolvedSourcePath = mapStagedPath(expectedWrittenBinding.resolvedSourcePath);
        for (auto &artifact : expectedWrittenBinding.sourceArtifacts)
            artifact.path = mapStagedPath(artifact.path);

        QString error;
        auto lease = session->reserveModelSave(0, outputPath, publicPaths, &error);
        QVERIFY2(lease, qPrintable(error));
        const QString runtimeDocumentId = session->modelCommandStamp(0).first;
        const auto abortPublication = qScopeGuard([session, &runtimeDocumentId, &lease] {
            session->abortModelSave(runtimeDocumentId, lease);
        });
        QVERIFY2(session->bindModelSaveArtifacts(runtimeDocumentId, expectedWrittenBinding, lease, &error),
                 qPrintable(error));

        const QByteArray replacementBytes("external replacement during publication");
        bool replacementWritten = false;
        qsizetype publishedFiles = 0;
        QString copyError;
        for (qsizetype i = 0; i < stagedPaths.size(); ++i)
        {
            const QString targetPath = publicPaths.at(i);
            const auto validateTarget = [session, runtimeDocumentId, &lease, &replacementWritten,
                                         &replacementBytes, i, targetPath](QString &validationError) {
                if (i == 1 && !replacementWritten)
                {
                    QSaveFile externalWriter(targetPath);
                    externalWriter.setDirectWriteFallback(false);
                    if (!externalWriter.open(QIODevice::WriteOnly))
                    {
                        validationError = externalWriter.errorString();
                        return false;
                    }
                    if (externalWriter.write(replacementBytes) != replacementBytes.size()
                        || !externalWriter.commit())
                    {
                        validationError = externalWriter.errorString();
                        externalWriter.cancelWriting();
                        return false;
                    }
                    replacementWritten = true;
                }
                return session->validateModelSaveArtifactTarget(runtimeDocumentId, targetPath,
                                                                 lease, &validationError);
            };
            if (!ModelIOPrivate::copyFileAtomically(stagedPaths.at(i), targetPath,
                                                    validateTarget, copyError))
                break;
            ++publishedFiles;
        }

        QVERIFY(replacementWritten);
        QCOMPARE(publishedFiles, 1);
        QVERIFY(!copyError.isEmpty());
        QCOMPARE(fileBytes(publicPaths.first()), fileBytes(stagedPaths.first()));
        QCOMPARE(fileBytes(publicPaths.at(1)), replacementBytes);
        session->abortModelSave(runtimeDocumentId, lease, copyError);
        QCOMPARE(fileBytes(publicPaths.first()), fileBytes(stagedPaths.first()));
        QCOMPARE(fileBytes(publicPaths.at(1)), replacementBytes);
        Q_UNUSED(abortPublication);
    }

    void nativeLinkTargetTamperingIsDetected()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("target-tamper"));
        QVERIFY(createSourceModel(sourcePath));
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        const QStringList writePaths = loaded.nativeWritePaths(sourcePath, true);
        QVERIFY(writePaths.size() >= 2);
        QString targetPath;
        for (const QString &candidate : writePaths)
            if (QFileInfo(candidate).canonicalFilePath() != QFileInfo(sourcePath).canonicalFilePath())
            {
                targetPath = candidate;
                break;
            }
        QVERIFY(!targetPath.isEmpty());
        QVERIFY(QFileInfo::exists(targetPath));
        session->addModel(loaded);

        QFile target(targetPath);
        QVERIFY(target.open(QIODevice::Append));
        QCOMPARE(target.write("x", 1), qint64(1));
        target.close();
        const QJsonObject context = session->documentProjectContext(0);
        QVERIFY(context.value(QStringLiteral("sourceConflict")).toBool());
        QCOMPARE(context.value(QStringLiteral("persistence")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("conflict"));
        QString error;
        QVERIFY(!session->canSaveModel(0, sourcePath, &error));
        QVERIFY(session->createDocumentSnapshot(0, &error).isEmpty());
    }

    void nativeSaveLeaseRejectsNumberedAliasAndManagedSnapshotPaths()
    {
        QTemporaryDir workspace;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture(QStringLiteral("First save document")));
        session->addModel(modalFixture(QStringLiteral("Second save document")));

        const QString basePath = nativeModelPath(outputs.path(), QStringLiteral("beam"));
        QVERIFY(saveModelJob(session, 0, basePath));
        const QStringList firstWritePaths = session->getModel(0).nativeWritePaths(basePath, true);
        QVERIFY(firstWritePaths.size() >= 2);
        QString recordTarget;
        for (const QString &candidate : firstWritePaths)
            if (QFileInfo(candidate).canonicalFilePath() != QFileInfo(basePath).canonicalFilePath())
            {
                recordTarget = candidate;
                break;
            }
        QVERIFY(!recordTarget.isEmpty());
        const QByteArray targetBefore = fileBytes(recordTarget);
        const QByteArray linkBefore = fileBytes(basePath);
        QVERIFY(!targetBefore.isEmpty() && !linkBefore.isEmpty());

        const QString numberedSaveAs = nativeModelPath(outputs.path(), QStringLiteral("beam-00001"));
        QString error;
        QVERIFY(!session->canSaveModel(1, numberedSaveAs, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!saveModelJob(session, 1, numberedSaveAs));
        QCOMPARE(fileBytes(recordTarget), targetBefore);
        QCOMPARE(fileBytes(basePath), linkBefore);

        const QJsonObject snapshot = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
        const QString managedPath = QDir(session->projectWorkspace()).filePath(
            snapshot.value(QStringLiteral("relativePath")).toString());
        const QByteArray snapshotBefore = fileBytes(managedPath);
        QVERIFY(!snapshotBefore.isEmpty());
        QVERIFY(!session->canSaveModel(1, managedPath, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!saveModelJob(session, 1, managedPath));
        QCOMPARE(fileBytes(managedPath), snapshotBefore);
    }

    void workerSliverExportIsPublishedOnSessionThreadAndCanSave()
    {
        QTemporaryDir workspace;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        Model source;
        source.setName(QStringLiteral("Worker export source"));
        source.setNNodes(3);
        source.setNode(0, RNode(0.0, 0.0, 0.0));
        source.setNode(1, RNode(1.0, 0.0, 0.0));
        source.setNode(2, RNode(0.001, 0.000001, 0.0));
        source.setNElements(1);
        source.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        RElement triangle(R_ELEMENT_TRI1);
        triangle.setNodeId(0, 0);
        triangle.setNodeId(1, 1);
        triangle.setNodeId(2, 2);
        source.setElement(0, triangle);
        source.updateSliverElements(Model::SliverElementEdgeRatio);
        QVERIFY(source.getNSlivers() > 0);
        session->addModel(source);

        auto *action = new ModelAction;
        ModelActionInput exportInput(0);
        exportInput.setExportSliverElements();
        action->addAction(exportInput);
        // The worker export creates a new none-task model. Drive its real task
        // dialog so this test stays focused on Session publication and Save.
        auto *mainWindow = Application::instance()->getMainWindow();
        QTimer taskDialogResolver;
        taskDialogResolver.setInterval(20);
        bool taskDialogHandled = false;
        const auto ensureStressTask = [](QDialog *dialog) {
            auto *tree = dialog->findChild<QTreeWidget *>();
            if (!tree || tree->topLevelItemCount() == 0) return false;
            QTreeWidgetItem *root = tree->topLevelItem(0);
            for (int i = 0; i < root->childCount(); ++i)
                if (root->child(i)->data(0, Qt::UserRole).toInt() == int(R_PROBLEM_STRESS_MODAL))
                    return true;
            auto *item = new QTreeWidgetItem(root);
            item->setText(0, QStringLiteral("Modal analysis"));
            item->setData(0, Qt::UserRole, int(R_PROBLEM_STRESS_MODAL));
            item->setData(1, Qt::UserRole, 1);
            item->setFirstColumnSpanned(true);
            return true;
        };
        QObject::connect(&taskDialogResolver, &QTimer::timeout, mainWindow, [&] {
            const auto dialogs = mainWindow->findChildren<QDialog *>();
            QDialog *taskDialog = nullptr;
            for (QDialog *dialog : dialogs)
                if (dialog->windowTitle() == QStringLiteral("Problem task flow"))
                {
                    taskDialog = dialog;
                    break;
                }
            if (!taskDialog) return;
            if (session->getNModels() == 0) return;
            const uint exportedModel = session->getNModels() - 1;
            session->getModel(exportedModel).setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
            if (ensureStressTask(taskDialog))
            {
                taskDialog->accept();
                taskDialogHandled = true;
                taskDialogResolver.stop();
            }
        });
        taskDialogResolver.start();
        const bool jobFinished = waitForJob(action);
        QTRY_VERIFY_WITH_TIMEOUT(taskDialogHandled, 5000);
        taskDialogResolver.stop();
        QVERIFY(jobFinished);
        QVERIFY(taskDialogHandled);
        QTRY_COMPARE_WITH_TIMEOUT(session->getNModels(), 2u, 5000);

        const QJsonObject exported = session->documentProjectContext(1);
        const QString persistentId = exported.value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!persistentId.isEmpty());
        QVERIFY(exported.value(QStringLiteral("sourcePath")).toString().isEmpty());
        const QString savePath = nativeModelPath(outputs.path(), QStringLiteral("sliver-export"));
        QVERIFY(saveModelJob(session, 1, savePath));
        const QJsonObject saved = session->documentProjectContext(1);
        QCOMPARE(saved.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(saved.value(QStringLiteral("sourcePath")).toString(), QFileInfo(savePath).canonicalFilePath());
        QVERIFY(!saved.value(QStringLiteral("savedArtifactHash")).toString().isEmpty());
        const auto sourceBinding = RProjectCatalog::resolveSourceBinding(savePath);
        QVERIFY(sourceBinding.has_value());
        QCOMPARE(saved.value(QStringLiteral("savedArtifactHash")).toString(), sourceBinding->savedArtifactHash);
    }

    void modalNumericDraftIsScopedAndSurvivesTreeRefresh()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);

        ProblemTree problemTree;
        auto *tolerance = activeProblemChild<ValueLineEdit>(&problemTree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        const double acceptedTolerance = session->getModel(0).getProblemSetup().getModalSetup().getConvergenceValue();
        QSignalSpy acceptedValues(tolerance, &ValueLineEdit::valueChanged);
        tolerance->setText(QStringLiteral("1e-"));
        QVERIFY(tolerance->getValidatorState() != QValidator::Acceptable);
        QVERIFY(acceptedValues.isEmpty());

        const QJsonObject pending = session->documentProjectContext(0);
        QCOMPARE(pending.value(QStringLiteral("draft")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));
        QCOMPARE(pending.value(QStringLiteral("draft")).toObject().value(QStringLiteral("scope")).toString(),
                 QStringLiteral("analysis.modal"));
        // Inspect draft semantics rather than arbitrary bytes: valid document
        // and project UUIDs can also contain the substring "1e-".
        QCOMPARE(pending.value(QStringLiteral("draft")).toObject(),
                 (QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")},
                              {QStringLiteral("scope"), QStringLiteral("analysis.modal")},
                              {QStringLiteral("changedFields"), QJsonArray{QStringLiteral("convergenceValue")}},
                              {QStringLiteral("otherScopesStatus"), QStringLiteral("unknown")}}));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getConvergenceValue(), acceptedTolerance);
        QVERIFY(session->hasDocumentDraft(0));
        QVERIFY(!session->canMutateDocument(0));

        session->setModelSelectionChanged(0);
        tolerance = activeProblemChild<ValueLineEdit>(&problemTree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        QCOMPARE(tolerance->text(), QStringLiteral("1e-"));
        QSignalSpy acceptedAfterRefresh(tolerance, &ValueLineEdit::valueChanged);
        tolerance->setText(QStringLiteral("0.0005"));
        QCOMPARE(acceptedAfterRefresh.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!session->hasDocumentDraft(0), 5000);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getConvergenceValue(), 0.0005);
        const QJsonObject accepted = session->documentProjectContext(0);
        QCOMPARE(accepted.value(QStringLiteral("draft")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("none"));
        QVERIFY(accepted.value(QStringLiteral("draft")).toObject().value(QStringLiteral("otherScopesStatus")).toString()
                == QStringLiteral("unknown"));
        QVERIFY(session->canMutateDocument(0));
    }

    void cleanDraftStateSurvivesSaveReopenAndAllowsHostApply()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);

        auto *problemTree = Application::instance()->getMainWindow()->findChild<ProblemTree *>();
        QVERIFY(problemTree);
        auto *tolerance = activeProblemChild<ValueLineEdit>(problemTree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        tolerance->setText(QStringLiteral("1e-"));
        QVERIFY(session->hasDocumentDraft(0));
        captureWindowEvidence(QStringLiteral("pending-modal-draft"));
        tolerance->setText(QStringLiteral("0.0006"));
        QTRY_VERIFY_WITH_TIMEOUT(!session->hasDocumentDraft(0), 5000);
        QVERIFY(session->canMutateDocument(0));

        const QString savePath = nativeModelPath(sources.path(), QStringLiteral("clean-draft"));
        QVERIFY(saveModelJob(session, 0, savePath));
        const QString persistentId = session->documentProjectContext(0)
                                         .value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!persistentId.isEmpty());
        session->clear();
        QVERIFY(openProject(session, workspace.path()));

        Model reopened;
        ModelIO::open(reopened, savePath);
        session->addModel(reopened);
        session->setModelSelected(0, true);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 persistentId);
        QVERIFY(!session->hasDocumentDraft(0));
        QVERIFY(session->canMutateDocument(0));
        QString error;
        const auto reopenedSnapshot = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!reopenedSnapshot.isEmpty(), qPrintable(error));
        verifyCatalogAcrossClients(session, reopenedSnapshot);
        captureWindowEvidence(QStringLiteral("reopened-frozen-document"));

        RModalSetup edited = session->getModel(0).getProblemSetup().getModalSetup();
        edited.setNIterations(23);
        QVERIFY2(session->canMutateDocument(0, &error), qPrintable(error));
        QVERIFY2(session->getModelCommandHost()->configureModal({0}, edited, &error), qPrintable(error));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 23u);
        QVERIFY(saveModelJob(session, 0, savePath));
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 persistentId);
    }

    void pendingNumericDraftSurvivesCloseReopenWithoutAcceptedDirty()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("pending-draft"));
        QVERIFY(createSourceModel(sourcePath));
        Model reopened;
        ModelIO::open(reopened, sourcePath);
        session->addModel(reopened);
        session->setModelSelected(0, true);
        auto *problemTree = Application::instance()->getMainWindow()->findChild<ProblemTree *>();
        QVERIFY(problemTree);
        const QString persistentId = session->documentProjectContext(0)
                                         .value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!persistentId.isEmpty());
        const QJsonObject savedBaseline = session->documentProjectContext(0);
        const QString sourceRevisionBeforeDraft = savedBaseline.value(QStringLiteral("sourceRevision")).toString();
        const QString savedHashBeforeDraft = savedBaseline.value(QStringLiteral("savedArtifactHash")).toString();
        const double acceptedTolerance = session->getModel(0).getProblemSetup().getModalSetup().getConvergenceValue();
        QCOMPARE(sourceRevisionBeforeDraft, QStringLiteral("r0"));
        QCOMPARE(savedBaseline.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);

        auto *tolerance = activeProblemChild<ValueLineEdit>(problemTree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        tolerance->setText(QStringLiteral("1e-"));
        QVERIFY(session->hasDocumentDraft(0));
        const QJsonObject pendingContext = session->documentProjectContext(0);
        QCOMPARE(pendingContext.value(QStringLiteral("sourceRevision")).toString(), sourceRevisionBeforeDraft);
        QCOMPARE(pendingContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);
        QVERIFY(!QJsonDocument(pendingContext).toJson(QJsonDocument::Compact).contains("1e-"));
        QTRY_VERIFY_WITH_TIMEOUT(session->documentProjectContext(0)
                                     .value(QStringLiteral("persistence")).toObject()
                                     .value(QStringLiteral("status")).toString()
                                     == QStringLiteral("available"), 5000);
        session->clear();
        QVERIFY(openProject(session, workspace.path()));
        Model loadedAgain;
        ModelIO::open(loadedAgain, sourcePath);
        session->addModel(loadedAgain);
        session->setModelSelected(0, true);
        const QJsonObject recovered = session->documentProjectContext(0);
        QCOMPARE(recovered.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(recovered.value(QStringLiteral("draft")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));
        QVERIFY(session->hasDocumentDraft(0));
        QCOMPARE(recovered.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool(),
                 false);
        QCOMPARE(recovered.value(QStringLiteral("sourceRevision")).toString(), sourceRevisionBeforeDraft);
        QCOMPARE(recovered.value(QStringLiteral("savedArtifactHash")).toString(), savedHashBeforeDraft);
        QVERIFY(!session->canMutateDocument(0));
        QString error;
        QVERIFY(session->createDocumentSnapshot(0, &error).isEmpty());
        QVERIFY(!error.isEmpty());

        QTRY_VERIFY_WITH_TIMEOUT(activeProblemChild<QPushButton>(problemTree, QStringLiteral("keepAcceptedModalValue")),
                                 5000);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getConvergenceValue(), acceptedTolerance);
        const quint64 recoveredRuntimeRevision = session->modelCommandStamp(0).second;
        const uint undoDepthBeforeKeep = session->getModel(0).getUndoStackSize();
        auto *keepAcceptedValue = activeProblemChild<QPushButton>(problemTree, QStringLiteral("keepAcceptedModalValue"));
        QVERIFY(keepAcceptedValue);
        auto *newDraft = activeProblemChild<ValueLineEdit>(problemTree, QStringLiteral("modalTolerance"));
        QVERIFY(newDraft);
        newDraft->setText(QStringLiteral("1e-"));
        QVERIFY(session->hasDocumentDraft(0));
        QCOMPARE(session->modelCommandStamp(0).second, recoveredRuntimeRevision);
        QTRY_VERIFY_WITH_TIMEOUT(keepAcceptedValue->isEnabled(), 5000);
        QSignalSpy confirmed(keepAcceptedValue, &QPushButton::clicked);
        if (qEnvironmentVariableIsSet("RANGE_PROJECT_GUI_EVIDENCE_DIRECTORY"))
        {
            captureWindowEvidence(QStringLiteral("recovered-draft-prompt"));
            QVERIFY(keepAcceptedValue->isVisible());
            QTest::mouseClick(keepAcceptedValue, Qt::LeftButton);
        }
        else
        {
            // The headless Application deliberately leaves MainWindow hidden.
            // Exercise its actual button action; the Windows gate uses a mouse.
            keepAcceptedValue->click();
        }
        QCOMPARE(confirmed.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!session->hasDocumentDraft(0), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!activeProblemChild<QPushButton>(problemTree, QStringLiteral("keepAcceptedModalValue")),
                                 5000);
        const QJsonObject acceptedAgain = session->documentProjectContext(0);
        QCOMPARE(acceptedAgain.value(QStringLiteral("sourceRevision")).toString(), sourceRevisionBeforeDraft);
        QCOMPARE(acceptedAgain.value(QStringLiteral("savedArtifactHash")).toString(), savedHashBeforeDraft);
        QCOMPARE(acceptedAgain.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool(),
                 false);
        QCOMPARE(acceptedAgain.value(QStringLiteral("draft")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("none"));
        QCOMPARE(session->modelCommandStamp(0).second, recoveredRuntimeRevision);
        QCOMPARE(session->getModel(0).getUndoStackSize(), undoDepthBeforeKeep);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getConvergenceValue(), acceptedTolerance);
        auto *acceptedEditor = activeProblemChild<ValueLineEdit>(problemTree, QStringLiteral("modalTolerance"));
        QVERIFY(acceptedEditor);
        QCOMPARE(acceptedEditor->getValidatorState(), QValidator::Acceptable);
        QVERIFY(acceptedEditor->text() != QStringLiteral("1e-"));
        QVERIFY(session->canMutateDocument(0));
        const QJsonObject allowedSnapshot = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!allowedSnapshot.isEmpty(), qPrintable(error));
        QCOMPARE(allowedSnapshot.value(QStringLiteral("sourceRevision")).toString(), sourceRevisionBeforeDraft);
        Model snapshotModel;
        ModelIO::open(snapshotModel, QDir(session->projectWorkspace()).filePath(
                                      allowedSnapshot.value(QStringLiteral("relativePath")).toString()));
        QCOMPARE(snapshotModel.getProblemSetup().getModalSetup().getConvergenceValue(), acceptedTolerance);
    }

    void unchangedWorkflowSourceDoesNotSplitAnotherDocumentsUndoRevision()
    {
        QTemporaryDir project, sources, workflowWorkspace;
        QVERIFY(project.isValid() && sources.isValid() && workflowWorkspace.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, project.path()));
        const QString sourceAPath = nativeModelPath(sources.path(), QStringLiteral("cross-document-A"));
        QVERIFY(createSourceModel(sourceAPath));
        Model sourceA;
        ModelIO::open(sourceA, sourceAPath);
        session->addModel(sourceA);

        // Use the same bounded beam fixture as the current-document workflow
        // tests, rather than manufacturing a workflow source registration.
#ifdef Q_OS_WIN
        const QString executableSuffix = QStringLiteral(".exe");
#else
        const QString executableSuffix;
#endif
        const QString generator = QFileInfo(QStringLiteral(RANGE_PROJECT_TEST_NATIVE)).dir()
            .filePath(QStringLiteral("range-workflow-native-import-fixture") + executableSuffix);
        QVERIFY2(QFileInfo(generator).isFile(), qPrintable(generator));
        const QString sourceBPath = nativeModelPath(sources.path(), QStringLiteral("cross-document-B"));
        QProcess fixture;
        fixture.start(generator, {QStringLiteral("beam"), sourceBPath});
        QVERIFY(fixture.waitForFinished(30000));
        QCOMPARE(fixture.exitStatus(), QProcess::NormalExit);
        QCOMPARE(fixture.exitCode(), 0);
        Model sourceB;
        ModelIO::open(sourceB, sourceBPath);
        session->addModel(sourceB);
        session->setModelSelected(0, false);
        session->setModelSelected(1, true);

        WorkflowWidget workflow;
        workflow.setSession(session);
        auto configuration = WorkflowProcess::defaultConfiguration(workflowWorkspace.path());
        configuration.solver = QStringLiteral(RANGE_PROJECT_TEST_SOLVER);
        configuration.native = QStringLiteral(RANGE_PROJECT_TEST_NATIVE);
        workflow.openWorkspace(configuration);
        QTRY_VERIFY_WITH_TIMEOUT(workflow.transport()->isReady(), 30000);
        QSignalSpy replies(workflow.transport(), &WorkflowProcess::replyReceived);
        auto *capture = workflow.findChild<QPushButton *>(QStringLiteral("workflowUseDocumentSnapshot"));
        QVERIFY(capture && capture->isEnabled());
        capture->click();
        const auto registeredSource = [&replies] {
            for (const auto &reply : replies)
                if (reply[1].toString() == QStringLiteral("model.source.register"))
                    return reply[2].toJsonObject();
            return QJsonObject{};
        };
        QTRY_VERIFY_WITH_TIMEOUT(!registeredSource().isEmpty(), 30000);
        const QString sourceId = registeredSource().value(QStringLiteral("sourceId")).toString();
        QVERIFY(!sourceId.isEmpty());
        auto *choice = workflow.findChild<QComboBox *>(QStringLiteral("workflowModel"));
        QVERIFY(choice);
        QCOMPARE(choice->currentData(Qt::UserRole + 1).toString(), sourceId);
        auto *configure = workflow.findChild<QPushButton *>(QStringLiteral("workflowConfigure"));
        auto *workflowApply = workflow.findChild<QPushButton *>(QStringLiteral("workflowApply"));
        QVERIFY(configure && configure->isEnabled() && workflowApply);
        configure->click();
        QTRY_VERIFY_WITH_TIMEOUT(workflowApply->isEnabled(), 30000);
        const auto workflowPreview = workflow.currentPreview();
        const auto bStamp = session->modelCommandStamp(1);
        const auto bContext = session->documentProjectContext(1);

        session->setModelSelected(1, false);
        session->setModelSelected(0, true);
        auto modal = session->getModel(0).getProblemSetup().getModalSetup();
        modal.setNIterations(24);
        QString error;
        QVERIFY2(session->getModelCommandHost()->configureModal({0}, modal, &error), qPrintable(error));
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("sourceRevision")).toString(),
                 QStringLiteral("r1"));

        GeometryCommandWidget geometry;
        geometry.setSession(session);
        geometry.findChild<QLineEdit *>(QStringLiteral("geometryNodeId"))->setText(
            QString::number(session->getModel(0).getNodePhysicalId(1)));
        geometry.findChild<QLineEdit *>(QStringLiteral("geometryCoordinates0"))->setText(QStringLiteral("2"));
        geometry.preview();
        auto *geometryApply = geometry.findChild<QPushButton *>(QStringLiteral("geometryApproveApply"));
        QVERIFY(geometryApply && geometryApply->isEnabled());
        const auto aStamp = session->modelCommandStamp(0);
        auto *actions = Application::instance()->getActionList();
        actions->processAvailability();
        auto *undo = actions->getAction(Action::ACTION_GEOMETRY_UNDO);
        QVERIFY(undo->isEnabled());
        undo->trigger();

        // Undo emits recordExternalChange followed by setModelChanged. A
        // synchronous check of unchanged source B used to flush A between them.
        QVERIFY(!geometryApply->isEnabled());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
        QCOMPARE(session->modelCommandStamp(0).second, aStamp.second + 2);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("sourceRevision")).toString(),
                 QStringLiteral("r2"));
        QCOMPARE(session->modelCommandStamp(1), bStamp);
        const auto afterB = session->documentProjectContext(1);
        for (const auto &field : {QStringLiteral("persistentDocumentId"), QStringLiteral("sourceRevision"),
                                  QStringLiteral("modelSha256"), QStringLiteral("unsaved")})
            QCOMPARE(afterB.value(field), bContext.value(field));
        QCOMPARE(choice->currentData(Qt::UserRole + 1).toString(), sourceId);
        QCOMPARE(workflow.currentPreview(), workflowPreview);
        QCoreApplication::processEvents();
        QVERIFY(workflowApply->isEnabled());
        QVERIFY(!geometryApply->isEnabled());
    }

    void acceptedModalUndoRedoBackgroundSaveAndDuplicateNameReorderKeepIdentity()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QTemporaryDir outputs;
        QVERIFY(workspace.isValid() && sources.isValid() && outputs.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        const QString repeatedName = QStringLiteral("Repeated project session document");
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("undo-redo-source"));
        QVERIFY(createSourceModel(sourcePath));
        Model loadedSource;
        ModelIO::open(loadedSource, sourcePath);
        loadedSource.setName(repeatedName);
        session->addModel(loadedSource);
        session->setModelSelected(0, true);
        const QJsonObject initialContext = session->documentProjectContext(0);
        const QString persistentId = initialContext.value(QStringLiteral("persistentDocumentId")).toString();
        const QString runtimeId = session->modelCommandStamp(0).first;
        QVERIFY(!persistentId.isEmpty() && !runtimeId.isEmpty());
        const QString baselineSourceRevision = initialContext.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(baselineSourceRevision, QStringLiteral("r0"));
        QCOMPARE(initialContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);
        const auto documentRow = [session](const QString &id) {
            for (const QJsonValue &value : session->projectContext()
                                                .value(QStringLiteral("documents")).toArray())
                if (value.toObject().value(QStringLiteral("persistentDocumentId")).toString() == id)
                    return value.toObject();
            return QJsonObject{};
        };

        auto *host = session->getModelCommandHost();
        const QPair<QString, quint64> initialStamp = session->modelCommandStamp(0);
        QVERIFY(queryHostDocument(session, 0).contains(QStringLiteral("documentRevision")));
        QCOMPARE(queryHostDocument(session, 0).value(QStringLiteral("documentRevision")).toDouble(),
                 double(initialStamp.second));
        RModalSetup configured = session->getModel(0).getProblemSetup().getModalSetup();
        configured.setNIterations(24);
        QString error;
        QVERIFY2(host->configureModal({0}, configured, &error), qPrintable(error));
        const QJsonObject configuredContext = session->documentProjectContext(0);
        QCOMPARE(configuredContext.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        const QString configuredSourceRevision = configuredContext.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(configuredSourceRevision, QStringLiteral("r1"));
        QCOMPARE(configuredContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);
        QCOMPARE(documentRow(persistentId).value(QStringLiteral("sourceRevision")).toString(),
                 configuredSourceRevision);
        QVERIFY(documentRow(persistentId).value(QStringLiteral("dirty")).toBool());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 24u);
        const QPair<QString, quint64> configuredStamp = session->modelCommandStamp(0);
        QVERIFY(configuredStamp.second > initialStamp.second);
        QCOMPARE(queryHostDocument(session, 0).value(QStringLiteral("documentRevision")).toDouble(),
                 double(configuredStamp.second));

        const QJsonObject frozenArtifact = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!frozenArtifact.isEmpty(), qPrintable(error));
        QCOMPARE(frozenArtifact.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(frozenArtifact.value(QStringLiteral("sourceRevision")).toString(), configuredSourceRevision);
        const QString frozenRelativePath = frozenArtifact.value(QStringLiteral("relativePath")).toString();
        const QString frozenPath = QDir(session->projectWorkspace()).filePath(frozenRelativePath);
        QVERIFY(QFileInfo::exists(frozenPath));
        Model frozenBeforeHistory;
        ModelIO::open(frozenBeforeHistory, frozenPath);
        QCOMPARE(frozenBeforeHistory.getProblemSetup().getModalSetup().getNIterations(), 24u);

        const QJsonObject rowWithSnapshot = documentRow(persistentId);
        QCOMPARE(rowWithSnapshot.value(QStringLiteral("snapshotSourceRevision")).toString(),
                 configuredSourceRevision);
        QCOMPARE(rowWithSnapshot.value(QStringLiteral("snapshotRelativePath")).toString(), frozenRelativePath);
        QCOMPARE(rowWithSnapshot.value(QStringLiteral("modelSha256")).toString(),
                 frozenArtifact.value(QStringLiteral("modelSha256")).toString());

        auto *actions = Application::instance()->getActionList();
        actions->processAvailability();
        QVERIFY(actions->getAction(Action::ACTION_GEOMETRY_UNDO)->isEnabled());
        actions->getAction(Action::ACTION_GEOMETRY_UNDO)->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
        const QJsonObject undoContext = session->documentProjectContext(0);
        const QString undoSourceRevision = undoContext.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(undoSourceRevision, QStringLiteral("r2"));
        QCOMPARE(undoContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);
        QCOMPARE(documentRow(persistentId).value(QStringLiteral("sourceRevision")).toString(),
                 undoSourceRevision);
        QVERIFY(documentRow(persistentId).value(QStringLiteral("dirty")).toBool());
        const QPair<QString, quint64> undoStamp = session->modelCommandStamp(0);
        QVERIFY(undoStamp.second > configuredStamp.second);
        QCOMPARE(queryHostDocument(session, 0).value(QStringLiteral("documentRevision")).toDouble(), double(undoStamp.second));
        const QJsonObject rowAfterUndo = documentRow(persistentId);
        bool frozenPointerArchived = false;
        for (const QJsonValue &value : rowAfterUndo.value(QStringLiteral("orphanSnapshots")).toArray())
        {
            const QJsonObject orphan = value.toObject();
            frozenPointerArchived = frozenPointerArchived
                                    || (orphan.value(QStringLiteral("sourceRevision")).toString()
                                            == frozenArtifact.value(QStringLiteral("sourceRevision")).toString()
                                        && orphan.value(QStringLiteral("relativePath")).toString()
                                            == frozenRelativePath
                                        && orphan.value(QStringLiteral("modelSha256")).toString()
                                            == frozenArtifact.value(QStringLiteral("modelSha256")).toString());
        }
        QVERIFY(frozenPointerArchived);

        Model stillFrozenAfterUndo;
        ModelIO::open(stillFrozenAfterUndo, frozenPath);
        QCOMPARE(stillFrozenAfterUndo.getProblemSetup().getModalSetup().getNIterations(), 24u);
        actions->processAvailability();
        QVERIFY(actions->getAction(Action::ACTION_GEOMETRY_REDO)->isEnabled());
        actions->getAction(Action::ACTION_GEOMETRY_REDO)->trigger();
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 24u);
        const QJsonObject redoContext = session->documentProjectContext(0);
        const QString redoSourceRevision = redoContext.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(redoSourceRevision, QStringLiteral("r3"));
        QCOMPARE(redoContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);
        QCOMPARE(documentRow(persistentId).value(QStringLiteral("sourceRevision")).toString(),
                 redoSourceRevision);
        QVERIFY(documentRow(persistentId).value(QStringLiteral("dirty")).toBool());
        const QPair<QString, quint64> redoStamp = session->modelCommandStamp(0);
        QVERIFY(redoStamp.second > undoStamp.second);
        QCOMPARE(queryHostDocument(session, 0).value(QStringLiteral("documentRevision")).toDouble(), double(redoStamp.second));

        const QString savePath = nativeModelPath(outputs.path(), QStringLiteral("accepted-undo-redo"));
        QVERIFY(saveModelJob(session, 0, savePath));
        const QJsonObject savedContext = session->documentProjectContext(0);
        QCOMPARE(savedContext.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(savedContext.value(QStringLiteral("sourceRevision")).toString(), redoSourceRevision);
        QCOMPARE(savedContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);
        QCOMPARE(documentRow(persistentId).value(QStringLiteral("sourceRevision")).toString(),
                 redoSourceRevision);
        QVERIFY(!documentRow(persistentId).value(QStringLiteral("dirty")).toBool());
        const auto savedBinding = RProjectCatalog::resolveSourceBinding(savePath);
        QVERIFY(savedBinding.has_value());
        QCOMPARE(savedContext.value(QStringLiteral("savedArtifactHash")).toString(),
                 savedBinding->savedArtifactHash);
        const QPair<QString, quint64> savedStamp = session->modelCommandStamp(0);
        QVERIFY(savedStamp.second > redoStamp.second);
        QCOMPARE(queryHostDocument(session, 0).value(QStringLiteral("documentRevision")).toDouble(), double(savedStamp.second));
        Model rereadSaved;
        ModelIO::open(rereadSaved, savePath);
        QCOMPARE(rereadSaved.getProblemSetup().getModalSetup().getNIterations(), 24u);
        Model rereadFrozen;
        ModelIO::open(rereadFrozen, frozenPath);
        QCOMPARE(rereadFrozen.getProblemSetup().getModalSetup().getNIterations(), 24u);
        const QJsonObject finalRow = documentRow(persistentId);
        QCOMPARE(finalRow.value(QStringLiteral("snapshotSourceRevision")).toString(), QString());
        QVERIFY(QFileInfo::exists(QDir(session->projectWorkspace()).filePath(frozenRelativePath)));

        session->addModel(modalFixture(repeatedName));
        session->addModel(modalFixture(repeatedName));
        const QJsonObject second = session->documentProjectContext(1);
        const QJsonObject third = session->documentProjectContext(2);
        const QString secondPersistentId = second.value(QStringLiteral("persistentDocumentId")).toString();
        const QString thirdPersistentId = third.value(QStringLiteral("persistentDocumentId")).toString();
        const QString secondRuntimeId = session->modelCommandStamp(1).first;
        const QString thirdRuntimeId = session->modelCommandStamp(2).first;
        const QString secondBaseRevision = second.value(QStringLiteral("sourceRevision")).toString();
        const QString thirdBaseRevision = third.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(second.value(QStringLiteral("label")).toString(), repeatedName);
        QCOMPARE(third.value(QStringLiteral("label")).toString(), repeatedName);
        QVERIFY(secondPersistentId != persistentId && thirdPersistentId != persistentId);
        QVERIFY(secondPersistentId != thirdPersistentId);
        QVERIFY(secondRuntimeId != thirdRuntimeId);
        QCOMPARE(secondBaseRevision, QStringLiteral("r0"));
        QCOMPARE(thirdBaseRevision, QStringLiteral("r0"));

        session->removeModel(0);
        QCOMPARE(session->modelCommandStamp(0).first, secondRuntimeId);
        QCOMPARE(session->modelCommandStamp(1).first, thirdRuntimeId);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 secondPersistentId);
        QCOMPARE(session->documentProjectContext(1).value(QStringLiteral("persistentDocumentId")).toString(),
                 thirdPersistentId);
        RModalSetup reorderedEdit = session->getModel(0).getProblemSetup().getModalSetup();
        reorderedEdit.setNIterations(37);
        QVERIFY2(host->configureModal({0}, reorderedEdit, &error), qPrintable(error));
        const QString reorderedSecondRevision = session->documentProjectContext(0)
                                                    .value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(reorderedSecondRevision, QStringLiteral("r1"));
        QCOMPARE(session->documentProjectContext(1).value(QStringLiteral("sourceRevision")).toString(),
                 thirdBaseRevision);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 secondPersistentId);
        QCOMPARE(session->documentProjectContext(1).value(QStringLiteral("persistentDocumentId")).toString(),
                 thirdPersistentId);
    }

    void wrongThreadSnapshotAndQueuedMutationCloseKeepFreezeIdentity()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString repeatedName = QStringLiteral("Queued freeze document");
        const QString firstSourcePath = nativeModelPath(sources.path(), QStringLiteral("queued-first"));
        const QString secondSourcePath = nativeModelPath(sources.path(), QStringLiteral("queued-second"));
        QVERIFY(createSourceModel(firstSourcePath));
        QVERIFY(createSourceModel(secondSourcePath));
        Model firstSource;
        ModelIO::open(firstSource, firstSourcePath);
        firstSource.setName(repeatedName);
        Model secondSource;
        ModelIO::open(secondSource, secondSourcePath);
        secondSource.setName(repeatedName);
        session->addModel(firstSource);
        session->addModel(secondSource);
        const QString originalRuntimeId = session->modelCommandStamp(0).first;
        const QJsonObject originalContext = session->documentProjectContext(0);
        const QString originalPersistentId = originalContext
                                                 .value(QStringLiteral("persistentDocumentId")).toString();
        const QString originalBaseSourceRevision = originalContext
                                                       .value(QStringLiteral("sourceRevision")).toString();
        const QString otherRuntimeId = session->modelCommandStamp(1).first;
        const QJsonObject otherContext = session->documentProjectContext(1);
        const QString otherPersistentId = otherContext.value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(originalRuntimeId != otherRuntimeId);
        QVERIFY(originalPersistentId != otherPersistentId);
        QCOMPARE(originalBaseSourceRevision, QStringLiteral("r0"));
        QCOMPARE(originalContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);
        QCOMPARE(otherContext.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
        QCOMPARE(otherContext.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), false);

        QJsonObject wrongThreadArtifact;
        QString wrongThreadError;
        std::thread snapshotWorker([&] {
            wrongThreadArtifact = session->createDocumentSnapshot(0, &wrongThreadError);
        });
        snapshotWorker.join();
        QVERIFY(wrongThreadArtifact.isEmpty());
        QVERIFY(wrongThreadError.contains(QStringLiteral("document thread"), Qt::CaseInsensitive));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);

        bool queued = false;
        std::thread queueWorker([&] {
            queued = QMetaObject::invokeMethod(session, [session, originalRuntimeId] {
                const uint id = session->findModelByCommandIdentity(originalRuntimeId);
                if (id >= session->getNModels()) return;
                session->getModel(id).getProblemSetup().getModalSetup().setNIterations(36);
                session->setProblemChanged(id);
                session->documentProjectContext(id); // Flush the accepted edit before the queued close.
                const uint currentId = session->findModelByCommandIdentity(originalRuntimeId);
                if (currentId < session->getNModels()) session->removeModel(currentId);
            }, Qt::QueuedConnection);
        });
        queueWorker.join();
        QVERIFY(queued);

        // Snapshot capture is synchronous and has no independent cancellation
        // API. Freeze the current model before the queued edit and close run.
        QString error;
        const QJsonObject frozen = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!frozen.isEmpty(), qPrintable(error));
        QCOMPARE(frozen.value(QStringLiteral("persistentDocumentId")).toString(), originalPersistentId);
        QCOMPARE(frozen.value(QStringLiteral("sourceRevision")).toString(), originalBaseSourceRevision);
        QCOMPARE(frozen.value(QStringLiteral("instanceRef")).toObject()
                     .value(QStringLiteral("documentId")).toString(), originalRuntimeId);
        const QString frozenPath = QDir(session->projectWorkspace()).filePath(
            frozen.value(QStringLiteral("relativePath")).toString());
        Model frozenModel;
        ModelIO::open(frozenModel, frozenPath);
        QCOMPARE(frozenModel.getProblemSetup().getModalSetup().getNIterations(), 12u);

        QTRY_COMPARE_WITH_TIMEOUT(session->getNModels(), 1u, 5000);
        QVERIFY(session->modelCommandStamp(0).first != originalRuntimeId);
        QCOMPARE(session->modelCommandStamp(0).first, otherRuntimeId);
        QCOMPARE(session->documentProjectContext(0).value(QStringLiteral("persistentDocumentId")).toString(),
                 otherPersistentId);
        const QJsonObject catalog = session->projectContext();
        QJsonObject closedDocument;
        for (const QJsonValue &value : catalog.value(QStringLiteral("documents")).toArray())
            if (value.toObject().value(QStringLiteral("persistentDocumentId")).toString()
                == originalPersistentId)
                closedDocument = value.toObject();
        QVERIFY(!closedDocument.isEmpty());
        const QString closedSourceRevision = closedDocument.value(QStringLiteral("sourceRevision")).toString();
        QCOMPARE(closedSourceRevision, QStringLiteral("r1"));
        QVERIFY(closedDocument.value(QStringLiteral("dirty")).toBool());
        bool frozenPointerArchived = false;
        for (const QJsonValue &value : closedDocument.value(QStringLiteral("orphanSnapshots")).toArray())
        {
            const QJsonObject orphan = value.toObject();
            frozenPointerArchived = frozenPointerArchived
                                    || (orphan.value(QStringLiteral("sourceRevision")).toString()
                                            == frozen.value(QStringLiteral("sourceRevision")).toString()
                                        && orphan.value(QStringLiteral("relativePath")).toString()
                                            == frozen.value(QStringLiteral("relativePath")).toString()
                                        && orphan.value(QStringLiteral("modelSha256")).toString()
                                            == frozen.value(QStringLiteral("modelSha256")).toString());
        }
        QVERIFY(frozenPointerArchived);
        Model frozenAfterClose;
        ModelIO::open(frozenAfterClose, frozenPath);
        QCOMPARE(frozenAfterClose.getProblemSetup().getModalSetup().getNIterations(), 12u);

        const QJsonObject remainingSnapshot = session->createDocumentSnapshot(0, &error);
        QVERIFY2(!remainingSnapshot.isEmpty(), qPrintable(error));
        QCOMPARE(remainingSnapshot.value(QStringLiteral("persistentDocumentId")).toString(), otherPersistentId);
        QCOMPARE(remainingSnapshot.value(QStringLiteral("instanceRef")).toObject()
                     .value(QStringLiteral("documentId")).toString(), otherRuntimeId);
        session->removeModel(0);
        QVERIFY(session->createDocumentSnapshot(0, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void nativeSurfaceOrientationNormalizationIsTracked()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        Model source;
        source.setName(QStringLiteral("Opposite surface orientations"));
        source.setNNodes(4);
        source.setNode(0, RNode(0, 0, 0));
        source.setNode(1, RNode(1, 0, 0));
        source.setNode(2, RNode(0, 1, 0));
        source.setNode(3, RNode(1, 1, 0));
        source.setNElements(2);
        for (uint index = 0; index < 2; ++index)
        {
            RElement triangle(R_ELEMENT_TRI1);
            for (uint node = 0; node < 3; ++node) triangle.setNodeId(node, index + node);
            source.setElement(index, triangle);
        }
        source.setNSurfaces(1);
        source.getSurface(0).add(0);
        source.getSurface(0).add(1);
        source.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        QVERIFY(!source.getElement(0).isNeighborNormalSync(source.getElement(1)));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("surface-orientations"));
        ModelIO::save(source, sourcePath, true);
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        QVERIFY(loaded.getElement(0).isNeighborNormalSync(loaded.getElement(1)));
        QVERIFY(loaded.hasNativeReadNormalization());
        session->addModel(loaded);
        const auto normalized = session->documentProjectContext(0);
        QCOMPARE(normalized.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QVERIFY(normalized.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool());
        QVERIFY(saveModelJob(session, 0, sourcePath));
        Model saved;
        ModelIO::open(saved, sourcePath);
        QVERIFY(!saved.hasNativeReadNormalization());
        QVERIFY(saved.getElement(0).isNeighborNormalSync(saved.getElement(1)));
    }

    void nativeColorDefaultsAreTrackedUntilSaved()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        Model source = modalFixture();
        RVariable displacement(R_VARIABLE_DISPLACEMENT, R_VARIABLE_APPLY_NODE);
        displacement.resize(3, source.getNNodes());
        displacement.getVariableData().setValueRangeName(QString());
        source.addVariable(displacement);
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("missing-color-default"));
        ModelIO::save(source, sourcePath, true);

        Model loaded;
        ModelIO::open(loaded, sourcePath);
        QVERIFY(loaded.hasNativeReadNormalization());
        QVERIFY(!loaded.getVariable(0).getVariableData().getValueRangeName().isEmpty());
        session->addModel(loaded);
        const auto normalized = session->documentProjectContext(0);
        QCOMPARE(normalized.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QVERIFY(normalized.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool());
        QVERIFY(saveModelJob(session, 0, sourcePath));
        QVERIFY(!session->getModel(0).hasNativeReadNormalization());
        QVERIFY(!session->documentProjectContext(0).value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool());
        Model reloaded;
        ModelIO::open(reloaded, sourcePath);
        QVERIFY(!reloaded.hasNativeReadNormalization());
        QCOMPARE(reloaded.getVariable(0).getVariableData().getValueRangeName(),
                 session->getModel(0).getVariable(0).getVariableData().getValueRangeName());
    }

    void reloadRetainedTimeInputsRemainDirtyUntilSaved()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));
        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("retained-time-inputs"));
        Model source = modalFixture();
        source.getTimeSolver().setInputNTimeSteps(10);
        source.getTimeSolver().setInputStartTime(0.0);
        source.getTimeSolver().setInputTimeStepSize(0.01);
        source.getTimeSolver().setTimes({0.0, 0.01, 0.02});
        ModelIO::save(source, sourcePath, true);
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);
        QVERIFY(!session->documentProjectContext(0).value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool());

        Model incoming = modalFixture();
        incoming.getTimeSolver().setInputNTimeSteps(20);
        incoming.getTimeSolver().setInputStartTime(1.0);
        incoming.getTimeSolver().setInputTimeStepSize(0.02);
        incoming.getTimeSolver().setTimes({1.0, 1.02});
        ModelIO::save(incoming, sourcePath, true);
        const auto writtenSource = RProjectCatalog::resolveSourceBinding(sourcePath);
        QVERIFY(writtenSource.has_value());
        QVERIFY(waitForJob(new ModelIO(MODEL_IO_UPDATE, sourcePath, session->getModelPtr(0))));
        const auto retained = session->documentProjectContext(0);
        QCOMPARE(retained.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QVERIFY(retained.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool());
        QCOMPARE(retained.value(QStringLiteral("savedArtifactHash")).toString(), writtenSource->savedArtifactHash);
        const RTimeSolver &acceptedTime = session->getModel(0).getTimeSolver();
        QCOMPARE(acceptedTime.getInputNTimeSteps(), 10u);
        QCOMPARE(acceptedTime.getInputStartTime(), 0.0);
        QCOMPARE(acceptedTime.getInputTimeStepSize(), 0.01);
        QVERIFY(acceptedTime.getTimes() == std::vector<double>({0.0, 0.01, 0.02}));
        QVERIFY(session->getModel(0).hasNativeReadNormalization());
        QVERIFY(saveModelJob(session, 0, sourcePath));
        QVERIFY(!session->getModel(0).hasNativeReadNormalization());
        QVERIFY(!session->documentProjectContext(0).value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool());
        RModel saved;
        saved.read(sourcePath);
        QCOMPARE(saved.getTimeSolver().getInputTimeStepSize(), 0.01);
        QCOMPARE(saved.getTimeSolver().getInputNTimeSteps(), 10u);
    }

    void nativeMeshDefaultsAreRecordedWhenLoadedSourceWasNormalized()
    {
        QTemporaryDir workspace;
        QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        QVERIFY(openProject(session, workspace.path()));

        const QString sourcePath = nativeModelPath(sources.path(), QStringLiteral("native-mesh-defaults"));
        Model source;
        source.setName(QStringLiteral("Native mesh defaults"));
        source.setNNodes(4);
        source.setNode(0, RNode(0.0, 0.0, 0.0));
        source.setNode(1, RNode(1.0, 0.0, 0.0));
        source.setNode(2, RNode(0.0, 1.0, 0.0));
        source.setNode(3, RNode(0.0, 0.0, 1.0));
        source.setNElements(1);
        RElement tetra(R_ELEMENT_TETRA1);
        tetra.setNodeId(0, 0);
        tetra.setNodeId(1, 1);
        tetra.setNodeId(2, 2);
        tetra.setNodeId(3, 3);
        source.setElement(0, tetra);
        source.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        source.getProblemSetup().getMeshSetup().setMinEdgeLength(0.0);
        source.getProblemSetup().getMeshSetup().setMaxEdgeLength(0.0);
        ModelIO::save(source, sourcePath, true);

        RModel savedNative;
        savedNative.read(sourcePath);
        QCOMPARE(savedNative.getProblemSetup().getMeshSetup().getMinEdgeLength(), 0.0);
        QCOMPARE(savedNative.getProblemSetup().getMeshSetup().getMaxEdgeLength(), 0.0);
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        QVERIFY(loaded.getProblemSetup().getMeshSetup().getMinEdgeLength() > 0.0);
        QVERIFY(loaded.hasNativeReadNormalization());
        QJsonObject observedDuringAdd;
        const auto observation = QObject::connect(session, &Session::modelAdded, session,
            [&](uint id) { observedDuringAdd = session->documentProjectContext(id); });
        session->addModel(loaded);
        QObject::disconnect(observation);

        QCOMPARE(observedDuringAdd.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QVERIFY(observedDuringAdd.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool());

        const QJsonObject normalized = session->documentProjectContext(0);
        const QString persistentId = normalized.value(QStringLiteral("persistentDocumentId")).toString();
        QVERIFY(!persistentId.isEmpty());
        QCOMPARE(normalized.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QCOMPARE(normalized.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);
        QCOMPARE(normalized.value(QStringLiteral("sourcePath")).toString(), QFileInfo(sourcePath).canonicalFilePath());
        const double minimumAfterAdd = session->getModel(0).getProblemSetup().getMeshSetup().getMinEdgeLength();
        const double maximumAfterAdd = session->getModel(0).getProblemSetup().getMeshSetup().getMaxEdgeLength();
        QVERIFY(minimumAfterAdd > 0.0);
        QVERIFY(maximumAfterAdd > minimumAfterAdd);
        const auto writtenSource = RProjectCatalog::resolveSourceBinding(sourcePath);
        QVERIFY(writtenSource.has_value());
        QCOMPARE(normalized.value(QStringLiteral("savedArtifactHash")).toString(),
                 writtenSource->savedArtifactHash);

        const QPair<QString, quint64> acceptedStamp = session->modelCommandStamp(0);
        session->getModel(0).createDependentEntities();
        session->setModelSelectionChanged(0);
        const QJsonObject refreshed = session->documentProjectContext(0);
        QCOMPARE(session->modelCommandStamp(0).second, acceptedStamp.second);
        QCOMPARE(refreshed.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
        QCOMPARE(refreshed.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
        QCOMPARE(refreshed.value(QStringLiteral("savedArtifactHash")).toString(),
                 writtenSource->savedArtifactHash);
        QCOMPARE(refreshed.value(QStringLiteral("unsaved")).toObject()
                     .value(QStringLiteral("dirty")).toBool(), true);

        RModel readFromDisk;
        readFromDisk.read(sourcePath);
        QCOMPARE(readFromDisk.getProblemSetup().getMeshSetup().getMinEdgeLength(), 0.0);
        QCOMPARE(readFromDisk.getProblemSetup().getMeshSetup().getMaxEdgeLength(), 0.0);
    }
};

int runProjectSessionTests(int argc, char **argv)
{
    if (qEnvironmentVariableIsSet("RANGE_PROJECT_GUI_EVIDENCE_DIRECTORY"))
    {
        auto *window = Application::instance()->getMainWindow();
        window->resize(1440, 900);
        window->show();
        QTest::qWait(250);
    }
    TestProjectSession test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_project_session.moc"
