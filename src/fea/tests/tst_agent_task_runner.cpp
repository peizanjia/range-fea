#include <QtTest>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <limits>

#include <rai_agent.h>
#include <rml_model.h>
#include <rml_time_solver.h>

#include "agent_task_runner.h"
#include "application.h"
#include "model_command_host.h"
#include "session.h"
#include "model_io.h"
#include "solver_manager.h"

namespace
{
Model modalFixture()
{
    Model model;
    model.setName(QStringLiteral("Agent task runner fixture"));
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

Model stressFixture()
{
    Model model = modalFixture();
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    return model;
}

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {})
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), operation},
            {QStringLiteral("parameters"), parameters}};
}

QJsonObject providerCall(const QString &id, const QString &name, const QJsonObject &arguments)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name},
                {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}};
}

QByteArray providerBody(const QString &text, const QJsonArray &calls = {})
{
    QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                        {QStringLiteral("content"), text}};
    if (!calls.isEmpty()) message.insert(QStringLiteral("tool_calls"), calls);
    QJsonObject choice{{QStringLiteral("message"), message},
                       {QStringLiteral("finish_reason"), calls.isEmpty() ? QStringLiteral("stop") : QStringLiteral("tool_calls")}};
    const QJsonObject response{{QStringLiteral("choices"), QJsonArray{choice}},
        {QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 11},
            {QStringLiteral("completion_tokens"), 4}, {QStringLiteral("total_tokens"), 15}}}};
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QJsonObject parseProviderBody(const QByteArray &body)
{
    QJsonObject turn;
    QString error;
    if (!RAgent::parseToolResponse(body, &turn, &error))
        qFatal("Test provider fixture was rejected by RAgent::parseToolResponse: %s", qPrintable(error));
    return turn;
}

QJsonObject documentContext(ModelCommandHost *host)
{
    return host->handle(hostRequest(QStringLiteral("context.query")))
        .value(QStringLiteral("result")).toObject();
}

QJsonObject modalProposalArguments()
{
    return {{QStringLiteral("method"), QStringLiteral("multiple")},
            {QStringLiteral("iterations"), 31}, {QStringLiteral("modes"), 2},
            {QStringLiteral("tolerance"), 1e-6}, {QStringLiteral("unit"), QStringLiteral("1")}};
}

QJsonObject structuralDynamicsArguments()
{
    return {{QStringLiteral("enabled"), true}, {QStringLiteral("method"), QStringLiteral("newmark")},
        {QStringLiteral("timeStep"), 0.01}, {QStringLiteral("unitSI"), true},
        {QStringLiteral("steps"), 10}, {QStringLiteral("outputFrequency"), 1},
        {QStringLiteral("rayleighAlpha"), 0.0}, {QStringLiteral("rayleighBeta"), 0.0},
        {QStringLiteral("rhoInfinity"), 1.0}, {QStringLiteral("newmarkGamma"), 0.5},
        {QStringLiteral("newmarkBeta"), 0.25}, {QStringLiteral("modalDampingRatios"), QJsonArray{}}};
}

QJsonObject documentProposal(const QString &documentId, quint64 revision)
{
    return {{QStringLiteral("documentId"), documentId},
            {QStringLiteral("documentRevision"), double(revision)},
            {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
            {QStringLiteral("arguments"), modalProposalArguments()}};
}

QJsonObject extensionManifest(const QString &extensionId)
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
    const QJsonObject table{{QStringLiteral("columns"), columns}};
    const QJsonObject gui{{QStringLiteral("form"), form}, {QStringLiteral("table"), table}};
    const QJsonArray dependencies{};
    return {{QStringLiteral("manifestVersion"), 1}, {QStringLiteral("extensionId"), extensionId},
        {QStringLiteral("version"), QStringLiteral("0.1.0")}, {QStringLiteral("sdkVersion"), QStringLiteral("range.extension/v1")},
        {QStringLiteral("kind"), QStringLiteral("numeric-series-statistics")},
        {QStringLiteral("entryPoint"), QStringLiteral("extension.py:process")},
        {QStringLiteral("dependencies"), dependencies},
        {QStringLiteral("inputSchema"), QStringLiteral("range.extension/v1/numeric-series")},
        {QStringLiteral("outputSchema"), QStringLiteral("range.extension/v1/statistics")},
        {QStringLiteral("gui"), gui}};
}

QJsonObject extensionStageArguments(const QString &extensionId)
{
    const QJsonObject input{{QStringLiteral("times"), QJsonArray{0.0, 0.5, 1.0}},
        {QStringLiteral("values"), QJsonArray{3.0, 3.0, 3.0}},
        {QStringLiteral("timeUnit"), QStringLiteral("s")}, {QStringLiteral("valueUnit"), QStringLiteral("m")}};
    const QJsonObject expected{{QStringLiteral("rms"), 3.0}, {QStringLiteral("peakAbs"), 3.0},
        {QStringLiteral("peakTime"), 0.0}, {QStringLiteral("timeUnit"), QStringLiteral("s")},
        {QStringLiteral("valueUnit"), QStringLiteral("m")}};
    const QString source = QStringLiteral(
        "import math\n"
        "def process(series):\n"
        "    values = series['values']\n"
        "    peak = max(abs(value) for value in values)\n"
        "    peak_index = next(index for index, value in enumerate(values) if abs(value) == peak)\n"
        "    rms = math.sqrt(sum(value * value for value in values) / len(values))\n"
        "    return {'rms': rms, 'peakAbs': peak, 'peakTime': series['times'][peak_index],\n"
        "            'timeUnit': series['timeUnit'], 'valueUnit': series['valueUnit']}\n");
    const QJsonObject testCase{{QStringLiteral("id"), QStringLiteral("constant-series")},
        {QStringLiteral("input"), input}, {QStringLiteral("expected"), expected}};
    const QJsonObject tests{{QStringLiteral("cases"), QJsonArray{testCase}}};
    return {{QStringLiteral("manifest"), extensionManifest(extensionId)},
        {QStringLiteral("source"), source}, {QStringLiteral("tests"), tests}};
}

QJsonObject workflowRequest(WorkflowProcess &workflow, const QString &operation,
                            const QJsonObject &parameters = {}, QString *error = nullptr)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QJsonObject response;
    QString transportError;
    QString requestId;
    bool complete = false;
    const auto replyConnection = QObject::connect(&workflow, &WorkflowProcess::replyReceived, &loop,
        [&](const QString &id, const QString &replyOperation, const QJsonObject &result) {
            if (id != requestId || replyOperation != operation) return;
            response = result;
            complete = true;
            loop.quit();
        });
    const auto failureConnection = QObject::connect(&workflow, &WorkflowProcess::requestFailed, &loop,
        [&](const QString &id, const QString &failedOperation, const QString &code, const QString &message) {
            if (id != requestId || failedOperation != operation) return;
            transportError = code + QStringLiteral(": ") + message;
            complete = true;
            loop.quit();
        });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    requestId = workflow.request(operation, parameters);
    timer.start(15000);
    if (!requestId.isEmpty()) loop.exec();
    QObject::disconnect(replyConnection);
    QObject::disconnect(failureConnection);
    if (error)
        *error = complete ? transportError : QStringLiteral("Timed out waiting for workflow operation %1").arg(operation);
    return response;
}

QString toolResultContent(const QJsonArray &messages, const QString &callId)
{
    for (const auto &messageValue : messages)
    {
        const auto message = messageValue.toObject();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("tool")
            && message.value(QStringLiteral("tool_call_id")).toString() == callId)
            return message.value(QStringLiteral("content")).toString();
    }
    return {};
}

bool registerCurrentAgentSource(WorkflowProcess &workflow, uint &modelID, QJsonObject &origin, QString *error)
{
    static QTemporaryDir projectRoot;
    if (!projectRoot.isValid()) { *error = QStringLiteral("Invalid source project root"); return false; }
    const QString project = projectRoot.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(project)) { *error = QStringLiteral("Cannot create source project"); return false; }
    auto *session = Application::instance()->getSession();
    if (!session->openProject(project, error)) return false;
#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_WORKFLOW_SOURCE_ROOT "."
#endif
    const QString source = QDir(project).filePath(QStringLiteral("agent-source.ram"));
    if (!QFile::copy(QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT)).filePath(
            QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram")), source))
    { *error = QStringLiteral("Cannot copy source fixture"); return false; }
    Model model;
    try { ModelIO::open(model, source); }
    catch (const RError &failure) { *error = failure.getMessage(); return false; }
    model.getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    model.getProblemSetup().setRestart(false);
    session->addModel(model);
    modelID = session->getNModels() - 1;
    const QJsonObject snapshot = session->createDocumentSnapshot(modelID, error);
    if (snapshot.isEmpty()) return false;
    for (const QString &key : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
         QStringLiteral("sourceRevision"), QStringLiteral("modelSha256"), QStringLiteral("instanceRef")})
        origin.insert(key, snapshot.value(key));
    const QJsonObject registered = workflowRequest(workflow, QStringLiteral("model.source.register"),
        {{QStringLiteral("catalogRoot"), project}, {QStringLiteral("snapshotRef"), origin}}, error);
    if (!error->isEmpty()) return false;
    const QString sourceId = registered.value(QStringLiteral("sourceId")).toString();
    if (sourceId.isEmpty()) { *error = QStringLiteral("Source registration returned no sourceId"); return false; }
    const QJsonObject projectState = workflowRequest(workflow, QStringLiteral("project.query"), {}, error);
    if (!error->isEmpty()) return false;
    const QJsonObject preview = workflowRequest(workflow, QStringLiteral("command.preview"),
        {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
         {QStringLiteral("expectedRevision"), projectState.value(QStringLiteral("revision"))},
         {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("sourceId"), sourceId}, {QStringLiteral("dt"), 0.001},
            {QStringLiteral("steps"), 4}, {QStringLiteral("targetDamping"), 0.05}}}}, error);
    if (!error->isEmpty()) return false;
    const QJsonObject approval = workflowRequest(workflow, QStringLiteral("host.approve"),
        {{QStringLiteral("previewId"), preview.value(QStringLiteral("previewId"))},
         {QStringLiteral("previewHash"), preview.value(QStringLiteral("previewHash"))}}, error);
    if (!error->isEmpty()) return false;
    workflowRequest(workflow, QStringLiteral("command.execute"),
        {{QStringLiteral("previewId"), preview.value(QStringLiteral("previewId"))},
         {QStringLiteral("approvalToken"), approval.value(QStringLiteral("approvalToken"))}}, error);
    return error->isEmpty();
}

