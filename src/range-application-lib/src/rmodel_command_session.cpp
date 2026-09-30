#include "rmodel_command_session.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <cmath>

struct RModelCommandSession::State
{
    explicit State(const RModel &source) : model(source) {}
    RModel model;
    quint64 generation = 0;
    QVector<RModalSetup> undo;
    QVector<RModalSetup> redo;
    QMap<QString, QPair<QJsonObject, QJsonObject>> ledger;
};

namespace {
bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const auto keys = object.keys();
    return QSet<QString>(keys.begin(), keys.end()) == expected;
}
bool token(const QJsonValue &value)
{
    return value.isString() && !value.toString().trimmed().isEmpty()
        && value.toString().size() <= 128;
}
bool integer(const QJsonValue &value, double minimum, double maximum)
{
    const auto number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum;
}
}

RModelCommandSession::RModelCommandSession(const RModel &model)
    : state(std::make_unique<State>(model)),
      identity(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      ownerThread(QThread::currentThreadId())
{}

RModelCommandSession::~RModelCommandSession() = default;
const RModel &RModelCommandSession::model() const { return state->model; }
QString RModelCommandSession::revision() const
{
    return identity + ":" + QString::number(state->generation);
}

QJsonObject RModelCommandSession::modalParameters(const RModalSetup &setup)
{
    return {{"method", setup.getMethod() == R_MODAL_MULTIPLE_MODES ? "multiple" : "dominant"},
            {"iterations", static_cast<int>(setup.getNIterations())},
            {"modes", static_cast<int>(setup.getNModesToExtract())},
            {"tolerance", setup.getConvergenceValue()}, {"unit", "1"}};
}

QJsonObject RModelCommandSession::query() const
{
    const bool supported = state->model.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL;
    return {{"apiVersion", "range.application/v1"}, {"modelId", identity},
            {"revision", revision()}, {"revisionScope", "owned-session"},
            {"modal", modalParameters(state->model.getProblemSetup().getModalSetup())},
            {"canConfigureModal", supported}, {"canUndo", !state->undo.isEmpty()},
            {"canRedo", !state->redo.isEmpty()},
            {"remainingCommands", MaxCommands - state->ledger.size()}};
}

QJsonObject RModelCommandSession::execute(const QJsonObject &request)
{
    auto rejected = [&](const QString &code, const QString &message) {
        return QJsonObject{{"apiVersion", "range.application/v1"}, {"status", "rejected"},
            {"requestId", request.value("requestId")}, {"modelId", identity},
            {"revision", revision()}, {"code", code}, {"message", message}};
    };
    if (QThread::currentThreadId() != ownerThread)
        return rejected("wrong_thread", "Use this editing session only on its owning thread.");
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 65536)
        return rejected("invalid_request", "Request exceeds 64 KiB.");
    if (!exactKeys(request, {"apiVersion", "commandId", "commandVersion", "modelId",
            "expectedRevision", "requestId", "idempotencyKey", "parameters", "dryRun"})
        || request.value("apiVersion") != QJsonValue("range.application/v1")
        || !integer(request.value("commandVersion"), 1, 1)
        || !token(request.value("requestId")) || !token(request.value("idempotencyKey"))
        || !token(request.value("expectedRevision")) || !token(request.value("modelId"))
        || !request.value("commandId").isString() || !request.value("parameters").isObject()
        || !request.value("dryRun").isBool())
        return rejected("invalid_request", "Expected the exact version 1 command envelope.");
    if (request.value("modelId").toString() != identity)
        return rejected("wrong_model", "The model identity belongs to another editing session.");

    const QString key = request.value("idempotencyKey").toString();
    const auto existing = state->ledger.constFind(key);
    if (existing != state->ledger.cend())
    {
        if (existing->first != request)
            return rejected("idempotency_conflict", "This key was used for a different request.");
        if (existing->second.value("revision").toString() != revision())
            return rejected("stale_revision", "The previously accepted result is no longer current.");
        return existing->second;
    }
    if (request.value("expectedRevision").toString() != revision())
        return rejected("stale_revision", "Query the current revision before submitting a command.");
    const QString command = request.value("commandId").toString();
    const auto parameters = request.value("parameters").toObject();
    RModalSetup proposed = state->model.getProblemSetup().getModalSetup();
    if (command == "analysis.modal.configure")
    {
        if (!(state->model.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL))
            return rejected("unsupported_analysis", "The model has no structural modal task.");
        if (!exactKeys(parameters, {"method", "iterations", "modes", "tolerance", "unit"})
            || (parameters.value("method") != QJsonValue("dominant")
                && parameters.value("method") != QJsonValue("multiple"))
            || parameters.value("unit") != QJsonValue("1")
            || !integer(parameters.value("iterations"), R_MODAL_ITERATIONS_MIN_NUMBER, R_MODAL_ITERATIONS_MAX_NUMBER)
            || !integer(parameters.value("modes"), R_MODAL_MODES_MIN_NUMBER, R_MODAL_MODES_MAX_NUMBER)
            || !parameters.value("tolerance").isDouble()
            || !std::isfinite(parameters.value("tolerance").toDouble())
            || parameters.value("tolerance").toDouble() <= 0.0
            || parameters.value("tolerance").toDouble() > R_MODAL_CONVERGENCE_MAX_VALUE)
            return rejected("invalid_parameters", "Invalid modal settings or dimensionless unit.");
        proposed.setMethod(parameters.value("method").toString() == "multiple" ? R_MODAL_MULTIPLE_MODES : R_MODAL_DOMINANT_MODE);
        proposed.setNIterations(parameters.value("iterations").toInt());
        proposed.setNModesToExtract(parameters.value("modes").toInt());
        proposed.setConvergenceValue(parameters.value("tolerance").toDouble());
    }
    else if (command == "model.undo" || command == "model.redo")
    {
        if (!parameters.isEmpty())
            return rejected("invalid_parameters", "Undo and redo take empty parameters.");
        const auto &history = command == "model.undo" ? state->undo : state->redo;
        if (history.isEmpty())
            return rejected("empty_history", "No accepted edit is available.");
        proposed = history.last();
    }
    else
        return rejected("unknown_command", "This command is not supported by the editing session.");

    QJsonObject response{{"apiVersion", "range.application/v1"}, {"status", "preview"},
        {"requestId", request.value("requestId")}, {"modelId", identity},
        {"preRevision", revision()}, {"revision", revision()},
        {"modal", modalParameters(proposed)}};
    if (request.value("dryRun").toBool()) return response;
    if (state->ledger.size() >= MaxCommands)
        return rejected("session_limit", "Publish or discard this session before opening a new one.");

    // Allocate/copy the complete candidate (including ledger/history) before
    // publication. No mutation of accepted state occurs until the final swap.
    auto candidate = std::make_unique<State>(*state);
    const auto previous = candidate->model.getProblemSetup().getModalSetup();
    if (command == "model.undo")
    {
        candidate->undo.removeLast();
        candidate->redo.append(previous);
    }
    else
    {
        candidate->undo.append(previous);
        if (command == "model.redo") candidate->redo.removeLast();
        else candidate->redo.clear();
    }
    candidate->model.getProblemSetup().setModalSetup(proposed);
    ++candidate->generation;
    response["status"] = "accepted";
    response["revision"] = identity + ":" + QString::number(candidate->generation);
    candidate->ledger.insert(key, {request, response});
    state.swap(candidate);
    return response;
}
