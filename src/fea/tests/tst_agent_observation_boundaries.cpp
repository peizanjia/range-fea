#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QLockFile>
#include <QMetaMethod>
#include <QSet>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QVariant>

#include <ragent_conversation_store.h>
#include <rai_agent.h>
#include <rml_model.h>

#include "agent_task_runner.h"
#include "agent_widget.h"
#include "application.h"
#include "model_command_host.h"
#include "session.h"
#include "workflow_process.h"

#include <functional>

namespace
{
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

Model modalFixture()
{
    Model model;
    model.setName(QStringLiteral("Agent observation boundary fixture"));
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

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), newId()},
            {QStringLiteral("operation"), operation},
            {QStringLiteral("parameters"), parameters}};
}

QJsonObject providerCall(const QString &id, const QString &name, const QJsonObject &arguments)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("arguments"),
                 QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}};
}

QJsonObject providerTurn(const QString &text, const QJsonArray &calls = {})
{
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                        {QStringLiteral("content"), text}};
    if (!calls.isEmpty()) message.insert(QStringLiteral("tool_calls"), calls);
    const QJsonObject response{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("message"), message},
        {QStringLiteral("finish_reason"), calls.isEmpty()
            ? QStringLiteral("stop") : QStringLiteral("tool_calls")}}}},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 4},
            {QStringLiteral("completion_tokens"), 3}, {QStringLiteral("total_tokens"), 7}}}};
    QJsonObject turn;
    QString error;
    if (!RAgent::parseToolResponse(QJsonDocument(response).toJson(QJsonDocument::Compact), &turn, &error))
        qFatal("Observation-boundary provider fixture was rejected: %s", qPrintable(error));
    return turn;
}

QJsonObject modalProposal(Session *session, uint modelId, uint iterations)
{
    if (!session) return {};
    const auto stamp = session->modelCommandStamp(modelId);
    return {{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("method"), QStringLiteral("multiple")},
                {QStringLiteral("iterations"), int(iterations)},
                {QStringLiteral("modes"), 2},
                {QStringLiteral("tolerance"), 1e-6},
                {QStringLiteral("unit"), QStringLiteral("1")}}}};
}

QJsonObject selectedDocument(ModelCommandHost *host)
{
    if (!host) return {};
    const QJsonObject response = host->handle(hostRequest(QStringLiteral("context.query")));
    const QJsonArray documents = response.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("documents")).toArray();
    return documents.isEmpty() ? QJsonObject{} : documents.first().toObject();
}

QJsonObject commandPreview(Session *session, const QString &documentId,
                           const QString &command, const QJsonObject &arguments)
{
    if (!session) return {};
    const uint modelId = session->findModelByCommandIdentity(documentId);
    if (modelId == RConstants::eod) return {};
    const auto stamp = session->modelCommandStamp(modelId);
    const QJsonObject parameters{
        {QStringLiteral("target"), QJsonObject{
            {QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), session->getModelCommandHost()->sessionEpoch()}}},
        {QStringLiteral("expectedDocumentRevision"), double(stamp.second)},
        {QStringLiteral("command"), command},
        {QStringLiteral("commandVersion"), 1},
        {QStringLiteral("arguments"), arguments}};
    return session->getModelCommandHost()->handle(
        hostRequest(QStringLiteral("command.preview"), parameters));
}

bool openProjectWithModal(Session *session, const QString &workspace, uint *modelId, QString *error)
{
    if (!session || !modelId) return false;
    session->clear();
    if (!session->openProject(workspace, error)) return false;
    session->addModel(modalFixture());
    *modelId = session->getNModels() - 1;
    session->setModelSelected(*modelId, true);
    return true;
}

QString toolResultForMessages(const QJsonArray &messages, const QString &callId)
{
    for (const QJsonValue &value : messages)
    {
        const QJsonObject message = value.toObject();
        if (message.value(QStringLiteral("role")) == QJsonValue(QStringLiteral("tool"))
            && message.value(QStringLiteral("tool_call_id")) == QJsonValue(callId))
            return message.value(QStringLiteral("content")).toString();
    }
    return {};
}

bool configureStore(RAgentConversationStore *store, ModelCommandHost *host,
                    QString *projectId, QString *error)
{
    if (!store || !host) return false;
    const QJsonObject identity = host->handle(hostRequest(QStringLiteral("project.query")))
        .value(QStringLiteral("result")).toObject();
    const QString id = identity.value(QStringLiteral("projectId")).toString();
    const QString workspace = host->projectWorkspace();
    if (id.isEmpty() || workspace.isEmpty()) return false;
    if (projectId) *projectId = id;
    return store->configure(workspace, id, error);
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(1);
    }
    return condition();
}

