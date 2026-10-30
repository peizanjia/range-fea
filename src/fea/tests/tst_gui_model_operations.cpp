#include <QtTest>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QScopeGuard>
#include <QSaveFile>
#include <QSemaphore>
#include <QSet>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>
#include <vector>

#include <rbl_job.h>
#include <rmodel_command_service.h>
#include <rtrusted_convergence_service.h>
#include <rtrusted_experiment_service.h>
#include <rtrusted_mechanics_service.h>
#include <rtrusted_verification_service.h>
#include <rbl_error.h>
#include <rml_file.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_save_file.h>

#include "application.h"
#include "model_command_host.h"
#include "model_io.h"
#include "problem_tree.h"
#include "model_tree.h"
#include "session.h"
#include "solver_manager.h"
#include "solver_task.h"
#include "trusted_solver_diagnostics.h"
#include "../src/trusted_artifact_paths.h"
#include "value_line_edit.h"

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <sys/stat.h>
#endif

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

QJsonObject trustedExperimentRodSpec()
{
    const auto quantity = [](double value, const char *unit) {
        return QJsonObject{{QStringLiteral("value"), value}, {QStringLiteral("unit"), QString::fromLatin1(unit)}};
    };
    const QJsonArray load{quantity(1000.0, "N"), quantity(0.0, "N"), quantity(0.0, "N")};
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("modelType"), QStringLiteral("axialRod")},
            {QStringLiteral("name"), QStringLiteral("Host experiment rod")},
            {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), quantity(1.0, "m")},
                                                       {QStringLiteral("segments"), 4}}},
            {QStringLiteral("material"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Steel")},
                                                       {QStringLiteral("source"), QStringLiteral("fixture")},
                                                       {QStringLiteral("youngModulus"), quantity(210e9, "Pa")},
                                                       {QStringLiteral("poissonRatio"), 0.3},
                                                       {QStringLiteral("density"), quantity(7850.0, "kg/m3")}}},
            {QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), quantity(1e-4, "m2")}}},
            {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}}},
            {QStringLiteral("supports"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}},
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}}}},
            {QStringLiteral("loads"), QJsonArray{
                 QJsonObject{{QStringLiteral("kind"), QStringLiteral("pointForce")},
                             {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("components"), load}}}},
            {QStringLiteral("initialConditions"), QJsonArray{}},
            {QStringLiteral("observables"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("quantity"), QStringLiteral("displacement")},
                             {QStringLiteral("component"), QStringLiteral("ux")}}}},
            {QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}}}};
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

void runTrustedConvergenceStudy(Session *session, const QString &documentId,
                                const QJsonObject &problemSpec, const QJsonObject &study,
                                const QJsonArray &ranges, int solveBudget, int repairBudget,
                                const QString &idempotencyKey, QJsonObject *result)
{
    if (!result) return;
    const auto validation = RTrustedMechanicsService::validate(problemSpec);
    if (!validation.valid)
        qWarning().noquote() << QJsonDocument(QJsonObject{{QStringLiteral("problemSpecIssues"), validation.issues}})
                                    .toJson(QJsonDocument::Compact);
    QVERIFY2(validation.valid,
             qPrintable(QJsonDocument(QJsonObject{{QStringLiteral("issues"), validation.issues}})
                            .toJson(QJsonDocument::Compact)));
    const auto studyValidation = RTrustedConvergenceService::validateStudy(
        problemSpec, study);
    QVERIFY2(studyValidation.ok,
             qPrintable(studyValidation.code + QStringLiteral(": ") + studyValidation.message));
    ModelCommandHost *host = session->getModelCommandHost();
    SolverManager &manager = SolverManager::getInstance();
    const auto preview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
        {{QStringLiteral("problemSpec"), problemSpec},
         {QStringLiteral("convergenceStudy"), study},
         {QStringLiteral("allowedNumericalRanges"), ranges},
         {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), solveBudget},
                                                   {QStringLiteral("repairs"), repairBudget},
                                                   {QStringLiteral("modelTurns"), 12}}}});
    QVERIFY2(preview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
             qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
    const QJsonObject plan = preview.value(QStringLiteral("result")).toObject();
    const QString previewId = plan.value(QStringLiteral("previewId")).toString();
    const QString experimentId = plan.value(QStringLiteral("experimentId")).toString();
    QVERIFY(!previewId.isEmpty());
    QVERIFY(!experimentId.isEmpty());
    QVERIFY(host->approvePreview(previewId));
    const QJsonObject submitted = commandExecute(session, previewId, idempotencyKey);
    QVERIFY2(submitted.value(QStringLiteral("status")).toString() == QStringLiteral("accepted"),
             qPrintable(QJsonDocument(submitted).toJson(QJsonDocument::Compact)));
    QVERIFY(!submitted.value(QStringLiteral("result")).toObject().value(QStringLiteral("jobId")).toString().isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const auto response = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}}));
        const QString state = response.value(QStringLiteral("result")).toObject()
                                  .value(QStringLiteral("state")).toString();
        return state == QStringLiteral("candidate_ready") || state == QStringLiteral("not_converged")
            || state == QStringLiteral("budget_exhausted") || state == QStringLiteral("failed")
            || state == QStringLiteral("needs_confirmation") || state == QStringLiteral("cancelled");
    }(), 240000);
    QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 30000);
    const auto queried = host->handle(hostRequest(QStringLiteral("experiment.query"),
        {{QStringLiteral("experimentId"), experimentId}}));
    QCOMPARE(queried.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    *result = queried.value(QStringLiteral("result")).toObject();
    const QString configuredDataRoot = Application::instance()->getApplicationSettings()->getDataDir();
    const QString dataRoot = configuredDataRoot.isEmpty() ? QDir::tempPath() : configuredDataRoot;
    RTrustedExperimentService experiments(QDir(dataRoot).filePath(QStringLiteral("trusted-experiments")));
    const auto detailed = experiments.query(experimentId);
    QVERIFY2(detailed.ok, qPrintable(detailed.message));
    const QJsonArray auditLevels = detailed.data.value(QStringLiteral("experiment")).toObject()
                                       .value(QStringLiteral("convergenceLevels")).toArray();
    for (const QJsonValue &entry : auditLevels) {
        const QJsonObject level = entry.toObject();
        const QJsonObject report = level.value(QStringLiteral("verificationReport")).toObject();
        const QJsonObject identity = report.value(QStringLiteral("identity")).toObject();
        QVERIFY2(report.value(QStringLiteral("schema")).toString() == QStringLiteral("range.verification-report/v1"),
                 qPrintable(QJsonDocument(level).toJson(QJsonDocument::Compact)));
        QCOMPARE(identity.value(QStringLiteral("artifactId")), level.value(QStringLiteral("artifactId")));
        QCOMPARE(identity.value(QStringLiteral("jobId")), level.value(QStringLiteral("jobId")));
        QCOMPARE(identity.value(QStringLiteral("manifestSha256")), level.value(QStringLiteral("manifestSha256")));
        QCOMPARE(level.value(QStringLiteral("measurementSha256")).toString().size(), 64);
        QCOMPARE(report.value(QStringLiteral("passed")).toBool(), level.value(QStringLiteral("verificationPassed")).toBool());
    }
    const QJsonArray children = result->value(QStringLiteral("childJobs")).toArray();
    QVERIFY(!children.isEmpty());
    for (const QJsonValue &child : children)
        QVERIFY2(child.toObject().value(QStringLiteral("state")).toString() == QStringLiteral("succeeded"),
                 qPrintable(QJsonDocument(*result).toJson(QJsonDocument::Compact)));
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

        const auto replayedRejection = commandExecute(session, previewId, QStringLiteral("gui-journal-failure-retry"));
        QCOMPARE(replayedRejection.value("status"), rejected.value("status"));
        QCOMPARE(replayedRejection.value("code"), rejected.value("code"));
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(identity)).second, beforeRevision);
        const auto accepted = commandExecute(session, previewId, QStringLiteral("gui-journal-failure-new-request"));
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
        QVERIFY2(submitted.value(QStringLiteral("status")).toString() == QStringLiteral("accepted"),
                 qPrintable(QJsonDocument(submitted).toJson(QJsonDocument::Compact)));
        const QJsonObject runResult = submitted.value(QStringLiteral("result")).toObject();
        const QString jobId = runResult.value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());
        QVERIFY(runResult.value(QStringLiteral("sourceSnapshotRef")).isObject());
        QVERIFY(QJsonDocument(submitted).toJson(QJsonDocument::Compact)
                    .contains("trustedAuthorizationDigest"));
        const auto operationQuery = host->handle(hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("operationId"), runResult.value(QStringLiteral("operationId"))}}));
        QCOMPARE(operationQuery.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(!QJsonDocument(operationQuery).toJson(QJsonDocument::Compact)
                     .contains("trustedAuthorizationDigest"));
        const auto durableOperationQuery = host->handle(hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("idempotencyKey"), executeKey}}));
        QCOMPARE(durableOperationQuery.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(!QJsonDocument(durableOperationQuery).toJson(QJsonDocument::Compact)
                     .contains("trustedAuthorizationDigest"));
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

    void trustedExperimentPreviewApprovalRunQueryAndCancelStayBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-experiment-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QByteArray previousFailureLogMode = qgetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        const bool hadPreviousFailureLogMode = qEnvironmentVariableIsSet("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        const auto restoreFailureLogMode = qScopeGuard([&] {
            if (hadPreviousFailureLogMode)
                qputenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE", previousFailureLogMode);
            else
                qunsetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        });
        qunsetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));
        const int jobsBefore = manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();

        const QJsonObject specification = trustedExperimentRodSpec();
        const auto missingBudgetPreview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), specification}});
        QCOMPARE(missingBudgetPreview.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(missingBudgetPreview.value(QStringLiteral("code")).toString(),
                 QStringLiteral("EXPERIMENT_SPEC_INVALID"));
        const auto preview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), specification},
             {QStringLiteral("allowedNumericalRanges"), QJsonArray{
                 QJsonObject{{QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                             {QStringLiteral("minimum"), 4}, {QStringLiteral("maximum"), 16}}}},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 3},
                                                       {QStringLiteral("repairs"), 1},
                                                       {QStringLiteral("modelTurns"), 4}}}});
        QVERIFY2(preview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        const QJsonObject plan = preview.value(QStringLiteral("result")).toObject();
        const QString previewId = plan.value(QStringLiteral("previewId")).toString();
        const QString experimentId = plan.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!previewId.isEmpty());
        QVERIFY(!experimentId.isEmpty());
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), jobsBefore);
        const QString configuredDataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
            ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        RTrustedExperimentService experimentStore(QDir(configuredDataRoot).filePath(QStringLiteral("trusted-experiments")));
        QVERIFY(!experimentStore.query(experimentId).ok);
        const QJsonObject expectedBudgets{{QStringLiteral("solves"), 3},
            {QStringLiteral("repairs"), 1}, {QStringLiteral("modelTurns"), 4}};
        QCOMPARE(plan.value(QStringLiteral("arguments")).toObject()
                     .value(QStringLiteral("budgets")).toObject(), expectedBudgets);
        QVERIFY(host->evaluationEvidenceSnapshot(experimentId).isEmpty());
        QVERIFY(host->evaluationEvidenceSnapshot(QStringLiteral("missing-evidence-experiment")).isEmpty());

        // Preview is read-only: durable authorization begins only on Host Apply.
        QVERIFY(host->approvePreview(previewId));
        const auto prepared = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}}));
        QCOMPARE(prepared.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject preparedSummary = prepared.value(QStringLiteral("result")).toObject();
        QCOMPARE(preparedSummary.value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
        const QJsonObject preparedSource = preparedSummary.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        QCOMPARE(preparedSource.value(QStringLiteral("documentId")).toString(), sourceStamp.first);
        QCOMPARE(preparedSource.value(QStringLiteral("sessionEpoch")).toString(), host->sessionEpoch());
        QCOMPARE(preparedSource.value(QStringLiteral("revision")).toString(), QString::number(sourceStamp.second));
        QCOMPARE(preparedSummary.value(QStringLiteral("budgets")).toObject(), expectedBudgets);
        const auto preparedRecord = experimentStore.query(experimentId);
        QVERIFY2(preparedRecord.ok, qPrintable(preparedRecord.message));
        QCOMPARE(preparedRecord.data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("authorization")).toObject()
                     .value(QStringLiteral("experimentPolicy")).toObject()
                     .value(QStringLiteral("budgets")).toObject(), expectedBudgets);

        const auto submitted = commandExecute(session, previewId, QStringLiteral("host-trusted-experiment-run-1"));
        QCOMPARE(submitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QJsonObject runResult = submitted.value(QStringLiteral("result")).toObject();
        const QString jobId = runResult.value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("succeeded"), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}})).value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"), 30000);
        QVERIFY(host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}})).value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("verificationPassed")).toBool());
        const QJsonObject reportSummary = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}})).value(QStringLiteral("result")).toObject();
        QCOMPARE(reportSummary.value(QStringLiteral("sourceRuntimeStamp")).toObject(), preparedSource);
        QCOMPARE(reportSummary.value(QStringLiteral("budgets")).toObject(), expectedBudgets);
        QCOMPARE(reportSummary.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("pass"));
        QVERIFY(!reportSummary.value(QStringLiteral("verification")).toObject()
                     .value(QStringLiteral("metrics")).toObject().isEmpty());
        const auto completedRecord = experimentStore.query(experimentId);
        QVERIFY2(completedRecord.ok, qPrintable(completedRecord.message));
        QCOMPARE(completedRecord.data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("authorization")).toObject()
                     .value(QStringLiteral("experimentPolicy")).toObject()
                     .value(QStringLiteral("budgets")).toObject(), expectedBudgets);
        const QJsonObject durableExperiment = completedRecord.data.value(QStringLiteral("experiment")).toObject();
        const QJsonObject evidenceSnapshot = host->evaluationEvidenceSnapshot(experimentId);
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.trusted-experiment-evidence-snapshot/v1"));
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("experimentId")).toString(), experimentId);
        const QString evidenceAttemptId = durableExperiment.value(QStringLiteral("latestSolveAttemptId")).toString();
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("attemptId")).toString(), evidenceAttemptId);
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("sourceBinding")).toObject(),
                 durableExperiment.value(QStringLiteral("authorization")).toObject()
                     .value(QStringLiteral("sourceBinding")).toObject());
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("sourceRuntimeStamp")).toObject(), preparedSource);
        const QJsonObject latestSolveIdentity = durableExperiment.value(QStringLiteral("latestSolveIdentity")).toObject();
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("latestSolveIdentity")).toObject(), latestSolveIdentity);
        const QJsonObject childEvidence = evidenceSnapshot.value(QStringLiteral("childJob")).toObject();
        QCOMPARE(childEvidence.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(childEvidence.value(QStringLiteral("experimentId")).toString(), experimentId);
        QCOMPARE(childEvidence.value(QStringLiteral("attemptId")).toString(), evidenceAttemptId);
        QCOMPARE(childEvidence.value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
        QCOMPARE(childEvidence.value(QStringLiteral("inputSnapshotSha256")),
                 latestSolveIdentity.value(QStringLiteral("inputSnapshotSha256")));
        QCOMPARE(childEvidence.value(QStringLiteral("inputPackageSha256")),
                 latestSolveIdentity.value(QStringLiteral("inputPackageSha256")));
        QCOMPARE(childEvidence.value(QStringLiteral("resultArtifactId")), latestSolveIdentity.value(QStringLiteral("artifactId")));
        QCOMPARE(childEvidence.value(QStringLiteral("nativeResultProvenance")).toObject()
                     .value(QStringLiteral("sourceSnapshotRef")), latestSolveIdentity.value(QStringLiteral("sourceSnapshotRef")));
        QVERIFY(childEvidence.value(QStringLiteral("resultManifestVerified")).toBool());
        QVERIFY(!childEvidence.value(QStringLiteral("resultManifestRef")).toString().isEmpty());
        QVERIFY(!childEvidence.value(QStringLiteral("resultManifestSha256")).toString().isEmpty());
        const QJsonObject persistedReport = durableExperiment.value(QStringLiteral("verificationReport")).toObject();
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("verificationReport")).toObject(), persistedReport);
        const QString expectedReportSha = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(persistedReport).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QCOMPARE(evidenceSnapshot.value(QStringLiteral("verificationReportSha256")).toString(), expectedReportSha);
        QVERIFY(QJsonDocument(evidenceSnapshot).toJson(QJsonDocument::Compact).size() <= 512 * 1024);

        const QJsonObject persistedReportIdentity = persistedReport.value(QStringLiteral("identity")).toObject();
        const QString sourceSnapshotRefSha256 = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(latestSolveIdentity.value(QStringLiteral("sourceSnapshotRef")).toObject())
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        const QJsonObject expectedCaptureIdentity{
            {QStringLiteral("jobId"), jobId},
            {QStringLiteral("artifactId"), latestSolveIdentity.value(QStringLiteral("artifactId"))},
            {QStringLiteral("manifestSha256"), childEvidence.value(QStringLiteral("resultManifestSha256"))},
            {QStringLiteral("inputSnapshotSha256"), latestSolveIdentity.value(QStringLiteral("inputSnapshotSha256"))},
            {QStringLiteral("inputPackageSha256"), latestSolveIdentity.value(QStringLiteral("inputPackageSha256"))},
            {QStringLiteral("sourceSnapshotRefSha256"), sourceSnapshotRefSha256},
            {QStringLiteral("normalizedProblemSpecSha256"), persistedReportIdentity.value(QStringLiteral("normalizedProblemSpecSha256"))},
            {QStringLiteral("physicalSummarySha256"), persistedReportIdentity.value(QStringLiteral("physicalSummarySha256"))},
            {QStringLiteral("trustedExperimentId"), experimentId},
            {QStringLiteral("trustedExperimentAttemptId"), evidenceAttemptId},
            {QStringLiteral("trustedAuthorizationDigest"),
                durableExperiment.value(QStringLiteral("authorization")).toObject()
                    .value(QStringLiteral("authorizationDigest"))}};
        const QJsonObject reopened = manager.trustedResultVerification(jobId,
            latestSolveIdentity.value(QStringLiteral("artifactId")).toString(),
            childEvidence.value(QStringLiteral("resultManifestSha256")).toString(),
            durableExperiment.value(QStringLiteral("candidateProblemSpecInput")).toObject(),
            expectedCaptureIdentity);
        QVERIFY2(reopened.value(QStringLiteral("valid")).toBool(),
                 qPrintable(QJsonDocument(reopened).toJson(QJsonDocument::Compact)));
        QVERIFY(reopened.value(QStringLiteral("artifactReverified")).toBool());
        QCOMPARE(reopened.value(QStringLiteral("verificationReport")).toObject(), persistedReport);
        QCOMPARE(reopened.value(QStringLiteral("reportSha256")).toString(), expectedReportSha);
        QCOMPARE(reopened.value(QStringLiteral("trustedExperimentId")).toString(), experimentId);
        QCOMPARE(reopened.value(QStringLiteral("trustedExperimentAttemptId")).toString(), evidenceAttemptId);
        QCOMPARE(reopened.value(QStringLiteral("trustedAuthorizationDigest")).toString(),
            durableExperiment.value(QStringLiteral("authorization")).toObject()
                .value(QStringLiteral("authorizationDigest")).toString());
        QVERIFY(!reopened.contains(QStringLiteral("resultManifestRef")));

        if (qEnvironmentVariableIsSet("RANGE_APPLICATION_PROFILE_DIR"))
        {
            const QString verifierExecutable = QDir(QCoreApplication::applicationDirPath())
                                                   .filePath(QStringLiteral("fea.exe"));
            QVERIFY2(QFileInfo::exists(verifierExecutable),
                     qPrintable(QStringLiteral("The headless verifier executable is missing: %1")
                                    .arg(verifierExecutable)));
            QJsonObject requestItem{
                {QStringLiteral("jobId"), jobId},
                {QStringLiteral("artifactId"), latestSolveIdentity.value(QStringLiteral("artifactId"))},
                {QStringLiteral("manifestSha256"), childEvidence.value(QStringLiteral("resultManifestSha256"))},
                {QStringLiteral("expectedCaptureIdentity"), expectedCaptureIdentity},
                {QStringLiteral("problemSpec"), durableExperiment.value(QStringLiteral("candidateProblemSpecInput"))}};
            QJsonObject verifiedItem = requestItem;
            verifiedItem.insert(QStringLiteral("slotId"), QStringLiteral("cli-batch-slot-01"));
            QJsonObject missingJobItem = requestItem;
            missingJobItem.insert(QStringLiteral("jobId"), QStringLiteral("missing-cli-batch-job"));
            missingJobItem.insert(QStringLiteral("artifactId"), QStringLiteral("missing-cli-batch-artifact"));
            missingJobItem.insert(QStringLiteral("slotId"), QStringLiteral("cli-batch-slot-02"));
            QJsonObject missingIdentity = expectedCaptureIdentity;
            missingIdentity.insert(QStringLiteral("jobId"), QStringLiteral("missing-cli-batch-job"));
            missingIdentity.insert(QStringLiteral("artifactId"), QStringLiteral("missing-cli-batch-artifact"));
            missingIdentity.insert(QStringLiteral("manifestSha256"), QString(64, QLatin1Char('a')));
            missingJobItem.insert(QStringLiteral("manifestSha256"), QString(64, QLatin1Char('a')));
            missingJobItem.insert(QStringLiteral("expectedCaptureIdentity"), missingIdentity);
            QJsonArray batchItems{verifiedItem, missingJobItem};
            const QJsonObject request{
                {QStringLiteral("schema"), QStringLiteral("range.trusted-artifact-reopen-batch/v1")},
                {QStringLiteral("requests"), batchItems}};
            QProcess verifier;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_WIN
            // The installed Windows runtime ships the Windows platform plugin,
            // not Qt's offscreen plugin. The verifier creates no widgets, so use
            // the native platform plugin for this separate CLI process.
            environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("windows"));
