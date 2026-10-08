#include <QtTest>
#include <QUuid>
#include <QTemporaryDir>
#include <QFile>

#include <rml_model.h>

#include "application.h"
#include "model_command_host.h"
#include "session.h"
#include "model_io.h"

namespace
{
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
        QCOMPARE(operation.value(QStringLiteral("persistence")).toString(), QStringLiteral("memory_only"));
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