QJsonObject sourceRunCall(const QString &callId)
{
    return providerCall(callId, QStringLiteral("workflow_propose"),
        {{QStringLiteral("command"), QStringLiteral("workflow.run")},
         {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("passive")},
            {QStringLiteral("independentReference"), true}}}});
}

QStringList workflowJobIds(const QJsonObject &response)
{
    QStringList ids;
    for (const QJsonValue &value : response.value(QStringLiteral("jobs")).toArray())
        ids.append(value.toObject().value(QStringLiteral("jobId")).toString());
    ids.sort();
    return ids;
}
}

class TestAgentTaskRunner : public QObject
{
    Q_OBJECT
private slots:
    void changedRegisteredSourceCannotApproveFreshRun_data()
    {
        QTest::addColumn<QString>("change");
        QTest::newRow("draft") << QStringLiteral("draft");
        QTest::newRow("edit") << QStringLiteral("edit");
        QTest::newRow("closed") << QStringLiteral("closed");
    }

    void changedRegisteredSourceCannotApproveFreshRun()
    {
        QFETCH(QString, change);
        QTemporaryDir workspace;
        WorkflowProcess workflow;
        workflow.start(WorkflowProcess::defaultConfiguration(workspace.path()));
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        uint modelID = 0;
        QJsonObject origin;
        QString error;
        QVERIFY2(registerCurrentAgentSource(workflow, modelID, origin, &error), qPrintable(error));
        const QStringList baselineJobIds = workflowJobIds(workflowRequest(workflow, QStringLiteral("jobs.query"), {}, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!baselineJobIds.isEmpty()); // Trusted setup submitted workflow.configure.
        auto *session = Application::instance()->getSession();
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy replies(&workflow, &WorkflowProcess::replyReceived);
        runner.startTask(QStringLiteral("Preview a fresh run of the registered source."));
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({}, {sourceRunCall(QStringLiteral("fresh-run"))})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 15000);
        QCOMPARE(proposals.first().at(2).toJsonObject().value(QStringLiteral("modelBinding")).toObject()
                     .value(QStringLiteral("originRef")).toObject(), origin);
        replies.clear();
        if (change == QStringLiteral("draft"))
            session->setNumericDraft(modelID, QStringLiteral("analysis.modal"), QStringLiteral("iterations"), QStringLiteral("invalid"), QStringLiteral("pending"));
        else if (change == QStringLiteral("edit"))
        {
            session->getModel(modelID).setDescription(QStringLiteral("Edited after source registration"));
            session->setModelChanged(modelID);
        }
        else session->removeModel(modelID);
        runner.authorizePending(proposals.first().at(1).toString(), true);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 5000);
        QVERIFY(toolResultContent(requested.last().at(1).toJsonArray(), QStringLiteral("fresh-run")).contains(QStringLiteral("STALE_SOURCE")));
        QCOMPARE(actions.size(), 0);
        for (const QList<QVariant> &reply : replies)
        {
            QVERIFY(reply.at(1).toString() != QStringLiteral("host.approve"));
            QVERIFY(reply.at(1).toString() != QStringLiteral("command.execute"));
        }
        const QJsonObject jobs = workflowRequest(workflow, QStringLiteral("jobs.query"), {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(workflowJobIds(jobs), baselineJobIds);
        for (const QJsonValue &job : jobs.value(QStringLiteral("jobs")).toArray())
            QVERIFY(job.toObject().value(QStringLiteral("command")) != QJsonValue(QStringLiteral("workflow.run")));
        runner.cancel(); workflow.stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isStopped(), 10000);
    }

    void sourceChangeWhileApprovalIsReturningPreventsExecute()
    {
        QTemporaryDir workspace;
        WorkflowProcess workflow;
        workflow.start(WorkflowProcess::defaultConfiguration(workspace.path()));
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        uint modelID = 0;
        QJsonObject origin;
        QString error;
        QVERIFY2(registerCurrentAgentSource(workflow, modelID, origin, &error), qPrintable(error));
        const QStringList baselineJobIds = workflowJobIds(workflowRequest(workflow, QStringLiteral("jobs.query"), {}, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!baselineJobIds.isEmpty());
        auto *session = Application::instance()->getSession();
        bool sourceChanged = false;
        // Registered before the runner's reply handler: change real Session
        // state at the returning approval boundary, before it can execute.
        connect(&workflow, &WorkflowProcess::replyReceived, &workflow,
                [session, modelID, &sourceChanged](const QString &, const QString &operation, const QJsonObject &)
        {
            if (operation != QStringLiteral("host.approve")) return;
            session->setNumericDraft(modelID, QStringLiteral("analysis.modal"), QStringLiteral("iterations"), QStringLiteral("invalid"), QStringLiteral("pending"));
            sourceChanged = true;
        });
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy replies(&workflow, &WorkflowProcess::replyReceived);
        runner.startTask(QStringLiteral("Approve this current source run."));
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({}, {sourceRunCall(QStringLiteral("approval-race"))})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 15000);
        replies.clear();
        runner.authorizePending(proposals.first().at(1).toString(), true);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 15000);
        QVERIFY(sourceChanged);
        QVERIFY(toolResultContent(requested.last().at(1).toJsonArray(), QStringLiteral("approval-race")).contains(QStringLiteral("STALE_SOURCE")));
        QCOMPARE(actions.size(), 0);
        bool approved = false;
        for (const QList<QVariant> &reply : replies)
        {
            approved |= reply.at(1).toString() == QStringLiteral("host.approve");
            QVERIFY(reply.at(1).toString() != QStringLiteral("command.execute"));
        }
        QVERIFY(approved);
        const QJsonObject jobs = workflowRequest(workflow, QStringLiteral("jobs.query"), {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(workflowJobIds(jobs), baselineJobIds);
        for (const QJsonValue &job : jobs.value(QStringLiteral("jobs")).toArray())
            QVERIFY(job.toObject().value(QStringLiteral("command")) != QJsonValue(QStringLiteral("workflow.run")));
        runner.cancel(); workflow.stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isStopped(), 10000);
    }