bool waitForWorkflowReply(WorkflowProcess *workflow, const QString &requestId,
                          const QString &operation, QJsonObject *result, int timeoutMs = 5000)
{
    if (!workflow || requestId.isEmpty()) return false;
    bool received = false;
    bool matched = false;
    QJsonObject reply;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(workflow, &WorkflowProcess::replyReceived, &loop,
        [&](const QString &id, const QString &replyOperation, const QJsonObject &value) {
            if (id != requestId) return;
            received = true;
            matched = replyOperation == operation;
            reply = value;
            loop.quit();
        });
    QObject::connect(workflow, &WorkflowProcess::requestFailed, &loop,
        [&](const QString &id, const QString &, const QString &, const QString &) {
            if (id != requestId) return;
            received = true;
            loop.quit();
        });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    if (result) *result = reply;
    return received && matched;
}

bool createWorkflowProtocolFixture(const QString &root, QString *pythonPath, QString *error)
{
    QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty()) python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        if (error) *error = QStringLiteral("A Python interpreter is required for the WorkflowProcess protocol fixture.");
        return false;
    }
    const QString packageRoot = QDir(root).filePath(QStringLiteral("fixture-package"));
    const QString moduleDirectory = QDir(packageRoot).filePath(QStringLiteral("range_workflow"));
    const QString examples = QDir(root).filePath(QStringLiteral("examples"));
    if (!QDir().mkpath(moduleDirectory) || !QDir().mkpath(examples)
        || !QDir().mkpath(QDir(root).filePath(QStringLiteral("workspace"))))
    {
        if (error) *error = QStringLiteral("Could not create the isolated WorkflowProcess fixture paths.");
        return false;
    }
    const QByteArray source = R"PY(import hashlib,json,sys,uuid
P="00000000-0000-4000-8000-000000000001"
previews={}
receipts={}
def send(q,s,c,r):
 print(json.dumps({"apiVersion":"range.workflow/v1","requestId":q["requestId"],"status":s,"code":c,"result":r},separators=(",",":")),flush=True)
for line in sys.stdin:
 q=json.loads(line); op=q["operation"]; a=q.get("parameters",{})
 if op=="capabilities.query": send(q,"ok","OK",{"apiVersion":"range.workflow/v1"})
 elif op=="project.query": send(q,"ok","OK",{"projectId":P,"revision":0})
 elif op=="command.preview":
  i=str(uuid.uuid4()); h=hashlib.sha256(i.encode()).hexdigest(); previews[i]=h
  send(q,"ok","OK",{"previewId":i,"previewHash":h,"projectId":P,"revision":0,
   "normalized":{"command":a["command"],"parameters":a["parameters"]},
   "effectiveConfiguration":{},"modelBinding":{"kind":"template","usesOpenDocument":False}})
 elif op=="host.approve": send(q,"ok","OK",{"previewId":a["previewId"],"approvalToken":"fixture-approval"})
 elif op=="command.execute":
  h=previews[a["previewId"]]
  receipts[h]={"projectId":P,"requestSha256":h,"accepted":True,"result":{"operationId":"fixture-"+h[:16]}}
  if len(receipts)==68:
   send(q,"ok","OK",{"jobId":"fixture-confirmed-job"}); continue
  # Retain acceptance, then return the structured lost-outcome response.
  send(q,"error","RESPONSE_LOST_AFTER_ACCEPTANCE",{"message":"Receipt is available by lookup."})
 elif op=="execution.query":
  h=a["requestSha256"]; send(q,"ok","OK",receipts.get(h,{"projectId":P,"requestSha256":h,"accepted":False}))
)PY";
    QFile module(QDir(moduleDirectory).filePath(QStringLiteral("__main__.py")));
    if (!module.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || module.write(source) != source.size() || !module.flush())
    {
        if (error) *error = QStringLiteral("Could not write the isolated WorkflowProcess protocol fixture.");
        return false;
    }
    const QString solver = QDir(root).filePath(QStringLiteral("fixture-solver.exe"));
    const QString native = QDir(root).filePath(QStringLiteral("fixture-native.exe"));
    for (const QString &path : {solver, native})
    {
        QFile placeholder(path);
        if (!placeholder.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            || placeholder.write("protocol fixture") < 0 || !placeholder.flush())
        {
            if (error) *error = QStringLiteral("Could not create an isolated WorkflowProcess validation placeholder.");
            return false;
        }
    }
    if (pythonPath) *pythonPath = python;
    return true;
}
}

