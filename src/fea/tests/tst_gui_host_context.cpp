#include <QtTest>
#include <QUuid>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <rml_variable.h>

#include <rml_model.h>
#include <rmodel_command_service.h>
#include <rbl_logger.h>
#include <new>

#include "application.h"
#include "action.h"
#include "action_list.h"
#include "model_command_host.h"
#include "mesh_job_controller.h"
#include "session.h"
#include "model_io.h"

namespace
{
QStringList *observedHistoryErrors = nullptr;
RLogHandler forwardedHistoryLog = nullptr;

void observeHistoryActionLog(const RMessage &message)
{
    if (observedHistoryErrors && message.getType() == RMessage::Type::Error)
        observedHistoryErrors->append(message);
    if (forwardedHistoryLog) forwardedHistoryLog(message);
}

struct ObserveHistoryActionErrors
{
    QStringList errors;
    RLogHandler previous = RLogger::getInstance().getLogHandler();
    bool previouslyHalted = RLogger::getInstance().getHalted();
    ObserveHistoryActionErrors()
    {
        // Integration startup can leave file/handler logging buffered. Drain it
        // through the existing handler first, then observe this action synchronously.
        RLogger::getInstance().setHalted(false);
        observedHistoryErrors = &errors; forwardedHistoryLog = previous;
        RLogger::getInstance().setLogHandler(observeHistoryActionLog);
    }
    ~ObserveHistoryActionErrors()
    {
        RLogger::getInstance().setLogHandler(previous);
        observedHistoryErrors = nullptr; forwardedHistoryLog = nullptr;
        RLogger::getInstance().setHalted(previouslyHalted);
    }
};

bool pickedReferencesAreValid(const Model &model, uint modelID)
{
    const auto &picks = Application::instance()->getSession()->getPickList();
    const auto groups = model.getPickedEntityIDs(modelID);
    for (const auto &group : groups)
    {
        const uint position = group.getEid();
        if (group.getType() == R_ENTITY_GROUP_POINT)
        { if (position >= model.getNPoints()) return false; model.getPoint(position); }
        else if (group.getType() == R_ENTITY_GROUP_SURFACE)
        { if (position >= model.getNSurfaces()) return false; model.getSurface(position); }
        else return false;
    }
    for (const auto &pick : picks.getItems(modelID))
    {
        if (pick.getElementID() >= model.getNElements()) return false;
        const auto &element = model.getElement(pick.getElementID());
        for (uint n=0; n<element.size(); ++n) model.getNode(element.getNodeId(n));
        if (pick.getItemType() == PICK_ITEM_NODE)
        {
            if (pick.getNodeID() >= model.getNNodes()) return false;
            model.getNode(pick.getNodeID());
        }
        model.getPickedResultsValues(pick);
    }
    return true;
}

Model meshSurfaceFixture()
{
    Model model; model.setNNodes(4); model.setNSurfaces(1);
    const double xyz[4][3]{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    for (uint n=0;n<4;++n)
    { model.getNode(n).set(xyz[n][0],xyz[n][1],xyz[n][2]); model.setNodePhysicalId(n,101+10*n); }
    const uint faces[4][3]{{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for (const auto &face : faces)
    {
        RElement element(R_ELEMENT_TRI1);
        for (uint n=0;n<3;++n) element.setNodeId(n,face[n]);
        model.addElement(element,true,0);
    }
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    return model;
}
QJsonObject meshOptions()
{ return {{"unit","m"},{"volumeUnit","m^3"},{"maxVolume",0.08},{"radiusEdgeRatio",1.6}}; }

Model unboundTetraFixture()
{
    Model model;
    model.setNNodes(4); model.setNSurfaces(1); model.setNVolumes(1);
    const double xyz[4][3] = {{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    for (uint n = 0; n < 4; ++n)
    {
        model.getNode(n).set(xyz[n][0], xyz[n][1], xyz[n][2]);
        model.setNodePhysicalId(n, 101 + 10*n);
    }
    const uint faces[4][3] = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for (const auto &face : faces)
    {
        RElement triangle(R_ELEMENT_TRI1);
        for (uint j = 0; j < 3; ++j) triangle.setNodeId(j, face[j]);
        model.addElement(triangle, true, 0);
    }
    RElement tetra(R_ELEMENT_TETRA1);
    for (uint j = 0; j < 4; ++j) tetra.setNodeId(j, j);
    model.addElement(tetra, true, 0);
    model.setProblemTaskTree(RProblemTaskItem()); model.setProblemSetup(RProblemSetup());
    model.setTimeSolver(RTimeSolver()); model.clearResults(); model.clearBoundaryConditions();
    model.clearInitialConditions(); model.clearEntityVariableData();
    for (uint g = 0; g < model.getNElementGroups(); ++g)
        while (model.getElementGroupPtr(g)->getNEnvironmentConditions())
            model.getElementGroupPtr(g)->removeEnvironmentCondition(0u);
    return model;
}

QJsonObject staticBindingArguments()
{
    return {{"unitSI", true}, {"staticStress", true},
        {"material", QJsonObject{{"youngModulusPa", 1000}, {"poissonRatio", 0.25}, {"densityKgM3", 1}}},
        {"constraints", QJsonArray{
            QJsonObject{{"nodePhysicalId", 101}, {"values", QJsonArray{0,0,0}}},
            QJsonObject{{"nodePhysicalId", 111}, {"values", QJsonArray{QJsonValue(QJsonValue::Null),0,0}}},
            QJsonObject{{"nodePhysicalId", 121}, {"values", QJsonArray{QJsonValue(QJsonValue::Null),QJsonValue(QJsonValue::Null),0}}}}},
        {"loads", QJsonArray{QJsonObject{{"nodePhysicalId", 131}, {"forceN", QJsonArray{0,0,-1}}}}}};
}

QPair<uint, uint> nextPointIdentityProbe(const Model &source)
{
    Model probe(source);
    RElement point(R_ELEMENT_POINT); point.setNodeId(0, 0);
    const uint groupPosition = probe.getNPoints();
    probe.addPoint();
    probe.addElement(point, true, groupPosition);
    return {probe.getElementPhysicalId(probe.getNElements() - 1),
            probe.getPoint(groupPosition).getPhysicalId()};
}

Model modalFixture()
{
    Model model;
    model.setName(QStringLiteral("Host context fixture"));
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

QJsonObject request(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), operation},
            {QStringLiteral("parameters"), parameters}};
}

QJsonObject targetFor(const QJsonObject &context)
{
    return {{QStringLiteral("documentId"), context.value(QStringLiteral("documentId"))},
            {QStringLiteral("sessionEpoch"), context.value(QStringLiteral("sessionEpoch"))}};
}

QJsonObject sourceOrigin(const QJsonObject &snapshot)
{
    QJsonObject origin;
    for (const QString &key : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
         QStringLiteral("sourceRevision"), QStringLiteral("modelSha256"), QStringLiteral("instanceRef")})
        origin.insert(key, snapshot.value(key));
    return origin;
}
}

class TestGuiHostContext : public QObject
{
    Q_OBJECT
private slots:
    void meshCommandsUseSharedControllerExactSourceAndManualApproval()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost(); QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(meshSurfaceFixture());
        const auto stamp = session->modelCommandStamp(0);
        const QJsonObject target{{"documentId",stamp.first},{"sessionEpoch",host->sessionEpoch()}};
        auto proposal = [&](const QString &command, const QJsonObject &arguments, quint64 revision) {
            return request("command.preview", {{"target",target},{"expectedDocumentRevision",double(revision)},
                {"command",command},{"commandVersion",1},{"arguments",arguments}});
        };
        auto extra = meshOptions(); extra.insert("approval",true);
        QCOMPARE(host->handle(proposal("mesh.generate",extra,stamp.second)).value("code"),QJsonValue("INVALID_PARAMETERS"));
        QCOMPARE(host->handle(proposal("mesh.generate",meshOptions(),stamp.second+1)).value("code"),QJsonValue("STALE_REVISION"));
        QCOMPARE(host->handle(proposal("mesh.accept",{{"attemptId","unknown"}},stamp.second)).value("code"),QJsonValue("MESH_SOURCE_MISMATCH"));
        QCOMPARE(host->handle(proposal("mesh.cancel",{{"attemptId","unknown"}},stamp.second)).value("code"),QJsonValue("MESH_SOURCE_MISMATCH"));
        QCOMPARE(host->handle(proposal("mesh.accept",{{"attemptId","unknown"},{"remapOldIds",true}},stamp.second)).value("code"),QJsonValue("INVALID_PARAMETERS"));
        const auto generateRequest = proposal("mesh.generate",meshOptions(),stamp.second);
        QCOMPARE(host->handleReadOnly(generateRequest).value("code"),QJsonValue("READ_ONLY_DENIED"));
        const auto generatedPreview = host->handle(generateRequest);
        QCOMPARE(generatedPreview.value("status"),QJsonValue("preview"));
        const auto preview = generatedPreview.value("result").toObject();
        const QString previewId = preview.value("previewId").toString();
        QVERIFY(preview.value("requiresManualApproval").toBool());
        QVERIFY(!preview.value("meshPreview").toObject().contains("request"));
        QVERIFY(host->meshController()->jobs().isEmpty()); // Proposals never start a worker.
        const auto execute = request("command.execute", {{"previewId",previewId},{"idempotencyKey","mesh-host-generation"}});
        QCOMPARE(host->handle(execute).value("code"),QJsonValue("AUTHORIZATION_REQUIRED"));
        QVERIFY(host->approvePreview(previewId));
        const auto started = host->handle(execute);
        QCOMPARE(started.value("status"),QJsonValue("accepted"));
        const QString attempt = started.value("result").toObject().value("attemptId").toString();
        QVERIFY(!attempt.isEmpty());
        QCOMPARE(host->meshController()->jobs().size(),1);
        QCOMPARE(host->handle(execute).value("result"),started.value("result")); // Durable one-shot replay.
        const QJsonObject queryParameters{{"target",target},{"expectedDocumentRevision",double(stamp.second)},{"attemptId",attempt}};
        auto wrongSource = queryParameters; wrongSource.insert("expectedDocumentRevision",double(stamp.second+1));
        QCOMPARE(host->handle(request("mesh.job.query",wrongSource)).value("code"),QJsonValue("MESH_SOURCE_MISMATCH"));
        auto unknown = queryParameters; unknown.insert("attemptId","unknown-attempt");
        QCOMPARE(host->handle(request("mesh.job.query",unknown)).value("code"),QJsonValue("MESH_SOURCE_MISMATCH"));
        auto extraQuery = queryParameters; extraQuery.insert("approve",true);
        QCOMPARE(host->handle(request("mesh.job.query",extraQuery)).value("code"),QJsonValue("INVALID_PARAMETERS"));
        QCOMPARE(host->handleReadOnly(request("mesh.job.query",queryParameters)).value("status"),QJsonValue("ok"));
        // Cancellation still belongs to the frozen attempt after live source edits.
        session->invalidateModelCommands(0);
        const auto editedStamp = session->modelCommandStamp(0);
        const auto staleState = host->handle(request("mesh.job.query",queryParameters));
        QCOMPARE(staleState.value("status"),QJsonValue("ok"));
        QVERIFY(!staleState.value("result").toObject().value("sourceCurrent").toBool());
        QCOMPARE(host->handle(proposal("mesh.cancel",{{"attemptId",attempt}},editedStamp.second)).value("code"),QJsonValue("MESH_SOURCE_MISMATCH"));
        const auto cancelled = host->handle(proposal("mesh.cancel",{{"attemptId",attempt}},stamp.second));
        QCOMPARE(cancelled.value("status"),QJsonValue("preview"));
        const QString cancelId = cancelled.value("result").toObject().value("previewId").toString();
        QVERIFY(host->approvePreview(cancelId));
        QCOMPARE(host->handle(request("command.execute",{{"previewId",cancelId},{"idempotencyKey","mesh-host-cancel"}})).value("status"),QJsonValue("accepted"));
        QCOMPARE(host->meshController()->query(attempt).value("result").toObject().value("status"),QJsonValue("cancelled"));
        QCOMPARE(host->handle(proposal("mesh.cancel",{{"attemptId",attempt}},stamp.second)).value("code"),QJsonValue("MESH_ATTEMPT_TERMINAL"));
        QCOMPARE(session->getNModels(),1u); QCOMPARE(session->modelCommandStamp(0),editedStamp);
    }

    void meshGenerationPreviewRetiresWhenSourceChanges()
    {
        static QTemporaryDir workspace; QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession(); auto *host = session->getModelCommandHost();
        QString error; QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(meshSurfaceFixture()); const auto stamp = session->modelCommandStamp(0);
        const auto preview = host->handle(request("command.preview",{{"target",QJsonObject{
            {"documentId",stamp.first},{"sessionEpoch",host->sessionEpoch()}}},
            {"expectedDocumentRevision",double(stamp.second)},{"command","mesh.generate"},
            {"commandVersion",1},{"arguments",meshOptions()}}));
        QCOMPARE(preview.value("status"),QJsonValue("preview"));
        const QString id = preview.value("result").toObject().value("previewId").toString();
        session->invalidateModelCommands(0);
        QVERIFY(!host->approvePreview(id)); QVERIFY(host->meshController()->jobs().isEmpty());
    }

    void staticBindingPublicationIsAtomicAndPreservesGuiHistory()
    {
        static QTemporaryDir workspace;
        static QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        Model source = unboundTetraFixture();
        source.setName("Unbound static source");
        source.getSurface(0).getData().setSelected(true);
        source.getVolume(0).getData().setVisible(false);
        source.initializeMeshInput();
        const double minimumSeed=source.getProblemSetup().getMeshSetup().getMinEdgeLength();
        const double maximumSeed=source.getProblemSetup().getMeshSetup().getMaxEdgeLength();
        const QString sourcePath = sources.filePath("source-static.ram");
        ModelIO::save(source, sourcePath, false);
        Model loaded; ModelIO::open(loaded, sourcePath); source = loaded;
        QVERIFY(source.getLoadedSourceBinding().has_value());
        // Actual GUI initialization, RAM save and native load preserve the
        // exact bbox sizing seeds, with no post-load fixture reset.
        QCOMPARE(source.getProblemTaskTree().getProblemTypeMask(),int(R_PROBLEM_NONE));
        QVERIFY(source.getProblemSetup().getMeshSetup().getMinEdgeLength()>0);
        QVERIFY(source.getProblemSetup().getMeshSetup().getMaxEdgeLength()>0);
        QCOMPARE(source.getProblemSetup().getMeshSetup().getMinEdgeLength(),minimumSeed);
        QCOMPARE(source.getProblemSetup().getMeshSetup().getMaxEdgeLength(),maximumSeed);
        const auto sourceBinding = *source.getLoadedSourceBinding();
        const auto core = RModelCommandService::preview(source, "mesh.rebind.static", staticBindingArguments());
        QVERIFY2(core.valid, qPrintable(core.message));
        const auto setupProof=core.report.value("meshEligibility").toObject().value("sourceSetupProof").toObject();
        QCOMPARE(setupProof.value("profile").toString(),QString("default_reset_with_bbox_sizing_seeds"));
        QVERIFY(setupProof.value("fullDefaultRemainderVerified").toBool());
        QCOMPARE(setupProof.value("minimumSizingSeedM").toDouble(),source.getProblemSetup().getMeshSetup().getMinEdgeLength());
        QCOMPARE(setupProof.value("maximumSizingSeedM").toDouble(),source.getProblemSetup().getMeshSetup().getMaxEdgeLength());
        const QString originalHash = RModelCommandService::stateFingerprint(source);
        // A failing allocation/validator in preparation cannot publish even
        // one POINT or advance the live allocator.
        QVERIFY(!source.applyStaticMeshBindingCandidate(core.candidate,
            [](const RModel &) -> bool { throw std::bad_alloc(); }, &error));
        QCOMPARE(RModelCommandService::stateFingerprint(source), originalHash);
        QCOMPARE(source.getNPoints(), 0u); QCOMPARE(source.getNElements(), 5u);
        RModel invalid(core.candidate);
        invalid.getElement(0).setNodeId(0, 1);
        QVERIFY(!source.applyStaticMeshBindingCandidate(invalid, [](const RModel &) { return true; }, &error));
        QCOMPARE(RModelCommandService::stateFingerprint(source), originalHash);
        // Suppress ordinary import UI only while seeding this unbound source.
        // Live command publication and mesh candidate acceptance remain unblocked.
        { const QSignalBlocker initialImport(session); session->addModel(source); }
        // Seed the actual tree after suppressing only the import task dialog.
        // Its first refresh establishes the live GUI selection baseline.
        session->setModelChanged(0);
        const bool sourceSurfaceSelected = session->getModel(0).getSurface(0).getData().getSelected();
        const bool sourceVolumeVisible = session->getModel(0).getVolume(0).getData().getVisible();
        const auto propose = [&](const QString &command, const QJsonObject &arguments = QJsonObject{}) {
            const auto stamp = session->modelCommandStamp(0);
            return host->handle(request("command.preview", {{"target", QJsonObject{
                {"documentId", stamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
                {"expectedDocumentRevision", double(stamp.second)}, {"command", command},
                {"commandVersion", 1}, {"arguments", arguments}}));
        };
        const auto preview = propose("mesh.rebind.static", staticBindingArguments());
        QCOMPARE(preview.value("status").toString(), QString("preview"));
        const auto body = preview.value("result").toObject();
        QVERIFY(!body.value("report").toObject().isEmpty());
        QVERIFY(!body.value("bindingReadSet").toObject().isEmpty());
        QCOMPARE(body.value("units").toObject().value("force").toString(), QString("N"));
        QVERIFY(body.value("invalidation").toObject().value("results").toBool());
        const auto id = body.value("previewId").toString();
        const auto execute = [&](const QString &previewId) {
            return host->handle(request("command.execute", {{"previewId", previewId},
                {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        };
        QCOMPARE(execute(id).value("status").toString(), QString("rejected"));
        QCOMPARE(session->getModel(0).getNPoints(), 0u);
        QVERIFY(host->approvePreview(id));
        QSignalSpy changed(session, &Session::modelChanged);
        QSignalSpy problem(session, &Session::problemChanged);
        const auto accepted = execute(id);
        QCOMPARE(accepted.value("status").toString(), QString("accepted"));
        QCOMPARE(changed.size(), 1); QCOMPARE(problem.size(), 0);
        QCOMPARE(session->getModel(0).getNPoints(), 4u);
        QCOMPARE(session->getModel(0).getProblemTaskTree().getProblemTypeMask(), int(R_PROBLEM_STRESS));
        QCOMPARE(session->getModel(0).getFileName(), sourcePath);
        QVERIFY(session->getModel(0).getLoadedSourceBinding().has_value());
        QCOMPARE(session->getModel(0).getLoadedSourceBinding()->savedArtifactHash, sourceBinding.savedArtifactHash);
        QCOMPARE(session->getModel(0).getSurface(0).getData().getSelected(), sourceSurfaceSelected);
        QCOMPARE(session->getModel(0).getVolume(0).getData().getVisible(), sourceVolumeVisible);
        QCOMPARE(session->getModel(0).getNVariables(), 0u);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)), core.delta.meshRebind.afterStateSha256);
        const QString boundHash = RModelCommandService::stateFingerprint(session->getModel(0));
        const auto boundNextIds = nextPointIdentityProbe(session->getModel(0));
        // These GUI choices were made after the captured history snapshot.
        // Restoring a whole Model would incorrectly revert them on Undo.
        session->getModel(0).getSurface(0).getData().setSelected(false);
        session->getModel(0).getVolume(0).getData().setVisible(true);
        auto &picks = session->getPickList(); picks.clear(); picks.setMultipleSelection(true);
        const PickItem sourceSurface(SessionEntityID(0, R_ENTITY_GROUP_SURFACE, 0), 0, 0);
        const PickItem sourceNode(SessionEntityID(0, R_ENTITY_GROUP_SURFACE, 0), 0, 0, 0, 0);
        const uint pointElement = session->getModel(0).getPoint(0).get(0);
        const PickItem ownedPoint(SessionEntityID(0, R_ENTITY_GROUP_POINT, 0), pointElement, 0);
        const uint pointNode = session->getModel(0).getElement(pointElement).getNodeId(0);
        const PickItem ownedPointNode(SessionEntityID(0, R_ENTITY_GROUP_POINT, 0), pointElement, 0, pointNode, 0);
        picks.addItem(sourceSurface); picks.addItem(sourceNode); picks.addItem(ownedPoint); picks.addItem(ownedPointNode);
        QVERIFY(pickedReferencesAreValid(session->getModel(0), 0));
        bool redrawReferencesValid = true;
        QObject redrawObserver;
        connect(session, &Session::modelChanged, &redrawObserver, [&](uint modelID) {
            redrawReferencesValid &= pickedReferencesAreValid(session->getModel(modelID), modelID);
        });
        for (const auto &command : {QStringLiteral("document.undo"), QStringLiteral("document.redo")})
        {
            const auto beforeDepth = session->getModel(0).getUndoStackSize();
            const auto historyPreview = propose(command).value("result").toObject();
            QVERIFY(!historyPreview.value("candidateFingerprint").toString().isEmpty());
            QVERIFY(!historyPreview.value("before").toObject().value("historyItemId").toString().isEmpty());
            QCOMPARE(session->getModel(0).getUndoStackSize(), beforeDepth);
            const auto history = historyPreview.value("previewId").toString();
            QVERIFY(!history.isEmpty()); QVERIFY(host->approvePreview(history));
            QCOMPARE(execute(history).value("status").toString(), QString("accepted"));
            QCOMPARE(session->getModel(0).getFileName(), sourcePath);
            QVERIFY(session->getModel(0).getLoadedSourceBinding().has_value());
            QCOMPARE(session->getModel(0).getLoadedSourceBinding()->savedArtifactHash, sourceBinding.savedArtifactHash);
            QCOMPARE(session->getModel(0).getNVariables(), 0u);
            QVERIFY(!session->getModel(0).getSurface(0).getData().getSelected());
            QVERIFY(session->getModel(0).getVolume(0).getData().getVisible());
            QCOMPARE(nextPointIdentityProbe(session->getModel(0)), boundNextIds);
            QVERIFY(picks.hasItem(sourceSurface)); QVERIFY(picks.hasItem(sourceNode));
            QVERIFY(!picks.hasItem(ownedPoint)); QVERIFY(!picks.hasItem(ownedPointNode));
            QCOMPARE(picks.getItems(0).size(), 2);
            QVERIFY(pickedReferencesAreValid(session->getModel(0), 0)); QVERIFY(redrawReferencesValid);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),
                command == "document.undo" ? originalHash : boundHash);
        }
    }

    void staticBindingTypedHistoryIsAtomicAndLegacyUndoUsesIt()
    {
        Model model = unboundTetraFixture();
        QString error;
        const auto core = RModelCommandService::preview(model, "mesh.rebind.static", staticBindingArguments());
        QVERIFY2(core.valid, qPrintable(core.message));
        const QString unboundHash = RModelCommandService::stateFingerprint(model);
        const auto originalNextIds = nextPointIdentityProbe(model);
        model.storeCurentVersion(8, "Older GUI operation");
        const auto reject = [](const RModel &) { return false; };
        const auto throwing = [](const RModel &) -> bool { throw std::bad_alloc(); };
        QVERIFY(!model.commitStaticMeshBindingWithHistory(core.candidate, core.delta, 8, "Binding", &error, reject));
        QVERIFY(!model.commitStaticMeshBindingWithHistory(core.candidate, core.delta, 8, "Binding", &error, throwing));
        QCOMPARE(RModelCommandService::stateFingerprint(model), unboundHash);
        QCOMPARE(model.getUndoStackSize(), 1u); QCOMPARE(model.getRedoStackSize(), 0u);
        QCOMPARE(model.getUndoActionMessage(), QString("Older GUI operation"));
        QCOMPARE(nextPointIdentityProbe(model), originalNextIds);
        QVERIFY2(model.commitStaticMeshBindingWithHistory(core.candidate, core.delta, 8, "Binding", &error), qPrintable(error));
        QCOMPARE(model.getUndoStackSize(), 2u);
        const QString boundHash = RModelCommandService::stateFingerprint(model);
        const auto nextIds = nextPointIdentityProbe(model);
        QVERIFY(nextIds.first > originalNextIds.first); QVERIFY(nextIds.second > originalNextIds.second);
        QVector<QPair<uint,uint>> ownedIds;
        for (uint p=0; p<model.getNPoints(); ++p)
            ownedIds.append({model.getPoint(p).getPhysicalId(), model.getElementPhysicalId(model.getPoint(p).get(0))});
        model.setFileName("current-live-source.ram");
        RProjectCatalog::SourceBinding liveBinding;
        liveBinding.logicalSourcePath = model.getFileName(); liveBinding.savedArtifactHash = "current-live-binding";
        model.setLoadedSourceBinding(liveBinding);
        model.getSurface(0).getData().setSelected(true);
        model.getVolume(0).getData().setVisible(false);
        QString hash;
        const QString undoId = model.staticMeshBindingHistoryIdentity(true);
        QVERIFY2(model.previewStaticMeshBindingHistory(true, &hash, &error), qPrintable(error));
        QCOMPARE(hash, unboundHash);
        QVERIFY(!model.applyStaticMeshBindingHistory(true, "other-history-item", hash, &error));
        QVERIFY(!model.applyStaticMeshBindingHistory(true, undoId, boundHash, &error));
        QVERIFY(!model.applyStaticMeshBindingHistory(true, undoId, hash, &error, reject));
        QVERIFY(!model.applyStaticMeshBindingHistory(true, undoId, hash, &error, throwing));
        QCOMPARE(RModelCommandService::stateFingerprint(model), boundHash);
        QCOMPARE(model.getUndoStackSize(), 2u); QCOMPARE(model.getRedoStackSize(), 0u);
        QCOMPARE(model.staticMeshBindingHistoryIdentity(true), undoId);
        QCOMPARE(nextPointIdentityProbe(model), nextIds);
        // Solver results must cause explicit rejection, never implicit clearing.
        RVariable result(R_VARIABLE_DISPLACEMENT, R_VARIABLE_APPLY_NODE);
        result.resize(3, 4); result.setValue(0, 3, 0.125); model.addVariable(result);
        QVERIFY(!model.previewStaticMeshBindingHistory(true, &hash, &error));
        QVERIFY(!model.applyStaticMeshBindingHistory(true, undoId, unboundHash, &error));
        bool legacyRejected = false;
        try { model.undo(0); } catch (const RError &) { legacyRejected = true; }
        QVERIFY(legacyRejected); QCOMPARE(model.getNVariables(), 1u);
        QCOMPARE(model.getVariable(0).getValue(0, 3), 0.125);
        QCOMPARE(model.getUndoStackSize(), 2u); QCOMPARE(model.getRedoStackSize(), 0u);
        model.clearResults();
        // The compatible Model entry point must also use atomic history.
        model.undo(0);
        QCOMPARE(RModelCommandService::stateFingerprint(model), unboundHash);
        QCOMPARE(model.getUndoStackSize(), 1u); QCOMPARE(model.getRedoStackSize(), 1u);
        QCOMPARE(model.getUndoActionMessage(), QString("Older GUI operation"));
        QCOMPARE(nextPointIdentityProbe(model), nextIds);
        QCOMPARE(model.getFileName(), QString("current-live-source.ram"));
        QCOMPARE(model.getLoadedSourceBinding()->savedArtifactHash, liveBinding.savedArtifactHash);
        QVERIFY(model.getSurface(0).getData().getSelected()); QVERIFY(!model.getVolume(0).getData().getVisible());
        const QString redoId = model.staticMeshBindingHistoryIdentity(false);
        QVERIFY2(model.previewStaticMeshBindingHistory(false, &hash, &error), qPrintable(error));
        QVERIFY(!model.applyStaticMeshBindingHistory(false, redoId, hash, &error, reject));
        QVERIFY(!model.applyStaticMeshBindingHistory(false, redoId, hash, &error, throwing));
        QCOMPARE(RModelCommandService::stateFingerprint(model), unboundHash);
        QCOMPARE(model.getUndoStackSize(), 1u); QCOMPARE(model.getRedoStackSize(), 1u);
        model.redo(0);
        QCOMPARE(RModelCommandService::stateFingerprint(model), boundHash);
        QCOMPARE(nextPointIdentityProbe(model), nextIds);
        for (uint p=0; p<model.getNPoints(); ++p)
            QCOMPARE(qMakePair(model.getPoint(p).getPhysicalId(), model.getElementPhysicalId(model.getPoint(p).get(0))), ownedIds[int(p)]);
        QCOMPARE(model.getLoadedSourceBinding()->savedArtifactHash, liveBinding.savedArtifactHash);
        // A new binding after Undo receives fresh identities rather than recycling.
        model.undo(0);
        const auto rebound = RModelCommandService::preview(model, "mesh.rebind.static", staticBindingArguments());
        QVERIFY2(rebound.valid, qPrintable(rebound.message));
        QVERIFY(model.commitStaticMeshBindingWithHistory(rebound.candidate, rebound.delta, 8, "Binding again", &error));
        QCOMPARE(model.getRedoStackSize(), 0u);
        for (uint p=0; p<model.getNPoints(); ++p)
        {
            QVERIFY(model.getPoint(p).getPhysicalId() >= nextIds.second);
            QVERIFY(model.getElementPhysicalId(model.getPoint(p).get(0)) >= nextIds.first);
        }
    }

    void staticBindingToolbarActionsRejectResultsAndPreserveSurvivingPicks()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *application = Application::instance();
        auto *session = application->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        { const QSignalBlocker initialImport(session); session->addModel(unboundTetraFixture()); }
        auto &model = session->getModel(0);
        const QString unboundHash = RModelCommandService::stateFingerprint(model);
        const auto binding = RModelCommandService::preview(model, "mesh.rebind.static", staticBindingArguments());
        QVERIFY2(binding.valid, qPrintable(binding.message));
        QVERIFY2(model.commitStaticMeshBindingWithHistory(binding.candidate, binding.delta, 8, "Binding", &error), qPrintable(error));
        const QString boundHash = RModelCommandService::stateFingerprint(model);
        const auto nextIds = nextPointIdentityProbe(model);
        session->setModelChanged(0);
        session->setModelSelected(0, true);
        auto &picks = session->getPickList(); picks.clear(); picks.setMultipleSelection(true);
        const PickItem sourceSurface(SessionEntityID(0, R_ENTITY_GROUP_SURFACE, 0), 0, 0);
        const PickItem sourceNode(SessionEntityID(0, R_ENTITY_GROUP_SURFACE, 0), 0, 0, 0, 0);
        const uint pointElement = model.getPoint(0).get(0);
        const PickItem ownedPoint(SessionEntityID(0, R_ENTITY_GROUP_POINT, 0), pointElement, 0);
        picks.addItem(sourceSurface); picks.addItem(sourceNode); picks.addItem(ownedPoint);
        QVERIFY(pickedReferencesAreValid(model, 0));
        auto *actions = application->getActionList();
        actions->processAvailability();
        auto *undo = actions->getAction(Action::ACTION_GEOMETRY_UNDO);
        auto *redo = actions->getAction(Action::ACTION_GEOMETRY_REDO);
        QVERIFY(undo->isEnabled());
        QSignalSpy changed(session, &Session::modelChanged);
        QSignalSpy pickChanged(&picks, &PickList::pickListChanged);
        bool redrawReferencesValid = true;
        QObject redrawObserver;
        connect(session, &Session::modelChanged, &redrawObserver, [&](uint modelID) {
            redrawReferencesValid &= pickedReferencesAreValid(session->getModel(modelID), modelID);
        });
        const auto journalBytes = [&]() {
            QFile journal(host->journalPath());
            if (!journal.open(QIODevice::ReadOnly)) return QByteArray();
            return journal.readAll();
        };
        const auto addResult = [&]() {
            RVariable result(R_VARIABLE_DISPLACEMENT, R_VARIABLE_APPLY_NODE);
            result.resize(3, 4); result.setValue(0, 3, 0.125); model.addVariable(result);
        };
        addResult();
        QVERIFY(host->query(0).value("status") != QJsonValue("rejected"));
        const auto beforeStamp = session->modelCommandStamp(0);
        const auto beforeJournal = journalBytes();
        const QString beforeHistoryId = model.staticMeshBindingHistoryIdentity(true);
        const QString boundWithResultsHash = RModelCommandService::stateFingerprint(model);
        QVERIFY(boundWithResultsHash != boundHash);
        {
            ObserveHistoryActionErrors logs;
            // Exercise the installed QAction connection, not a Model try/catch.
            undo->trigger();
            QVERIFY(logs.errors.join('\n').contains("Could not undo the model"));
        }
        QCOMPARE(changed.size(), 0); QCOMPARE(pickChanged.size(), 0);
        QCOMPARE(session->modelCommandStamp(0), beforeStamp);
        QCOMPARE(journalBytes(), beforeJournal);
        QCOMPARE(RModelCommandService::stateFingerprint(model), boundWithResultsHash);
        QCOMPARE(model.getNVariables(), 1u); QCOMPARE(model.getVariable(0).getValue(0, 3), 0.125);
        QCOMPARE(model.getUndoStackSize(), 1u); QCOMPARE(model.getRedoStackSize(), 0u);
        QCOMPARE(model.staticMeshBindingHistoryIdentity(true), beforeHistoryId);
        QCOMPARE(nextPointIdentityProbe(model), nextIds);
        QCOMPARE(picks.getItems(0).size(), 3);
        QVERIFY(picks.hasItem(sourceSurface)); QVERIFY(picks.hasItem(sourceNode)); QVERIFY(picks.hasItem(ownedPoint));
        model.clearResults();
        QCOMPARE(RModelCommandService::stateFingerprint(model), boundHash);
        undo->trigger();
        QCOMPARE(changed.size(), 1); QCOMPARE(pickChanged.size(), 1);
        QCOMPARE(RModelCommandService::stateFingerprint(model), unboundHash);
        QCOMPARE(model.getUndoStackSize(), 0u); QCOMPARE(model.getRedoStackSize(), 1u);
        QCOMPARE(nextPointIdentityProbe(model), nextIds);
        QVERIFY(!picks.hasItem(ownedPoint)); QVERIFY(picks.hasItem(sourceSurface)); QVERIFY(picks.hasItem(sourceNode));
        QVERIFY(pickedReferencesAreValid(model, 0)); QVERIFY(redrawReferencesValid);
        actions->processAvailability(); QVERIFY(redo->isEnabled());
        addResult();
        QVERIFY(host->query(0).value("status") != QJsonValue("rejected"));
        const auto redoStamp = session->modelCommandStamp(0);
        const auto redoJournal = journalBytes();
        const QString redoId = model.staticMeshBindingHistoryIdentity(false);
        const QString unboundWithResultsHash = RModelCommandService::stateFingerprint(model);
        QVERIFY(unboundWithResultsHash != unboundHash);
        {
            ObserveHistoryActionErrors logs;
            redo->trigger();
            QVERIFY(logs.errors.join('\n').contains("Could not redo the model"));
        }
        QCOMPARE(changed.size(), 1); QCOMPARE(pickChanged.size(), 1);
        QCOMPARE(session->modelCommandStamp(0), redoStamp); QCOMPARE(journalBytes(), redoJournal);
        QCOMPARE(model.staticMeshBindingHistoryIdentity(false), redoId);
        QCOMPARE(RModelCommandService::stateFingerprint(model), unboundWithResultsHash);
        QCOMPARE(model.getNVariables(), 1u); QCOMPARE(model.getVariable(0).getValue(0, 3), 0.125);
        QCOMPARE(model.getUndoStackSize(), 0u); QCOMPARE(model.getRedoStackSize(), 1u);
        QCOMPARE(picks.getItems(0).size(), 2); QVERIFY(picks.hasItem(sourceSurface)); QVERIFY(picks.hasItem(sourceNode));
        model.clearResults();
        QCOMPARE(RModelCommandService::stateFingerprint(model), unboundHash);
        redo->trigger();
        QCOMPARE(changed.size(), 2); QCOMPARE(pickChanged.size(), 1);
        QCOMPARE(RModelCommandService::stateFingerprint(model), boundHash);
        QCOMPARE(model.getUndoStackSize(), 1u); QCOMPARE(model.getRedoStackSize(), 0u);
        QCOMPARE(nextPointIdentityProbe(model), nextIds);
        QCOMPARE(picks.getItems(0).size(), 2); QVERIFY(picks.hasItem(sourceSurface)); QVERIFY(picks.hasItem(sourceNode));
        QVERIFY(pickedReferencesAreValid(model, 0)); QVERIFY(redrawReferencesValid);
    }

    void staticBindingRejectsChangedReadSetAndFailedJournalBeforePublication()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        // The initial unbound fixture must not open the ordinary task-selection dialog.
        { const QSignalBlocker initialImport(session); session->addModel(unboundTetraFixture()); }
        const auto stamp = session->modelCommandStamp(0);
        const QString sourceHash = RModelCommandService::stateFingerprint(session->getModel(0));
        const auto preview = [&] {
            return host->handle(request("command.preview", {{"target", QJsonObject{
                {"documentId", stamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
                {"expectedDocumentRevision", double(session->modelCommandStamp(0).second)},
                {"command", "mesh.rebind.static"}, {"commandVersion", 1}, {"arguments", staticBindingArguments()}}));
        };
        const auto execute = [&](const QString &id) {
            return host->handle(request("command.execute", {{"previewId", id},
                {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        };
        auto response = preview(); QCOMPARE(response.value("status").toString(), QString("preview"));
        auto id = response.value("result").toObject().value("previewId").toString();
        QVERIFY(host->approvePreview(id));
        // A legacy edit can change fields without advancing the command stamp.
        session->getModel(0).getNode(3).set(0,0,2);
        QCOMPARE(execute(id).value("code").toString(), QString("STALE_READ_SET"));
        QCOMPARE(session->getModel(0).getNPoints(), 0u);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 0u);
        session->getModel(0).getNode(3).set(0,0,1);
        response = preview(); QCOMPARE(response.value("status").toString(), QString("preview"));
        id = response.value("result").toObject().value("previewId").toString();
        QVERIFY(host->approvePreview(id));
        session->invalidateModelCommands(0);
        QCOMPARE(execute(id).value("code").toString(), QString("STALE_REVISION"));
        response = preview(); QCOMPARE(response.value("status").toString(), QString("preview"));
        id = response.value("result").toObject().value("previewId").toString();
        QVERIFY(host->approvePreview(id));
        QFile journal(host->journalPath());
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const auto bytes = journal.readAll(); journal.close();
        struct RestoreJournal
        {
            QString path; QByteArray bytes;
            ~RestoreJournal() { QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(bytes); }
        } restore{host->journalPath(), bytes};
        QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(journal.write("invalid journal") > 0); journal.close();
        QCOMPARE(execute(id).value("code").toString(), QString("COMMAND_JOURNAL_FAILURE"));
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)), sourceHash);
        QCOMPARE(session->getModel(0).getNPoints(), 0u);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 0u);
    }

    void geometryPublicationPreservesGuiSourceResultsAndRoundTripsHistory()
    {
        static QTemporaryDir workspace;
        static QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        Model source = modalFixture();
        source.setNodePhysicalId(0, 41); source.setNodePhysicalId(1, 73);
        source.setNLines(1); source.getLine(0).add(0);
        RVariable result(R_VARIABLE_DISPLACEMENT, R_VARIABLE_APPLY_NODE);
        result.resize(3, 2); result.setValue(0, 1, 0.125);
        source.addVariable(result);
        const QString path = sources.filePath("geometry-roundtrip.ram");
        ModelIO::save(source, path, false);
        Model loaded; ModelIO::open(loaded, path);
        session->addModel(loaded);
        session->setModelSelected(0, true);
        const auto beforeStamp = session->modelCommandStamp(0);
        const QString fileName = session->getModel(0).getFileName();
        // Nonempty models store selection on entity groups, independently of
        // the model-level flag. Preserve both actual states across publication.
        const bool modelSelectedBefore = session->getModel(0).getSelected();
        const bool lineSelectedBefore = session->getModel(0).getSelected(R_ENTITY_GROUP_LINE, 0);
        QVERIFY(lineSelectedBefore);
        QVERIFY(session->getSelectedModelIDs().contains(0));
        QVERIFY(session->getModel(0).getLoadedSourceBinding().has_value());
        const auto binding = *session->getModel(0).getLoadedSourceBinding();
        const uint resultPosition = session->getModel(0).findVariable(R_VARIABLE_DISPLACEMENT);
        QVERIFY(resultPosition < session->getModel(0).getNVariables());
        const auto propose = [&](const QString &command, const QJsonObject &arguments) {
            return host->handle(request("command.preview", {
                {"target", QJsonObject{{"documentId", beforeStamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
                {"expectedDocumentRevision", double(session->modelCommandStamp(0).second)},
                {"command", command}, {"commandVersion", 1}, {"arguments", arguments}}));
        };
        // Material preview observes geometry even though it edits a different field.
        const auto material = propose("entity.material.set", {{"targetId", QString("g:%1").arg(session->getModel(0).getLine(0).getPhysicalId())},
            {"name", "Steel"}, {"youngModulusPa", 2e11}, {"densityKgM3", 7800.0},
            {"poissonRatio", 0.3}, {"thermalExpansionPerK", 1.2e-5}});
        QCOMPARE(material.value("status").toString(), QString("preview"));
        const auto materialId = material.value("result").toObject().value("previewId").toString();
        QVERIFY(host->approvePreview(materialId));
        const auto geometry = propose("geometry.node_coordinates.set", {{"unit", "m"}, {"directionPolicy", "preserve_global"},
            {"nodes", QJsonArray{QJsonObject{{"physicalNodeId", 73}, {"coordinates", QJsonArray{2, 0, 0}}}}}});
        QCOMPARE(geometry.value("status").toString(), QString("preview"));
        const QString previewId = geometry.value("result").toObject().value("previewId").toString();
        QSignalSpy modelChanged(session, &Session::modelChanged);
        QSignalSpy problemChanged(session, &Session::problemChanged);
        QVERIFY(host->approvePreview(previewId));
        const auto accepted = host->handle(request("command.execute", {{"previewId", previewId}, {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        QCOMPARE(accepted.value("status").toString(), QString("accepted"));
        QCOMPARE(modelChanged.size(), 1); QCOMPARE(problemChanged.size(), 0);
        QVERIFY(accepted.value("result").toObject().value("coreJournalRecorded").toBool());
        QCOMPARE(session->getModel(0).getNode(1).getX(), 2.0);
        QCOMPARE(session->getModel(0).getNode(0).getX(), 0.0);
        QCOMPARE(session->getModel(0).getFileName(), fileName);
        QVERIFY(session->getModel(0).getLoadedSourceBinding().has_value());
        QCOMPARE(session->getModel(0).getLoadedSourceBinding()->savedArtifactHash, binding.savedArtifactHash);
        QCOMPARE(session->getModel(0).getLoadedSourceBinding()->logicalSourcePath, binding.logicalSourcePath);
        QCOMPARE(session->getModel(0).getVariable(resultPosition).getValue(0, 1), 0.125);
        QCOMPARE(session->getModel(0).getSelected(), modelSelectedBefore);
        QCOMPARE(session->getModel(0).getSelected(R_ENTITY_GROUP_LINE, 0), lineSelectedBefore);
        QVERIFY(session->getSelectedModelIDs().contains(0));
        QCOMPARE(session->getModel(0).getUndoStackSize(), 1u);
        const auto stale = host->handle(request("command.execute", {{"previewId", materialId}, {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        QCOMPARE(stale.value("code").toString(), QString("STALE_REVISION"));
        quint64 revision = session->modelCommandStamp(0).second;
        for (const auto &command : {QString("document.undo"), QString("document.redo")})
        {
            const auto history = propose(command, {});
            QCOMPARE(history.value("status").toString(), QString("preview"));
            const auto id = history.value("result").toObject().value("previewId").toString();
            QVERIFY(host->approvePreview(id));
            const auto committed = host->handle(request(command, {{"previewId", id}, {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
            QCOMPARE(committed.value("status").toString(), QString("accepted"));
            QCOMPARE(session->getModel(0).getNode(1).getX(), command == "document.undo" ? 1.0 : 2.0);
            QVERIFY(session->modelCommandStamp(0).second > revision);
            revision = session->modelCommandStamp(0).second;
            QCOMPARE(session->getModel(0).getSelected(), modelSelectedBefore);
            QCOMPARE(session->getModel(0).getSelected(R_ENTITY_GROUP_LINE, 0), lineSelectedBefore);
            QVERIFY(session->getSelectedModelIDs().contains(0));
            QCOMPARE(session->getModel(0).getFileName(), fileName);
            QCOMPARE(session->getModel(0).getLoadedSourceBinding()->savedArtifactHash, binding.savedArtifactHash);
            QCOMPARE(session->getModel(0).getVariable(resultPosition).getValue(0, 1), 0.125);
        }
    }

    void irregularAxisPreviewReplaysExactCandidate()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        const auto stamp = session->modelCommandStamp(0);
        const QJsonObject arguments{{"unit", "m"}, {"angleUnit", "deg"},
            {"directionPolicy", "preserve_global"}, {"center", QJsonArray{0,0,0}},
            {"axis", QJsonArray{3,4,0}}, {"angle", 37}, {"translation", QJsonArray{0,0,0}}};
        const auto corePreview = RModelCommandService::preview(session->getModel(0),
            "geometry.rigid_transform", arguments, stamp.first, QString::number(stamp.second));
        QVERIFY2(corePreview.valid, qPrintable(corePreview.message));
        const QString preStateHash = RModelCommandService::stateFingerprint(session->getModel(0));
        const QString postStateHash = RModelCommandService::stateFingerprint(corePreview.candidate);
        QVERIFY(preStateHash != postStateHash);
        const auto preview = host->handle(request("command.preview", {
            {"target", QJsonObject{{"documentId", stamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
            {"expectedDocumentRevision", double(stamp.second)}, {"command", "geometry.rigid_transform"},
            {"commandVersion", 1}, {"arguments", arguments}}));
        QCOMPARE(preview.value("status").toString(), QString("preview"));
        const auto record = preview.value("result").toObject();
        QCOMPARE(record.value("candidateFingerprint").toString(), corePreview.fingerprint);
        QCOMPARE(record.value("inputStateFingerprint").toString(), preStateHash);
        QCOMPARE(record.value("before").toObject(), corePreview.before);
        QCOMPARE(record.value("after").toObject(), corePreview.after);
        const auto id = record.value("previewId").toString();
        QVERIFY(host->approvePreview(id));
        const auto accepted = host->handle(request("command.execute", {{"previewId", id},
            {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        QCOMPARE(accepted.value("status").toString(), QString("accepted"));
        const auto receipt = accepted.value("result").toObject();
        // The candidate fingerprint hashes the source, command and before/after
        // payload; receipt state hashes cover only the supported model state.
        QCOMPARE(receipt.value("candidateFingerprint"), record.value("candidateFingerprint"));
        QCOMPARE(receipt.value("preStateHash").toString(), preStateHash);
        QCOMPARE(receipt.value("postStateHash").toString(), postStateHash);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)), postStateHash);
        for (const auto &change : corePreview.delta.geometry.nodes)
        {
            const uint position = session->getModel(0).findNodePositionByPhysicalId(change.physicalNodeId);
            QVERIFY(position < session->getModel(0).getNNodes());
            const auto &node = session->getModel(0).getNode(position);
            QCOMPARE(node.getX(), change.afterXYZ[0]);
            QCOMPARE(node.getY(), change.afterXYZ[1]);
            QCOMPARE(node.getZ(), change.afterXYZ[2]);
        }
        QVERIFY(session->modelCommandStamp(0).second > stamp.second);
    }

    void geometryRejectsExtraArgumentsNoOpAndJournalFailureWithoutMutation()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        const auto stamp = session->modelCommandStamp(0);
        const auto propose = [&](const QJsonObject &arguments) {
            return host->handle(request("command.preview", {
                {"target", QJsonObject{{"documentId", stamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
                {"expectedDocumentRevision", double(stamp.second)}, {"command", "geometry.rigid_transform"},
                {"commandVersion", 1}, {"arguments", arguments}}));
        };
        QJsonObject transform{{"unit", "m"}, {"angleUnit", "deg"}, {"directionPolicy", "preserve_global"},
            {"center", QJsonArray{0, 0, 0}}, {"axis", QJsonArray{0, 0, 1}},
            {"angle", 360}, {"translation", QJsonArray{0, 0, 0}}};
        const auto emptyHistory = host->handle(request("command.preview", {
            {"target", QJsonObject{{"documentId", stamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
            {"expectedDocumentRevision", double(stamp.second)}, {"command", "document.undo"},
            {"commandVersion", 1}, {"arguments", QJsonObject{}}}));
        QCOMPARE(emptyHistory.value("code").toString(), QString("EMPTY_HISTORY"));
        QCOMPARE(session->modelCommandStamp(0), stamp);
        Model unsupported = modalFixture();
        unsupported.setNNodes(3); unsupported.setNode(2, RNode(0, 1, 0));
        RElement triangle(R_ELEMENT_TRI1);
        triangle.setNodeId(0, 0); triangle.setNodeId(1, 1); triangle.setNodeId(2, 2);
        unsupported.setElement(0, triangle);
        session->addModel(unsupported);
        const auto unsupportedStamp = session->modelCommandStamp(1);
        const auto refused = host->handle(request("command.preview", {
            {"target", QJsonObject{{"documentId", unsupportedStamp.first}, {"sessionEpoch", host->sessionEpoch()}}},
            {"expectedDocumentRevision", double(unsupportedStamp.second)}, {"command", "geometry.rigid_transform"},
            {"commandVersion", 1}, {"arguments", transform}}));
        QCOMPARE(refused.value("code").toString(), QString("INVALID_SOURCE_GEOMETRY"));
        QCOMPARE(session->modelCommandStamp(1), unsupportedStamp);
        session->removeModel(1);
        QJsonObject bad = transform; bad.insert("scale", 2);
        QCOMPARE(propose(bad).value("code").toString(), QString("INVALID_ARGUMENTS"));
        bad = transform; bad.insert("unit", "mm");
        QCOMPARE(propose(bad).value("code").toString(), QString("INVALID_ARGUMENTS"));
        transform.insert("axis", QJsonArray{3, 4, 0});
        const auto noOp = propose(transform);
        QCOMPARE(noOp.value("status").toString(), QString("preview"));
        const auto noOpPreview = noOp.value("result").toObject();
        QVERIFY(noOpPreview.value("changedPaths").toArray().isEmpty());
        const QJsonArray submittedAxis{3, 4, 0};
        QCOMPARE(noOpPreview.value("normalizedArguments").toObject().value("axis").toArray(), submittedAxis);
        const QString noOpStateHash = RModelCommandService::stateFingerprint(session->getModel(0));
        QCOMPARE(noOpPreview.value("inputStateFingerprint").toString(), noOpStateHash);
        // A no-op retains the geometry hash and before/after payload even though
        // its command/source-bound candidate fingerprint has a different scope.
        QCOMPARE(noOpPreview.value("before").toObject(), noOpPreview.value("after").toObject());
        QVERIFY(!noOpPreview.value("before").toObject().value("geometrySha256").toString().isEmpty());
        const auto noOpCore = RModelCommandService::preview(session->getModel(0),
            "geometry.rigid_transform", transform, stamp.first, QString::number(stamp.second));
        QVERIFY2(noOpCore.valid, qPrintable(noOpCore.message));
        QVERIFY(noOpCore.delta.geometry.noOp);
        QCOMPARE(noOpPreview.value("candidateFingerprint").toString(), noOpCore.fingerprint);
        QCOMPARE(RModelCommandService::stateFingerprint(noOpCore.candidate), noOpStateHash);
        const auto noOpId = noOpPreview.value("previewId").toString();
        QVERIFY(host->approvePreview(noOpId));
        QFile journal(host->journalPath()); QVERIFY(journal.open(QIODevice::ReadOnly));
        const auto journalBefore = journal.readAll(); journal.close();
        const auto accepted = host->handle(request("command.execute", {{"previewId", noOpId}, {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        QCOMPARE(accepted.value("status").toString(), QString("accepted"));
        const auto noOpResult = accepted.value("result").toObject();
        QVERIFY(noOpResult.value("noOp").toBool());
        QVERIFY(noOpResult.value("changedPaths").toArray().isEmpty());
        QCOMPARE(noOpResult.value("history").toString(), QString("none"));
        QCOMPARE(noOpResult.value("preStateHash").toString(), noOpPreview.value("inputStateFingerprint").toString());
        QCOMPARE(noOpResult.value("postStateHash").toString(), noOpStateHash);
        QCOMPARE(noOpResult.value("preStateHash"), noOpResult.value("postStateHash"));
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)), noOpStateHash);
        QCOMPARE(session->modelCommandStamp(0), stamp);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 0u);
        QCOMPARE(session->getModel(0).getRedoStackSize(), 0u);
        QVERIFY(journal.open(QIODevice::ReadOnly)); QCOMPARE(journal.readAll(), journalBefore); journal.close();

        transform.insert("translation", QJsonArray{0, 1, 0});
        const auto preview = propose(transform);
        QCOMPARE(preview.value("status").toString(), QString("preview"));
        const auto id = preview.value("result").toObject().value("previewId").toString();
        QVERIFY(host->approvePreview(id));
        QVERIFY(journal.open(QIODevice::ReadOnly));
        const auto original = journal.readAll(); journal.close();
        struct RestoreJournal
        {
            QString path; QByteArray bytes;
            ~RestoreJournal() { QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(bytes); }
        } restore{host->journalPath(), original};
        QVERIFY(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(journal.write("invalid journal") > 0); journal.close();
        const auto rejected = host->handle(request("command.execute", {{"previewId", id}, {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)}}));
        QCOMPARE(rejected.value("code").toString(), QString("COMMAND_JOURNAL_FAILURE"));
        QCOMPARE(session->modelCommandStamp(0), stamp);
        QCOMPARE(session->getModel(0).getNode(0).getY(), 0.0);
        QCOMPARE(session->getModel(0).getNode(1).getY(), 0.0);
        QCOMPARE(session->getModel(0).getUndoStackSize(), 0u);
    }

    void sourceFileConflictRejectsMatchingOriginWithoutRuntimeEdit()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        const QString sourcePath = workspace.filePath(QStringLiteral("external-source.ram"));
        Model source = modalFixture();
        ModelIO::save(source, sourcePath, false);
        Model loaded;
        ModelIO::open(loaded, sourcePath);
        session->addModel(loaded);
        const uint modelID = session->getNModels() - 1;
        const QJsonObject snapshot = session->createDocumentSnapshot(modelID, &error);
        QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
        const QJsonObject origin = sourceOrigin(snapshot);
        const QPair<QString, quint64> runtimeStamp = session->modelCommandStamp(modelID);
        QCOMPARE(host->handle(request(QStringLiteral("document.source.validate"), {{QStringLiteral("snapshotRef"), origin}}))
                     .value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

        // Change the real persisted source independently of Session. Its
        // document revision and cached accepted snapshot fields remain equal
        // to the registered origin; freshness depends on conflict availability.
        QFile externalWriter(sourcePath);
        QVERIFY(externalWriter.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(externalWriter.write("\n"), qint64(1));
        externalWriter.close();
        const QJsonObject conflict = session->documentProjectContext(modelID);
        QCOMPARE(session->modelCommandStamp(modelID), runtimeStamp);
        QVERIFY(conflict.value(QStringLiteral("sourceConflict")).toBool());
        QCOMPARE(conflict.value(QStringLiteral("availability")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(conflict.value(QStringLiteral("persistence")).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("conflict"));
        for (const QString &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
             QStringLiteral("sourceRevision"), QStringLiteral("modelSha256")})
            QCOMPARE(conflict.value(field), origin.value(field));
        const QJsonObject rejected = host->handle(request(QStringLiteral("document.source.validate"), {{QStringLiteral("snapshotRef"), origin}}));
        QCOMPARE(rejected.value(QStringLiteral("code")).toString(), QStringLiteral("SOURCE_UNAVAILABLE"));
        QCOMPARE(session->modelCommandStamp(modelID), runtimeStamp);
        session->removeModel(modelID);
    }

    void sourceValidationRequiresCurrentDocumentWithoutDraftAndExactOrigin()
    {
        // Keep the project alive until process exit because Session retains its
        // catalog after individual documents close.
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        const uint modelID = session->getNModels() - 1;
        const QJsonObject snapshot = session->createDocumentSnapshot(modelID, &error);
        QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
        const QJsonObject origin = sourceOrigin(snapshot);
        const auto validate = [host](const QJsonObject &reference)
        {
            return host->handle(request(QStringLiteral("document.source.validate"), {{QStringLiteral("snapshotRef"), reference}}));
        };
        const QJsonObject current = validate(origin);
        QCOMPARE(current.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(current.value(QStringLiteral("result")).toObject().value(QStringLiteral("valid")).toBool());
        QCOMPARE(current.value(QStringLiteral("result")).toObject().value(QStringLiteral("snapshotRef")).toObject(), origin);

        QJsonObject earlierSession = origin;
        QJsonObject instance = origin.value(QStringLiteral("instanceRef")).toObject();
        instance.insert(QStringLiteral("sessionEpoch"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        earlierSession.insert(QStringLiteral("instanceRef"), instance);
        QCOMPARE(validate(earlierSession).value(QStringLiteral("code")).toString(), QStringLiteral("STALE_SESSION"));
        QJsonObject wrongHash = origin;
        wrongHash.insert(QStringLiteral("modelSha256"), QString(64, QLatin1Char('0')));
        QCOMPARE(validate(wrongHash).value(QStringLiteral("code")).toString(), QStringLiteral("STALE_SOURCE"));
        QJsonObject arbitraryPath = origin;
        arbitraryPath.insert(QStringLiteral("path"), workspace.filePath(QStringLiteral("model.ram")));
        QCOMPARE(validate(arbitraryPath).value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_PARAMETERS"));

        session->setNumericDraft(modelID, QStringLiteral("analysis.modal"), QStringLiteral("iterations"),
                                 QStringLiteral("invalid"), QStringLiteral("unfinished-value"));
        QVERIFY(session->hasDocumentDraft(modelID));
        QCOMPARE(validate(origin).value(QStringLiteral("code")).toString(), QStringLiteral("STALE_SOURCE"));
        session->clearNumericDraft(modelID, QStringLiteral("analysis.modal"), QStringLiteral("iterations"));
        QVERIFY(!session->hasDocumentDraft(modelID));
        QCOMPARE(validate(origin).value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

        session->getModel(modelID).getProblemSetup().getModalSetup().setNIterations(19);
        session->setProblemChanged(modelID);
        QCOMPARE(validate(origin).value(QStringLiteral("code")).toString(), QStringLiteral("STALE_SOURCE"));
        const QJsonObject nextSnapshot = session->createDocumentSnapshot(modelID, &error);
        QVERIFY2(!nextSnapshot.isEmpty(), qPrintable(error));
        const QJsonObject nextOrigin = sourceOrigin(nextSnapshot);
        QCOMPARE(validate(nextOrigin).value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        session->removeModel(modelID);
        QCOMPARE(validate(nextOrigin).value(QStringLiteral("code")).toString(), QStringLiteral("DOCUMENT_CLOSED"));
    }

    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        Application::instance()->getSession()->clear();
    }

    void versionedContextAndCapabilities()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        const auto stamp = session->modelCommandStamp(0);

        const auto capabilities = host->handle(request(QStringLiteral("capabilities.query")));
        QCOMPARE(capabilities.value(QStringLiteral("apiVersion")).toString(), QStringLiteral("range.host/v1"));
        QCOMPARE(capabilities.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const auto capabilityResult = capabilities.value(QStringLiteral("result")).toObject();
        const QString epoch = capabilityResult.value(QStringLiteral("sessionEpoch")).toString();
        QVERIFY(!epoch.isEmpty());
        QCOMPARE(capabilityResult.value(QStringLiteral("identityPersistence")).toString(), QStringLiteral("host-session"));
        bool hasPreview = false;
        bool hasSourceValidation = false;
        for (const auto &value : capabilityResult.value(QStringLiteral("supportedOperations")).toArray())
        {
            hasPreview |= value.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("command.preview");
            hasSourceValidation |= value.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("document.source.validate");
        }
        QVERIFY(hasPreview);
        QVERIFY(hasSourceValidation);

        const auto contextResponse = host->handle(request(QStringLiteral("context.query"),
            {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), stamp.first},
                                                    {QStringLiteral("sessionEpoch"), epoch}}}}));
        QCOMPARE(contextResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const auto contextResult = contextResponse.value(QStringLiteral("result")).toObject();
        QCOMPARE(contextResult.value(QStringLiteral("sessionEpoch")).toString(), epoch);
        QCOMPARE(contextResult.value(QStringLiteral("identityPersistence")).toString(), QStringLiteral("host-session"));
        QCOMPARE(contextResult.value(QStringLiteral("documents")).toArray().size(), 1);
        const auto document = contextResult.value(QStringLiteral("documents")).toArray().first().toObject();
        QCOMPARE(document.value(QStringLiteral("documentId")).toString(), stamp.first);
        bool hasModalCommand = false;
        for (const auto &value : document.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("supportedOperations")).toArray())
            hasModalCommand |= value.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("analysis.modal.configure");
        QVERIFY(hasModalCommand);
        QCOMPARE(document.value(QStringLiteral("documentRevision")).toDouble(), double(stamp.second));
        QCOMPARE(document.value(QStringLiteral("revisionScope")).toString(), QStringLiteral("session-command-invalidation-stamp"));
        QCOMPARE(document.value(QStringLiteral("stateAvailability")).toString(), QStringLiteral("live"));
        QCOMPARE(document.value(QStringLiteral("modelName")).toString(), QStringLiteral("Host context fixture"));
        QCOMPARE(document.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("scope")).toString(), QStringLiteral("analysis.modal"));
        QCOMPARE(document.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("modal")).toObject()
                     .value(QStringLiteral("iterations")).toInt(), 12);
        QCOMPARE(document.value(QStringLiteral("draft")).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("none"));
        QCOMPARE(document.value(QStringLiteral("draft")).toObject().value(QStringLiteral("scope")).toString(), QStringLiteral("analysis.modal"));
        QCOMPARE(document.value(QStringLiteral("draft")).toObject().value(QStringLiteral("otherScopesStatus")).toString(), QStringLiteral("unknown"));
        QCOMPARE(document.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("unsaved"));
        QVERIFY(document.value(QStringLiteral("unsaved")).toObject().value(QStringLiteral("dirty")).toBool());
    }

    void approvedModalCommandIsVersionBoundAndIdempotent()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        const auto identity = session->modelCommandStamp(0).first;
        const auto capability = host->handle(request(QStringLiteral("capabilities.query"))).value(QStringLiteral("result")).toObject();
        const QString epoch = capability.value(QStringLiteral("sessionEpoch")).toString();
        const auto query = host->handle(request(QStringLiteral("context.query"),
            {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), identity},
                                                    {QStringLiteral("sessionEpoch"), epoch}}}}));
        const auto current = query.value(QStringLiteral("result")).toObject().value(QStringLiteral("documents")).toArray().first().toObject();
        auto arguments = current.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("modal")).toObject();
        arguments.insert(QStringLiteral("iterations"), 31);
        const auto previewResponse = host->handle(request(QStringLiteral("command.preview"),
            {{QStringLiteral("target"), targetFor({{QStringLiteral("documentId"), identity}, {QStringLiteral("sessionEpoch"), epoch}})},
             {QStringLiteral("expectedDocumentRevision"), current.value(QStringLiteral("documentRevision"))},
             {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
             {QStringLiteral("commandVersion"), 1}, {QStringLiteral("arguments"), arguments}}));
        QCOMPARE(previewResponse.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const auto preview = previewResponse.value(QStringLiteral("result")).toObject();
        QVERIFY(!preview.value(QStringLiteral("previewId")).toString().isEmpty());
        QVERIFY(!preview.value(QStringLiteral("digest")).toString().isEmpty());
        QCOMPARE(preview.value(QStringLiteral("changedPaths")).toArray().first().toString(), QStringLiteral("analysis.modal"));

        const auto executeRequest = [&]() {
            return request(QStringLiteral("command.execute"),
                {{QStringLiteral("previewId"), preview.value(QStringLiteral("previewId"))},
                 {QStringLiteral("idempotencyKey"), QStringLiteral("host-context-edit-1")}});
        };
        auto unauthorized = host->handle(executeRequest());
        QCOMPARE(unauthorized.value(QStringLiteral("code")).toString(), QStringLiteral("AUTHORIZATION_REQUIRED"));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);

        QVERIFY(host->approvePreview(preview.value(QStringLiteral("previewId")).toString()));
        const auto accepted = host->handle(executeRequest());
        QCOMPARE(accepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const auto operation = accepted.value(QStringLiteral("result")).toObject();
        QVERIFY(!operation.value(QStringLiteral("operationId")).toString().isEmpty());
        QCOMPARE(operation.value(QStringLiteral("documentId")).toString(), identity);
        QCOMPARE(operation.value(QStringLiteral("persistence")).toString(), QStringLiteral("accepted_snapshot"));
        const auto acceptedSnapshot = operation.value(QStringLiteral("acceptedSnapshotRef")).toObject();
        QVERIFY(!acceptedSnapshot.value(QStringLiteral("modelSha256")).toString().isEmpty());
        QVERIFY(!acceptedSnapshot.value(QStringLiteral("sourceRevision")).toString().isEmpty());
        QCOMPARE(acceptedSnapshot.value(QStringLiteral("modelSha256")),
                 session->documentProjectContext(0).value(QStringLiteral("modelSha256")));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 31u);

        const auto duplicate = host->handle(executeRequest());
        QCOMPARE(duplicate.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(duplicate.value(QStringLiteral("result")).toObject().value(QStringLiteral("operationId")).toString(),
                 operation.value(QStringLiteral("operationId")).toString());
        const auto operationQuery = host->handle(request(QStringLiteral("operation.query"),
            {{QStringLiteral("operationId"), operation.value(QStringLiteral("operationId"))}}));
        QCOMPARE(operationQuery.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(operationQuery.value(QStringLiteral("result")).toObject().value(QStringLiteral("operationId")).toString(),
                 operation.value(QStringLiteral("operationId")).toString());

        const auto nextContext = host->handle(request(QStringLiteral("context.query"),
            {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), identity},
                                                    {QStringLiteral("sessionEpoch"), epoch}}}}));
        const auto nextDocument = nextContext.value(QStringLiteral("result")).toObject().value(QStringLiteral("documents")).toArray().first().toObject();
        const auto undoPreview = host->handle(request(QStringLiteral("command.preview"),
            {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), identity}, {QStringLiteral("sessionEpoch"), epoch}}},
             {QStringLiteral("expectedDocumentRevision"), nextDocument.value(QStringLiteral("documentRevision"))},
             {QStringLiteral("command"), QStringLiteral("document.undo")},
             {QStringLiteral("commandVersion"), 1}, {QStringLiteral("arguments"), QJsonObject{}}}));
        QCOMPARE(undoPreview.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString undoPreviewId = undoPreview.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")).toString();
        QVERIFY(host->approvePreview(undoPreviewId));
        const auto undoAccepted = host->handle(request(QStringLiteral("document.undo"),
            {{QStringLiteral("previewId"), undoPreviewId}, {QStringLiteral("idempotencyKey"), QStringLiteral("host-context-undo-1")}}));
        QCOMPARE(undoAccepted.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
    }

    void orderedEventsAreBoundedAndRequestResynchronization()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelectionChanged(0);
        const auto initial = host->handle(request(QStringLiteral("events.read"),
            {{QStringLiteral("afterEventSeq"), 0}, {QStringLiteral("limit"), 100}}));
        const auto initialResult = initial.value(QStringLiteral("result")).toObject();
        QVERIFY(!initialResult.value(QStringLiteral("events")).toArray().isEmpty());
        quint64 previous = 0;
        for (const auto &eventValue : initialResult.value(QStringLiteral("events")).toArray())
        {
            const quint64 current = quint64(eventValue.toObject().value(QStringLiteral("eventSeq")).toDouble());
            QVERIFY(current > previous);
            previous = current;
        }

        for (int i = 0; i < 300; ++i) session->setModelSelectionChanged(0);
        const auto overflow = host->handle(request(QStringLiteral("events.read"),
            {{QStringLiteral("afterEventSeq"), 0}, {QStringLiteral("limit"), 100}}));
        const auto overflowResult = overflow.value(QStringLiteral("result")).toObject();
        QVERIFY(overflowResult.value(QStringLiteral("resyncRequired")).toBool());
        QVERIFY(overflowResult.value(QStringLiteral("events")).toArray().isEmpty());
        QVERIFY(overflowResult.value(QStringLiteral("latestEventSeq")).toDouble() > 256.0);
    }
};

int runGuiHostContextTests(int argc, char **argv)
{
    TestGuiHostContext test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_gui_host_context.moc"
