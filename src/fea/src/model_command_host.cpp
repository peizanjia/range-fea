#include "model_command_host.h"

#include "application.h"
#include "session.h"
#include "solver_manager.h"
#include <rmodel_command_session.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <map>

namespace
{
QString modelArtifact(QString path, QString *error = nullptr)
{
    QSet<QString> visited;
    try
    {
        for (int depth = 0; depth < 16; ++depth)
        {
            path = QFileInfo(path).canonicalFilePath();
            if (path.isEmpty() || visited.contains(path)) break;
            visited.insert(path);
            const QString extension = QFileInfo(path).suffix();
            const bool ascii = extension == RModel::getDefaultFileExtension(false);
            if (!ascii && extension != RModel::getDefaultFileExtension(true)) break;
            RFile file(path, ascii ? RFile::ASCII : RFile::BINARY);
            if (!file.open(ascii ? QIODevice::ReadOnly | QIODevice::Text : QIODevice::ReadOnly)) break;
            RFileHeader header;
            if (ascii) RFileIO::readAscii(file, header); else RFileIO::readBinary(file, header);
            if (header.getType() == R_FILE_TYPE_MODEL) return path;
            if (header.getType() != R_FILE_TYPE_LINK) break;
            path = RFileUtils::findLinkTargetFileName(path, header.getInformation());
        }
    }
    catch (const RError &failure)
    {
        if (error) *error = failure.getMessage();
        return {};
    }
    if (error) *error = QStringLiteral("Cannot resolve the complete native model artifact.");
    return {};
}

QJsonObject rejected(const QString &code, const QString &message)
{
    return {{"apiVersion", "range.application/v1"}, {"status", "rejected"}, {"code", code}, {"message", message}};
}

QJsonObject modalRequest(const QJsonObject &current, const RModalSetup &setup, bool dryRun)
{
    return {{"apiVersion", "range.application/v1"}, {"commandId", "analysis.modal.configure"}, {"commandVersion", 1},
        {"modelId", current["modelId"]}, {"expectedRevision", current["revision"]},
        {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"idempotencyKey", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"parameters", RModelCommandSession::modalParameters(setup)}, {"dryRun", dryRun}};
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) return false;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.contains(it.key())) return false;
    return true;
}

bool validToken(const QJsonValue &value, int maximum = 128)
{
    if (!value.isString()) return false;
    const QString token = value.toString();
    if (token.isEmpty() || token.size() > maximum) return false;
    static const QRegularExpression grammar(QStringLiteral("^[A-Za-z0-9._:-]+$"));
    return grammar.match(token).hasMatch();
}

bool unsignedInteger(const QJsonValue &value, quint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    // JSON numbers cannot exactly represent integers beyond this boundary.
    constexpr double maxExactJsonInteger = 9007199254740991.0;
    if (!std::isfinite(number) || number < 0.0 || number > maxExactJsonInteger || std::floor(number) != number)
        return false;
    if (result) *result = quint64(number);
    return true;
}

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject hostResponse(const QJsonValue &requestId, const QString &status, const QString &code,
                         const QJsonObject &result = {})
{
    QJsonObject response{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                         {QStringLiteral("requestId"), requestId},
                         {QStringLiteral("status"), status},
                         {QStringLiteral("code"), code}};
    if (!result.isEmpty()) response.insert(QStringLiteral("result"), result);
    return response;
}

QJsonObject hostRejected(const QJsonValue &requestId, const QString &code, const QString &message)
{
    QJsonObject response = hostResponse(requestId, QStringLiteral("rejected"), code);
    response.insert(QStringLiteral("message"), message);
    return response;
}

class TrySessionLock
{
public:
    explicit TrySessionLock(Session *session) : session(session), locked(session && session->trylock(0)) {}
    ~TrySessionLock() { if (locked) session->unlock(); }
    TrySessionLock(const TrySessionLock &) = delete;
    TrySessionLock &operator=(const TrySessionLock &) = delete;
    bool ownsLock() const { return locked; }
    void unlock() { if (locked) { session->unlock(); locked = false; } }
private:
    Session *session;
    bool locked;
};
}

struct ModelCommandHost::State
{
    struct Preview
    {
        QString id;
        QString documentId;
        QString sessionEpoch;
        QString command;
        QString nativeModelId;
        QString nativeRevision;
        QString nativeRequestId;
        QString nativeIdempotencyKey;
        QString digest;
        quint64 stampGeneration = 0;
        QJsonObject arguments;
        QJsonObject nativePreview;
        bool approved = false;
        bool consumed = false;
    };

    struct IdempotentResult
    {
        QString requestDigest;
        QString documentId;
        QJsonObject response;
    };

    struct Entry
    {
        quint64 generation = 0;
        std::unique_ptr<RModelCommandSession> editing;
    };
    std::map<QString, Entry> entries;
    std::map<QString, Preview> previews;
    std::map<QString, QJsonObject> operations;
    std::map<QString, IdempotentResult> idempotency;
    QList<QString> previewOrder;
    QList<QString> operationOrder;
    QList<QString> idempotencyOrder;
    QList<QString> knownDocuments;
    QList<QJsonObject> events;
    QMap<QString, quint64> lastDocumentRevisions;
    QMap<QString, quint64> selectionRevisions;
    QJsonObject lastSelection;
    QString sessionEpoch = uuid();
    QString activeOperationId;
    QJsonArray activeChangedPaths;
    quint64 nextEventSeq = 1;
    quint64 nextSelectionRevision = 0;
    QString journal;
    QString failure;

    QJsonObject appendEvent(const QString &type, const QString &documentId, const QJsonObject &fields = {})
    {
        QJsonObject event{{QStringLiteral("eventId"), uuid()},
                          {QStringLiteral("eventSeq"), double(nextEventSeq++)},
                          {QStringLiteral("type"), type},
                          {QStringLiteral("sessionEpoch"), sessionEpoch},
                          {QStringLiteral("documentId"), documentId}};
        for (auto it = fields.begin(); it != fields.end(); ++it) event.insert(it.key(), it.value());
        events.append(event);
        if (events.size() > 256) events.removeFirst();
        return event;
    }