class TestAgentObservationBoundaries : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        QVERIFY(application->getSession());
        application->getSession()->clear();
    }

    void cleanup()
    {
        Application *application = Application::instance();
        if (application && application->getSession()) application->getSession()->clear();
    }

    void recorderSourceChangeRejectsWithoutEffectAndReleasesDurableReservation()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);
        const QJsonObject initialDocument = selectedDocument(host);
        QVERIFY(!initialDocument.value(QStringLiteral("documentId")).toString().isEmpty());

        RAgentConversationStore store;
        QString projectId;
        QVERIFY2(configureStore(&store, host, &projectId, &error), qPrintable(error));
        const QJsonObject created = store.create(QStringLiteral("Observation boundary"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString conversationId = created.value(QStringLiteral("conversationId")).toString();
        QString revision = created.value(QStringLiteral("revision")).toString();

        for (int offset = 0; offset < 510; offset += RAgentConversationStore::MaxPageSize)
        {
            QJsonArray batch;
            const int count = qMin(RAgentConversationStore::MaxPageSize, 510 - offset);
            for (int index = 0; index < count; ++index)
                batch.append(QJsonObject{{QStringLiteral("messageId"), newId()},
                    {QStringLiteral("role"), QStringLiteral("user")},
                    {QStringLiteral("text"), QStringLiteral("bounded capacity fixture %1").arg(offset + index)}});
            const QJsonObject appended = store.append(conversationId, revision, batch, &error);
            QVERIFY2(!appended.isEmpty(), qPrintable(error));
            revision = appended.value(QStringLiteral("revision")).toString();
        }
        QCOMPARE(store.page(conversationId, 0, 1).value(QStringLiteral("messageCount")).toInt(), 510);

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requests(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy unknown(&runner, &AgentTaskRunner::executionObservationUnknown);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        QString intentRevision;
        QString taskId;
        QJsonObject safeReference;

        runner.setExecutionRecorder([&](const QString &recordedTask, const QString &origin,
                                        const QJsonObject &reference) {
            if (origin != QStringLiteral("document")) return false;
            taskId = recordedTask;
            safeReference = reference;
            QJsonObject facts = reference;
            facts.insert(QStringLiteral("status"), QStringLiteral("pending"));
            facts.insert(QStringLiteral("code"), QStringLiteral("OUTCOME_NOT_OBSERVED"));
            const QJsonObject entry{{QStringLiteral("messageId"), newId()},
                {QStringLiteral("taskId"), recordedTask}, {QStringLiteral("role"), QStringLiteral("fact")},
                {QStringLiteral("text"), QStringLiteral("Execution intent recorded before document approval.")},
                {QStringLiteral("facts"), facts}};
            const QJsonObject summary = store.append(conversationId, revision, QJsonArray{entry}, &error);
            if (summary.isEmpty()) return false;
            revision = summary.value(QStringLiteral("revision")).toString();
            intentRevision = revision;

            // Advance the real Session revision before AgentTaskRunner approval.
            session->getModel(modelId).setDescription(
                QStringLiteral("Source changed inside the trusted recorder."));
            session->setModelChanged(modelId);
            return true;
        });
        connect(&runner, &AgentTaskRunner::executionObservationUnknown, &runner,
            [&](const QString &observedTask, const QString &, const QJsonObject &reference, const QString &code) {
                if (observedTask != taskId || code != QStringLiteral("NOT_SUBMITTED")) return;
                QJsonObject facts = reference;
                facts.insert(QStringLiteral("status"), QStringLiteral("rejected"));
                facts.insert(QStringLiteral("code"), code);
                const QJsonObject entry{{QStringLiteral("messageId"), newId()},
                    {QStringLiteral("taskId"), observedTask}, {QStringLiteral("role"), QStringLiteral("fact")},
                    {QStringLiteral("text"), QStringLiteral("The changed source prevented submission.")},
                    {QStringLiteral("facts"), facts}};
                const QJsonObject summary = store.append(conversationId, revision, QJsonArray{entry}, &error);
                if (!summary.isEmpty()) revision = summary.value(QStringLiteral("revision")).toString();
                runner.resolveExecutionObservation(observedTask, reference);
            });

        runner.startTask(QStringLiteral("Prepare a reviewed modal change."));
        QCOMPARE(requests.size(), 1);
        const QJsonObject proposal = modalProposal(session, modelId, 25);
        runner.receiveTurn(requests.first().at(0).toString(), providerTurn(QString(), QJsonArray{
            providerCall(QStringLiteral("stale-source-proposal"),
                         QStringLiteral("document_propose"), proposal)}));
        QCOMPARE(proposals.size(), 1);
        const QString proposalId = proposals.first().at(1).toString();
        QVERIFY(!proposalId.isEmpty());
        runner.authorizePending(proposalId, true);

        QCOMPARE(unknown.size(), 1);
        QCOMPARE(unknown.first().at(0).toString(), taskId);
        QCOMPARE(unknown.first().at(2).toJsonObject(), safeReference);
        QCOMPARE(unknown.first().at(3).toString(), QStringLiteral("NOT_SUBMITTED"));
        QCOMPARE(actions.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 5000);
        const QString staleResult = toolResultForMessages(requests.last().at(1).toJsonArray(),
                                                        QStringLiteral("stale-source-proposal"));
        QVERIFY(staleResult.contains(QStringLiteral("STALE_PREVIEW")));
        runner.receiveTurn(requests.last().at(0).toString(),
                           providerTurn(QStringLiteral("The changed source requires a new preview.")));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(session->getModel(modelId).getProblemSetup().getModalSetup().getNIterations(), 12u);
        QVERIFY(!intentRevision.isEmpty());

        RAgentConversationStore reopened;
        QVERIFY2(reopened.configure(host->projectWorkspace(), projectId, &error), qPrintable(error));
        const QJsonObject transcript = reopened.exportConversation(conversationId, &error);
        QVERIFY2(!transcript.isEmpty(), qPrintable(error));
        const QJsonArray messages = transcript.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), RAgentConversationStore::MaxMessagesPerConversation);
        const QJsonObject pendingFact = messages.at(510).toObject();
        const QJsonObject rejectedFact = messages.at(511).toObject();
        QCOMPARE(pendingFact.value(QStringLiteral("facts")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending"));
        QCOMPARE(rejectedFact.value(QStringLiteral("facts")).toObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("rejected"));
        QCOMPARE(rejectedFact.value(QStringLiteral("taskId")).toString(), taskId);
        QCOMPARE(rejectedFact.value(QStringLiteral("facts")).toObject()
                     .value(QStringLiteral("idempotencyKey")).toString(),
                 safeReference.value(QStringLiteral("idempotencyKey")).toString());
        const QJsonObject resolvedBranch = reopened.branch(conversationId,
            rejectedFact.value(QStringLiteral("messageId")).toString(),
            QStringLiteral("Resolved receipt branch"), &error);
        QVERIFY2(!resolvedBranch.isEmpty(), qPrintable(error));

        // Direct stale execution also produces a durable host rejection.
        const QString documentId = initialDocument.value(QStringLiteral("documentId")).toString();
        const QJsonObject directProposal = modalProposal(session, modelId, 29);
        const QJsonObject previewResponse = commandPreview(session, documentId,
            QStringLiteral("analysis.modal.configure"), directProposal.value(QStringLiteral("arguments")).toObject());
        QCOMPARE(previewResponse.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        const QString previewId = previewResponse.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString();
        QVERIFY(!previewId.isEmpty());
        QVERIFY(host->approvePreview(previewId));
        session->getModel(modelId).setDescription(
            QStringLiteral("Source changed after direct host approval."));
        session->setModelChanged(modelId);
        const QString idempotencyKey = newId();
        const QJsonObject execution = host->handle(hostRequest(QStringLiteral("command.execute"),
            {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey}}));
        QCOMPARE(execution.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(execution.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_REVISION"));
        QCOMPARE(session->getModel(modelId).getProblemSetup().getModalSetup().getNIterations(), 12u);
        const QJsonObject receipt = host->handle(hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("idempotencyKey"), idempotencyKey}}));
        QCOMPARE(receipt.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject durable = receipt.value(QStringLiteral("result")).toObject();
        QCOMPARE(durable.value(QStringLiteral("phase")).toString(), QStringLiteral("observed"));
        const QJsonObject observation = durable.value(QStringLiteral("observation")).toObject();
        QCOMPARE(observation.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        QCOMPARE(observation.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_REVISION"));
    }

    void unavailableLedgerNeverClassifiesUnknownExecutionAsAbsent()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        auto *host = session->getModelCommandHost();
        const QJsonObject query = hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("idempotencyKey"), newId()}});
        QCOMPARE(host->handleReadOnly(query).value(QStringLiteral("code")).toString(),
                 QStringLiteral("OPERATION_NOT_FOUND"));
        QLockFile lock(QDir(host->projectWorkspace()).filePath(QStringLiteral(".project.lock")));
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock(0));
        QCOMPARE(host->handleReadOnly(query).value(QStringLiteral("code")).toString(),
                 QStringLiteral("PERSISTENCE_ERROR"));
        lock.unlock();
        QVERIFY(QDir().mkpath(QDir(host->projectWorkspace()).filePath(QStringLiteral("operations"))));
        QFile ledger(QDir(host->projectWorkspace()).filePath(QStringLiteral("operations/ledger.json")));
        QVERIFY(ledger.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(ledger.write("invalid-test-ledger"), qint64(19));
        ledger.close();
        QCOMPARE(host->handleReadOnly(query).value(QStringLiteral("code")).toString(),
                 QStringLiteral("PERSISTENCE_ERROR"));
        QVERIFY(ledger.remove());
        QCOMPARE(host->handleReadOnly(query).value(QStringLiteral("code")).toString(),
                 QStringLiteral("OPERATION_NOT_FOUND"));
    }

    void twoWidgetsRebaseStaleAppendAndKeepPrimaryAuthority()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = application->getSession();
        uint modelId = 0;
        QString error;
        QVERIFY2(openProjectWithModal(session, workspace.path(), &modelId, &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        QVERIFY(host);

        RAgentConversationStore store;
        QString projectId;
        QVERIFY2(configureStore(&store, host, &projectId, &error), qPrintable(error));
        const QJsonObject created = store.create(QStringLiteral("Shared widget transcript"), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        const QString conversationId = created.value(QStringLiteral("conversationId")).toString();

        WorkflowProcess workflow;
        const QJsonObject documentProposal = modalProposal(session, modelId, 25);
        AgentWidget primary(host, &workflow, nullptr, nullptr,
                            AgentTaskRunner::AccessPolicy::ReadWrite, false);
        AgentWidget readOnly(host, &workflow, nullptr, nullptr,
                             AgentTaskRunner::AccessPolicy::ReadOnly, false);
        QVERIFY2(primary.loadConversation(conversationId, &error), qPrintable(error));
        QVERIFY2(readOnly.loadConversation(conversationId, &error), qPrintable(error));
        QCOMPARE(primary.conversationId(), conversationId);
        QCOMPARE(readOnly.conversationId(), conversationId);

        // Keep widget persistence and authority connections; inject provider turns.
        QVERIFY(QObject::disconnect(primary.taskRunner(),
            QMetaMethod::fromSignal(&AgentTaskRunner::turnRequested), &primary, QMetaMethod()));
        QVERIFY(QObject::disconnect(readOnly.taskRunner(),
            QMetaMethod::fromSignal(&AgentTaskRunner::turnRequested), &readOnly, QMetaMethod()));
        QSignalSpy primaryTurns(primary.taskRunner(), &AgentTaskRunner::turnRequested);
        QSignalSpy readOnlyTurns(readOnly.taskRunner(), &AgentTaskRunner::turnRequested);
        QSignalSpy primaryProposals(primary.taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy primaryActions(primary.taskRunner(), &AgentTaskRunner::actionCompleted);
        QSignalSpy readOnlyProposals(readOnly.taskRunner(), &AgentTaskRunner::proposalReady);
        QSignalSpy readOnlyActions(readOnly.taskRunner(), &AgentTaskRunner::actionCompleted);
        primary.taskRunner()->startTask(QStringLiteral("Primary reviewed write."));
        QCOMPARE(primaryTurns.size(), 1);
        primary.taskRunner()->receiveTurn(primaryTurns.first().at(0).toString(), providerTurn(QString(),
            QJsonArray{providerCall(QStringLiteral("primary-modal-proposal"),
                QStringLiteral("document_propose"), documentProposal)}));
        QCOMPARE(primaryProposals.size(), 1);
        const QString pendingProposalId = primaryProposals.first().at(1).toString();
        QVERIFY(!pendingProposalId.isEmpty());
        const QJsonObject beforeReadOnly = host->query(modelId);
        QCOMPARE(session->getModel(modelId).getProblemSetup().getModalSetup().getNIterations(), 12u);

        // The read-only widget still holds revision 1 after the primary append.
        const QJsonObject afterPrimaryUser = store.page(conversationId, 0, 10, &error);
        QCOMPARE(afterPrimaryUser.value(QStringLiteral("messageCount")).toInt(), 1);
        const QString primaryMessageId = afterPrimaryUser.value(QStringLiteral("messages")).toArray()
            .first().toObject().value(QStringLiteral("messageId")).toString();
        readOnly.taskRunner()->startTask(QStringLiteral("Concurrent independent read."));
        const QJsonObject afterBothUsers = store.page(conversationId, 0, 10, &error);
        QVERIFY2(!afterBothUsers.isEmpty(), qPrintable(error));
        QCOMPARE(afterBothUsers.value(QStringLiteral("messageCount")).toInt(), 2);
        QCOMPARE(afterBothUsers.value(QStringLiteral("revision")).toString(), QStringLiteral("3"));
        QCOMPARE(readOnlyTurns.size(), 1);
        QHash<QString, QString> userMessageIds;
        for (const QJsonValue &value : afterBothUsers.value(QStringLiteral("messages")).toArray())
        {
            const QJsonObject message = value.toObject();
            if (message.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("user"))) continue;
            const QString text = message.value(QStringLiteral("text")).toString();
            QVERIFY(!userMessageIds.contains(text));
            userMessageIds.insert(text, message.value(QStringLiteral("messageId")).toString());
        }
        QCOMPARE(userMessageIds.size(), 2);
        QCOMPARE(userMessageIds.value(QStringLiteral("Primary reviewed write.")), primaryMessageId);
        QVERIFY(!userMessageIds.value(QStringLiteral("Concurrent independent read.")).isEmpty());

        readOnly.taskRunner()->receiveTurn(readOnlyTurns.first().at(0).toString(), providerTurn(QString(),
            QJsonArray{providerCall(QStringLiteral("read-only-write-attempt"),
                QStringLiteral("document_propose"), documentProposal)}));
        QTRY_COMPARE_WITH_TIMEOUT(readOnlyTurns.size(), 2, 3000);
        QVERIFY(toolResultForMessages(readOnlyTurns.last().at(1).toJsonArray(),
            QStringLiteral("read-only-write-attempt")).contains(QStringLiteral("READ_ONLY_TOOL_DENIED")));
        readOnly.taskRunner()->cancel();
        QVERIFY(!readOnly.taskRunner()->isActive());
        QCOMPARE(readOnlyProposals.size(), 0);
        QCOMPARE(readOnlyActions.size(), 0);
        QCOMPARE(host->query(modelId), beforeReadOnly);
        QCOMPARE(primary.taskRunner()->pendingProposal().value(QStringLiteral("previewId")),
                 primaryProposals.first().at(2).toJsonObject().value(QStringLiteral("previewId")));

        primary.taskRunner()->authorizePending(pendingProposalId, true);
        QCOMPARE(primaryActions.size(), 1);
        QCOMPARE(primaryActions.first().at(0).toString(), QStringLiteral("document"));
        QCOMPARE(session->getModel(modelId).getProblemSetup().getModalSetup().getNIterations(), 25u);
        primary.taskRunner()->cancel();

        const QJsonObject finalTranscript = store.exportConversation(conversationId, &error);
        QVERIFY2(!finalTranscript.isEmpty(), qPrintable(error));
        QSet<QString> finalUserMessageIds;
        int finalUserCount = 0;
        for (const QJsonValue &value : finalTranscript.value(QStringLiteral("messages")).toArray())
        {
            const QJsonObject message = value.toObject();
            if (message.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("user"))) continue;
            ++finalUserCount;
            finalUserMessageIds.insert(message.value(QStringLiteral("messageId")).toString());
        }
        QCOMPARE(finalUserCount, 2);
        const QSet<QString> expectedMessageIds{primaryMessageId,
            userMessageIds.value(QStringLiteral("Concurrent independent read."))};
        QCOMPARE(finalUserMessageIds, expectedMessageIds);
    }

    void workflowFailureReceiptReconciliationClearsOnlyMatchingCapacity()
    {
        // Protocol-only lost response fixture; no solver or numerical claim.
        QTemporaryDir runtime;
        QVERIFY(runtime.isValid());
        QString python;
        QString error;
        QVERIFY2(createWorkflowProtocolFixture(runtime.path(), &python, &error), qPrintable(error));
        WorkflowProcess::Configuration configuration =
            WorkflowProcess::defaultConfiguration(QDir(runtime.path()).filePath(QStringLiteral("workspace")));
        configuration.python = python;
        configuration.packageDirectory = QDir(runtime.path()).filePath(QStringLiteral("fixture-package"));
        configuration.solver = QDir(runtime.path()).filePath(QStringLiteral("fixture-solver.exe"));
        configuration.native = QDir(runtime.path()).filePath(QStringLiteral("fixture-native.exe"));
        configuration.examples = QDir(runtime.path()).filePath(QStringLiteral("examples"));
        configuration.workspace = QDir(runtime.path()).filePath(QStringLiteral("workspace"));
        configuration.nativeResults.clear();
        configuration.requestTimeoutMs = 5000;

        WorkflowProcess workflow;
        workflow.start(configuration);
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy turns(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy unknown(&runner, &AgentTaskRunner::executionObservationUnknown);
        QSignalSpy observed(&runner, &AgentTaskRunner::executionObserved);
        QSignalSpy replies(&workflow, &WorkflowProcess::replyReceived);
        QSignalSpy failures(&workflow, &WorkflowProcess::requestFailed);
        connect(&runner, &AgentTaskRunner::executionObservationUnknown, &runner,
            [&runner](const QString &, const QString &, const QJsonObject &, const QString &) {
                // Cancel at requestFailed; retain the safe lookup reference.
                runner.cancel();
            });

        const auto executeFailureCount = [&] {
            int count = 0;
            for (const QList<QVariant> &arguments : failures)
                if (arguments.at(1).toString() == QStringLiteral("command.execute")) ++count;
            return count;
        };
        const auto hostApprovalReplyCount = [&] {
            int count = 0;
            for (const QList<QVariant> &arguments : replies)
                if (arguments.at(1).toString() == QStringLiteral("host.approve")) ++count;
            return count;
        };
        struct UnknownReference
        {
            QString taskId;
            QJsonObject reference;
        };
        QList<UnknownReference> references;

        const auto runAmbiguousTask = [&](int index, int expectedCount) -> bool {
            if (runner.isActive()) return false;
            const int turnBase = turns.size();
            const int proposalBase = proposals.size();
            const int unknownBase = unknown.size();
            const QString callId = QStringLiteral("workflow-execute-%1").arg(index);
            runner.startTask(QStringLiteral("Reconcile workflow outcome %1.").arg(index));
            if (!waitUntil([&] { return turns.size() == turnBase + 1; }, 5000)) return false;
            const QJsonObject arguments{{QStringLiteral("command"), QStringLiteral("workflow.configure")},
                {QStringLiteral("arguments"), QJsonObject{
                    {QStringLiteral("dt"), 0.001}, {QStringLiteral("steps"), 5},
                    {QStringLiteral("targetDamping"), 0.05}}}};
            runner.receiveTurn(turns.at(turnBase).at(0).toString(), providerTurn(QString(),
                QJsonArray{providerCall(callId, QStringLiteral("workflow_propose"), arguments)}));
            if (!waitUntil([&] { return proposals.size() == proposalBase + 1; }, 5000)) return false;
            runner.authorizePending(proposals.last().at(1).toString(), true);
            if (!waitUntil([&] { return unknown.size() == unknownBase + 1; }, 5000)) return false;
            if (!waitUntil([&] { return !runner.isActive(); }, 2000)) return false;
            const QList<QVariant> observed = unknown.last();
            const QString taskId = observed.at(0).toString();
            const QJsonObject reference = observed.at(2).toJsonObject();
            if (observed.at(1).toString() != QStringLiteral("workflow")
                || observed.at(3).toString() != QStringLiteral("RESPONSE_LOST_AFTER_ACCEPTANCE")
                || reference.value(QStringLiteral("command")).toString() != QStringLiteral("workflow.configure")
                || reference.value(QStringLiteral("requestSha256")).toString().size() != 64
                || reference.value(QStringLiteral("workflowProjectId")).toString()
                    != QStringLiteral("00000000-0000-4000-8000-000000000001")
                || runner.pendingExecutionObservationCount() != expectedCount)
                return false;
            references.append({taskId, reference});
            return true;
        };

        const auto acceptedReceiptFor = [&](const UnknownReference &reference,
                                            QJsonObject *receipt) -> bool {
            const QString requestId = workflow.request(QStringLiteral("execution.query"),
                {{QStringLiteral("requestSha256"),
                  reference.reference.value(QStringLiteral("requestSha256"))}});
            if (requestId.isEmpty()) return false;
            return waitForWorkflowReply(&workflow, requestId, QStringLiteral("execution.query"), receipt);
        };

        for (int index = 0; index < 64; ++index)
            QVERIFY2(runAmbiguousTask(index, index + 1),
                     qPrintable(QStringLiteral("ambiguous command.execute failed at index %1").arg(index)));
        QCOMPARE(runner.pendingExecutionObservationCount(), 64);
        QCOMPARE(executeFailureCount(), 64);

        QJsonObject firstReceipt;
        QVERIFY(acceptedReceiptFor(references.first(), &firstReceipt));
        QVERIFY(firstReceipt.value(QStringLiteral("accepted")).toBool());
        QCOMPARE(firstReceipt.value(QStringLiteral("requestSha256")).toString(),
                 references.first().reference.value(QStringLiteral("requestSha256")).toString());
        QCOMPARE(firstReceipt.value(QStringLiteral("projectId")).toString(),
                 references.first().reference.value(QStringLiteral("workflowProjectId")).toString());

        // Wrong task/reference pairs cannot release capacity.
        runner.resolveExecutionObservation(references.first().taskId, references.at(1).reference);
        runner.resolveExecutionObservation(references.at(1).taskId, references.first().reference);
        QCOMPARE(runner.pendingExecutionObservationCount(), 64);
        const int blockedTurnBase = turns.size();
        const int blockedProposalBase = proposals.size();
        const int blockedUnknownBase = unknown.size();
        const QString blockedCallId = QStringLiteral("workflow-capacity-block");
        runner.startTask(QStringLiteral("Check that unresolved outcomes remain bounded."));
        QVERIFY(waitUntil([&] { return turns.size() == blockedTurnBase + 1; }, 5000));
        const QJsonObject blockedArguments{{QStringLiteral("command"), QStringLiteral("workflow.configure")},
            {QStringLiteral("arguments"), QJsonObject{
                {QStringLiteral("dt"), 0.001}, {QStringLiteral("steps"), 5},
                {QStringLiteral("targetDamping"), 0.05}}}};
        runner.receiveTurn(turns.at(blockedTurnBase).at(0).toString(), providerTurn(QString(),
            QJsonArray{providerCall(blockedCallId, QStringLiteral("workflow_propose"), blockedArguments)}));
        QVERIFY(waitUntil([&] { return proposals.size() == blockedProposalBase + 1; }, 5000));
        runner.authorizePending(proposals.last().at(1).toString(), true);
        QVERIFY(waitUntil([&] { return turns.size() == blockedTurnBase + 2; }, 5000));
        const QString blockedResult = toolResultForMessages(
            turns.last().at(1).toJsonArray(), blockedCallId);
        QVERIFY(blockedResult.contains(QStringLiteral("OBSERVATION_UNAVAILABLE")));
        QCOMPARE(unknown.size(), blockedUnknownBase);
        QCOMPARE(runner.pendingExecutionObservationCount(), 64);
        QCOMPARE(executeFailureCount(), 64);
        runner.cancel();

        const int failuresBeforeResolve = executeFailureCount();
        const int approvalsBeforeResolve = hostApprovalReplyCount();
        runner.resolveExecutionObservation(references.first().taskId, references.first().reference);
        QCOMPARE(runner.pendingExecutionObservationCount(), 63);
        QCOMPARE(executeFailureCount(), failuresBeforeResolve);
        QCOMPARE(hostApprovalReplyCount(), approvalsBeforeResolve);

        // Keep reconciling accepted receipts after crossing the 64-entry cap.
        for (int index = 64; index < 67; ++index)
        {
            QVERIFY2(runAmbiguousTask(index, 64),
                     qPrintable(QStringLiteral("reconciled command.execute failed at index %1").arg(index)));
            const UnknownReference reference = references.last();
            QJsonObject receipt;
            QVERIFY(acceptedReceiptFor(reference, &receipt));
            QVERIFY(receipt.value(QStringLiteral("accepted")).toBool());
            QCOMPARE(receipt.value(QStringLiteral("requestSha256")).toString(),
                     reference.reference.value(QStringLiteral("requestSha256")).toString());
            const int failuresAtReceipt = executeFailureCount();
            const int approvalsAtReceipt = hostApprovalReplyCount();
            runner.resolveExecutionObservation(reference.taskId, reference.reference);
            QCOMPARE(runner.pendingExecutionObservationCount(), 63);
            QCOMPARE(executeFailureCount(), failuresAtReceipt);
            QCOMPARE(hostApprovalReplyCount(), approvalsAtReceipt);
        }
        QCOMPARE(unknown.size(), 67);
        QCOMPARE(observed.size(), 0);
        QCOMPARE(executeFailureCount(), 67);
        QCOMPARE(references.size(), 67);
        QCOMPARE(runner.pendingExecutionObservationCount(), 63);

        // Delivery of a successful reply is not proof that the recorder's
        // owner durably saved the outcome. Keep its slot until explicit
        // matched reconciliation, including when that owner cannot append.
        runner.setExecutionRecorder([](const QString &, const QString &, const QJsonObject &) { return true; });
        const int confirmedTurnBase = turns.size();
        const int confirmedProposalBase = proposals.size();
        runner.startTask(QStringLiteral("Keep a confirmed but unpersisted outcome reserved."));
        QVERIFY(waitUntil([&] { return turns.size() == confirmedTurnBase + 1; }, 5000));
        runner.receiveTurn(turns.last().at(0).toString(), providerTurn(QString(), QJsonArray{
            providerCall(QStringLiteral("confirmed-receipt"), QStringLiteral("workflow_propose"),
                         blockedArguments)}));
        QVERIFY(waitUntil([&] { return proposals.size() == confirmedProposalBase + 1; }, 5000));
        runner.authorizePending(proposals.last().at(1).toString(), true);
        QVERIFY(waitUntil([&] { return observed.size() == 1; }, 5000));
        QCOMPARE(runner.pendingExecutionObservationCount(), 64);
        const auto confirmedObservation = observed.last();
        const QString confirmedTask = confirmedObservation.at(0).toString();
        const QJsonObject confirmedReference = confirmedObservation.at(2).toJsonObject();
        runner.resolveExecutionObservation(references.first().taskId, confirmedReference);
        QCOMPARE(runner.pendingExecutionObservationCount(), 64);
        runner.resolveExecutionObservation(confirmedTask, confirmedReference);
        QCOMPARE(runner.pendingExecutionObservationCount(), 63);
        runner.cancel();

        workflow.stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isStopped(), 5000);
    }
};

int runAgentObservationBoundaryTests(int argc, char **argv)
{
    TestAgentObservationBoundaries test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_observation_boundaries.moc"
