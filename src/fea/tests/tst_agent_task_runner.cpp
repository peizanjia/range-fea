#include <QtTest>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <limits>

#include <rai_agent.h>
#include <rml_model.h>
#include <rml_time_solver.h>
#include <rmodel_command_service.h>
#include <rtrusted_experiment_service.h>

#include "agent_task_runner.h"
#include "application.h"
#include "model_command_host.h"
#include "mesh_job_controller.h"
#include "session.h"
#include "model_io.h"
#include "solver_manager.h"

namespace
{
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
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS)); return model;
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

QJsonObject trustedExperimentBudgets(int solves = 1, int repairs = 0, int modelTurns = 12)
{
    return {{QStringLiteral("solves"), solves}, {QStringLiteral("repairs"), repairs},
            {QStringLiteral("modelTurns"), modelTurns}};
}

QJsonObject rawProviderCall(const QString &id, const QString &name, const QJsonObject &arguments)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name},
                {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}};
}

QJsonObject providerCall(const QString &id, const QString &name, const QJsonObject &arguments)
{
    QString toolName = name;
    QJsonObject toolArguments = arguments;
    if (name == QStringLiteral("document_propose")
        && arguments.value(QStringLiteral("command")) == QJsonValue(QStringLiteral("experiment.run")))
    {
        toolName = QStringLiteral("experiment_run");
        toolArguments = {{QStringLiteral("documentId"), arguments.value(QStringLiteral("documentId"))},
            {QStringLiteral("documentRevision"), arguments.value(QStringLiteral("documentRevision"))}};
        const QJsonObject commandArguments = arguments.value(QStringLiteral("arguments")).toObject();
        for (auto it = commandArguments.constBegin(); it != commandArguments.constEnd(); ++it)
            toolArguments.insert(it.key(), it.value());
    }
    return rawProviderCall(id, toolName, toolArguments);
}

QSet<QString> functionNames(const QJsonArray &definitions)
{
    QSet<QString> names;
    for (const QJsonValue &value : definitions)
        names.insert(value.toObject().value(QStringLiteral("function")).toObject()
            .value(QStringLiteral("name")).toString());
    return names;
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

QJsonObject trustedRodProblemSpec(int segments = 4)
{
    const auto q = [](double value, const char *unit) {
        return QJsonObject{{QStringLiteral("value"), value}, {QStringLiteral("unit"), QString::fromLatin1(unit)}};
    };
    const QJsonArray load{q(1000.0, "N"), q(0.0, "N"), q(0.0, "N")};
    return {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("modelType"), QStringLiteral("axialRod")},
        {QStringLiteral("name"), QStringLiteral("Agent Stage 6 rod")},
        {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), q(1.0, "m")},
            {QStringLiteral("segments"), segments}}},
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
            {QStringLiteral("components"), load}}}},
        {QStringLiteral("initialConditions"), QJsonArray{}},
        {QStringLiteral("observables"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("component"), QStringLiteral("ux")}}}},
        {QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}}}};
}