#else
            environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
#endif
            verifier.setProcessEnvironment(environment);
            verifier.setWorkingDirectory(QCoreApplication::applicationDirPath());
            verifier.start(verifierExecutable, {QStringLiteral("--trusted-evidence-reopen")});
            QVERIFY2(verifier.waitForStarted(10000),
                     qPrintable(QStringLiteral("The headless verifier did not start: %1")
                                    .arg(verifier.errorString())));
            const QByteArray requestBytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
            QCOMPARE(verifier.write(requestBytes), qint64(requestBytes.size()));
            verifier.closeWriteChannel();
            QVERIFY2(verifier.waitForFinished(120000),
                     qPrintable(QStringLiteral("The headless verifier did not finish: %1")
                                    .arg(QString::fromUtf8(verifier.readAllStandardError()))));
            QCOMPARE(verifier.exitStatus(), QProcess::NormalExit);
            QCOMPARE(verifier.exitCode(), 0);
            QJsonParseError cliParseError;
            const QJsonObject cliBatch = QJsonDocument::fromJson(verifier.readAllStandardOutput(),
                                                                  &cliParseError).object();
            QCOMPARE(cliParseError.error, QJsonParseError::NoError);
            QCOMPARE(cliBatch.value(QStringLiteral("schema")).toString(),
                     QStringLiteral("range.trusted-artifact-reopen-batch/v1"));
            const QJsonArray cliResults = cliBatch.value(QStringLiteral("results")).toArray();
            QCOMPARE(cliResults.size(), 2);
            const QJsonObject verifiedRow = cliResults.at(0).toObject();
            QVERIFY2(verifiedRow.value(QStringLiteral("valid")).toBool(),
                     qPrintable(QJsonDocument(verifiedRow).toJson(QJsonDocument::Compact)));
            QCOMPARE(verifiedRow.value(QStringLiteral("slotId")).toString(),
                     QStringLiteral("cli-batch-slot-01"));
            QCOMPARE(verifiedRow.value(QStringLiteral("verificationReport")).toObject(), persistedReport);
            QCOMPARE(verifiedRow.value(QStringLiteral("trustedExperimentId")).toString(), experimentId);
            QCOMPARE(verifiedRow.value(QStringLiteral("trustedExperimentAttemptId")).toString(), evidenceAttemptId);
            QCOMPARE(verifiedRow.value(QStringLiteral("trustedAuthorizationDigest")).toString(),
                durableExperiment.value(QStringLiteral("authorization")).toObject()
                    .value(QStringLiteral("authorizationDigest")).toString());
            QVERIFY(!verifiedRow.contains(QStringLiteral("resultManifestRef")));

            const QJsonObject missingRow = cliResults.at(1).toObject();
            QVERIFY(!missingRow.value(QStringLiteral("valid")).toBool());
            QCOMPARE(missingRow.value(QStringLiteral("slotId")).toString(),
                     QStringLiteral("cli-batch-slot-02"));
            QCOMPARE(missingRow.value(QStringLiteral("code")).toString(), QStringLiteral("UNKNOWN_NATIVE_JOB"));

            QJsonObject duplicateRequest = request;
            QJsonObject duplicateArtifactItem = verifiedItem;
            duplicateArtifactItem.insert(QStringLiteral("slotId"), QStringLiteral("cli-batch-slot-alias"));
            duplicateRequest.insert(QStringLiteral("requests"),
                QJsonArray{verifiedItem, duplicateArtifactItem});
            QProcess duplicateVerifier;
            duplicateVerifier.setProcessEnvironment(environment);
            duplicateVerifier.setWorkingDirectory(QCoreApplication::applicationDirPath());
            duplicateVerifier.start(verifierExecutable, {QStringLiteral("--trusted-evidence-reopen")});
            QVERIFY2(duplicateVerifier.waitForStarted(10000),
                     qPrintable(QStringLiteral("The duplicate-artifact verifier did not start: %1")
                                    .arg(duplicateVerifier.errorString())));
            const QByteArray duplicateBytes = QJsonDocument(duplicateRequest).toJson(QJsonDocument::Compact);
            QCOMPARE(duplicateVerifier.write(duplicateBytes), qint64(duplicateBytes.size()));
            duplicateVerifier.closeWriteChannel();
            QVERIFY2(duplicateVerifier.waitForFinished(30000),
                     qPrintable(QStringLiteral("The duplicate-artifact verifier did not finish: %1")
                                    .arg(QString::fromUtf8(duplicateVerifier.readAllStandardError()))));
            QCOMPARE(duplicateVerifier.exitStatus(), QProcess::NormalExit);
            QCOMPARE(duplicateVerifier.exitCode(), 2);
            QJsonParseError duplicateParseError;
            const QJsonObject duplicateResult = QJsonDocument::fromJson(
                duplicateVerifier.readAllStandardOutput(), &duplicateParseError).object();
            QCOMPARE(duplicateParseError.error, QJsonParseError::NoError);
            QCOMPARE(duplicateResult.value(QStringLiteral("code")).toString(),
                     QStringLiteral("INVALID_REQUEST"));
        }

        QJsonObject mismatchedAttemptIdentity = expectedCaptureIdentity;
        mismatchedAttemptIdentity.insert(QStringLiteral("trustedExperimentAttemptId"), QStringLiteral("attempt-other"));
        const QJsonObject attemptMismatch = manager.trustedResultVerification(jobId,
            latestSolveIdentity.value(QStringLiteral("artifactId")).toString(),
            childEvidence.value(QStringLiteral("resultManifestSha256")).toString(),
            durableExperiment.value(QStringLiteral("candidateProblemSpecInput")).toObject(),
            mismatchedAttemptIdentity);
        QVERIFY(!attemptMismatch.value(QStringLiteral("valid")).toBool());
        QCOMPARE(attemptMismatch.value(QStringLiteral("code")).toString(), QStringLiteral("CAPTURE_IDENTITY_MISMATCH"));

        QJsonObject mismatchedCaptureIdentity = expectedCaptureIdentity;
        mismatchedCaptureIdentity.insert(QStringLiteral("inputPackageSha256"), QString(64, QLatin1Char('0')));
        const QJsonObject mismatch = manager.trustedResultVerification(jobId,
            latestSolveIdentity.value(QStringLiteral("artifactId")).toString(),
            childEvidence.value(QStringLiteral("resultManifestSha256")).toString(),
            durableExperiment.value(QStringLiteral("candidateProblemSpecInput")).toObject(),
            mismatchedCaptureIdentity);
        QVERIFY(!mismatch.value(QStringLiteral("valid")).toBool());
        QCOMPARE(mismatch.value(QStringLiteral("code")).toString(), QStringLiteral("CAPTURE_IDENTITY_MISMATCH"));
        QVERIFY(!mismatch.contains(QStringLiteral("verificationReport")));

        QJsonObject missingJobIdentity = expectedCaptureIdentity;
        missingJobIdentity.insert(QStringLiteral("jobId"), QStringLiteral("missing-evidence-job"));
        const QJsonObject missingJob = manager.trustedResultVerification(QStringLiteral("missing-evidence-job"),
            latestSolveIdentity.value(QStringLiteral("artifactId")).toString(),
            childEvidence.value(QStringLiteral("resultManifestSha256")).toString(),
            durableExperiment.value(QStringLiteral("candidateProblemSpecInput")).toObject(),
            missingJobIdentity);
        QVERIFY(!missingJob.value(QStringLiteral("valid")).toBool());
        QCOMPARE(missingJob.value(QStringLiteral("code")).toString(), QStringLiteral("UNKNOWN_NATIVE_JOB"));

        // The durable service checks authorization integrity; the Host snapshot also
        // rejects a mismatched solve identity joined against its report and child.
        const QString experimentRecordPath = QDir(configuredDataRoot)
            .filePath(QStringLiteral("trusted-experiments/%1.json").arg(experimentId));
        QFile experimentRecordFile(experimentRecordPath);
        QVERIFY(experimentRecordFile.open(QIODevice::ReadOnly));
        const QByteArray originalExperimentRecord = experimentRecordFile.readAll();
        experimentRecordFile.close();
        bool experimentRecordRestored = false;
        auto restoreExperimentRecord = qScopeGuard([&] {
            if (experimentRecordRestored) return;
            QSaveFile restoreFile(experimentRecordPath);
            if (restoreFile.open(QIODevice::WriteOnly)
                && restoreFile.write(originalExperimentRecord) == originalExperimentRecord.size())
                restoreFile.commit();
        });
        QJsonParseError recordParseError;
        QJsonObject tamperedRecord = QJsonDocument::fromJson(originalExperimentRecord, &recordParseError).object();
        QCOMPARE(recordParseError.error, QJsonParseError::NoError);
        QJsonObject tamperedIdentity = tamperedRecord.value(QStringLiteral("latestSolveIdentity")).toObject();
        QJsonObject tamperedSourceSnapshot = tamperedIdentity.value(QStringLiteral("sourceSnapshotRef")).toObject();
        tamperedSourceSnapshot.insert(QStringLiteral("modelSha256"), QString(64, QLatin1Char('0')));
        tamperedIdentity.insert(QStringLiteral("sourceSnapshotRef"), tamperedSourceSnapshot);
        tamperedRecord.insert(QStringLiteral("latestSolveIdentity"), tamperedIdentity);
        QSaveFile tamperFile(experimentRecordPath);
        QVERIFY(tamperFile.open(QIODevice::WriteOnly));
        const QByteArray tamperedRecordBytes = QJsonDocument(tamperedRecord).toJson(QJsonDocument::Compact);
        QCOMPARE(tamperFile.write(tamperedRecordBytes), qint64(tamperedRecordBytes.size()));
        QVERIFY(tamperFile.commit());
        QVERIFY(host->evaluationEvidenceSnapshot(experimentId).isEmpty());
        QSaveFile restoreNow(experimentRecordPath);
        QVERIFY(restoreNow.open(QIODevice::WriteOnly));
        QCOMPARE(restoreNow.write(originalExperimentRecord), qint64(originalExperimentRecord.size()));
        QVERIFY(restoreNow.commit());
        experimentRecordRestored = true;

        QJsonObject tamperedReportRecord = QJsonDocument::fromJson(originalExperimentRecord).object();
        QJsonObject tamperedReport = tamperedReportRecord.value(QStringLiteral("verificationReport")).toObject();
        QJsonObject tamperedMetrics = tamperedReport.value(QStringLiteral("metrics")).toObject();
        QJsonObject tamperedIdentityMetric = tamperedMetrics.value(QStringLiteral("identity")).toObject();
        tamperedIdentityMetric.insert(QStringLiteral("reason"), QStringLiteral("forged report body with unchanged identity"));
        tamperedMetrics.insert(QStringLiteral("identity"), tamperedIdentityMetric);
        tamperedReport.insert(QStringLiteral("metrics"), tamperedMetrics);
        QCOMPARE(tamperedReport.value(QStringLiteral("identity")).toObject(), persistedReport.value(QStringLiteral("identity")).toObject());
        tamperedReportRecord.insert(QStringLiteral("verificationReport"), tamperedReport);
        bool reportRecordRestored = false;
        auto restoreReportRecord = qScopeGuard([&] {
            if (reportRecordRestored) return;
            QSaveFile restoreFile(experimentRecordPath);
            if (restoreFile.open(QIODevice::WriteOnly)
                && restoreFile.write(originalExperimentRecord) == originalExperimentRecord.size())
                restoreFile.commit();
        });
        QSaveFile tamperReportFile(experimentRecordPath);
        QVERIFY(tamperReportFile.open(QIODevice::WriteOnly));
        const QByteArray tamperedReportBytes = QJsonDocument(tamperedReportRecord).toJson(QJsonDocument::Compact);
        QCOMPARE(tamperReportFile.write(tamperedReportBytes), qint64(tamperedReportBytes.size()));
        QVERIFY(tamperReportFile.commit());
        QVERIFY(host->evaluationEvidenceSnapshot(experimentId).isEmpty());
        QSaveFile restoreReportNow(experimentRecordPath);
        QVERIFY(restoreReportNow.open(QIODevice::WriteOnly));
        QCOMPARE(restoreReportNow.write(originalExperimentRecord), qint64(originalExperimentRecord.size()));
        QVERIFY(restoreReportNow.commit());
        reportRecordRestored = true;

        QJsonObject forgedRepairRecord = QJsonDocument::fromJson(originalExperimentRecord).object();
        QJsonObject forgedCandidateInput = forgedRepairRecord.value(QStringLiteral("candidateProblemSpecInput")).toObject();
        QJsonObject forgedPrecision = forgedCandidateInput.value(QStringLiteral("precision")).toObject();
        forgedPrecision.insert(QStringLiteral("displacementRelative"), 0.02);
        forgedCandidateInput.insert(QStringLiteral("precision"), forgedPrecision);
        const auto forgedCandidateValidation = RTrustedMechanicsService::validate(forgedCandidateInput);
        QVERIFY2(forgedCandidateValidation.valid, qPrintable(QJsonDocument(forgedCandidateValidation.normalizedSpec)
            .toJson(QJsonDocument::Compact)));
        const QJsonObject forgedCandidateSpec = forgedCandidateValidation.normalizedSpec;
        const QString forgedNumericalDigest = RTrustedMechanicsService::summaryDigest(
            RTrustedMechanicsService::numericalSummary(forgedCandidateSpec));
        forgedRepairRecord.insert(QStringLiteral("candidateProblemSpecInput"), forgedCandidateInput);
        forgedRepairRecord.insert(QStringLiteral("candidateProblemSpec"), forgedCandidateSpec);
        forgedRepairRecord.insert(QStringLiteral("candidateNumericalConfigDigest"), forgedNumericalDigest);
        QJsonObject forgedCounters = forgedRepairRecord.value(QStringLiteral("counters")).toObject();
        forgedCounters.insert(QStringLiteral("repairs"), 1);
        forgedRepairRecord.insert(QStringLiteral("counters"), forgedCounters);
        QJsonArray forgedHistory = forgedRepairRecord.value(QStringLiteral("history")).toArray();
        forgedHistory.append(QJsonObject{{QStringLiteral("atUtc"), QStringLiteral("2026-10-24T00:00:00.000Z")},
            {QStringLiteral("event"), QStringLiteral("repair-verified")},
            {QStringLiteral("from"), QStringLiteral("diagnosing")},
            {QStringLiteral("to"), QStringLiteral("diagnosing")}});
        forgedRepairRecord.insert(QStringLiteral("history"), forgedHistory);
        QJsonArray forgedAttempts = forgedRepairRecord.value(QStringLiteral("attempts")).toArray();
        int forgedAttemptMatches = 0;
        for (int i = 0; i < forgedAttempts.size(); ++i) {
            QJsonObject attempt = forgedAttempts.at(i).toObject();
            if (attempt.value(QStringLiteral("attemptId")).toString() != evidenceAttemptId) continue;
            ++forgedAttemptMatches;
            attempt.insert(QStringLiteral("numericalConfigDigest"), forgedNumericalDigest);
            forgedAttempts.replace(i, attempt);
        }
        QCOMPARE(forgedAttemptMatches, 1);
        forgedRepairRecord.insert(QStringLiteral("attempts"), forgedAttempts);
        const QJsonObject authorizationPolicy = forgedRepairRecord.value(QStringLiteral("authorization")).toObject()
            .value(QStringLiteral("experimentPolicy")).toObject();
        const bool includeConvergenceMeasurements =
            !authorizationPolicy.value(QStringLiteral("convergenceStudy")).toObject().isEmpty();
        const QJsonObject forgedReport = RTrustedVerificationService::verify(forgedCandidateInput,
            latestSolveIdentity, childEvidence.value(QStringLiteral("resultManifestRef")).toString(),
            childEvidence.value(QStringLiteral("resultManifestSha256")).toString(), includeConvergenceMeasurements);
        QVERIFY2(forgedReport.value(QStringLiteral("passed")).toBool(),
                 qPrintable(QJsonDocument(forgedReport).toJson(QJsonDocument::Compact)));
        const QJsonObject forgedReportIdentity = forgedReport.value(QStringLiteral("identity")).toObject();
        for (const QString &key : {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                                   QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256"),
                                   QStringLiteral("sourceSnapshotRef")})
            QCOMPARE(forgedReportIdentity.value(key), latestSolveIdentity.value(key));
        QCOMPARE(forgedReportIdentity.value(QStringLiteral("manifestSha256")),
                 childEvidence.value(QStringLiteral("resultManifestSha256")));
        forgedRepairRecord.insert(QStringLiteral("verificationReport"), forgedReport);
        forgedRepairRecord.insert(QStringLiteral("verificationPassed"), forgedReport.value(QStringLiteral("passed")));
        QVERIFY(experimentStore.query(experimentId).ok);
        bool forgedRepairRecordRestored = false;
        auto restoreForgedRepairRecord = qScopeGuard([&] {
            if (forgedRepairRecordRestored) return;
            QSaveFile restoreFile(experimentRecordPath);
            if (restoreFile.open(QIODevice::WriteOnly)
                && restoreFile.write(originalExperimentRecord) == originalExperimentRecord.size())
                restoreFile.commit();
        });
        QSaveFile forgedRepairFile(experimentRecordPath);
        QVERIFY(forgedRepairFile.open(QIODevice::WriteOnly));
        const QByteArray forgedRepairRecordBytes = QJsonDocument(forgedRepairRecord).toJson(QJsonDocument::Compact);
        QCOMPARE(forgedRepairFile.write(forgedRepairRecordBytes), qint64(forgedRepairRecordBytes.size()));
        QVERIFY(forgedRepairFile.commit());
        QVERIFY(host->evaluationEvidenceSnapshot(experimentId).isEmpty());
        QSaveFile restoreForgedRepairNow(experimentRecordPath);
        QVERIFY(restoreForgedRepairNow.open(QIODevice::WriteOnly));
        QCOMPARE(restoreForgedRepairNow.write(originalExperimentRecord), qint64(originalExperimentRecord.size()));
        QVERIFY(restoreForgedRepairNow.commit());
        forgedRepairRecordRestored = true;

        const QByteArray reportSummaryJson = QJsonDocument(reportSummary).toJson(QJsonDocument::Compact);
        QVERIFY(!reportSummaryJson.contains("sourceSnapshotRef"));
        QVERIFY(!reportSummaryJson.contains("manifestPath"));
        QVERIFY(!reportSummaryJson.contains("solverLog"));
        QVERIFY(!reportSummary.contains(QStringLiteral("latestSolveIdentity")));
        QVERIFY(!reportSummary.contains(QStringLiteral("verificationReportSha256")));
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);

        const auto cancelPreview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), specification},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                                                        {QStringLiteral("repairs"), 0},
                                                        {QStringLiteral("modelTurns"), 1}}}});
        QCOMPARE(cancelPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString cancelPreviewId = cancelPreview.value(QStringLiteral("result")).toObject()
                                            .value(QStringLiteral("previewId")).toString();
        const QString cancelExperimentId = cancelPreview.value(QStringLiteral("result")).toObject()
                                               .value(QStringLiteral("experimentId")).toString();
        QVERIFY(host->approvePreview(cancelPreviewId));
        const auto cancelSubmitted = commandExecute(session, cancelPreviewId,
            QStringLiteral("host-trusted-experiment-cancel-active"));
        QCOMPARE(cancelSubmitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString cancelJobId = cancelSubmitted.value(QStringLiteral("result")).toObject()
                                        .value(QStringLiteral("jobId")).toString();
        QVERIFY(!cancelJobId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(cancelJobId).value(QStringLiteral("processStarted")).toBool(),
                                 15000);
        const QString experimentDataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
            ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        RTrustedExperimentService experiments(QDir(experimentDataRoot).filePath(QStringLiteral("trusted-experiments")));
        const QJsonObject activeBeforeCancel = experiments.query(cancelExperimentId).data
            .value(QStringLiteral("experiment")).toObject();
        QCOMPARE(activeBeforeCancel.value(QStringLiteral("state")).toString(), QStringLiteral("solving"));
        QCOMPARE(activeBeforeCancel.value(QStringLiteral("activeAttempt")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("running"));
        const auto cancelled = host->handle(hostRequest(QStringLiteral("experiment.cancel"),
            {{QStringLiteral("experimentId"), cancelExperimentId}}));
        QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject cancelledSummary = cancelled.value(QStringLiteral("result")).toObject();
        QVERIFY(cancelledSummary.value(QStringLiteral("cancellationPersisted")).toBool());
        QCOMPARE(cancelledSummary.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        const QJsonObject cancelledSource = cancelledSummary.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        QCOMPARE(cancelledSource.value(QStringLiteral("documentId")).toString(), sourceStamp.first);
        QCOMPARE(cancelledSource.value(QStringLiteral("sessionEpoch")).toString(), host->sessionEpoch());
        QCOMPARE(cancelledSource.value(QStringLiteral("revision")).toString(), QString::number(sourceStamp.second));
        QCOMPARE(cancelledSummary
                     .value(QStringLiteral("childCancellationRequestFailed")).toBool(), false);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(cancelJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("cancelled"), 30000);
        const QJsonObject stoppedChild = manager.jobQuery(cancelJobId);
        QVERIFY(stoppedChild.value(QStringLiteral("processStarted")).toBool());
        QVERIFY(stoppedChild.value(QStringLiteral("processExited")).toBool());
        QVERIFY(stoppedChild.value(QStringLiteral("cancellationRequested")).toBool());
        const QJsonObject cancelledRecord = experiments.query(cancelExperimentId).data
            .value(QStringLiteral("experiment")).toObject();
        QCOMPARE(cancelledRecord.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QVERIFY(cancelledRecord.value(QStringLiteral("activeAttempt")).toObject().isEmpty());
        const QJsonObject cancelledAttempt = cancelledRecord.value(QStringLiteral("attempts")).toArray().first().toObject();
        QCOMPARE(cancelledAttempt.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
        QVERIFY(!cancelledRecord.contains(QStringLiteral("verificationReport")));
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), jobsBefore + 2);
    }

    void trustedArtifactContainmentRespectsFilesystemCaseSensitivity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString rootPath = QDir(directory.path()).filePath(QStringLiteral("TrustedRuns"));
        const QString caseVariantSibling = QDir(directory.path()).filePath(QStringLiteral("trustedruns"));
        QVERIFY(QDir().mkpath(rootPath));
        QVERIFY(QDir().mkpath(caseVariantSibling));

        QFile inRoot(QDir(rootPath).filePath(QStringLiteral("candidate.json")));
        QVERIFY(inRoot.open(QIODevice::WriteOnly));
        QCOMPARE(inRoot.write("{}"), qint64(2));
        inRoot.close();
        QFile inSibling(QDir(caseVariantSibling).filePath(QStringLiteral("candidate.json")));
        QVERIFY(inSibling.open(QIODevice::WriteOnly));
        QCOMPARE(inSibling.write("{}"), qint64(2));
        inSibling.close();

        QString canonicalPath;
        QVERIFY(TrustedArtifactPaths::canonicalFileUnderRoot(inRoot.fileName(), rootPath, &canonicalPath));
        QVERIFY(!canonicalPath.isEmpty());
        const bool caseVariantResolvesToSameDirectory =
            QDir(rootPath).canonicalPath() == QDir(caseVariantSibling).canonicalPath();
        QCOMPARE(TrustedArtifactPaths::canonicalFileUnderRoot(inSibling.fileName(), rootPath),
                 caseVariantResolvesToSameDirectory);

        QString openedCanonicalPath;
        QString openError;
        auto openedFile = TrustedArtifactPaths::openFileUnderRoot(
            inRoot.fileName(), rootPath, &openedCanonicalPath, &openError, true);
        QVERIFY2(openedFile != nullptr, qPrintable(openError));
        QVERIFY(!openedCanonicalPath.isEmpty());
        const QByteArray stableSnapshot = TrustedArtifactPaths::readOpenedFileSnapshot(*openedFile, 1024, &openError);
        QVERIFY2(!stableSnapshot.isEmpty(), qPrintable(openError));
        QCOMPARE(stableSnapshot, QByteArray("{}"));
#if defined(Q_OS_WIN)
        QVERIFY(!QFile::remove(inRoot.fileName()));
#endif
        openedFile.reset();
        QVERIFY(QFile::remove(inRoot.fileName()));
        QFile replacement(inRoot.fileName());
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QCOMPARE(replacement.write("replacement"), qint64(11));
        replacement.close();
        QCOMPARE(stableSnapshot, QByteArray("{}"));
        QFile replacedPath(inRoot.fileName());
        QVERIFY(replacedPath.open(QIODevice::ReadOnly));
        QCOMPARE(replacedPath.readAll(), QByteArray("replacement"));

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
        const QString fifoPath = QDir(rootPath).filePath(QStringLiteral("candidate.pipe"));
        const QByteArray nativeFifoPath = QFile::encodeName(fifoPath);
        QCOMPARE(::mkfifo(nativeFifoPath.constData(), 0600), 0);
        auto fifo = TrustedArtifactPaths::openRegularFileNoFollow(fifoPath, &openError);
        QVERIFY(!fifo);
#endif
    }

    void verifiedModelParserUsesHashBoundSnapshotAfterPathReplacement()
    {
        for (const bool binary : {true, false})
        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString extension = RModel::getDefaultFileExtension(binary);
            const QString originalPath = QDir(directory.path()).filePath(QStringLiteral("candidate.") + extension);
            const QString replacementPath = QDir(directory.path()).filePath(QStringLiteral("replacement.") + extension);

            Model original = modalFixture(QStringLiteral("Verified held model"));
            original.write(originalPath, false);
            QVERIFY(QFileInfo::exists(originalPath));
            Model replacement = stressLineFixture(QStringLiteral("Path replacement model"));
            replacement.write(replacementPath, false);
            QVERIFY(QFileInfo::exists(replacementPath));

            QString openError;
            auto verifiedFile = TrustedArtifactPaths::openFileUnderRoot(
                originalPath, directory.path(), nullptr, &openError, true);
            QVERIFY2(verifiedFile != nullptr, qPrintable(openError));
            const QByteArray originalSnapshot = TrustedArtifactPaths::readOpenedFileSnapshot(
                *verifiedFile, 32 * 1024 * 1024, &openError);
            QVERIFY2(!originalSnapshot.isEmpty(), qPrintable(openError));
            const QByteArray originalSnapshotSha = QCryptographicHash::hash(
                originalSnapshot, QCryptographicHash::Sha256);
            verifiedFile.reset();
            QVERIFY(QFile::remove(originalPath));
            QVERIFY(QFile::rename(replacementPath, originalPath));

            QTemporaryDir privateSnapshotDirectory(
                QDir(directory.path()).filePath(QStringLiteral("private-snapshot-XXXXXX")));
            QVERIFY(privateSnapshotDirectory.isValid());
            const QFileDevice::Permissions privateDirectoryPermissions = QFileDevice::ReadOwner |
                QFileDevice::WriteOwner | QFileDevice::ExeOwner;
            const QFileDevice::Permissions privateFilePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
            QVERIFY(QFile::setPermissions(privateSnapshotDirectory.path(), privateDirectoryPermissions));
            const QString privateSnapshotPath = QDir(privateSnapshotDirectory.path()).filePath(
                QStringLiteral("verified-input.") + extension);
            QFile privateWriter(privateSnapshotPath);
            QVERIFY(privateWriter.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(privateWriter.write(originalSnapshot), qint64(originalSnapshot.size()));
            QVERIFY(privateWriter.flush());
            QCOMPARE(privateWriter.size(), qint64(originalSnapshot.size()));
            privateWriter.close();
            QVERIFY(QFile::setPermissions(privateSnapshotPath, privateFilePermissions));

            auto privateFile = TrustedArtifactPaths::openFileUnderRoot(
                privateSnapshotPath, privateSnapshotDirectory.path(), nullptr, &openError, true);
            QVERIFY2(privateFile != nullptr, qPrintable(openError));
            const QByteArray privateSnapshotBytes = TrustedArtifactPaths::readOpenedFileSnapshot(
                *privateFile, 32 * 1024 * 1024, &openError);
            QVERIFY2(!privateSnapshotBytes.isEmpty(), qPrintable(openError));
            QCOMPARE(privateSnapshotBytes, originalSnapshot);
            QCOMPARE(QCryptographicHash::hash(privateSnapshotBytes, QCryptographicHash::Sha256), originalSnapshotSha);

            Model parsed;
            parsed.readFromVerifiedFile(privateSnapshotPath, *privateFile);
            QCOMPARE(parsed.getName(), QStringLiteral("Verified held model"));
            QCOMPARE(parsed.getProblemTaskTree().getProblemTypeMask(), R_PROBLEM_STRESS_MODAL);

            const QByteArray verifiedBytes = TrustedArtifactPaths::readOpenedFileSnapshot(
                *privateFile, 32 * 1024 * 1024, &openError);
            QVERIFY2(!verifiedBytes.isEmpty(), qPrintable(openError));
            QCOMPARE(verifiedBytes, originalSnapshot);
            QCOMPARE(QCryptographicHash::hash(verifiedBytes, QCryptographicHash::Sha256), originalSnapshotSha);
            Model pathModel;
            pathModel.read(originalPath);
            QCOMPARE(pathModel.getName(), QStringLiteral("Path replacement model"));
        }
    }

    void verifiedModelReadRejectsNativeLinksAndPreservesTargetOnFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString extension = RModel::getDefaultFileExtension(true);
        const QString linkPath = QDir(directory.path()).filePath(QStringLiteral("candidate-link.") + extension);
        RModel::writeLink(linkPath, QStringLiteral("missing-target.") + extension);

        QString openError;
        auto linkFile = TrustedArtifactPaths::openFileUnderRoot(
            linkPath, directory.path(), nullptr, &openError, true);
        QVERIFY2(linkFile != nullptr, qPrintable(openError));
        Model target = modalFixture(QStringLiteral("Target remains unchanged"));
        const QString targetFingerprint = RModelCommandService::stateFingerprint(target);
        bool linkRejected = false;
        try
        {
            target.readFromVerifiedFile(linkPath, *linkFile);
        }
        catch (const RError &)
        {
            linkRejected = true;
        }
        QVERIFY(linkRejected);
        QCOMPARE(RModelCommandService::stateFingerprint(target), targetFingerprint);
        QCOMPARE(target.getName(), QStringLiteral("Target remains unchanged"));
        QCOMPARE(target.getProblemTaskTree().getProblemTypeMask(), R_PROBLEM_STRESS_MODAL);

        const QString invalidPath = QDir(directory.path()).filePath(QStringLiteral("invalid.") + extension);
        RSaveFile invalid(invalidPath, RSaveFile::BINARY);
        invalid.setVersion(RModel::version);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        const RFileHeader wrongTypeHeader(R_FILE_TYPE_MATERIAL, RModel::version);
        RFileIO::writeBinary(invalid, wrongTypeHeader);
        QVERIFY(invalid.commit());
        auto invalidFile = TrustedArtifactPaths::openFileUnderRoot(
            invalidPath, directory.path(), nullptr, &openError, true);
        QVERIFY2(invalidFile != nullptr, qPrintable(openError));
        bool invalidRejected = false;
        try
        {
            target.readFromVerifiedFile(invalidPath, *invalidFile);
        }
        catch (const RError &)
        {
            invalidRejected = true;
        }
        QVERIFY(invalidRejected);
        QCOMPARE(RModelCommandService::stateFingerprint(target), targetFingerprint);
        QCOMPARE(target.getName(), QStringLiteral("Target remains unchanged"));
        QCOMPARE(target.getProblemTaskTree().getProblemTypeMask(), R_PROBLEM_STRESS_MODAL);

        const QString truncatedPath = QDir(directory.path()).filePath(QStringLiteral("truncated.") + extension);
        Model completeModel = modalFixture(QStringLiteral("Complete source model"));
        completeModel.write(truncatedPath, false);
        QFile truncatedWriter(truncatedPath);
        QVERIFY(truncatedWriter.open(QIODevice::ReadWrite));
        QVERIFY(truncatedWriter.size() > 1);
        QVERIFY(truncatedWriter.resize(truncatedWriter.size() - 1));
        truncatedWriter.close();

        auto truncatedFile = TrustedArtifactPaths::openFileUnderRoot(
            truncatedPath, directory.path(), nullptr, &openError, true);
        QVERIFY2(truncatedFile != nullptr, qPrintable(openError));
        bool truncatedRejected = false;
        try
        {
            target.readFromVerifiedFile(truncatedPath, *truncatedFile);
        }
        catch (const RError &)
        {
            truncatedRejected = true;
        }
        QVERIFY(truncatedRejected);
        QCOMPARE(RModelCommandService::stateFingerprint(target), targetFingerprint);
        QCOMPARE(target.getName(), QStringLiteral("Target remains unchanged"));
        QCOMPARE(target.getProblemTaskTree().getProblemTypeMask(), R_PROBLEM_STRESS_MODAL);

        const QString truncatedStringPath = QDir(directory.path()).filePath(QStringLiteral("truncated-string.bin"));
        QFile truncatedStringWriter(truncatedStringPath);
        QVERIFY(truncatedStringWriter.open(QIODevice::WriteOnly));
        const int declaredStringBytes = 4;
        QCOMPARE(truncatedStringWriter.write(reinterpret_cast<const char*>(&declaredStringBytes),
                                             qint64(sizeof(declaredStringBytes))),
                 qint64(sizeof(declaredStringBytes)));
        QCOMPARE(truncatedStringWriter.write("ab",2),qint64(2));
        truncatedStringWriter.close();

        RFile truncatedStringFile(truncatedStringPath,RFile::BINARY);
        QVERIFY(truncatedStringFile.open(QIODevice::ReadOnly));
        QString preservedString = QStringLiteral("preserve on failed read");
        bool truncatedStringRejected = false;
        try
        {
            RFileIO::readBinary(truncatedStringFile,preservedString);
        }
        catch (const RError &)
        {
            truncatedStringRejected = true;
        }
        QVERIFY(truncatedStringRejected);
        QCOMPARE(preservedString,QStringLiteral("preserve on failed read"));
    }

    void trustedExperimentFailurePersistsJobBoundDiagnosticPackage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray previousFailureLogMode = qgetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        const bool hadPreviousFailureLogMode = qEnvironmentVariableIsSet("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        const auto restoreFailureLogMode = qScopeGuard([&] {
            if (hadPreviousFailureLogMode)
                qputenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE", previousFailureLogMode);
            else
                qunsetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        });
        qunsetenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE");
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-failure-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);

        const QString failingExecutable = QCoreApplication::applicationFilePath();
        QVERIFY2(!failingExecutable.isEmpty(), "The test executable is required for deterministic failure injection.");
        auto *settings = Application::instance()->getApplicationSettings();
        const QString originalSolverPath = settings->getSolverPath();
        const auto restoreSolverPath = qScopeGuard([&] { settings->setSolverPath(originalSolverPath); });
        settings->setSolverPath(failingExecutable);

        const auto preview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), trustedExperimentRodSpec()},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                                                        {QStringLiteral("repairs"), 0},
                                                        {QStringLiteral("modelTurns"), 1}}}});
        QVERIFY2(preview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        const QJsonObject plan = preview.value(QStringLiteral("result")).toObject();
        const QString previewId = plan.value(QStringLiteral("previewId")).toString();
        const QString experimentId = plan.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!previewId.isEmpty());
        QVERIFY(!experimentId.isEmpty());
        QVERIFY(host->approvePreview(previewId));
        const QJsonObject submitted = commandExecute(session, previewId,
            QStringLiteral("host-trusted-failure-run-1"));
        QCOMPARE(submitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString jobId = submitted.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());
        settings->setSolverPath(originalSolverPath);

        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        const QJsonObject job = manager.jobQuery(jobId);
        QVERIFY(job.value(QStringLiteral("found")).toBool());
        const QJsonObject boundDiagnosis = job.value(QStringLiteral("trustedFailureDiagnosis")).toObject();
        QVERIFY2(!boundDiagnosis.isEmpty(), qPrintable(QJsonDocument(job).toJson(QJsonDocument::Compact)));
        QCOMPARE(boundDiagnosis.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QVERIFY2(boundDiagnosis.value(QStringLiteral("status")).toString() == QStringLiteral("complete"),
                 qPrintable(QJsonDocument(boundDiagnosis).toJson(QJsonDocument::Compact)));

        const QJsonObject package = boundDiagnosis.value(QStringLiteral("package")).toObject();
        QVERIFY(!package.isEmpty());
        QCOMPARE(package.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(package.value(QStringLiteral("experimentId")).toString(), experimentId);
        QCOMPARE(package.value(QStringLiteral("sourceId")).toString(), sourceStamp.first);
        QCOMPARE(package.value(QStringLiteral("modelRevision")).toString(), QString::number(sourceStamp.second));
        QCOMPARE(package.value(QStringLiteral("attemptId")).toString(), job.value("trustedExperimentAttemptId").toString());
        QCOMPARE(package.value(QStringLiteral("inputSnapshotSha256")).toString(), job.value("inputSnapshotSha256").toString());
        QCOMPARE(package.value(QStringLiteral("inputPackageSha256")).toString(), job.value("inputPackageSha256").toString());
        QCOMPARE(package.value(QStringLiteral("modelSnapshotAvailable")).toBool(), true);
        const QJsonObject logCapture = package.value(QStringLiteral("solverLogCapture")).toObject();
        QCOMPARE(logCapture.value(QStringLiteral("source")).toString(), QStringLiteral("solver-log-tail"));
        QVERIFY(logCapture.value(QStringLiteral("fileSizeBytes")).toDouble() > 0.0);
        QCOMPARE(logCapture.value(QStringLiteral("captureError")).toString(), QString());
        QVERIFY(!logCapture.value(QStringLiteral("artifactRef")).toString().isEmpty());
        const QString registeredLogPath = job.value(QStringLiteral("logArtifactRefs")).toObject()
            .value(QStringLiteral("solverLog")).toString();
        QVERIFY(!registeredLogPath.isEmpty());
        QCOMPARE(QFileInfo(logCapture.value(QStringLiteral("artifactRef")).toString()).canonicalFilePath(),
                 QFileInfo(registeredLogPath).canonicalFilePath());
        const QByteArray packageBytes = QJsonDocument(package).toJson(QJsonDocument::Compact);
        QCOMPARE(boundDiagnosis.value(QStringLiteral("packageSha256")).toString(),
                 QString::fromLatin1(QCryptographicHash::hash(packageBytes, QCryptographicHash::Sha256).toHex()));

        const QJsonObject failurePackage = package.value(QStringLiteral("failurePackage")).toObject();
        QVERIFY(!failurePackage.value(QStringLiteral("solverLog")).toString().trimmed().isEmpty());
        QFile capturedSolverLog(logCapture.value(QStringLiteral("artifactRef")).toString());
        QVERIFY(capturedSolverLog.open(QIODevice::ReadOnly));
        const QByteArray capturedSolverLogBytes = capturedSolverLog.readAll();
        QVERIFY(capturedSolverLogBytes.contains("TRUSTED_FAILURE_LOG_SENTINEL"));
        QCOMPARE(failurePackage.value(QStringLiteral("solverLog")).toString(), QString::fromUtf8(capturedSolverLogBytes));
        QCOMPARE(failurePackage.value(QStringLiteral("causeDetermined")).toBool(), false);
        QCOMPARE(failurePackage.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(failurePackage.value(QStringLiteral("sourceId")).toString(), sourceStamp.first);
        QCOMPARE(failurePackage.value(QStringLiteral("modelRevision")).toString(), QString::number(sourceStamp.second));
        QCOMPARE(failurePackage.value(QStringLiteral("modelSnapshotSha256")).toString(),
                 RTrustedMechanicsService::summaryDigest(failurePackage.value(QStringLiteral("modelSnapshot")).toObject()));
        QCOMPARE(failurePackage.value(QStringLiteral("solverLogSha256")).toString(),
                 QString::fromLatin1(QCryptographicHash::hash(
                     failurePackage.value(QStringLiteral("solverLog")).toString().toUtf8(),
                     QCryptographicHash::Sha256).toHex()));
        bool sawUnknownFailure = false;
        for (const QJsonValue &issue : package.value(QStringLiteral("diagnosticReport")).toObject()
                 .value(QStringLiteral("issues")).toArray())
            sawUnknownFailure = sawUnknownFailure || issue.toObject().value(QStringLiteral("code")).toString()
                == QStringLiteral("SOLVER_FAILURE_UNCLASSIFIED");
        QVERIFY(sawUnknownFailure);

        QFile recordFile(job.value(QStringLiteral("nativeJobRecordRef")).toString());
        QVERIFY(recordFile.open(QIODevice::ReadOnly));
        const QJsonObject diskRecord = QJsonDocument::fromJson(recordFile.readAll()).object();
        QCOMPARE(diskRecord.value(QStringLiteral("trustedFailureDiagnosis")).toObject(), boundDiagnosis);
        SolverManager reopened;
        const QJsonObject reopenedJob = reopened.jobQuery(jobId);
        QVERIFY(reopenedJob.value(QStringLiteral("found")).toBool());
        QCOMPARE(reopenedJob.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QCOMPARE(reopenedJob.value(QStringLiteral("trustedExperimentId")).toString(), experimentId);
        const QJsonObject reopenedDiagnosis = reopenedJob.value(QStringLiteral("trustedFailureDiagnosis")).toObject();
        QCOMPARE(reopenedDiagnosis, boundDiagnosis);
        const QJsonObject reopenedPackage = reopenedDiagnosis.value(QStringLiteral("package")).toObject();
        QCOMPARE(reopenedPackage.value(QStringLiteral("jobId")).toString(), jobId);
        QCOMPARE(reopenedPackage.value(QStringLiteral("attemptId")).toString(), package.value(QStringLiteral("attemptId")).toString());
        QCOMPARE(reopenedPackage.value(QStringLiteral("inputSnapshotSha256")).toString(),
                 package.value(QStringLiteral("inputSnapshotSha256")).toString());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);

        const QString configuredDataRoot = settings->getDataDir().isEmpty() ? QDir::tempPath() : settings->getDataDir();
        RTrustedExperimentService experiments(QDir(configuredDataRoot).filePath(QStringLiteral("trusted-experiments")));
        const auto experiment = experiments.query(experimentId);
        QVERIFY(experiment.ok);
        QCOMPARE(experiment.data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("failed"));

        qputenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE", "missing");
        settings->setSolverPath(failingExecutable);
        const auto noLogPreview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), trustedExperimentRodSpec()},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                                                        {QStringLiteral("repairs"), 0},
                                                        {QStringLiteral("modelTurns"), 1}}}});
        QVERIFY2(noLogPreview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(noLogPreview).toJson(QJsonDocument::Compact)));
        const QString noLogPreviewId = noLogPreview.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString();
        QVERIFY(!noLogPreviewId.isEmpty());
        QVERIFY(host->approvePreview(noLogPreviewId));
        const QJsonObject noLogSubmitted = commandExecute(session, noLogPreviewId,
            QStringLiteral("host-trusted-failure-without-log-1"));
        QCOMPARE(noLogSubmitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString noLogJobId = noLogSubmitted.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("jobId")).toString();
        QVERIFY(!noLogJobId.isEmpty());
        settings->setSolverPath(originalSolverPath);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(noLogJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        const QJsonObject noLogJob = manager.jobQuery(noLogJobId);
        const QJsonObject noLogDiagnosis = noLogJob.value(QStringLiteral("trustedFailureDiagnosis")).toObject();
        QVERIFY(!noLogDiagnosis.isEmpty());
        QCOMPARE(noLogDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
        const QJsonObject noLogPackage = noLogDiagnosis.value(QStringLiteral("package")).toObject();
        QVERIFY(!noLogPackage.isEmpty());
        const QJsonObject noLogCapture = noLogPackage.value(QStringLiteral("solverLogCapture")).toObject();
        QCOMPARE(noLogCapture.value(QStringLiteral("source")).toString(), QStringLiteral("terminal-message-fallback"));
        QVERIFY(!noLogPackage.value(QStringLiteral("terminalMessage")).toString().trimmed().isEmpty());
        QCOMPARE(noLogPackage.value(QStringLiteral("processEvidence")).toObject()
                     .value(QStringLiteral("processStarted")).toBool(), true);

        qputenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE", "empty");
        settings->setSolverPath(failingExecutable);
        const auto emptyLogPreview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), trustedExperimentRodSpec()},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                                                        {QStringLiteral("repairs"), 0},
                                                        {QStringLiteral("modelTurns"), 1}}}});
        QVERIFY2(emptyLogPreview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(emptyLogPreview).toJson(QJsonDocument::Compact)));
        const QString emptyLogPreviewId = emptyLogPreview.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString();
        QVERIFY(!emptyLogPreviewId.isEmpty());
        QVERIFY(host->approvePreview(emptyLogPreviewId));
        const QJsonObject emptyLogSubmitted = commandExecute(session, emptyLogPreviewId,
            QStringLiteral("host-trusted-failure-empty-log-1"));
        QCOMPARE(emptyLogSubmitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString emptyLogJobId = emptyLogSubmitted.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("jobId")).toString();
        QVERIFY(!emptyLogJobId.isEmpty());
        settings->setSolverPath(originalSolverPath);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(emptyLogJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        const QJsonObject emptyLogJob = manager.jobQuery(emptyLogJobId);
        const QJsonObject emptyLogDiagnosis = emptyLogJob.value(QStringLiteral("trustedFailureDiagnosis")).toObject();
        QVERIFY(!emptyLogDiagnosis.isEmpty());
        QCOMPARE(emptyLogDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
        const QJsonObject emptyLogPackage = emptyLogDiagnosis.value(QStringLiteral("package")).toObject();
        QVERIFY(!emptyLogPackage.isEmpty());
        const QJsonObject emptyLogCapture = emptyLogPackage.value(QStringLiteral("solverLogCapture")).toObject();
        QCOMPARE(emptyLogCapture.value(QStringLiteral("source")).toString(), QStringLiteral("terminal-message-fallback"));
        QCOMPARE(emptyLogCapture.value(QStringLiteral("fileSizeBytes")).toDouble(), 0.0);
        QVERIFY(!emptyLogCapture.value(QStringLiteral("captureError")).toString().trimmed().isEmpty());
        const QString registeredSolverLogPath = emptyLogJob.value(QStringLiteral("logArtifactRefs")).toObject()
            .value(QStringLiteral("solverLog")).toString();
        QVERIFY(!registeredSolverLogPath.isEmpty());
        QCOMPARE(QFileInfo(emptyLogCapture.value(QStringLiteral("artifactRef")).toString()).canonicalFilePath(),
                 QFileInfo(registeredSolverLogPath).canonicalFilePath());
        const QFileInfo emptySolverLogInfo(registeredSolverLogPath);
        QVERIFY(emptySolverLogInfo.isFile());
        QCOMPARE(emptySolverLogInfo.size(), qint64(0));
        const QJsonObject emptyLogProcessEvidence = emptyLogPackage.value(QStringLiteral("processEvidence")).toObject();
        QCOMPARE(emptyLogProcessEvidence.value(QStringLiteral("processStarted")).toBool(), true);
        QCOMPARE(emptyLogProcessEvidence.value(QStringLiteral("processExited")).toBool(), true);
        QCOMPARE(emptyLogProcessEvidence.value(QStringLiteral("processExitCode")).toInt(), 23);

        qputenv("RANGE_TEST_TRUSTED_FAILURE_LOG_MODE", "unicode-whitespace-tail");
        settings->setSolverPath(failingExecutable);
        const auto whitespaceTailPreview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), trustedExperimentRodSpec()},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                                                        {QStringLiteral("repairs"), 0},
                                                        {QStringLiteral("modelTurns"), 1}}}});
        QVERIFY2(whitespaceTailPreview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(whitespaceTailPreview).toJson(QJsonDocument::Compact)));
        const QString whitespaceTailPreviewId = whitespaceTailPreview.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString();
        QVERIFY(!whitespaceTailPreviewId.isEmpty());
        QVERIFY(host->approvePreview(whitespaceTailPreviewId));
        const QJsonObject whitespaceTailSubmitted = commandExecute(session, whitespaceTailPreviewId,
            QStringLiteral("host-trusted-failure-whitespace-tail-1"));
        QCOMPARE(whitespaceTailSubmitted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString whitespaceTailJobId = whitespaceTailSubmitted.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("jobId")).toString();
        QVERIFY(!whitespaceTailJobId.isEmpty());
        settings->setSolverPath(originalSolverPath);
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(whitespaceTailJobId).value(QStringLiteral("state")).toString(),
                                  QStringLiteral("failed"), 30000);
        const QJsonObject whitespaceTailJob = manager.jobQuery(whitespaceTailJobId);
        const QJsonObject whitespaceTailDiagnosis = whitespaceTailJob
            .value(QStringLiteral("trustedFailureDiagnosis")).toObject();
        QVERIFY(!whitespaceTailDiagnosis.isEmpty());
        QCOMPARE(whitespaceTailDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("incomplete"));
        const QJsonObject whitespaceTailPackage = whitespaceTailDiagnosis.value(QStringLiteral("package")).toObject();
        QVERIFY(!whitespaceTailPackage.isEmpty());
        const QJsonObject whitespaceTailCapture = whitespaceTailPackage
            .value(QStringLiteral("solverLogCapture")).toObject();
        QCOMPARE(whitespaceTailCapture.value(QStringLiteral("source")).toString(),
                 QStringLiteral("terminal-message-fallback"));
        QVERIFY(whitespaceTailCapture.value(QStringLiteral("fileSizeBytes")).toDouble() > 256.0 * 1024.0);
        QCOMPARE(whitespaceTailCapture.value(QStringLiteral("truncated")).toBool(), true);
        QVERIFY(!whitespaceTailCapture.value(QStringLiteral("captureError")).toString().trimmed().isEmpty());
        QVERIFY(!whitespaceTailPackage.value(QStringLiteral("terminalMessage")).toString().trimmed().isEmpty());
    }

    void trustedAnalysisInspectAndDiagnoseBindSourceAndBoundReadback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-analysis-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto stamp = session->modelCommandStamp(modelId);
        const QString initialFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));
        SolverManager &manager = SolverManager::getInstance();
        const auto requestFor = [&](const QString &operation, const QString &basis,
                                    const QJsonObject &spec = QJsonObject{}, bool includeSpec = false,
                                    const double revision = -1.0, const QString &epoch = QString(),
                                    const QString &jobId = QString(),
                                    const QString &requestedDocumentId = QString()) {
            const QString selectedDocumentId = requestedDocumentId.isEmpty() ? documentId : requestedDocumentId;
            const uint selectedModelId = session->findModelByCommandIdentity(selectedDocumentId);
            const auto selectedStamp = selectedModelId == RConstants::eod
                ? stamp : session->modelCommandStamp(selectedModelId);
            QJsonObject parameters{{QStringLiteral("target"),
                QJsonObject{{QStringLiteral("documentId"), selectedDocumentId},
                    {QStringLiteral("sessionEpoch"), epoch.isEmpty() ? host->sessionEpoch() : epoch}}},
                {QStringLiteral("expectedDocumentRevision"), revision < 0.0 ? double(selectedStamp.second) : revision},
                {QStringLiteral("basis"), basis}};
            if (includeSpec) parameters.insert(QStringLiteral("problemSpec"), spec);
            if (!jobId.isEmpty()) parameters.insert(QStringLiteral("jobId"), jobId);
            return host->handle(hostRequest(operation, parameters));
        };

        const QJsonObject documentRead = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"));
        QVERIFY2(documentRead.value(QStringLiteral("status")).toString() == QStringLiteral("ok"),
                 qPrintable(QJsonDocument(documentRead).toJson(QJsonDocument::Compact)));
        const QJsonObject documentResult = documentRead.value(QStringLiteral("result")).toObject();
        QCOMPARE(documentResult.value(QStringLiteral("basis")).toString(), QStringLiteral("document"));
        QCOMPARE(documentResult.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), documentId);
        QVERIFY(documentResult.value(QStringLiteral("mesh")).toObject().value(QStringLiteral("nodeCount")).toInt() > 0);
        const QJsonObject documentModel = documentResult.value(QStringLiteral("model")).toObject();
        QCOMPARE(documentModel.value(QStringLiteral("fingerprint")).toString(), initialFingerprint);
        QCOMPARE(documentModel.value(QStringLiteral("topology")).toObject(),
                 documentResult.value(QStringLiteral("mesh")).toObject());
        QVERIFY(!documentModel.contains(QStringLiteral("nodes")));
        QVERIFY(!documentModel.contains(QStringLiteral("elements")));
        const QJsonObject documentObserved = documentResult.value(QStringLiteral("observedPhysicalState")).toObject();
        const RModel &documentSourceModel = session->getModel(modelId);
        QCOMPARE(documentObserved.value(QStringLiteral("nodes")).toArray().size(),
                 int(documentSourceModel.getNNodes()));
        QCOMPARE(documentObserved.value(QStringLiteral("elements")).toArray().size(),
                 int(documentSourceModel.getNElements()));
        for (uint index = 0; index < documentSourceModel.getNNodes(); ++index)
        {
            const QJsonObject observedNode = documentObserved.value(QStringLiteral("nodes")).toArray()
                .at(int(index)).toObject();
            QCOMPARE(observedNode.value(QStringLiteral("position")).toInt(), int(index));
            const QJsonArray xyz = observedNode.value(QStringLiteral("xyz_m")).toArray();
            QCOMPARE(xyz.size(), 3);
            QCOMPARE(xyz.at(0).toDouble(), documentSourceModel.getNode(index).getX());
            QCOMPARE(xyz.at(1).toDouble(), documentSourceModel.getNode(index).getY());
            QCOMPARE(xyz.at(2).toDouble(), documentSourceModel.getNode(index).getZ());
        }
        QCOMPARE(documentResult.value(QStringLiteral("observedPhysicalStateDigest")).toString().size(), 64);
        QCOMPARE(documentResult.value(QStringLiteral("observedPhysicalStateDigestBasis")).toString(),
                 QStringLiteral("host-projected-observed-physical-state/v1"));
        QCOMPARE(documentResult.value(QStringLiteral("observedPhysicalStateDigest")).toString(),
                 RTrustedMechanicsService::summaryDigest(documentObserved));
        QCOMPARE(documentResult.value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("no_job"));
        QCOMPARE(documentResult.value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("no_job"));
        QCOMPARE(documentResult.value(QStringLiteral("resultSource")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("no_job"));
        const auto documentDiagnosis = requestFor(QStringLiteral("analysis.diagnose"), QStringLiteral("document"),
                                                  trustedExperimentRodSpec(), true);
        QCOMPARE(documentDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(documentDiagnosis.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("basis")).toString(), QStringLiteral("document"));
        QVERIFY(documentDiagnosis.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("diagnosis")).toObject().contains(QStringLiteral("issues")));

        const auto detachedRead = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("detached"),
                                             trustedExperimentRodSpec(), true);
        QVERIFY2(detachedRead.value(QStringLiteral("status")).toString() == QStringLiteral("ok"),
                 qPrintable(QJsonDocument(detachedRead).toJson(QJsonDocument::Compact)));
        const QJsonObject detachedResult = detachedRead.value(QStringLiteral("result")).toObject();
        QCOMPARE(detachedResult.value(QStringLiteral("basis")).toString(), QStringLiteral("detached"));
        QCOMPARE(detachedResult.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("revision")).toString(), QString::number(stamp.second));
        // The readback counts all model elements, including the two point entities that
        // encode the support and point load in addition to the four rod elements.
        QCOMPARE(detachedResult.value(QStringLiteral("mesh")).toObject().value(QStringLiteral("elementCount")).toInt(), 6);
        QVERIFY(detachedResult.value(QStringLiteral("physicalSummaryDigest")).toString().size() == 64);
        const QJsonObject detachedObserved = detachedResult.value(QStringLiteral("observedPhysicalState")).toObject();
        const QJsonArray detachedNodes = detachedObserved.value(QStringLiteral("nodes")).toArray();
        QCOMPARE(detachedNodes.size(), 5);
        QCOMPARE(detachedNodes.first().toObject().value(QStringLiteral("xyz_m")).toArray(),
                 QJsonArray({0.0, 0.0, 0.0}));
        QCOMPARE(detachedNodes.last().toObject().value(QStringLiteral("xyz_m")).toArray(),
                 QJsonArray({1.0, 0.0, 0.0}));
        QCOMPARE(detachedObserved.value(QStringLiteral("elements")).toArray().size(), 6);
        const QJsonObject observedLine = detachedObserved.value(QStringLiteral("lines")).toArray()
            .first().toObject();
        QCOMPARE(observedLine.value(QStringLiteral("section")).toObject()
                     .value(QStringLiteral("area_m2")).toDouble(), 1e-4);
        const QJsonArray observedMaterialProperties = observedLine.value(QStringLiteral("material")).toObject()
            .value(QStringLiteral("properties")).toArray();
        bool observedYoungModulus = false;
        for (const QJsonValue &propertyValue : observedMaterialProperties)
        {
            const QJsonObject property = propertyValue.toObject();
            if (property.value(QStringLiteral("type")).toString() != QStringLiteral("prop-modulus_of_elasticity"))
                continue;
            observedYoungModulus = property.value(QStringLiteral("table")).toArray()
                .first().toObject().value(QStringLiteral("value")).toDouble() == 210e9;
        }
        QVERIFY(observedYoungModulus);
        const QJsonArray observedPoints = detachedObserved.value(QStringLiteral("points")).toArray();
        QCOMPARE(observedPoints.size(), 2);
        const QJsonArray observedLoadConditions = observedPoints.at(1).toObject()
            .value(QStringLiteral("boundaryConditions")).toArray();
        QVERIFY(!observedLoadConditions.isEmpty());
        const QJsonArray observedLoadComponents = observedLoadConditions.first().toObject()
            .value(QStringLiteral("components")).toArray();
        QVERIFY(!observedLoadComponents.isEmpty());
        QCOMPARE(observedLoadComponents.first().toObject().value(QStringLiteral("samples")).toArray()
                     .first().toObject().value(QStringLiteral("value")).toDouble(), 1000.0);
        QCOMPARE(detachedObserved.value(QStringLiteral("analysis")).toObject()
                     .value(QStringLiteral("problemType")).toInt(),
                 int(session->getModel(modelId).getProblemTaskTree().getProblemType()));
        const QJsonObject detachedModel = detachedResult.value(QStringLiteral("model")).toObject();
        QVERIFY(!detachedModel.value(QStringLiteral("fingerprint")).toString().isEmpty());
        QCOMPARE(detachedModel.value(QStringLiteral("topology")).toObject(),
                 detachedResult.value(QStringLiteral("mesh")).toObject());
        QVERIFY(!detachedModel.contains(QStringLiteral("nodes")));
        QVERIFY(!detachedModel.contains(QStringLiteral("elements")));
        const QByteArray detachedJson = QJsonDocument(detachedResult).toJson(QJsonDocument::Compact);
        QVERIFY(!detachedJson.contains(directory.path().toUtf8()));
        QVERIFY(!detachedJson.contains("solverLog"));
        QVERIFY(!detachedJson.contains("filePath"));

        const auto missingJob = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"),
                                           {}, false, -1.0, QString(), QStringLiteral("missing-job-identity"));
        QCOMPARE(missingJob.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(missingJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("not_found"));
        QCOMPARE(missingJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("not_found"));
        QCOMPARE(missingJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("resultSource")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("not_found"));

        const QJsonObject mismatchInspectResponse = requestFor(
            QStringLiteral("analysis.inspect"), QStringLiteral("document"),
            trustedExperimentRodSpec(), true);
        QCOMPARE(mismatchInspectResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject documentWithDifferentValidSpec = mismatchInspectResponse.value(QStringLiteral("result")).toObject();
        QVERIFY(documentWithDifferentValidSpec.contains(QStringLiteral("diagnosis")));
        const QJsonObject mismatchDiagnosis = documentWithDifferentValidSpec
            .value(QStringLiteral("diagnosis")).toObject();
        QCOMPARE(mismatchDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        bool geometryMismatchReported = false;
        for (const QJsonValue &issue : mismatchDiagnosis.value(QStringLiteral("issues")).toArray())
            geometryMismatchReported = geometryMismatchReported
                || issue.toObject().value(QStringLiteral("code")).toString()
                    == QStringLiteral("MODEL_GEOMETRY_SPEC_MISMATCH");
        QVERIFY(geometryMismatchReported);

        const auto missingSpec = requestFor(QStringLiteral("analysis.diagnose"), QStringLiteral("detached"));
        QCOMPARE(missingSpec.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject missingDiagnosis = missingSpec.value(QStringLiteral("result")).toObject()
                                                 .value(QStringLiteral("diagnosis")).toObject();
        QCOMPARE(missingDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        QVERIFY(!missingDiagnosis.value(QStringLiteral("issues")).toArray().isEmpty());

        QJsonObject badUnits = trustedExperimentRodSpec();
        QJsonObject material = badUnits.value(QStringLiteral("material")).toObject();
        material.insert(QStringLiteral("youngModulus"), QJsonObject{{QStringLiteral("value"), 210e9},
                                                                      {QStringLiteral("unit"), QStringLiteral("N")}});
        badUnits.insert(QStringLiteral("material"), material);
        const auto badUnitDiagnosis = requestFor(QStringLiteral("analysis.diagnose"), QStringLiteral("detached"),
                                                 badUnits, true);
        QCOMPARE(badUnitDiagnosis.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        bool unitMismatch = false;
        for (const QJsonValue &issue : badUnitDiagnosis.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("diagnosis")).toObject().value(QStringLiteral("issues")).toArray())
            unitMismatch = unitMismatch || issue.toObject().value(QStringLiteral("code")).toString()
                == QStringLiteral("UNIT_DIMENSION_MISMATCH");
        QVERIFY(unitMismatch);

        QJsonObject largeSpec = trustedExperimentRodSpec();
        QJsonArray manyObservables;
        const QJsonObject observable{{QStringLiteral("selector"),
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("component"), QStringLiteral("ux")}};
        for (int index = 0; index < 500; ++index) manyObservables.append(observable);
        largeSpec.insert(QStringLiteral("observables"), manyObservables);
        QVERIFY(QJsonDocument(largeSpec).toJson(QJsonDocument::Compact).size() < 60 * 1024);
        const auto oversizedReadback = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("detached"),
                                                  largeSpec, true);
        QCOMPARE(oversizedReadback.value(QStringLiteral("code")).toString(),
                 QStringLiteral("READBACK_LIMIT_EXCEEDED"));

        Model oversizedDampingModel;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-oversized-damping-source"),
                              &oversizedDampingModel));
        oversizedDampingModel.getProblemSetup().getStructuralDynamicsSetup()
            .setModalDampingRatios(std::vector<double>(4097, 0.01));
        const QString oversizedDampingDocumentId = models.add(oversizedDampingModel);
        const auto oversizedDampingReadback = requestFor(QStringLiteral("analysis.inspect"),
            QStringLiteral("document"), {}, false, -1.0, QString(), QString(), oversizedDampingDocumentId);
        QCOMPARE(oversizedDampingReadback.value(QStringLiteral("code")).toString(),
                 QStringLiteral("READBACK_LIMIT_EXCEEDED"));

        const auto staleRevision = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"),
                                              {}, false, double(stamp.second + 1));
        QCOMPARE(staleRevision.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_REVISION"));
        const auto staleEpoch = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"),
                                           {}, false, -1.0, QStringLiteral("foreign-session"));
        QCOMPARE(staleEpoch.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_SESSION"));

        // Capture one exact native job for the source. Accepting its result
        // advances the document revision, so inspection must mark that older
        // job stale instead of treating it as the latest source result.
        auto *manualTask = new SolverTask(Application::instance()->getApplicationSettings(), modelId);
        manualTask->setBlocking(false);
        QString submitError;
        const QString jobId = manager.submitValidated(manualTask, QJsonObject(), true, &submitError);
        QVERIFY2(!jobId.isEmpty(), qPrintable(submitError));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const QString state = manager.jobQuery(jobId).value(QStringLiteral("state")).toString();
            return state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
                state == QStringLiteral("cancelled") || state == QStringLiteral("interrupted");
        }(), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        const QString fingerprintAfterAcceptedResult =
            RModelCommandService::stateFingerprint(session->getModel(modelId));

        const auto matchedJob = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"),
                                           {}, false, -1.0, QString(), jobId);
        QCOMPARE(matchedJob.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject matchedJobIdentity = matchedJob.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("job")).toObject();
        const QJsonObject inspectedRuntimeStamp = matchedJob.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("sourceRuntimeStamp")).toObject();
        const QJsonObject jobRuntimeStamp = matchedJobIdentity.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        QCOMPARE(jobRuntimeStamp.value(QStringLiteral("documentId")).toString(), documentId);
        QCOMPARE(jobRuntimeStamp.value(QStringLiteral("revision")).toString(), QString::number(stamp.second));
        QCOMPARE(matchedJobIdentity.value(QStringLiteral("status")).toString(), QStringLiteral("stale"));
        QCOMPARE(matchedJobIdentity.value(QStringLiteral("resultSourceStatus")).toString(), QStringLiteral("stale"));
        QVERIFY(inspectedRuntimeStamp != jobRuntimeStamp);
        QCOMPARE(matchedJobIdentity.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), documentId);
        const QJsonObject terminalJob = manager.jobQuery(jobId);
        QCOMPARE(matchedJobIdentity.value(QStringLiteral("state")).toString(),
                 terminalJob.value(QStringLiteral("state")).toString());
        QCOMPARE(terminalJob.value(QStringLiteral("sourceRuntimeStamp")).toObject(), jobRuntimeStamp);
        const QJsonObject resultSource = matchedJob.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("resultSource")).toObject();
        QCOMPARE(resultSource.value(QStringLiteral("status")).toString(), QStringLiteral("stale"));
        QCOMPARE(resultSource.value(QStringLiteral("sourceRuntimeStamp")).toObject(), jobRuntimeStamp);
        QVERIFY(!resultSource.contains(QStringLiteral("resultArtifactRef")));
        QVERIFY(!resultSource.contains(QStringLiteral("filePath")));
        const QString jobInputPath = manager.jobQuery(jobId).value(QStringLiteral("inputSnapshotRef")).toString();
        QVERIFY(!jobInputPath.isEmpty());
        const QByteArray matchedJobJson = QJsonDocument(matchedJobIdentity).toJson(QJsonDocument::Compact);
        QVERIFY(!matchedJobJson.contains(jobInputPath.toUtf8()));
        QVERIFY(!matchedJobJson.contains(directory.path().toUtf8()));
        QVERIFY(!matchedJobJson.contains("solverLog"));
        QVERIFY(!matchedJobJson.contains("filePath"));

        // A second document has the same model content but a different exact
        // host identity, so the explicit job is stale for that target.
        Model otherSource;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-analysis-other-source"), &otherSource));
        const QString otherDocumentId = models.add(otherSource);
        const auto staleJob = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"),
                                         {}, false, -1.0, QString(), jobId, otherDocumentId);
        QCOMPARE(staleJob.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(staleJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("stale"));

        // The accepted result advanced the document revision, so this old job
        // remains stale even when a detached candidate is also supplied.
        const auto mismatchedJob = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("detached"),
            trustedExperimentRodSpec(), true, -1.0, QString(), jobId);
        QCOMPARE(mismatchedJob.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(mismatchedJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("stale"));

        session->setModelChanged(modelId);
        const auto revisedStamp = session->modelCommandStamp(modelId);
        QCOMPARE(revisedStamp.first, documentId);
        QVERIFY(revisedStamp.second > stamp.second);
        const auto staleRevisionJob = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"),
            {}, false, -1.0, QString(), jobId);
        QCOMPARE(staleRevisionJob.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(staleRevisionJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("stale"));

        const auto noImplicitLatestJob = requestFor(QStringLiteral("analysis.inspect"), QStringLiteral("document"));
        QCOMPARE(noImplicitLatestJob.value(QStringLiteral("result")).toObject().value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("no_job"));

        QCOMPARE(session->modelCommandStamp(modelId), revisedStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), fingerprintAfterAcceptedResult);
    }

    void trustedSolverDiagnosticBundleRetainsSuccessAndFailure_data()
    {
        QTest::addColumn<int>("exitCode");
        QTest::newRow("success") << 0;
        QTest::newRow("solver-exit-1") << 1;
    }

    void trustedSolverDiagnosticBundleRetainsSuccessAndFailure()
    {
        QFETCH(int, exitCode);
        QTemporaryDir bundle;
        QVERIFY(bundle.isValid());
        const QByteArray oldCredential = qgetenv("RANGE_TEST_DIAGNOSTIC_SECRET");
        qputenv("RANGE_TEST_DIAGNOSTIC_SECRET", "diagnostic-secret-must-not-be-exported");
        auto restore = qScopeGuard([&] {
            if (oldCredential.isNull()) qunsetenv("RANGE_TEST_DIAGNOSTIC_SECRET");
            else qputenv("RANGE_TEST_DIAGNOSTIC_SECRET", oldCredential);
        });
        QString capturedInput;
        QString capturedLog;
        QString manifestPath;
        {
            QTemporaryDir runtime;
            QVERIFY(runtime.isValid());
            const QString input = runtime.filePath("job/input/model.ram");
            const QString log = runtime.filePath("job/logs/solver.log");
            QVERIFY(QDir().mkpath(QFileInfo(input).absolutePath()));
            QVERIFY(QDir().mkpath(QFileInfo(log).absolutePath()));
            QVERIFY(TrustedSolverDiagnostics::append(input, "frozen model fixture\n"));
            QVERIFY(TrustedSolverDiagnostics::append(log, "solver diagnostic survives teardown\n"));
            const QJsonObject record{{"jobId", "123"}, {"state", exitCode ? "failed" : "succeeded"},
                {"processStarted", true}, {"processExited", true}, {"processExitCode", exitCode},
                {"inputSnapshotRef", input}, {"resultArtifactRef", runtime.filePath("job/result/missing.rbm")},
                {"logArtifactRefs", QJsonObject{{"solverLog", log}}}};
            QVERIFY(TrustedSolverDiagnostics::captureJob(bundle.path(), record,
                QCoreApplication::applicationFilePath(), "test-server", 4));
            capturedInput = bundle.filePath("children/123/artifacts/input/model.ram");
            capturedLog = bundle.filePath("children/123/artifacts/logs/solver.log");
            QCOMPARE(TrustedSolverDiagnostics::sha256(capturedInput), TrustedSolverDiagnostics::sha256(input));
            QCOMPARE(TrustedSolverDiagnostics::sha256(capturedLog), TrustedSolverDiagnostics::sha256(log));
            manifestPath = bundle.filePath("children/123/diagnostic.json");
            // A repeated capture must preserve the existing package and report failure.
            QVERIFY(!TrustedSolverDiagnostics::captureJob(bundle.path(), record,
                QCoreApplication::applicationFilePath(), "test-server", 4));
        }
        QVERIFY(QFileInfo::exists(capturedInput));
        QVERIFY(QFileInfo::exists(capturedLog));
        QFile manifest(manifestPath);
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const QByteArray bytes = manifest.readAll();
        QVERIFY(!bytes.contains("diagnostic-secret-must-not-be-exported"));
        const QJsonObject captured = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(captured.value("processExitCode").toInt(-1), exitCode);
        QCOMPARE(captured.value("command").toObject().value("workingDirectory").toString(), QDir::currentPath());
        QCOMPARE(captured.value("command").toObject().value("arguments").toArray().size(), 7);
        QCOMPARE(captured.value("outputs").toArray().first().toObject().value("exists").toBool(), false);
        QVERIFY(!TrustedSolverDiagnostics::captureJob(bundle.path(), {{"jobId", "../escape"}}, {}, {}, 4));
    }

    void trustedSolverDiagnosticsRejectMissingOrMismatchedInput()
    {
        QTemporaryDir bundle;
        QTemporaryDir runtime;
        QVERIFY(bundle.isValid() && runtime.isValid());
        const QString input = runtime.filePath("job/input/model.ram");
        QJsonObject record{{"jobId", "missing"}, {"inputSnapshotRef", input}};
        QVERIFY(!TrustedSolverDiagnostics::captureJob(bundle.path(), record, {}, {}, 4));
        QFile missing(bundle.filePath("children/missing/diagnostic.json"));
        QVERIFY(missing.open(QIODevice::ReadOnly));
        QVERIFY(!QJsonDocument::fromJson(missing.readAll()).object().value("copyComplete").toBool(true));
        QVERIFY(QDir().mkpath(QFileInfo(input).absolutePath()));
        QVERIFY(TrustedSolverDiagnostics::append(input, "frozen input\n"));
        record.insert("jobId", "mismatch");
        record.insert("inputSnapshotSha256", QString(64, QLatin1Char('0')));
        QVERIFY(!TrustedSolverDiagnostics::captureJob(bundle.path(), record, {}, {}, 4));
    }

    void trustedExperimentAuthorizedMeshRefinementConverges()
    {
        const QString diagnosticTemplate = TrustedSolverDiagnostics::directoryTemplate();
        QVERIFY(!diagnosticTemplate.isEmpty());
        QTemporaryDir directory(diagnosticTemplate);
        QVERIFY(directory.isValid());
        directory.setAutoRemove(false);
        qInfo().noquote() << "Persistent authorized-repair diagnostic bundle:" << directory.path();
        Session *session = Application::instance()->getSession();
        ModelCommandHost *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        QString experimentId;
        QSet<QString> observedJobs;
        bool captureComplete = true;
        const QString solverExecutable = Application::instance()->getApplicationSettings()->getSolverPath();
        const uint solverThreads = Application::instance()->getApplicationSettings()->getNThreads();
        QJsonObject diagnostic{{"schema", "range.test.authorized-repair-diagnostics/v1"},
            {"selector", "trustedExperimentAuthorizedMeshRefinementConverges"},
            {"applicationVersion", QString::fromLatin1(VERSION)}, {"qtRuntimeVersion", QString::fromLatin1(qVersion())},
            {"testExecutable", QCoreApplication::applicationFilePath()},
            {"testExecutableSha256", TrustedSolverDiagnostics::sha256(QCoreApplication::applicationFilePath())},
            {"solverExecutable", solverExecutable}, {"solverExecutableSha256", TrustedSolverDiagnostics::sha256(solverExecutable)},
            {"runtimeDirectory", qEnvironmentVariable("RANGE_TEST_TRUSTED_RUNTIME_DIRECTORY")},
            {"workingDirectory", QDir::currentPath()}, {"captureState", "started"}};
        QVERIFY(TrustedSolverDiagnostics::writeJson(directory.filePath("diagnostic.json"), diagnostic));
        QVERIFY(TrustedSolverDiagnostics::append(directory.filePath("solver.stdout.log"), {}));
        QVERIFY(TrustedSolverDiagnostics::append(directory.filePath("solver.stderr.log"), {}));
        QObject captureScope;
        connect(&manager, &SolverManager::nativeJobUpdated, &captureScope,
                [&](const QString &jobId, const QJsonObject &) { observedJobs.insert(jobId); });
        connect(&manager, &SolverManager::readyReadStandardOutput, &captureScope, [&](const QString &text) {
            captureComplete = TrustedSolverDiagnostics::append(directory.filePath("solver.stdout.log"), text) && captureComplete;
        });
        connect(&manager, &SolverManager::readyReadStandardError, &captureScope, [&](const QString &text) {
            captureComplete = TrustedSolverDiagnostics::append(directory.filePath("solver.stderr.log"), text) && captureComplete;
        });
        auto finishDiagnostic = qScopeGuard([&] {
            QJsonArray children;
            const QStringList observedJobIds = observedJobs.values();
            for (const QString &jobId : observedJobIds)
            {
                const QJsonObject record = manager.jobQuery(jobId);
                if (experimentId.isEmpty() || record.value("trustedExperimentId").toString() != experimentId) continue;
                children.append(jobId);
                captureComplete = TrustedSolverDiagnostics::captureJob(directory.path(), record,
                    solverExecutable, manager.getTaskServerName(), solverThreads) && captureComplete;
            }
            diagnostic.insert("experimentId", experimentId);
            diagnostic.insert("childJobs", children);
            diagnostic.insert("testFailed", QTest::currentTestFailed());
            diagnostic.insert("captureState", captureComplete ? "complete" : "incomplete");
            const bool saved = TrustedSolverDiagnostics::writeJson(directory.filePath("diagnostic.json"), diagnostic);
            if (!saved || !captureComplete) QTest::qFail("Persistent solver diagnostic capture was incomplete", __FILE__, __LINE__);
        });
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-refinement-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));
        const int jobsBefore = manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt();

        QJsonObject specification = trustedExperimentRodSpec();
        QJsonObject geometry = specification.value(QStringLiteral("geometry")).toObject();
        geometry.insert(QStringLiteral("segments"), 1);
        specification.insert(QStringLiteral("geometry"), geometry);
        specification.insert(QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("modal")},
                                                                        {QStringLiteral("modes"), 1}});
        specification.insert(QStringLiteral("loads"), QJsonArray{});
        specification.insert(QStringLiteral("observables"), QJsonArray{
            QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                        {QStringLiteral("quantity"), QStringLiteral("frequency")},
                        {QStringLiteral("component"), QStringLiteral("mode")}}});
        specification.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("frequencyRelative"), 0.02}});

        QVERIFY(TrustedSolverDiagnostics::writeJson(directory.filePath("problem-spec.json"), specification));

        const auto preview = commandPreview(session, documentId, QStringLiteral("experiment.run"),
            {{QStringLiteral("problemSpec"), specification},
             {QStringLiteral("allowedNumericalRanges"), QJsonArray{
                 QJsonObject{{QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                             {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 4}}}},
             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 4},
                                                       {QStringLiteral("repairs"), 3},
                                                       {QStringLiteral("modelTurns"), 4}}}});
        QVERIFY2(preview.value(QStringLiteral("status")).toString() == QStringLiteral("preview"),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        const QJsonObject plan = preview.value(QStringLiteral("result")).toObject();
        const QString previewId = plan.value(QStringLiteral("previewId")).toString();
        experimentId = plan.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!previewId.isEmpty());
        QVERIFY(!experimentId.isEmpty());
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), jobsBefore);

        QVERIFY(host->approvePreview(previewId));
        const auto submitted = commandExecute(session, previewId, QStringLiteral("host-trusted-refinement-run-1"));
        QVERIFY2(submitted.value(QStringLiteral("status")).toString() == QStringLiteral("accepted"),
                 qPrintable(QJsonDocument(submitted).toJson(QJsonDocument::Compact)));
        const QString firstJobId = submitted.value(QStringLiteral("result")).toObject()
                                       .value(QStringLiteral("jobId")).toString();
        QVERIFY(!firstJobId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const QString state = manager.jobQuery(firstJobId).value(QStringLiteral("state")).toString();
            return state == QStringLiteral("succeeded") || state == QStringLiteral("failed")
                || state == QStringLiteral("cancelled") || state == QStringLiteral("interrupted");
        }(), 120000);
        QCOMPARE(manager.jobQuery(firstJobId).value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);

        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto response = host->handle(hostRequest(QStringLiteral("experiment.query"),
                {{QStringLiteral("experimentId"), experimentId}}));
            const QString state = response.value(QStringLiteral("result")).toObject()
                                      .value(QStringLiteral("state")).toString();
            return state == QStringLiteral("candidate_ready") || state == QStringLiteral("failed")
                || state == QStringLiteral("budget_exhausted") || state == QStringLiteral("needs_confirmation");
        }(), 120000);
        const auto queried = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}}));
        QCOMPARE(queried.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject result = queried.value(QStringLiteral("result")).toObject();
        QVERIFY(TrustedSolverDiagnostics::writeJson(directory.filePath("experiment-result.json"), result));
        QVERIFY2(result.value(QStringLiteral("state")).toString() == QStringLiteral("candidate_ready"),
                 qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        QVERIFY(result.value(QStringLiteral("verificationPassed")).toBool());
        const QJsonArray repairEvidence = result.value(QStringLiteral("repairs")).toArray();
        QCOMPARE(repairEvidence.size(), 4);
        QVERIFY(repairEvidence.first().toObject().contains(QStringLiteral("changedProblemFields")));
        const QByteArray repairEvidenceJson = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY(!repairEvidenceJson.contains("sourceSnapshotRef"));
        QVERIFY(!repairEvidenceJson.contains("manifestPath"));
        QVERIFY(!repairEvidenceJson.contains("solverLog"));
        const QJsonObject counters = result.value(QStringLiteral("counters")).toObject();
        QCOMPARE(counters.value(QStringLiteral("solves")).toInt(), 3);
        QCOMPARE(counters.value(QStringLiteral("repairs")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("childJobs")).toArray().size(), 3);
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);
        QCOMPARE(manager.jobsQuery(0, 200).value(QStringLiteral("total")).toInt(), jobsBefore + 3);
    }

    void trustedExperimentSpatialConvergenceKeepsBeamPhysicsFixed()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-spatial-study-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));

        QJsonObject specification = trustedExperimentRodSpec();
        specification.insert(QStringLiteral("modelType"), QStringLiteral("cantileverBeam"));
        QJsonObject geometry = specification.value(QStringLiteral("geometry")).toObject();
        geometry.insert(QStringLiteral("segments"), 1);
        specification.insert(QStringLiteral("geometry"), geometry);
        specification.insert(QStringLiteral("section"), QJsonObject{
            {QStringLiteral("area"), QJsonObject{{QStringLiteral("value"), 1.0e-4}, {QStringLiteral("unit"), QStringLiteral("m2")}}},
            {QStringLiteral("iy"), QJsonObject{{QStringLiteral("value"), 8.333333333333333e-10}, {QStringLiteral("unit"), QStringLiteral("m4")}}},
            {QStringLiteral("iz"), QJsonObject{{QStringLiteral("value"), 8.333333333333333e-10}, {QStringLiteral("unit"), QStringLiteral("m4")}}},
            {QStringLiteral("torsionalConstant"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-9}, {QStringLiteral("unit"), QStringLiteral("m4")}}},
            {QStringLiteral("wy"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-7}, {QStringLiteral("unit"), QStringLiteral("m3")}}},
            {QStringLiteral("wz"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-7}, {QStringLiteral("unit"), QStringLiteral("m3")}}},
            {QStringLiteral("wt"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-7}, {QStringLiteral("unit"), QStringLiteral("m3")}}},
            {QStringLiteral("localY"), QJsonArray{0.0, 1.0, 0.0}}});
        specification.insert(QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}});
        specification.insert(QStringLiteral("supports"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
            {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                                                 QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")}}}});
        specification.insert(QStringLiteral("loads"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("pointForce")},
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("components"), QJsonArray{
                QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}},
                QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}},
                QJsonObject{{QStringLiteral("value"), -1.0}, {QStringLiteral("unit"), QStringLiteral("N")}}}}}});
        specification.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("component"), QStringLiteral("uz")}}});
        specification.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}});

        QJsonObject result;
        runTrustedConvergenceStudy(session, documentId, specification,
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("mesh")}, {QStringLiteral("maximumLevels"), 3}},
            QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                                   {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 4}}},
            3, 2, QStringLiteral("host-trusted-spatial-study"), &result);
        QVERIFY2(!result.isEmpty(), "Convergence study did not produce a terminal experiment record.");
        QVERIFY2(result.value(QStringLiteral("state")).toString() == QStringLiteral("candidate_ready"),
                 qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        QVERIFY(result.value(QStringLiteral("verificationPassed")).toBool());
        const QJsonObject convergence = result.value(QStringLiteral("convergence")).toObject();
        QCOMPARE(convergence.value(QStringLiteral("status")).toString(), QStringLiteral("converged"));
        QCOMPARE(convergence.value(QStringLiteral("levelsCompleted")).toInt(), 3);
        QCOMPARE(result.value(QStringLiteral("childJobs")).toArray().size(), 3);
        QCOMPARE(convergence.value(QStringLiteral("adjacentChanges")).toArray().size(), 2);
        const QJsonArray levels = convergence.value(QStringLiteral("levels")).toArray();
        QCOMPARE(levels.size(), 3);
        const QJsonArray expectedSegments{1, 2, 4};
        QString physicalDigest;
        for (qsizetype i = 0; i < levels.size(); ++i) {
            const QJsonObject level = levels.at(i).toObject();
            QCOMPARE(level.value(QStringLiteral("physicalSummaryDigest")).toString().isEmpty(), false);
            if (physicalDigest.isEmpty()) physicalDigest = level.value(QStringLiteral("physicalSummaryDigest")).toString();
            QCOMPARE(level.value(QStringLiteral("physicalSummaryDigest")).toString(), physicalDigest);
            QCOMPARE(level.value(QStringLiteral("numericalConfiguration")).toObject()
                         .value(QStringLiteral("mesh")).toObject()
                         .value(QStringLiteral("segments")).toInt(), expectedSegments.at(i).toInt());
            QVERIFY(level.value(QStringLiteral("verificationPassed")).toBool());
        }
        for (const QJsonValue &change : convergence.value(QStringLiteral("adjacentChanges")).toArray())
            QVERIFY(change.toObject().value(QStringLiteral("belowTolerance")).toBool());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);
    }

    void trustedExperimentModalMeshConvergenceImprovesFrequencyAccuracy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-modal-study-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));

        QJsonObject specification = trustedExperimentRodSpec();
        QJsonObject geometry = specification.value(QStringLiteral("geometry")).toObject();
        geometry.insert(QStringLiteral("segments"), 2);
        specification.insert(QStringLiteral("geometry"), geometry);
        specification.insert(QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("modal")},
                                                                       {QStringLiteral("modes"), 1}});
        specification.insert(QStringLiteral("loads"), QJsonArray{});
        specification.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("quantity"), QStringLiteral("frequency")},
            {QStringLiteral("component"), QStringLiteral("mode")}}});
        specification.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("frequencyRelative"), 0.02}});

        QJsonObject result;
        runTrustedConvergenceStudy(session, documentId, specification,
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("mesh")}, {QStringLiteral("maximumLevels"), 4}},
            QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                                   {QStringLiteral("minimum"), 2}, {QStringLiteral("maximum"), 16}}},
            4, 3, QStringLiteral("host-trusted-modal-mesh-study"), &result);
        QVERIFY2(!result.isEmpty(), "Convergence study did not produce a terminal experiment record.");
        QVERIFY2(result.value(QStringLiteral("state")).toString() == QStringLiteral("candidate_ready"),
                 qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        QVERIFY(result.value(QStringLiteral("verificationPassed")).toBool());
        const QJsonObject convergence = result.value(QStringLiteral("convergence")).toObject();
        QCOMPARE(convergence.value(QStringLiteral("status")).toString(), QStringLiteral("converged"));
        QCOMPARE(convergence.value(QStringLiteral("levelsCompleted")).toInt(), 4);
        QCOMPARE(result.value(QStringLiteral("childJobs")).toArray().size(), 4);
        QCOMPARE(convergence.value(QStringLiteral("adjacentChanges")).toArray().size(), 2);
        const QJsonArray levels = convergence.value(QStringLiteral("levels")).toArray();
        QCOMPARE(levels.size(), 4);
        const QJsonArray expectedSegments{2, 4, 8, 16};
        QString physicalDigest;
        double previousError = 0.0;
        for (qsizetype i = 0; i < levels.size(); ++i) {
            const QJsonObject level = levels.at(i).toObject();
            QVERIFY(!level.value(QStringLiteral("physicalSummaryDigest")).toString().isEmpty());
            if (physicalDigest.isEmpty()) physicalDigest = level.value(QStringLiteral("physicalSummaryDigest")).toString();
            QCOMPARE(level.value(QStringLiteral("physicalSummaryDigest")).toString(), physicalDigest);
            QCOMPARE(level.value(QStringLiteral("numericalConfiguration")).toObject()
                         .value(QStringLiteral("mesh")).toObject()
                         .value(QStringLiteral("segments")).toInt(), expectedSegments.at(i).toInt());
            QVERIFY(!level.value(QStringLiteral("manifestSha256")).toString().isEmpty());
            QVERIFY(!level.value(QStringLiteral("measurementSha256")).toString().isEmpty());
            QCOMPARE(level.value(QStringLiteral("mode")).toInt(), 1);
            const QJsonObject frequency = level.value(QStringLiteral("referenceErrors")).toObject()
                .value(QStringLiteral("frequency")).toObject().value(QStringLiteral("evidence")).toObject();
            const double error = frequency.value(QStringLiteral("relativeError")).toDouble(-1.0);
            QVERIFY2(error >= 0.0, qPrintable(QJsonDocument(frequency).toJson(QJsonDocument::Compact)));
            if (i == 0) {
                QVERIFY(error > 0.02);
                QVERIFY(!level.value(QStringLiteral("verificationPassed")).toBool());
            } else {
                QVERIFY2(error < previousError,
                         qPrintable(QStringLiteral("modal reference error did not improve: %1 >= %2")
                                    .arg(error).arg(previousError)));
                QVERIFY(level.value(QStringLiteral("verificationPassed")).toBool());
            }
            previousError = error;
        }
        for (const QJsonValue &change : convergence.value(QStringLiteral("adjacentChanges")).toArray())
            QVERIFY(change.toObject().value(QStringLiteral("belowTolerance")).toBool());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);
    }

    void trustedExperimentTemporalConvergencePreservesDurationAndPhysics()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-temporal-study-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));

        QJsonObject specification = trustedExperimentRodSpec();
        QJsonObject material = specification.value(QStringLiteral("material")).toObject();
        material.insert(QStringLiteral("youngModulus"), QJsonObject{{QStringLiteral("value"), 0.3},
            {QStringLiteral("unit"), QStringLiteral("Pa")}});
        material.insert(QStringLiteral("density"), QJsonObject{{QStringLiteral("value"), 1.2},
            {QStringLiteral("unit"), QStringLiteral("kg/m3")}});
        specification.insert(QStringLiteral("material"), material);
        specification.insert(QStringLiteral("section"), QJsonObject{{QStringLiteral("area"),
            QJsonObject{{QStringLiteral("value"), 1.0}, {QStringLiteral("unit"), QStringLiteral("m2")}}}});
        specification.insert(QStringLiteral("analysis"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("linearTransient")},
            {QStringLiteral("timeStep"), QJsonObject{{QStringLiteral("value"), 0.0125},
                {QStringLiteral("unit"), QStringLiteral("s")}}},
            {QStringLiteral("steps"), 20},
            {QStringLiteral("damping"), QJsonObject{
                {QStringLiteral("rayleighAlpha"), QJsonObject{{QStringLiteral("value"), 0.0},
                    {QStringLiteral("unit"), QStringLiteral("1/s")}}},
                {QStringLiteral("rayleighBeta"), QJsonObject{{QStringLiteral("value"), 0.0},
                    {QStringLiteral("unit"), QStringLiteral("s")}}}}}});
        specification.insert(QStringLiteral("loads"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("pointForce")},
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("components"), QJsonArray{
                QJsonObject{{QStringLiteral("value"), 0.0001}, {QStringLiteral("unit"), QStringLiteral("N")}},
                QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}},
                QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}}}}}});
        specification.insert(QStringLiteral("initialConditions"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("displacement")},
                        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                        {QStringLiteral("components"), QJsonArray{
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m")}}}}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("velocity")},
                        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                        {QStringLiteral("components"), QJsonArray{
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m/s")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m/s")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m/s")}}}}}});
        specification.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("responseRelative"), 0.01}});

        QJsonObject result;
        runTrustedConvergenceStudy(session, documentId, specification,
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("timeStep")}, {QStringLiteral("maximumLevels"), 3}},
            QJsonArray{
                QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/timeStep")},
                            {QStringLiteral("minimum"), 0.003125}, {QStringLiteral("maximum"), 0.0125}},
                QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/steps")},
                            {QStringLiteral("minimum"), 20}, {QStringLiteral("maximum"), 80}}},
            3, 2, QStringLiteral("host-trusted-temporal-study"), &result);
        QVERIFY2(!result.isEmpty(), "Convergence study did not produce a terminal experiment record.");
        QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
        QVERIFY(result.value(QStringLiteral("verificationPassed")).toBool());
        const QJsonObject convergence = result.value(QStringLiteral("convergence")).toObject();
        QCOMPARE(convergence.value(QStringLiteral("status")).toString(), QStringLiteral("converged"));
        QCOMPARE(convergence.value(QStringLiteral("levelsCompleted")).toInt(), 3);
        QCOMPARE(result.value(QStringLiteral("childJobs")).toArray().size(), 3);
        const QJsonArray levels = convergence.value(QStringLiteral("levels")).toArray();
        QCOMPARE(levels.size(), 3);
        const QJsonArray expectedSteps{20, 40, 80};
        const QJsonArray expectedTimeSteps{0.0125, 0.00625, 0.003125};
        QString physicalDigest;
        for (qsizetype i = 0; i < levels.size(); ++i) {
            const QJsonObject level = levels.at(i).toObject();
            if (physicalDigest.isEmpty()) physicalDigest = level.value(QStringLiteral("physicalSummaryDigest")).toString();
            QCOMPARE(level.value(QStringLiteral("physicalSummaryDigest")).toString(), physicalDigest);
            const QJsonObject numericalAnalysis = level.value(QStringLiteral("numericalConfiguration")).toObject()
                                                     .value(QStringLiteral("analysis")).toObject();
            QCOMPARE(numericalAnalysis.value(QStringLiteral("steps")).toInt(), expectedSteps.at(i).toInt());
            QVERIFY(qAbs(level.value(QStringLiteral("timeStepS")).toDouble()
                         - expectedTimeSteps.at(i).toDouble()) < 1.0e-12);
            QVERIFY(qAbs(level.value(QStringLiteral("totalDurationS")).toDouble() - 0.25) < 1.0e-12);
            QVERIFY(level.value(QStringLiteral("verificationPassed")).toBool());
            QVERIFY(!level.value(QStringLiteral("quantities")).toArray().isEmpty());
        }
        QCOMPARE(convergence.value(QStringLiteral("adjacentChanges")).toArray().size(), 2);
        for (const QJsonValue &change : convergence.value(QStringLiteral("adjacentChanges")).toArray())
            QVERIFY(change.toObject().value(QStringLiteral("belowTolerance")).toBool());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);
    }

    void trustedExperimentConvergenceStopsWhenSolveBudgetRunsOut()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Session *session = Application::instance()->getSession();
        SessionModelGuard models(session);
        Model source;
        QVERIFY(loadTinyTruss(directory, QStringLiteral("host-trusted-budget-study-source"), &source));
        const QString documentId = models.add(source);
        const uint modelId = session->findModelByCommandIdentity(documentId);
        const auto sourceStamp = session->modelCommandStamp(modelId);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(modelId));

        QJsonObject specification = trustedExperimentRodSpec();
        QJsonObject material = specification.value(QStringLiteral("material")).toObject();
        material.insert(QStringLiteral("youngModulus"), QJsonObject{{QStringLiteral("value"), 0.3},
            {QStringLiteral("unit"), QStringLiteral("Pa")}});
        material.insert(QStringLiteral("density"), QJsonObject{{QStringLiteral("value"), 1.2},
            {QStringLiteral("unit"), QStringLiteral("kg/m3")}});
        specification.insert(QStringLiteral("material"), material);
        specification.insert(QStringLiteral("section"), QJsonObject{{QStringLiteral("area"),
            QJsonObject{{QStringLiteral("value"), 1.0}, {QStringLiteral("unit"), QStringLiteral("m2")}}}});
        specification.insert(QStringLiteral("analysis"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("linearTransient")},
            {QStringLiteral("timeStep"), QJsonObject{{QStringLiteral("value"), 0.05},
                {QStringLiteral("unit"), QStringLiteral("s")}}},
            {QStringLiteral("steps"), 20},
            {QStringLiteral("damping"), QJsonObject{
                {QStringLiteral("rayleighAlpha"), QJsonObject{{QStringLiteral("value"), 0.0},
                    {QStringLiteral("unit"), QStringLiteral("1/s")}}},
                {QStringLiteral("rayleighBeta"), QJsonObject{{QStringLiteral("value"), 0.0},
                    {QStringLiteral("unit"), QStringLiteral("s")}}}}}});
        specification.insert(QStringLiteral("loads"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("pointForce")},
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("components"), QJsonArray{
                QJsonObject{{QStringLiteral("value"), 0.0001}, {QStringLiteral("unit"), QStringLiteral("N")}},
                QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}},
                QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}}}}}});
        specification.insert(QStringLiteral("initialConditions"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("displacement")},
                        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                        {QStringLiteral("components"), QJsonArray{
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m")}}}}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("velocity")},
                        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                        {QStringLiteral("components"), QJsonArray{
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m/s")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m/s")}},
                            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("m/s")}}}}}});
        specification.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("responseRelative"), 0.01}});

        QJsonObject result;
        runTrustedConvergenceStudy(session, documentId, specification,
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("timeStep")}, {QStringLiteral("maximumLevels"), 4}},
            QJsonArray{
                QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/timeStep")},
                            {QStringLiteral("minimum"), 0.00625}, {QStringLiteral("maximum"), 0.05}},
                QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/steps")},
                            {QStringLiteral("minimum"), 20}, {QStringLiteral("maximum"), 160}}},
            2, 3, QStringLiteral("host-trusted-budget-study"), &result);
        QVERIFY2(!result.isEmpty(), "Convergence study did not produce a terminal experiment record.");
        QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("budget_exhausted"));
        QVERIFY(!result.value(QStringLiteral("verificationPassed")).toBool());
        const QJsonObject convergence = result.value(QStringLiteral("convergence")).toObject();
        QCOMPARE(convergence.value(QStringLiteral("status")).toString(), QStringLiteral("not_converged"));
        QCOMPARE(convergence.value(QStringLiteral("stopReason")).toString(), QStringLiteral("solve_budget_exhausted"));
        QCOMPARE(convergence.value(QStringLiteral("levelsCompleted")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("counters")).toObject().value(QStringLiteral("solves")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("childJobs")).toArray().size(), 2);
        QCOMPARE(convergence.value(QStringLiteral("adjacentChanges")).toArray().size(), 0);
        for (const QJsonValue &level : convergence.value(QStringLiteral("levels")).toArray())
            QVERIFY(!level.toObject().value(QStringLiteral("verificationPassed")).toBool());
        QCOMPARE(session->modelCommandStamp(modelId), sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(modelId)), sourceFingerprint);
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
        const auto capabilityResponse = host->handle(hostRequest("capabilities.query", {}));
        bool displaySupported = false;
        for (const auto &entry : capabilityResponse.value("result").toObject().value("supportedOperations").toArray())
            if (entry.toObject().value("id").toString() == "native.result.display")
                displaySupported = entry.toObject().value("version").toInt() == 1;
        QVERIFY(displaySupported);
        const QString artifactId = job.value("resultArtifactId").toString();
        QVERIFY(!artifactId.isEmpty());
        const QJsonObject channelPage = manager.nativeResultQuery(jobId, "channels", {{"artifactId", artifactId}, {"offset", 0}, {"limit", 1}});
        QVERIFY2(channelPage.value("valid").toBool(), qPrintable(QJsonDocument(channelPage).toJson()));
        const QJsonObject physicalChannel = channelPage.value("channels").toArray().first().toObject();
        const QString channelId = physicalChannel.value("channelId").toString();
        const auto display = [&](const QString &artifact, const QString &channel) {
            return host->handle(hostRequest("native.result.display", {{"jobId", jobId}, {"artifactId", artifact}, {"channelId", channel}}));
        };
        const uint beforeRejectedDisplay = session->getNModels();
        QCOMPARE(display("artifact-another-job", channelId).value("status").toString(), QString("rejected"));
        QCOMPARE(display(artifactId, "node:4294967294:displacement:X").value("status").toString(), QString("rejected"));
        QCOMPARE(session->getNModels(), beforeRejectedDisplay);

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
        const auto sourceStampBeforeDisplay = session->modelCommandStamp(session->findModelByCommandIdentity(sourceIdentity));
        const auto resultStampBeforeDisplay = session->modelCommandStamp(session->findModelByCommandIdentity(resultIdentity));
        const auto displayed = display(artifactId, channelId);
        QVERIFY2(displayed.value("status").toString() == "accepted", qPrintable(QJsonDocument(displayed).toJson()));
        const QJsonObject field = displayed.value("result").toObject();
        QCOMPARE(field.value("documentId").toString(), resultIdentity);
        QCOMPARE(field.value("artifactId").toString(), artifactId);
        QCOMPARE(field.value("fieldDisplayScope").toString(), QString("full-variable-magnitude"));
        QCOMPARE(field.value("coordinateSystem").toString(), QString("global"));
        QCOMPARE(field.value("unit"), physicalChannel.value("unit"));
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(sourceIdentity)), sourceStampBeforeDisplay);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(resultIdentity)), resultStampBeforeDisplay);
        const uint displayedID = session->findModelByCommandIdentity(resultIdentity);
        QVERIFY(session->getSelectedModelIDs().contains(displayedID));
        const auto picks = session->getPickList().getItems(displayedID);
        QCOMPARE(picks.size(), 1);
        QCOMPARE(session->getModel(displayedID).getNodePhysicalId(picks.first().getNodeID()),
                 uint(field.value("nodePhysicalId").toDouble()));
        const auto &displayData = session->getModel(displayedID).getEntityGroupPtr(
            session->getModel(displayedID).getEntityGroupID(picks.first().getEntityID().getType(), picks.first().getEntityID().getEid()))->getData();
        QVERIFY(displayData.findVariableByDisplayType(R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR) != R_VARIABLE_NONE);
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

        const auto reopenedFresh = display(artifactId, channelId);
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

        // A synchronous selection observer may edit a result document. The
        // host must resolve its identity again rather than display stale data.
        bool selectionEditInjected = false;
        const auto selectionEdit = connect(session, &Session::modelSelectionChanged, session, [&](uint id) {
            if (!selectionEditInjected && session->modelCommandStamp(id).first == freshIdentity)
            {
                selectionEditInjected = true;
                session->setModelChanged(id);
            }
        });
        const auto reentrantDisplay = display(artifactId, channelId);
        disconnect(selectionEdit);
        QVERIFY(selectionEditInjected);
        QCOMPARE(reentrantDisplay.value("status").toString(), QString("rejected"));
        QCOMPARE(reentrantDisplay.value("code").toString(), QString("RESULT_DOCUMENT_CHANGED"));
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(sourceIdentity)), sourceStampBeforeDisplay);
        QCOMPARE(session->getModel(session->findModelByCommandIdentity(sourceIdentity)).getElementGroupPtr(0)->getMaterial().toJson(), sourceMaterial);

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

        // The source and two independent result documents exercise both a
        // middle removal and the first-slot removal that previously left a
        // surviving result pick with an out-of-range model ID.
        const uint sourceModelID = session->findModelByCommandIdentity(sourceIdentity);
        const uint resultModelID = session->findModelByCommandIdentity(resultIdentity);
        const uint freshModelID = session->findModelByCommandIdentity(freshIdentity);
        QVERIFY(sourceModelID != RConstants::eod);
        QVERIFY(resultModelID != RConstants::eod);
        QVERIFY(freshModelID != RConstants::eod);
        QCOMPARE(resultModelID, sourceModelID + 1);
        QCOMPARE(freshModelID, resultModelID + 1);

        const auto sourceStampBeforeRemoval = session->modelCommandStamp(sourceModelID);
        const auto freshStampBeforeRemoval = session->modelCommandStamp(freshModelID);
        const QVector<uint> nodePositions{0, 1};
        QHash<QString, QSet<uint>> expectedNodePositions;
        QHash<QString, QHash<uint, uint>> expectedPhysicalNodes;
        session->getPickList().clear();
        for (const auto &entry : {qMakePair(sourceIdentity, sourceModelID),
                                  qMakePair(resultIdentity, resultModelID),
                                  qMakePair(freshIdentity, freshModelID)})
        {
            const Model &model = session->getModel(entry.second);
            QVERIFY(model.getNLines() > 0);
            QVERIFY(model.getNNodes() > 1);
            const uint lineGroupID = model.getEntityGroupID(R_ENTITY_GROUP_LINE, 0);
            QVERIFY(lineGroupID != RConstants::eod);
            QVERIFY(model.getEntityGroupPtr(lineGroupID));

            QSet<uint> nodeSet;
            for (uint nodeID : nodePositions)
            {
                nodeSet.insert(nodeID);
                expectedPhysicalNodes[entry.first].insert(nodeID, model.getNodePhysicalId(nodeID));
                session->getPickList().addItem(PickItem(
                    SessionEntityID(entry.second, R_ENTITY_GROUP_LINE, 0), 0, 0, nodeID, nodeID));
            }
            expectedNodePositions.insert(entry.first, nodeSet);
        }

        int observedPickListChanges = 0;
        bool observedPicksRemainValid = true;
        const auto validatePickList = [&]() {
            const auto &items = session->getPickList().getItems();
            qsizetype expectedCount = 0;
            for (auto it = expectedNodePositions.cbegin(); it != expectedNodePositions.cend(); ++it)
                expectedCount += it.value().size();
            if (items.size() != expectedCount) return false;

            QHash<QString, QSet<uint>> observedNodes;
            for (const PickItem &item : items)
            {
                const uint modelID = item.getEntityID().getMid();
                if (modelID >= session->getNModels()) return false;
                const QString identity = session->modelCommandStamp(modelID).first;
                if (!expectedNodePositions.contains(identity)) return false;
                const uint nodeID = item.getNodeID();
                const auto expectedPhysical = expectedPhysicalNodes.value(identity);
                if (!expectedPhysical.contains(nodeID)
                    || item.getEntityID().getType() != R_ENTITY_GROUP_LINE
                    || item.getEntityID().getEid() != 0
                    || item.getElementID() != 0
                    || item.getElementPosition() != 0
                    || item.getNodePosition() != nodeID
                    || item.getItemType() != PICK_ITEM_NODE)
                    return false;

                const Model &model = session->getModel(modelID);
                const uint lineGroupID = model.getEntityGroupID(R_ENTITY_GROUP_LINE, 0);
                if (lineGroupID == RConstants::eod || !model.getEntityGroupPtr(lineGroupID)
                    || nodeID >= model.getNNodes()
                    || model.getNodePhysicalId(nodeID) != expectedPhysical.value(nodeID))
                    return false;
                observedNodes[identity].insert(nodeID);
            }
            return observedNodes == expectedNodePositions;
        };
        const auto pickListObserver = connect(session, &Session::pickListChanged, session, [&]() {
            ++observedPickListChanges;
            observedPicksRemainValid = observedPicksRemainValid && validatePickList();
        }, Qt::DirectConnection);

        expectedNodePositions.remove(resultIdentity);
        expectedPhysicalNodes.remove(resultIdentity);
        session->removeModel(resultModelID);
        QCOMPARE(observedPickListChanges, 1);
        QVERIFY(observedPicksRemainValid);
        QCOMPARE(session->findModelByCommandIdentity(sourceIdentity), sourceModelID);
        QCOMPARE(session->findModelByCommandIdentity(resultIdentity), RConstants::eod);
        QCOMPARE(session->findModelByCommandIdentity(freshIdentity), resultModelID);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(sourceIdentity)),
                 sourceStampBeforeRemoval);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(freshIdentity)),
                 freshStampBeforeRemoval);

        expectedNodePositions.clear();
        expectedNodePositions.insert(freshIdentity, QSet<uint>{0, 1});
        expectedPhysicalNodes.remove(sourceIdentity);
        session->removeModel(sourceModelID);
        QCOMPARE(observedPickListChanges, 2);
        QVERIFY(observedPicksRemainValid);
        QCOMPARE(session->findModelByCommandIdentity(sourceIdentity), RConstants::eod);
        QCOMPARE(session->findModelByCommandIdentity(freshIdentity), sourceModelID);
        QCOMPARE(session->modelCommandStamp(session->findModelByCommandIdentity(freshIdentity)),
                 freshStampBeforeRemoval);
        disconnect(pickListObserver);
    }
};

int runGuiModelOperationsTests(int argc, char **argv)
{
    TestGuiModelOperations test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_gui_model_operations.moc"
