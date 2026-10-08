#include <QtTest>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSemaphore>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>

#include <rbl_job.h>
#include <rmodel_command_service.h>
#include <rbl_error.h>

#include "application.h"
#include "model_command_host.h"
#include "model_io.h"
#include "problem_tree.h"
#include "model_tree.h"
#include "session.h"
#include "solver_manager.h"
#include "solver_task.h"
#include "value_line_edit.h"

namespace
{
Model modalFixture(const QString &name = QStringLiteral("Modal host fixture"))
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
    return model;
}

Model stressLineFixture(const QString &name = QStringLiteral("Stress host fixture"))
{
    Model model;
    model.setName(name);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.setNNodes(2);
    model.getNode(0).set(0.0, 0.0, 0.0);
    model.getNode(1).set(1.0, 0.0, 0.0);
    model.setNLines(1);
    model.getLine(0).setName(QStringLiteral("Truss"));
    RElement truss(R_ELEMENT_TRUSS1);
    truss.setNodeId(0, 0);
    truss.setNodeId(1, 1);
    model.addElement(truss, true, 0);
    return model;
}

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), operation},
            {QStringLiteral("parameters"), parameters}};
}

QJsonObject lastJournalRecord(const QString &path, const QString &commandId,
                              const QString &resultStatus = QStringLiteral("accepted"))
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return {};
    const QJsonArray records = document.object().value(QStringLiteral("records")).toArray();
    for (qsizetype i = records.size(); i > 0; --i)
    {
        const QJsonObject record = records.at(i - 1).toObject();
        if (record.value(QStringLiteral("commandId")).toString() == commandId
            && record.value(QStringLiteral("resultStatus")).toString() == resultStatus)
            return record;
    }
    return {};
}

QJsonObject targetFor(const Session *session, const QString &documentId)
{
    return {{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), session->getModelCommandHost()->sessionEpoch()}};
}

ValueLineEdit *currentNamedEditor(ProblemTree *tree, const QString &name)
{
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
        if (auto *widget = tree->itemWidget(tree->topLevelItem(index), 0))
            if (auto *editor = widget->findChild<ValueLineEdit *>(name)) return editor;
    return nullptr;
}

QJsonObject commandPreview(Session *session, const QString &documentId,
                           const QString &command, const QJsonObject &arguments)
{
    const uint modelID = session->findModelByCommandIdentity(documentId);
    if (modelID == RConstants::eod) return {};
    const auto stamp = session->modelCommandStamp(modelID);
    return session->getModelCommandHost()->handle(hostRequest(QStringLiteral("command.preview"),
        {{QStringLiteral("target"), targetFor(session, documentId)},
         {QStringLiteral("expectedDocumentRevision"), double(stamp.second)},
         {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
         {QStringLiteral("arguments"), arguments}}));
}

QJsonObject commandExecute(Session *session, const QString &previewId,
                           const QString &idempotencyKey,
                           const QString &operation = QStringLiteral("command.execute"))
{
    return session->getModelCommandHost()->handle(hostRequest(operation,
        {{QStringLiteral("previewId"), previewId},
         {QStringLiteral("idempotencyKey"), idempotencyKey}}));
}

bool loadTinyTruss(QTemporaryDir &directory, const QString &stem, Model *model)
{
#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_WORKFLOW_SOURCE_ROOT "."
#endif
    const QString fixture = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT))
        .filePath(QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram"));
    const QString target = directory.filePath(stem + QStringLiteral(".ram"));
    if (!QFile::copy(fixture, target)) return false;
    try
    {
        ModelIO::open(*model, target);
    }
    catch (const RError &)
    {
        return false;
    }
    // The fixture's controller binding is for another workflow benchmark.
    model->getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    model->setName(stem);
    if (model->getNLines() == 0) return false;
    for (uint index = 0; index < model->getNPoints(); ++index)
        model->getPoint(index).setMaterial(model->getLine(0).getMaterial());
    ModelIO::save(*model, target, false);
    return model->getNNodes() > 0 && model->getNElements() > 0;
}

class SessionModelGuard
{
public:
    explicit SessionModelGuard(Session *session) : session(session) {}
    ~SessionModelGuard()
    {
        for (const QString &identity : identities)
        {
            const uint modelID = session->findModelByCommandIdentity(identity);
            if (modelID != RConstants::eod) session->removeModel(modelID);
        }
    }

    QString add(const Model &model)
    {
        session->addModel(model);
        const QString identity = session->modelCommandStamp(session->getNModels() - 1).first;
        identities.append(identity);
        return identity;
    }

    void track(const QString &identity)
    {
        if (!identities.contains(identity)) identities.append(identity);
    }

private:
    Session *session;
    QStringList identities;
};

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
}

class TestGuiModelOperations : public QObject
{
    Q_OBJECT

private:
    QString nativeEvidenceDirectory;

    bool captureNativeWindowEvidence(const QString &stem, const QString &documentId)
    {
        if (nativeEvidenceDirectory.isEmpty()) return true;
        auto *app = Application::instance();
        auto *window = app->getMainWindow();
        if (!window) return false;
        Session *session = app->getSession();
        const uint selectedID = session->findModelByCommandIdentity(documentId);
        if (selectedID != RConstants::eod)
        {
            for (uint i = 0; i < session->getNModels(); ++i)
                session->setModelSelected(i, i == selectedID);
        }
        auto *solverDock = window->findChild<QDockWidget *>(QStringLiteral("solverDock"));
        if (!solverDock) return false;
        solverDock->show();
        solverDock->raise();
        if (auto *tabs = solverDock->findChild<QTabWidget *>(); tabs && tabs->count() > 0)
            tabs->setCurrentIndex(0); // Problem
        window->show();
        window->raise();
        window->activateWindow();
        QCoreApplication::processEvents();
        if (!window->isVisible() || !solverDock->isVisible()) return false;
        if (!QDir().mkpath(nativeEvidenceDirectory)) return false;
        return window->grab().save(QDir(nativeEvidenceDirectory).filePath(stem + QStringLiteral(".png")))
            && solverDock->grab().save(QDir(nativeEvidenceDirectory).filePath(stem + QStringLiteral(".problem-dock.png")));
    }

private slots:
    void initTestCase()
    {
        nativeEvidenceDirectory = qEnvironmentVariable("RANGE_NATIVE_GUI_EVIDENCE_DIRECTORY");
        if (nativeEvidenceDirectory.isEmpty()) return;
#if defined(Q_OS_WIN)
        QVERIFY2(QApplication::platformName() != QStringLiteral("offscreen"),
                 "Native GUI evidence mode requires a visible Windows Qt platform.");
        auto *window = Application::instance()->getMainWindow();
        QVERIFY(window);
        window->resize(1440, 900);
        window->show();
        window->raise();
        window->activateWindow();
        QCoreApplication::processEvents();
        QVERIFY(window->winId() != 0);
        QTRY_VERIFY_WITH_TIMEOUT(window->isVisible(), 10000);
        QVERIFY2(QTest::qWaitForWindowExposed(window, 10000), "The real Range FEA main window was not exposed.");
        auto *solverDock = window->findChild<QDockWidget *>(QStringLiteral("solverDock"));
        QVERIFY(solverDock);
        solverDock->show();
        solverDock->raise();
        auto *tabs = solverDock->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QVERIFY(tabs->count() > 0);
        tabs->setCurrentIndex(0); // Problem
        QCoreApplication::processEvents();
        QVERIFY(QDir().mkpath(nativeEvidenceDirectory));
#else
        QFAIL("RANGE_NATIVE_GUI_EVIDENCE_DIRECTORY is supported only on Windows.");
#endif
    }