    QJsonObject appendRevisionEvent(const QString &documentId, quint64 revision)
    {
        QJsonObject fields{{QStringLiteral("afterRevision"), double(revision)},
                           {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")},
                           {QStringLiteral("changedPaths"), activeChangedPaths}};
        if (lastDocumentRevisions.contains(documentId))
            fields.insert(QStringLiteral("beforeRevision"), double(lastDocumentRevisions.value(documentId)));
        else
            fields.insert(QStringLiteral("beforeRevision"), QJsonValue(QJsonValue::Null));
        if (!activeOperationId.isEmpty()) fields.insert(QStringLiteral("operationId"), activeOperationId);
        lastDocumentRevisions.insert(documentId, revision);
        return appendEvent(activeOperationId.isEmpty() ? QStringLiteral("document.revision_changed")
                                                       : QStringLiteral("document.changed"),
                           documentId, fields);
    }

    Entry *ensure(Session *session, uint id)
    {
        const auto stamp = session->modelCommandStamp(id);
        if (stamp.first.isEmpty()) { failure = QStringLiteral("The model has been closed."); return nullptr; }
        auto &entry = entries[stamp.first];
        if (entry.editing && entry.generation != stamp.second)
        {
            const bool recorded = entry.editing->recordExternalChange("external_edit", session->getModel(id), &failure);
            entry.editing.reset();
            if (!recorded) return nullptr;
        }
        if (!entry.editing)
        {
            RModelCommandSession::JournalOptions options;
            options.path = journal;
            options.actorId = QStringLiteral("gui-user");
            const QString source = modelArtifact(session->getModel(id).getFileName());
            QFile file(source);
            if (!source.isEmpty() && file.open(QIODevice::ReadOnly))
            {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (hash.addData(&file))
                {
                    options.sourcePath = QFileInfo(source).absoluteFilePath();
                    options.sourceSha256 = QString::fromLatin1(hash.result().toHex());
                }
            }
            entry.editing = std::make_unique<RModelCommandSession>(session->getModel(id), options);
            entry.generation = stamp.second;
        }
        failure.clear();
        return &entry;
    }
};

ModelCommandHost::ModelCommandHost(Session *session) : QObject(session), state(std::make_unique<State>()), session(session)
{
    const QDir directory(QDir(Application::instance()->getApplicationSettings()->getSessionDir()).filePath("command-audit"));
    QDir().mkpath(directory.absolutePath());
    state->journal = directory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json");
    for (uint id = 0; id < session->getNModels(); ++id)
    {
        const auto stamp = session->modelCommandStamp(id);
        state->knownDocuments.append(stamp.first);
        state->lastDocumentRevisions.insert(stamp.first, stamp.second);
    }
    connect(session, &Session::documentRevisionChanged, this, [this](const QString &documentId, quint64 revision) {
        emit eventPublished(state->appendRevisionEvent(documentId, revision));
    });
    connect(session, &Session::modelAdded, this, [this](uint modelID) {
        const auto stamp = this->session->modelCommandStamp(modelID);
        if (stamp.first.isEmpty()) return;
        if (modelID >= uint(state->knownDocuments.size())) state->knownDocuments.append(stamp.first);
        else state->knownDocuments.insert(int(modelID), stamp.first);
        state->lastDocumentRevisions.insert(stamp.first, stamp.second);
        emit eventPublished(state->appendEvent(QStringLiteral("document.opened"), stamp.first,
            {{QStringLiteral("afterRevision"), double(stamp.second)},
             {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")}}));
    });
    connect(session, &Session::modelRemoved, this, [this](uint modelID) {
        QString documentId;
        if (modelID < uint(state->knownDocuments.size())) documentId = state->knownDocuments.takeAt(int(modelID));
        if (!documentId.isEmpty())
            emit eventPublished(state->appendEvent(QStringLiteral("document.closed"), documentId));
        this->pruneClosedModels();
    });
    connect(session, &Session::modelSelectionChanged, this, [this](uint modelID) {
        const auto stamp = this->session->modelCommandStamp(modelID);
        if (stamp.first.isEmpty()) return;
        const quint64 revision = state->selectionRevisions.value(stamp.first) + 1;
        state->selectionRevisions.insert(stamp.first, revision);
        state->nextSelectionRevision = qMax(state->nextSelectionRevision, revision);
        emit eventPublished(state->appendEvent(QStringLiteral("selection.changed"), stamp.first,
            {{QStringLiteral("selectionRevision"), double(revision)},
             {QStringLiteral("documentRevision"), double(stamp.second)},
             {QStringLiteral("revisionScope"), QStringLiteral("selection-change-counter")}}));
    });
}

ModelCommandHost::~ModelCommandHost()
{
    // Session members may already have been destroyed; only owned snapshots
    // are safe to visit while QObject tears down its children.
    for (auto &[identity, entry] : state->entries)
        if (entry.editing && !entry.editing->close())
            RLogger::error("Model session %s closed with incomplete audit.\n", identity.toUtf8().constData());
}
QString ModelCommandHost::journalPath() const { return state->journal; }
QString ModelCommandHost::sessionEpoch() const { return state->sessionEpoch; }

bool ModelCommandHost::isBusy()
{
    return SolverManager::getInstance().getNRunning() || SolverManager::getInstance().getNWaiting()
        || RJobManager::getInstance().getNRunning() || RJobManager::getInstance().getNWaiting();
}

bool ModelCommandHost::isMutationBlocked()
{
    return RJobManager::getInstance().getNRunning() || RJobManager::getInstance().getNWaiting()
        || SolverManager::getInstance().hasLiveModelTasks();
}

void ModelCommandHost::pruneClosedModels()
{
    for (auto it = state->entries.begin(); it != state->entries.end();)
    {
        if (session->findModelByCommandIdentity(it->first) == RConstants::eod)
        {
            if (it->second.editing && !it->second.editing->close())
            {
                const auto message = tr("The model was closed, but its audit record could not be saved. The pending record will be retried on the next model command.");
                RLogger::error("%s\n", message.toUtf8().constData());
                emit commandRejected(message);
                ++it;
                continue;
            }
            it = state->entries.erase(it);
        }
        else ++it;
    }
}

QJsonObject ModelCommandHost::query(uint modelID)
{
    if (QThread::currentThread() != thread()) return rejected("wrong_thread", tr("Model commands require the GUI thread."));
    pruneClosedModels();
    if (isMutationBlocked()) return rejected("model_busy", tr("Wait for active model tasks to finish."));
    QMutexLocker guard(session->getMutex());
    auto *entry = state->ensure(session, modelID);
    if (!entry) return rejected("session_unavailable", state->failure);
    if (!entry->editing->setRunning(false)) return rejected("journal_unavailable", tr("Cannot record model availability."));
    auto result = entry->editing->query();
    result.insert("guiModelIdentity", session->modelCommandStamp(modelID).first);
    return result;
}

QJsonObject ModelCommandHost::execute(uint modelID, const QJsonObject &request)
{
    if (QThread::currentThread() != thread()) return rejected("wrong_thread", tr("Model commands require the GUI thread."));
    pruneClosedModels();
    if (isMutationBlocked()) return rejected("model_busy", tr("Wait for active model tasks to finish."));
    QJsonObject result;
    {
        QMutexLocker guard(session->getMutex());
        auto *entry = state->ensure(session, modelID);
        if (!entry) return rejected("session_unavailable", state->failure);
        if (!entry->editing->setRunning(false)) return rejected("journal_unavailable", tr("Cannot record model availability."));
        // GUI history is authoritative; callers use the existing Undo/Redo actions.
        if (request.value("commandId") != QJsonValue("analysis.modal.configure"))
            return rejected("unsupported_gui_command", tr("Use the GUI history actions for Undo and Redo."));
        const QString beforeRevision = entry->editing->query().value("revision").toString();
        result = entry->editing->execute(request);
        if (result.value("status") != QJsonValue("accepted")
            || entry->editing->query().value("revision").toString() == beforeRevision) return result;
        session->storeCurentModelVersion(modelID, tr("Configure modal analysis"));
        session->getModel(modelID).getProblemSetup().setModalSetup(entry->editing->model().getProblemSetup().getModalSetup());
        ++entry->generation; // The following notification increments the live stamp once.
    }
    session->setProblemChanged(modelID);
    return result;
}

QJsonObject ModelCommandHost::handle(const QJsonObject &request)
{
    const QJsonValue requestId = request.value(QStringLiteral("requestId"));
    auto invalid = [&](const QString &code, const QString &message) {
        return hostRejected(requestId, code, message);
    };
    if (QThread::currentThread() != thread())
        return invalid(QStringLiteral("WRONG_THREAD"), tr("GUI host requests must be handled on the GUI thread."));
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 65536
        || !exactKeys(request, {QStringLiteral("apiVersion"), QStringLiteral("requestId"),
                                QStringLiteral("operation"), QStringLiteral("parameters")})
        || request.value(QStringLiteral("apiVersion")) != QJsonValue(QStringLiteral("range.host/v1"))
        || !validToken(requestId) || !request.value(QStringLiteral("operation")).isString()
        || !request.value(QStringLiteral("parameters")).isObject())
        return invalid(QStringLiteral("INVALID_REQUEST"), tr("Expected the exact range.host/v1 request envelope."));

    const QString operation = request.value(QStringLiteral("operation")).toString();
    const QJsonObject parameters = request.value(QStringLiteral("parameters")).toObject();
    auto ok = [&](const QJsonObject &result, const QString &status = QStringLiteral("ok")) {
        return hostResponse(requestId, status, QStringLiteral("OK"), result);
    };
    auto targetFieldsValid = [&](QString *documentId, QString *sessionEpoch) {
        if (!parameters.value(QStringLiteral("target")).isObject()) return false;
        const auto target = parameters.value(QStringLiteral("target")).toObject();
        if (!exactKeys(target, {QStringLiteral("documentId"), QStringLiteral("sessionEpoch")})
            || !validToken(target.value(QStringLiteral("documentId")))
            || !validToken(target.value(QStringLiteral("sessionEpoch")))) return false;
        *documentId = target.value(QStringLiteral("documentId")).toString();
        *sessionEpoch = target.value(QStringLiteral("sessionEpoch")).toString();
        return true;
    };
    auto makeDocumentSnapshot = [&](const QString &documentId, uint modelID, bool liveRead,
                                    const QJsonObject &cachedState = QJsonObject{}) {
        const auto stamp = session->modelCommandStamp(modelID);
        QJsonObject snapshot{{QStringLiteral("documentId"), documentId},
                             {QStringLiteral("sessionEpoch"), state->sessionEpoch},
                             {QStringLiteral("documentRevision"), double(stamp.second)},
                             {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")},
                             {QStringLiteral("stampGeneration"), double(stamp.second)},
                             {QStringLiteral("stateAvailability"), liveRead ? QStringLiteral("live")
                                 : (cachedState.isEmpty() ? QStringLiteral("unavailable") : QStringLiteral("cached"))},
                             {QStringLiteral("stateFreshness"), liveRead ? QStringLiteral("locked-live")
                                 : (cachedState.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("last-known-only"))},
                             {QStringLiteral("draft"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")},
                                 {QStringLiteral("reason"), QStringLiteral("The GUI does not expose uncommitted form drafts to this host.")}}},
                             {QStringLiteral("unsaved"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")},
                                 {QStringLiteral("reason"), QStringLiteral("The host has no authoritative file-dirty state API.")}}}};
        QJsonObject modal;
        QString modelName;
        bool canConfigureModal = false;
        bool canUndo = false;
        bool canRedo = false;
        bool historyAvailable = liveRead;
        QString modalSessionRevision;
        if (liveRead)
        {
            const auto before = session->modelCommandStamp(modelID);
            const auto &model = session->getModel(modelID);
            modal = RModelCommandSession::modalParameters(model.getProblemSetup().getModalSetup());
            modelName = model.getName();
            canConfigureModal = bool(model.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL);
            canUndo = model.getUndoStackSize() > 0;
            canRedo = model.getRedoStackSize() > 0;
            const auto entry = state->entries.find(documentId);
            if (entry != state->entries.end() && entry->second.editing && entry->second.generation == stamp.second)
                modalSessionRevision = entry->second.editing->query().value(QStringLiteral("revision")).toString();
            const auto after = session->modelCommandStamp(modelID);
            if (before.first != documentId || before.second != after.second)
            {
                modal = {};
                modelName.clear();
                canConfigureModal = canUndo = canRedo = false;
                historyAvailable = false;
                snapshot[QStringLiteral("stateAvailability")] = QStringLiteral("unavailable");
                snapshot[QStringLiteral("stateFreshness")] = QStringLiteral("unknown");
            }
        }
        else if (!cachedState.isEmpty())
        {
            modal = cachedState.value(QStringLiteral("modal")).toObject();
            canConfigureModal = cachedState.value(QStringLiteral("canConfigureModal")).toBool();
            modalSessionRevision = cachedState.value(QStringLiteral("revision")).toString();
            historyAvailable = false;
        }
        if (!modalSessionRevision.isEmpty()) snapshot.insert(QStringLiteral("modalSessionRevision"), modalSessionRevision);
        if (!modelName.isEmpty()) snapshot.insert(QStringLiteral("modelName"), modelName);
        QJsonArray supported;
        if (canConfigureModal)
            supported.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.modal.configure")},
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("requiresPreview"), true},
                                         {QStringLiteral("requiresHostApproval"), true}});
        if (historyAvailable && canUndo)
            supported.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("document.undo")},
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("requiresPreview"), true},
                                         {QStringLiteral("requiresHostApproval"), true}});
        if (historyAvailable && canRedo)
            supported.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("document.redo")},
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("requiresPreview"), true},
                                         {QStringLiteral("requiresHostApproval"), true}});
        QJsonObject acceptedState{{QStringLiteral("scope"), QStringLiteral("analysis.modal")},
                                  {QStringLiteral("available"), !modal.isEmpty()},
                                  {QStringLiteral("supportedOperations"), supported}};
        if (!modal.isEmpty()) acceptedState.insert(QStringLiteral("modal"), modal);
        snapshot.insert(QStringLiteral("acceptedState"), acceptedState);
        snapshot.insert(QStringLiteral("history"), QJsonObject{
            {QStringLiteral("availability"), historyAvailable ? QStringLiteral("live") : QStringLiteral("unknown")},
            {QStringLiteral("canUndo"), historyAvailable ? QJsonValue(canUndo) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("canRedo"), historyAvailable ? QJsonValue(canRedo) : QJsonValue(QJsonValue::Null)}});
        const QJsonObject project = session->documentProjectContext(modelID);
        snapshot.insert(QStringLiteral("project"), project);
        if (liveRead)
        {
            if (project.value(QStringLiteral("draft")).isObject())
                snapshot.insert(QStringLiteral("draft"), project.value(QStringLiteral("draft")));
            if (project.value(QStringLiteral("unsaved")).isObject())
                snapshot.insert(QStringLiteral("unsaved"), project.value(QStringLiteral("unsaved")));
        }
        return snapshot;
    };

    if (operation == QStringLiteral("project.query"))
    {
        if (!parameters.isEmpty()) return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("project.query takes no parameters."));
        return ok(session->projectContext());
    }
    if (operation == QStringLiteral("document.project.query"))
    {
        QString documentId, epoch;
        if (!exactKeys(parameters, {QStringLiteral("target")}) || !targetFieldsValid(&documentId, &epoch))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A runtime document target is required."));
        if (epoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The document belongs to a different session."));
        const uint modelID = session->findModelByCommandIdentity(documentId);
        if (modelID == RConstants::eod)
            return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The document is no longer open."));
        return ok(session->documentProjectContext(modelID));
    }

    if (operation == QStringLiteral("capabilities.query"))
    {
        if (!parameters.isEmpty()) return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("capabilities.query takes an empty parameters object."));
        const QJsonArray operations{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("project.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.project.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("context.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("capabilities.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("events.read")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("command.preview")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("command.execute")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("operation.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.undo")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.redo")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}}};
        return ok({{QStringLiteral("sessionEpoch"), state->sessionEpoch},
                   {QStringLiteral("identityPersistence"), QStringLiteral("host-session")},
                   {QStringLiteral("capabilityCatalogVersion"), 1},
                   {QStringLiteral("supportedOperations"), operations},
                   {QStringLiteral("editingScope"), QJsonObject{
                        {QStringLiteral("version"), 1},
                        {QStringLiteral("models"), QStringLiteral("open native documents with a structural modal task")},
                        {QStringLiteral("elements"), QStringLiteral("configuration preserves existing elements; no element creation or numerical acceptance")},
                        {QStringLiteral("analyses"), QJsonArray{QStringLiteral("structural.modal.configuration")}},
                        {QStringLiteral("units"), QJsonObject{{QStringLiteral("modal.parameters"), QStringLiteral("1")}}},
                        {QStringLiteral("refusals"), QJsonArray{QStringLiteral("unknown command version or fields"),
                            QStringLiteral("non-modal analysis for modal configuration"), QStringLiteral("invalid parameter ranges or units"),
                            QStringLiteral("stale or closed document instance"), QStringLiteral("pending known drafts or project conflicts")}}}},
                   {QStringLiteral("project"), session->projectContext()}});
    }

    if (operation == QStringLiteral("events.read"))
    {
        if (!exactKeys(parameters, {QStringLiteral("afterEventSeq"), QStringLiteral("limit")}))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("events.read requires afterEventSeq and limit."));
        quint64 cursor = 0;
        quint64 limit = 0;
        if (!unsignedInteger(parameters.value(QStringLiteral("afterEventSeq")), &cursor)
            || !unsignedInteger(parameters.value(QStringLiteral("limit")), &limit)
            || limit < 1 || limit > 100)
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The event cursor must be non-negative and limit must be 1 through 100."));
        const quint64 latest = state->nextEventSeq - 1;
        if (cursor > latest) return invalid(QStringLiteral("INVALID_CURSOR"), tr("The event cursor is newer than the host event stream."));
        const quint64 earliest = state->events.isEmpty()
            ? state->nextEventSeq : quint64(state->events.constFirst().value(QStringLiteral("eventSeq")).toDouble());
        const bool resync = !state->events.isEmpty() && cursor + 1 < earliest;
        QJsonArray events;
        quint64 returned = 0;
        if (!resync)
        {
            for (const auto &event : state->events)
            {
                const quint64 seq = quint64(event.value(QStringLiteral("eventSeq")).toDouble());
                if (seq > cursor && returned < limit) { events.append(event); ++returned; }
            }
        }
        const quint64 lastReturned = returned ? quint64(events.last().toObject().value(QStringLiteral("eventSeq")).toDouble()) : cursor;
        return ok({{QStringLiteral("events"), events},
                   {QStringLiteral("earliestEventSeq"), double(earliest)},
                   {QStringLiteral("latestEventSeq"), double(latest)},
                   {QStringLiteral("resyncRequired"), resync},
                   {QStringLiteral("hasMore"), !resync && lastReturned < latest}});
    }

    if (operation == QStringLiteral("context.query"))
    {
        QString requestedDocument;
        QString requestedEpoch;
        const bool hasTarget = parameters.contains(QStringLiteral("target"));
        if ((hasTarget && (!exactKeys(parameters, {QStringLiteral("target")})
                           || !targetFieldsValid(&requestedDocument, &requestedEpoch)))
            || (!hasTarget && !parameters.isEmpty()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("context.query accepts only an optional fixed target."));
        if (hasTarget && requestedEpoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The target belongs to a different host session."));

        const bool taskBusy = isBusy();
        bool editingBlocked = isMutationBlocked();
        QList<uint> selectedModels;
        QList<SessionEntityID> selectedEntities;
        QStringList documentIds;
        bool selectionLive = false;
        if (!editingBlocked && session->trylock(0))
        {
            if (!isMutationBlocked())
            {
                selectionLive = true;
                if (hasTarget)
                {
                    const uint targetId = session->findModelByCommandIdentity(requestedDocument);
                    if (targetId == RConstants::eod) { session->unlock(); return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open.")); }
                    documentIds.append(requestedDocument);
                }
                else
                {
                    selectedModels = session->getSelectedModelIDs();
                    for (const uint id : selectedModels)
                    {
                        const auto stamp = session->modelCommandStamp(id);
                        if (stamp.first.isEmpty()) continue;
                        documentIds.append(stamp.first);
                        selectedEntities.append(session->getModel(id).getSelectedEntityIDs(id));
                    }
                }
                QJsonArray documentArray;
                for (const auto &id : documentIds) documentArray.append(id);
                QJsonArray entityArray;
                constexpr qsizetype maxSelectionEntities = 256;
                const qsizetype count = selectedEntities.size();
                for (qsizetype i = 0; i < qMin(count, maxSelectionEntities); ++i)
                {
                    const auto &entity = selectedEntities.at(i);
                    const auto stamp = session->modelCommandStamp(entity.getMid());
                    if (stamp.first.isEmpty()) continue;
                    entityArray.append(QJsonObject{{QStringLiteral("documentId"), stamp.first},
                                                   {QStringLiteral("groupType"), int(entity.getType())},
                                                   {QStringLiteral("entityId"), double(entity.getEid())}});
                }
                state->lastSelection = {{QStringLiteral("revision"), double(state->nextSelectionRevision)},
                                        {QStringLiteral("availability"), QStringLiteral("live")},
                                        {QStringLiteral("documents"), documentArray},
                                        {QStringLiteral("entities"), entityArray},
                                        {QStringLiteral("entityCount"), double(count)},
                                        {QStringLiteral("truncated"), count > maxSelectionEntities}};
            }
            else editingBlocked = true;
            if (!hasTarget && !editingBlocked)
            {
                // The selected document IDs were read while the Session lock was held.
            }
            session->unlock();
        }
        else if (!hasTarget)
        {
            editingBlocked = true;
        }
        if (!selectionLive)
        {
            if (!state->lastSelection.isEmpty())
            {
                state->lastSelection.insert(QStringLiteral("availability"), QStringLiteral("cached"));
            }
            else
            {
                state->lastSelection = {{QStringLiteral("revision"), double(state->nextSelectionRevision)},
                                        {QStringLiteral("availability"), QStringLiteral("unavailable")},
                                        {QStringLiteral("documents"), QJsonArray{}},
                                        {QStringLiteral("entities"), QJsonArray{}},
                                        {QStringLiteral("entityCount"), 0},
                                        {QStringLiteral("truncated"), false}};
            }
            if (!hasTarget)
            {
                for (const auto &value : state->lastSelection.value(QStringLiteral("documents")).toArray())
                    documentIds.append(value.toString());
            }
        }
        if (hasTarget && !documentIds.contains(requestedDocument)) documentIds.append(requestedDocument);

        QJsonArray documents;
        QSet<QString> emitted;
        for (const auto &documentId : documentIds)
        {
            if (documentId.isEmpty() || emitted.contains(documentId)) continue;
            emitted.insert(documentId);
            const uint modelID = session->findModelByCommandIdentity(documentId);
            if (modelID == RConstants::eod) continue;
            const auto stamp = session->modelCommandStamp(modelID);
            if (stamp.first != documentId) continue;
            if (selectionLive && !editingBlocked)
            {
                if (!session->trylock(0))
                {
                    documents.append(makeDocumentSnapshot(documentId, modelID, false));
                    continue;
                }
                if (isMutationBlocked())
                {
                    documents.append(makeDocumentSnapshot(documentId, modelID, false));
                    session->unlock();
                    continue;
                }
                const auto current = session->findModelByCommandIdentity(documentId);
                if (current == RConstants::eod) { session->unlock(); continue; }
                documents.append(makeDocumentSnapshot(documentId, current, true));
                session->unlock();
            }
            else
            {
                QJsonObject cached;
                const auto entry = state->entries.find(documentId);
                if (entry != state->entries.end() && entry->second.editing && entry->second.generation == stamp.second)
                    cached = entry->second.editing->query();
                documents.append(makeDocumentSnapshot(documentId, modelID, false, cached));
            }
        }
        return ok({{QStringLiteral("sessionEpoch"), state->sessionEpoch},
                   {QStringLiteral("identityPersistence"), QStringLiteral("host-session")},
                   {QStringLiteral("taskBusy"), taskBusy},
                   {QStringLiteral("editingBlocked"), editingBlocked},
                   {QStringLiteral("selection"), state->lastSelection},
                   {QStringLiteral("documents"), documents}});
    }

    if (operation == QStringLiteral("command.preview"))
    {
        if (!exactKeys(parameters, {QStringLiteral("target"), QStringLiteral("expectedDocumentRevision"),
                                    QStringLiteral("command"), QStringLiteral("commandVersion"),
                                    QStringLiteral("arguments")}))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("command.preview requires target, expectedDocumentRevision, command, commandVersion and arguments."));
        QString documentId;
        QString targetEpoch;
        quint64 expectedRevision = 0;
        if (!targetFieldsValid(&documentId, &targetEpoch)
            || targetEpoch != state->sessionEpoch
            || !unsignedInteger(parameters.value(QStringLiteral("expectedDocumentRevision")), &expectedRevision)
            || !parameters.value(QStringLiteral("command")).isString()
            || parameters.value(QStringLiteral("commandVersion")).toInt(-1) != 1
            || !parameters.value(QStringLiteral("arguments")).isObject())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The target and command version are malformed or belong to another host session."));
        const QString command = parameters.value(QStringLiteral("command")).toString();
        const auto arguments = parameters.value(QStringLiteral("arguments")).toObject();
        if (command != QStringLiteral("analysis.modal.configure")
            && command != QStringLiteral("document.undo") && command != QStringLiteral("document.redo"))
            return invalid(QStringLiteral("UNSUPPORTED_CAPABILITY"), tr("This host supports modal configuration and the existing document history only."));
        if ((command == QStringLiteral("analysis.modal.configure") && arguments.isEmpty())
            || ((command == QStringLiteral("document.undo") || command == QStringLiteral("document.redo")) && !arguments.isEmpty()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The command arguments do not match the selected operation."));
        if (isMutationBlocked()) return invalid(QStringLiteral("MODEL_BUSY"), tr("The target model is in use by a task that has not detached its model."));
        uint modelID = session->findModelByCommandIdentity(documentId);
        if (modelID == RConstants::eod) return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
        QString projectFailure;
        if (!session->canMutateDocument(modelID, &projectFailure))
            return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
        auto stamp = session->modelCommandStamp(modelID);
        if (stamp.first != documentId || stamp.second != expectedRevision)
            return invalid(QStringLiteral("STALE_REVISION"), tr("Query the current document revision and create a new preview."));

        QJsonObject nativeQuery;
        QJsonObject nativePreview;
        State::Preview preview;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The model could not be read without waiting for an active task."));
            modelID = session->findModelByCommandIdentity(documentId);
            if (modelID == RConstants::eod) return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            stamp = session->modelCommandStamp(modelID);
            if (stamp.first != documentId || stamp.second != expectedRevision)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed while the preview was being prepared."));
            auto *entry = state->ensure(session, modelID);
            if (!entry) return invalid(QStringLiteral("SESSION_UNAVAILABLE"), state->failure);
            if (!entry->editing->setRunning(false))
                return invalid(QStringLiteral("JOURNAL_UNAVAILABLE"), tr("Cannot record model availability."));
            nativeQuery = entry->editing->query();
            preview.id = uuid();
            preview.documentId = documentId;
            preview.sessionEpoch = state->sessionEpoch;
            preview.command = command;
            preview.stampGeneration = stamp.second;
            preview.arguments = arguments;
            preview.nativeModelId = nativeQuery.value(QStringLiteral("modelId")).toString();
            preview.nativeRevision = nativeQuery.value(QStringLiteral("revision")).toString();
            preview.nativeRequestId = uuid();
            preview.nativeIdempotencyKey = uuid();
            if (command == QStringLiteral("analysis.modal.configure"))
            {
                QJsonObject nativeRequest{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                    {QStringLiteral("commandId"), command}, {QStringLiteral("commandVersion"), 1},
                    {QStringLiteral("modelId"), nativeQuery.value(QStringLiteral("modelId"))},
                    {QStringLiteral("expectedRevision"), preview.nativeRevision},
                    {QStringLiteral("requestId"), preview.nativeRequestId},
                    {QStringLiteral("idempotencyKey"), preview.nativeIdempotencyKey},
                    {QStringLiteral("parameters"), arguments}, {QStringLiteral("dryRun"), true}};
                nativePreview = entry->editing->execute(nativeRequest);
                if (nativePreview.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("preview")))
                    return invalid(QStringLiteral("PREVIEW_REJECTED"), nativePreview.value(QStringLiteral("message")).toString());
                preview.arguments = nativePreview.value(QStringLiteral("modal")).toObject();
            }
            else
            {
                const auto &model = session->getModel(modelID);
                const bool available = command == QStringLiteral("document.undo")
                    ? model.getUndoStackSize() > 0 : model.getRedoStackSize() > 0;
                if (!available) return invalid(QStringLiteral("EMPTY_HISTORY"), tr("No matching operation is available in the document history."));
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("historyAction"), command == QStringLiteral("document.undo")
                        ? model.getUndoActionMessage() : model.getRedoActionMessage()},
                    {QStringLiteral("historyDepth"), double(command == QStringLiteral("document.undo")
                        ? model.getUndoStackSize() : model.getRedoStackSize())}};
            }
        }
        preview.nativePreview = nativePreview;
        QJsonObject digestInput{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("expectedDocumentRevision"), double(expectedRevision)},
            {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("arguments"), preview.arguments},
            {QStringLiteral("nativeRevision"), preview.nativeRevision}};
        preview.digest = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(digestInput).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        const QJsonObject before = command == QStringLiteral("analysis.modal.configure")
            ? nativeQuery.value(QStringLiteral("modal")).toObject()
            : QJsonObject{{QStringLiteral("historyAction"), nativePreview.value(QStringLiteral("historyAction"))},
                          {QStringLiteral("historyDepth"), nativePreview.value(QStringLiteral("historyDepth"))}};
        const QJsonObject after = command == QStringLiteral("analysis.modal.configure")
            ? nativePreview.value(QStringLiteral("modal")).toObject()
            : QJsonObject{{QStringLiteral("historyState"), QStringLiteral("computed_on_commit")}};
        const QJsonArray changedPaths{command == QStringLiteral("analysis.modal.configure")
            ? QStringLiteral("analysis.modal") : QStringLiteral("document.history")};
        QJsonObject result{{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                                                                   {QStringLiteral("sessionEpoch"), state->sessionEpoch}}},
            {QStringLiteral("expectedDocumentRevision"), double(expectedRevision)},
            {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("arguments"), preview.arguments}, {QStringLiteral("before"), before},
            {QStringLiteral("after"), after}, {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("readSet"), QJsonArray{command == QStringLiteral("analysis.modal.configure")
                ? QStringLiteral("analysis.modal") : QStringLiteral("document.history"), QStringLiteral("document.revision")}},
            {QStringLiteral("invalidatedResultsAvailability"), QStringLiteral("unknown_not_exposed")},
            {QStringLiteral("previewId"), preview.id}, {QStringLiteral("digest"), preview.digest},
            {QStringLiteral("nativeModalSessionRevision"), preview.nativeRevision}};
        if (state->previewOrder.size() >= 128)
        {
            const QString expired = state->previewOrder.takeFirst();
            state->previews.erase(expired);
        }
        state->previewOrder.append(preview.id);
        state->previews.emplace(preview.id, preview);
        return ok(result, QStringLiteral("preview"));
    }

    if (operation == QStringLiteral("operation.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("operationId")}) || !validToken(parameters.value(QStringLiteral("operationId"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("operation.query requires an operationId."));
        const auto found = state->operations.find(parameters.value(QStringLiteral("operationId")).toString());
        if (found == state->operations.end()) return invalid(QStringLiteral("OPERATION_NOT_FOUND"), tr("No in-memory operation with this ID is available."));
        return ok(found->second);
    }

    if (operation == QStringLiteral("command.execute") || operation == QStringLiteral("document.undo")
        || operation == QStringLiteral("document.redo"))
    {
        if (!exactKeys(parameters, {QStringLiteral("previewId"), QStringLiteral("idempotencyKey")})
            || !validToken(parameters.value(QStringLiteral("previewId")))
            || !validToken(parameters.value(QStringLiteral("idempotencyKey"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("Execution requires a previewId and idempotencyKey."));
        const QString previewId = parameters.value(QStringLiteral("previewId")).toString();
        const QString idempotencyKey = parameters.value(QStringLiteral("idempotencyKey")).toString();
        const QString requestDigest = QString::fromLatin1(QCryptographicHash::hash(
            (operation + QLatin1Char('\n') + previewId).toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto prior = state->idempotency.find(idempotencyKey);
        if (prior != state->idempotency.end())
        {
            if (prior->second.requestDigest != requestDigest)
                return invalid(QStringLiteral("IDEMPOTENCY_CONFLICT"), tr("This idempotencyKey is already bound to a different preview."));
            QJsonObject response = prior->second.response;
            response.insert(QStringLiteral("requestId"), requestId);
            return response;
        }
        auto stored = state->previews.find(previewId);
        if (stored == state->previews.end()) return invalid(QStringLiteral("PREVIEW_EXPIRED"), tr("The preview is no longer available in this host session."));
        State::Preview &preview = stored->second;
        const QString expectedCommand = operation == QStringLiteral("document.undo") ? QStringLiteral("document.undo")
            : (operation == QStringLiteral("document.redo") ? QStringLiteral("document.redo") : QString());
        if (!expectedCommand.isEmpty() && preview.command != expectedCommand)
            return invalid(QStringLiteral("PREVIEW_MISMATCH"), tr("The approved preview is for a different document operation."));
        if (!preview.approved) return invalid(QStringLiteral("AUTHORIZATION_REQUIRED"), tr("A trusted GUI caller must approve this preview first."));
        if (preview.consumed) return invalid(QStringLiteral("PREVIEW_ALREADY_EXECUTED"), tr("This preview has already been used."));
        if (preview.sessionEpoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The preview belongs to a different host session."));
        if (isMutationBlocked()) return invalid(QStringLiteral("MODEL_BUSY"), tr("The target model is in use by a task that has not detached its model."));
        uint modelID = session->findModelByCommandIdentity(preview.documentId);
        if (modelID == RConstants::eod) return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
        QString projectFailure;
        if (!session->canMutateDocument(modelID, &projectFailure))
            return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
        auto stamp = session->modelCommandStamp(modelID);
        if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
            return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview. Query and preview the command again."));

        const QString operationId = uuid();
        const QString previousActiveOperation = state->activeOperationId;
        const QJsonArray previousChangedPaths = state->activeChangedPaths;
        const QJsonArray changedPaths{preview.command == QStringLiteral("analysis.modal.configure")
            ? QStringLiteral("analysis.modal") : QStringLiteral("document.history")};
        QJsonObject nativeResult;
        bool auditRecorded = true;
        if (preview.command == QStringLiteral("analysis.modal.configure"))
        {
            QJsonObject nativeRequest{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                {QStringLiteral("commandId"), preview.command}, {QStringLiteral("commandVersion"), 1},
                {QStringLiteral("modelId"), preview.nativeModelId},
                {QStringLiteral("expectedRevision"), preview.nativeRevision},
                {QStringLiteral("requestId"), preview.nativeRequestId},
                {QStringLiteral("idempotencyKey"), preview.nativeIdempotencyKey},
                {QStringLiteral("parameters"), preview.arguments}, {QStringLiteral("dryRun"), false}};
            state->activeOperationId = operationId;
            state->activeChangedPaths = changedPaths;
            nativeResult = execute(modelID, nativeRequest);
            state->activeOperationId = previousActiveOperation;
            state->activeChangedPaths = previousChangedPaths;
            if (nativeResult.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("accepted")))
                return invalid(QStringLiteral("COMMAND_REJECTED"), nativeResult.value(QStringLiteral("message")).toString());
        }
        else
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The document history could not be changed safely."));
            modelID = session->findModelByCommandIdentity(preview.documentId);
            if (modelID == RConstants::eod) return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            stamp = session->modelCommandStamp(modelID);
            if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview."));
            auto &model = session->getModel(modelID);
            const bool undo = preview.command == QStringLiteral("document.undo");
            const uint depth = undo ? model.getUndoStackSize() : model.getRedoStackSize();
            const QString action = undo ? model.getUndoActionMessage() : model.getRedoActionMessage();
            if (depth == 0 || action != preview.nativePreview.value(QStringLiteral("historyAction")).toString())
                return invalid(QStringLiteral("STALE_PREVIEW"), tr("The selected GUI history item is no longer current."));
            state->activeOperationId = operationId;
            state->activeChangedPaths = changedPaths;
            session->getPickList().removeItems(modelID);
            if (undo) model.undo(0); else model.redo(0);
            guard.unlock();
            QString auditError;
            auditRecorded = recordExternalChange(modelID, undo ? QStringLiteral("undo") : QStringLiteral("redo"), &auditError);
            session->setModelChanged(modelID);
            state->activeOperationId = previousActiveOperation;
            state->activeChangedPaths = previousChangedPaths;
            nativeResult = {{QStringLiteral("status"), QStringLiteral("accepted")},
                            {QStringLiteral("historyAction"), action},
                            {QStringLiteral("auditRecorded"), auditRecorded}};
        }

        const auto afterStamp = session->modelCommandStamp(session->findModelByCommandIdentity(preview.documentId));
        QJsonObject operationResult{{QStringLiteral("operationId"), operationId},
            {QStringLiteral("documentId"), preview.documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("beforeRevision"), double(preview.stampGeneration)},
            {QStringLiteral("afterRevision"), double(afterStamp.second)},
            {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")},
            {QStringLiteral("command"), preview.command}, {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("eventSeq"), double(state->nextEventSeq - 1)},
            {QStringLiteral("persistence"), QStringLiteral("memory_only")},
            {QStringLiteral("fileSaveRequired"), true},
            {QStringLiteral("auditRecorded"), auditRecorded}};
        if (preview.command == QStringLiteral("analysis.modal.configure"))
            operationResult.insert(QStringLiteral("acceptedStateRevision"), nativeResult.value(QStringLiteral("revision")));
        else
            operationResult.insert(QStringLiteral("historyAction"), nativeResult.value(QStringLiteral("historyAction")));
        const QJsonObject response = hostResponse(requestId, QStringLiteral("accepted"), QStringLiteral("OK"), operationResult);
        preview.consumed = true;

        if (state->operationOrder.size() >= 256)
        {
            const QString expired = state->operationOrder.takeFirst();
            state->operations.erase(expired);
        }
        state->operationOrder.append(operationId);
        state->operations.emplace(operationId, operationResult);
        if (state->idempotencyOrder.size() >= 256)
        {
            const QString expired = state->idempotencyOrder.takeFirst();
            state->idempotency.erase(expired);
        }
        state->idempotencyOrder.append(idempotencyKey);
        state->idempotency.emplace(idempotencyKey, State::IdempotentResult{requestDigest, preview.documentId, response});
        return response;
    }

    return invalid(QStringLiteral("UNSUPPORTED_OPERATION"), tr("This range.host/v1 operation is not implemented by the GUI host."));
}

bool ModelCommandHost::approvePreview(const QString &previewId)
{
    if (QThread::currentThread() != thread() || isMutationBlocked()) return false;
    auto found = state->previews.find(previewId);
    if (found == state->previews.end() || found->second.approved || found->second.consumed
        || found->second.sessionEpoch != state->sessionEpoch) return false;
    uint modelID = session->findModelByCommandIdentity(found->second.documentId);
    if (modelID == RConstants::eod) return false;
    TrySessionLock guard(session);
    if (!guard.ownsLock() || isMutationBlocked()) return false;
    modelID = session->findModelByCommandIdentity(found->second.documentId);
    if (modelID == RConstants::eod) return false;
    if (!session->canMutateDocument(modelID)) return false;
    const auto stamp = session->modelCommandStamp(modelID);
    if (stamp.first != found->second.documentId || stamp.second != found->second.stampGeneration) return false;
    if (found->second.command == QStringLiteral("analysis.modal.configure"))
    {
        const auto entry = state->entries.find(found->second.documentId);
        if (entry == state->entries.end() || !entry->second.editing
            || entry->second.generation != stamp.second
            || entry->second.editing->query().value(QStringLiteral("revision")).toString() != found->second.nativeRevision)
            return false;
    }
    else
    {
        const auto &model = session->getModel(modelID);
        const bool undo = found->second.command == QStringLiteral("document.undo");
        const uint depth = undo ? model.getUndoStackSize() : model.getRedoStackSize();
        const QString action = undo ? model.getUndoActionMessage() : model.getRedoActionMessage();
        if (depth == 0 || action != found->second.nativePreview.value(QStringLiteral("historyAction")).toString()) return false;
    }
    found->second.approved = true;
    return true;
}

bool ModelCommandHost::configureModal(const QList<uint> &modelIDs, const RModalSetup &setup, QString *error)
{
    auto fail = [&](const QString &message) { if (error) *error = message; emit commandRejected(message); return false; };
    if (QThread::currentThread() != thread()) return fail(tr("Model commands require the GUI thread."));
    if (isMutationBlocked()) return fail(tr("Wait for active model tasks to finish before changing modal settings."));
    if (modelIDs.isEmpty()) return fail(tr("No model is selected."));
    for (const uint id : modelIDs)
    {
        QString projectFailure;
        if (!session->canMutateDocument(id, &projectFailure)) return fail(projectFailure);
    }
    QList<QJsonObject> commands;
    QList<State::Entry *> entries;
    {
        QMutexLocker guard(session->getMutex());
        QSet<uint> unique;
        // Validate every candidate without publishing any live model change.
        for (const uint id : modelIDs)
        {
            if (unique.contains(id)) return fail(tr("A model was selected more than once."));
            unique.insert(id);
            auto *entry = state->ensure(session, id);
            if (!entry) return fail(state->failure);
            if (!entry->editing->setRunning(false)) return fail(tr("Cannot record model availability."));
            auto command = modalRequest(entry->editing->query(), setup, true);
            const auto preview = entry->editing->execute(command);
            if (preview.value("status") != QJsonValue("preview")) return fail(preview.value("message").toString());
            command["dryRun"] = false;
            commands.append(command);
            entries.append(entry);
        }
        for (qsizetype i = 0; i < entries.size(); ++i)
        {
            const auto result = entries[i]->editing->execute(commands[i]);
            if (result.value("status") != QJsonValue("accepted"))
            {
                // Accepted native candidates remain unpublished. Rotate them so
                // a subsequent request cannot observe a partially accepted batch.
                for (const uint id : modelIDs) session->invalidateModelCommands(id);
                return fail(result.value("message").toString());
            }
        }
        for (qsizetype i = 0; i < modelIDs.size(); ++i)
        {
            session->storeCurentModelVersion(modelIDs[i], tr("Configure modal analysis"));
            session->getModel(modelIDs[i]).getProblemSetup().setModalSetup(entries[i]->editing->model().getProblemSetup().getModalSetup());
            ++entries[i]->generation;
        }
    }
    for (qsizetype i = 0; i < modelIDs.size(); ++i)
    {
        session->setProblemChanged(modelIDs[i]);
    }
    return true;
}

bool ModelCommandHost::recordArtifactPublished(uint modelID, const QString &path, QString *error)
{
    if (QThread::currentThread() != thread()) { if (error) *error = tr("Publication requires the GUI thread."); return false; }
    QMutexLocker guard(session->getMutex());
    const QString artifact = modelArtifact(path, error);
    if (artifact.isEmpty()) return false;
    const auto stamp = session->modelCommandStamp(modelID);
    auto found = state->entries.find(stamp.first);
    State::Entry *entry = nullptr;
    // A save advances the GUI stamp once, but the saved bytes still belong to
    // the accepted editing revision. Preserve that association in the journal.
    if (found != state->entries.end() && found->second.editing && found->second.generation + 1 == stamp.second)
        entry = &found->second;
    else entry = state->ensure(session, modelID);
    if (!entry) { if (error) *error = state->failure; return false; }
    if (!entry->editing->recordArtifactPublished(artifact, nullptr, error)) return false;
    const QString visiblePath = QFileInfo(path).canonicalFilePath();
    if (visiblePath != artifact && !entry->editing->recordArtifactPublished(visiblePath, nullptr, error)) return false;
    const bool closed = entry->editing->close();
    entry->editing.reset();
    if (!closed && error) *error = tr("The model was saved and journaled, but its editing session could not be closed in the journal.");
    return closed;
}

bool ModelCommandHost::recordExternalChange(uint modelID, const QString &reason, QString *error)
{
    if (QThread::currentThread() != thread()) { if (error) *error = tr("External changes require the GUI thread."); return false; }
    QString failure;
    bool recorded = true;
    {
        QMutexLocker guard(session->getMutex());
        session->invalidateModelCommands(modelID);
        const auto stamp = session->modelCommandStamp(modelID);
        auto found = state->entries.find(stamp.first);
        if (found != state->entries.end() && found->second.editing)
        {
            recorded = found->second.editing->recordExternalChange(reason, session->getModel(modelID), &failure);
            found->second.editing.reset();
        }
    }
    if (!recorded)
    {
        const QString message = tr("The model changed, but its audit record could not be saved: %1").arg(failure);
        if (error) *error = message;
        RLogger::error("%s\n", message.toUtf8().constData());
        emit commandRejected(message);
    }
    return recorded;
}