    void sourceOutsideInitialSelectionRequiresExplicitAndCurrentApproval()
    {
        QTemporaryDir workspace;
        WorkflowProcess workflow;
        workflow.start(WorkflowProcess::defaultConfiguration(workspace.path()));
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        uint modelID = 0;
        QJsonObject origin;
        QString error;
        QVERIFY2(registerCurrentAgentSource(workflow, modelID, origin, &error), qPrintable(error));
        const QStringList baselineJobIds = workflowJobIds(workflowRequest(workflow, QStringLiteral("jobs.query"), {}, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!baselineJobIds.isEmpty());
        auto *session = Application::instance()->getSession();
        session->setModelSelected(modelID, false);
        session->addModel(modalFixture());
        session->setModelSelected(session->getNModels() - 1, true);
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy replies(&workflow, &WorkflowProcess::replyReceived);
        runner.startTask(QStringLiteral("Run the workspace source with task authorization."), true);
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({}, {sourceRunCall(QStringLiteral("outside-scope"))})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 15000);
        QTest::qWait(100);
        QCOMPARE(actions.size(), 0);
        QVERIFY(!runner.pendingProposal().isEmpty());
        for (const QList<QVariant> &reply : replies)
            QVERIFY(reply.at(1).toString() != QStringLiteral("host.approve") && reply.at(1).toString() != QStringLiteral("command.execute"));
        // Later selection changes do not extend the task's initial authority.
        session->setModelSelected(modelID, true);
        session->setNumericDraft(modelID, QStringLiteral("analysis.modal"), QStringLiteral("iterations"), QStringLiteral("invalid"), QStringLiteral("pending"));
        runner.authorizePending(proposals.first().at(1).toString(), true);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 5000);
        QVERIFY(toolResultContent(requested.last().at(1).toJsonArray(), QStringLiteral("outside-scope")).contains(QStringLiteral("STALE_SOURCE")));
        QCOMPARE(actions.size(), 0);
        const QJsonObject jobs = workflowRequest(workflow, QStringLiteral("jobs.query"), {}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(workflowJobIds(jobs), baselineJobIds);
        for (const QJsonValue &job : jobs.value(QStringLiteral("jobs")).toArray())
            QVERIFY(job.toObject().value(QStringLiteral("command")) != QJsonValue(QStringLiteral("workflow.run")));
        runner.cancel(); workflow.stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isStopped(), 10000);
    }

    void workflowResultAndSourceReadsUseOnlyReadonlyOperationsAndConfigureAcceptsSourceId()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        WorkflowProcess workflow;
        workflow.start(WorkflowProcess::defaultConfiguration(workspace.path()));
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        QString transportError;
        const QJsonObject before = workflowRequest(workflow, QStringLiteral("project.query"), {}, &transportError);
        QVERIFY2(transportError.isEmpty(), qPrintable(transportError));
        QSignalSpy replies(&workflow, &WorkflowProcess::replyReceived);
        QSignalSpy failures(&workflow, &WorkflowProcess::requestFailed);
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Read actual native result operations and registered sources."));
        QJsonArray reads;
        for (const QString &operation : {QStringLiteral("catalog"), QStringLiteral("channels"), QStringLiteral("series"), QStringLiteral("summary")})
            reads.append(providerCall(QStringLiteral("read-") + operation, QStringLiteral("workflow_native_result"),
                {{QStringLiteral("jobId"), QStringLiteral("job-does-not-exist")}, {QStringLiteral("operation"), operation},
                 {QStringLiteral("mode"), QStringLiteral("passive")}}));
        reads.append(providerCall(QStringLiteral("registered-sources"), QStringLiteral("workflow_sources"), {}));
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({}, reads)));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 15000);
        QStringList observed;
        for (const QList<QVariant> &event : replies) observed.append(event.at(1).toString());
        for (const QList<QVariant> &event : failures) observed.append(event.at(1).toString());
        for (const QString &operation : {QStringLiteral("catalog"), QStringLiteral("channels"), QStringLiteral("series"), QStringLiteral("summary")})
            QVERIFY2(observed.contains(QStringLiteral("result.") + operation + QStringLiteral(".query")), qPrintable(observed.join(QLatin1Char(','))));
        QVERIFY(observed.contains(QStringLiteral("model.sources.query")));
        QVERIFY(!observed.contains(QStringLiteral("model.source.register")));
        const QJsonObject sourceList = QJsonDocument::fromJson(toolResultContent(requested.last().at(1).toJsonArray(), QStringLiteral("registered-sources")).toUtf8()).object();
        QVERIFY(!sourceList.contains(QStringLiteral("code")));
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(finished.size(), 0);

        replies.clear(); failures.clear();
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({}, {
            providerCall(QStringLiteral("configure-known-id-schema"), QStringLiteral("workflow_propose"),
                {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
                 {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("dt"), 0.001}, {QStringLiteral("steps"), 4},
                    {QStringLiteral("targetDamping"), 0.05}, {QStringLiteral("sourceId"), QStringLiteral("source-does-not-exist")}}}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 15000);
        observed.clear();
        for (const QList<QVariant> &event : replies) observed.append(event.at(1).toString());
        for (const QList<QVariant> &event : failures) observed.append(event.at(1).toString());
        // The schema accepts an opaque sourceId and reaches service preview;
        // the service then correctly rejects an unregistered source.
        QVERIFY(observed.contains(QStringLiteral("command.preview")));
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(finished.size(), 0);
        const QJsonObject after = workflowRequest(workflow, QStringLiteral("project.query"), {}, &transportError);
        QVERIFY2(transportError.isEmpty(), qPrintable(transportError));
        QCOMPARE(after.value(QStringLiteral("configuration")).toObject(), before.value(QStringLiteral("configuration")).toObject());
        runner.cancel();
        workflow.stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isStopped(), 10000);
    }

    void nativeResultAndSourceToolsRejectPathsNonfiniteAndRegistration()
    {
        auto *host = Application::instance()->getSession()->getModelCommandHost();
        WorkflowProcess workflow;
        const auto reject = [host, &workflow](const QString &name, const QJsonObject &arguments)
        {
            AgentTaskRunner runner(host, &workflow);
            QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy finished(&runner, &AgentTaskRunner::finished);
            QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
            runner.startTask(QStringLiteral("Validate a bounded read or source selector."));
            runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
                {providerCall(QStringLiteral("invalid-call"), name, arguments)})));
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
            QCOMPARE(proposals.size(), 0);
            QCOMPARE(requested.size(), 1);
        };
        const QJsonObject native{{QStringLiteral("jobId"), QStringLiteral("opaque-job")},
                                 {QStringLiteral("operation"), QStringLiteral("catalog")}};
        for (const QString &name : {QStringLiteral("native_result_query"), QStringLiteral("workflow_native_result")})
        {
            QJsonObject path = native;
            path.insert(QStringLiteral("manifestPath"), QStringLiteral("C:/private/result.manifest.json"));
            reject(name, path);
            QJsonObject nonfinite = native;
            // JSON represents non-finite numeric input as null. Neither that
            // representation nor an in-memory non-finite value is a page size.
            nonfinite.insert(QStringLiteral("limit"), std::numeric_limits<double>::infinity());
            reject(name, nonfinite);
            QJsonObject wrongArtifact = native;
            wrongArtifact.insert(QStringLiteral("artifactId"), 1);
            reject(name, wrongArtifact);
        }
        QJsonObject wrongMode = native;
        wrongMode.insert(QStringLiteral("mode"), QStringLiteral("active"));
        reject(QStringLiteral("native_result_query"), wrongMode);
        reject(QStringLiteral("workflow_sources"), {{QStringLiteral("operation"), QStringLiteral("model.source.register")}});
        reject(QStringLiteral("workflow_sources"), {{QStringLiteral("path"), QStringLiteral("C:/private/source.ram")}});
        // A legal provider function name reaches the agent's tool allowlist.
        // The dotted backend RPC name is rejected earlier by the provider parser.
        reject(QStringLiteral("model_source_register"), {});
        QJsonObject configure{{QStringLiteral("dt"), 0.001}, {QStringLiteral("steps"), 4},
                               {QStringLiteral("targetDamping"), 0.05}, {QStringLiteral("path"), QStringLiteral("C:/private/source.ram")}};
        reject(QStringLiteral("workflow_propose"), {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
                                                   {QStringLiteral("arguments"), configure}});
        configure.remove(QStringLiteral("path"));
        configure.insert(QStringLiteral("sourceId"), QStringLiteral("source-opaque"));
        configure.insert(QStringLiteral("model"), QStringLiteral("four-rod"));
        reject(QStringLiteral("workflow_propose"), {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
                                                   {QStringLiteral("arguments"), configure}});
    }

    void nativeResultToolReadsActualHostBrokerSamplesAndRejectsAnotherArtifact()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        SolverManager &manager = SolverManager::getInstance();
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
#ifndef RANGE_WORKFLOW_SOURCE_ROOT
#define RANGE_WORKFLOW_SOURCE_ROOT "."
#endif
        const QString fixture = QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT)).filePath(QStringLiteral("fea-solver/examples/stage9/stage9_truss4.ram"));
        const QString input = directory.filePath(QStringLiteral("agent-native.ram"));
        QVERIFY(QFile::copy(fixture, input));
        Model model;
        ModelIO::open(model, input);
        model.getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
        QVERIFY(model.getNLines() > 0);
        for (uint i = 0; i < model.getNPoints(); ++i) model.getPoint(i).setMaterial(model.getLine(0).getMaterial());
        auto &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
        dynamics.setEnabled(true); dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
        dynamics.setTimeStep(0.001); dynamics.setNSteps(4); dynamics.setOutputFrequency(2);
        auto &time = model.getTimeSolver();
        time.setEnabled(true); time.setInputStartTime(0.0); time.setInputNTimeSteps(4);
        time.setInputTimeStepSize(0.001); time.setOutputFrequency(2); time.setComputedTime(0.0);
        time.setTimes(RTimeSolver::findTimesVector(4, 0.0, 0.001)); time.setCurrentTimeStep(0);
        session->addModel(model);
        QString error;
        const QString jobId = manager.submitValidated(session->getNModels() - 1, {}, true, &error);
        QVERIFY2(!jobId.isEmpty(), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(manager.jobQuery(jobId).value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"), 120000);
        QTRY_VERIFY_WITH_TIMEOUT(manager.getNRunning() == 0 && manager.getNWaiting() == 0, 15000);
        const QJsonObject catalog = host->handle(hostRequest(QStringLiteral("native.result.catalog.query"), {{QStringLiteral("jobId"), jobId}}));
        QCOMPARE(catalog.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QString artifactId = catalog.value(QStringLiteral("result")).toObject().value(QStringLiteral("artifactId")).toString();
        const QJsonObject channels = host->handle(hostRequest(QStringLiteral("native.result.channels.query"),
            {{QStringLiteral("jobId"), jobId}, {QStringLiteral("artifactId"), artifactId}}));
        QCOMPARE(channels.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonArray available = channels.value(QStringLiteral("result")).toObject().value(QStringLiteral("channels")).toArray();
        QVERIFY(!available.isEmpty());
        const QString channelId = available.first().toObject().value(QStringLiteral("channelId")).toString();
        QJsonObject seriesArguments{{QStringLiteral("jobId"), jobId}, {QStringLiteral("artifactId"), artifactId},
                                    {QStringLiteral("channelId"), channelId}, {QStringLiteral("limit"), 10}};
        const QJsonObject expectedSeries = host->handle(hostRequest(QStringLiteral("native.result.series.query"), seriesArguments));
        QCOMPARE(expectedSeries.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(expectedSeries.value(QStringLiteral("result")).toObject().value(QStringLiteral("samples")).toArray().size(), 2);

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        runner.startTask(QStringLiteral("Read the actual persisted native result samples."));
        seriesArguments.insert(QStringLiteral("operation"), QStringLiteral("series"));
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({}, {
            providerCall(QStringLiteral("actual-series"), QStringLiteral("native_result_query"), seriesArguments),
            providerCall(QStringLiteral("wrong-artifact"), QStringLiteral("native_result_query"),
                {{QStringLiteral("jobId"), jobId}, {QStringLiteral("operation"), QStringLiteral("catalog")},
                 {QStringLiteral("artifactId"), QStringLiteral("artifact-from-another-job")}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 10000);
        const QJsonArray messages = requested.last().at(1).toJsonArray();
        const QJsonObject actual = QJsonDocument::fromJson(toolResultContent(messages, QStringLiteral("actual-series")).toUtf8()).object();
        QCOMPARE(actual.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(actual.value(QStringLiteral("result")).toObject(), expectedSeries.value(QStringLiteral("result")).toObject());
        const QJsonObject wrong = QJsonDocument::fromJson(toolResultContent(messages, QStringLiteral("wrong-artifact")).toUtf8()).object();
        QCOMPARE(wrong.value(QStringLiteral("code")).toString(), QStringLiteral("ARTIFACT_ID_MISMATCH"));
        runner.cancel();
    }

    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        Application::instance()->getSession()->clear();
    }

    void providerParserAndToolDefinitionsRoundTrip()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Summarize the current state."));
        QCOMPARE(requested.size(), 1);

        QByteArray requestBody;
        QString error;
        QVERIFY2(RAgent::buildToolRequest(QStringLiteral("offline-test-model"), 512, 0.0,
            requested.first().at(1).toJsonArray(), requested.first().at(2).toJsonArray(),
            false, &requestBody, &error), qPrintable(error));
        QVERIFY(!requestBody.isEmpty());
        bool hasAskUser = false;
        bool hasTelemetry = false;
        QStringList hostTools;
        QJsonArray documentCommands;
        for (const QJsonValue &toolValue : requested.first().at(2).toJsonArray())
        {
            const QJsonObject function = toolValue.toObject().value(QStringLiteral("function")).toObject();
            const QString toolName = function.value(QStringLiteral("name")).toString();
            if (toolName == QStringLiteral("document_inputs") || toolName == QStringLiteral("document_targets")
                || toolName == QStringLiteral("native_jobs") || toolName == QStringLiteral("native_job")
                || toolName == QStringLiteral("native_result_open") || toolName == QStringLiteral("native_result_query")
                || toolName == QStringLiteral("workflow_native_result") || toolName == QStringLiteral("workflow_sources")) hostTools.append(toolName);
            if (toolName == QStringLiteral("document_propose"))
                documentCommands = function.value(QStringLiteral("parameters")).toObject()
                    .value(QStringLiteral("properties")).toObject().value(QStringLiteral("command")).toObject()
                    .value(QStringLiteral("enum")).toArray();
            if (function.value(QStringLiteral("name")) == QJsonValue(QStringLiteral("ask_user")))
            {
                hasAskUser = true;
                const QJsonObject options = function.value(QStringLiteral("parameters")).toObject()
                    .value(QStringLiteral("properties")).toObject().value(QStringLiteral("options")).toObject();
                QCOMPARE(options.value(QStringLiteral("maxItems")).toInt(), 3);
            }
            if (function.value(QStringLiteral("name")) == QJsonValue(QStringLiteral("workflow_telemetry")))
                hasTelemetry = true;
        }
        QVERIFY(hasAskUser);
        QVERIFY(hasTelemetry);
        for (const QString &toolName : {QStringLiteral("document_inputs"), QStringLiteral("document_targets"),
             QStringLiteral("native_jobs"), QStringLiteral("native_job"), QStringLiteral("native_result_open"),
             QStringLiteral("native_result_query"), QStringLiteral("workflow_native_result"), QStringLiteral("workflow_sources")})
            QVERIFY(hostTools.contains(toolName));
        for (const QString &command : {QStringLiteral("analysis.structural_dynamics.configure"),
             QStringLiteral("entity.material.set"), QStringLiteral("entity.boundary.set"),
             QStringLiteral("analysis.run"), QStringLiteral("analysis.cancel")})
            QVERIFY(documentCommands.contains(QJsonValue(command)));

        const auto turn = parseProviderBody(providerBody(QStringLiteral("No documents are open.")));
        QCOMPARE(turn.value(QStringLiteral("text")).toString(), QStringLiteral("No documents are open."));
        QCOMPARE(turn.value(QStringLiteral("toolCalls")).toArray().size(), 0);
        const QString requestId = requested.first().at(0).toString();
        runner.receiveTurn(requestId, turn);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
    }

    void guiDocumentQueriesAndStructuralDynamicsUseTheVersionedHost()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(stressFixture());
        session->setModelSelected(0, true);
        const QJsonObject document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString documentId = document.value(QStringLiteral("documentId")).toString();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());
        QVERIFY(!documentId.isEmpty());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Inspect this GUI model and configure structural dynamics."));
        const QJsonArray queries{
            providerCall(QStringLiteral("inputs-read"), QStringLiteral("document_inputs"),
                {{QStringLiteral("documentId"), documentId}}),
            providerCall(QStringLiteral("targets-read"), QStringLiteral("document_targets"),
                {{QStringLiteral("documentId"), documentId},
                 {QStringLiteral("page"), QJsonObject{{QStringLiteral("offset"), 0}, {QStringLiteral("limit"), 20}}}})};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(), queries)));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);

        QJsonObject inputsResult, targetsResult;
        for (const QJsonValue &messageValue : requested.last().at(1).toJsonArray())
        {
            const QJsonObject message = messageValue.toObject();
            if (message.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("tool"))) continue;
            const QJsonObject result = QJsonDocument::fromJson(message.value(QStringLiteral("content")).toString().toUtf8()).object();
            if (message.value(QStringLiteral("tool_call_id")) == QJsonValue(QStringLiteral("inputs-read"))) inputsResult = result;
            if (message.value(QStringLiteral("tool_call_id")) == QJsonValue(QStringLiteral("targets-read"))) targetsResult = result;
        }
        QCOMPARE(inputsResult.value(QStringLiteral("apiVersion")).toString(), QStringLiteral("range.host/v1"));
        QCOMPARE(inputsResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(inputsResult.value(QStringLiteral("result")).toObject().contains(QStringLiteral("configuration")));
        QCOMPARE(targetsResult.value(QStringLiteral("apiVersion")).toString(), QStringLiteral("range.host/v1"));
        QCOMPARE(targetsResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(targetsResult.value(QStringLiteral("result")).toObject().value(QStringLiteral("runtimeStamp"))
                     .toObject().value(QStringLiteral("documentId")).toString(), documentId);

        const QJsonObject proposal{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("documentRevision"), double(revision)},
            {QStringLiteral("command"), QStringLiteral("analysis.structural_dynamics.configure")},
            {QStringLiteral("arguments"), structuralDynamicsArguments()}};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall(QStringLiteral("dynamics-preview"), QStringLiteral("document_propose"), proposal)})));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.first().at(2).toJsonObject().value(QStringLiteral("command")).toString(),
                 QStringLiteral("analysis.structural_dynamics.configure"));
        QCOMPARE(ready.first().at(2).toJsonObject().value(QStringLiteral("arguments")).toObject()
                     .value(QStringLiteral("unitSI")).toBool(), true);
        runner.authorizePending(ready.first().at(1).toString(), true);
        QCOMPARE(applied.size(), 1);
        QCOMPARE(applied.first().at(1).toJsonObject().value(QStringLiteral("command")).toString(),
                 QStringLiteral("analysis.structural_dynamics.configure"));
        QCOMPARE(session->getModel(0).getProblemSetup().getStructuralDynamicsSetup().getNSteps(), 10u);

        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 1000);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The host reads and approved structural dynamics command completed."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void newCoreCommandArgumentsRejectExtraFieldsWrongUnitsAndUnstableTargets()
    {
        auto *host = Application::instance()->getSession()->getModelCommandHost();
        WorkflowProcess workflow;
        auto rejected = [&](const QString &callId, const QString &command, const QJsonObject &arguments) {
            AgentTaskRunner runner(host, &workflow);
            QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
            QSignalSpy finished(&runner, &AgentTaskRunner::finished);
            runner.startTask(QStringLiteral("Validate one GUI command."));
            const QJsonObject call{{QStringLiteral("documentId"), QStringLiteral("doc-test")},
                {QStringLiteral("documentRevision"), 0}, {QStringLiteral("command"), command},
                {QStringLiteral("arguments"), arguments}};
            runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
                QJsonArray{providerCall(callId, QStringLiteral("document_propose"), call)})));
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
            QCOMPARE(ready.size(), 0);
        };

        QJsonObject dynamics = structuralDynamicsArguments();
        dynamics.insert(QStringLiteral("timeUnit"), QStringLiteral("ms"));
        rejected(QStringLiteral("extra-field"), QStringLiteral("analysis.structural_dynamics.configure"), dynamics);
        rejected(QStringLiteral("wrong-boundary-unit"), QStringLiteral("entity.boundary.set"),
            {{QStringLiteral("targetId"), QStringLiteral("g:1")},
             {QStringLiteral("kind"), QStringLiteral("displacement")},
             {QStringLiteral("values"), QJsonArray{0.0, QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null)}},
             {QStringLiteral("unit"), QStringLiteral("mm")}});
        rejected(QStringLiteral("unstable-target"), QStringLiteral("entity.material.set"),
            {{QStringLiteral("targetId"), QStringLiteral("label:fixed-end")},
             {QStringLiteral("name"), QStringLiteral("Steel")},
             {QStringLiteral("youngModulusPa"), 2.0e11}, {QStringLiteral("densityKgM3"), 7800.0},
             {QStringLiteral("poissonRatio"), 0.3}, {QStringLiteral("thermalExpansionPerK"), 1.2e-5}});

        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Do not route native jobs through the workflow workspace."));
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall(QStringLiteral("wrong-workflow-route"), QStringLiteral("workflow_propose"),
                {{QStringLiteral("command"), QStringLiteral("analysis.run")},
                 {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("warningsAccepted"), true}}}})})));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(ready.size(), 0);
    }

    void automaticDocumentRevisionAdvancesThroughItsOwnAcceptedCommand()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(stressFixture());
        session->setModelSelected(0, true);
        const QJsonObject document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString documentId = document.value(QStringLiteral("documentId")).toString();
        const quint64 initialRevision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Configure structural dynamics in two accepted steps."), true);
        auto proposal = [&](quint64 revision, QJsonObject arguments, const QString &callId) {
            return providerCall(callId, QStringLiteral("document_propose"),
                {{QStringLiteral("documentId"), documentId}, {QStringLiteral("documentRevision"), double(revision)},
                 {QStringLiteral("command"), QStringLiteral("analysis.structural_dynamics.configure")},
                 {QStringLiteral("arguments"), arguments}});
        };
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{proposal(initialRevision, structuralDynamicsArguments(), QStringLiteral("first-dynamics-edit"))})));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(applied.size(), 1);
        const quint64 acceptedRevision = quint64(applied.first().at(1).toJsonObject()
            .value(QStringLiteral("afterRevision")).toDouble());
        QVERIFY(acceptedRevision > initialRevision);

        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        QJsonObject secondArguments = structuralDynamicsArguments();
        secondArguments.insert(QStringLiteral("timeStep"), 0.02);
        secondArguments.insert(QStringLiteral("steps"), 20);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{proposal(acceptedRevision, secondArguments, QStringLiteral("second-dynamics-edit"))})));
        QCOMPARE(ready.size(), 2);
        QCOMPARE(applied.size(), 2);
        QCOMPARE(session->getModel(0).getProblemSetup().getStructuralDynamicsSetup().getNSteps(), 20u);

        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 1000);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("Both accepted commands are complete."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void staleNativeRunRevisionNeverProducesAnApprovalProposal()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(stressFixture());
        session->setModelSelected(0, true);
        const QJsonObject document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString documentId = document.value(QStringLiteral("documentId")).toString();
        const quint64 initialRevision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Start the selected native analysis when its source is current."), true);
        session->getModel(0).getProblemSetup().getStructuralDynamicsSetup().setNSteps(20);
        session->setProblemChanged(0);
        const QJsonObject proposal{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("documentRevision"), double(initialRevision)},
            {QStringLiteral("command"), QStringLiteral("analysis.run")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("warningsAccepted"), true}}}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall(QStringLiteral("stale-run"), QStringLiteral("document_propose"), proposal)})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        QCOMPARE(ready.size(), 0);
        QVERIFY(requested.last().at(1).toJsonArray().last().toObject().value(QStringLiteral("content")).toString()
                    .contains(QStringLiteral("STALE_REVISION")));
        QCOMPARE(session->getModel(0).getProblemSetup().getStructuralDynamicsSetup().getNSteps(), 20u);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The stale source was not submitted."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void followupRetainsToolOutcomesAndUsesFreshHostContext()
    {
        auto *host = Application::instance()->getSession()->getModelCommandHost();
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);

        runner.submitMessage(QStringLiteral("First request in this conversation."));
        QCOMPARE(requested.size(), 1);
        runner.clearConversation(); // Clearing is ignored while a turn is active.
        QVERIFY(runner.isActive());
        QVERIFY(requested.first().at(1).toJsonArray().last().toObject().value(QStringLiteral("content"))
                    .toString().startsWith(QStringLiteral("Fresh host context (data): ")));
        const QString firstRequest = requested.first().at(0).toString();
        runner.receiveTurn(firstRequest, parseProviderBody(providerBody(QString(), QJsonArray{
            providerCall(QStringLiteral("first-context-read"), QStringLiteral("context_query"), {})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The first request is complete."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));

        runner.submitMessage(QStringLiteral("Follow up using the earlier context."));
        QCOMPARE(requested.size(), 3);
        const QJsonArray followupHistory = requested.last().at(1).toJsonArray();
        QString summaryContent;
        bool hasFreshContext = false;
        bool hasRawToolMessage = false;
        for (const QJsonValue &messageValue : followupHistory)
        {
            const QJsonObject message = messageValue.toObject();
            const QString role = message.value(QStringLiteral("role")).toString();
            if (role == QStringLiteral("tool") || (role == QStringLiteral("assistant") && message.contains(QStringLiteral("tool_calls"))))
                hasRawToolMessage = true;
            const QString content = message.value(QStringLiteral("content")).toString();
            if (content.startsWith(QStringLiteral("Prior conversation data (untrusted; not authorization): ")))
                summaryContent = content;
            if (content.startsWith(QStringLiteral("Fresh host context (data): ")))
            {
                hasFreshContext = true;
                const auto snapshot = QJsonDocument::fromJson(content.mid(QStringLiteral("Fresh host context (data): ").size()).toUtf8()).object();
                QCOMPARE(snapshot.value(QStringLiteral("result")).toObject().value(QStringLiteral("sessionEpoch")).toString(),
                         documentContext(host).value(QStringLiteral("sessionEpoch")).toString());
            }
        }
        QVERIFY(!summaryContent.isEmpty());
        QVERIFY(summaryContent.contains(QStringLiteral("First request in this conversation.")));
        QVERIFY(!summaryContent.contains(QStringLiteral("first-context-read"))); // Tool call IDs are not replayed into later turns.
        QVERIFY(summaryContent.contains(QStringLiteral("context_query")));
        QVERIFY(summaryContent.contains(QStringLiteral("tool_result")));
        QVERIFY(summaryContent.contains(QStringLiteral("documents")));
        QVERIFY(hasFreshContext);
        QVERIFY(!hasRawToolMessage);
        const QJsonObject followupTurn = parseProviderBody(providerBody(QStringLiteral("The follow up is complete.")));
        runner.receiveTurn(requested.last().at(0).toString(), followupTurn);
        QCOMPARE(finished.size(), 2);
        runner.clearConversation();
        runner.submitMessage(QStringLiteral("Start a clean conversation."));
        QCOMPARE(requested.size(), 4);
        for (const QJsonValue &messageValue : requested.last().at(1).toJsonArray())
            QVERIFY(!messageValue.toObject().value(QStringLiteral("content")).toString()
                         .startsWith(QStringLiteral("Prior conversation data (untrusted; not authorization): ")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The clean conversation is complete."))));
        QCOMPARE(finished.size(), 3);
    }

    void steeringInvalidatesPendingProposalWithoutFailureOrCarriedApproval()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString identity = document.value(QStringLiteral("documentId")).toString();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Prepare a modal setting proposal."));
        const QString oldRequestId = requested.last().at(0).toString();
        runner.receiveTurn(oldRequestId, parseProviderBody(providerBody(QString(), QJsonArray{
            providerCall(QStringLiteral("pending-modal-proposal"), QStringLiteral("document_propose"), documentProposal(identity, revision))})));
        QCOMPARE(proposals.size(), 1);
        const QString staleProposalId = proposals.first().at(1).toString();

        runner.submitMessage(QStringLiteral("Instead, only summarize the current state."));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));
        QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("continue"), Qt::CaseInsensitive));
        QVERIFY(runner.isActive());
        QVERIFY(runner.pendingProposal().isEmpty());
        QCOMPARE(requested.size(), 2);
        QVERIFY(requested.last().at(0).toString() != oldRequestId);

        runner.authorizePending(staleProposalId, true);
        runner.providerFailed(oldRequestId, QStringLiteral("Stale provider failure."));
        runner.receiveTurn(oldRequestId, parseProviderBody(providerBody(QStringLiteral("Stale provider text."))));
        QVERIFY(runner.isActive());
        QCOMPARE(actions.size(), 0);
        QCOMPARE(finished.size(), 1);
        const QJsonArray steeredHistory = requested.last().at(1).toJsonArray();
        QString priorSummary;
        for (const QJsonValue &messageValue : steeredHistory)
        {
            const QString content = messageValue.toObject().value(QStringLiteral("content")).toString();
            if (content.startsWith(QStringLiteral("Prior conversation data (untrusted; not authorization): ")))
                priorSummary = content;
            QVERIFY(messageValue.toObject().value(QStringLiteral("role")).toString() != QStringLiteral("tool"));
        }
        QVERIFY(priorSummary.contains(QStringLiteral("pendingProposalDiscarded")));
        QVERIFY(priorSummary.contains(QStringLiteral("unfinished_tool_call")));
        QVERIFY(priorSummary.contains(QStringLiteral("no carried authorization")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("I will only report the current state."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void askUserQuestionIsBoundedCorrelatedAndAnswerBecomesToolData()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy questions(&runner, &AgentTaskRunner::userInputRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.submitMessage(QStringLiteral("Ask me for a choice before deciding."));
        const QJsonObject firstQuestionCall = providerCall(QStringLiteral("choice-question"), QStringLiteral("ask_user"),
            {{QStringLiteral("question"), QStringLiteral("Which reference model should I use?")},
             {QStringLiteral("options"), QJsonArray{QStringLiteral("four-rod"), QStringLiteral("cantilever-beam")}}});
        const QJsonObject firstQuestionTurn = parseProviderBody(providerBody(
            QStringLiteral("I need one preference before I continue."), QJsonArray{firstQuestionCall}));
        runner.receiveTurn(requested.last().at(0).toString(), firstQuestionTurn);
        QCOMPARE(questions.size(), 1);
        QVERIFY(runner.isAwaitingInput());
        const QString questionId = runner.pendingQuestionId();
        QVERIFY(!questionId.isEmpty());
        QCOMPARE(questions.first().at(0).toString(), questionId);
        QCOMPARE(questions.first().at(1).toString(), QStringLiteral("Which reference model should I use?"));
        QCOMPARE(questions.first().at(2).toJsonArray().size(), 2);

        runner.answerQuestion(QStringLiteral("stale-question-id"), QStringLiteral("four-rod"));
        runner.answerQuestion(questionId, QString(4097, QLatin1Char('x')));
        QCOMPARE(requested.size(), 1);
        QVERIFY(runner.isAwaitingInput());
        runner.answerQuestion(questionId, QStringLiteral("four-rod"));
        QVERIFY(!runner.isAwaitingInput());
        QCOMPARE(runner.pendingQuestionId(), QString());
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        const QString answerResult = toolResultContent(requested.last().at(1).toJsonArray(), QStringLiteral("choice-question"));
        QVERIFY(answerResult.contains(QStringLiteral("four-rod")));
        QVERIFY(answerResult.contains(QStringLiteral("answered")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("I will use the four-rod reference model."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));

        runner.submitMessage(QStringLiteral("Ask one more choice."));
        const QJsonObject secondQuestionCall = providerCall(QStringLiteral("stale-choice-question"), QStringLiteral("ask_user"),
            {{QStringLiteral("question"), QStringLiteral("Should I continue waiting?")},
             {QStringLiteral("options"), QJsonArray{QStringLiteral("yes"), QStringLiteral("no")}}});
        const QJsonObject secondQuestionTurn = parseProviderBody(providerBody(QString(), QJsonArray{secondQuestionCall}));
        runner.receiveTurn(requested.last().at(0).toString(), secondQuestionTurn);
        QCOMPARE(questions.size(), 2);
        const QString staleQuestionId = runner.pendingQuestionId();
        QVERIFY(!staleQuestionId.isEmpty());
        runner.submitMessage(QStringLiteral("Stop waiting and summarize now."));
        QVERIFY(!runner.isAwaitingInput());
        QCOMPARE(requested.size(), 4);
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("superseded"));
        runner.answerQuestion(staleQuestionId, QStringLiteral("yes"));
        QCOMPARE(requested.size(), 4);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The new direction is complete."))));
        QCOMPARE(finished.size(), 3);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void assistantMessageMaySynchronouslySteerWithoutAdvancingOldTurn()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy assistantMessages(&runner, &AgentTaskRunner::assistantMessage);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        bool steered = false;
        connect(&runner, &AgentTaskRunner::assistantMessage, &runner,
            [&runner, &steered](const QString &text) {
                if (!steered && text == QStringLiteral("The old task is still speaking."))
                {
                    steered = true;
                    runner.submitMessage(QStringLiteral("Follow the replacement request."));
                }
            });

        runner.submitMessage(QStringLiteral("Initial user message."));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The old task is still speaking."))));
        QVERIFY(steered);
        QCOMPARE(assistantMessages.size(), 1);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));
        QCOMPARE(requested.size(), 2);
        QVERIFY(runner.isActive());
        QVERIFY(requested.last().at(1).toJsonArray().at(1).toObject().value(QStringLiteral("content"))
                    .toString().contains(QStringLiteral("The old task is still speaking.")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The replacement request is complete."))));
        QCOMPARE(assistantMessages.size(), 2);
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void analyzingStateChangeSteeringDoesNotLaunchRetiredProviderTurn()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        bool steered = false;
        connect(&runner, &AgentTaskRunner::stateChanged, &runner,
            [&runner, &steered](const QString &state, const QString &) {
                if (!steered && state == QStringLiteral("analyzing"))
                {
                    steered = true;
                    runner.submitMessage(QStringLiteral("The replacement message."));
                }
            });

        runner.submitMessage(QStringLiteral("The request that should be retired."));
        QVERIFY(steered);
        QCOMPARE(requested.size(), 1); // Only the replacement request reaches the provider.
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));
        const QJsonArray messages = requested.first().at(1).toJsonArray();
        QVERIFY(messages.last().toObject().value(QStringLiteral("content")).toString()
                    .contains(QStringLiteral("Fresh host context (data): ")));
        bool hasReplacement = false;
        for (const QJsonValue &messageValue : messages)
            if (messageValue.toObject().value(QStringLiteral("content")).toString()
                    == QStringLiteral("The replacement message.")) hasReplacement = true;
        QVERIFY(hasReplacement);
        runner.receiveTurn(requested.first().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("Replacement complete."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void executingStateChangeSteeringDoesNotApplyRetiredAutomaticProposal()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString documentId = document.value(QStringLiteral("documentId")).toString();
        const quint64 beforeRevision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        bool steered = false;
        connect(&runner, &AgentTaskRunner::stateChanged, &runner,
            [&runner, &steered](const QString &state, const QString &) {
                if (!steered && state == QStringLiteral("executing"))
                {
                    steered = true;
                    runner.submitMessage(QStringLiteral("Only report the current state."));
                }
            });

        runner.startTask(QStringLiteral("Automatically apply the old proposal."), true);
        const QString oldRequestId = requested.last().at(0).toString();
        const QJsonObject oldProposal = providerCall(QStringLiteral("retired-auto-proposal"),
            QStringLiteral("document_propose"), documentProposal(documentId, beforeRevision));
        runner.receiveTurn(oldRequestId, parseProviderBody(providerBody(QString(), QJsonArray{oldProposal})));
        QVERIFY(steered);
        QCOMPARE(requested.size(), 2);
        QCOMPARE(actions.size(), 0);
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));
        const auto afterDocument = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        QCOMPARE(quint64(afterDocument.value(QStringLiteral("documentRevision")).toDouble()), beforeRevision);
        QVERIFY(runner.isActive());
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("No action was taken."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void reasoningContentIsRetainedPrivatelyAndBoundedTelemetryToolIsValidated()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.submitMessage(QStringLiteral("Read bounded live telemetry."));
        QJsonObject telemetryTurn = parseProviderBody(providerBody(QString(), QJsonArray{
            providerCall(QStringLiteral("telemetry-read"), QStringLiteral("workflow_telemetry"),
                {{QStringLiteral("jobId"), QStringLiteral("job-1")},
                 {QStringLiteral("afterSeq"), 3}, {QStringLiteral("limit"), 16},
                 {QStringLiteral("mode"), QStringLiteral("passive")}})}));
        QJsonObject assistant = telemetryTurn.value(QStringLiteral("assistantMessage")).toObject();
        assistant.insert(QStringLiteral("reasoning_content"), QStringLiteral("provider-private reasoning"));
        telemetryTurn.insert(QStringLiteral("assistantMessage"), assistant);
        runner.receiveTurn(requested.last().at(0).toString(), telemetryTurn);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        bool retainedReasoning = false;
        for (const QJsonValue &messageValue : requested.last().at(1).toJsonArray())
        {
            const QJsonObject message = messageValue.toObject();
            if (message.value(QStringLiteral("role")) == QJsonValue(QStringLiteral("assistant"))
                && message.value(QStringLiteral("reasoning_content")).toString() == QStringLiteral("provider-private reasoning"))
                retainedReasoning = true;
        }
        QVERIFY(retainedReasoning);
        QVERIFY(toolResultContent(requested.last().at(1).toJsonArray(), QStringLiteral("telemetry-read"))
                    .contains(QStringLiteral("WORKSPACE_CLOSED")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("Telemetry is available only in a connected workflow."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));

        runner.submitMessage(QStringLiteral("Reject overlarge provider reasoning."));
        QJsonObject oversized = parseProviderBody(providerBody(QStringLiteral("A normal response.")));
        assistant = oversized.value(QStringLiteral("assistantMessage")).toObject();
        assistant.insert(QStringLiteral("reasoning_content"), QString(128 * 1024 + 1, QLatin1Char('r')));
        oversized.insert(QStringLiteral("assistantMessage"), assistant);
        runner.receiveTurn(requested.last().at(0).toString(), oversized);
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
    }

    void oldProviderReplyCannotCompleteNewTask()
    {
        auto *session = Application::instance()->getSession();
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy cancelled(&runner, &AgentTaskRunner::providerCancellationRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);

        runner.startTask(QStringLiteral("First task"));
        const QString firstRequest = requested.last().at(0).toString();
        runner.cancel();
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(cancelled.first().first().toString(), firstRequest);
        runner.startTask(QStringLiteral("Second task"));
        QCOMPARE(requested.size(), 2);
        const QString secondRequest = requested.last().at(0).toString();
        QVERIFY(firstRequest != secondRequest);

        runner.providerFailed(firstRequest, QStringLiteral("Late failure from the first task."));
        runner.receiveTurn(firstRequest, parseProviderBody(providerBody(QStringLiteral("Late response from the first task."))));
        QVERIFY(runner.isActive());
        QCOMPARE(finished.size(), 1);
        runner.receiveTurn(secondRequest, parseProviderBody(providerBody(QStringLiteral("Second task completed."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void synchronousProviderAbortCallbackCannotOverrideCancellation()
    {
        auto *session = Application::instance()->getSession();
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy cancelled(&runner, &AgentTaskRunner::providerCancellationRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        connect(&runner, &AgentTaskRunner::providerCancellationRequested, &runner,
            [&runner](const QString &requestId) {
                // Models QNetworkReply::abort() synchronously emitting finished.
                runner.providerFailed(requestId, QStringLiteral("The aborted reply also reported a failure."));
            });

        runner.startTask(QStringLiteral("Cancel while the provider request is pending."));
        const QString firstRequest = requested.last().at(0).toString();
        runner.cancel();
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("cancelled"));

        runner.startTask(QStringLiteral("A new task has its own provider request."));
        const QString secondRequest = requested.last().at(0).toString();
        QVERIFY(firstRequest != secondRequest);
        runner.providerFailed(firstRequest, QStringLiteral("Late failure after abort."));
        QVERIFY(runner.isActive());
        QCOMPARE(cancelled.size(), 1);
        QCOMPARE(finished.size(), 1);
        runner.receiveTurn(secondRequest, parseProviderBody(providerBody(QStringLiteral("New task completed."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void queuedToolContinuationCannotAdvanceNewTask()
    {
        auto *session = Application::instance()->getSession();
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);

        runner.startTask(QStringLiteral("Task before cancellation"));
        const QString oldRequest = requested.last().at(0).toString();
        const auto contextTurn = parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(QStringLiteral("old-context-call"), QStringLiteral("context_query"), {})}));
        runner.receiveTurn(oldRequest, contextTurn);
        runner.cancel();
        runner.startTask(QStringLiteral("New task remains at its first turn"));
        QTest::qWait(20);
        QCOMPARE(requested.size(), 2);
        QVERIFY(runner.isActive());
    }

    void deniedProposalIsReportedAndDoesNotMutate()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString identity = document.value(QStringLiteral("documentId")).toString();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Suggest a modal setting change."));
        const auto turn = parseProviderBody(providerBody(QStringLiteral(""), QJsonArray{
            providerCall(QStringLiteral("proposal-1"), QStringLiteral("document_propose"), documentProposal(identity, revision))}));
        runner.receiveTurn(requested.last().at(0).toString(), turn);
        QCOMPARE(ready.size(), 1);
        const QString proposalId = ready.first().at(1).toString();
        QVERIFY(!proposalId.isEmpty());
        runner.authorizePending(QStringLiteral("stale-proposal-id"), true);
        QVERIFY(!runner.pendingProposal().isEmpty());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);

        runner.authorizePending(proposalId, false);
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 1000);
        QVERIFY(requested.last().at(1).toJsonArray().last().toObject().value(QStringLiteral("content"))
                    .toString().contains(QStringLiteral("DECLINED")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("I left the model unchanged."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(applied.size(), 0);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
    }

    void selectedDocumentCanUseExplicitAutomaticTaskAuthorization()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString identity = document.value(QStringLiteral("documentId")).toString();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Set the selected document to 31 modal iterations."), true);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(QStringLiteral("automatic-document-edit"), QStringLiteral("document_propose"),
                                    documentProposal(identity, revision))})));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(applied.size(), 1);
        QCOMPARE(applied.first().at(0).toString(), QStringLiteral("document"));
        QVERIFY(!applied.first().at(1).toJsonObject().value(QStringLiteral("operationId")).toString().isEmpty());
        QCOMPARE(applied.first().at(1).toJsonObject().value(QStringLiteral("documentId")).toString(), identity);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 31u);

        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 1000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral("Applied the approved selected-document change."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void automaticScopeDoesNotExpandWhenSelectionChangesMidTask()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        const QString identity = session->modelCommandStamp(0).first;
        // MainWindow selects newly opened models. Freeze an actually empty
        // selection instead of relying on a later refresh changing revisions.
        session->setModelSelected(0, false);
        QVERIFY(documentContext(host).value(QStringLiteral("documents")).toArray().isEmpty());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        runner.startTask(QStringLiteral("The task began with no selected document."), true);

        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(QStringLiteral("late-selection"), QStringLiteral("document_propose"),
                                    documentProposal(identity, revision))})));

        QCOMPARE(ready.size(), 1);
        QCOMPARE(applied.size(), 0);
        QVERIFY(!runner.pendingProposal().isEmpty());
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
        runner.authorizePending(ready.first().at(1).toString(), false);
    }

    void staleDocumentRevisionNeverProducesApprovalCard()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString identity = document.value(QStringLiteral("documentId")).toString();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Use a revision-bound proposal."));
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(QStringLiteral("stale-revision"), QStringLiteral("document_propose"),
                                    documentProposal(identity, revision + 1))})));
        QCOMPARE(ready.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 1000);
        QVERIFY(requested.last().at(1).toJsonArray().last().toObject().value(QStringLiteral("content"))
                    .toString().contains(QStringLiteral("STALE_REVISION")));
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral("No change was made."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);
    }

    void staleApprovalCannotMutateChangedDocument()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();
        const QString identity = document.value(QStringLiteral("documentId")).toString();
        const quint64 revision = quint64(document.value(QStringLiteral("documentRevision")).toDouble());

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Prepare an edit but require approval."));
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(QStringLiteral("stale-approval"), QStringLiteral("document_propose"),
                                    documentProposal(identity, revision))})));
        QCOMPARE(ready.size(), 1);
        const QString proposalId = ready.first().at(1).toString();

        session->getModel(0).getProblemSetup().getModalSetup().setNIterations(22);
        session->setProblemChanged(0);
        runner.authorizePending(proposalId, true);
        QCOMPARE(applied.size(), 0);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 22u);
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 1000);
        QVERIFY(requested.last().at(1).toJsonArray().last().toObject().value(QStringLiteral("content"))
                    .toString().contains(QStringLiteral("STALE_PREVIEW")));
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral("The changed document was preserved."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
    }

    void malformedBatchAndModelApprovalRequestAreRejectedAtomically()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto document = documentContext(host).value(QStringLiteral("documents")).toArray().first().toObject();

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Read only."));
        const QJsonArray maliciousBatch{
            providerCall(QStringLiteral("valid-first"), QStringLiteral("document_propose"),
                         documentProposal(document.value(QStringLiteral("documentId")).toString(),
                                          quint64(document.value(QStringLiteral("documentRevision")).toDouble()))),
            providerCall(QStringLiteral("model-approval"), QStringLiteral("host_approve"),
                         {{QStringLiteral("previewId"), QStringLiteral("anything")},
                          {QStringLiteral("previewHash"), QStringLiteral("anything")}})};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""), maliciousBatch)));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(ready.size(), 0);
        QCOMPARE(session->getModel(0).getProblemSetup().getModalSetup().getNIterations(), 12u);

        bool hasApprovalTool = false;
        for (const auto &value : AgentTaskRunner::toolDefinitions())
        {
            const auto name = value.toObject().value(QStringLiteral("function")).toObject()
                .value(QStringLiteral("name")).toString();
            hasApprovalTool |= name.contains(QStringLiteral("approve"), Qt::CaseInsensitive)
                || name.contains(QStringLiteral("execute"), Qt::CaseInsensitive);
        }
        QVERIFY(!hasApprovalTool);
    }

    void toolValidationDiagnosticsClassifyRepeatedIdsAndHideMalformedArguments()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);

        runner.startTask(QStringLiteral("Check safe tool-call diagnostics."));
        const QString batchRequest = requested.last().at(0).toString();
        auto duplicateBatch = parseProviderBody(providerBody(QStringLiteral(""), QJsonArray{
            providerCall(QStringLiteral("first-id"), QStringLiteral("context_query"), {}),
            providerCall(QStringLiteral("second-id"), QStringLiteral("context_query"), {})}));
        auto normalizedCalls = duplicateBatch.value(QStringLiteral("toolCalls")).toArray();
        auto repeatedCall = normalizedCalls.at(1).toObject();
        repeatedCall.insert(QStringLiteral("id"), QStringLiteral("first-id"));
        normalizedCalls.replace(1, repeatedCall);
        duplicateBatch.insert(QStringLiteral("toolCalls"), normalizedCalls);
        runner.receiveTurn(batchRequest, duplicateBatch);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
        QVERIFY(finished.last().at(1).toString().contains(QStringLiteral("within one response batch")));

        runner.startTask(QStringLiteral("Check repeated tool-call history."));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        const QString reusedId = QStringLiteral("history-reuse-id");
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(reusedId, QStringLiteral("context_query"), {})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 1000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(reusedId, QStringLiteral("context_query"), {})})));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
        QVERIFY(finished.last().at(1).toString().contains(QStringLiteral("from an earlier turn")));

        runner.startTask(QStringLiteral("Check malformed extension schema diagnostics."));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 4, 1000);
        const QString privateMarker = QStringLiteral("private-source-marker-must-not-appear-in-errors");
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(QStringLiteral("malformed-extension"), QStringLiteral("extension_propose"),
                {{QStringLiteral("command"), QStringLiteral("extension.stage")},
                 {QStringLiteral("arguments"), privateMarker}})})));
        QCOMPARE(finished.size(), 3);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
        const QString schemaFailure = finished.last().at(1).toString();
        QVERIFY(schemaFailure.contains(QStringLiteral("extension_propose")));
        QVERIFY(schemaFailure.contains(QStringLiteral("arguments must be an object")));
        QVERIFY(!schemaFailure.contains(privateMarker));
    }

    void toolCallBudgetStopsAfterThirtyTwoExecutions()
    {
        auto *session = Application::instance()->getSession();
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        int batchNumber = 0;
        int createdCalls = 0;
        connect(&runner, &AgentTaskRunner::turnRequested, &runner,
            [&runner, &batchNumber, &createdCalls](const QString &requestId, const QJsonArray &, const QJsonArray &) {
                QJsonArray calls;
                for (int i = 0; i < 8; ++i)
                {
                    const QString id = QStringLiteral("budget-%1").arg(createdCalls++);
                    calls.append(providerCall(id, QStringLiteral("context_query"), {}));
                }
                ++batchNumber;
                const auto turn = parseProviderBody(providerBody(QStringLiteral(""), calls));
                QTimer::singleShot(0, &runner, [&runner, requestId, turn] { runner.receiveTurn(requestId, turn); });
            });
        runner.startTask(QStringLiteral("Exercise the bounded read-only tool loop."));
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("budget_exhausted"));
        QCOMPARE(batchNumber, 5);
    }

    void extensionProposalRequiresExplicitApprovalAndUsesBoundedRuntimeGateway()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkflowProcess workflow;
        const auto config = WorkflowProcess::defaultConfiguration(directory.path());
        QVERIFY(QFileInfo(config.python).isFile());
        QVERIFY(QFileInfo(config.solver).isFile());
        QVERIFY(QFileInfo(config.native).isFile());
        QVERIFY(QFileInfo(config.packageDirectory).isDir());
        QVERIFY(QFileInfo(config.examples).isDir());
        QVERIFY(!config.trustedExtensions); // Staging stores generated code; this test never executes it.
        QSignalSpy serviceErrors(&workflow, &WorkflowProcess::serviceError);
        workflow.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        QCOMPARE(serviceErrors.size(), 0);

        auto *host = Application::instance()->getSession()->getModelCommandHost();
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        const QString extensionId = QStringLiteral("agent-runner-generated-stats");
        const QString stageCallId = QStringLiteral("stage-generated-candidate");
        runner.startTask(QStringLiteral("Generate and stage a small numeric-series statistics extension."), true);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 1000);

        const QJsonArray reads{
            providerCall(QStringLiteral("host-catalog"), QStringLiteral("host_capabilities"), {}),
            providerCall(QStringLiteral("workflow-catalog"), QStringLiteral("workflow_capabilities"), {}),
            providerCall(QStringLiteral("manifest-schema"), QStringLiteral("schema_get"),
                {{QStringLiteral("schemaId"), QStringLiteral("range.extension/v1/manifest")}}),
            providerCall(QStringLiteral("extension-guidance"), QStringLiteral("knowledge_query"),
                {{QStringLiteral("query"), QStringLiteral("numeric extension input, output and validation")}}),
            providerCall(QStringLiteral("extension-summary"), QStringLiteral("extension_list"), {})};
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("I will inspect the bounded capabilities and schemas first."), reads)));
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 15000);

        const QJsonArray messages = requested.last().at(1).toJsonArray();
        QVERIFY(toolResultContent(messages, QStringLiteral("workflow-catalog")).contains(QStringLiteral("extension.stage")));
        QVERIFY(toolResultContent(messages, QStringLiteral("manifest-schema")).contains(QStringLiteral("extensionId")));
        const auto guidanceDocument = QJsonDocument::fromJson(
            toolResultContent(messages, QStringLiteral("extension-guidance")).toUtf8());
        QVERIFY(guidanceDocument.isObject());
        const auto guidanceItems = guidanceDocument.object().value(QStringLiteral("items")).toArray();
        QVERIFY(!guidanceItems.isEmpty());
        const auto guidanceItem = guidanceItems.first().toObject();
        QVERIFY(guidanceItem.value(QStringLiteral("sourcePath")).toString().startsWith(QStringLiteral("doc/")));
        QVERIFY(!guidanceItem.value(QStringLiteral("text")).toString().trimmed().isEmpty());
        QCOMPARE(guidanceItem.value(QStringLiteral("sha256")).toString().size(), 64);
        QVERIFY(toolResultContent(messages, QStringLiteral("extension-summary")).contains(QStringLiteral("extensions")));
        auto stageArguments = extensionStageArguments(extensionId);
        auto largeSource = stageArguments.value(QStringLiteral("source")).toString();
        largeSource += QStringLiteral("\n# ") + QString(48 * 1024, QLatin1Char('s')) + QLatin1Char('\n');
        stageArguments.insert(QStringLiteral("source"), largeSource);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(stageCallId, QStringLiteral("extension_propose"),
                {{QStringLiteral("command"), QStringLiteral("extension.stage")},
                 {QStringLiteral("arguments"), stageArguments}})})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 15000);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("extension"));
        QCOMPARE(actions.size(), 0); // The task's automatic-action flag never authorizes extension work.
        QVERIFY(!runner.pendingProposal().isEmpty());

        QString queryError;
        const auto beforeApproval = workflowRequest(workflow, QStringLiteral("extension.list"), {}, &queryError);
        QVERIFY2(queryError.isEmpty(), qPrintable(queryError));
        QVERIFY(beforeApproval.value(QStringLiteral("extensions")).toArray().isEmpty());

        const QString proposalId = proposals.first().at(1).toString();
        QVERIFY(!proposalId.isEmpty());
        runner.authorizePending(proposalId, true);
        QTRY_COMPARE_WITH_TIMEOUT(actions.size(), 1, 15000);
        QCOMPARE(actions.first().at(0).toString(), QStringLiteral("extension"));
        const QJsonObject accepted = actions.first().at(1).toJsonObject();
        const QString jobId = accepted.value(QStringLiteral("jobId")).toString();
        QVERIFY(!jobId.isEmpty());

        QElapsedTimer wait;
        wait.start();
        QJsonObject jobResponse;
        QString jobState;
        while (wait.elapsed() < 15000)
        {
            jobResponse = workflowRequest(workflow, QStringLiteral("jobs.query"),
                {{QStringLiteral("jobId"), jobId}}, &queryError);
            QVERIFY2(queryError.isEmpty(), qPrintable(queryError));
            jobState = jobResponse.value(QStringLiteral("job")).toObject().value(QStringLiteral("state")).toString();
            if (jobState == QStringLiteral("succeeded") || jobState == QStringLiteral("failed")
                || jobState == QStringLiteral("cancelled")) break;
            QTest::qWait(100);
        }
        QCOMPARE(jobState, QStringLiteral("succeeded"));

        const auto listed = workflowRequest(workflow, QStringLiteral("extension.list"),
            {{QStringLiteral("extensionId"), extensionId}, {QStringLiteral("candidateLimit"), 4}}, &queryError);
        QVERIFY2(queryError.isEmpty(), qPrintable(queryError));
        const auto entries = listed.value(QStringLiteral("extensions")).toArray();
        QCOMPARE(entries.size(), 1);
        const auto entry = entries.first().toObject();
        QCOMPARE(entry.value(QStringLiteral("candidateTotal")).toInt(), 1);
        const auto candidates = entry.value(QStringLiteral("candidates")).toArray();
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().toObject().value(QStringLiteral("state")).toString(), QStringLiteral("staged"));
        const QString candidateId = candidates.first().toObject().value(QStringLiteral("candidateId")).toString();
        QVERIFY(!candidateId.isEmpty());

        const auto candidateResponse = workflowRequest(workflow, QStringLiteral("extension.candidate.get"),
            {{QStringLiteral("candidateId"), candidateId}}, &queryError);
        QVERIFY2(queryError.isEmpty(), qPrintable(queryError));
        const auto candidate = candidateResponse.value(QStringLiteral("candidate")).toObject();
        QCOMPARE(candidate.value(QStringLiteral("manifest")).toObject().value(QStringLiteral("extensionId")).toString(), extensionId);
        QCOMPARE(candidate.value(QStringLiteral("source")).toString(), largeSource);
        QCOMPARE(candidate.value(QStringLiteral("tests")).toObject(), stageArguments.value(QStringLiteral("tests")).toObject());
        QVERIFY(candidate.contains(QStringLiteral("baseSource")));
        QVERIFY(candidate.contains(QStringLiteral("sourceDiff")));

        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 3, 5000);
        const QString candidateCallId = QStringLiteral("selected-candidate-details");
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(candidateCallId, QStringLiteral("extension_candidate_get"),
                {{QStringLiteral("candidateId"), candidateId}})})));
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 4, 15000);
        const QString projectedCandidateText = toolResultContent(
            requested.last().at(1).toJsonArray(), candidateCallId);
        QVERIFY(projectedCandidateText.toUtf8().size() > 48 * 1024);
        QVERIFY(projectedCandidateText.toUtf8().size() < 512 * 1024);
        const auto projectedDocument = QJsonDocument::fromJson(projectedCandidateText.toUtf8());
        QVERIFY(projectedDocument.isObject());
        const auto projectedCandidate = projectedDocument.object().value(QStringLiteral("candidate")).toObject();
        QCOMPARE(projectedCandidate.value(QStringLiteral("source")).toString(), largeSource);
        QCOMPARE(projectedCandidate.value(QStringLiteral("tests")).toObject(), stageArguments.value(QStringLiteral("tests")).toObject());
        QVERIFY(!projectedCandidate.contains(QStringLiteral("baseSource")));
        QVERIFY(!projectedCandidate.contains(QStringLiteral("sourceDiff")));
        const auto omittedFields = projectedCandidate.value(QStringLiteral("omittedFields")).toArray();
        QVERIFY(omittedFields.contains(QStringLiteral("baseSource")));
        QVERIFY(omittedFields.contains(QStringLiteral("sourceDiff")));

        const QString listCallId = QStringLiteral("selected-extension-details");
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(listCallId, QStringLiteral("extension_list"),
                {{QStringLiteral("extensionId"), extensionId}, {QStringLiteral("candidateOffset"), 0},
                 {QStringLiteral("candidateLimit"), 4}})})));
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 5, 15000);
        QVERIFY(toolResultContent(requested.last().at(1).toJsonArray(), listCallId).contains(candidateId));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The extension candidate is staged and awaiting validation and explicit activation."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(serviceErrors.size(), 0);
        workflow.stop();
    }

    void workflowReconnectAtSamePathDoesNotExtendTaskAuthority()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkflowProcess workflow;
        const auto config = WorkflowProcess::defaultConfiguration(directory.path());
        workflow.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);

        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Configure the connected reference workflow."), true);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 1000);
        const QString staleRequestId = requested.first().at(0).toString();

        // Reopen the same directory, which creates a new runtime session and
        // increments its project revision. The original task must not inherit
        // authority merely because the path string is unchanged.
        workflow.stop();
        QElapsedTimer restart;
        restart.start();
        while (!workflow.isReady() && restart.elapsed() < 10000)
        {
            workflow.start(config);
            QTest::qWait(100);
        }
        QVERIFY(workflow.isReady());

        const QString proposalCallId = QStringLiteral("workflow-after-reconnect");
        runner.receiveTurn(staleRequestId, parseProviderBody(providerBody(QStringLiteral(""),
            QJsonArray{providerCall(proposalCallId, QStringLiteral("workflow_propose"),
                {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
                 {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("model"), QStringLiteral("four-rod")},
                     {QStringLiteral("dt"), 0.005}, {QStringLiteral("steps"), 100},
                     {QStringLiteral("targetDamping"), 0.1}}}})})));
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 5000);
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(actions.size(), 0);
        QVERIFY(toolResultContent(requested.last().at(1).toJsonArray(), proposalCallId)
                    .contains(QStringLiteral("WORKSPACE_CHANGED")));
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("This task cannot act in the reconnected workflow session."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));

        // A pending preview is also retired immediately on disconnect, so a
        // stale approval card cannot be accepted for the disconnected session.
        runner.startTask(QStringLiteral("Propose a workflow configuration."));
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 3, 1000);
        const auto previewCall = providerCall(QStringLiteral("pending-workflow-change"), QStringLiteral("workflow_propose"),
            {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("model"), QStringLiteral("four-rod")},
                 {QStringLiteral("dt"), 0.005}, {QStringLiteral("steps"), 100},
                 {QStringLiteral("targetDamping"), 0.1}}}});
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral(""), QJsonArray{previewCall})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 5000);
        const QString pendingProposalId = proposals.first().at(1).toString();
        workflow.stop();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 1000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
        runner.authorizePending(pendingProposalId, true);
        QCOMPARE(actions.size(), 0);
    }

    void steeringPreservesApprovedAndWaitingWorkflowJobOutcomes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        WorkflowProcess workflow;
        const auto config = WorkflowProcess::defaultConfiguration(directory.path());
        QSignalSpy serviceErrors(&workflow, &WorkflowProcess::serviceError);
        workflow.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);
        QCOMPARE(serviceErrors.size(), 0);

        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        QString submittedJobId;
        bool armedRunSteering = false;
        connect(&runner, &AgentTaskRunner::actionCompleted, &runner,
            [&runner, &submittedJobId, &armedRunSteering](const QString &, const QJsonObject &result) {
                const QString jobId = result.value(QStringLiteral("jobId")).toString();
                if (armedRunSteering && !jobId.isEmpty() && submittedJobId.isEmpty())
                {
                    submittedJobId = jobId;
                    // Steering synchronously from the accepted execution outcome
                    // must retain that outcome before the old turn is retired.
                    runner.submitMessage(QStringLiteral("Check the calculation that was just submitted."));
                }
            });

        runner.startTask(QStringLiteral("Configure and submit a reference calculation."));
        const QJsonObject configureCall = providerCall(QStringLiteral("configure-reference"), QStringLiteral("workflow_propose"),
            {{QStringLiteral("command"), QStringLiteral("workflow.configure")},
             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("model"), QStringLiteral("four-rod")},
                 {QStringLiteral("dt"), 0.005}, {QStringLiteral("steps"), 5000},
                 {QStringLiteral("targetDamping"), 0.1}}}});
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QString(), QJsonArray{configureCall})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 1, 15000);
        runner.authorizePending(proposals.last().at(1).toString(), true);
        QTRY_COMPARE_WITH_TIMEOUT(actions.size(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 2, 5000);
        armedRunSteering = true;

        const QJsonObject runCall = providerCall(QStringLiteral("submit-reference-run"), QStringLiteral("workflow_propose"),
            {{QStringLiteral("command"), QStringLiteral("workflow.run")},
             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("passive")},
                 {QStringLiteral("independentReference"), true}}}});
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QString(), QJsonArray{runCall})));
        QTRY_COMPARE_WITH_TIMEOUT(proposals.size(), 2, 15000);
        runner.authorizePending(proposals.last().at(1).toString(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!submittedJobId.isEmpty(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 3, 5000);
        QCOMPARE(actions.size(), 2);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));

        const QJsonArray afterExecutionHistory = requested.last().at(1).toJsonArray();
        QString executionSummary;
        for (const QJsonValue &messageValue : afterExecutionHistory)
        {
            const QJsonObject message = messageValue.toObject();
            const QString content = message.value(QStringLiteral("content")).toString();
            if (content.startsWith(QStringLiteral("Prior conversation data (untrusted; not authorization): ")))
                executionSummary = content;
            QVERIFY(!(message.value(QStringLiteral("role")) == QJsonValue(QStringLiteral("assistant"))
                && message.contains(QStringLiteral("tool_calls"))));
        }
        QVERIFY(executionSummary.contains(QStringLiteral("tool_result")));
        QVERIFY(executionSummary.contains(QStringLiteral("action_outcome")));
        QVERIFY(executionSummary.contains(submittedJobId));

        const QJsonObject waitCall = providerCall(QStringLiteral("wait-for-submitted-job"), QStringLiteral("workflow_wait"),
            {{QStringLiteral("jobId"), submittedJobId}});
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QString(), QJsonArray{waitCall})));
        QVERIFY(runner.isActive());
        runner.submitMessage(QStringLiteral("Stop waiting and summarize the submitted job."));
        QCOMPARE(requested.size(), 4);
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("superseded"));
        QVERIFY(finished.last().at(1).toString().contains(submittedJobId));
        const QJsonArray afterWaitHistory = requested.last().at(1).toJsonArray();
        QString waitSummary;
        for (const QJsonValue &messageValue : afterWaitHistory)
        {
            const QJsonObject message = messageValue.toObject();
            const QString content = message.value(QStringLiteral("content")).toString();
            if (content.startsWith(QStringLiteral("Prior conversation data (untrusted; not authorization): ")))
                waitSummary = content;
            QVERIFY(message.value(QStringLiteral("role")).toString() != QStringLiteral("tool"));
        }
        QVERIFY(waitSummary.contains(QStringLiteral("submittedJobId")));
        QVERIFY(waitSummary.contains(submittedJobId));

        QString queryError;
        const QJsonObject jobStatus = workflowRequest(workflow, QStringLiteral("jobs.query"),
            {{QStringLiteral("jobId"), submittedJobId}}, &queryError);
        QVERIFY2(queryError.isEmpty(), qPrintable(queryError));
        QCOMPARE(jobStatus.value(QStringLiteral("job")).toObject().value(QStringLiteral("jobId")).toString(), submittedJobId);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The submitted job remains available in the workflow panel."))));
        QCOMPARE(finished.size(), 3);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(serviceErrors.size(), 0);
        workflow.stop();
    }
};

int runAgentTaskRunnerTests(int argc, char **argv)
{
    TestAgentTaskRunner test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_task_runner.moc"