QJsonObject trustedBeamProblemSpec(int segments = 4)
{
    QJsonObject spec = trustedRodProblemSpec(segments);
    spec.insert(QStringLiteral("name"), QStringLiteral("Agent Stage 6 cantilever beam"));
    spec.insert(QStringLiteral("modelType"), QStringLiteral("cantileverBeam"));
    spec.insert(QStringLiteral("section"), QJsonObject{
        {QStringLiteral("area"), QJsonObject{{QStringLiteral("value"), 1.0e-4}, {QStringLiteral("unit"), QStringLiteral("m2")}}},
        {QStringLiteral("iy"), QJsonObject{{QStringLiteral("value"), 8.333333333333333e-10}, {QStringLiteral("unit"), QStringLiteral("m4")}}},
        {QStringLiteral("iz"), QJsonObject{{QStringLiteral("value"), 8.333333333333333e-10}, {QStringLiteral("unit"), QStringLiteral("m4")}}},
        {QStringLiteral("torsionalConstant"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-9}, {QStringLiteral("unit"), QStringLiteral("m4")}}},
        {QStringLiteral("wy"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-7}, {QStringLiteral("unit"), QStringLiteral("m3")}}},
        {QStringLiteral("wz"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-7}, {QStringLiteral("unit"), QStringLiteral("m3")}}},
        {QStringLiteral("wt"), QJsonObject{{QStringLiteral("value"), 1.666666666666667e-7}, {QStringLiteral("unit"), QStringLiteral("m3")}}},
        {QStringLiteral("localY"), QJsonArray{0.0, 1.0, 0.0}}});
    spec.insert(QStringLiteral("supports"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
        {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                                             QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")}}}});
    spec.insert(QStringLiteral("loads"), QJsonArray{QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("pointForce")},
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("components"), QJsonArray{
            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}},
            QJsonObject{{QStringLiteral("value"), 0.0}, {QStringLiteral("unit"), QStringLiteral("N")}},
            QJsonObject{{QStringLiteral("value"), -1.0}, {QStringLiteral("unit"), QStringLiteral("N")}}}}}});
    spec.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
        {QStringLiteral("quantity"), QStringLiteral("displacement")},
        {QStringLiteral("component"), QStringLiteral("uz")}}});
    spec.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}});
    return spec;
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
    void evaluationCaptureEnvironmentConfigurationIsExplicitAndExact()
    {
        Application::EvaluationCaptureConfiguration configuration;
        QString error;
        QVERIFY2(Application::parseEvaluationCaptureConfiguration(false, QString(), false, QString(),
                                                                  &configuration, &error), qPrintable(error));
        QVERIFY(!configuration.enabled);
        QCOMPARE(configuration.arm, AgentTaskRunner::EvaluationArm::Standard);

        QVERIFY(!Application::parseEvaluationCaptureConfiguration(true, QStringLiteral("direct_nl"),
                                                                   false, QString(), &configuration, &error));
        QVERIFY(error.contains(QStringLiteral("both")));
        QVERIFY(!Application::parseEvaluationCaptureConfiguration(false, QString(), true,
                                                                   QStringLiteral("capture.jsonl"),
                                                                   &configuration, &error));
        QVERIFY(error.contains(QStringLiteral("both")));

        const QList<QPair<QString, AgentTaskRunner::EvaluationArm>> acceptedArms{
            {QStringLiteral("direct_nl"), AgentTaskRunner::EvaluationArm::DirectNl},
            {QStringLiteral("tools_feedback"), AgentTaskRunner::EvaluationArm::ToolsFeedback},
            {QStringLiteral("full_trusted"), AgentTaskRunner::EvaluationArm::FullTrusted}};
        for (const auto &entry : acceptedArms)
        {
            QVERIFY2(Application::parseEvaluationCaptureConfiguration(true, entry.first, true,
                QStringLiteral("capture.jsonl"), &configuration, &error), qPrintable(error));
            QVERIFY(configuration.enabled);
            QCOMPARE(configuration.arm, entry.second);
            QCOMPARE(configuration.capturePath, QStringLiteral("capture.jsonl"));
        }

        for (const QString &invalidArm : {QStringLiteral("Direct_Nl"), QStringLiteral("direct_nl "),
                                          QStringLiteral("standard"), QStringLiteral("")})
            QVERIFY(!Application::parseEvaluationCaptureConfiguration(true, invalidArm, true,
                QStringLiteral("capture.jsonl"), &configuration, &error));
        QVERIFY(!Application::parseEvaluationCaptureConfiguration(true, QStringLiteral("direct_nl"), true,
                                                                   QStringLiteral("  "), &configuration, &error));
        QVERIFY(error.contains(QStringLiteral("new JSONL")));
    }

    void trustedExperimentLifecycleStateAllowlistMatchesHostContract()
    {
        for (const QString &state : RTrustedExperimentService::lifecycleStates())
            QVERIFY2(AgentTaskRunner::isKnownExperimentLifecycleState(state), qPrintable(state));
        QVERIFY(!AgentTaskRunner::isKnownExperimentLifecycleState(QStringLiteral("running")));
        QVERIFY(!AgentTaskRunner::isKnownExperimentLifecycleState(QStringLiteral("unknown")));
        QVERIFY(!AgentTaskRunner::isKnownExperimentLifecycleState(QString()));
    }

    void toolResultObservationMatchesDeliveredFallback()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        runner.active = true;
        runner.activityTaskId = QStringLiteral("task-oversized-result");
        runner.currentCall = {{QStringLiteral("id"), QStringLiteral("call-oversized")},
                              {QStringLiteral("name"), QStringLiteral("native_job")}};
        QSignalSpy observed(&runner, &AgentTaskRunner::toolResultObserved);

        runner.finishTool({{QStringLiteral("status"), QStringLiteral("ok")},
                           {QStringLiteral("payload"), QString(600 * 1024, QLatin1Char('x'))}});

        QCOMPARE(observed.size(), 1);
        const QJsonObject event = observed.first().first().toJsonObject();
        const QByteArray delivered = "{\"status\":\"error\",\"code\":\"RESULT_TOO_LARGE\",\"message\":\"Read a smaller result scope.\"}";
        QCOMPARE(event.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        QCOMPARE(event.value(QStringLiteral("code")).toString(), QStringLiteral("RESULT_TOO_LARGE"));
        QCOMPARE(event.value(QStringLiteral("contentBytes")).toInt(), delivered.size());
        QCOMPARE(event.value(QStringLiteral("contentSha256")).toString(),
                 QString::fromLatin1(QCryptographicHash::hash(delivered, QCryptographicHash::Sha256).toHex()));
        QCOMPARE(runner.history.last().toObject().value(QStringLiteral("content")).toString(), QString::fromUtf8(delivered));
        runner.cancel();
    }

    void toolResultObservationKeepsEmptyCodeForSuccess()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        runner.active = true;
        runner.activityTaskId = QStringLiteral("task-result-without-code");
        runner.currentCall = {{QStringLiteral("id"), QStringLiteral("call-no-code")},
                              {QStringLiteral("name"), QStringLiteral("native_job")}};
        QSignalSpy observed(&runner, &AgentTaskRunner::toolResultObserved);

        runner.finishTool({{QStringLiteral("status"), QStringLiteral("ok")},
                           {QStringLiteral("message"), QStringLiteral("completed")}});

        QCOMPARE(observed.size(), 1);
        const QJsonObject event = observed.first().first().toJsonObject();
        QCOMPARE(event.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(event.value(QStringLiteral("code")).isString());
        QVERIFY(event.value(QStringLiteral("code")).toString().isEmpty());
        runner.cancel();
    }

    void humanQuestionWaitPreservesRemainingExecutionBudget()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Ask before continuing."));
        QVERIFY(runner.budgetTimer.isActive());
        QCOMPARE(runner.remainingExecutionBudgetMs, 300000);
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            // Simulate execution having consumed time without a wall-clock sleep.
            const int remaining = 4000 - cycle * 1000;
            runner.budgetTimer.start(remaining);
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                {providerCall(QStringLiteral("question-%1").arg(cycle), QStringLiteral("ask_user"),
                    {{QStringLiteral("question"), QStringLiteral("Which model?")},
                     {QStringLiteral("options"), QJsonArray{QStringLiteral("rod"), QStringLiteral("beam")}}})})));
            QVERIFY(runner.isAwaitingInput());
            QVERIFY(runner.executionBudgetPaused);
            QVERIFY(!runner.budgetTimer.isActive());
            const int saved = runner.remainingExecutionBudgetMs;
            QVERIFY(saved > 0 && saved <= remaining);
            // A timeout delivered after the timer was stopped must not retire the wait.
            QVERIFY(QMetaObject::invokeMethod(&runner.budgetTimer, "timeout", Qt::DirectConnection));
            QCOMPARE(finished.size(), 0);
            runner.answerQuestion(QStringLiteral("stale"), QStringLiteral("rod"));
            runner.answerQuestion(runner.pendingQuestionId(), QString(4097, QLatin1Char('x')));
            QCOMPARE(runner.remainingExecutionBudgetMs, saved);
            QVERIFY(runner.executionBudgetPaused);
            QVERIFY(runner.pauseExecutionBudget());
            QCOMPARE(runner.remainingExecutionBudgetMs, saved);
            runner.answerQuestion(runner.pendingQuestionId(), QStringLiteral("rod"));
            QVERIFY(!runner.executionBudgetPaused);
            QVERIFY(runner.budgetTimer.isActive());
            QVERIFY(runner.budgetTimer.remainingTime() <= saved);
            QTRY_COMPARE_WITH_TIMEOUT(requested.size(), cycle + 2, 1000);
        }
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral("Done."))));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        QVERIFY(!runner.budgetTimer.isActive());
        QVERIFY(!runner.executionBudgetPaused);
        QCOMPARE(runner.remainingExecutionBudgetMs, 0);
    }

    void manualApprovalWaitResumesBudget_data()
    {
        QTest::addColumn<bool>("accepted");
        QTest::newRow("approve") << true;
        QTest::newRow("decline") << false;
    }

    void manualApprovalWaitResumesBudget()
    {
        QFETCH(bool, accepted);
        auto *session = Application::instance()->getSession();
        session->addModel(modalFixture());
        const uint modelId = session->getNModels() - 1;
        session->setModelSelected(modelId, true);
        const auto stamp = session->modelCommandStamp(modelId);
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        runner.startTask(QStringLiteral("Prepare an edit for manual approval."));
        runner.budgetTimer.start(4000);
        bool pausedWhenPublished = false;
        connect(&runner, &AgentTaskRunner::proposalReady, &runner, [&] {
            pausedWhenPublished = runner.executionBudgetPaused && !runner.budgetTimer.isActive();
        });
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            {providerCall(QStringLiteral("manual-budget"), QStringLiteral("document_propose"),
                          documentProposal(stamp.first, stamp.second))})));
        QCOMPARE(ready.size(), 1);
        QVERIFY(pausedWhenPublished);
        const int saved = runner.remainingExecutionBudgetMs;
        QVERIFY(saved > 0 && saved <= 4000);
        runner.authorizePending(QStringLiteral("stale"), accepted);
        QVERIFY(runner.executionBudgetPaused);
        QCOMPARE(runner.remainingExecutionBudgetMs, saved);
        runner.authorizePending(ready.first().at(1).toString(), accepted);
        QVERIFY(!runner.executionBudgetPaused);
        QVERIFY(runner.budgetTimer.isActive());
        QVERIFY(runner.budgetTimer.remainingTime() <= saved);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        QCOMPARE(actions.size(), accepted ? 1 : 0);
        runner.cancel();
        QVERIFY(!runner.budgetTimer.isActive());
        QVERIFY(!runner.executionBudgetPaused);
        QCOMPARE(runner.remainingExecutionBudgetMs, 0);
        session->clear();
    }

    void synchronousHumanAnswerResumesBudgetBeforeNextTurn()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        bool pausedWhenPublished = false;
        connect(&runner, &AgentTaskRunner::userInputRequested, &runner,
            [&](const QString &id, const QString &, const QJsonArray &) {
                pausedWhenPublished = runner.executionBudgetPaused && !runner.budgetTimer.isActive();
                runner.answerQuestion(id, QStringLiteral("yes"));
            });
        runner.startTask(QStringLiteral("Ask one question."));
        runner.budgetTimer.start(4000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            {providerCall(QStringLiteral("sync-answer"), QStringLiteral("ask_user"),
                {{QStringLiteral("question"), QStringLiteral("Continue?")},
                 {QStringLiteral("options"), QJsonArray{QStringLiteral("yes"), QStringLiteral("no")}}})})));
        QVERIFY(pausedWhenPublished);
        QVERIFY(!runner.isAwaitingInput());
        QVERIFY(!runner.executionBudgetPaused);
        QVERIFY(runner.budgetTimer.isActive());
        QVERIFY(runner.budgetTimer.remainingTime() <= 4000);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        runner.cancel();
    }

    void automaticProposalKeepsExecutionBudgetRunning()
    {
        auto *session = Application::instance()->getSession();
        session->addModel(modalFixture());
        const uint modelId = session->getNModels() - 1;
        session->setModelSelected(modelId, true);
        const auto stamp = session->modelCommandStamp(modelId);
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        bool runningWhenPublished = false;
        connect(&runner, &AgentTaskRunner::proposalReady, &runner, [&] {
            runningWhenPublished = runner.budgetTimer.isActive() && !runner.executionBudgetPaused;
        });
        runner.startTask(QStringLiteral("Apply the selected document edit."), true);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            {providerCall(QStringLiteral("auto-budget"), QStringLiteral("document_propose"),
                          documentProposal(stamp.first, stamp.second))})));
        QVERIFY(runningWhenPublished);
        QTRY_COMPARE_WITH_TIMEOUT(actions.size(), 1, 1000);
        QVERIFY(!runner.executionBudgetPaused);
        runner.cancel();
        session->clear();
    }

    void experimentModelTurnBudgetCapsProviderTurnsAtFour()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy cancellations(&runner, &AgentTaskRunner::experimentCancellationObserved);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Submit an experiment with a four-turn model budget."), true);
        QCOMPARE(requested.size(), 1);

        const QJsonObject arguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
            {QStringLiteral("budgets"), trustedExperimentBudgets(1, 0, 4)}};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("four-turn-experiment"),
                QStringLiteral("experiment_run"), arguments)})));
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
        const QString experimentId = proposals.first().at(2).toJsonObject()
            .value(QStringLiteral("experimentId")).toString();
        QVERIFY(!experimentId.isEmpty());

        runner.authorizePending(proposals.first().at(1).toString(), true);
        QCOMPARE(actions.size(), 1);
        const QJsonObject expectedSourceBinding = runner.taskExperimentSourceBindings.value(experimentId);
        QVERIFY(!expectedSourceBinding.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("four-turn-experiment-wait"),
                QStringLiteral("experiment_wait"), {{QStringLiteral("experimentId"), experimentId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 30000);
        const QJsonObject waitResponse = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("four-turn-experiment-wait")).toUtf8()).object();
        QCOMPARE(waitResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(waitResponse.value(QStringLiteral("result")).toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("candidate_ready"));
        for (int providerTurn = 3; providerTurn <= 4; ++providerTurn)
        {
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("capability-%1").arg(providerTurn),
                    QStringLiteral("host_capabilities"), {})})));
            if (providerTurn < 4)
                QTRY_COMPARE_WITH_TIMEOUT(requested.size(), providerTurn + 1, 1000);
            else
                QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        }
        QCOMPARE(requested.size(), 4);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("budget_exhausted"));
        const QString finishedMessage = finished.first().at(1).toString();
        QVERIFY(finishedMessage.contains(QStringLiteral("Agent-authored completion report is unavailable")));
        QVERIFY(finishedMessage.contains(experimentId));
        QVERIFY(finishedMessage.contains(QStringLiteral("\"state\":\"candidate_ready\"")));
        QVERIFY(finishedMessage.contains(QStringLiteral("\"verificationPassed\":true")));
        QVERIFY(finishedMessage.contains(QStringLiteral("\"providerTurnRequestsIssued\":4")));
        QVERIFY(finishedMessage.contains(QStringLiteral("\"providerTurnLimit\":4")));
        const QString evidenceMarker = QStringLiteral("it is not model-generated prose: ");
        const qsizetype evidenceStart = finishedMessage.indexOf(evidenceMarker);
        QVERIFY(evidenceStart >= 0);
        const QJsonDocument evidenceDocument = QJsonDocument::fromJson(
            finishedMessage.mid(evidenceStart + evidenceMarker.size()).toUtf8());
        QVERIFY(evidenceDocument.isObject());
        const QJsonArray experiments = evidenceDocument.object().value(QStringLiteral("experiments")).toArray();
        QCOMPARE(experiments.size(), 1);
        const QJsonObject hostFacts = experiments.first().toObject();
        const QJsonObject sourceStamp = hostFacts.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        QCOMPARE(sourceStamp.value(QStringLiteral("documentId")).toString(),
                 expectedSourceBinding.value(QStringLiteral("documentId")).toString());
        QCOMPARE(sourceStamp.value(QStringLiteral("revision")).toString(),
                 QString::number(expectedSourceBinding.value(QStringLiteral("documentRevision")).toDouble()));
        QCOMPARE(sourceStamp.value(QStringLiteral("sessionEpoch")).toString(),
                 expectedSourceBinding.value(QStringLiteral("sessionEpoch")).toString());
        QCOMPARE(cancellations.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const QJsonObject query = host->handle(hostRequest(QStringLiteral("experiment.query"),
                {{QStringLiteral("experimentId"), experimentId}}));
            return query.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("ok"))
                && query.value(QStringLiteral("result")).toObject().value(QStringLiteral("state"))
                    == QJsonValue(QStringLiteral("candidate_ready"));
        }(), 30000);
        session->clear();
    }

    void budgetExhaustionEvidenceRejectsMismatchedBindings()
    {
        const QString experimentId = QStringLiteral("experiment-identity-fixture");
        const QString documentId = QStringLiteral("document-identity-fixture");
        const QString sessionEpoch = QStringLiteral("session-identity-fixture");
        const QString digest(64, QLatin1Char('a'));
        const QJsonObject sourceBinding{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("documentRevision"), 7},
            {QStringLiteral("sessionEpoch"), sessionEpoch},
            {QStringLiteral("authorizationDigest"), digest}};
        const QJsonObject trustedSummary{{QStringLiteral("experimentId"), experimentId},
            {QStringLiteral("authorizationDigest"), digest},
            {QStringLiteral("sourceRuntimeStamp"), QJsonObject{
                {QStringLiteral("documentId"), documentId},
                {QStringLiteral("revision"), QStringLiteral("7")},
                {QStringLiteral("sessionEpoch"), sessionEpoch}}},
            {QStringLiteral("state"), QStringLiteral("candidate_ready")},
            {QStringLiteral("verificationPassed"), true}};
        const QJsonObject successfulResponse{{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), trustedSummary}};
        const QJsonObject accepted = AgentTaskRunner::budgetExhaustionExperimentEvidence(
            experimentId, sourceBinding, successfulResponse);
        QCOMPARE(accepted.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
        QCOMPARE(accepted.value(QStringLiteral("verificationPassed")).toBool(), true);
        QVERIFY(!accepted.contains(QStringLiteral("evidenceStatus")));

        QJsonObject wrongDigest = trustedSummary;
        wrongDigest.insert(QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('b')));
        const QJsonObject rejectedDigest = AgentTaskRunner::budgetExhaustionExperimentEvidence(
            experimentId, sourceBinding,
            {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("result"), wrongDigest}});
        QCOMPARE(rejectedDigest.value(QStringLiteral("evidenceStatus")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(rejectedDigest.value(QStringLiteral("code")).toString(),
                 QStringLiteral("HOST_FACT_IDENTITY_MISMATCH"));
        QVERIFY(!rejectedDigest.contains(QStringLiteral("state")));
        QVERIFY(!rejectedDigest.contains(QStringLiteral("verificationPassed")));

        QJsonObject wrongSource = trustedSummary;
        wrongSource.insert(QStringLiteral("sourceRuntimeStamp"), QJsonObject{
            {QStringLiteral("documentId"), QStringLiteral("other-document")},
            {QStringLiteral("revision"), QStringLiteral("7")},
            {QStringLiteral("sessionEpoch"), sessionEpoch}});
        const QJsonObject rejectedSource = AgentTaskRunner::budgetExhaustionExperimentEvidence(
            experimentId, sourceBinding,
            {{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("result"), wrongSource}});
        QCOMPARE(rejectedSource.value(QStringLiteral("evidenceStatus")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(rejectedSource.value(QStringLiteral("code")).toString(),
                 QStringLiteral("HOST_FACT_IDENTITY_MISMATCH"));
        QVERIFY(!rejectedSource.contains(QStringLiteral("state")));
        QVERIFY(!rejectedSource.contains(QStringLiteral("verificationPassed")));

        QJsonObject missingIdentityBinding = sourceBinding;
        missingIdentityBinding.insert(QStringLiteral("documentId"), QString());
        missingIdentityBinding.insert(QStringLiteral("sessionEpoch"), QString());
        QJsonObject missingIdentitySummary = trustedSummary;
        missingIdentitySummary.insert(QStringLiteral("sourceRuntimeStamp"), QJsonObject{
            {QStringLiteral("documentId"), QString()},
            {QStringLiteral("revision"), QStringLiteral("7")},
            {QStringLiteral("sessionEpoch"), QString()}});
        const QJsonObject rejectedMissingIdentity = AgentTaskRunner::budgetExhaustionExperimentEvidence(
            experimentId, missingIdentityBinding,
            {{QStringLiteral("status"), QStringLiteral("ok")},
             {QStringLiteral("result"), missingIdentitySummary}});
        QCOMPARE(rejectedMissingIdentity.value(QStringLiteral("evidenceStatus")).toString(),
                 QStringLiteral("unavailable"));
        QCOMPARE(rejectedMissingIdentity.value(QStringLiteral("code")).toString(),
                 QStringLiteral("HOST_FACT_IDENTITY_MISMATCH"));
        QVERIFY(!rejectedMissingIdentity.contains(QStringLiteral("state")));
        QVERIFY(!rejectedMissingIdentity.contains(QStringLiteral("verificationPassed")));

        const QJsonObject failedQuery = AgentTaskRunner::budgetExhaustionExperimentEvidence(
            experimentId, sourceBinding,
            {{QStringLiteral("status"), QStringLiteral("rejected")},
             {QStringLiteral("code"), QStringLiteral("EXPERIMENT_NOT_FOUND")}});
        QCOMPARE(failedQuery.value(QStringLiteral("evidenceStatus")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(failedQuery.value(QStringLiteral("code")).toString(), QStringLiteral("EXPERIMENT_NOT_FOUND"));
        QVERIFY(!failedQuery.contains(QStringLiteral("verificationPassed")));
    }

    void budgetExhaustionEvidenceFallsBackWithinLimitForOversizedHostFacts()
    {
        QJsonArray experiments;
        experiments.append(QJsonObject{
            {QStringLiteral("experimentId"), QStringLiteral("trusted-experiment")},
            {QStringLiteral("state"), QStringLiteral("candidate_ready")},
            {QStringLiteral("verificationPassed"), true},
            {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                {QStringLiteral("repairs"), 0}, {QStringLiteral("modelTurns"), 4}}},
            {QStringLiteral("counters"), QJsonObject{{QStringLiteral("solves"), 1},
                {QStringLiteral("repairs"), 0}, {QStringLiteral("modelTurns"), 0}}}});
        const QString hugeFact(20 * 1024, QLatin1Char('x'));
        for (int index = 0; index < 32; ++index)
        {
            experiments.append(QJsonObject{
                {QStringLiteral("experimentId"), QStringLiteral("experiment-%1").arg(index)},
                {QStringLiteral("state"), hugeFact},
                {QStringLiteral("counters"), QJsonObject{{QStringLiteral("solves"), hugeFact}}},
                {QStringLiteral("evidenceStatus"), QStringLiteral("unavailable")}});
        }

        const QJsonObject evidence = AgentTaskRunner::boundedBudgetExhaustionEvidence(12, 12, experiments);
        const QByteArray encoded = QJsonDocument(evidence).toJson(QJsonDocument::Compact);
        QVERIFY2(encoded.size() <= 16 * 1024, qPrintable(QString::number(encoded.size())));
        const QJsonArray compactExperiments = evidence.value(QStringLiteral("experiments")).toArray();
        QCOMPARE(compactExperiments.size(), experiments.size());
        QCOMPARE(compactExperiments.first().toObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("candidate_ready"));
        QCOMPARE(compactExperiments.first().toObject().value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("solves")).toInt(), 1);
        QVERIFY(!compactExperiments.at(1).toObject().contains(QStringLiteral("state")));
        QVERIFY(!compactExperiments.at(1).toObject().contains(QStringLiteral("counters")));

        QJsonArray amplifiedExperiments;
        for (int index = 0; index < 512; ++index)
            amplifiedExperiments.append(QJsonObject{
                {QStringLiteral("experimentId"), QStringLiteral("experiment-%1").arg(index)},
                {QStringLiteral("evidenceStatus"), QStringLiteral("unavailable")}});
        const QJsonObject fallback = AgentTaskRunner::boundedBudgetExhaustionEvidence(12, 12, amplifiedExperiments);
        const QByteArray fallbackEncoded = QJsonDocument(fallback).toJson(QJsonDocument::Compact);
        QVERIFY2(fallbackEncoded.size() <= 16 * 1024, qPrintable(QString::number(fallbackEncoded.size())));
        QCOMPARE(fallback.value(QStringLiteral("evidenceStatus")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(fallback.value(QStringLiteral("code")).toString(),
                 QStringLiteral("HOST_FACT_SNAPSHOT_TOO_LARGE"));
        QCOMPARE(fallback.value(QStringLiteral("experimentCount")).toInt(), amplifiedExperiments.size());
        QVERIFY(!fallback.contains(QStringLiteral("experiments")));
    }

    void conversationByteLimitReportsOnlyDispatchedProviderTurns()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        ModelCommandHost *host = session->getModelCommandHost();
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);

        runner.startTask(QStringLiteral("Exercise the conversation byte ceiling."));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 1000);
        const QString experimentId = QStringLiteral("missing-budget-evidence-record");
        runner.taskExperimentIds.insert(experimentId);
        runner.taskExperimentSourceBindings.insert(experimentId, QJsonObject{
            {QStringLiteral("documentId"), QStringLiteral("source-fixture")},
            {QStringLiteral("documentRevision"), 0},
            {QStringLiteral("sessionEpoch"), QStringLiteral("session-fixture")},
            {QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('a'))}});
        runner.history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("content"), QString(1024 * 1024 + 1, QLatin1Char('x'))}});

        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("byte-limit-capability"),
                QStringLiteral("host_capabilities"), {})})));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("budget_exhausted"));
        QCOMPARE(requested.size(), 1);
        QCOMPARE(runner.turns, 1);
        const QString finishedMessage = finished.first().at(1).toString();
        QVERIFY(finishedMessage.contains(QStringLiteral("\"providerTurnRequestsIssued\":1")));
        QVERIFY(!finishedMessage.contains(QStringLiteral("\"providerTurnRequestsIssued\":2")));
        session->clear();
    }

    void experimentRunBeyondRequestedTurnBudgetIsRejectedWithoutPreview()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);
        const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
            ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        const QDir experimentDirectory(QDir(dataRoot).filePath(QStringLiteral("trusted-experiments")));
        const auto experimentEntries = [&experimentDirectory] {
            return experimentDirectory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        };

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Exhaust four turns before proposing an experiment."));
        QCOMPARE(requested.size(), 1);
        for (int providerTurn = 1; providerTurn <= 4; ++providerTurn)
        {
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("preflight-capability-%1").arg(providerTurn),
                    QStringLiteral("host_capabilities"), {})})));
            QTRY_COMPARE_WITH_TIMEOUT(requested.size(), providerTurn + 1, 1000);
        }
        QCOMPARE(requested.size(), 5);
        const QStringList recordsBeforePreviewAttempt = experimentEntries();
        const QJsonObject arguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
            {QStringLiteral("budgets"), trustedExperimentBudgets(1, 0, 4)}};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("late-experiment"),
                QStringLiteral("experiment_run"), arguments)})));

        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 6, 1000);
        QCOMPARE(finished.size(), 0);
        QVERIFY(runner.isActive());
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(experimentEntries(), recordsBeforePreviewAttempt);
        const QJsonObject toolError = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("late-experiment")).toUtf8()).object();
        QCOMPARE(toolError.value(QStringLiteral("code")).toString(),
                 QStringLiteral("EXPERIMENT_MODEL_TURN_BUDGET_TOO_SMALL"));
        runner.cancel();
        session->clear();
    }

    void declinedExperimentPreviewDoesNotLowerTaskModelTurnLimit()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        const auto stamp = session->modelCommandStamp(0);

        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Decline a low-budget experiment preview."), true);
        const QJsonObject arguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
            {QStringLiteral("budgets"), trustedExperimentBudgets(1, 0, 4)}};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("declined-four-turn-experiment"),
                QStringLiteral("experiment_run"), arguments)})));
        QCOMPARE(proposals.size(), 1);
        runner.authorizePending(proposals.last().at(1).toString(), false);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);

        for (int providerTurn = 2; providerTurn <= 5; ++providerTurn)
        {
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("after-decline-%1").arg(providerTurn),
                    QStringLiteral("host_capabilities"), {})})));
            QTRY_COMPARE_WITH_TIMEOUT(requested.size(), providerTurn + 1, 1000);
            QCOMPARE(finished.size(), 0);
        }
        runner.cancel();
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("cancelled"));
        session->clear();
    }

    void globalModelTurnCapStillAllowsTwelveProviderRequests()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Use the default global model-turn budget."));
        QCOMPARE(requested.size(), 1);
        for (int providerTurn = 1; providerTurn <= 12; ++providerTurn)
        {
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("global-capability-%1").arg(providerTurn),
                    QStringLiteral("host_capabilities"), {})})));
            if (providerTurn < 12)
                QTRY_COMPARE_WITH_TIMEOUT(requested.size(), providerTurn + 1, 1000);
            else
                QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        }
        QCOMPARE(requested.size(), 12);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("budget_exhausted"));
    }

    void humanWaitBudgetClearsOnRetirementAndDoesNotReplenishExhaustedBudget()
    {
        WorkflowProcess workflow;
        AgentTaskRunner runner(Application::instance()->getSession()->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        const QJsonObject question = providerCall(QStringLiteral("retire-question"), QStringLiteral("ask_user"),
            {{QStringLiteral("question"), QStringLiteral("Continue?")},
             {QStringLiteral("options"), QJsonArray{QStringLiteral("yes"), QStringLiteral("no")}}});
        for (const QString &status : {QStringLiteral("cancelled"), QStringLiteral("failed"), QStringLiteral("steering")})
        {
            runner.startTask(QStringLiteral("Wait for a decision."));
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({}, {question})));
            const QString oldQuestion = runner.pendingQuestionId();
            QVERIFY(runner.executionBudgetPaused);
            if (status == QStringLiteral("steering")) runner.retireTaskForSteering();
            else if (status == QStringLiteral("cancelled")) runner.cancel();
            else runner.finishTask(status, QStringLiteral("Simulated failure."));
            QVERIFY(!runner.isActive());
            QVERIFY(!runner.budgetTimer.isActive());
            QVERIFY(!runner.executionBudgetPaused);
            QCOMPARE(runner.remainingExecutionBudgetMs, 0);
            runner.answerQuestion(oldQuestion, QStringLiteral("yes"));
            QVERIFY(!runner.budgetTimer.isActive());
        }
        runner.startTask(QStringLiteral("A new task gets its own budget."));
        QCOMPARE(runner.remainingExecutionBudgetMs, 300000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({}, {question})));
        runner.remainingExecutionBudgetMs = 0;
        runner.answerQuestion(runner.pendingQuestionId(), QStringLiteral("yes"));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("budget_exhausted"));
        QVERIFY(!runner.isActive());
        QCOMPARE(runner.remainingExecutionBudgetMs, 0);
        QVERIFY(!runner.budgetTimer.isActive());
        QVERIFY(!runner.executionBudgetPaused);
        runner.startTask(QStringLiteral("Execution still has a hard time limit."));
        QVERIFY(QMetaObject::invokeMethod(&runner.budgetTimer, "timeout", Qt::DirectConnection));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("budget_exhausted"));
        QVERIFY(!runner.isActive());
        QCOMPARE(runner.remainingExecutionBudgetMs, 0);
        QVERIFY(!runner.budgetTimer.isActive());

    }

    void trustedMechanicsToolSchemasAreExactAndInvalidSpecsReturnHostDiagnosis()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        const uint modelId = session->getNModels() - 1;
        session->setModelSelected(modelId, true);
        const auto stamp = session->modelCommandStamp(modelId);

        WorkflowProcess workflow;
        AgentTaskRunner diagnose(host, &workflow);
        QSignalSpy requested(&diagnose, &AgentTaskRunner::turnRequested);
        QSignalSpy finished(&diagnose, &AgentTaskRunner::finished);
        diagnose.startTask(QStringLiteral("Diagnose this incomplete physical specification."));
        const QJsonObject diagnosisArguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("scope"), QStringLiteral("document")},
            {QStringLiteral("problemSpec"), QJsonObject{}}};
        diagnose.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("invalid-physics"), QStringLiteral("trusted_problem_diagnose"), diagnosisArguments)})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 1000);
        const QJsonObject hostResponse = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("invalid-physics")).toUtf8()).object();
        QCOMPARE(hostResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject diagnosis = hostResponse.value(QStringLiteral("result")).toObject();
        QCOMPARE(diagnosis.value(QStringLiteral("problemSpecStatus")).toString(), QStringLiteral("invalid"));
        const QJsonArray missingIssues = diagnosis.value(QStringLiteral("diagnosis")).toObject()
            .value(QStringLiteral("issues")).toArray();
        QVERIFY(!missingIssues.isEmpty());
        bool hostIdentifiedMissingMaterial = false;
        for (const QJsonValue &issueValue : missingIssues)
        {
            const QJsonObject issue = issueValue.toObject();
            const QString code = issue.value(QStringLiteral("code")).toString();
            const QString path = issue.value(QStringLiteral("path")).toString();
            if (code.startsWith(QStringLiteral("MATERIAL")) || path.startsWith(QStringLiteral("material.")))
                hostIdentifiedMissingMaterial = true;
        }
        QVERIFY(hostIdentifiedMissingMaterial);
        QCOMPARE(finished.size(), 0);
        diagnose.cancel();

        AgentTaskRunner inspect(host, &workflow);
        QSignalSpy inspectRequests(&inspect, &AgentTaskRunner::turnRequested);
        QSignalSpy inspectFinished(&inspect, &AgentTaskRunner::finished);
        inspect.startTask(QStringLiteral("Inspect this explicit detached rod problem."));
        const QJsonObject validInspectArguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("scope"), QStringLiteral("candidate")},
            {QStringLiteral("jobId"), QStringLiteral("unregistered-inspection-job")},
            {QStringLiteral("problemSpec"), trustedRodProblemSpec()}};
        inspect.receiveTurn(inspectRequests.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("valid-inspect"), QStringLiteral("trusted_problem_inspect"),
                                    validInspectArguments)})));
        QTRY_COMPARE_WITH_TIMEOUT(inspectRequests.size(), 2, 1000);
        const QJsonObject inspectHostResponse = QJsonDocument::fromJson(toolResultContent(
            inspectRequests.last().at(1).toJsonArray(), QStringLiteral("valid-inspect")).toUtf8()).object();
        QCOMPARE(inspectHostResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject inspectResult = inspectHostResponse.value(QStringLiteral("result")).toObject();
        QCOMPARE(inspectResult.value(QStringLiteral("basis")).toString(), QStringLiteral("detached"));
        QCOMPARE(inspectResult.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                     .value(QStringLiteral("documentId")).toString(), stamp.first);
        QCOMPARE(inspectResult.value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("jobId")).toString(), QStringLiteral("unregistered-inspection-job"));
        QCOMPARE(inspectResult.value(QStringLiteral("job")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("not_found"));
        QCOMPARE(inspectResult.value(QStringLiteral("resultSource")).toObject()
                     .value(QStringLiteral("status")).toString(), QStringLiteral("not_found"));
        QVERIFY(inspectResult.value(QStringLiteral("mesh")).toObject().value(QStringLiteral("nodeCount")).toInt() > 0);
        QCOMPARE(inspectResult.value(QStringLiteral("physicalSummaryDigest")).toString().size(), 64);
        inspect.receiveTurn(inspectRequests.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The detached rod candidate was inspected through Host."))));
        QCOMPARE(inspectFinished.size(), 1);
        QCOMPARE(inspectFinished.first().at(0).toString(), QStringLiteral("completed"));

        const QList<QPair<QString, QJsonObject>> invalidCalls{
            {QStringLiteral("trusted_problem_inspect"), {{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)}, {QStringLiteral("scope"), QStringLiteral("document")},
                {QStringLiteral("problemSpec"), trustedRodProblemSpec()}, {QStringLiteral("extra"), true}}},
            {QStringLiteral("trusted_problem_inspect"), {{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)}, {QStringLiteral("scope"), QStringLiteral("document")},
                {QStringLiteral("problemSpec"), trustedRodProblemSpec()}, {QStringLiteral("jobId"), QString()}}},
            {QStringLiteral("trusted_problem_diagnose"), {{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)}, {QStringLiteral("scope"), QStringLiteral("guess")},
                {QStringLiteral("problemSpec"), trustedRodProblemSpec()}}},
            {QStringLiteral("experiment_query"), {{QStringLiteral("experimentId"), QStringLiteral("owned-later")},
                {QStringLiteral("includeLogs"), true}}},
            {QStringLiteral("experiment_wait"), {{QStringLiteral("experimentId"), QStringLiteral("owned-later")},
                {QStringLiteral("timeoutMs"), 10}}},
            {QStringLiteral("experiment_cancel"), {{QStringLiteral("experimentId"), QStringLiteral("owned-later")},
                {QStringLiteral("force"), true}}},
            {QStringLiteral("document_propose"), {{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)},
                {QStringLiteral("command"), QStringLiteral("experiment.run")},
                {QStringLiteral("arguments"), QJsonObject{
                    {QStringLiteral("problemSpec"), trustedRodProblemSpec()}}}}},
            {QStringLiteral("experiment_run"), {{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)},
                {QStringLiteral("problemSpec"), trustedRodProblemSpec()}}},
            {QStringLiteral("experiment_run"), {{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)},
                {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
                {QStringLiteral("budgets"), trustedExperimentBudgets(9, 0, 1)}}}};
        for (const auto &entry : invalidCalls)
        {
            AgentTaskRunner malformed(host, &workflow);
            QSignalSpy malformedRequests(&malformed, &AgentTaskRunner::turnRequested);
            QSignalSpy malformedFinished(&malformed, &AgentTaskRunner::finished);
            malformed.startTask(QStringLiteral("Reject malformed trusted tool arguments."));
            const auto call = entry.first == QStringLiteral("document_propose")
                ? rawProviderCall(QStringLiteral("invalid-schema"), entry.first, entry.second)
                : providerCall(QStringLiteral("invalid-schema"), entry.first, entry.second);
            malformed.receiveTurn(malformedRequests.first().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{call})));
            QCOMPARE(malformedFinished.size(), 1);
            QCOMPARE(malformedFinished.first().at(0).toString(), QStringLiteral("failed"));
            malformed.cancel();
        }
        session->clear();
    }

    void boundedExperimentAuthorizationIsExactAndSingleUse()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);
        const QJsonObject target{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("sessionEpoch"), host->sessionEpoch()}};
        const QJsonObject input{{QStringLiteral("target"), target},
            {QStringLiteral("expectedDocumentRevision"), double(stamp.second)},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), trustedRodProblemSpec()},
                {QStringLiteral("budgets"), trustedExperimentBudgets()}}}};
        WorkflowProcess workflow;
        {
            AgentTaskRunner stale(host, &workflow);
            QJsonObject staleInput = input;
            QJsonObject staleTarget = target;
            staleTarget.insert(QStringLiteral("sessionEpoch"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            staleInput.insert(QStringLiteral("target"), staleTarget);
            QVERIFY(stale.prepareBoundedExperimentAuthorization(staleInput).value(QStringLiteral("status"))
                != QJsonValue(QStringLiteral("preview")));
            QVERIFY(!stale.armBoundedExperimentAuthorization(QStringLiteral("not-authorized")));
        }

        // Changed physics and budget each require a fresh explicit proposal decision.
        for (int mismatch = 0; mismatch < 3; ++mismatch)
        {
            AgentTaskRunner runner(host, &workflow);
            const QJsonObject prepared = runner.prepareBoundedExperimentAuthorization(input);
            QCOMPARE(prepared.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
            QVERIFY(!runner.armBoundedExperimentAuthorization(QStringLiteral("wrong-preview")));
            QVERIFY(runner.armBoundedExperimentAuthorization(prepared.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("previewId")).toString()));
            QSignalSpy requests(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
            QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
            if (mismatch == 2) session->setModelSelected(0, false);
            runner.startTask(QStringLiteral("Run the bounded authorized experiment."), false);
            QJsonObject changedPhysics = trustedRodProblemSpec();
            QJsonObject material = changedPhysics.value(QStringLiteral("material")).toObject();
            material.insert(QStringLiteral("youngModulus"), QJsonObject{{QStringLiteral("value"), 100e9},
                {QStringLiteral("unit"), QStringLiteral("Pa")}});
            changedPhysics.insert(QStringLiteral("material"), material);
            QJsonObject arguments{{QStringLiteral("documentId"), stamp.first},
                {QStringLiteral("documentRevision"), double(stamp.second)},
                {QStringLiteral("problemSpec"), mismatch == 0 ? changedPhysics : trustedRodProblemSpec()},
                {QStringLiteral("budgets"), mismatch == 1 ? trustedExperimentBudgets(2, 0, 12) : trustedExperimentBudgets()}};
            runner.receiveTurn(requests.first().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("mismatch"), QStringLiteral("experiment_run"), arguments)})));
            QCOMPARE(proposals.size(), 1);
            QCOMPARE(actions.size(), 0);
            runner.cancel();
            if (mismatch == 2) session->setModelSelected(0, true);
        }

        AgentTaskRunner exact(host, &workflow);
        QJsonObject recorded;
        exact.setExecutionRecorder([&recorded](const QString &, const QString &, const QJsonObject &reference) {
            recorded = reference; return true;
        });
        const auto prepared = exact.prepareBoundedExperimentAuthorization(input);
        QCOMPARE(prepared.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
        QVERIFY(exact.armBoundedExperimentAuthorization(prepared.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("previewId")).toString()));
        QSignalSpy requests(&exact, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&exact, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&exact, &AgentTaskRunner::actionCompleted);
        exact.startTask(QStringLiteral("Run the bounded authorized experiment."), false);
        const QJsonObject arguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
            {QStringLiteral("budgets"), trustedExperimentBudgets()}};
        exact.receiveTurn(requests.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("exact"), QStringLiteral("experiment_run"), arguments)})));
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(actions.size(), 1);
        QCOMPARE(recorded.value(QStringLiteral("authorizationMode")).toString(), QStringLiteral("bounded_task_authorization"));
        QCOMPARE(recorded.value(QStringLiteral("trustedAuthorizationPreviewId")),
                 prepared.value(QStringLiteral("result")).toObject().value(QStringLiteral("previewId")));
        QCOMPARE(recorded.value(QStringLiteral("trustedAuthorizationExperimentId")),
                 prepared.value(QStringLiteral("result")).toObject().value(QStringLiteral("experimentId")));
        QCOMPARE(recorded.value(QStringLiteral("trustedAuthorizationDigest")),
                 prepared.value(QStringLiteral("result")).toObject().value(QStringLiteral("authorizationDigest")));
        QVERIFY(exact.taskExperimentAuthorization.isEmpty());
        exact.cancel();
        session->clear();
    }

    void experimentRunNeedsManualApprovalAndWaitReturnsNeedsConfirmationFacts()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto sourceStamp = session->modelCommandStamp(0);
        const QString sourceHash = RModelCommandService::stateFingerprint(session->getModel(0));

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy observed(&runner, &AgentTaskRunner::experimentObserved);
        QSignalSpy activities(&runner, &AgentTaskRunner::toolActivity);
        runner.startTask(QStringLiteral("Run and report a trusted rod experiment."), true);
        const QJsonObject proposal{{QStringLiteral("documentId"), sourceStamp.first},
            {QStringLiteral("documentRevision"), double(sourceStamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), trustedRodProblemSpec()},
                {QStringLiteral("budgets"), trustedExperimentBudgets()}}}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("experiment-preview"), QStringLiteral("document_propose"), proposal)})));
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
        QCOMPARE(actions.size(), 0); // Auto mode cannot cross the manual experiment approval fence.
        const QJsonObject approvedPreview = proposals.first().at(2).toJsonObject();
        const QString experimentId = approvedPreview.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!experimentId.isEmpty());

        runner.authorizePending(proposals.first().at(1).toString(), true);
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.first().at(0).toString(), QStringLiteral("experiment"));
        QCOMPARE(actions.first().at(1).toJsonObject().value(QStringLiteral("experimentId")).toString(), experimentId);

        const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
            ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
        RTrustedExperimentService experiments(QDir(dataRoot).filePath(QStringLiteral("trusted-experiments")));
        RTrustedExperimentService::SourceIdentity source;
        source.workspaceId = host->projectWorkspace();
        source.sessionId = session->getID();
        source.sessionEpoch = host->sessionEpoch();
        source.documentId = sourceStamp.first;
        source.documentRevision = QString::number(sourceStamp.second);
        source.snapshotSha256 = sourceHash;
        const auto confirmation = experiments.markNeedsConfirmation(experimentId, source,
            QStringLiteral("The test requires explicit confirmation."));
        QVERIFY2(confirmation.ok, qPrintable(confirmation.message));

        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 3000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("wait-confirmation"), QStringLiteral("experiment_wait"),
                {{QStringLiteral("experimentId"), experimentId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 3000);
        const QJsonObject waitResponse = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("wait-confirmation")).toUtf8()).object();
        QCOMPARE(waitResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(waitResponse.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("needs_confirmation"));
        QVERIFY(!observed.isEmpty());
        QCOMPARE(observed.last().at(1).toString(), experimentId);
        QCOMPARE(observed.last().at(2).toJsonObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("needs_confirmation"));
        const QJsonObject waitFacts = activities.last().last().toJsonObject();
        QVERIFY(waitFacts.contains(QStringLiteral("experimentId")));
        QVERIFY(waitFacts.contains(QStringLiteral("state")));
        QVERIFY(!waitFacts.contains(QStringLiteral("verificationPassed")));
        QVERIFY(!waitFacts.contains(QStringLiteral("verification")));

        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("cancel-owned"), QStringLiteral("experiment_cancel"),
                {{QStringLiteral("experimentId"), experimentId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 4, 3000);
        const QJsonObject cancelResponse = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("cancel-owned")).toUtf8()).object();
        QCOMPARE(cancelResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(cancelResponse.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(observed.last().at(2).toJsonObject().value(QStringLiteral("state")).toString(),
                 QStringLiteral("cancelled"));
        runner.cancel();

        AgentTaskRunner nextTask(host, &workflow);
        QSignalSpy nextRequests(&nextTask, &AgentTaskRunner::turnRequested);
        nextTask.startTask(QStringLiteral("Try to read an experiment from a prior task."));
        nextTask.receiveTurn(nextRequests.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("foreign-experiment"), QStringLiteral("experiment_query"),
                {{QStringLiteral("experimentId"), experimentId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(nextRequests.size(), 2, 1000);
        QVERIFY(toolResultContent(nextRequests.last().at(1).toJsonArray(),
                                  QStringLiteral("foreign-experiment")).contains(QStringLiteral("EXPERIMENT_NOT_TASK_OWNED")));
        nextTask.cancel();
        session->clear();
    }

    void physicalClarificationUsesHostDiagnosisAndCorrelatedAnswer()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        const uint modelId = session->getNModels() - 1;
        session->setModelSelected(modelId, true);
        const auto stamp = session->modelCommandStamp(modelId);
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy questions(&runner, &AgentTaskRunner::userInputRequested);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Diagnose the incomplete beam specification and ask before choosing its material."));

        const QJsonObject incomplete{{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("modelType"), QStringLiteral("cantileverBeam")},
            {QStringLiteral("name"), QStringLiteral("Beam awaiting material choice")}};
        const QJsonObject diagnosisArgs{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("scope"), QStringLiteral("candidate")},
            {QStringLiteral("problemSpec"), incomplete}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("missing-material-diagnose"),
                QStringLiteral("trusted_problem_diagnose"), diagnosisArgs)})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 3000);
        const QJsonObject diagnosisResponse = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("missing-material-diagnose")).toUtf8()).object();
        QCOMPARE(diagnosisResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject diagnosis = diagnosisResponse.value(QStringLiteral("result")).toObject();
        QCOMPARE(diagnosis.value(QStringLiteral("problemSpecStatus")).toString(), QStringLiteral("invalid"));
        const QJsonArray missingIssues = diagnosis.value(QStringLiteral("diagnosis")).toObject()
            .value(QStringLiteral("issues")).toArray();
        QVERIFY(!missingIssues.isEmpty());
        bool hostIdentifiedMissingMaterial = false;
        for (const QJsonValue &issueValue : missingIssues)
        {
            const QJsonObject issue = issueValue.toObject();
            if (issue.value(QStringLiteral("code")).toString().startsWith(QStringLiteral("MATERIAL"))
                || issue.value(QStringLiteral("path")).toString().startsWith(QStringLiteral("material.")))
                hostIdentifiedMissingMaterial = true;
        }
        QVERIFY(hostIdentifiedMissingMaterial);

        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(
            QStringLiteral("The Host reports missing physical inputs. Provide a named material record with E in Pa, Poisson ratio, density in kg/m3, and its source."),
            QJsonArray{providerCall(QStringLiteral("material-question"), QStringLiteral("ask_user"),
                {{QStringLiteral("question"), QStringLiteral("Which complete material record should be used for this beam?")},
                 {QStringLiteral("options"), QJsonArray{
                     QStringLiteral("Steel fixture: E=210000000000 Pa; nu=0.30; density=7850 kg/m3; source=test fixture"),
                     QStringLiteral("Aluminum fixture: E=70000000000 Pa; nu=0.33; density=2700 kg/m3; source=test fixture")}}})})));
        QCOMPARE(questions.size(), 1);
        QVERIFY(runner.isAwaitingInput());
        const QString questionId = runner.pendingQuestionId();
        QVERIFY(!questionId.isEmpty());
        runner.answerQuestion(QStringLiteral("stale-material-question"), QStringLiteral("Steel"));
        QVERIFY(runner.isAwaitingInput());
        const QString aluminumRecord = QStringLiteral(
            "Aluminum fixture: E=70000000000 Pa; nu=0.33; density=2700 kg/m3; source=test fixture");
        runner.answerQuestion(questionId, aluminumRecord);
        QVERIFY(!runner.isAwaitingInput());
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 3000);
        const QString answerEvidence = toolResultContent(requested.last().at(1).toJsonArray(),
                                                         QStringLiteral("material-question"));
        QVERIFY(answerEvidence.contains(aluminumRecord));
        QVERIFY(answerEvidence.contains(QStringLiteral("answered")));

        QJsonObject chosenMaterial = trustedBeamProblemSpec();
        QJsonObject material = chosenMaterial.value(QStringLiteral("material")).toObject();
        material.insert(QStringLiteral("name"), QStringLiteral("Aluminum fixture"));
        material.insert(QStringLiteral("source"), QStringLiteral("test fixture; user-confirmed"));
        material.insert(QStringLiteral("youngModulus"), QJsonObject{
            {QStringLiteral("value"), 70.0e9}, {QStringLiteral("unit"), QStringLiteral("Pa")}});
        material.insert(QStringLiteral("poissonRatio"), 0.33);
        material.insert(QStringLiteral("density"), QJsonObject{
            {QStringLiteral("value"), 2700.0}, {QStringLiteral("unit"), QStringLiteral("kg/m3")}});
        chosenMaterial.insert(QStringLiteral("material"), material);
        const QJsonObject inspectArgs{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("scope"), QStringLiteral("candidate")},
            {QStringLiteral("problemSpec"), chosenMaterial}};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("inspect-chosen-material"),
                QStringLiteral("trusted_problem_inspect"), inspectArgs)})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 4, 3000);
        const QJsonObject inspectResponse = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("inspect-chosen-material")).toUtf8()).object();
        QCOMPARE(inspectResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QJsonObject actualMaterial = inspectResponse.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("physicalSummary")).toObject()
            .value(QStringLiteral("material")).toObject();
        QCOMPARE(actualMaterial.value(QStringLiteral("youngModulus")).toDouble(), 70.0e9);
        QCOMPARE(actualMaterial.value(QStringLiteral("poissonRatio")).toDouble(), 0.33);
        QCOMPARE(actualMaterial.value(QStringLiteral("density")).toDouble(), 2700.0);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("diagnose-chosen-material"),
                QStringLiteral("trusted_problem_diagnose"), inspectArgs)})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 5, 3000);
        const QJsonObject chosenDiagnosis = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("diagnose-chosen-material")).toUtf8()).object()
            .value(QStringLiteral("result")).toObject().value(QStringLiteral("diagnosis")).toObject();
        QVERIFY(chosenDiagnosis.value(QStringLiteral("issues")).isArray());
        for (const QJsonValue &issueValue : chosenDiagnosis.value(QStringLiteral("issues")).toArray())
        {
            const QJsonObject issue = issueValue.toObject();
            const QString code = issue.value(QStringLiteral("code")).toString();
            const QString severity = issue.value(QStringLiteral("severity")).toString();
            QVERIFY(!(code.startsWith(QStringLiteral("MATERIAL")) && severity == QStringLiteral("error")));
        }
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The chosen material was read back from the Host candidate."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        session->clear();
    }

    void trustedRodBeamAndAuthorizedNumericalRepairRunThroughAgentHost()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        if (qEnvironmentVariableIsSet("RANGE_TEST_PRESERVE_RUNTIME"))
            workspace.setAutoRemove(false);
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto sourceStamp = session->modelCommandStamp(0);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(0));
        WorkflowProcess workflow;

        struct AgentExperimentCase
        {
            QString label;
            QJsonObject problem;
            QJsonArray allowedRanges;
            QJsonObject budgets;
            int minimumRepairs = 0;
        };
        QJsonObject repairProblem = trustedRodProblemSpec(1);
        repairProblem.insert(QStringLiteral("analysis"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("modal")}, {QStringLiteral("modes"), 1}});
        repairProblem.insert(QStringLiteral("loads"), QJsonArray{});
        repairProblem.insert(QStringLiteral("observables"), QJsonArray{QJsonObject{
            {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
            {QStringLiteral("quantity"), QStringLiteral("frequency")},
            {QStringLiteral("component"), QStringLiteral("mode")}}});
        repairProblem.insert(QStringLiteral("precision"), QJsonObject{
            {QStringLiteral("frequencyRelative"), 0.02}});
        const QJsonArray repairRanges{QJsonObject{{QStringLiteral("path"), QStringLiteral("/geometry/segments")},
            {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 4}}};
        const QJsonObject repairBudgets{{QStringLiteral("solves"), 4},
            {QStringLiteral("repairs"), 3}, {QStringLiteral("modelTurns"), 4}};
        const QList<AgentExperimentCase> cases{
            {QStringLiteral("rod"), trustedRodProblemSpec(), {}, trustedExperimentBudgets(), 0},
            {QStringLiteral("beam"), trustedBeamProblemSpec(), {}, trustedExperimentBudgets(), 0},
            {QStringLiteral("rod-modal-authorized-repair"), repairProblem, repairRanges, repairBudgets, 1}};
        QJsonArray evidenceRecords;

        for (const AgentExperimentCase &testCase : cases)
        {
            const bool compactTurnBudget = testCase.budgets.value(QStringLiteral("modelTurns")).toInt() == 4;
            AgentTaskRunner runner(host, &workflow);
            QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
            QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
            QSignalSpy observed(&runner, &AgentTaskRunner::experimentObserved);
            QSignalSpy finished(&runner, &AgentTaskRunner::finished);
            runner.startTask(QStringLiteral("Inspect, diagnose, run, verify and report the trusted %1 problem.")
                                 .arg(testCase.label), true);

            const QJsonObject inspectArgs{{QStringLiteral("documentId"), sourceStamp.first},
                {QStringLiteral("documentRevision"), double(sourceStamp.second)},
                {QStringLiteral("scope"), QStringLiteral("candidate")},
                {QStringLiteral("problemSpec"), testCase.problem}};
            QJsonArray preflightCalls{providerCall(QStringLiteral("%1-inspect").arg(testCase.label),
                QStringLiteral("trusted_problem_inspect"), inspectArgs)};
            if (compactTurnBudget)
                preflightCalls.append(providerCall(QStringLiteral("%1-diagnose").arg(testCase.label),
                    QStringLiteral("trusted_problem_diagnose"), inspectArgs));
            runner.receiveTurn(requested.last().at(0).toString(),
                parseProviderBody(providerBody({}, preflightCalls)));
            QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 3000);
            const QJsonObject inspectResult = QJsonDocument::fromJson(toolResultContent(
                requested.last().at(1).toJsonArray(), QStringLiteral("%1-inspect").arg(testCase.label)).toUtf8()).object();
            QCOMPARE(inspectResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(inspectResult.value(QStringLiteral("result")).toObject()
                         .value(QStringLiteral("basis")).toString(), QStringLiteral("detached"));

            if (!compactTurnBudget)
            {
                runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                    QJsonArray{providerCall(QStringLiteral("%1-diagnose").arg(testCase.label),
                        QStringLiteral("trusted_problem_diagnose"), inspectArgs)})));
                QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 3000);
            }
            const QJsonObject diagnosisResult = QJsonDocument::fromJson(toolResultContent(
                requested.last().at(1).toJsonArray(), QStringLiteral("%1-diagnose").arg(testCase.label)).toUtf8()).object();
            QCOMPARE(diagnosisResult.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));

            QJsonObject experimentArguments{{QStringLiteral("problemSpec"), testCase.problem}};
            if (!testCase.allowedRanges.isEmpty())
                experimentArguments.insert(QStringLiteral("allowedNumericalRanges"), testCase.allowedRanges);
            if (!testCase.budgets.isEmpty()) experimentArguments.insert(QStringLiteral("budgets"), testCase.budgets);
            const QJsonObject proposal{{QStringLiteral("documentId"), sourceStamp.first},
                {QStringLiteral("documentRevision"), double(sourceStamp.second)},
                {QStringLiteral("command"), QStringLiteral("experiment.run")},
                {QStringLiteral("arguments"), experimentArguments}};
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("%1-preview").arg(testCase.label),
                    QStringLiteral("document_propose"), proposal)})));
            QCOMPARE(proposals.size(), 1);
            QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
            QCOMPARE(actions.size(), 0);
            const QString experimentId = proposals.first().at(2).toJsonObject()
                .value(QStringLiteral("experimentId")).toString();
            QVERIFY(!experimentId.isEmpty());
            QCOMPARE(proposals.first().at(2).toJsonObject().value(QStringLiteral("arguments")).toObject()
                         .value(QStringLiteral("budgets")).toObject(), testCase.budgets);
            const QString dataRoot = Application::instance()->getApplicationSettings()->getDataDir().isEmpty()
                ? QDir::tempPath() : Application::instance()->getApplicationSettings()->getDataDir();
            RTrustedExperimentService experiments(QDir(dataRoot).filePath(QStringLiteral("trusted-experiments")));
            // Preview is side-effect free. The Host commits this exact authorization
            // only when the user approves the pending preview.
            QVERIFY(!experiments.query(experimentId).ok);
            runner.authorizePending(proposals.first().at(1).toString(), true);
            QCOMPARE(actions.size(), 1);
            QCOMPARE(actions.first().at(1).toJsonObject()
                         .value(QStringLiteral("experimentId")).toString(), experimentId);
            const auto prepared = experiments.query(experimentId);
            QVERIFY2(prepared.ok, qPrintable(prepared.message));
            const QJsonObject authorization = prepared.data.value(QStringLiteral("experiment")).toObject()
                .value(QStringLiteral("authorization")).toObject();
            QCOMPARE(authorization.value(QStringLiteral("experimentPolicy")).toObject()
                         .value(QStringLiteral("budgets")).toObject(), testCase.budgets);

            const int waitTurnNumber = compactTurnBudget ? 3 : 4;
            QTRY_COMPARE_WITH_TIMEOUT(requested.size(), waitTurnNumber, 5000);
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("%1-wait").arg(testCase.label),
                    QStringLiteral("experiment_wait"), {{QStringLiteral("experimentId"), experimentId}})})));
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                for (const auto &observation : observed)
                {
                    if (observation.at(1).toString() != experimentId) continue;
                    const QString state = observation.at(2).toJsonObject().value(QStringLiteral("state")).toString();
                    if (state == QStringLiteral("candidate_ready") || state == QStringLiteral("failed")
                        || state == QStringLiteral("budget_exhausted") || state == QStringLiteral("not_converged"))
                        return true;
                }
                return false;
            }(), 120000);
            QTRY_COMPARE_WITH_TIMEOUT(requested.size(), waitTurnNumber + 1, 5000);
            const QJsonObject waitResponse = QJsonDocument::fromJson(toolResultContent(
                requested.last().at(1).toJsonArray(), QStringLiteral("%1-wait").arg(testCase.label)).toUtf8()).object();
            QCOMPARE(waitResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(waitResponse.value(QStringLiteral("result")).toObject()
                         .value(QStringLiteral("budgets")).toObject(), testCase.budgets);
            const QJsonObject query = host->handle(hostRequest(QStringLiteral("experiment.query"),
                {{QStringLiteral("experimentId"), experimentId}}));
            QCOMPARE(query.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            const QJsonObject hostSummary = query.value(QStringLiteral("result")).toObject();
            QCOMPARE(hostSummary.value(QStringLiteral("state")).toString(), QStringLiteral("candidate_ready"));
            QCOMPARE(hostSummary.value(QStringLiteral("budgets")).toObject(), testCase.budgets);
            QVERIFY(hostSummary.value(QStringLiteral("verificationPassed")).toBool());
            QCOMPARE(hostSummary.value(QStringLiteral("verification")).toObject()
                         .value(QStringLiteral("passed")).toBool(), true);
            const QJsonObject evidenceSnapshot = host->evaluationEvidenceSnapshot(experimentId);
            QCOMPARE(evidenceSnapshot.value(QStringLiteral("schema")).toString(),
                     QStringLiteral("range.trusted-experiment-evidence-snapshot/v1"));
            QCOMPARE(evidenceSnapshot.value(QStringLiteral("experimentId")).toString(), experimentId);
            QCOMPARE(evidenceSnapshot.value(QStringLiteral("childJob")).toObject()
                         .value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
            QCOMPARE(evidenceSnapshot.value(QStringLiteral("verificationReport")).toObject()
                         .value(QStringLiteral("status")).toString(), QStringLiteral("pass"));
            evidenceRecords.append(QJsonObject{{QStringLiteral("case"), testCase.label},
                                               {QStringLiteral("evidence"), evidenceSnapshot}});
            const auto completedRecord = experiments.query(experimentId);
            QVERIFY2(completedRecord.ok, qPrintable(completedRecord.message));
            QCOMPARE(completedRecord.data.value(QStringLiteral("experiment")).toObject()
                         .value(QStringLiteral("authorization")).toObject()
                         .value(QStringLiteral("experimentPolicy")).toObject()
                         .value(QStringLiteral("budgets")).toObject(), testCase.budgets);
            if (testCase.minimumRepairs > 0)
            {
                const QJsonObject counters = hostSummary.value(QStringLiteral("counters")).toObject();
                QVERIFY(counters.value(QStringLiteral("repairs")).toInt() >= testCase.minimumRepairs);
                QCOMPARE(counters.value(QStringLiteral("solves")).toInt(), 3);
                QVERIFY(hostSummary.value(QStringLiteral("repairs")).toArray().size() >= 2);
            }
            runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(
                QStringLiteral("The trusted Host evidence is complete; only its verifier fields determine the result."))));
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
            QCOMPARE(session->modelCommandStamp(0), sourceStamp);
            QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)), sourceFingerprint);
        }
        const QString evidencePath = qEnvironmentVariable("RANGE_TEST_EVIDENCE_EXPORT");
        if (!evidencePath.isEmpty())
        {
            const QJsonObject exported{{QStringLiteral("schema"), QStringLiteral("range.stage8.trusted-flow-evidence/v1")},
                {QStringLiteral("cases"), evidenceRecords}};
            const QByteArray bytes = QJsonDocument(exported).toJson(QJsonDocument::Indented);
            QSaveFile file(evidencePath);
            QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
            QCOMPARE(file.write(bytes), qint64(bytes.size()));
            QVERIFY2(file.commit(), qPrintable(file.errorString()));
            const QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
            qInfo().noquote() << "Stage 8 trusted flow evidence exported:" << evidencePath
                              << "cases=" << evidenceRecords.size()
                              << "sha256=" << QString::fromLatin1(digest);
        }
        session->clear();
    }

    void steeringRetiresExperimentWaitTimerBeforeItCanAdvanceAnotherTask()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Submit a bounded rod experiment."), true);
        const QJsonObject proposal{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), trustedRodProblemSpec()},
                {QStringLiteral("budgets"), trustedExperimentBudgets()}}}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("timer-preview"), QStringLiteral("document_propose"), proposal)})));
        QCOMPARE(proposals.size(), 1);
        const QString experimentId = proposals.first().at(2).toJsonObject()
            .value(QStringLiteral("experimentId")).toString();
        runner.authorizePending(proposals.first().at(1).toString(), true);
        QCOMPARE(actions.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 3000);

        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("old-wait"), QStringLiteral("experiment_wait"),
                {{QStringLiteral("experimentId"), experimentId}})})));
        // The first Host read schedules a poll timer. Steering starts a new
        // generation before that timer can call finishTool or request a turn.
        runner.submitMessage(QStringLiteral("Stop waiting and answer this new request."));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 1000);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));

        const QJsonObject cancelled = host->handle(hostRequest(QStringLiteral("experiment.cancel"),
            {{QStringLiteral("experimentId"), experimentId}}));
        QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QTest::qWait(650);
        QCOMPARE(requested.size(), 3); // No stale timer may advance the replacement task.
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The replacement request is complete."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        session->clear();
    }

    void stoppingTaskCancelsOnlyItsAcceptedExperimentAndReportsBoundSource()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy cancellationFacts(&runner, &AgentTaskRunner::experimentCancellationObserved);
        QSignalSpy states(&runner, &AgentTaskRunner::stateChanged);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        QSignalSpy taskStarted(&runner, &AgentTaskRunner::taskStarted);
        QSignalSpy pendingMessagesCancelled(&runner, &AgentTaskRunner::pendingMessagesCancelled);
        runner.startTask(QStringLiteral("Run one trusted rod experiment."), true);
        const QJsonObject proposal{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), trustedRodProblemSpec(10000)},
                {QStringLiteral("budgets"), trustedExperimentBudgets()}}}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("stop-preview"), QStringLiteral("document_propose"), proposal)})));
        QCOMPARE(proposals.size(), 1);
        const QJsonObject preview = proposals.first().at(2).toJsonObject();
        const QString experimentId = preview.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!experimentId.isEmpty());

        runner.authorizePending(proposals.first().at(1).toString(), true);
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.first().at(1).toJsonObject().value(QStringLiteral("experimentId")).toString(), experimentId);

        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 5000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("stop-wait"), QStringLiteral("experiment_wait"),
                {{QStringLiteral("experimentId"), experimentId}})})));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto &state : states)
                if (state.at(0).toString() == QStringLiteral("waiting")
                    && state.at(1).toString().startsWith(QStringLiteral("Trusted experiment: ")))
                    return true;
            return false;
        }(), 5000);
        const QJsonObject activeSummary = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}})).value(QStringLiteral("result")).toObject();
        QCOMPARE(activeSummary.value(QStringLiteral("state")).toString(), QStringLiteral("solving"));
        const QJsonArray childJobs = activeSummary.value(QStringLiteral("childJobs")).toArray();
        QVERIFY(!childJobs.isEmpty());
        const QString childJobId = childJobs.first().toObject().value(QStringLiteral("jobId")).toString();
        QVERIFY(!childJobId.isEmpty());
        SolverManager &solverManager = SolverManager::getInstance();
        const QString childStateBeforeCancel = solverManager.jobQuery(childJobId).value(QStringLiteral("state")).toString();
        QVERIFY(childStateBeforeCancel == QStringLiteral("queued")
            || childStateBeforeCancel == QStringLiteral("running")
            || childStateBeforeCancel == QStringLiteral("cancelling"));
        const QString retiredTaskId = runner.taskId();
        bool reenteredDuringRetirement = false;
        QTimer::singleShot(0, &runner, [&runner, &reenteredDuringRetirement] {
            reenteredDuringRetirement = runner.isActive();
            runner.submitMessage(QStringLiteral("First queued follow-up after cancellation evidence."), false);
            runner.submitMessage(QStringLiteral("Second queued follow-up must also be retained."), false);
        });
        runner.cancel();

        QVERIFY(reenteredDuringRetirement);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("cancelled"));
        QCOMPARE(cancellationFacts.size(), 1);
        QCOMPARE(cancellationFacts.first().at(0).toString(), retiredTaskId);
        QCOMPARE(cancellationFacts.first().at(1).toString(), experimentId);
        const QJsonObject sourceBinding = cancellationFacts.first().at(2).toJsonObject();
        QCOMPARE(sourceBinding.value(QStringLiteral("documentId")).toString(), stamp.first);
        QCOMPARE(sourceBinding.value(QStringLiteral("sessionEpoch")).toString(), host->sessionEpoch());
        QCOMPARE(sourceBinding.value(QStringLiteral("documentRevision")).toDouble(), double(stamp.second));
        QCOMPARE(sourceBinding.value(QStringLiteral("authorizationDigest")), preview.value(QStringLiteral("authorizationDigest")));
        QCOMPARE(sourceBinding.value(QStringLiteral("jobId")).toString(), childJobId);
        QVERIFY(!sourceBinding.value(QStringLiteral("operationId")).toString().isEmpty());
        const QJsonObject fact = cancellationFacts.first().at(3).toJsonObject();
        QCOMPARE(fact.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.agent-experiment-cancellation/v1"));
        QCOMPARE(fact.value(QStringLiteral("trigger")).toString(), QStringLiteral("user_stop"));
        const QString childDisposition = fact.value(QStringLiteral("status")).toString();
        QVERIFY(childDisposition == QStringLiteral("cancelled")
            || childDisposition == QStringLiteral("terminal"));
        if (childDisposition == QStringLiteral("terminal"))
            QCOMPARE(fact.value(QStringLiteral("code")).toString(),
                     QStringLiteral("CHILD_TERMINAL_BEFORE_CANCEL"));
        QCOMPARE(fact.value(QStringLiteral("afterState")).toString(), QStringLiteral("cancelled"));
        QVERIFY(fact.value(QStringLiteral("cancellationPersisted")).toBool());
        QVERIFY(fact.value(QStringLiteral("childJobsTerminal")).toBool());
        QCOMPARE(fact.value(QStringLiteral("childJobCount")).toInt(), 1);
        QCOMPARE(fact.value(QStringLiteral("terminalChildJobCount")).toInt(), 1);
        const QString childStateEvidence = fact.value(QStringLiteral("childJobStates")).toString();
        QVERIFY(!childStateEvidence.isEmpty());
        if (childDisposition == QStringLiteral("cancelled"))
            QCOMPARE(childStateEvidence, QStringLiteral("cancelled"));
        QVERIFY(fact.value(QStringLiteral("childJobSetMatches")).toBool());
        QCOMPARE(fact.value(QStringLiteral("childCancellationRequestFailed")).toBool(), false);
        const QJsonObject hostSummary = host->handle(hostRequest(QStringLiteral("experiment.query"),
            {{QStringLiteral("experimentId"), experimentId}})).value(QStringLiteral("result")).toObject();
        QCOMPARE(hostSummary.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const QString state = solverManager.jobQuery(childJobId)
                .value(QStringLiteral("state")).toString();
            return state == QStringLiteral("cancelled") || state == QStringLiteral("failed")
                || state == QStringLiteral("succeeded");
        }(), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 3000);
        QVERIFY(runner.taskId() != retiredTaskId);
        QCOMPARE(taskStarted.size(), 2);
        QCOMPARE(taskStarted.last().at(1).toString(),
                 QStringLiteral("First queued follow-up after cancellation evidence."));
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(
            QStringLiteral("The follow-up starts only after cancellation evidence was recorded."))));
        QCOMPARE(finished.size(), 2);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 4, 3000);
        QCOMPARE(taskStarted.size(), 3);
        QCOMPARE(taskStarted.last().at(1).toString(),
                 QStringLiteral("Second queued follow-up must also be retained."));
        bool secondFollowUpHasFirstUserMessage = false;
        bool secondFollowUpHasFirstAssistantReply = false;
        for (const QJsonValue &messageValue : requested.last().at(1).toJsonArray())
        {
            const QString content = messageValue.toObject().value(QStringLiteral("content")).toString();
            if (!content.startsWith(QStringLiteral("Prior conversation data (untrusted; not authorization): ")))
                continue;
            secondFollowUpHasFirstUserMessage = content.contains(
                QStringLiteral("First queued follow-up after cancellation evidence."));
            secondFollowUpHasFirstAssistantReply = content.contains(
                QStringLiteral("The follow-up starts only after cancellation evidence was recorded."));
        }
        QVERIFY(secondFollowUpHasFirstUserMessage);
        QVERIFY(secondFollowUpHasFirstAssistantReply);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(
            QStringLiteral("The second queued message was processed in order."))));
        QCOMPARE(finished.size(), 3);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));

        runner.startTask(QStringLiteral("Start a task so Stop can discard a queued follow-up."), true);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 5, 3000);
        const QJsonObject stopProposal{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("command"), QStringLiteral("experiment.run")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), trustedRodProblemSpec(10000)},
                {QStringLiteral("budgets"), trustedExperimentBudgets()}}}};
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("stop-queue-preview"), QStringLiteral("document_propose"), stopProposal)})));
        QCOMPARE(proposals.size(), 2);
        runner.authorizePending(proposals.last().at(1).toString(), true);
        QCOMPARE(actions.size(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 6, 5000);
        const QString stoppedExperimentId = actions.last().at(1).toJsonObject()
            .value(QStringLiteral("experimentId")).toString();
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("stop-queue-wait"), QStringLiteral("experiment_wait"),
                {{QStringLiteral("experimentId"), stoppedExperimentId}})})));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const QJsonObject current = host->handle(hostRequest(QStringLiteral("experiment.query"),
                {{QStringLiteral("experimentId"), stoppedExperimentId}})).value(QStringLiteral("result")).toObject();
            const QString state = current.value(QStringLiteral("state")).toString();
            return state == QStringLiteral("solving") || state == QStringLiteral("candidate_ready");
        }(), 10000);
        const int requestsBeforeStop = requested.size();
        bool stopReenteredDuringRetirement = false;
        QTimer::singleShot(0, &runner, [&runner, &stopReenteredDuringRetirement] {
            stopReenteredDuringRetirement = runner.isActive();
            runner.submitMessage(QStringLiteral("This queued message must be discarded by Stop."), false);
            runner.cancel();
        });
        runner.cancel();
        QVERIFY(stopReenteredDuringRetirement);
        QCOMPARE(finished.size(), 4);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("cancelled"));
        QCOMPARE(pendingMessagesCancelled.size(), 1);
        QCOMPARE(pendingMessagesCancelled.first().at(0).toStringList(),
                 QStringList{QStringLiteral("This queued message must be discarded by Stop.")});
        QTest::qWait(650);
        QCOMPARE(requested.size(), requestsBeforeStop);
        session->clear();
    }

    void childCancellationReportWaitsForTerminalStateAndNeverMasksRequestFailure()
    {
        const QStringList expectedChildJobIds{QStringLiteral("12345678901234567890")};
        QJsonObject summary{{QStringLiteral("state"), QStringLiteral("cancelled")},
            {QStringLiteral("cancellationPersisted"), true},
            {QStringLiteral("childJobsTruncated"), false},
            {QStringLiteral("childJobs"), QJsonArray{
                QJsonObject{{QStringLiteral("jobId"), QStringLiteral("12345678901234567890")},
                    {QStringLiteral("state"), QStringLiteral("cancelling")}}}}};
        const QJsonObject delayed = AgentTaskRunner::summarizeChildCancellation(
            summary, false, expectedChildJobIds);
        QCOMPARE(delayed.value(QStringLiteral("status")).toString(), QStringLiteral("cancelling"));
        QCOMPARE(delayed.value(QStringLiteral("code")).toString(), QStringLiteral("CHILD_STOP_PENDING"));
        QCOMPARE(delayed.value(QStringLiteral("childJobsTerminal")).toBool(), false);
        QVERIFY(delayed.value(QStringLiteral("childJobSetMatches")).toBool());
        QCOMPARE(delayed.value(QStringLiteral("terminalChildJobCount")).toInt(), 0);
        QCOMPARE(delayed.value(QStringLiteral("childJobStates")).toString(), QStringLiteral("cancelling"));

        QJsonObject missingChild = summary;
        missingChild.insert(QStringLiteral("childJobs"), QJsonArray{});
        const QJsonObject omitted = AgentTaskRunner::summarizeChildCancellation(
            missingChild, false, expectedChildJobIds);
        QCOMPARE(omitted.value(QStringLiteral("status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(omitted.value(QStringLiteral("code")).toString(), QStringLiteral("CHILD_JOB_SET_MISMATCH"));
        QVERIFY(!omitted.value(QStringLiteral("childJobSetMatches")).toBool());
        QVERIFY(!omitted.value(QStringLiteral("childJobsTerminal")).toBool());

        QJsonObject emptyChildSet{{QStringLiteral("state"), QStringLiteral("cancelled")},
            {QStringLiteral("cancellationPersisted"), true},
            {QStringLiteral("childJobsTruncated"), false},
            {QStringLiteral("childJobs"), QJsonArray{}}};
        const QJsonObject noChildJobs = AgentTaskRunner::summarizeChildCancellation(
            emptyChildSet, false, {});
        QCOMPARE(noChildJobs.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
        QVERIFY(noChildJobs.value(QStringLiteral("childJobSetMatches")).toBool());
        QVERIFY(noChildJobs.value(QStringLiteral("childJobsTerminal")).toBool());
        QCOMPARE(noChildJobs.value(QStringLiteral("childJobCount")).toInt(), 0);
        QCOMPARE(noChildJobs.value(QStringLiteral("childJobStates")).toString(), QStringLiteral("none"));

        const QString acceptedJobId = QStringLiteral("98765432109876543210");
        const QJsonObject acceptedChildOmitted = AgentTaskRunner::summarizeChildCancellation(
            emptyChildSet, false, {acceptedJobId});
        QCOMPARE(acceptedChildOmitted.value(QStringLiteral("status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(acceptedChildOmitted.value(QStringLiteral("code")).toString(),
                 QStringLiteral("CHILD_JOB_SET_MISMATCH"));
        QVERIFY(!acceptedChildOmitted.value(QStringLiteral("childJobSetMatches")).toBool());

        const QJsonObject missingAcceptedJob = AgentTaskRunner::summarizeChildCancellation(
            emptyChildSet, false, {}, false);
        QCOMPARE(missingAcceptedJob.value(QStringLiteral("status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(missingAcceptedJob.value(QStringLiteral("code")).toString(),
                 QStringLiteral("CHILD_JOB_SET_INCOMPLETE"));

        const QString expectedDigest(64, QLatin1Char('a'));
        QJsonObject cancellationBinding;
        QVERIFY(!AgentTaskRunner::authorizationDigestMatches(cancellationBinding, expectedDigest));
        cancellationBinding.insert(QStringLiteral("authorizationDigest"), expectedDigest);
        QVERIFY(AgentTaskRunner::authorizationDigestMatches(cancellationBinding, expectedDigest));
        cancellationBinding.insert(QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('b')));
        QVERIFY(!AgentTaskRunner::authorizationDigestMatches(cancellationBinding, expectedDigest));

        emptyChildSet.insert(QStringLiteral("childJobsTruncated"), true);
        const QJsonObject truncated = AgentTaskRunner::summarizeChildCancellation(
            emptyChildSet, false, {});
        QCOMPARE(truncated.value(QStringLiteral("status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(truncated.value(QStringLiteral("code")).toString(), QStringLiteral("CHILD_JOB_LIST_TRUNCATED"));

        QJsonArray children = summary.value(QStringLiteral("childJobs")).toArray();
        QJsonObject child = children.first().toObject();
        child.insert(QStringLiteral("state"), QStringLiteral("cancelled"));
        children[0] = child;
        summary.insert(QStringLiteral("childJobs"), children);
        const QJsonObject cancelled = AgentTaskRunner::summarizeChildCancellation(
            summary, false, expectedChildJobIds);
        QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(cancelled.value(QStringLiteral("code")).toString(), QStringLiteral("CANCELLED"));
        QCOMPARE(cancelled.value(QStringLiteral("childJobsTerminal")).toBool(), true);
        QVERIFY(cancelled.value(QStringLiteral("childJobSetMatches")).toBool());

        const QJsonObject failedRequest = AgentTaskRunner::summarizeChildCancellation(
            summary, true, expectedChildJobIds);
        QCOMPARE(failedRequest.value(QStringLiteral("status")).toString(),
                 QStringLiteral("cancellation_request_failed"));
        QCOMPARE(failedRequest.value(QStringLiteral("code")).toString(),
                 QStringLiteral("CHILD_CANCELLATION_REQUEST_FAILED"));
        QCOMPARE(failedRequest.value(QStringLiteral("childCancellationRequestFailed")).toBool(), true);

        child.insert(QStringLiteral("state"), QStringLiteral("succeeded"));
        children[0] = child;
        summary.insert(QStringLiteral("childJobs"), children);
        const QJsonObject racedTerminal = AgentTaskRunner::summarizeChildCancellation(
            summary, false, expectedChildJobIds);
        QCOMPARE(racedTerminal.value(QStringLiteral("status")).toString(), QStringLiteral("terminal"));
        QCOMPARE(racedTerminal.value(QStringLiteral("code")).toString(),
                 QStringLiteral("CHILD_TERMINAL_BEFORE_CANCEL"));
    }

    void meshGenerationAndAcceptanceAlwaysNeedFreshManualApprovalInAuto()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost(); QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        session->addModel(meshSurfaceFixture()); session->setModelSelected(0,true);
        const auto sourceStamp = session->modelCommandStamp(0);
        const QString sourceFingerprint = RModelCommandService::stateFingerprint(session->getModel(0));
        WorkflowProcess workflow;
        AgentTaskRunner generate(host,&workflow);
        QSignalSpy requested(&generate,&AgentTaskRunner::turnRequested);
        QSignalSpy ready(&generate,&AgentTaskRunner::proposalReady);
        QSignalSpy applied(&generate,&AgentTaskRunner::actionCompleted);
        generate.startTask("Generate a mesh and accept the candidate.",true);
        const QJsonObject proposal{{"documentId",sourceStamp.first},{"documentRevision",double(sourceStamp.second)},
            {"command","mesh.generate"},{"arguments",meshOptions()}};
        generate.receiveTurn(requested.first().at(0).toString(),parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall("mesh-generate","document_propose",proposal)})));
        QCOMPARE(ready.size(),1); QCOMPARE(applied.size(),0);
        QVERIFY(host->meshController()->jobs().isEmpty());
        generate.authorizePending(ready.first().at(1).toString(),true);
        QCOMPARE(applied.size(),1);
        const QString attempt = applied.first().at(1).toJsonObject().value("attemptId").toString();
        QVERIFY(!attempt.isEmpty()); generate.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(host->meshController()->query(attempt).value("result").toObject().value("status").toString(),
                                  QStringLiteral("candidate_ready"),30000);
        AgentTaskRunner accept(host,&workflow);
        QSignalSpy acceptRequested(&accept,&AgentTaskRunner::turnRequested);
        QSignalSpy acceptReady(&accept,&AgentTaskRunner::proposalReady);
        QSignalSpy acceptApplied(&accept,&AgentTaskRunner::actionCompleted);
        accept.startTask("Accept the verified mesh into a new document.",true);
        auto acceptProposal = proposal; acceptProposal.insert("command","mesh.accept");
        acceptProposal.insert("arguments",QJsonObject{{"attemptId",attempt}});
        accept.receiveTurn(acceptRequested.first().at(0).toString(),parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall("mesh-accept","document_propose",acceptProposal)})));
        QCOMPARE(acceptReady.size(),1); QCOMPARE(acceptApplied.size(),0); QCOMPARE(session->getNModels(),1u);
        const auto exactPreview = acceptReady.first().at(2).toJsonObject().value("meshPreview").toObject();
        QCOMPARE(exactPreview.value("attemptId"),QJsonValue(attempt));
        QCOMPARE(exactPreview.value("acceptanceMode"),QJsonValue("new_document_from_current_source"));
        accept.authorizePending(acceptReady.first().at(1).toString(),true);
        QCOMPARE(acceptApplied.size(),1); QCOMPARE(session->getNModels(),2u);
        QCOMPARE(session->modelCommandStamp(0),sourceStamp);
        QCOMPARE(RModelCommandService::stateFingerprint(session->getModel(0)),sourceFingerprint);
        QVERIFY(session->modelCommandStamp(1).first != sourceStamp.first);
        QCOMPARE(session->getModel(1).getProblemTaskTree().getProblemTypeMask(),R_PROBLEM_NONE);
        QCOMPARE(acceptApplied.first().at(1).toJsonObject().value("bindingState"),QJsonValue("REQUIRES_REBINDING"));
        for (uint n=0;n<session->getModel(1).getNNodes();++n)
            for (uint old=0;old<session->getModel(0).getNNodes();++old)
                QVERIFY(session->getModel(1).getNodePhysicalId(n) != session->getModel(0).getNodePhysicalId(old));
        accept.cancel();
        session->setModelSelected(0,true); session->setModelSelected(1,false);

        AgentTaskRunner readOnly(host,&workflow,nullptr,AgentTaskRunner::AccessPolicy::ReadOnly);
        QSignalSpy reads(&readOnly,&AgentTaskRunner::turnRequested);
        QSignalSpy readReady(&readOnly,&AgentTaskRunner::proposalReady);
        readOnly.startTask("Inspect the candidate without changing it.");
        readOnly.receiveTurn(reads.first().at(0).toString(),parseProviderBody(providerBody(QString(),QJsonArray{
            providerCall("mesh-read","mesh_job",{{"documentId",sourceStamp.first},{"documentRevision",double(sourceStamp.second)},{"attemptId",attempt}}),
            providerCall("mesh-denied","document_propose",acceptProposal)})));
        QTRY_COMPARE_WITH_TIMEOUT(reads.size(),2,1000);
        const auto readHistory = reads.last().at(1).toJsonArray();
        QVERIFY(toolResultContent(readHistory,"mesh-read").contains("accepted"));
        QVERIFY(toolResultContent(readHistory,"mesh-denied").contains("READ_ONLY_TOOL_DENIED"));
        QCOMPARE(readReady.size(),0); readOnly.cancel();
    }

    void meshToolsRejectUnknownMalformedAndExtraArguments()
    {
        auto *host = Application::instance()->getSession()->getModelCommandHost();
        QList<QPair<QString,QJsonObject>> cases;
        auto options = meshOptions(); options.insert("approval",true); cases.append({"mesh.generate",options});
        options = meshOptions(); options.insert("maxVolume",0); cases.append({"mesh.generate",options});
        options = meshOptions(); options.insert("unit","mm"); cases.append({"mesh.generate",options});
        cases.append({"mesh.accept",{{"attemptId","unknown"},{"remapOldIds",true}}});
        cases.append({"mesh.cancel",{{"attemptId",42}}});
        cases.append({"mesh.approve",{{"previewId","host-preview"}}});
        for (const auto &invalid : cases)
        {
            WorkflowProcess workflow; AgentTaskRunner runner(host,&workflow);
            QSignalSpy requested(&runner,&AgentTaskRunner::turnRequested);
            QSignalSpy ready(&runner,&AgentTaskRunner::proposalReady);
            QSignalSpy finished(&runner,&AgentTaskRunner::finished);
            runner.startTask("Inspect exact mesh schema.",true);
            runner.receiveTurn(requested.first().at(0).toString(),parseProviderBody(providerBody(QString(),QJsonArray{
                providerCall("invalid-mesh","document_propose",{{"documentId","source"},{"documentRevision",0},
                    {"command",invalid.first},{"arguments",invalid.second}})})));
            QCOMPARE(finished.size(),1);
            QCOMPARE(finished.first().at(0).toString(),QStringLiteral("failed"));
            QCOMPARE(ready.size(),0); runner.cancel();
        }
    }

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

    void evaluationArmToolDefinitionsAreExactAndStandardRemainsUnchanged()
    {
        const auto defaultWrite = functionNames(AgentTaskRunner::toolDefinitions());
        const auto explicitStandardWrite = functionNames(AgentTaskRunner::toolDefinitions(
            AgentTaskRunner::AccessPolicy::ReadWrite, AgentTaskRunner::EvaluationArm::Standard));
        QVERIFY(defaultWrite == explicitStandardWrite);

        const auto defaultReadOnly = functionNames(AgentTaskRunner::toolDefinitions(
            AgentTaskRunner::AccessPolicy::ReadOnly));
        const auto explicitStandardReadOnly = functionNames(AgentTaskRunner::toolDefinitions(
            AgentTaskRunner::AccessPolicy::ReadOnly, AgentTaskRunner::EvaluationArm::Standard));
        QVERIFY(defaultReadOnly == explicitStandardReadOnly);
        QVERIFY(defaultWrite.contains(QStringLiteral("document_propose")));
        QVERIFY(defaultWrite.contains(QStringLiteral("workflow_propose")));
        QVERIFY(defaultReadOnly.contains(QStringLiteral("ask_user")));
        QVERIFY(!defaultReadOnly.contains(QStringLiteral("document_propose")));

        const auto direct = functionNames(AgentTaskRunner::toolDefinitions(
            AgentTaskRunner::AccessPolicy::ReadWrite, AgentTaskRunner::EvaluationArm::DirectNl));
        QVERIFY(direct.isEmpty());

        const QSet<QString> queryTools{
            QStringLiteral("host_capabilities"), QStringLiteral("context_query"),
            QStringLiteral("trusted_problem_inspect"), QStringLiteral("trusted_problem_diagnose"),
            QStringLiteral("document_inputs"), QStringLiteral("document_targets"),
            QStringLiteral("native_jobs"), QStringLiteral("native_job"),
            QStringLiteral("native_job_recovery"), QStringLiteral("mesh_job"),
            QStringLiteral("native_result_query")};
        const auto toolsFeedback = functionNames(AgentTaskRunner::toolDefinitions(
            AgentTaskRunner::AccessPolicy::ReadWrite, AgentTaskRunner::EvaluationArm::ToolsFeedback));
        QVERIFY(toolsFeedback == queryTools);

        QSet<QString> fullTrusted = queryTools;
        fullTrusted.unite({QStringLiteral("experiment_run"), QStringLiteral("experiment_query"),
                           QStringLiteral("experiment_wait"), QStringLiteral("experiment_cancel")});
        const auto full = functionNames(AgentTaskRunner::toolDefinitions(
            AgentTaskRunner::AccessPolicy::ReadWrite, AgentTaskRunner::EvaluationArm::FullTrusted));
        QVERIFY(full == fullTrusted);
        for (const QString &forbidden : {QStringLiteral("ask_user"), QStringLiteral("document_propose"),
             QStringLiteral("workflow_propose"), QStringLiteral("extension_propose"),
             QStringLiteral("runtime_parameters_propose"), QStringLiteral("task_plan_propose")})
            QVERIFY(!full.contains(forbidden));
        QCOMPARE(AgentTaskRunner::evaluationArmId(AgentTaskRunner::EvaluationArm::DirectNl),
                 QStringLiteral("direct_nl"));
        QCOMPARE(AgentTaskRunner::evaluationArmId(AgentTaskRunner::EvaluationArm::ToolsFeedback),
                 QStringLiteral("tools_feedback"));
        QCOMPARE(AgentTaskRunner::evaluationArmId(AgentTaskRunner::EvaluationArm::FullTrusted),
                 QStringLiteral("full_trusted"));
    }

    void evaluationArmRejectsGloballyKnownCallsBeforeHostDispatch()
    {
        Session *session = Application::instance()->getSession();
        session->clear();
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);
        const auto before = host->query(0);
        QSignalSpy published(host, &ModelCommandHost::eventPublished);

        const QList<AgentTaskRunner::EvaluationArm> arms{
            AgentTaskRunner::EvaluationArm::DirectNl,
            AgentTaskRunner::EvaluationArm::ToolsFeedback,
            AgentTaskRunner::EvaluationArm::FullTrusted};
        for (const auto arm : arms)
        {
            WorkflowProcess workflow;
            AgentTaskRunner runner(host, &workflow, nullptr, AgentTaskRunner::AccessPolicy::ReadWrite, arm);
            QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
            QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
            QSignalSpy finished(&runner, &AgentTaskRunner::finished);
            runner.startTask(QStringLiteral("Use only the selected evaluation arm."));
            QCOMPARE(requested.size(), 1);
            const int hostEventCount = published.size();
            const QJsonObject validWrite = documentProposal(stamp.first, stamp.second);
            runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
                QJsonArray{providerCall(QStringLiteral("forbidden-document-proposal"),
                    QStringLiteral("document_propose"), validWrite)})));
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
            QCOMPARE(requested.size(), 1);
            QCOMPARE(proposals.size(), 0);
            QCOMPARE(actions.size(), 0);
            QCOMPARE(published.size(), hostEventCount);
            QCOMPARE(host->query(0), before);
        }

        // Exercise the dispatch fence independently of response validation.
        WorkflowProcess workflow;
        AgentTaskRunner direct(host, &workflow, nullptr, AgentTaskRunner::AccessPolicy::ReadWrite,
                               AgentTaskRunner::EvaluationArm::DirectNl);
        QSignalSpy toolResults(&direct, &AgentTaskRunner::toolResultObserved);
        direct.active = true;
        direct.activityTaskId = QStringLiteral("dispatch-fence-fixture");
        const QJsonObject arguments = documentProposal(stamp.first, stamp.second);
        direct.calls = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("bypassed-validation")},
            {QStringLiteral("name"), QStringLiteral("document_propose")},
            {QStringLiteral("arguments"), arguments}}};
        const int hostEventCount = published.size();
        direct.nextTool();
        direct.active = false;
        direct.budgetTimer.stop();
        QCOMPARE(toolResults.size(), 1);
        QCOMPARE(toolResults.first().first().toJsonObject().value(QStringLiteral("code")).toString(),
                 QStringLiteral("EVALUATION_ARM_TOOL_DENIED"));
        QCOMPARE(published.size(), hostEventCount);
        QCOMPARE(host->query(0), before);
        session->clear();
    }

    void fullTrustedExperimentRunProducesOnlyAnUnappliedPreview()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        Session *session = Application::instance()->getSession();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        session->addModel(modalFixture());
        session->setModelSelected(0, true);
        ModelCommandHost *host = session->getModelCommandHost();
        const auto stamp = session->modelCommandStamp(0);
        const auto sourceBefore = host->query(0);

        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow, nullptr, AgentTaskRunner::AccessPolicy::ReadWrite,
                               AgentTaskRunner::EvaluationArm::FullTrusted);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        runner.startTask(QStringLiteral("Prepare a trusted rod experiment."));
        QCOMPARE(requested.size(), 1);
        const auto names = functionNames(requested.first().at(2).toJsonArray());
        QVERIFY(names.contains(QStringLiteral("experiment_run")));
        QVERIFY(!names.contains(QStringLiteral("document_propose")));
        const QJsonObject runArguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), double(stamp.second)},
            {QStringLiteral("problemSpec"), trustedRodProblemSpec()},
            {QStringLiteral("budgets"), trustedExperimentBudgets(1, 0, 1)}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody({},
            QJsonArray{providerCall(QStringLiteral("full-trusted-preview"),
                QStringLiteral("experiment_run"), runArguments)})));

        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposals.first().at(0).toString(), QStringLiteral("experiment"));
        QVERIFY(!proposals.first().at(2).toJsonObject()
                     .value(QStringLiteral("previewId")).toString().isEmpty());
        QCOMPARE(actions.size(), 0);
        QVERIFY(runner.isActive());
        QVERIFY(!runner.isAwaitingInput());
        QCOMPARE(session->modelCommandStamp(0), stamp);
        QCOMPARE(host->query(0), sourceBefore);
        QVERIFY(!ModelCommandHost::isBusy());
        runner.cancel();
        session->clear();
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
             QStringLiteral("geometry.node_coordinates.set"), QStringLiteral("geometry.rigid_transform"),
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

    void runtimeParameterToolArgumentsRejectMalformedRequests()
    {
        auto *host = Application::instance()->getSession()->getModelCommandHost();
        WorkflowProcess workflow;
        const QJsonObject validArguments{{QStringLiteral("jobId"), QStringLiteral("runtime-job-1")},
            {QStringLiteral("velocityGainNsm"), 3.0},
            {QStringLiteral("actuatorLimits"), QJsonArray{QJsonObject{
                {QStringLiteral("actuatorId"), QStringLiteral("actuator-1")},
                {QStringLiteral("minN"), -10.0}, {QStringLiteral("maxN"), 10.0},
                {QStringLiteral("rateNPerS"), 0.0}}}}};

        auto rejected = [&](const QString &callId, const QJsonObject &arguments) {
            AgentTaskRunner runner(host, &workflow);
            QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
            QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
            QSignalSpy finished(&runner, &AgentTaskRunner::finished);
            runner.startTask(QStringLiteral("Validate a runtime parameter preview."), true);
            runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
                QJsonArray{providerCall(callId, QStringLiteral("runtime_parameters_propose"), arguments)})));
            QCOMPARE(finished.size(), 1);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
            QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("arguments do not match its supported schema")));
            QCOMPARE(proposals.size(), 0);
            QCOMPARE(actions.size(), 0);
        };

        QJsonObject reversedLimits = validArguments;
        reversedLimits.insert(QStringLiteral("actuatorLimits"), QJsonArray{QJsonObject{
            {QStringLiteral("actuatorId"), QStringLiteral("actuator-1")},
            {QStringLiteral("minN"), 11.0}, {QStringLiteral("maxN"), 10.0},
            {QStringLiteral("rateNPerS"), 0.0}}});
        rejected(QStringLiteral("reversed-limits"), reversedLimits);

        QJsonObject malformedActuator = validArguments;
        malformedActuator.insert(QStringLiteral("actuatorLimits"), QJsonArray{QJsonObject{
            {QStringLiteral("actuatorId"), QStringLiteral("actuator-1")},
            {QStringLiteral("minN"), -10.0}, {QStringLiteral("maxN"), 10.0}}});
        rejected(QStringLiteral("malformed-actuator"), malformedActuator);

        QJsonObject nonFiniteGain = validArguments;
        nonFiniteGain.insert(QStringLiteral("velocityGainNsm"), std::numeric_limits<double>::quiet_NaN());
        rejected(QStringLiteral("non-finite-gain"), nonFiniteGain);

        QJsonObject booleanGain = validArguments;
        booleanGain.insert(QStringLiteral("velocityGainNsm"), true);
        rejected(QStringLiteral("boolean-gain"), booleanGain);

        QJsonObject unknownField = validArguments;
        unknownField.insert(QStringLiteral("filesystemPath"), QStringLiteral("C:/model.ram"));
        rejected(QStringLiteral("unknown-field"), unknownField);
    }

    void runtimeParameterToolsOnlyPreviewAndQueryAndRemainDeniedInReadOnly()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto config = WorkflowProcess::defaultConfiguration(directory.filePath(QStringLiteral("workspace")));
        config.solver = config.python;
        config.native = config.python;
        config.examples = directory.path();
        config.packageDirectory = directory.filePath(QStringLiteral("package"));
        const QString packagePath = config.packageDirectory + QStringLiteral("/range_workflow");
        QVERIFY(QDir().mkpath(packagePath));
        QFile packageInit(packagePath + QStringLiteral("/__init__.py"));
        QVERIFY(packageInit.open(QIODevice::WriteOnly));
        packageInit.close();