    void nativeWindowModalControlPreservesIdentityAndBlocksPendingDraft()
    {
        if (nativeEvidenceDirectory.isEmpty()) QSKIP("Opt-in real Windows control acceptance.");
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(modalFixture(QStringLiteral("Native GUI control fixture")));
        const QString otherIdentity = models.add(modalFixture(QStringLiteral("Unrelated background document")));
        const uint modelID = session->findModelByCommandIdentity(identity);
        session->setModelSelected(session->findModelByCommandIdentity(otherIdentity), false);
        session->setModelSelected(modelID, true);
        QCoreApplication::processEvents();
        auto *window = Application::instance()->getMainWindow();
        auto *modelTree = window->findChild<ModelTree *>();
        QVERIFY(modelTree);
        QVERIFY(modelTree->topLevelItemCount() > int(modelID));
        QTest::mouseClick(modelTree->viewport(), Qt::LeftButton, Qt::NoModifier,
            modelTree->visualItemRect(modelTree->topLevelItem(int(modelID))).center());
        QCoreApplication::processEvents();
        QCOMPARE(session->getSelectedModelIDs(), QList<uint>{modelID});
        auto *tree = window->findChild<ProblemTree *>();
        QVERIFY(tree);
        auto *tolerance = currentNamedEditor(tree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        QVERIFY(tolerance->isVisible());
        QTest::mouseClick(tolerance, Qt::LeftButton);
        QVERIFY(tolerance->hasFocus());

        const QJsonObject before = host->query(modelID);
        const quint64 revision = session->modelCommandStamp(modelID).second;
        QTest::keyClick(tolerance, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(tolerance, Qt::Key_Backspace);
        QVERIFY(tolerance->text().isEmpty());
        QVERIFY(session->hasDocumentDraft(modelID));
        QCOMPARE(session->modelCommandStamp(modelID).second, revision);
        const auto rejectedDraft = commandPreview(session, identity, QStringLiteral("analysis.modal.configure"),
            {{QStringLiteral("method"), QStringLiteral("multiple")},
             {QStringLiteral("iterations"), 20}, {QStringLiteral("modes"), 3},
             {QStringLiteral("tolerance"), 1e-6}, {QStringLiteral("unit"), QStringLiteral("1")}});
        QVERIFY2(rejectedDraft.value(QStringLiteral("status")).toString() == QStringLiteral("rejected"),
                 qPrintable(QJsonDocument(rejectedDraft).toJson(QJsonDocument::Compact)));
        QCOMPARE(rejectedDraft.value(QStringLiteral("code")).toString(), QStringLiteral("DRAFT_CONFLICT"));
        QVERIFY(tolerance->hasFocus());
        const auto readonly = host->handle(hostRequest(QStringLiteral("document.inputs.query"),
            {{QStringLiteral("target"), targetFor(session, identity)}}));
        QCOMPARE(readonly.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(tolerance->hasFocus());
        QVERIFY(tolerance->text().isEmpty());
        const uint otherID = session->findModelByCommandIdentity(otherIdentity);
        session->getModel(otherID).setDescription(QStringLiteral("Background accepted change"));
        session->setModelChanged(otherID);
        QCoreApplication::processEvents();
        tolerance = currentNamedEditor(tree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        QStringList selectedIds;
        for (uint selectedID : session->getSelectedModelIDs())
            selectedIds.append(session->modelCommandStamp(selectedID).first);
        QWidget *actualFocus = QApplication::focusWidget();
        if (!tolerance->hasFocus())
            window->grab().save(QDir(nativeEvidenceDirectory).filePath(QStringLiteral("focus-diagnostic.png")));
        QVERIFY2(tolerance->hasFocus(), qPrintable(QStringLiteral("focus=%1/%2 selected=%3 expected=%4 visible=%5 text=%6")
            .arg(actualFocus ? QString::fromLatin1(actualFocus->metaObject()->className()) : QStringLiteral("null"),
                 actualFocus ? actualFocus->objectName() : QStringLiteral("null"), selectedIds.join(QLatin1Char(',')), identity)
            .arg(tolerance->isVisible()).arg(tolerance->text())));
        QVERIFY(tolerance->text().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(tree, "populate", Qt::DirectConnection));
        ValueLineEdit *rebuiltTolerance = currentNamedEditor(tree, QStringLiteral("modalTolerance"));
        QVERIFY(rebuiltTolerance);
        QCoreApplication::processEvents();
        QVERIFY(rebuiltTolerance->hasFocus());
        QVERIFY(rebuiltTolerance->text().isEmpty());
        QCOMPARE(session->modelCommandStamp(modelID).second, revision);
        tolerance = rebuiltTolerance;

        tolerance->setText(QStringLiteral("1e-"));
        tolerance->setSelection(3, -2);
        QVERIFY(QMetaObject::invokeMethod(tree, "populate", Qt::DirectConnection));
        tolerance = currentNamedEditor(tree, QStringLiteral("modalTolerance"));
        QVERIFY(tolerance);
        QCoreApplication::processEvents();
        QVERIFY(tolerance->hasFocus());
        QCOMPARE(tolerance->text(), QStringLiteral("1e-"));
        QCOMPARE(tolerance->selectedText(), QStringLiteral("e-"));
        QCOMPARE(tolerance->cursorPosition(), 1);
        QCOMPARE(session->modelCommandStamp(modelID).second, revision);

        tolerance->setText(QStringLiteral("0.000001"));
        QVERIFY(!session->hasDocumentDraft(modelID));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity))
                     .getProblemSetup().getModalSetup().getConvergenceValue(), 1e-6);
        const auto after = host->query(session->findModelByCommandIdentity(identity));
        QCOMPARE(after.value(QStringLiteral("modelId")), before.value(QStringLiteral("modelId")));
        QVERIFY(after.value(QStringLiteral("revision")).toString() != before.value(QStringLiteral("revision")).toString());
        QVERIFY2(captureNativeWindowEvidence(QStringLiteral("native-modal-control-draft-cleared"), identity),
                 "Could not capture the accepted real-control edit in the visible Windows GUI.");
    }

    void hostQueriesExposeVersionedInputsAndPhysicalTargetCandidates()
    {
        Session *session = Application::instance()->getSession();
        SessionModelGuard models(session);
        const QString identity = models.add(stressLineFixture());
        const uint modelID = session->findModelByCommandIdentity(identity);
        const uint physicalId = session->getModel(modelID).getLine(0).getPhysicalId();
        const auto target = targetFor(session, identity);

        const auto targetsResponse = session->getModelCommandHost()->handle(hostRequest(
            QStringLiteral("document.targets.query"),
            {{QStringLiteral("target"), target},
             {QStringLiteral("page"), QJsonObject{{QStringLiteral("limit"), 1},
                                                   {QStringLiteral("labelQuery"), QStringLiteral("Truss")}}}}));
        QCOMPARE(targetsResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject targets = targetsResponse.value(QStringLiteral("result")).toObject();
        QCOMPARE(targets.value(QStringLiteral("target")).toObject().value(QStringLiteral("documentId")).toString(), identity);
        QCOMPARE(targets.value(QStringLiteral("runtimeStamp")).toObject().value(QStringLiteral("revision")).toString(),
                 QString::number(session->modelCommandStamp(modelID).second));
        const QJsonArray page = targets.value(QStringLiteral("targets")).toArray();
        QCOMPARE(page.size(), 1);
        QCOMPARE(page.first().toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("g:%1").arg(physicalId));
        QCOMPARE(targets.value(QStringLiteral("candidateIds")).toArray().first().toString(),
                 QStringLiteral("g:%1").arg(physicalId));
        QVERIFY(targets.value(QStringLiteral("persistentProject")).isObject());

        const auto inputsResponse = session->getModelCommandHost()->handle(hostRequest(
            QStringLiteral("document.inputs.query"), {{QStringLiteral("target"), target}}));
        QCOMPARE(inputsResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject inputs = inputsResponse.value(QStringLiteral("result")).toObject();
        QVERIFY(inputs.value(QStringLiteral("analysis")).isObject());
        QVERIFY(inputs.value(QStringLiteral("configuration")).isObject());
        QVERIFY(inputs.value(QStringLiteral("capabilities")).toObject()
                    .value(QStringLiteral("commands")).isArray());
        QCOMPARE(inputs.value(QStringLiteral("targetPage")).toObject().value(QStringLiteral("sourceId")).toString(), identity);

        const auto jobsResponse = session->getModelCommandHost()->handle(hostRequest(
            QStringLiteral("native.jobs.query"), {{QStringLiteral("offset"), 0}, {QStringLiteral("limit"), 20}}));
        QCOMPARE(jobsResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(jobsResponse.value(QStringLiteral("result")).toObject().value(QStringLiteral("jobs")).isArray());
        QCOMPARE(session->getModel(modelID).getName(), QStringLiteral("Stress host fixture"));
    }

    void supportedCoreEditsUseCurrentFieldsAndOneUndoGroupEach()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(stressLineFixture());
        const uint modelID = session->findModelByCommandIdentity(identity);
        const uint physicalId = session->getModel(modelID).getLine(0).getPhysicalId();
        const QJsonObject originalMaterial = session->getModel(modelID).getLine(0).getMaterial().toJson();
        const QString originalName = session->getModel(modelID).getName();
        const uint originalNodes = session->getModel(modelID).getNNodes();

        const QJsonObject materialArguments{{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(physicalId)},
            {QStringLiteral("name"), QStringLiteral("Steel")},
            {QStringLiteral("youngModulusPa"), 2.0e11}, {QStringLiteral("densityKgM3"), 8000.0},
            {QStringLiteral("poissonRatio"), 0.3}, {QStringLiteral("thermalExpansionPerK"), 1.2e-5}};
        const auto materialPreview = commandPreview(session, identity, QStringLiteral("entity.material.set"), materialArguments);
        QCOMPARE(materialPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QJsonObject materialPreviewResult = materialPreview.value(QStringLiteral("result")).toObject();
        QVERIFY(!materialPreviewResult.value(QStringLiteral("candidateFingerprint")).toString().isEmpty());
        QVERIFY(!materialPreviewResult.value(QStringLiteral("readSetHash")).toString().isEmpty());
        QVERIFY(host->approvePreview(materialPreviewResult.value(QStringLiteral("previewId")).toString()));
        const auto materialAccepted = commandExecute(session, materialPreviewResult.value(QStringLiteral("previewId")).toString(),
                                                     QStringLiteral("gui-core-material-1"));
        QCOMPARE(materialAccepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject materialOperation = materialAccepted.value(QStringLiteral("result")).toObject();
        QVERIFY(materialOperation.value(QStringLiteral("auditRecorded")).toBool());
        QVERIFY(materialOperation.value(QStringLiteral("coreJournalRecorded")).toBool());
        QCOMPARE(materialOperation.value(QStringLiteral("auditCommandId")).toString(), QStringLiteral("entity.material.set"));
        QCOMPARE(materialOperation.value(QStringLiteral("preStateHash")).toString(),
                 materialPreviewResult.value(QStringLiteral("inputStateFingerprint")).toString());
        QCOMPARE(materialOperation.value(QStringLiteral("changedPaths")).toArray(),
                 materialPreviewResult.value(QStringLiteral("changedPaths")).toArray());
        const QJsonObject materialAudit = lastJournalRecord(host->journalPath(), QStringLiteral("entity.material.set"));
        QVERIFY(!materialAudit.isEmpty());
        QCOMPARE(materialAudit.value(QStringLiteral("preStateHash")).toString(),
                 materialOperation.value(QStringLiteral("preStateHash")).toString());
        QCOMPARE(materialAudit.value(QStringLiteral("postStateHash")).toString(),
                 materialOperation.value(QStringLiteral("postStateHash")).toString());
        QCOMPARE(materialAudit.value(QStringLiteral("parameters")).toObject(),
                 materialPreviewResult.value(QStringLiteral("normalizedArguments")).toObject());
        QVERIFY2(captureNativeWindowEvidence(QStringLiteral("accepted-core-material"), identity),
                 "Could not capture the accepted supported edit in the visible Windows GUI.");
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity)).getLine(0).getMaterial().getName(),
                 QStringLiteral("Steel"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity)).getName(), originalName);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity)).getNNodes(), originalNodes);

        auto undoMaterial = commandPreview(session, identity, QStringLiteral("document.undo"), {});
        QCOMPARE(undoMaterial.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString undoMaterialId = undoMaterial.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(undoMaterialId));
        QCOMPARE(commandExecute(session, undoMaterialId, QStringLiteral("gui-core-material-undo-1"),
                                QStringLiteral("document.undo")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity)).getLine(0).getMaterial().toJson(), originalMaterial);

        const QJsonObject boundaryArguments{{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(physicalId)},
            {QStringLiteral("kind"), QStringLiteral("displacement")},
            {QStringLiteral("values"), QJsonArray{0.0, QJsonValue(QJsonValue::Null), 0.0}},
            {QStringLiteral("unit"), QStringLiteral("m")}};
        const auto boundaryPreview = commandPreview(session, identity, QStringLiteral("entity.boundary.set"), boundaryArguments);
        QVERIFY2(boundaryPreview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(boundaryPreview).toJson(QJsonDocument::Compact)));
        const QString boundaryPreviewId = boundaryPreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(boundaryPreviewId));
        const auto boundaryAccepted = commandExecute(session, boundaryPreviewId, QStringLiteral("gui-core-boundary-1"));
        QVERIFY2(boundaryAccepted.value(QStringLiteral("status")).toString() == QStringLiteral("accepted"),
                 qPrintable(QJsonDocument(boundaryAccepted).toJson(QJsonDocument::Compact)));
        const QJsonObject boundaryOperation = boundaryAccepted.value(QStringLiteral("result")).toObject();
        QVERIFY(boundaryOperation.value(QStringLiteral("auditRecorded")).toBool());
        QCOMPARE(boundaryOperation.value(QStringLiteral("auditCommandId")).toString(), QStringLiteral("entity.boundary.set"));
        QCOMPARE(boundaryOperation.value(QStringLiteral("changedPaths")).toArray(),
                 boundaryPreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("changedPaths")).toArray());
        const QJsonObject boundaryAudit = lastJournalRecord(host->journalPath(), QStringLiteral("entity.boundary.set"));
        QVERIFY(!boundaryAudit.isEmpty());
        QCOMPARE(boundaryAudit.value(QStringLiteral("selectionSnapshot")).toObject()
                     .value(QStringLiteral("parametersEncoding")).toString(), QStringLiteral("json-text/v1"));
        QCOMPARE(QJsonDocument::fromJson(boundaryAudit.value(QStringLiteral("parameters")).toObject()
                     .value(QStringLiteral("json")).toString().toUtf8()).object(),
                 boundaryPreview.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("normalizedArguments")).toObject());
        QCOMPARE(boundaryAudit.value(QStringLiteral("preStateHash")).toString(),
                 boundaryOperation.value(QStringLiteral("preStateHash")).toString());
        QCOMPARE(boundaryAudit.value(QStringLiteral("postStateHash")).toString(),
                 boundaryOperation.value(QStringLiteral("postStateHash")).toString());
        QVERIFY(session->getModel(session->findModelByCommandIdentity(identity)).getLine(0)
                    .hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
        auto undoBoundary = commandPreview(session, identity, QStringLiteral("document.undo"), {});
        const QString undoBoundaryId = undoBoundary.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(undoBoundaryId));
        QCOMPARE(commandExecute(session, undoBoundaryId, QStringLiteral("gui-core-boundary-undo-1"),
                                QStringLiteral("document.undo")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        QVERIFY(!session->getModel(session->findModelByCommandIdentity(identity)).getLine(0)
                     .hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
        const quint64 revisionBeforeBadUnits = session->modelCommandStamp(
            session->findModelByCommandIdentity(identity)).second;
        QJsonObject badUnits = boundaryArguments;
        badUnits.insert(QStringLiteral("unit"), QStringLiteral("mm"));
        const auto invalidUnits = commandPreview(session, identity, QStringLiteral("entity.boundary.set"), badUnits);
        QCOMPARE(invalidUnits.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(invalidUnits.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_UNITS"));
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identity)).second, revisionBeforeBadUnits);
        QVERIFY(!session->getModel(session->findModelByCommandIdentity(identity)).getLine(0)
                     .hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));

        const QJsonObject dynamicsArguments{{QStringLiteral("enabled"), true},
            {QStringLiteral("method"), QStringLiteral("newmark")},
            {QStringLiteral("timeStep"), 0.01}, {QStringLiteral("unitSI"), true},
            {QStringLiteral("steps"), 4}, {QStringLiteral("outputFrequency"), 2},
            {QStringLiteral("rayleighAlpha"), 0.0}, {QStringLiteral("rayleighBeta"), 0.0},
            {QStringLiteral("rhoInfinity"), 1.0}, {QStringLiteral("newmarkGamma"), 0.5},
            {QStringLiteral("newmarkBeta"), 0.25}, {QStringLiteral("modalDampingRatios"), QJsonArray{}}};
        const auto previousTimes = session->getModel(session->findModelByCommandIdentity(identity)).getTimeSolver().getTimes();
        const bool previousTimeEnabled = session->getModel(session->findModelByCommandIdentity(identity)).getTimeSolver().getEnabled();
        const auto dynamicsPreview = commandPreview(session, identity,
            QStringLiteral("analysis.structural_dynamics.configure"), dynamicsArguments);
        QCOMPARE(dynamicsPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString dynamicsPreviewId = dynamicsPreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(dynamicsPreviewId));
        const auto dynamicsAccepted = commandExecute(session, dynamicsPreviewId, QStringLiteral("gui-core-dynamics-1"));
        QCOMPARE(dynamicsAccepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject dynamicsOperation = dynamicsAccepted.value(QStringLiteral("result")).toObject();
        QVERIFY(dynamicsOperation.value(QStringLiteral("auditRecorded")).toBool());
        QCOMPARE(dynamicsOperation.value(QStringLiteral("auditCommandId")).toString(),
                 QStringLiteral("analysis.structural_dynamics.configure"));
        QCOMPARE(dynamicsOperation.value(QStringLiteral("changedPaths")).toArray(),
                 dynamicsPreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("changedPaths")).toArray());
        const QJsonObject dynamicsAudit = lastJournalRecord(host->journalPath(),
            QStringLiteral("analysis.structural_dynamics.configure"));
        QVERIFY(!dynamicsAudit.isEmpty());
        QCOMPARE(dynamicsAudit.value(QStringLiteral("preStateHash")).toString(),
                 dynamicsOperation.value(QStringLiteral("preStateHash")).toString());
        QCOMPARE(dynamicsAudit.value(QStringLiteral("postStateHash")).toString(),
                 dynamicsOperation.value(QStringLiteral("postStateHash")).toString());
        uint currentID = session->findModelByCommandIdentity(identity);
        QVERIFY(session->getModel(currentID).getProblemSetup().getStructuralDynamicsSetup().getEnabled());
        QCOMPARE(session->getModel(currentID).getTimeSolver().getTimes().size(), qsizetype(4));
        auto undoDynamics = commandPreview(session, identity, QStringLiteral("document.undo"), {});
        const QString undoDynamicsId = undoDynamics.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(undoDynamicsId));
        QCOMPARE(commandExecute(session, undoDynamicsId, QStringLiteral("gui-core-dynamics-undo-1"),
                                QStringLiteral("document.undo")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("accepted"));
        currentID = session->findModelByCommandIdentity(identity);
        QCOMPARE(session->getModel(currentID).getProblemSetup().getStructuralDynamicsSetup().getEnabled(), false);
        QCOMPARE(session->getModel(currentID).getTimeSolver().getEnabled(), previousTimeEnabled);
        QCOMPARE(session->getModel(currentID).getTimeSolver().getTimes(), previousTimes);
    }

    void stalePreviewAndSessionReorderingNeverReplaceUnrelatedModelState()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString firstIdentity = models.add(modalFixture(QStringLiteral("Unrelated first")));
        const QString targetIdentity = models.add(modalFixture(QStringLiteral("Target remains named")));
        const uint targetID = session->findModelByCommandIdentity(targetIdentity);
        const auto preview = commandPreview(session, targetIdentity, QStringLiteral("analysis.modal.configure"),
            {{QStringLiteral("method"), QStringLiteral("multiple")}, {QStringLiteral("iterations"), 31},
             {QStringLiteral("modes"), 3}, {QStringLiteral("tolerance"), 1e-7}, {QStringLiteral("unit"), QStringLiteral("1")}});
        QCOMPARE(preview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString previewId = preview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QCOMPARE(preview.value(QStringLiteral("result")).toObject().value(QStringLiteral("changedPaths")).toArray().first().toString(),
                 QStringLiteral("analysis.modal"));
        QVERIFY(host->approvePreview(previewId));

        // Removing an earlier tab changes the slot but not the fixed document identity.
        session->removeModel(session->findModelByCommandIdentity(firstIdentity));
        models.track(targetIdentity);
        QVERIFY(session->findModelByCommandIdentity(targetIdentity) != RConstants::eod);
        const auto accepted = commandExecute(session, previewId, QStringLiteral("gui-stable-target-after-reorder"));
        QCOMPARE(accepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        uint currentID = session->findModelByCommandIdentity(targetIdentity);
        QCOMPARE(session->getModel(currentID).getName(), QStringLiteral("Target remains named"));
        QCOMPARE(session->getModel(currentID).getProblemSetup().getModalSetup().getNIterations(), 31u);

        // A later geometry notification invalidates an approved stale preview.
        const auto stalePreview = commandPreview(session, targetIdentity, QStringLiteral("analysis.modal.configure"),
            {{QStringLiteral("method"), QStringLiteral("multiple")}, {QStringLiteral("iterations"), 40},
             {QStringLiteral("modes"), 4}, {QStringLiteral("tolerance"), 1e-7}, {QStringLiteral("unit"), QStringLiteral("1")}});
        QCOMPARE(stalePreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString staleId = stalePreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(staleId));
        currentID = session->findModelByCommandIdentity(targetIdentity);
        session->getModel(currentID).getNode(0).set(3.0, 0.0, 0.0);
        session->setModelChanged(currentID);
        const auto staleExecute = commandExecute(session, staleId, QStringLiteral("gui-stale-geometry-1"));
        QCOMPARE(staleExecute.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(staleExecute.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_REVISION"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(targetIdentity)).getNode(0).getX(), 3.0);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(targetIdentity))
                     .getProblemSetup().getModalSetup().getNIterations(), 31u);
        QCOMPARE(session->getModel(targetID < session->getNModels() ? targetID : 0).getNNodes(), 2u);
    }

    void legacyConfigureModalUsesTheSameCoreDeltaPath()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(modalFixture());
        const uint modelID = session->findModelByCommandIdentity(identity);
        const QString beforeHash = RModelCommandService::stateFingerprint(session->getModel(modelID));
        const QJsonObject beforeQuery = host->query(modelID);
        RModalSetup setup;
        setup.setMethod(R_MODAL_MULTIPLE_MODES);
        setup.setNIterations(43);
        setup.setNModesToExtract(4);
        QString error;
        QVERIFY2(host->configureModal({modelID}, setup, &error), qPrintable(error));
        const uint currentID = session->findModelByCommandIdentity(identity);
        QCOMPARE(session->getModel(currentID).getProblemSetup().getModalSetup().getNIterations(), 43u);
        const auto afterQuery = host->query(currentID);
        QCOMPARE(afterQuery.value(QStringLiteral("modelId")), beforeQuery.value(QStringLiteral("modelId")));
        const QJsonObject legacyAudit = lastJournalRecord(host->journalPath(),
            QStringLiteral("analysis.modal.configure"));
        QVERIFY(!legacyAudit.isEmpty());
        QCOMPARE(legacyAudit.value(QStringLiteral("preStateHash")).toString(), beforeHash);
        QCOMPARE(legacyAudit.value(QStringLiteral("postStateHash")).toString(),
                 RModelCommandService::stateFingerprint(session->getModel(currentID)));
    }

    void legacyConfigureModalJournalFailureRejectsBeforePublication()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(modalFixture());
        const uint modelID = session->findModelByCommandIdentity(identity);
        QVERIFY(!host->query(modelID).value(QStringLiteral("modelId")).toString().isEmpty());
        const QString beforeHash = RModelCommandService::stateFingerprint(session->getModel(modelID));
        const quint64 beforeRevision = session->modelCommandStamp(modelID).second;
        RModalSetup setup;
        setup.setMethod(R_MODAL_MULTIPLE_MODES);
        setup.setNIterations(47);
        setup.setNModesToExtract(4);

        QLockFile journalLock(host->journalPath() + QStringLiteral(".lock"));
        journalLock.setStaleLockTime(0);
        QVERIFY(journalLock.tryLock(0));
        QString error;
        QVERIFY(!host->configureModal({modelID}, setup, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity))
                     .getProblemSetup().getModalSetup().getNIterations(), 12u);
        QCOMPARE(RModelCommandService::stateFingerprint(
                     session->getModel(session->findModelByCommandIdentity(identity))), beforeHash);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identity)).second, beforeRevision);
        journalLock.unlock();
    }

    void coreJournalFailureRejectsBeforePublishingGuiMutation()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(stressLineFixture());
        const uint modelID = session->findModelByCommandIdentity(identity);
        const uint physicalId = session->getModel(modelID).getLine(0).getPhysicalId();
        const QJsonObject beforeMaterial = session->getModel(modelID).getLine(0).getMaterial().toJson();
        const quint64 beforeRevision = session->modelCommandStamp(modelID).second;
        const auto preview = commandPreview(session, identity, QStringLiteral("entity.material.set"),
            {{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(physicalId)},
             {QStringLiteral("name"), QStringLiteral("Must not publish")},
             {QStringLiteral("youngModulusPa"), 2.0e11}, {QStringLiteral("densityKgM3"), 8000.0},
             {QStringLiteral("poissonRatio"), 0.3}, {QStringLiteral("thermalExpansionPerK"), 1.2e-5}});
        QCOMPARE(preview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString previewId = preview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(previewId));

        QLockFile journalLock(host->journalPath() + QStringLiteral(".lock"));
        journalLock.setStaleLockTime(0);
        QVERIFY(journalLock.tryLock(0));
        const auto rejected = commandExecute(session, previewId, QStringLiteral("gui-journal-failure-retry"));
        QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(rejected.value(QStringLiteral("code")).toString(), QStringLiteral("COMMAND_JOURNAL_FAILURE"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity)).getLine(0).getMaterial().toJson(),
                 beforeMaterial);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identity)).second, beforeRevision);
        journalLock.unlock();

        const auto accepted = commandExecute(session, previewId, QStringLiteral("gui-journal-failure-retry"));
        QCOMPARE(accepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QVERIFY(accepted.value(QStringLiteral("result")).toObject().value(QStringLiteral("auditRecorded")).toBool());
        const QJsonObject record = lastJournalRecord(host->journalPath(), QStringLiteral("entity.material.set"));
        QVERIFY(!record.isEmpty());
        QCOMPARE(record.value(QStringLiteral("resultStatus")).toString(), QStringLiteral("accepted"));
    }

    void undoAuditFailureReportsModifiedStateAndIdempotentlyReplays()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(modalFixture());
        const auto editPreview = commandPreview(session, identity, QStringLiteral("analysis.modal.configure"),
            {{QStringLiteral("method"), QStringLiteral("multiple")},
             {QStringLiteral("iterations"), 24}, {QStringLiteral("modes"), 3},
             {QStringLiteral("tolerance"), 1e-7}, {QStringLiteral("unit"), QStringLiteral("1")}});
        QCOMPARE(editPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString editPreviewId = editPreview.value(QStringLiteral("result")).toObject()
                                          .value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(editPreviewId));
        const auto acceptedEdit = commandExecute(session, editPreviewId, QStringLiteral("gui-audit-failure-edit"));
        QCOMPARE(acceptedEdit.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identity))
                     .getProblemSetup().getModalSetup().getNIterations(), 24u);

        const auto undoPreview = commandPreview(session, identity, QStringLiteral("document.undo"), {});
        QCOMPARE(undoPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString undoPreviewId = undoPreview.value(QStringLiteral("result")).toObject()
                                         .value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(undoPreviewId));
        const quint64 beforeUndoRevision = session->modelCommandStamp(
            session->findModelByCommandIdentity(identity)).second;
        const QString undoKey = QStringLiteral("gui-undo-audit-failure-once");
        QLockFile journalLock(host->journalPath() + QStringLiteral(".lock"));
        journalLock.setStaleLockTime(0);
        QVERIFY(journalLock.tryLock(0));
        const auto failedAuditUndo = commandExecute(session, undoPreviewId, undoKey, QStringLiteral("document.undo"));
        QVERIFY2(failedAuditUndo.value(QStringLiteral("status")).toString() == QStringLiteral("error"),
                 qPrintable(QJsonDocument(failedAuditUndo).toJson(QJsonDocument::Compact)));
        QCOMPARE(failedAuditUndo.value(QStringLiteral("code")).toString(), QStringLiteral("OBSERVED_MODIFIED_UNLOGGED"));
        const QJsonObject failedResult = failedAuditUndo.value(QStringLiteral("result")).toObject();
        QVERIFY(failedResult.value(QStringLiteral("modelModified")).toBool());
        QVERIFY(!failedResult.value(QStringLiteral("auditRecorded")).toBool());
        QCOMPARE(failedResult.value(QStringLiteral("outcome")).toString(), QStringLiteral("observed_modified_unlogged"));
        const uint modelID = session->findModelByCommandIdentity(identity);
        QCOMPARE(session->getModel(modelID).getProblemSetup().getModalSetup().getNIterations(), 12u);
        const quint64 afterUndoRevision = session->modelCommandStamp(modelID).second;
        QVERIFY(afterUndoRevision > beforeUndoRevision);

        const auto operationQuery = host->handle(hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("operationId"), failedResult.value(QStringLiteral("operationId"))}}));
        QCOMPARE(operationQuery.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(operationQuery.value(QStringLiteral("result")).toObject().value(QStringLiteral("outcome")).toString(),
                 QStringLiteral("observed_modified_unlogged"));
        const auto duplicate = commandExecute(session, undoPreviewId, undoKey, QStringLiteral("document.undo"));
        QCOMPARE(duplicate.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        QCOMPARE(duplicate.value(QStringLiteral("code")).toString(), QStringLiteral("OBSERVED_MODIFIED_UNLOGGED"));
        QCOMPARE(duplicate.value(QStringLiteral("result")).toObject().value(QStringLiteral("operationId")).toString(),
                 failedResult.value(QStringLiteral("operationId")).toString());
        QCOMPARE(session->modelCommandStamp(modelID).second, afterUndoRevision);
        QCOMPARE(session->getModel(modelID).getProblemSetup().getModalSetup().getNIterations(), 12u);
        journalLock.unlock();
    }

    void modalDialogBlocksWritesButLeavesHostContextReadable()
    {
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        const QString identity = models.add(modalFixture());
        const QJsonObject target = targetFor(session, identity);
        const auto initialStamp = session->modelCommandStamp(session->findModelByCommandIdentity(identity));
        QDialog dialog;
        dialog.setModal(true);
        dialog.show();
        QCoreApplication::processEvents();
        QVERIFY(QApplication::activeModalWidget());

        const auto context = host->handle(hostRequest(QStringLiteral("context.query"),
            {{QStringLiteral("target"), target}}));
        QCOMPARE(context.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const auto blocked = host->handle(hostRequest(QStringLiteral("command.preview"),
            {{QStringLiteral("target"), target},
             {QStringLiteral("expectedDocumentRevision"), double(initialStamp.second)},
             {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
             {QStringLiteral("commandVersion"), 1},
             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("method"), QStringLiteral("multiple")},
                 {QStringLiteral("iterations"), 20}, {QStringLiteral("modes"), 3},
                 {QStringLiteral("tolerance"), 1e-7}, {QStringLiteral("unit"), QStringLiteral("1")}}}}));
        QCOMPARE(blocked.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(blocked.value(QStringLiteral("code")).toString(), QStringLiteral("INTERACTION_BLOCKED"));
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identity)).second, initialStamp.second);
        dialog.close();
    }

    void detachedNativeJobIsIdempotentEditableClosableAndCancellableByIdentity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        SessionModelGuard models(session);
        Model sourceA;
        Model sourceB;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-run-a"), &sourceA));
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-edit-b"), &sourceB));
        const QString identityA = models.add(sourceA);
        const QString identityB = models.add(sourceB);
        const uint modelB = session->findModelByCommandIdentity(identityB);
        const uint physicalB = session->getModel(modelB).getElementGroupPtr(0)->getPhysicalId();
        const QString sourceAName = session->getModel(session->findModelByCommandIdentity(identityA)).getName();
        const QString sourceBName = session->getModel(modelB).getName();
        const QString sourceBFile = session->getModel(modelB).getFileName();
        const quint64 sourceRevisionBeforeRun = session->modelCommandStamp(
            session->findModelByCommandIdentity(identityA)).second;

        const auto runPreview = commandPreview(session, identityA, QStringLiteral("analysis.run"),
            {{QStringLiteral("warningsAccepted"), true}});
        QVERIFY2(runPreview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(runPreview).toJson(QJsonDocument::Compact)));
        const QJsonObject runPlan = runPreview.value(QStringLiteral("result")).toObject();
        QVERIFY(runPlan.value(QStringLiteral("irreversible")).toBool());
        QCOMPARE(runPlan.value(QStringLiteral("history")).toString(), QStringLiteral("none"));
        const QString runPreviewId = runPlan.value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(runPreviewId));
        const QString executeKey = QStringLiteral("gui-native-run-idempotency-1");
        const auto submitted = commandExecute(session, runPreviewId, executeKey);
        QCOMPARE(submitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject runResult = submitted.value(QStringLiteral("result")).toObject();
        const QString jobId = runResult.value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());
        QVERIFY(runResult.value(QStringLiteral("sourceSnapshotRef")).isObject());
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identityA)).second,
                 sourceRevisionBeforeRun);
        const int totalAfterSubmit = manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();
        const auto duplicate = commandExecute(session, runPreviewId, executeKey);
        QCOMPARE(duplicate.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(duplicate.value(QStringLiteral("result")).toObject().value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), totalAfterSubmit);

        // A's frozen source does not block a supported edit to B.
        const QJsonObject materialArguments{{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(physicalB)},
            {QStringLiteral("name"), QStringLiteral("B steel")}, {QStringLiteral("youngModulusPa"), 2.0e11},
            {QStringLiteral("densityKgM3"), 8000.0}, {QStringLiteral("poissonRatio"), 0.3},
            {QStringLiteral("thermalExpansionPerK"), 1.2e-5}};
        const auto editPreview = commandPreview(session, identityB, QStringLiteral("entity.material.set"), materialArguments);
        QCOMPARE(editPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString editPreviewId = editPreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(editPreviewId));
        QCOMPARE(commandExecute(session, editPreviewId, QStringLiteral("gui-edit-b-during-a-run"))
                     .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identityB)).getElementGroupPtr(0)
                     ->getMaterial().getName(), QStringLiteral("B steel"));
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identityB)).getName(), sourceBName);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identityB)).getFileName(), sourceBFile);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(identityA)).getName(), sourceAName);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identityA)).second,
                 sourceRevisionBeforeRun);

        const uint sourceID = session->findModelByCommandIdentity(identityA);
        QVERIFY(sourceID != RConstants::eod);
        session->removeModel(sourceID);
        QVERIFY(session->findModelByCommandIdentity(identityA) == RConstants::eod);
        const auto cancelPreview = host->handle(hostRequest(QStringLiteral("command.preview"),
            {{QStringLiteral("command"), QStringLiteral("analysis.cancel")},
             {QStringLiteral("commandVersion"), 1},
             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("jobId"), jobId}}}}));
        QVERIFY2(cancelPreview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(cancelPreview).toJson(QJsonDocument::Compact)));
        const QString cancelPreviewId = cancelPreview.value(QStringLiteral("result")).toObject()
                                            .value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(cancelPreviewId));
        const auto cancelled = commandExecute(session, cancelPreviewId, QStringLiteral("gui-native-cancel-closed-source-1"));
        QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(cancelled.value(QStringLiteral("result")).toObject().value(QStringLiteral("history")).toString(),
                 QStringLiteral("none"));
        QVERIFY(cancelled.value(QStringLiteral("result")).toObject().value(QStringLiteral("irreversible")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("cancelled")
                                 || manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("failed")
                                 || manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("succeeded"),
                                 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
    }

    void manualNativeJobRetainsHostSourceForExplicitCancelAfterClose()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-manual-cancel"), &source));
        const QString identity = models.add(source);
        const uint modelID = session->findModelByCommandIdentity(identity);
        auto *manualTask = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        manualTask->setBlocking(false);
        QString submitError;
        const QString jobId = manager.submitValidated(manualTask, QJsonObject(), true, &submitError);
        QVERIFY2(!jobId.isEmpty(), qPrintable(submitError));

        const auto queried = host->handle(hostRequest(QStringLiteral("native.job.query"),
            {{QStringLiteral("jobId"), jobId}}));
        QCOMPARE(queried.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject job = queried.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject();
        const QJsonObject hostSource = job.value(QStringLiteral("hostSource")).toObject();
        QCOMPARE(hostSource.value(QStringLiteral("documentId")).toString(), identity);
        QCOMPARE(hostSource.value(QStringLiteral("sessionEpoch")).toString(), host->sessionEpoch());
        QCOMPARE(hostSource.value(QStringLiteral("revision")).toString(),
                 job.value(QStringLiteral("sourceRuntimeStamp")).toObject().value(QStringLiteral("revision")).toString());

        session->removeModel(session->findModelByCommandIdentity(identity));
        QVERIFY(session->findModelByCommandIdentity(identity) == RConstants::eod);
        const auto preview = host->handle(hostRequest(QStringLiteral("command.preview"),
            {{QStringLiteral("command"), QStringLiteral("analysis.cancel")},
             {QStringLiteral("commandVersion"), 1},
             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("jobId"), jobId}}}}));
        QVERIFY2(preview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        const QString previewId = preview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(previewId));
        const auto cancelled = commandExecute(session, previewId, QStringLiteral("gui-manual-job-cancel-after-close"));
        QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(cancelled.value(QStringLiteral("result")).toObject().value(QStringLiteral("jobId")).toString(), jobId);
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("cancelled")
                                 || manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("failed")
                                 || manager.jobQuery(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("succeeded"),
                                 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
    }

    void verifiedNativeResultOpensAsIndependentReusableDocument()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-result-source"), &source));
        const QJsonObject sourceMaterial = source.getElementGroupPtr(0)->getMaterial().toJson();
        const QString sourceIdentity = models.add(source);
        const QString sourceName = session->getModel(session->findModelByCommandIdentity(sourceIdentity)).getName();

        const auto preview = commandPreview(session, sourceIdentity, QStringLiteral("analysis.run"),
            {{QStringLiteral("warningsAccepted"), true}});
        QVERIFY2(preview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        const QString previewId = preview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(previewId));
        const auto submitted = commandExecute(session, previewId, QStringLiteral("gui-result-run-1"));
        QCOMPARE(submitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString jobId = submitted.value(QStringLiteral("result")).toObject().value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);

        const auto jobResponse = host->handle(hostRequest(QStringLiteral("native.job.query"),
            {{QStringLiteral("jobId"), jobId}}));
        QCOMPARE(jobResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject job = jobResponse.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject();
        QVERIFY(job.value(QStringLiteral("resultArtifactVerified")).toBool());
        const QString resultSha = job.value(QStringLiteral("resultArtifactSha256")).toString();
        QCOMPARE(resultSha.size(), 64);
        QVERIFY(QFileInfo::exists(job.value(QStringLiteral("resultArtifactRef")).toString()));

        auto gateStarted = std::make_shared<QSemaphore>();
        auto gateRelease = std::make_shared<QSemaphore>();
        GateReleaseGuard gateGuard{gateRelease};
        RJobManager::getInstance().submit(new GateJob(gateStarted, gateRelease));
        QVERIFY(gateStarted->tryAcquire(1, 10000));
        const uint documentsBeforeBusyOpen = session->getNModels();
        const auto readOnlyJobQuery = host->handle(hostRequest(QStringLiteral("native.job.query"),
            {{QStringLiteral("jobId"), jobId}}));
        QCOMPARE(readOnlyJobQuery.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const auto busyOpen = host->handle(hostRequest(QStringLiteral("native.result.open"),
            {{QStringLiteral("jobId"), jobId}, {QStringLiteral("expectedSHA"), resultSha}}));
        QCOMPARE(busyOpen.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(busyOpen.value(QStringLiteral("code")).toString(), QStringLiteral("MODEL_BUSY"));
        QCOMPARE(session->getNModels(), documentsBeforeBusyOpen);
        gateRelease->release();
        gateGuard.release.reset();
        QTRY_VERIFY_WITH_TIMEOUT(RJobManager::getInstance().getNRunning() == 0
                                     && RJobManager::getInstance().getNWaiting() == 0,
                                 15000);

        QString wrongSha = resultSha;
        wrongSha[0] = wrongSha.at(0) == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
        const uint documentsBeforeWrongHash = session->getNModels();
        const auto rejectedOpen = host->handle(hostRequest(QStringLiteral("native.result.open"),
            {{QStringLiteral("jobId"), jobId}, {QStringLiteral("expectedSHA"), wrongSha}}));
        QCOMPARE(rejectedOpen.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(rejectedOpen.value(QStringLiteral("code")).toString(), QStringLiteral("ARTIFACT_IDENTITY_MISMATCH"));
        QCOMPARE(session->getNModels(), documentsBeforeWrongHash);

        const auto open = [&]() {
            return host->handle(hostRequest(QStringLiteral("native.result.open"),
                {{QStringLiteral("jobId"), jobId}, {QStringLiteral("expectedSHA"), resultSha}}));
        };
        const auto opened = open();
        QVERIFY2(opened.value(QStringLiteral("status")).toString() == QStringLiteral("accepted"),
                 qPrintable(QJsonDocument(opened).toJson(QJsonDocument::Compact)));
        const QJsonObject openedResult = opened.value(QStringLiteral("result")).toObject();
        QCOMPARE(openedResult.value(QStringLiteral("openResult")).toString(), QStringLiteral("created"));
        QVERIFY(openedResult.value(QStringLiteral("resultViewCreated")).toBool());
        QVERIFY(openedResult.value(QStringLiteral("sourceAndSolverUnchanged")).toBool());
        const QString resultIdentity = openedResult.value(QStringLiteral("documentId")).toString();
        QVERIFY(!resultIdentity.isEmpty());
        QVERIFY(resultIdentity != sourceIdentity);
        models.track(resultIdentity);
        QCOMPARE(openedResult.value(QStringLiteral("sourceJob")).toObject().value(QStringLiteral("jobId")).toString(), jobId);
        QVERIFY(!openedResult.value(QStringLiteral("persistentDocumentId")).toString().isEmpty());
        const QJsonObject sourceJob = openedResult.value(QStringLiteral("sourceJob")).toObject();
        QCOMPARE(sourceJob.value(QStringLiteral("hostSource")).toObject()
                     .value(QStringLiteral("sessionEpoch")).toString(), host->sessionEpoch());
        const QJsonObject inputProvenance = sourceJob.value(QStringLiteral("inputProvenance")).toObject();
        QCOMPARE(inputProvenance.value(QStringLiteral("inputDerivedFrom")).toString(),
                 QStringLiteral("frozen_project_snapshot"));
        QVERIFY(inputProvenance.value(QStringLiteral("frozenSnapshotVerified")).toBool());
        QVERIFY(!inputProvenance.value(QStringLiteral("frozenSnapshotSha256")).toString().isEmpty());
        QVERIFY(!inputProvenance.value(QStringLiteral("solver_inputSnapshotSha256")).toString().isEmpty());
        const QJsonObject displayDescriptor = openedResult.value(QStringLiteral("displayDescriptor")).toObject();
        QVERIFY(displayDescriptor.contains(QStringLiteral("normalizedAtOpen")));
        QVERIFY(displayDescriptor.contains(QStringLiteral("dirtyStatus")));
        const uint resultID = session->findModelByCommandIdentity(resultIdentity);
        QVERIFY(resultID != RConstants::eod);
        QCOMPARE(session->getModel(resultID).getName(), sourceName);
        QVERIFY2(captureNativeWindowEvidence(QStringLiteral("independent-native-result"), resultIdentity),
                 "Could not capture the independent result document in the visible Windows GUI.");

        const auto reopened = open();
        QCOMPARE(reopened.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject reopenedResult = reopened.value(QStringLiteral("result")).toObject();
        QCOMPARE(reopenedResult.value(QStringLiteral("openResult")).toString(), QStringLiteral("reused"));
        QCOMPARE(reopenedResult.value(QStringLiteral("documentId")).toString(), resultIdentity);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(sourceIdentity)).getName(), sourceName);

        const uint editedResultID = session->findModelByCommandIdentity(resultIdentity);
        const uint resultPhysicalId = session->getModel(editedResultID).getElementGroupPtr(0)->getPhysicalId();
        const QJsonObject materialArguments{{QStringLiteral("targetId"), QStringLiteral("g:%1").arg(resultPhysicalId)},
            {QStringLiteral("name"), QStringLiteral("Edited result only")},
            {QStringLiteral("youngModulusPa"), 2.5e11}, {QStringLiteral("densityKgM3"), 8050.0},
            {QStringLiteral("poissonRatio"), 0.29}, {QStringLiteral("thermalExpansionPerK"), 1.3e-5}};
        const auto editPreview = commandPreview(session, resultIdentity,
            QStringLiteral("entity.material.set"), materialArguments);
        QCOMPARE(editPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString editPreviewId = editPreview.value(QStringLiteral("result")).toObject()
                                          .value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(editPreviewId));
        QCOMPARE(commandExecute(session, editPreviewId, QStringLiteral("gui-edit-open-result-view"))
                     .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject editedMaterial = session->getModel(session->findModelByCommandIdentity(resultIdentity))
                                               .getElementGroupPtr(0)->getMaterial().toJson();
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(resultIdentity))
                     .getElementGroupPtr(0)->getMaterial().getName(), QStringLiteral("Edited result only"));

        const auto editedContext = host->handle(hostRequest(QStringLiteral("document.inputs.query"),
            {{QStringLiteral("target"), targetFor(session, resultIdentity)}}));
        QCOMPARE(editedContext.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject resultView = editedContext.value(QStringLiteral("result")).toObject()
                                           .value(QStringLiteral("resultView")).toObject();
        QVERIFY(resultView.value(QStringLiteral("modifiedSinceOpen")).toBool());
        QCOMPARE(resultView.value(QStringLiteral("dirtyStatus")).toString(), QStringLiteral("edited_after_open"));

        const auto reopenedFresh = open();
        QVERIFY2(reopenedFresh.value(QStringLiteral("status")).toString() == QStringLiteral("accepted"),
                 qPrintable(QJsonDocument(reopenedFresh).toJson(QJsonDocument::Compact)));
        const QJsonObject freshResult = reopenedFresh.value(QStringLiteral("result")).toObject();
        QVERIFY2(freshResult.value(QStringLiteral("openResult")).toString() == QStringLiteral("created"),
                 qPrintable(QJsonDocument(reopenedFresh).toJson(QJsonDocument::Compact)));
        QVERIFY(freshResult.value(QStringLiteral("documentId")).toString() != resultIdentity);
        const QString freshIdentity = freshResult.value(QStringLiteral("documentId")).toString();
        models.track(freshIdentity);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(resultIdentity))
                     .getElementGroupPtr(0)->getMaterial().toJson(), editedMaterial);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(freshIdentity))
                     .getElementGroupPtr(0)->getMaterial().toJson(), sourceMaterial);
        const auto reopenedFreshAgain = open();
        QCOMPARE(reopenedFreshAgain.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(reopenedFreshAgain.value(QStringLiteral("result")).toObject().value(QStringLiteral("openResult")).toString(),
                 QStringLiteral("reused"));
        QCOMPARE(reopenedFreshAgain.value(QStringLiteral("result")).toObject().value(QStringLiteral("documentId")).toString(),
                 freshIdentity);

        const QJsonObject immutableBinding = job.value(QStringLiteral("resultSourceBinding")).toObject();
        const QString terminalPath = immutableBinding.value(QStringLiteral("resolvedSourcePath")).toString();
        QVERIFY(!terminalPath.isEmpty());
        QVERIFY(terminalPath != immutableBinding.value(QStringLiteral("logicalSourcePath")).toString());
        QFile terminal(terminalPath);
        QVERIFY(terminal.open(QIODevice::ReadOnly));
        const QByteArray originalTerminal = terminal.readAll();
        terminal.close();
        QVERIFY(terminal.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(terminal.write("\n"), qint64(1));
        terminal.close();
        const uint documentsBeforeTamper = session->getNModels();
        const auto terminalTamper = open();
        QCOMPARE(terminalTamper.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(terminalTamper.value(QStringLiteral("code")).toString(), QStringLiteral("ARTIFACT_CHANGED"));
        QCOMPARE(session->getNModels(), documentsBeforeTamper);
        QCOMPARE(manager.jobQuery(jobId).value(QStringLiteral("resultSourceBinding")).toObject(), immutableBinding);
        QVERIFY(terminal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(terminal.write(originalTerminal), qint64(originalTerminal.size()));
        terminal.close();
    }
};

int runGuiModelOperationsTests(int argc, char **argv)
{
    TestGuiModelOperations test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_gui_model_operations.moc"