#ifdef RANGE_WORKFLOW_SOURCE_ROOT
        QFile fixtureSource(QDir(QStringLiteral(RANGE_WORKFLOW_SOURCE_ROOT)).filePath(
            QStringLiteral("range-workflow/tests/runtime_widget_bootstrap.py")));
        QVERIFY(fixtureSource.open(QIODevice::ReadOnly));
        QByteArray fixtureScript = fixtureSource.readAll();
#else
        QFAIL("RANGE_WORKFLOW_SOURCE_ROOT must identify the isolated runtime fixture");
        return;
#endif
        const QByteArray originalCounts = "counts = {\"preview\": 0, \"approve\": 0, \"submit\": 0, \"pause\": 0}";
        QVERIFY(fixtureScript.contains(originalCounts));
        fixtureScript.replace(originalCounts,
            "counts = {\"preview\": 0, \"approve\": 0, \"submit\": 0, \"pause\": 0, \"query\": 0}");
        const QByteArray fixtureQueryRoute = "    if op == \"fixture.query\":";
        QVERIFY(fixtureScript.contains(fixtureQueryRoute));
        fixtureScript.replace(fixtureQueryRoute,
            "    if op == \"project.query\":\n"
            "        return dict(projectId=\"fixture-project\", modelId=\"fixture-model\", sessionId=\"fixture-session\",\n"
            "                    configuration={}, modelBinding=dict(kind=\"template\"))\n"
            "    if op == \"fixture.query\":");
        const QByteArray runtimeQueryRoute = "    if op == \"runtime.parameters.query\":";
        QVERIFY(fixtureScript.contains(runtimeQueryRoute));
        fixtureScript.replace(runtimeQueryRoute,
            "    if op == \"runtime.parameters.query\":\n"
            "        counts[\"query\"] += 1");
        QFile fixtureMain(packagePath + QStringLiteral("/__main__.py"));
        QVERIFY(fixtureMain.open(QIODevice::WriteOnly));
        QCOMPARE(fixtureMain.write(fixtureScript), qint64(fixtureScript.size()));
        fixtureMain.close();

        QVERIFY(QFileInfo::exists(config.python));
        WorkflowProcess workflow;
        workflow.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isReady(), 15000);

        auto *host = Application::instance()->getSession()->getModelCommandHost();
        const QString jobId = QStringLiteral("00000000-0000-4000-8000-000000000001");
        const QJsonObject limits{ {QStringLiteral("actuatorId"), QStringLiteral("actuator")},
            {QStringLiteral("minN"), -20.0}, {QStringLiteral("maxN"), 20.0},
            {QStringLiteral("rateNPerS"), 0.0}};
        const QJsonObject proposalArguments{{QStringLiteral("jobId"), jobId},
            {QStringLiteral("velocityGainNsm"), 4.5},
            {QStringLiteral("actuatorLimits"), QJsonArray{limits}}};
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy proposals(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy actions(&runner, &AgentTaskRunner::actionCompleted);
        QSignalSpy finished(&runner, &AgentTaskRunner::finished);
        runner.startTask(QStringLiteral("Preview current-run runtime parameters and query their actual state."), true);
        QCOMPARE(requested.size(), 1);

        QJsonObject proposalDefinition;
        QJsonObject queryDefinition;
        QStringList definedToolNames;
        for (const QJsonValue &toolValue : requested.first().at(2).toJsonArray())
        {
            const QJsonObject function = toolValue.toObject().value(QStringLiteral("function")).toObject();
            definedToolNames.append(function.value(QStringLiteral("name")).toString());
            if (function.value(QStringLiteral("name")) == QJsonValue(QStringLiteral("runtime_parameters_propose")))
                proposalDefinition = function;
            if (function.value(QStringLiteral("name")) == QJsonValue(QStringLiteral("runtime_parameters_query")))
                queryDefinition = function;
        }
        QVERIFY(!proposalDefinition.isEmpty());
        QVERIFY(!queryDefinition.isEmpty());
        for (const QString &forbiddenTool : {QStringLiteral("runtime_parameters_approve"),
             QStringLiteral("runtime_parameters_submit"), QStringLiteral("runtime_execution_pause"),
             QStringLiteral("runtime_execution_resume"), QStringLiteral("runtime_execution_cancel")})
            QVERIFY(!definedToolNames.contains(forbiddenTool));
        const QJsonObject proposalSchema = proposalDefinition.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(proposalSchema.value(QStringLiteral("additionalProperties")).toBool(), false);
        const QJsonArray expectedProposalFields{QStringLiteral("jobId"), QStringLiteral("velocityGainNsm"),
            QStringLiteral("actuatorLimits")};
        QCOMPARE(proposalSchema.value(QStringLiteral("required")).toArray(), expectedProposalFields);
        const QJsonObject proposalProperties = proposalSchema.value(QStringLiteral("properties")).toObject();
        QCOMPARE(proposalProperties.value(QStringLiteral("velocityGainNsm")).toObject().value(QStringLiteral("minimum")).toDouble(), 1e-9);
        QCOMPARE(proposalProperties.value(QStringLiteral("velocityGainNsm")).toObject().value(QStringLiteral("maximum")).toDouble(), 1e12);
        const QJsonObject limitsSchema = proposalProperties.value(QStringLiteral("actuatorLimits")).toObject();
        QCOMPARE(limitsSchema.value(QStringLiteral("minItems")).toInt(), 1);
        QCOMPARE(limitsSchema.value(QStringLiteral("maxItems")).toInt(), 1);
        const QJsonObject querySchema = queryDefinition.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(querySchema.value(QStringLiteral("additionalProperties")).toBool(), false);
        const QJsonArray expectedQueryFields{QStringLiteral("jobId")};
        QCOMPARE(querySchema.value(QStringLiteral("required")).toArray(), expectedQueryFields);
        const QString proposalDescription = proposalDefinition.value(QStringLiteral("description")).toString();
        QVERIFY(proposalDescription.contains(QStringLiteral("N*s/m")));
        QVERIFY(proposalDescription.contains(QStringLiteral("N/s")));
        QVERIFY(proposalDescription.contains(QStringLiteral("trusted user approval")));
        QVERIFY(queryDefinition.value(QStringLiteral("description")).toString().contains(QStringLiteral("accepted native receipt")));
        const QString systemPrompt = requested.first().at(1).toJsonArray().first().toObject()
            .value(QStringLiteral("content")).toString();
        QVERIFY(systemPrompt.contains(QStringLiteral("Run parameters")));
        QVERIFY(systemPrompt.contains(QStringLiteral("later document revision")));
        QVERIFY(systemPrompt.contains(QStringLiteral("knowledge_query")));

        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(), QJsonArray{
            providerCall(QStringLiteral("runtime-current-before"), QStringLiteral("runtime_parameters_query"),
                {{QStringLiteral("jobId"), jobId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 2, 5000);
        const QJsonObject initialCurrent = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("runtime-current-before")).toUtf8()).object();
        const QJsonObject initialAttempts = initialCurrent.value(QStringLiteral("runtimeControl")).toObject()
            .value(QStringLiteral("attempts")).toObject();
        QCOMPARE(initialAttempts.size(), 1);
        const QJsonObject initialAttempt = initialAttempts.constBegin().value().toObject();
        const QJsonObject identity = initialAttempt.value(QStringLiteral("identity")).toObject();
        QCOMPARE(identity.value(QStringLiteral("jobId")).toString(), jobId);
        QVERIFY(!identity.value(QStringLiteral("runId")).toString().isEmpty());
        QVERIFY(!identity.value(QStringLiteral("modelSha256")).toString().isEmpty());
        QCOMPARE(initialAttempt.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("parameters")).toObject()
                     .value(QStringLiteral("velocityGainNsm")).toDouble(), 2.0);

        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall(QStringLiteral("runtime-preview"), QStringLiteral("runtime_parameters_propose"), proposalArguments)})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 3, 5000);
        const QJsonObject previewResult = QJsonDocument::fromJson(toolResultContent(
            requested.last().at(1).toJsonArray(), QStringLiteral("runtime-preview")).toUtf8()).object();
        const QString proposalId = previewResult.value(QStringLiteral("proposalId")).toString();
        QVERIFY(!proposalId.isEmpty());
        QCOMPARE(previewResult.value(QStringLiteral("status")).toString(), QStringLiteral("previewed"));
        QCOMPARE(previewResult.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("velocityGainNsm")).toDouble(), 4.5);
        QCOMPARE(previewResult.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("actuatorLimits")).toArray().first().toObject(), limits);
        QCOMPARE(previewResult.value(QStringLiteral("identity")).toObject(), identity);
        QVERIFY(!previewResult.contains(QStringLiteral("receipt")));

        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QString(), QJsonArray{
            providerCall(QStringLiteral("runtime-current"), QStringLiteral("runtime_parameters_query"),
                {{QStringLiteral("jobId"), jobId}}),
            providerCall(QStringLiteral("runtime-preview-query"), QStringLiteral("runtime_parameters_query"),
                {{QStringLiteral("jobId"), jobId}, {QStringLiteral("proposalId"), proposalId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 4, 5000);
        const QJsonArray queryHistory = requested.last().at(1).toJsonArray();
        const QJsonObject currentResult = QJsonDocument::fromJson(toolResultContent(
            queryHistory, QStringLiteral("runtime-current")).toUtf8()).object();
        const QJsonObject queriedProposal = QJsonDocument::fromJson(toolResultContent(
            queryHistory, QStringLiteral("runtime-preview-query")).toUtf8()).object();
        const QJsonObject currentAttempt = currentResult.value(QStringLiteral("runtimeControl")).toObject()
            .value(QStringLiteral("attempts")).toObject().value(identity.value(QStringLiteral("runId")).toString()).toObject();
        QCOMPARE(currentAttempt.value(QStringLiteral("identity")).toObject(), identity);
        QCOMPARE(currentAttempt.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("parameters")).toObject()
                     .value(QStringLiteral("velocityGainNsm")).toDouble(), 2.0);
        QCOMPARE(queriedProposal.value(QStringLiteral("proposalId")).toString(), proposalId);
        QCOMPARE(queriedProposal.value(QStringLiteral("status")).toString(), QStringLiteral("previewed"));
        QVERIFY(!queriedProposal.contains(QStringLiteral("receipt")));
        QJsonObject counters = workflowRequest(workflow, QStringLiteral("fixture.query"))
            .value(QStringLiteral("counts")).toObject();
        QCOMPARE(counters.value(QStringLiteral("preview")).toInt(), 1);
        QCOMPARE(counters.value(QStringLiteral("query")).toInt(), 3);
        QCOMPARE(counters.value(QStringLiteral("approve")).toInt(), 0);
        QCOMPARE(counters.value(QStringLiteral("submit")).toInt(), 0);
        QCOMPARE(proposals.size(), 0);
        QCOMPARE(actions.size(), 0);

        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The saved preview remains unapproved and unapplied."))));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));

        AgentTaskRunner readOnly(host, &workflow, nullptr, AgentTaskRunner::AccessPolicy::ReadOnly);
        QSignalSpy readOnlyRequested(&readOnly, &AgentTaskRunner::turnRequested);
        QSignalSpy readOnlyProposals(&readOnly, &AgentTaskRunner::proposalReady);
        QSignalSpy readOnlyActions(&readOnly, &AgentTaskRunner::actionCompleted);
        QSignalSpy readOnlyFinished(&readOnly, &AgentTaskRunner::finished);
        readOnly.startTask(QStringLiteral("Inspect this current runtime read-only."));
        QTRY_COMPARE_WITH_TIMEOUT(readOnlyRequested.size(), 1, 5000);
        for (const QJsonValue &toolValue : readOnlyRequested.first().at(2).toJsonArray())
        {
            const QString toolName = toolValue.toObject().value(QStringLiteral("function")).toObject()
                .value(QStringLiteral("name")).toString();
            QVERIFY(toolName != QStringLiteral("runtime_parameters_propose"));
            QVERIFY(toolName != QStringLiteral("runtime_parameters_query"));
        }
        readOnly.receiveTurn(readOnlyRequested.first().at(0).toString(), parseProviderBody(providerBody(QString(), QJsonArray{
            providerCall(QStringLiteral("hostile-runtime-preview"), QStringLiteral("runtime_parameters_propose"), proposalArguments),
            providerCall(QStringLiteral("hostile-runtime-query"), QStringLiteral("runtime_parameters_query"),
                {{QStringLiteral("jobId"), jobId}, {QStringLiteral("proposalId"), proposalId}})})));
        QTRY_COMPARE_WITH_TIMEOUT(readOnlyRequested.size(), 2, 5000);
        const QJsonArray deniedHistory = readOnlyRequested.last().at(1).toJsonArray();
        QVERIFY(toolResultContent(deniedHistory, QStringLiteral("hostile-runtime-preview")).contains(QStringLiteral("READ_ONLY_TOOL_DENIED")));
        QVERIFY(toolResultContent(deniedHistory, QStringLiteral("hostile-runtime-query")).contains(QStringLiteral("READ_ONLY_TOOL_DENIED")));
        counters = workflowRequest(workflow, QStringLiteral("fixture.query")).value(QStringLiteral("counts")).toObject();
        QCOMPARE(counters.value(QStringLiteral("preview")).toInt(), 1);
        QCOMPARE(counters.value(QStringLiteral("query")).toInt(), 3);
        QCOMPARE(counters.value(QStringLiteral("approve")).toInt(), 0);
        QCOMPARE(counters.value(QStringLiteral("submit")).toInt(), 0);
        QCOMPARE(readOnlyProposals.size(), 0);
        QCOMPARE(readOnlyActions.size(), 0);
        readOnly.receiveTurn(readOnlyRequested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("No runtime action was available to this read-only task."))));
        QTRY_COMPARE_WITH_TIMEOUT(readOnlyFinished.size(), 1, 5000);
        QCOMPARE(readOnlyFinished.first().at(0).toString(), QStringLiteral("completed"));
        workflow.stop();
        QTRY_VERIFY_WITH_TIMEOUT(workflow.isStopped(), 5000);
    }

    void staticBindingRequiresManualApprovalEvenInAutoAndDeniesReadOnly()
    {
        static QTemporaryDir workspace;
        static QTemporaryDir sources;
        QVERIFY(workspace.isValid() && sources.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        // Suppress ordinary import UI only when seeding the unbound source;
        // actual command publication and candidate acceptance keep their GUI callbacks.
        Model source=unboundTetraFixture();
        source.initializeMeshInput();
        const double minimumSeed=source.getProblemSetup().getMeshSetup().getMinEdgeLength();
        const double maximumSeed=source.getProblemSetup().getMeshSetup().getMaxEdgeLength();
        const QString sourcePath=sources.filePath("agent-unbound.ram");
        ModelIO::save(source,sourcePath,false);
        Model loaded; ModelIO::open(loaded,sourcePath);
        QCOMPARE(loaded.getProblemTaskTree().getProblemTypeMask(),int(R_PROBLEM_NONE));
        QVERIFY(loaded.getLoadedSourceBinding().has_value());
        QVERIFY(loaded.getProblemSetup().getMeshSetup().getMinEdgeLength()>0);
        QVERIFY(loaded.getProblemSetup().getMeshSetup().getMaxEdgeLength()>0);
        QCOMPARE(loaded.getProblemSetup().getMeshSetup().getMinEdgeLength(),minimumSeed);
        QCOMPARE(loaded.getProblemSetup().getMeshSetup().getMaxEdgeLength(),maximumSeed);
        { const QSignalBlocker initialImport(session); session->addModel(loaded); }
        session->setModelSelected(0, true);
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        runner.startTask("Bind this mesh for static Stress.", true);
        const auto stamp = session->modelCommandStamp(0);
        QJsonObject proposal{{"documentId", stamp.first}, {"documentRevision", double(stamp.second)},
            {"command", "mesh.rebind.static"}, {"arguments", staticBindingArguments()}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall("bind-static", "document_propose", proposal)})));
        QCOMPARE(ready.size(), 1); QCOMPARE(applied.size(), 0);
        QCOMPARE(session->getModel(0).getNPoints(), 0u);
        runner.authorizePending(ready.first().at(1).toString(), true);
        QCOMPARE(applied.size(), 1); QCOMPARE(session->getModel(0).getNPoints(), 4u);
        runner.cancel();
        AgentTaskRunner readOnly(host, &workflow, nullptr, AgentTaskRunner::AccessPolicy::ReadOnly);
        QSignalSpy reads(&readOnly, &AgentTaskRunner::turnRequested);
        QSignalSpy readReady(&readOnly, &AgentTaskRunner::proposalReady);
        readOnly.startTask("Inspect static binding without editing.");
        proposal.insert("documentRevision", double(session->modelCommandStamp(0).second));
        readOnly.receiveTurn(reads.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall("denied-static", "document_propose", proposal)})));
        QTRY_COMPARE_WITH_TIMEOUT(reads.size(), 2, 1000);
        QVERIFY(toolResultContent(reads.last().at(1).toJsonArray(), "denied-static").contains("READ_ONLY_TOOL_DENIED"));
        QCOMPARE(readReady.size(), 0); readOnly.cancel();
    }

    void staticBindingRejectsUnknownAndMalformedTypedArguments()
    {
        auto *host = Application::instance()->getSession()->getModelCommandHost();
        WorkflowProcess workflow;
        QList<QJsonObject> bad;
        auto arguments = staticBindingArguments(); arguments.insert("oldMapper", "legacy-target"); bad.append(arguments);
        arguments = staticBindingArguments(); arguments.insert("unitSI", "true"); bad.append(arguments);
        arguments = staticBindingArguments(); arguments.insert("staticStress", false); bad.append(arguments);
        arguments = staticBindingArguments(); arguments.insert("loads", QJsonArray{QJsonObject{
            {"nodePhysicalId", 131}, {"forceN", QJsonArray{0,0,QJsonValue(QJsonValue::Null)}}}}); bad.append(arguments);
        arguments = staticBindingArguments(); arguments.insert("constraints", QJsonArray{QJsonObject{
            {"nodePhysicalId", 0}, {"values", QJsonArray{0,0,0}}}}); bad.append(arguments);
        arguments = staticBindingArguments(); auto material = arguments.value("material").toObject();
        material.insert("poissonRatio", 0.5); arguments.insert("material", material); bad.append(arguments);
        arguments = staticBindingArguments(); arguments.insert("loads", QJsonArray{QJsonObject{
            {"nodePhysicalId", 131}, {"forceN", QJsonArray{0,0,-1}}, {"code", "execute"}}}); bad.append(arguments);
        for (const auto &invalid : bad)
        {
            AgentTaskRunner runner(host, &workflow);
            QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
            QSignalSpy finished(&runner, &AgentTaskRunner::finished);
            QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
            runner.startTask("Validate static binding input.");
            runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
                QJsonArray{providerCall("invalid-static", "document_propose", {{"documentId", "document-test"},
                    {"documentRevision", 0}, {"command", "mesh.rebind.static"}, {"arguments", invalid}})})));
            QCOMPARE(finished.size(), 1); QCOMPARE(ready.size(), 0);
            QCOMPARE(finished.first().at(0).toString(), QString("failed"));
        }
    }

    void geometryToolsUseHostPreviewApprovalAndRemainDeniedReadOnly()
    {
        static QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        QString error;
        QVERIFY2(session->openProject(workspace.path(), &error), qPrintable(error));
        Model fixture = stressFixture();
        fixture.setNodePhysicalId(1, 73);
        session->addModel(fixture); session->setModelSelected(0, true);
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy applied(&runner, &AgentTaskRunner::actionCompleted);
        runner.startTask("Change the physical node coordinates after preview.");
        const auto stamp = session->modelCommandStamp(0);
        QJsonObject proposal{{"documentId", stamp.first}, {"documentRevision", double(stamp.second)},
            {"command", "geometry.node_coordinates.set"}, {"arguments", QJsonObject{{"unit", "m"},
                {"directionPolicy", "preserve_global"}, {"nodes", QJsonArray{QJsonObject{
                    {"physicalNodeId", 73}, {"coordinates", QJsonArray{2, 0, 0}}}}}}}};
        runner.receiveTurn(requested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall("geometry-node", "document_propose", proposal)})));
        QCOMPARE(ready.size(), 1); QCOMPARE(applied.size(), 0);
        QCOMPARE(session->getModel(0).getNode(1).getX(), 1.0);
        runner.authorizePending(ready.first().at(1).toString(), true);
        QCOMPARE(applied.size(), 1);
        QCOMPARE(session->getModel(0).getNode(1).getX(), 2.0);
        runner.cancel();

        AgentTaskRunner readOnly(host, &workflow, nullptr, AgentTaskRunner::AccessPolicy::ReadOnly);
        QSignalSpy readRequested(&readOnly, &AgentTaskRunner::turnRequested);
        QSignalSpy readReady(&readOnly, &AgentTaskRunner::proposalReady);
        QSignalSpy readApplied(&readOnly, &AgentTaskRunner::actionCompleted);
        readOnly.startTask("Describe geometry without making changes.");
        proposal.insert("documentRevision", double(session->modelCommandStamp(0).second));
        proposal.insert("command", "geometry.rigid_transform");
        proposal.insert("arguments", QJsonObject{{"unit", "m"}, {"angleUnit", "deg"},
            {"directionPolicy", "preserve_global"}, {"center", QJsonArray{0, 0, 0}},
            {"axis", QJsonArray{0, 0, 1}}, {"angle", 90}, {"translation", QJsonArray{0, 0, 0}}});
        readOnly.receiveTurn(readRequested.first().at(0).toString(), parseProviderBody(providerBody(QString(),
            QJsonArray{providerCall("denied-geometry", "document_propose", proposal)})));
        QTRY_COMPARE_WITH_TIMEOUT(readRequested.size(), 2, 1000);
        QVERIFY(toolResultContent(readRequested.last().at(1).toJsonArray(), "denied-geometry").contains("READ_ONLY_TOOL_DENIED"));
        QCOMPARE(readReady.size(), 0); QCOMPARE(readApplied.size(), 0);
        QCOMPARE(session->getModel(0).getNode(1).getX(), 2.0);
        readOnly.cancel();
    }

    void extensionV2SchemasAreAdvertisedWithV1Preserved()
    {
        QJsonArray ids;
        for (const auto &tool : AgentTaskRunner::toolDefinitions())
        {
            const auto function = tool.toObject().value("function").toObject();
            if (function.value("name") == QJsonValue("schema_get"))
                ids = function.value("parameters").toObject().value("properties").toObject()
                    .value("schemaId").toObject().value("enum").toArray();
        }
        for (const auto &id : {"range.extension/v1/manifest", "range.extension/v1/numeric-series",
            "range.extension/v1/statistics", "range.extension/v2/manifest", "range.extension/v2/multichannel-series",
            "range.extension/v2/spectral-result", "range.task-plan/v1"})
            QVERIFY(ids.contains(QJsonValue(id)));
    }

    void experimentRunToolSchemaRequiresAllBoundedBudgets()
    {
        QJsonObject runParameters;
        QJsonObject documentProposeParameters;
        QJsonArray documentCommands;
        for (const auto &tool : AgentTaskRunner::toolDefinitions())
        {
            const auto function = tool.toObject().value(QStringLiteral("function")).toObject();
            const QString name = function.value(QStringLiteral("name")).toString();
            if (name == QStringLiteral("experiment_run"))
                runParameters = function.value(QStringLiteral("parameters")).toObject();
            if (name == QStringLiteral("document_propose"))
            {
                documentProposeParameters = function.value(QStringLiteral("parameters")).toObject();
                documentCommands = documentProposeParameters
                    .value(QStringLiteral("properties")).toObject()
                    .value(QStringLiteral("command")).toObject().value(QStringLiteral("enum")).toArray();
            }
        }
        QVERIFY(!runParameters.isEmpty());
        const QJsonArray required = runParameters.value(QStringLiteral("required")).toArray();
        for (const QString &field : {QStringLiteral("documentId"), QStringLiteral("documentRevision"),
                                     QStringLiteral("problemSpec"), QStringLiteral("budgets")})
            QVERIFY(required.contains(field));
        QVERIFY(!documentCommands.contains(QJsonValue(QStringLiteral("experiment.run"))));
        const QJsonObject genericCommandArguments = documentProposeParameters.value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("arguments")).toObject().value(QStringLiteral("properties")).toObject();
        QVERIFY(!genericCommandArguments.contains(QStringLiteral("budgets")));
        QVERIFY(!genericCommandArguments.contains(QStringLiteral("problemSpec")));
        const QJsonObject budgetSchema = runParameters.value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("budgets")).toObject();
        const QJsonArray budgetRequired = budgetSchema.value(QStringLiteral("required")).toArray();
        for (const QString &field : {QStringLiteral("solves"), QStringLiteral("repairs"), QStringLiteral("modelTurns")})
            QVERIFY(budgetRequired.contains(field));
        const QJsonObject budgetProperties = budgetSchema.value(QStringLiteral("properties")).toObject();
        QCOMPARE(budgetProperties.value(QStringLiteral("solves")).toObject().value(QStringLiteral("minimum")).toInt(), 1);
        QCOMPARE(budgetProperties.value(QStringLiteral("solves")).toObject().value(QStringLiteral("maximum")).toInt(), 8);
        QCOMPARE(budgetProperties.value(QStringLiteral("repairs")).toObject().value(QStringLiteral("minimum")).toInt(), 0);
        QCOMPARE(budgetProperties.value(QStringLiteral("repairs")).toObject().value(QStringLiteral("maximum")).toInt(), 3);
        QCOMPARE(budgetProperties.value(QStringLiteral("modelTurns")).toObject().value(QStringLiteral("minimum")).toInt(), 1);
        QCOMPARE(budgetProperties.value(QStringLiteral("modelTurns")).toObject().value(QStringLiteral("maximum")).toInt(), 12);
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

        QJsonObject geometry{{"unit", "m"}, {"directionPolicy", "preserve_global"},
            {"nodes", QJsonArray{QJsonObject{{"physicalNodeId", 73}, {"coordinates", QJsonArray{2, 0, 0}}}}}};
        auto invalidGeometry = geometry; invalidGeometry.insert("unit", "mm");
        rejected("geometry-unit", "geometry.node_coordinates.set", invalidGeometry);
        invalidGeometry = geometry; invalidGeometry.insert("selectedOnly", true);
        rejected("geometry-extra", "geometry.node_coordinates.set", invalidGeometry);
        invalidGeometry = geometry;
        invalidGeometry.insert("nodes", QJsonArray{QJsonObject{{"physicalNodeId", "73"}, {"coordinates", QJsonArray{2, 0, 0}}}});
        rejected("geometry-id", "geometry.node_coordinates.set", invalidGeometry);
        invalidGeometry = geometry;
        invalidGeometry.insert("nodes", QJsonArray{QJsonObject{{"physicalNodeId", 73}, {"coordinates", QJsonArray{1000001, 0, 0}}}});
        rejected("geometry-range", "geometry.node_coordinates.set", invalidGeometry);
        rejected("geometry-axis", "geometry.rigid_transform", {{"unit", "m"}, {"angleUnit", "deg"},
            {"directionPolicy", "preserve_global"}, {"center", QJsonArray{0, 0, 0}}, {"axis", QJsonArray{0, 0, 0}},
            {"angle", 90}, {"translation", QJsonArray{0, 0, 0}}});
        rejected("geometry-angle", "geometry.rigid_transform", {{"unit", "m"}, {"angleUnit", "deg"},
            {"directionPolicy", "preserve_global"}, {"center", QJsonArray{0, 0, 0}}, {"axis", QJsonArray{0, 0, 1}},
            {"angle", 361}, {"translation", QJsonArray{0, 0, 0}}});

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

    void toolActivityIsBoundedAndCannotAdvanceAfterSynchronousSteering()
    {
        auto *session = Application::instance()->getSession();
        WorkflowProcess workflow;
        AgentTaskRunner runner(session->getModelCommandHost(), &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy activity(&runner, &AgentTaskRunner::toolActivity);
        runner.startTask(QStringLiteral("Read the current context."));
        auto first = parseProviderBody(providerBody({}, QJsonArray{
            providerCall(QStringLiteral("first-read"), QStringLiteral("context_query"), {})}));
        runner.receiveTurn(requested.last().at(0).toString(), first);
        QCOMPARE(activity.size(), 2);
        const QString firstTask = activity.first().at(0).toString();
        QVERIFY(!firstTask.isEmpty());
        QCOMPARE(activity.first().at(1).toString(), QStringLiteral("first-read"));
        QCOMPARE(activity.first().at(3).toString(), QStringLiteral("reading"));
        QCOMPARE(activity.last().at(0).toString(), firstTask);
        QCOMPARE(activity.last().at(3).toString(), QStringLiteral("completed"));
        QVERIFY(!activity.last().at(5).toJsonObject().contains(QStringLiteral("arguments")));
        runner.cancel();
        runner.startTask(QStringLiteral("Start a second read."));
        bool steered = false;
        connect(&runner, &AgentTaskRunner::toolActivity, &runner,
            [&](const QString &, const QString &, const QString &, const QString &phase,
                const QString &, const QJsonObject &) {
                if (!steered && phase == QStringLiteral("reading"))
                {
                    steered = true;
                    runner.submitMessage(QStringLiteral("New task after activity signal."));
                }
            });
        QSignalSpy lateActivity(&runner, &AgentTaskRunner::toolActivity);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({}, QJsonArray{
            providerCall(QStringLiteral("second-read"), QStringLiteral("context_query"), {})})));
        QVERIFY(steered);
        QCOMPARE(activity.size(), 3);
        QCOMPARE(lateActivity.size(), 1);
        QCOMPARE(lateActivity.first().at(0).toString(), activity.last().at(0).toString());
        QVERIFY(activity.last().at(0).toString() != firstTask);
        const int requestCount = requested.size();
        QTest::qWait(30);
        QCOMPARE(requested.size(), requestCount);
        runner.receiveTurn(requested.last().at(0).toString(),
            parseProviderBody(providerBody(QStringLiteral("The new task completed."))));
        QVERIFY(!runner.isActive());
    }

    void synchronousActivityAuthorizationAndSteeringPreserveTheNewProposal()
    {
        auto *session = Application::instance()->getSession();
        auto *host = session->getModelCommandHost();
        session->addModel(modalFixture());
        const auto initial = documentContext(host).value("documents").toArray().first().toObject();
        const QString identity = initial.value("documentId").toString();
        WorkflowProcess workflow;
        AgentTaskRunner runner(host, &workflow);
        QSignalSpy requested(&runner, &AgentTaskRunner::turnRequested);
        QSignalSpy ready(&runner, &AgentTaskRunner::proposalReady);
        QSignalSpy accepted(&runner, &AgentTaskRunner::actionCompleted);
        bool recursiveAuthorization = false, steered = false;
        runner.startTask(QStringLiteral("Propose the first edit."));
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({}, QJsonArray{
            providerCall("first-proposal", "document_propose", documentProposal(identity,
                quint64(initial.value("documentRevision").toDouble())))})));
        QCOMPARE(ready.size(), 1);
        const QString firstProposal = ready.first().at(1).toString();
        connect(&runner, &AgentTaskRunner::toolActivity, &runner,
            [&](const QString &, const QString &call, const QString &, const QString &phase,
                const QString &, const QJsonObject &) {
                if (call != "first-proposal") return;
                if (phase == "executing" && !recursiveAuthorization)
                {
                    recursiveAuthorization = true;
                    runner.authorizePending(firstProposal, true);
                    runner.authorizePending(firstProposal, false);
                }
                if (phase == "completed" && !steered)
                {
                    steered = true;
                    runner.submitMessage(QStringLiteral("Keep the new proposal awaiting approval."));
                    const auto current = documentContext(host).value("documents").toArray().first().toObject();
                    auto followup = documentProposal(identity, quint64(current.value("documentRevision").toDouble()));
                    auto parameters = followup.value("arguments").toObject();
                    parameters["iterations"] = 33;
                    followup["arguments"] = parameters;
                    runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody({}, QJsonArray{
                        providerCall("second-proposal", "document_propose", followup)})));
                }
            });
        runner.authorizePending(firstProposal, true);
        QVERIFY(recursiveAuthorization);
        QVERIFY(steered);
        QCOMPARE(ready.size(), 2);
        QCOMPARE(accepted.size(), 0); // The old action fact was retained before steering.
        QVERIFY(!runner.pendingProposal().isEmpty());
        QVERIFY(ready.last().at(1).toString() != firstProposal);
        runner.authorizePending(ready.last().at(1).toString(), false);
        QTRY_VERIFY_WITH_TIMEOUT(requested.size() >= 3, 1000);
        runner.receiveTurn(requested.last().at(0).toString(), parseProviderBody(providerBody(QStringLiteral("Finished."))));
        QVERIFY(!runner.isActive());
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
        QVERIFY(guidanceItem.value(QStringLiteral("sourcePath")).toString().startsWith(
            QStringLiteral("package:range_workflow/knowledge/core-v1.json#")));
        QCOMPARE(guidanceItem.value(QStringLiteral("version")).toString(), QStringLiteral("1.0.0"));
        QCOMPARE(guidanceItem.value(QStringLiteral("schemaHash")).toString().size(), 64);
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
