#include "model_command_host.h"

#include "application.h"
#include "model_io.h"
#include "session.h"
#include "solver_manager.h"
#include <rmodel_command_service.h>
#include <rmodel_command_session.h>
#include <rproject_catalog.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <QCryptographicHash>
#include <QApplication>
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
#include <limits>
#include <map>

namespace
{
QString durableOperationPhase(RProjectCatalog::OperationPhase phase)
{
    switch (phase)
    {
        case RProjectCatalog::OperationPhase::Intent: return QStringLiteral("intent");
        case RProjectCatalog::OperationPhase::Accepted: return QStringLiteral("accepted");
        case RProjectCatalog::OperationPhase::ArtifactPublished: return QStringLiteral("artifact_published");
        case RProjectCatalog::OperationPhase::Observed: return QStringLiteral("observed");
        case RProjectCatalog::OperationPhase::Conflict: return QStringLiteral("conflict");
        case RProjectCatalog::OperationPhase::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QJsonObject durableOperationJson(const RProjectCatalog::DurableOperation &operation)
{
    return {{"projectId", operation.projectId}, {"operationId", operation.operationId},
            {"phase", durableOperationPhase(operation.phase)}, {"persistence", "durable"},
            {"normalizedRequestSha256", operation.normalizedRequestSha256},
            {"acceptedRevision", operation.association.acceptedRevision},
            {"acceptedModelSha256", operation.association.acceptedModelSha256},
            {"diagnosticCode", operation.association.diagnosticCode},
            {"observation", operation.association.observation}};
}

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

bool isCoreEditCommand(const QString &command)
{
    return command == QStringLiteral("analysis.modal.configure")
        || command == QStringLiteral("analysis.structural_dynamics.configure")
        || command == QStringLiteral("entity.material.set")
        || command == QStringLiteral("entity.boundary.set");
}

RElementGroup *mutableElementGroup(RModel &model, uint physicalId)
{
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    if (!model.findElementGroupByPhysicalId(physicalId, &type, &index)) return nullptr;
    switch (type)
    {
        case R_ENTITY_GROUP_POINT: return &model.getPoint(index);
        case R_ENTITY_GROUP_LINE: return &model.getLine(index);
        case R_ENTITY_GROUP_SURFACE: return &model.getSurface(index);
        case R_ENTITY_GROUP_VOLUME: return &model.getVolume(index);
        default: return nullptr;
    }
}

const RElementGroup *constElementGroup(const RModel &model, uint physicalId)
{
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    if (!model.findElementGroupByPhysicalId(physicalId, &type, &index)) return nullptr;
    switch (type)
    {
        case R_ENTITY_GROUP_POINT: return &model.getPoint(index);
        case R_ENTITY_GROUP_LINE: return &model.getLine(index);
        case R_ENTITY_GROUP_SURFACE: return &model.getSurface(index);
        case R_ENTITY_GROUP_VOLUME: return &model.getVolume(index);
        default: return nullptr;
    }
}

bool commitSupportedCandidate(RModel &liveModel, const RModel &candidate,
                             const RModelCommandService::Delta &delta,
                             QString *error = nullptr)
{
    switch (delta.kind)
    {
        case RModelCommandService::Delta::Kind::Modal:
            liveModel.getProblemSetup().setModalSetup(
                candidate.getProblemSetup().getModalSetup());
            return true;
        case RModelCommandService::Delta::Kind::StructuralDynamics:
            liveModel.getProblemSetup().setStructuralDynamicsSetup(
                candidate.getProblemSetup().getStructuralDynamicsSetup());
            liveModel.setTimeSolver(candidate.getTimeSolver());
            return true;
        case RModelCommandService::Delta::Kind::Material:
        case RModelCommandService::Delta::Kind::Boundary:
        {
            RElementGroup *liveGroup = mutableElementGroup(liveModel, delta.physicalId);
            const RElementGroup *candidateGroup = constElementGroup(candidate, delta.physicalId);
            if (!liveGroup || !candidateGroup)
            {
                if (error) *error = QObject::tr("The approved physical target no longer exists.");
                return false;
            }
            if (delta.kind == RModelCommandService::Delta::Kind::Material)
            {
                liveGroup->setMaterial(candidateGroup->getMaterial());
                return true;
            }
            const bool candidateHas = candidateGroup->hasBoundaryCondition(delta.boundaryType);
            if (candidateHas)
                liveGroup->addBoundaryCondition(candidateGroup->getBoundaryCondition(delta.boundaryType));
            else if (liveGroup->hasBoundaryCondition(delta.boundaryType))
                liveGroup->removeBoundaryCondition(delta.boundaryType);
            return true;
        }
        case RModelCommandService::Delta::Kind::None:
            break;
    }
    if (error) *error = QObject::tr("The approved command did not contain a supported model change.");
    return false;
}

bool commitSupportedDelta(RModel &liveModel, const RModelCommandService::Delta &delta,
                          QString *error = nullptr)
{
    const auto evaluated = RModelCommandService::applyDelta(liveModel, delta, true);
    if (!evaluated.valid)
    {
        if (error) *error = evaluated.message;
        return false;
    }
    return commitSupportedCandidate(liveModel, evaluated.candidate, delta, error);
}

bool activeModalInteraction()
{
    return QApplication::activeModalWidget() != nullptr;
}

QJsonObject sourceStampJson(const QPair<QString, quint64> &stamp, const QString &sessionEpoch)
{
    return {{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("revision"), QString::number(stamp.second)},
            {QStringLiteral("sessionEpoch"), sessionEpoch}};
}

QJsonObject nativeSourceBindingJson(const RProjectCatalog::SourceBinding &binding)
{
    QJsonArray artifacts;
    for (const auto &artifact : binding.sourceArtifacts) artifacts.append(artifact.toJson());
    return {{QStringLiteral("logicalSourcePath"), binding.logicalSourcePath},
            {QStringLiteral("resolvedSourcePath"), binding.resolvedSourcePath},
            {QStringLiteral("savedArtifactHash"), binding.savedArtifactHash},
            {QStringLiteral("sourceArtifacts"), artifacts}};
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
        QString nativeRevision;
        QString digest;
        quint64 stampGeneration = 0;
        QJsonObject arguments;
        QJsonObject nativePreview;
        QJsonArray readSet;
        RModelCommandService::Delta delta;
        bool hasSupportedDelta = false;
        QString inputFingerprint;
        QString readSetHash;
        QString jobId;
        QString jobSourceDocumentId;
        QString jobSourceRevision;
        QJsonObject projectSnapshotRef;
        QJsonObject validation;
        QJsonObject recoveryQualification;
        bool approved = false;
        bool consumed = false;
    };

    struct JobSource
    {
        QString documentId;
        QString sessionEpoch;
        QString revision;
        QJsonObject projectSnapshotRef;
    };

    struct ResultDocument
    {
        QString documentId;
        QString artifactPath;
        QString artifactSha256;
        quint64 openedAtRevision = 0;
        bool normalizedAtOpen = false;
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
        QString closeRetryId, closeOperationId;
        QJsonObject closeRetryPayload;
    };
    std::map<QString, Entry> entries;
    std::map<QString, Preview> previews;
    std::map<QString, QJsonObject> operations;
    std::map<QString, IdempotentResult> idempotency;
    std::map<QString, JobSource> jobSources;
    std::map<QString, ResultDocument> resultDocuments;
    std::map<QString, QJsonObject> resultDescriptors;
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
            entry.closeRetryId.clear();
            entry.closeOperationId.clear();
            entry.closeRetryPayload = {};
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
    SolverManager &manager = SolverManager::getInstance();
    if (RJobManager::getInstance().getNRunning() || RJobManager::getInstance().getNWaiting()
        || manager.hasLiveModelTasks()) return true;

    // Native jobs own frozen source snapshots, so they do not hold the live
    // Session models. Keep legacy SolverManager jobs blocked, while allowing
    // independent edits and reads to proceed during detached native runs.
    const int managerJobs = int(manager.getNRunning() + manager.getNWaiting());
    if (managerJobs == 0) return false;
    int registeredNativeJobs = 0;
    int offset = 0;
    while (true)
    {
        const QJsonObject page = manager.jobsQuery(offset, 200);
        if (!page.value(QStringLiteral("error")).toString().isEmpty()) return true;
        const QJsonArray jobs = page.value(QStringLiteral("jobs")).toArray();
        for (const QJsonValue &value : jobs)
        {
            const QJsonObject job = value.toObject();
            const QString state = job.value(QStringLiteral("state")).toString();
            if (state != QStringLiteral("queued") && state != QStringLiteral("running")
                && state != QStringLiteral("cancelling")) continue;
            const QJsonObject sourceStamp = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
            const QString snapshotPath = job.value(QStringLiteral("inputSnapshotRef")).toString();
            const QString snapshotHash = job.value(QStringLiteral("inputSnapshotSha256")).toString();
            const QString packagePath = job.value(QStringLiteral("inputPackageRef")).toString();
            const QString packageHash = job.value(QStringLiteral("inputPackageSha256")).toString();
            static const QRegularExpression digestGrammar(QStringLiteral("^[0-9a-f]{64}$"));
            const bool frozenInput = !sourceStamp.value(QStringLiteral("documentId")).toString().isEmpty()
                && !sourceStamp.value(QStringLiteral("revision")).toString().isEmpty()
                && QFileInfo(snapshotPath).isFile() && digestGrammar.match(snapshotHash).hasMatch()
                && QFileInfo(packagePath).isDir() && digestGrammar.match(packageHash).hasMatch();
            if (frozenInput) ++registeredNativeJobs;
        }
        offset += jobs.size();
        if (jobs.isEmpty() || offset >= page.value(QStringLiteral("total")).toInt()) break;
    }
    return managerJobs > registeredNativeJobs;
}

void ModelCommandHost::pruneClosedModels()
{
    for (auto it = state->entries.begin(); it != state->entries.end();)
    {
        if (session->findModelByCommandIdentity(it->first) == RConstants::eod)
        {
            if (it->second.editing && !closeEditingDurably(it->first))
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

bool ModelCommandHost::closeEditingDurably(const QString &documentId, QString *error,
                                          const RProjectCatalog::PublicationLeasePtr &publicationLease)
{
    auto found = state->entries.find(documentId);
    if (found == state->entries.end() || !found->second.editing) return true;
    auto &entry = found->second;
    const bool alreadyClosed = entry.editing->query().value("lifecycle") == QJsonValue("closed");
    if (alreadyClosed && entry.closeRetryPayload.isEmpty()) return true;
    auto *catalog = session->getProjectCatalog();
    if (!catalog)
    {
        if (error) *error = tr("The project ledger is unavailable; the close record remains pending.");
        return false;
    }
    if (entry.closeRetryPayload.isEmpty())
    {
        if (entry.closeRetryId.isEmpty()) entry.closeRetryId = uuid();
        auto intent = catalog->beginOperation(entry.closeRetryId,
            {{"command", "model.session.close"}, {"documentId", documentId},
             {"editingIdentity", entry.editing->query().value("modelId")}}, {}, publicationLease, error);
        if (intent.operation.operationId.isEmpty()) return false;
        entry.closeOperationId = intent.operation.operationId;
        const auto payload = entry.editing->prepareCloseRetry(entry.closeRetryId, entry.closeOperationId, error);
        if (payload.isEmpty() || !catalog->queueCloseRetry(entry.closeRetryId, entry.closeOperationId,
                "close_record_pending", payload, publicationLease, error)) return false;
        entry.closeRetryPayload = payload;
        if (!catalog->advanceOperation(entry.closeOperationId, RProjectCatalog::OperationPhase::Accepted,
                                        {}, publicationLease, error)) return false;
    }
    if (alreadyClosed ? !RModelCommandSession::replayCloseRetry(entry.closeRetryPayload, error)
                      : !entry.editing->closeWithRetryPayload(entry.closeRetryPayload, error)) return false;
    RProjectCatalog::OperationAssociation association;
    association.observation = {{"status", "ok"}, {"code", "OK"},
        {"result", QJsonObject{{"operationId", entry.closeOperationId},
            {"command", "model.session.close"},
            {"editingSessionId", entry.closeRetryPayload.value("record").toObject().value("sessionId")},
            {"recordId", entry.closeRetryPayload.value("record").toObject().value("recordId")},
            {"persistence", "durable"}}}};
    if (!catalog->advanceOperation(entry.closeOperationId, RProjectCatalog::OperationPhase::Observed,
                                  association, publicationLease, error)) return false;
    return catalog->completeCloseRetry(entry.closeRetryId, publicationLease, error);
}

bool ModelCommandHost::closeDurableSessions(QString *error)
{
    bool complete = true;
    for (const auto &[identity, entry] : state->entries)
        if (entry.editing && !closeEditingDurably(identity, error)) complete = false;
    return complete;
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
    if (isMutationBlocked() || activeModalInteraction())
        return rejected("model_busy", tr("Wait for active document interaction and model tasks to finish."));
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
    QString durableIntentId;
    bool durableSideEffect = false;
    auto invalid = [&](const QString &code, const QString &message) {
        auto response = hostRejected(requestId, code, message);
        if (!durableIntentId.isEmpty())
        {
            auto *catalog = session->getProjectCatalog();
            RProjectCatalog::OperationAssociation association;
            association.diagnosticCode = code.toLower();
            association.observation = {{"status", "rejected"}, {"code", code}, {"result", QJsonObject{}}, {"message", message}};
            QString error;
            if (!catalog || !catalog->advanceOperation(durableIntentId, durableSideEffect
                    ? RProjectCatalog::OperationPhase::Unknown : RProjectCatalog::OperationPhase::Observed,
                    association, {}, &error))
                response.insert("durableObservation", "unavailable");
        }
        return response;
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
    auto describeResultView = [&](const QString &documentId, const QPair<QString, quint64> &stamp) {
        const auto found = state->resultDescriptors.find(documentId);
        if (found == state->resultDescriptors.end()) return QJsonObject{};
        QJsonObject descriptor = found->second;
        const QJsonObject openedStamp = descriptor.value(QStringLiteral("openedAtRuntimeStamp")).toObject();
        const bool modified = openedStamp.value(QStringLiteral("documentId")).toString() != stamp.first
            || openedStamp.value(QStringLiteral("sessionEpoch")).toString() != state->sessionEpoch
            || openedStamp.value(QStringLiteral("revision")).toString() != QString::number(stamp.second);
        const bool normalized = descriptor.value(QStringLiteral("normalizedAtOpen")).toBool();
        descriptor.insert(QStringLiteral("modifiedSinceOpen"), modified);
        descriptor.insert(QStringLiteral("dirtyStatus"), modified ? QStringLiteral("edited_after_open")
            : normalized ? QStringLiteral("normalized_at_open") : QStringLiteral("unchanged_since_open"));
        descriptor.insert(QStringLiteral("documentAvailability"), modified
            ? QStringLiteral("live_edited_session_state") : QStringLiteral("live_session_state"));
        descriptor.insert(QStringLiteral("currentRuntimeStamp"), sourceStampJson(stamp, state->sessionEpoch));
        return descriptor;
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
        snapshot.insert(QStringLiteral("runtimeStamp"), QJsonObject{
            {QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("revision"), QString::number(stamp.second)},
            {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")}});
        const QJsonObject project = session->documentProjectContext(modelID);
        snapshot.insert(QStringLiteral("project"), project);
        snapshot.insert(QStringLiteral("persistentProject"), project);
        const QJsonObject resultDescriptor = describeResultView(documentId, stamp);
        if (!resultDescriptor.isEmpty()) snapshot.insert(QStringLiteral("resultView"), resultDescriptor);
        if (liveRead)
        {
            if (project.value(QStringLiteral("draft")).isObject())
                snapshot.insert(QStringLiteral("draft"), project.value(QStringLiteral("draft")));
            if (project.value(QStringLiteral("unsaved")).isObject())
                snapshot.insert(QStringLiteral("unsaved"), project.value(QStringLiteral("unsaved")));
        }
        return snapshot;
    };

    auto readFixedDocument = [&](const QString &documentId, RModel *model,
                                 QPair<QString, quint64> *stamp, uint *modelID,
                                 bool *liveRead, QString *message) {
        uint current = session->findModelByCommandIdentity(documentId);
        if (current == RConstants::eod)
        {
            if (message) *message = tr("The target document is no longer open.");
            return false;
        }
        QPair<QString, quint64> currentStamp = session->modelCommandStamp(current);
        if (currentStamp.first != documentId)
        {
            if (message) *message = tr("The target document identity changed while it was being read.");
            return false;
        }
        TrySessionLock guard(session);
        if (guard.ownsLock())
        {
            current = session->findModelByCommandIdentity(documentId);
            if (current == RConstants::eod)
            {
                if (message) *message = tr("The target document was closed while it was being read.");
                return false;
            }
            currentStamp = session->modelCommandStamp(current);
            if (currentStamp.first != documentId) return false;
            *model = session->getModel(current);
            if (session->modelCommandStamp(current) != currentStamp)
            {
                if (message) *message = tr("The target document changed while it was being read.");
                return false;
            }
            guard.unlock();
            *stamp = currentStamp;
            *modelID = current;
            *liveRead = true;
            return true;
        }
        const auto entry = state->entries.find(documentId);
        if (entry == state->entries.end() || !entry->second.editing
            || entry->second.generation != currentStamp.second)
        {
            if (message) *message = tr("The live document is busy and no current cached snapshot is available.");
            return false;
        }
        *model = entry->second.editing->model();
        *stamp = currentStamp;
        *modelID = current;
        *liveRead = false;
        return true;
    };

    const QJsonArray hostCommands{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.run")},
            {QStringLiteral("version"), 1}, {QStringLiteral("requiresPreview"), true},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("warningsAccepted"), QStringLiteral("boolean")}}},
            {QStringLiteral("history"), QStringLiteral("none")},
            {QStringLiteral("sideEffect"), QStringLiteral("submit_native_solver_job")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.cancel")},
            {QStringLiteral("version"), 1}, {QStringLiteral("requiresPreview"), true},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("jobId"), QStringLiteral("registered native job ID")}}},
            {QStringLiteral("history"), QStringLiteral("none")},
            {QStringLiteral("sideEffect"), QStringLiteral("cancel_specific_native_job")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.recover")},
            {QStringLiteral("version"), 1}, {QStringLiteral("requiresPreview"), true},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("arguments"), QJsonObject{{"jobId", "registered interrupted job ID"},
                {"checkpointSha256", "verified accepted checkpoint SHA-256"}, {"remainingSteps", "frozen remaining range"}}},
            {QStringLiteral("history"), QStringLiteral("none")},
            {QStringLiteral("sideEffect"), QStringLiteral("submit_detached_native_recovery_attempt")}}};

    auto nativeJob = [&](const QString &jobId) {
        return SolverManager::getInstance().jobQuery(jobId);
    };
    auto addHostJobSource = [&](QJsonObject *job, const QString &jobId) {
        const QString jobState = job->value(QStringLiteral("state")).toString();
        job->insert(QStringLiteral("progressState"), job->value(QStringLiteral("progressStatus")));
        job->insert(QStringLiteral("cancellationState"),
            jobState == QStringLiteral("queued") || jobState == QStringLiteral("running")
                ? QStringLiteral("cancellable_by_jobId")
                : jobState == QStringLiteral("cancelling") ? QStringLiteral("cancellation_requested")
                                                            : QStringLiteral("not_cancellable_terminal"));
        const QJsonObject immutableStamp = job->value(QStringLiteral("sourceRuntimeStamp")).toObject();
        const auto source = state->jobSources.find(jobId);
        if (source != state->jobSources.end())
        {
            if (source->second.documentId != immutableStamp.value(QStringLiteral("documentId")).toString()
                || source->second.revision != immutableStamp.value(QStringLiteral("revision")).toString()
                || (!immutableStamp.value(QStringLiteral("sessionEpoch")).toString().isEmpty()
                    && source->second.sessionEpoch != immutableStamp.value(QStringLiteral("sessionEpoch")).toString())) return;
            job->insert(QStringLiteral("hostSource"), QJsonObject{
                {QStringLiteral("documentId"), source->second.documentId},
                {QStringLiteral("sessionEpoch"), source->second.sessionEpoch},
                {QStringLiteral("revision"), source->second.revision}});
            return;
        }
        const QString immutableEpoch = immutableStamp.value(QStringLiteral("sessionEpoch")).toString();
        if (immutableEpoch == state->sessionEpoch)
            job->insert(QStringLiteral("hostSource"), QJsonObject{
                {QStringLiteral("documentId"), immutableStamp.value(QStringLiteral("documentId"))},
                {QStringLiteral("sessionEpoch"), immutableEpoch},
                {QStringLiteral("revision"), immutableStamp.value(QStringLiteral("revision"))}});
    };

    if (operation == QStringLiteral("document.targets.query")
        || operation == QStringLiteral("document.inputs.query"))
    {
        const bool hasPage = parameters.contains(QStringLiteral("page"));
        if ((!exactKeys(parameters, {QStringLiteral("target")})
             && !exactKeys(parameters, {QStringLiteral("target"), QStringLiteral("page")}))
            || (hasPage && !parameters.value(QStringLiteral("page")).isObject()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A fixed document target and optional page object are required."));
        QString documentId, epoch;
        if (!targetFieldsValid(&documentId, &epoch))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A runtime document target is required."));
        if (epoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The target belongs to a different host session."));
        RModel model;
        QPair<QString, quint64> stamp;
        uint modelID = RConstants::eod;
        bool liveRead = false;
        QString readError;
        if (!readFixedDocument(documentId, &model, &stamp, &modelID, &liveRead, &readError))
            return invalid(QStringLiteral("DOCUMENT_UNAVAILABLE"), readError);
        const QJsonObject page = parameters.value(QStringLiteral("page")).toObject();
        QJsonObject result = operation == QStringLiteral("document.targets.query")
            ? RModelCommandService::targets(model, documentId, QString::number(stamp.second), page)
            : RModelCommandService::context(model, documentId, QString::number(stamp.second), page);
        if (result.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok")))
            return invalid(result.value(QStringLiteral("code")).toString().toUpper(),
                           result.value(QStringLiteral("message")).toString());
        result.insert(QStringLiteral("target"), QJsonObject{
            {QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch}});
        result.insert(QStringLiteral("runtimeStamp"), sourceStampJson(stamp, state->sessionEpoch));
        result.insert(QStringLiteral("stateAvailability"), liveRead ? QStringLiteral("live") : QStringLiteral("cached"));
        result.insert(QStringLiteral("stateFreshness"), liveRead ? QStringLiteral("locked-live") : QStringLiteral("last-known-only"));
        result.insert(QStringLiteral("persistentProject"), session->documentProjectContext(modelID));
        result.insert(QStringLiteral("hostCommands"), hostCommands);
        const QJsonObject descriptor = describeResultView(documentId, stamp);
        if (!descriptor.isEmpty()) result.insert(QStringLiteral("resultView"), descriptor);
        return ok(result);
    }

    if (operation == QStringLiteral("document.source.validate"))
    {
        if (!exactKeys(parameters, {QStringLiteral("snapshotRef")}) || !parameters.value("snapshotRef").isObject())
            return invalid("INVALID_PARAMETERS", tr("An immutable source reference is required."));
        const auto origin = parameters.value("snapshotRef").toObject();
        if (!exactKeys(origin, {"projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef"})
            || !origin.value("instanceRef").isObject())
            return invalid("INVALID_PARAMETERS", tr("The source reference has unsupported fields."));
        const auto instance = origin.value("instanceRef").toObject();
        quint64 revision = 0;
        if (!exactKeys(instance, {"documentId", "sessionEpoch", "documentRevision"})
            || !validToken(instance.value("documentId")) || !validToken(instance.value("sessionEpoch"))
            || !unsignedInteger(instance.value("documentRevision"), &revision))
            return invalid("INVALID_PARAMETERS", tr("A valid source runtime stamp is required."));
        if (instance.value("sessionEpoch").toString() != state->sessionEpoch)
            return invalid("STALE_SESSION", tr("The source document belongs to an earlier session."));
        const uint modelID = session->findModelByCommandIdentity(instance.value("documentId").toString());
        if (modelID >= session->getNModels()) return invalid("DOCUMENT_CLOSED", tr("The source document closed."));
        if (!session->trylock(0)) return invalid("MODEL_BUSY", tr("The source document state is busy."));
        const auto stamp = session->modelCommandStamp(modelID);
        const auto context = session->documentProjectContext(modelID);
        const bool draft = session->hasDocumentDraft(modelID);
        session->unlock();
        if (context.value("availability") != QJsonValue("available")
            || context.value("persistence").toObject().value("status") != QJsonValue("available"))
            return invalid("SOURCE_UNAVAILABLE", tr("The source project has a conflict or unavailable persistence state."));
        if (stamp.second != revision || draft)
            return invalid("STALE_SOURCE", tr("The source document was edited or has an unresolved draft."));
        for (const QString &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
                                    QStringLiteral("sourceRevision"), QStringLiteral("modelSha256")})
            if (!origin.value(field).isString() || origin.value(field).toString().isEmpty() || context.value(field) != origin.value(field))
                return invalid("STALE_SOURCE", tr("The persistent source version or hash changed."));
        return ok({{"valid", true}, {"snapshotRef", origin}});
    }

    if (operation == QStringLiteral("native.jobs.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("offset"), QStringLiteral("limit")}))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("native.jobs.query requires offset and limit."));
        quint64 offset = 0, limit = 0;
        if (!unsignedInteger(parameters.value(QStringLiteral("offset")), &offset)
            || !unsignedInteger(parameters.value(QStringLiteral("limit")), &limit)
            || offset > quint64(std::numeric_limits<int>::max()) || limit < 1 || limit > 200)
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("Job pagination requires offset >= 0 and limit from 1 through 200."));
        QJsonObject result = SolverManager::getInstance().jobsQuery(int(offset), int(limit));
        if (!result.value(QStringLiteral("error")).toString().isEmpty())
            return invalid(result.value(QStringLiteral("error")).toString().toUpper(),
                           tr("The native job page could not be read."));
        QJsonArray jobs = result.value(QStringLiteral("jobs")).toArray();
        for (qsizetype i = 0; i < jobs.size(); ++i)
        {
            QJsonObject job = jobs.at(i).toObject();
            addHostJobSource(&job, job.value(QStringLiteral("jobId")).toString());
            jobs.replace(i, job);
        }
        result.insert(QStringLiteral("jobs"), jobs);
        return ok(result);
    }

    if (operation == QStringLiteral("native.job.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("jobId")})
            || !validToken(parameters.value(QStringLiteral("jobId"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("native.job.query requires a jobId."));
        const QString jobId = parameters.value(QStringLiteral("jobId")).toString();
        QJsonObject job = nativeJob(jobId);
        if (!job.value(QStringLiteral("found")).toBool())
            return invalid(QStringLiteral("JOB_NOT_FOUND"), tr("No native job with this ID is registered."));
        addHostJobSource(&job, jobId);
        return ok({{QStringLiteral("job"), job}});
    }
    if (operation == QStringLiteral("native.job.recovery.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("jobId")}) || !validToken(parameters.value("jobId")))
            return invalid("INVALID_PARAMETERS", tr("Native recovery query requires a registered jobId."));
        auto qualification = SolverManager::getInstance().nativeJobRecoveryQuery(parameters.value("jobId").toString());
        if (!qualification.value("found").toBool()) return invalid("JOB_NOT_FOUND", tr("No native job with this ID is registered."));
        qualification.remove("checkpointPath");
        qualification.remove("checkpointDirectory");
        return ok(qualification);
    }

    if (operation == QStringLiteral("native.result.catalog.query")
        || operation == QStringLiteral("native.result.channels.query")
        || operation == QStringLiteral("native.result.series.query")
        || operation == QStringLiteral("native.result.summary.query"))
    {
        if (!validToken(parameters.value(QStringLiteral("jobId"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A registered native jobId is required."));
        QJsonObject arguments = parameters;
        const QString jobId = arguments.take(QStringLiteral("jobId")).toString();
        const QString queryOperation = operation.section(QLatin1Char('.'), 2, 2);
        const QJsonObject result = SolverManager::getInstance().nativeResultQuery(jobId, queryOperation, arguments);
        if (!result.value(QStringLiteral("valid")).toBool())
            return invalid(result.value(QStringLiteral("code")).toString(), result.value(QStringLiteral("message")).toString());
        return ok(result);
    }

    if (operation == QStringLiteral("native.result.display"))
    {
        if (!exactKeys(parameters, {"jobId", "artifactId", "channelId"})
            || !validToken(parameters.value("jobId")) || !validToken(parameters.value("artifactId"))
            || !validToken(parameters.value("channelId")))
            return invalid("INVALID_PARAMETERS", tr("native.result.display requires jobId, artifactId and channelId only."));
        if (activeModalInteraction())
            return invalid("INTERACTION_BLOCKED", tr("Close the active modal interaction before displaying a solver result."));
        if (isMutationBlocked()) return invalid("MODEL_BUSY", tr("Wait for live model tasks before displaying a solver result."));
        const QString jobId = parameters.value("jobId").toString();
        const QString artifactId = parameters.value("artifactId").toString();
        const QJsonObject job = nativeJob(jobId);
        if (!job.value("found").toBool()) return invalid("JOB_NOT_FOUND", tr("No native job with this ID is registered."));
        if (job.value("state").toString() != "succeeded" || !job.value("resultReadbackComplete").toBool()
            || !job.value("resultArtifactVerified").toBool())
            return invalid("RESULT_UNAVAILABLE", tr("Display requires a verified completed native job."));
        SolverManager &manager = SolverManager::getInstance();
        const QJsonObject catalog = manager.nativeResultQuery(jobId, "catalog", {{"artifactId", artifactId}, {"offset", 0}, {"limit", 1}});
        if (!catalog.value("valid").toBool()) return invalid(catalog.value("code").toString(), catalog.value("message").toString());
        if (catalog.value("artifactId").toString() != artifactId || catalog.value("jobId").toString() != jobId)
            return invalid("ARTIFACT_IDENTITY_MISMATCH", tr("The catalog does not match the requested job and artifact."));
        if (job.value("resultArtifactId").toString() != artifactId)
            return invalid("ARTIFACT_IDENTITY_MISMATCH", tr("Field display requires the registered terminal artifact, not an earlier live snapshot."));
        const QJsonObject summary = manager.nativeResultQuery(jobId, "summary",
            {{"artifactId", artifactId}, {"channelId", parameters.value("channelId")}});
        if (!summary.value("valid").toBool()) return invalid(summary.value("code").toString(), summary.value("message").toString());
        if (summary.value("artifactId").toString() != artifactId || summary.value("channelId") != parameters.value("channelId"))
            return invalid("ARTIFACT_IDENTITY_MISMATCH", tr("The physical channel does not match the requested artifact."));
        const QMap<QString, RVariableType> supported{
            {"var-displacement", R_VARIABLE_DISPLACEMENT}, {"var-velocity", R_VARIABLE_VELOCITY},
            {"var-acceleration", R_VARIABLE_ACCELERATION}, {"var-rotation", R_VARIABLE_ROTATION},
            {"var-angular_velocity", R_VARIABLE_ANGULAR_VELOCITY}, {"var-angular_acceleration", R_VARIABLE_ANGULAR_ACCELERATION}};
        const RVariableType variableType = supported.value(summary.value("variableId").toString(), R_VARIABLE_NONE);
        if (variableType == R_VARIABLE_NONE) return invalid("UNSUPPORTED_CHANNEL", tr("This physical channel has no supported native field display."));
        QJsonObject openRequest = request;
        openRequest.insert("operation", "native.result.open");
        openRequest.insert("parameters", QJsonObject{{"jobId", jobId}, {"expectedSHA", job.value("resultArtifactSha256")}});
        const QJsonObject opened = handle(openRequest);
        if (opened.value("status").toString() != "accepted") return opened;
        QJsonObject result = opened.value("result").toObject();
        const QString documentId = result.value("documentId").toString();
        const uint openedModelID = session->findModelByCommandIdentity(documentId);
        if (openedModelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CLOSED", tr("The result document closed during opening."));
        const quint64 revision = session->modelCommandStamp(openedModelID).second;
        auto currentModel = [&]() -> uint {
            if (isMutationBlocked() || activeModalInteraction()) return RConstants::eod;
            const uint id = session->findModelByCommandIdentity(documentId);
            if (id == RConstants::eod || session->modelCommandStamp(id).second != revision) return RConstants::eod;
            return id;
        };
        // Selection emits synchronous GUI signals: never retain a Model reference
        // or an index across them, and reject a closed or edited result document.
        QStringList selectedDocuments;
        for (uint id : session->getSelectedModelIDs()) selectedDocuments.append(session->modelCommandStamp(id).first);
        for (const QString &selected : selectedDocuments)
        {
            if (currentModel() == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during selection."));
            const uint id = session->findModelByCommandIdentity(selected);
            if (id != RConstants::eod && selected != documentId) session->setModelSelected(id, false);
        }
        uint modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during selection."));
        session->setModelSelected(modelID, true);
        modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during selection."));
        uint nodeID = RConstants::eod;
        uint groupID = RConstants::eod;
        PickItem picked;
        {
            Model &model = session->getModel(modelID);
            const uint physicalId = uint(summary.value("nodePhysicalId").toDouble());
            for (uint i = 0; i < model.getNNodes(); ++i)
                if (model.getNodePhysicalId(i) == physicalId) { nodeID = i; break; }
            if (nodeID == RConstants::eod) return invalid("NODE_NOT_FOUND", tr("The physical node is absent from the verified result document."));
            if (model.findVariable(variableType) == RConstants::eod)
                return invalid("FIELD_UNAVAILABLE", tr("The terminal result document has no requested field."));
            for (uint i = 0; i < model.getNEntityGroups(true) && groupID == RConstants::eod; ++i)
            {
                const auto *group = static_cast<const RElementGroup *>(model.getEntityGroupPtr(i, true));
                for (uint j = 0; j < group->size() && groupID == RConstants::eod; ++j)
                {
                    const uint elementID = group->get(j);
                    const RElement &element = model.getElement(elementID);
                    for (uint k = 0; k < element.size(); ++k)
                        if (element.getNodeId(k) == nodeID)
                        {
                            groupID = i;
                            picked = PickItem(SessionEntityID(modelID, model.getEntityGroupType(i, true),
                                model.getEntityGroupPosition(i, true)), elementID, j, nodeID, k);
                            break;
                        }
                }
            }
            if (groupID == RConstants::eod) return invalid("NODE_NOT_DISPLAYABLE", tr("The physical node has no incident native entity group."));
            auto &data = model.getEntityGroupPtr(groupID, true)->getData();
            data.setVisible(true);
            data.clearVariableDisplayType(R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR);
            data.clearVariableDisplayType(R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT);
            data.getVariableData()[variableType] |= R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR;
            if (variableType == R_VARIABLE_DISPLACEMENT)
                data.getVariableData()[variableType] |= R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT;
        }
        session->getPickList().clear();
        modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during highlighting."));
        // PickList signals can also reorder Session storage.
        const SessionEntityID entityID(modelID, picked.getEntityID().getType(), picked.getEntityID().getEid());
        session->getPickList().addItem(PickItem(entityID, picked.getElementID(), picked.getElementPosition(), nodeID, picked.getNodePosition()));
        modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during highlighting."));
        // Only display masks changed. variableDataChanged invalidates the
        // accepted document revision and is reserved for variable edits.
        session->setDisplayPropertiesChanged(modelID, entityID.getType(), entityID.getEid());
        if (currentModel() == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during field display."));
        result.insert("artifactId", artifactId);
        result.insert("channelId", parameters.value("channelId"));
        result.insert("nodePhysicalId", summary.value("nodePhysicalId"));
        result.insert("variableId", summary.value("variableId"));
        result.insert("unit", summary.value("unit"));
        result.insert("coordinateSystem", "global");
        result.insert("selectedChannelAxis", summary.value("axis"));
        result.insert("fieldDisplayScope", "full-variable-magnitude");
        result.insert("fieldDisplayEntityScope", "incident-entity-group");
        result.insert("displacementGeometry", variableType == R_VARIABLE_DISPLACEMENT);
        result.insert("message", tr("Displayed the full native field magnitude and highlighted the physical node; the selected channel component remains a separate series."));
        return ok(result, "accepted");
    }

    if (operation == QStringLiteral("native.result.open"))
    {
        if (activeModalInteraction())
            return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before opening a solver result document."));
        if (isMutationBlocked())
            return invalid(QStringLiteral("MODEL_BUSY"), tr("A live model task is using Session storage; wait before opening a result document."));
        if (!exactKeys(parameters, {QStringLiteral("jobId"), QStringLiteral("expectedSHA")})
            || !validToken(parameters.value(QStringLiteral("jobId")))
            || !parameters.value(QStringLiteral("expectedSHA")).isString())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("native.result.open requires jobId and expectedSHA."));
        const QString jobId = parameters.value(QStringLiteral("jobId")).toString();
        const QString expectedSha = parameters.value(QStringLiteral("expectedSHA")).toString();
        static const QRegularExpression shaGrammar(QStringLiteral("^[0-9a-f]{64}$"));
        if (!shaGrammar.match(expectedSha).hasMatch())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("expectedSHA must be a lowercase SHA-256 digest."));
        QJsonObject job = nativeJob(jobId);
        if (!job.value(QStringLiteral("found")).toBool())
            return invalid(QStringLiteral("JOB_NOT_FOUND"), tr("No native job with this ID is registered."));
        addHostJobSource(&job, jobId);
        if (job.value(QStringLiteral("state")).toString() != QStringLiteral("succeeded")
            || !job.value(QStringLiteral("resultReadbackComplete")).toBool()
            || !job.value(QStringLiteral("resultArtifactVerified")).toBool())
            return invalid(QStringLiteral("RESULT_UNAVAILABLE"), tr("The native job has no verified terminal result artifact."));
        const QString registeredPath = job.value(QStringLiteral("resultArtifactRef")).toString();
        const QString registeredSha = job.value(QStringLiteral("resultArtifactSha256")).toString();
        if (registeredPath.isEmpty() || registeredSha != expectedSha)
            return invalid(QStringLiteral("ARTIFACT_IDENTITY_MISMATCH"), tr("The expected SHA does not match the immutable registered result identity."));
        QString sourceError;
        const auto beforeBinding = RProjectCatalog::resolveSourceBinding(registeredPath, &sourceError);
        const QJsonObject registeredBinding = job.value(QStringLiteral("resultSourceBinding")).toObject();
        if (!beforeBinding || beforeBinding->sourceArtifacts.isEmpty()
            || beforeBinding->sourceArtifacts.first().sha256 != expectedSha
            || nativeSourceBindingJson(*beforeBinding) != registeredBinding)
            return invalid(QStringLiteral("ARTIFACT_CHANGED"), sourceError.isEmpty()
                ? tr("The native result source chain no longer matches its immutable registered identity.") : sourceError);

        QString resultDocumentId;
        bool reused = false;
        bool normalizedAtOpen = false;
        quint64 openedAtRevision = 0;
        if (!session->trylock(0))
            return invalid(QStringLiteral("MODEL_BUSY"), tr("The session is busy and cannot open a result document now."));
        if (isMutationBlocked())
        {
            session->unlock();
            return invalid(QStringLiteral("MODEL_BUSY"), tr("A live model task is using Session storage; wait before opening a result document."));
        }
        QSet<QString> knownOpenDocuments;
        for (uint i = 0; i < session->getNModels(); ++i)
        {
            const auto stamp = session->modelCommandStamp(i);
            if (!stamp.first.isEmpty()) knownOpenDocuments.insert(stamp.first);
        }
        // Reuse only a result document whose current Session revision still
        // equals the revision captured when this host opened it.
        for (const auto &[knownJobId, recorded] : state->resultDocuments)
        {
            Q_UNUSED(knownJobId)
            if (recorded.artifactPath != beforeBinding->logicalSourcePath
                || recorded.artifactSha256 != expectedSha) continue;
            const uint currentID = session->findModelByCommandIdentity(recorded.documentId);
            if (currentID == RConstants::eod) continue;
            const auto currentStamp = session->modelCommandStamp(currentID);
            if (currentStamp.second == recorded.openedAtRevision)
            {
                resultDocumentId = recorded.documentId;
                openedAtRevision = recorded.openedAtRevision;
                normalizedAtOpen = recorded.normalizedAtOpen;
                reused = true;
                break;
            }
        }
        // An unchanged native document opened outside this host is reusable
        // only while it is still at its initial runtime revision.
        if (resultDocumentId.isEmpty())
        {
            for (uint i = 0; i < session->getNModels(); ++i)
            {
                const auto stamp = session->modelCommandStamp(i);
                if (stamp.first.isEmpty() || state->resultDescriptors.find(stamp.first) != state->resultDescriptors.end())
                    continue;
                const Model &existing = session->getModel(i);
                const auto &binding = existing.getLoadedSourceBinding();
                if (stamp.second == 0 && binding && *binding == *beforeBinding
                    && !existing.hasNativeReadNormalization())
                {
                    resultDocumentId = stamp.first;
                    openedAtRevision = stamp.second;
                    reused = true;
                    break;
                }
            }
        }
        const uint appendModelIndex = session->getNModels();
        session->unlock();

        if (resultDocumentId.isEmpty())
        {
            Model model;
            RProjectCatalog::SourceBinding loadedBinding;
            try
            {
                ModelIO::open(model, registeredPath, &loadedBinding);
            }
            catch (const RError &failure)
            {
                return invalid(QStringLiteral("RESULT_OPEN_FAILED"), failure.getMessage());
            }
            normalizedAtOpen = model.hasNativeReadNormalization();
            QString afterError;
            const auto afterBinding = RProjectCatalog::resolveSourceBinding(registeredPath, &afterError);
            if (!afterBinding || *afterBinding != *beforeBinding || loadedBinding != *beforeBinding
                || nativeSourceBindingJson(*afterBinding) != registeredBinding)
                return invalid(QStringLiteral("ARTIFACT_CHANGED"), afterError.isEmpty()
                    ? tr("The terminal native artifact changed while the result was being loaded.") : afterError);

            if (!session->trylock(0))
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The session became busy before the result document could be attached."));
            const bool attachBlocked = isMutationBlocked();
            session->unlock();
            if (attachBlocked)
                return invalid(QStringLiteral("MODEL_BUSY"), tr("A live model task started before the result document could be attached."));

            // addModel is synchronous on the GUI thread. Capture the runtime
            // identity for the slot appended by this call. Project catalog
            // registration can detach a duplicate source as an independent
            // copy and clear its loaded-source binding, so binding equality
            // is not a reliable way to discover the document that was just
            // created.
            auto onModelAdded = connect(session, &Session::modelAdded, this, [&](uint modelID) {
                if (modelID != appendModelIndex) return;
                const auto stamp = session->modelCommandStamp(modelID);
                if (stamp.first.isEmpty()) return;
                if (!knownOpenDocuments.contains(stamp.first))
                    resultDocumentId = stamp.first;
            });
            session->addModel(model, loadedBinding);
            disconnect(onModelAdded);
            if (resultDocumentId.isEmpty())
            {
                if (!session->trylock(0))
                    return invalid(QStringLiteral("RESULT_OPEN_PENDING"), tr("The result was queued for session opening but its runtime document identity is not available yet."));
                if (session->getNModels() > appendModelIndex)
                {
                    const auto stamp = session->modelCommandStamp(appendModelIndex);
                    if (!stamp.first.isEmpty() && !knownOpenDocuments.contains(stamp.first))
                        resultDocumentId = stamp.first;
                }
                session->unlock();
            }
            if (resultDocumentId.isEmpty())
                return invalid(QStringLiteral("RESULT_OPEN_FAILED"), tr("The result artifact was loaded but no independent GUI document was created."));
        }

        const uint resultModelID = session->findModelByCommandIdentity(resultDocumentId);
        if (resultModelID == RConstants::eod)
            return invalid(QStringLiteral("RESULT_DOCUMENT_CLOSED"), tr("The result document closed before its open response was built."));
        const QPair<QString, quint64> resultStamp = session->modelCommandStamp(resultModelID);
        if (reused && resultStamp.second != openedAtRevision)
            return invalid(QStringLiteral("RESULT_DOCUMENT_CHANGED"), tr("The result document changed while its reuse response was being built."));
        if (!reused)
        {
            openedAtRevision = resultStamp.second;
            normalizedAtOpen = normalizedAtOpen || openedAtRevision != 0;
        }
        const QJsonObject displayDescriptor{{QStringLiteral("kind"), QStringLiteral("native_solver_result")},
            {QStringLiteral("displayLabel"), tr("Solver result %1").arg(jobId.left(8))},
            {QStringLiteral("sourceJobId"), jobId},
            {QStringLiteral("artifactRef"), registeredPath},
            {QStringLiteral("artifactSHA256"), expectedSha},
            {QStringLiteral("artifactBytesVerifiedAtOpen"), true},
            {QStringLiteral("openedAtRuntimeStamp"), QJsonObject{
                {QStringLiteral("documentId"), resultDocumentId},
                {QStringLiteral("sessionEpoch"), state->sessionEpoch},
                {QStringLiteral("revision"), QString::number(openedAtRevision)}}},
            {QStringLiteral("normalizedAtOpen"), normalizedAtOpen},
            {QStringLiteral("documentStateAtOpen"), normalizedAtOpen
                ? QStringLiteral("normalized_during_open") : QStringLiteral("decoded_from_verified_artifact")},
            {QStringLiteral("artifactModelEquivalence"), normalizedAtOpen
                ? QStringLiteral("not_asserted_after_normalization") : QStringLiteral("verified_source_load")},
            {QStringLiteral("dirtyStatus"), normalizedAtOpen
                ? QStringLiteral("normalized_at_open") : QStringLiteral("unchanged_since_open")},
            {QStringLiteral("modifiedSinceOpen"), false}};
        state->resultDocuments[jobId] = State::ResultDocument{resultDocumentId,
            beforeBinding->logicalSourcePath, expectedSha, openedAtRevision, normalizedAtOpen};
        state->resultDescriptors[resultDocumentId] = displayDescriptor;
        const QJsonObject project = session->documentProjectContext(resultModelID);
        const QJsonObject source = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        QJsonObject inputProvenance;
        for (const QString &field : {QStringLiteral("inputDerivedFrom"),
                                     QStringLiteral("frozenSnapshotVerified"),
                                     QStringLiteral("frozenSnapshotPath"),
                                     QStringLiteral("frozenSnapshotSha256")})
            if (!job.value(field).isUndefined()) inputProvenance.insert(field, job.value(field));
        for (const QString &field : {QStringLiteral("inputSnapshotRef"),
                                     QStringLiteral("inputSnapshotSha256"),
                                     QStringLiteral("inputPackageRef"),
                                     QStringLiteral("inputPackageSha256")})
            if (!job.value(field).isUndefined()) inputProvenance.insert(QStringLiteral("solver_") + field, job.value(field));
        const QJsonObject result{{QStringLiteral("target"), QJsonObject{
                {QStringLiteral("documentId"), resultDocumentId},
                {QStringLiteral("sessionEpoch"), state->sessionEpoch}}},
            {QStringLiteral("documentId"), resultDocumentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("runtimeStamp"), sourceStampJson(resultStamp, state->sessionEpoch)},
            {QStringLiteral("persistentDocumentId"), project.value(QStringLiteral("persistentDocumentId"))},
            {QStringLiteral("projectId"), project.value(QStringLiteral("projectId"))},
            {QStringLiteral("persistentProject"), project},
            {QStringLiteral("displayDescriptor"), displayDescriptor},
            {QStringLiteral("sourceJob"), QJsonObject{{QStringLiteral("jobId"), jobId},
                {QStringLiteral("sourceRuntimeStamp"), source},
                {QStringLiteral("hostSource"), job.value(QStringLiteral("hostSource"))},
                {QStringLiteral("projectSnapshotRef"), job.value(QStringLiteral("projectSnapshotRef"))},
                {QStringLiteral("inputProvenance"), inputProvenance},
                {QStringLiteral("artifactRef"), registeredPath},
                {QStringLiteral("artifactSHA256"), expectedSha}}},
            {QStringLiteral("openResult"), reused ? QStringLiteral("reused") : QStringLiteral("created")},
            {QStringLiteral("resultViewCreated"), !reused},
            {QStringLiteral("message"), reused ? tr("Reused the open independent solver result view.")
                                                : tr("Created a new independent solver result view; the source model and solver state are unchanged.")},
            {QStringLiteral("sourceAndSolverUnchanged"), true}};
        if (!reused)
        {
            QJsonObject event = state->appendEvent(QStringLiteral("document.result_opened"), resultDocumentId,
                {{QStringLiteral("sourceJobId"), jobId},
                 {QStringLiteral("artifactSHA256"), expectedSha},
                 {QStringLiteral("sessionEpoch"), state->sessionEpoch}});
            emit eventPublished(event);
        }
        return ok(result, QStringLiteral("accepted"));
    }

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
        QJsonArray operations{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("project.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.project.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("context.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("capabilities.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("events.read")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("command.preview")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("command.execute")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("operation.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("operations.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.targets.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.inputs.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.jobs.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.job.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.job.recovery.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.source.validate")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.result.open")}, {QStringLiteral("version"), 1},
                {QStringLiteral("readOnly"), true}, {QStringLiteral("createsIndependentResultDocument"), true},
                {QStringLiteral("requiresRegisteredArtifactSHA"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.result.display")}, {QStringLiteral("version"), 1},
                {QStringLiteral("readOnly"), true}, {QStringLiteral("createsIndependentResultDocument"), true},
                {QStringLiteral("identity"), QStringLiteral("registered-job-artifact-and-channel")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.undo")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.redo")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}}};
        for (const QString &quantity : {QStringLiteral("catalog"), QStringLiteral("channels"),
                                       QStringLiteral("series"), QStringLiteral("summary")})
            operations.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("native.result.") + quantity + QStringLiteral(".query")},
                {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true},
                {QStringLiteral("identity"), QStringLiteral("registered-job-and-artifact")}});
        return ok({{QStringLiteral("sessionEpoch"), state->sessionEpoch},
                   {QStringLiteral("identityPersistence"), QStringLiteral("host-session")},
                   {QStringLiteral("capabilityCatalogVersion"), 1},
                   {QStringLiteral("supportedOperations"), operations},
                   {QStringLiteral("hostCommands"), hostCommands},
                   {QStringLiteral("editingScope"), QJsonObject{
                        {QStringLiteral("version"), 1},
                        {QStringLiteral("models"), QStringLiteral("open native documents with supported structural analysis tasks")},
                        {QStringLiteral("elements"), QStringLiteral("physical group edits preserve other model fields; labels are read-only target candidates")},
                        {QStringLiteral("analyses"), QJsonArray{QStringLiteral("structural.modal.configuration"),
                            QStringLiteral("structural.dynamics.configuration"), QStringLiteral("native.solver.run"),
                            QStringLiteral("native.solver.cancel")}},
                        {QStringLiteral("units"), QJsonObject{{QStringLiteral("modal.parameters"), QStringLiteral("1")},
                            {QStringLiteral("material.modulus"), QStringLiteral("Pa")},
                            {QStringLiteral("material.density"), QStringLiteral("kg/m^3")},
                            {QStringLiteral("boundary.displacement"), QStringLiteral("m")},
                            {QStringLiteral("boundary.force"), QStringLiteral("N")}}},
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
        bool editingBlocked = isMutationBlocked() || activeModalInteraction();
        QList<uint> selectedModels;
        QList<SessionEntityID> selectedEntities;
        QStringList documentIds;
        bool selectionLive = false;
        if (!editingBlocked && session->trylock(0))
        {
            if (!isMutationBlocked() && !activeModalInteraction())
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
                if (isMutationBlocked() || activeModalInteraction())
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
        if (!parameters.value(QStringLiteral("command")).isString()
            || parameters.value(QStringLiteral("commandVersion")).toInt(-1) != 1
            || !parameters.value(QStringLiteral("arguments")).isObject())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The command and versioned arguments are required."));
        const QString command = parameters.value(QStringLiteral("command")).toString();
        const QJsonObject arguments = parameters.value(QStringLiteral("arguments")).toObject();
        const bool cancelCommand = command == QStringLiteral("analysis.cancel");
        const bool hasTarget = parameters.contains(QStringLiteral("target"));
        const QSet<QString> standardKeys{QStringLiteral("target"), QStringLiteral("expectedDocumentRevision"),
            QStringLiteral("command"), QStringLiteral("commandVersion"), QStringLiteral("arguments")};
        const QSet<QString> detachedCancelKeys{QStringLiteral("command"), QStringLiteral("commandVersion"),
            QStringLiteral("arguments")};
        if (command == "analysis.recover")
        {
            const int remaining = arguments.value("remainingSteps").toInt(-1);
            static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
            if (!exactKeys(parameters, detachedCancelKeys) || !exactKeys(arguments, {"jobId", "checkpointSha256", "remainingSteps"})
                || !validToken(arguments.value("jobId")) || !arguments.value("checkpointSha256").isString()
                || !shaPattern.match(arguments.value("checkpointSha256").toString()).hasMatch()
                || !arguments.value("remainingSteps").isDouble() || arguments.value("remainingSteps").toDouble() != remaining
                || remaining < 1 || remaining > 5000)
                return invalid("INVALID_PARAMETERS", tr("Recovery requires an opaque job ID, selected checkpoint SHA and exact positive remaining range."));
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before previewing native recovery."));
            const auto qualification = SolverManager::getInstance().nativeJobRecoveryQuery(arguments.value("jobId").toString());
            const auto checkpoint = qualification.value("checkpoint").toObject();
            if (qualification.value("eligibility") != QJsonValue("recoverable")
                || checkpoint.value("checkpointSha256") != arguments.value("checkpointSha256")
                || checkpoint.value("remainingSteps").toInt(-1) != remaining)
                return invalid("RECOVERY_CHANGED", tr("The selected checkpoint or remaining range is unavailable. Query native recovery again."));
            State::Preview preview;
            preview.id = uuid();
            preview.sessionEpoch = state->sessionEpoch;
            preview.command = command;
            preview.jobId = arguments.value("jobId").toString();
            preview.arguments = arguments;
            preview.recoveryQualification = qualification;
            preview.digest = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(qualification)
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject result{{"command", command}, {"commandVersion", 1}, {"previewId", preview.id},
                {"digest", preview.digest}, {"arguments", arguments}, {"requiresHostApproval", true},
                {"before", QJsonObject{{"jobId", preview.jobId}, {"acceptedSeq", checkpoint.value("acceptedSeq")}}},
                {"after", QJsonObject{{"remainingSteps", remaining}, {"targetSteps", checkpoint.value("targetSteps")},
                    {"sourceDocumentModified", false}, {"newAttempt", true}}},
                {"history", "none"}, {"changedPaths", QJsonArray{}}};
            if (state->previewOrder.size() >= 128)
            { state->previews.erase(state->previewOrder.takeFirst()); }
            state->previewOrder.append(preview.id);
            state->previews.emplace(preview.id, preview);
            return ok(result, "preview");
        }
        if (cancelCommand)
        {
            if ((!hasTarget && !exactKeys(parameters, detachedCancelKeys))
                || (hasTarget && !exactKeys(parameters, standardKeys))
                || !exactKeys(arguments, {QStringLiteral("jobId")})
                || !validToken(arguments.value(QStringLiteral("jobId"))))
                return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("analysis.cancel requires only a registered jobId and may include its original runtime target."));
            if (activeModalInteraction())
                return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before cancelling a solver job."));
            const QString jobId = arguments.value(QStringLiteral("jobId")).toString();
            const QJsonObject job = nativeJob(jobId);
            if (!job.value(QStringLiteral("found")).toBool())
                return invalid(QStringLiteral("JOB_NOT_FOUND"), tr("No native job with this ID is registered."));
            const QString jobState = job.value(QStringLiteral("state")).toString();
            if (jobState != QStringLiteral("queued") && jobState != QStringLiteral("running")
                && jobState != QStringLiteral("cancelling"))
                return invalid(QStringLiteral("JOB_TERMINAL"), tr("The selected native job is already terminal."));
            const QJsonObject jobStamp = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
            const QString sourceDocumentId = jobStamp.value(QStringLiteral("documentId")).toString();
            const QString sourceRevision = jobStamp.value(QStringLiteral("revision")).toString();
            if (sourceDocumentId.isEmpty() || sourceRevision.isEmpty())
                return invalid(QStringLiteral("JOB_SOURCE_UNAVAILABLE"), tr("The job has no immutable source document identity."));
            QString sourceEpoch;
            const auto ownedSource = state->jobSources.find(jobId);
            if (ownedSource != state->jobSources.end())
            {
                if (ownedSource->second.documentId != sourceDocumentId || ownedSource->second.revision != sourceRevision)
                    return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The registered job source differs from its host-owned immutable source."));
                const QString managerEpoch = jobStamp.value(QStringLiteral("sessionEpoch")).toString();
                if (!managerEpoch.isEmpty() && managerEpoch != ownedSource->second.sessionEpoch)
                    return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The registered job session epoch differs from its host-owned immutable source."));
                sourceEpoch = ownedSource->second.sessionEpoch;
            }
            else
            {
                sourceEpoch = jobStamp.value(QStringLiteral("sessionEpoch")).toString();
                if (sourceEpoch.isEmpty()) sourceEpoch = job.value(QStringLiteral("projectSnapshotRef")).toObject()
                                  .value(QStringLiteral("instanceRef")).toObject()
                                  .value(QStringLiteral("sessionEpoch")).toString();
                if (sourceEpoch.isEmpty())
                {
                    const uint currentId = session->findModelByCommandIdentity(sourceDocumentId);
                    if (currentId != RConstants::eod
                        && QString::number(session->modelCommandStamp(currentId).second) == sourceRevision)
                        sourceEpoch = state->sessionEpoch;
                }
            }
            if (sourceEpoch != state->sessionEpoch)
                return invalid(QStringLiteral("STALE_SESSION"), tr("The job belongs to a different host session."));
            if (ownedSource == state->jobSources.end())
                state->jobSources[jobId] = State::JobSource{sourceDocumentId, sourceEpoch, sourceRevision,
                    job.value(QStringLiteral("projectSnapshotRef")).toObject()};
            if (hasTarget)
            {
                QString targetDocument, targetEpoch;
                quint64 expectedRevision = 0;
                if (!targetFieldsValid(&targetDocument, &targetEpoch)
                    || !unsignedInteger(parameters.value(QStringLiteral("expectedDocumentRevision")), &expectedRevision)
                    || targetDocument != sourceDocumentId || targetEpoch != sourceEpoch
                    || QString::number(expectedRevision) != sourceRevision)
                    return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The supplied target does not match the job's immutable source."));
            }
            State::Preview preview;
            preview.id = uuid();
            preview.documentId = sourceDocumentId;
            preview.sessionEpoch = sourceEpoch;
            preview.command = command;
            preview.jobId = jobId;
            preview.jobSourceDocumentId = sourceDocumentId;
            preview.jobSourceRevision = sourceRevision;
            preview.arguments = arguments;
            preview.nativePreview = job;
            const QJsonObject readSetObject{{QStringLiteral("jobId"), jobId},
                {QStringLiteral("sourceRuntimeStamp"), jobStamp},
                {QStringLiteral("sessionEpoch"), sourceEpoch}};
            preview.readSetHash = QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(readSetObject).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject digestInput{{QStringLiteral("command"), command},
                {QStringLiteral("commandVersion"), 1}, {QStringLiteral("arguments"), arguments},
                {QStringLiteral("job"), jobStamp}, {QStringLiteral("sessionEpoch"), sourceEpoch},
                {QStringLiteral("readSetHash"), preview.readSetHash}};
            preview.digest = QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(digestInput).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject result{{QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
                {QStringLiteral("arguments"), arguments},
                {QStringLiteral("before"), QJsonObject{{QStringLiteral("jobState"), jobState},
                    {QStringLiteral("sourceRuntimeStamp"), jobStamp}}},
                {QStringLiteral("after"), QJsonObject{{QStringLiteral("cancellationRequested"), true},
                    {QStringLiteral("terminalStateDeterminedBySolver"), true}}},
                {QStringLiteral("changedPaths"), QJsonArray{}},
                {QStringLiteral("readSet"), QJsonArray{QStringLiteral("native.job:%1").arg(jobId),
                    QStringLiteral("native.job.sourceRuntimeStamp"), QStringLiteral("host.sessionEpoch")}},
                {QStringLiteral("readSetHash"), preview.readSetHash},
                {QStringLiteral("previewId"), preview.id}, {QStringLiteral("digest"), preview.digest},
                {QStringLiteral("requiresHostApproval"), true},
                {QStringLiteral("history"), QStringLiteral("none")},
                {QStringLiteral("irreversible"), true}};
            if (state->previewOrder.size() >= 128)
            {
                const QString expired = state->previewOrder.takeFirst();
                state->previews.erase(expired);
            }
            state->previewOrder.append(preview.id);
            state->previews.emplace(preview.id, preview);
            return ok(result, QStringLiteral("preview"));
        }

        if (!exactKeys(parameters, standardKeys))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("command.preview requires target, expectedDocumentRevision, command, commandVersion and arguments."));
        QString documentId;
        QString targetEpoch;
        quint64 expectedRevision = 0;
        if (!targetFieldsValid(&documentId, &targetEpoch)
            || targetEpoch != state->sessionEpoch
            || !unsignedInteger(parameters.value(QStringLiteral("expectedDocumentRevision")), &expectedRevision))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The target revision is malformed or belongs to another host session."));
        const bool runCommand = command == QStringLiteral("analysis.run");
        const bool historyCommand = command == QStringLiteral("document.undo") || command == QStringLiteral("document.redo");
        if (!isCoreEditCommand(command) && !runCommand && !historyCommand)
            return invalid(QStringLiteral("UNSUPPORTED_CAPABILITY"), tr("This host does not support the requested versioned command."));
        if ((historyCommand && !arguments.isEmpty())
            || (runCommand && (!exactKeys(arguments, {QStringLiteral("warningsAccepted")})
                               || !arguments.value(QStringLiteral("warningsAccepted")).isBool()))
            || (isCoreEditCommand(command) && arguments.isEmpty()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The command arguments do not match the selected operation."));
        if (activeModalInteraction())
            return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before previewing a write or solver command."));
        if (isMutationBlocked())
            return invalid(QStringLiteral("MODEL_BUSY"), tr("The target model is held by an active legacy task."));
        uint modelID = session->findModelByCommandIdentity(documentId);
        if (modelID == RConstants::eod)
            return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
        QString projectFailure;
        if (!session->canMutateDocument(modelID, &projectFailure))
            return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
        auto stamp = session->modelCommandStamp(modelID);
        if (stamp.first != documentId || stamp.second != expectedRevision)
            return invalid(QStringLiteral("STALE_REVISION"), tr("Query the current document revision and create a new preview."));

        // SolverManager performs its own non-blocking Session lock while it
        // validates a live model. Do not call it from the host's locked model
        // read below: QMutex is not recursive, so that would make a valid
        // source look stale simply because the host already owns the mutex.
        QJsonObject runValidation;
        if (runCommand)
        {
            runValidation = SolverManager::getInstance().validateRun(modelID);
            if (runValidation.value(QStringLiteral("sourceStamp")).toObject()
                != sourceStampJson(stamp, state->sessionEpoch))
                return invalid(QStringLiteral("STALE_REVISION"), tr("The solver validation source changed during preview."));
            if (!runValidation.value(QStringLiteral("valid")).toBool()
                || !runValidation.value(QStringLiteral("errors")).toArray().isEmpty())
            {
                QJsonObject response = invalid(QStringLiteral("SOLVER_VALIDATION_FAILED"),
                    tr("The native solver rejected this model during preview."));
                response.insert(QStringLiteral("validation"), runValidation);
                return response;
            }
            if (!runValidation.value(QStringLiteral("warnings")).toArray().isEmpty()
                && !arguments.value(QStringLiteral("warningsAccepted")).toBool())
            {
                QJsonObject response = invalid(QStringLiteral("WARNINGS_NOT_ACCEPTED"),
                    tr("Explicit warningsAccepted=true is required before this solver run can be approved."));
                response.insert(QStringLiteral("warnings"), runValidation.value(QStringLiteral("warnings")));
                return response;
            }
        }

        State::Preview preview;
        QJsonObject nativePreview;
        QJsonObject before;
        QJsonObject after;
        QJsonArray changedPaths;
        QJsonArray readSet{QStringLiteral("/document/revision")};
        QString candidateFingerprint;
        QString inputFingerprint;
        QString nativeRevision;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The model could not be read safely for preview."));
            modelID = session->findModelByCommandIdentity(documentId);
            if (modelID == RConstants::eod)
                return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            stamp = session->modelCommandStamp(modelID);
            if (stamp.first != documentId || stamp.second != expectedRevision)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed while the preview was being prepared."));
            QString currentProjectFailure;
            if (!session->canMutateDocument(modelID, &currentProjectFailure))
                return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"),
                    currentProjectFailure);
            if (runCommand
                && runValidation.value(QStringLiteral("sourceStamp")).toObject()
                    != sourceStampJson(stamp, state->sessionEpoch))
                return invalid(QStringLiteral("STALE_REVISION"), tr("The solver validation source changed while the preview was being prepared."));
            const Model &model = session->getModel(modelID);
            auto *entry = state->ensure(session, modelID);
            if (!entry) return invalid(QStringLiteral("SESSION_UNAVAILABLE"), state->failure);
            if (!entry->editing->setRunning(false))
                return invalid(QStringLiteral("JOURNAL_UNAVAILABLE"), tr("Cannot record model availability."));
            const QJsonObject nativeQuery = entry->editing->query();
            nativeRevision = nativeQuery.value(QStringLiteral("revision")).toString();
            inputFingerprint = RModelCommandService::stateFingerprint(model);

            if (isCoreEditCommand(command))
            {
                const auto core = RModelCommandService::preview(model, command, arguments,
                    documentId, QString::number(stamp.second));
                if (!core.valid)
                    return invalid(core.code.toUpper(), core.message);
                preview.delta = core.delta;
                preview.hasSupportedDelta = true;
                preview.arguments = core.normalizedArguments;
                before = core.before;
                after = core.after;
                changedPaths = command == QStringLiteral("analysis.modal.configure")
                    ? QJsonArray{QStringLiteral("analysis.modal")}
                    : (core.changedPaths.isEmpty() ? QJsonArray{core.delta.changedPath} : core.changedPaths);
                candidateFingerprint = core.fingerprint;
                readSet.append(QStringLiteral("/analysis/taskTree"));
                readSet.append(QStringLiteral("/problemSetup"));
                if (core.delta.physicalId)
                    readSet.append(QStringLiteral("/entityGroups/g:%1").arg(core.delta.physicalId));
                for (const QJsonValue &path : changedPaths) readSet.append(path);
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("normalizedArguments"), core.normalizedArguments},
                    {QStringLiteral("before"), core.before}, {QStringLiteral("after"), core.after},
                    {QStringLiteral("changedPaths"), changedPaths},
                    {QStringLiteral("candidateFingerprint"), core.fingerprint},
                    {QStringLiteral("diagnostics"), core.diagnostics}};
            }
            else if (runCommand)
            {
                const bool warningsAccepted = arguments.value(QStringLiteral("warningsAccepted")).toBool();
                preview.arguments = arguments;
                preview.validation = runValidation;
                preview.projectSnapshotRef = session->documentProjectContext(modelID);
                before = {{QStringLiteral("sourceRuntimeStamp"), sourceStampJson(stamp, state->sessionEpoch)},
                          {QStringLiteral("validation"), runValidation}};
                after = {{QStringLiteral("jobState"), QStringLiteral("queued")},
                         {QStringLiteral("warningsAccepted"), warningsAccepted},
                         {QStringLiteral("modelRevisionChanged"), false}};
                readSet.append(QStringLiteral("/solver.validation"));
                readSet.append(QStringLiteral("/project.persistentDocument"));
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("validation"), runValidation},
                    {QStringLiteral("warningsAccepted"), warningsAccepted}};
            }
            else
            {
                const bool undo = command == QStringLiteral("document.undo");
                const bool available = undo ? model.getUndoStackSize() > 0 : model.getRedoStackSize() > 0;
                if (!available)
                    return invalid(QStringLiteral("EMPTY_HISTORY"), tr("No matching operation is available in the document history."));
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("historyAction"), undo ? model.getUndoActionMessage() : model.getRedoActionMessage()},
                    {QStringLiteral("historyDepth"), double(undo ? model.getUndoStackSize() : model.getRedoStackSize())}};
                before = {{QStringLiteral("historyAction"), nativePreview.value(QStringLiteral("historyAction"))},
                          {QStringLiteral("historyDepth"), nativePreview.value(QStringLiteral("historyDepth"))}};
                after = {{QStringLiteral("historyState"), QStringLiteral("computed_on_commit")}};
                changedPaths = QJsonArray{QStringLiteral("document.history")};
                readSet.append(QStringLiteral("/document/history"));
            }
            preview.id = uuid();
            preview.documentId = documentId;
            preview.sessionEpoch = state->sessionEpoch;
            preview.command = command;
            preview.stampGeneration = stamp.second;
            preview.nativeRevision = nativeRevision;
            if (!isCoreEditCommand(command) && !runCommand) preview.arguments = arguments;
        }
        preview.nativePreview = nativePreview;
        preview.inputFingerprint = inputFingerprint;
        preview.readSet = readSet;
        QJsonObject readSetSource{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("revision"), QString::number(expectedRevision)},
            {QStringLiteral("inputStateFingerprint"), inputFingerprint},
            {QStringLiteral("readSet"), readSet}};
        preview.readSetHash = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(readSetSource).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QJsonObject digestInput{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("expectedDocumentRevision"), double(expectedRevision)},
            {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("submittedArguments"), arguments},
            {QStringLiteral("normalizedArguments"), preview.arguments},
            {QStringLiteral("before"), before}, {QStringLiteral("after"), after},
            {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("candidateFingerprint"), candidateFingerprint},
            {QStringLiteral("inputStateFingerprint"), inputFingerprint},
            {QStringLiteral("readSetHash"), preview.readSetHash},
            {QStringLiteral("nativeRevision"), nativeRevision}};
        preview.digest = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(digestInput).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QJsonObject result{{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                                                                   {QStringLiteral("sessionEpoch"), state->sessionEpoch}}},
            {QStringLiteral("runtimeStamp"), sourceStampJson(stamp, state->sessionEpoch)},
            {QStringLiteral("expectedDocumentRevision"), double(expectedRevision)},
            {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("arguments"), preview.arguments},
            {QStringLiteral("normalizedArguments"), preview.arguments},
            {QStringLiteral("submittedArguments"), arguments}, {QStringLiteral("before"), before},
            {QStringLiteral("after"), after}, {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("readSet"), readSet}, {QStringLiteral("readSetHash"), preview.readSetHash},
            {QStringLiteral("inputStateFingerprint"), inputFingerprint},
            {QStringLiteral("candidateFingerprint"), candidateFingerprint},
            {QStringLiteral("invalidatedResultsAvailability"), QStringLiteral("unknown_not_exposed")},
            {QStringLiteral("previewId"), preview.id}, {QStringLiteral("digest"), preview.digest},
            {QStringLiteral("nativeModalSessionRevision"), preview.nativeRevision}};
        if (runCommand)
        {
            result.insert(QStringLiteral("validation"), preview.validation);
            result.insert(QStringLiteral("requiresHostApproval"), true);
            result.insert(QStringLiteral("history"), QStringLiteral("none"));
            result.insert(QStringLiteral("sideEffect"), QStringLiteral("submit_native_solver_job"));
            result.insert(QStringLiteral("irreversible"), true);
        }
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
        if (found == state->operations.end())
        {
            auto *catalog = session->getProjectCatalog();
            QString error;
            const auto persisted = catalog ? catalog->queryOperation(parameters.value("operationId").toString(), {}, &error)
                                           : std::optional<RProjectCatalog::DurableOperation>{};
            if (!persisted) return invalid(QStringLiteral("OPERATION_NOT_FOUND"), error.isEmpty() ? tr("No durable operation with this ID is available.") : error);
            return ok(durableOperationJson(*persisted));
        }
        return ok(found->second);
    }

    if (operation == QStringLiteral("operations.query"))
    {
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            if (it.key() != "limit" && it.key() != "afterOperationId")
                return invalid("INVALID_PARAMETERS", tr("Unknown operations query field."));
        const int limit = parameters.value("limit").toInt(100);
        if ((parameters.contains("limit") && (!parameters.value("limit").isDouble()
                || parameters.value("limit").toDouble() != double(limit))) || limit < 1 || limit > 100
            || (parameters.contains("afterOperationId") && !validToken(parameters.value("afterOperationId"))))
            return invalid("INVALID_PARAMETERS", tr("Operations page limit must be 1–100."));
        auto *catalog = session->getProjectCatalog();
        if (!catalog) return invalid("PERSISTENCE_UNAVAILABLE", tr("The project operations ledger is unavailable."));
        QString error;
        const auto page = catalog->queryOperations(limit, parameters.value("afterOperationId").toString(), {}, &error);
        if (!error.isEmpty()) return invalid("PERSISTENCE_ERROR", error);
        QJsonArray items;
        for (const auto &item : page.operations) items.append(durableOperationJson(item));
        return ok({{"operations", items}, {"nextCursor", page.nextCursor}, {"persistence", "durable"}});
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
        auto *catalog = session->getProjectCatalog();
        if (!catalog) return invalid("PERSISTENCE_UNAVAILABLE", tr("The project operations ledger is unavailable."));
        const QJsonObject normalizedRequest{{"operation", operation}, {"previewId", previewId}};
        const QString durableRequestDigest = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(normalizedRequest).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QString ledgerError;
        const auto durablePrior = catalog->queryOperationByIdempotencyKey(idempotencyKey, {}, &ledgerError);
        if (!ledgerError.isEmpty()) return invalid("PERSISTENCE_ERROR", ledgerError);
        if (durablePrior)
        {
            if (durablePrior->normalizedRequestSha256 != durableRequestDigest)
                return invalid("IDEMPOTENCY_CONFLICT", tr("This key is durably bound to a different request."));
            if (durablePrior->phase == RProjectCatalog::OperationPhase::Observed
                && !durablePrior->association.observation.isEmpty())
            {
                const auto observed = durablePrior->association.observation;
                auto response = hostResponse(requestId, observed.value("status").toString(),
                    observed.value("code").toString(), observed.value("result").toObject());
                if (observed.contains("message")) response.insert("message", observed.value("message"));
                return response;
            }
            return invalid("OPERATION_RECONCILE_REQUIRED", tr("The earlier operation is unobserved; query its durable record before any new action."));
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
        RProjectCatalog::OperationAssociation initialAssociation;
        initialAssociation.candidateSha256 = preview.nativePreview.value("candidateFingerprint").toString();
        initialAssociation.approvalSummarySha256 = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(QJsonObject{{"command", preview.command}, {"arguments", preview.arguments}})
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        const auto durableIntent = catalog->beginOperation(idempotencyKey, normalizedRequest, initialAssociation, {}, &ledgerError);
        if (durableIntent.disposition == RProjectCatalog::OperationDisposition::Reused)
        {
            const auto &record = durableIntent.operation;
            if (record.phase == RProjectCatalog::OperationPhase::Observed && !record.association.observation.isEmpty())
            {
                const auto observed = record.association.observation;
                auto response = hostResponse(requestId, observed.value("status").toString(),
                    observed.value("code").toString(), observed.value("result").toObject());
                if (observed.contains("message")) response.insert("message", observed.value("message"));
                return response;
            }
            return invalid("OPERATION_RECONCILE_REQUIRED", tr("Another runtime recorded this operation; query its outcome before any new action."));
        }
        if (durableIntent.operation.operationId.isEmpty() || durableIntent.disposition != RProjectCatalog::OperationDisposition::Created)
            return invalid("PERSISTENCE_ERROR", ledgerError.isEmpty() ? tr("Could not persist this operation intent.") : ledgerError);
        const QString operationId = durableIntent.operation.operationId;
        durableIntentId = operationId;
        const QString previousActiveOperation = state->activeOperationId;
        const QJsonArray previousChangedPaths = state->activeChangedPaths;
        auto finishCompleted = [&](const QJsonObject &operationResult, const QString &status,
                                   const QString &code, const QString &message) {
            QJsonObject observedResult = operationResult;
            RProjectCatalog::OperationAssociation association = initialAssociation;
            const uint currentModel = session->findModelByCommandIdentity(preview.documentId);
            QString snapshotError;
            if (status == "accepted" && observedResult.contains("afterRevision") && currentModel != RConstants::eod)
            {
                const auto snapshot = session->createDocumentSnapshot(currentModel, &snapshotError);
                if (!snapshot.isEmpty())
                {
                    observedResult.insert("acceptedSnapshotRef", snapshot);
                    observedResult.insert("persistence", "accepted_snapshot");
                    association.acceptedRevision = snapshot.value("sourceRevision").toString();
                    association.acceptedModelSha256 = snapshot.value("modelSha256").toString();
                }
                else
                {
                    observedResult.insert("recoveryProtection", "memory_only");
                    observedResult.insert("recoveryDiagnostic", snapshotError);
                }
            }
            QJsonObject response = hostResponse(requestId, status, code, observedResult);
            if (!message.isEmpty()) response.insert(QStringLiteral("message"), message);
            association.observation = {{"status", status}, {"code", code}, {"result", observedResult}};
            if (!message.isEmpty()) association.observation.insert("message", message);
            association.diagnosticCode = status == "accepted" ? QString() : code.toLower();
            const bool acceptedRecorded = status != "accepted" || catalog->advanceOperation(
                operationId, RProjectCatalog::OperationPhase::Accepted, association, {}, &ledgerError);
            if (!acceptedRecorded || !catalog->advanceOperation(operationId, RProjectCatalog::OperationPhase::Observed,
                                            association, {}, &ledgerError))
            {
                response = hostResponse(requestId, "error", "OBSERVED_MODIFIED_UNLOGGED", observedResult);
                response.insert("message", tr("The operation happened, but its durable observation failed: %1").arg(ledgerError));
            }
            preview.consumed = true;
            if (state->operationOrder.size() >= 256)
            {
                const QString expired = state->operationOrder.takeFirst();
                state->operations.erase(expired);
            }
            state->operationOrder.append(operationId);
            state->operations.emplace(operationId, observedResult);
            if (state->idempotencyOrder.size() >= 256)
            {
                const QString expired = state->idempotencyOrder.takeFirst();
                state->idempotency.erase(expired);
            }
            state->idempotencyOrder.append(idempotencyKey);
            state->idempotency.emplace(idempotencyKey,
                State::IdempotentResult{requestDigest, preview.documentId, response});
            durableIntentId.clear();
            return response;
        };
        auto finishAccepted = [&](const QJsonObject &operationResult) {
            return finishCompleted(operationResult, QStringLiteral("accepted"), QStringLiteral("OK"), QString());
        };
        auto finishModifiedUnlogged = [&](const QJsonObject &operationResult, const QString &message) {
            return finishCompleted(operationResult, QStringLiteral("error"),
                                   QStringLiteral("OBSERVED_MODIFIED_UNLOGGED"), message);
        };

        if (preview.command == "analysis.recover")
        {
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before native recovery."));
            const auto qualification = SolverManager::getInstance().nativeJobRecoveryQuery(preview.jobId);
            if (qualification != preview.recoveryQualification)
                return invalid("RECOVERY_CHANGED", tr("Checkpoint qualification changed after preview; a new approval is required."));
            QString error;
            const QString attempt = SolverManager::getInstance().submitNativeRecovery(preview.jobId,
                preview.arguments.value("checkpointSha256").toString(), uint(preview.arguments.value("remainingSteps").toInt()), &error);
            if (attempt.isEmpty()) return invalid("RECOVERY_REJECTED", error);
            durableSideEffect = true;
            return finishAccepted({{"operationId", operationId}, {"command", preview.command},
                {"jobId", attempt}, {"recoverySourceJobId", preview.jobId},
                {"checkpointSha256", preview.arguments.value("checkpointSha256")},
                {"remainingSteps", preview.arguments.value("remainingSteps")},
                {"sourceDocumentModified", false}, {"newAttempt", true}, {"history", "none"}});
        }

        if (preview.command == QStringLiteral("analysis.cancel"))
        {
            if (activeModalInteraction())
                return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before cancelling a solver job."));
            const auto owned = state->jobSources.find(preview.jobId);
            const QJsonObject currentJob = SolverManager::getInstance().jobQuery(preview.jobId);
            const QJsonObject currentSource = currentJob.value(QStringLiteral("sourceRuntimeStamp")).toObject();
            const QString jobState = currentJob.value(QStringLiteral("state")).toString();
            if (!currentJob.value(QStringLiteral("found")).toBool()
                || owned == state->jobSources.end()
                || owned->second.documentId != preview.jobSourceDocumentId
                || owned->second.sessionEpoch != preview.sessionEpoch
                || owned->second.revision != preview.jobSourceRevision
                || currentSource.value(QStringLiteral("documentId")).toString() != preview.jobSourceDocumentId
                || currentSource.value(QStringLiteral("revision")).toString() != preview.jobSourceRevision
                || (!currentSource.value(QStringLiteral("sessionEpoch")).toString().isEmpty()
                    && currentSource.value(QStringLiteral("sessionEpoch")).toString() != preview.sessionEpoch))
                return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The job no longer matches its host-owned immutable source."));
            if (jobState != QStringLiteral("queued") && jobState != QStringLiteral("running")
                && jobState != QStringLiteral("cancelling"))
                return invalid(QStringLiteral("JOB_TERMINAL"), tr("The selected native job is already terminal."));
            QString cancelError;
            if (!SolverManager::getInstance().cancelJob(preview.jobId, &cancelError))
                return invalid(QStringLiteral("CANCEL_REJECTED"), cancelError);
            durableSideEffect = true;
            QJsonObject afterJob = SolverManager::getInstance().jobQuery(preview.jobId);
            addHostJobSource(&afterJob, preview.jobId);
            QJsonObject event = state->appendEvent(QStringLiteral("analysis.job_cancellation_requested"),
                preview.jobSourceDocumentId, {{QStringLiteral("jobId"), preview.jobId},
                    {QStringLiteral("sourceRevision"), preview.jobSourceRevision},
                    {QStringLiteral("cancellationRequested"), true}});
            emit eventPublished(event);
            QJsonObject operationResult{{QStringLiteral("operationId"), operationId},
                {QStringLiteral("command"), preview.command}, {QStringLiteral("jobId"), preview.jobId},
                {QStringLiteral("sourceDocumentId"), preview.jobSourceDocumentId},
                {QStringLiteral("sourceSessionEpoch"), preview.sessionEpoch},
                {QStringLiteral("sourceRevision"), preview.jobSourceRevision},
                {QStringLiteral("job"), afterJob},
                {QStringLiteral("cancellationState"), afterJob.value(QStringLiteral("state"))},
                {QStringLiteral("cancellationRequested"), true},
                {QStringLiteral("terminalStateDeterminedBySolver"), true},
                {QStringLiteral("sideEffect"), QStringLiteral("cancel_specific_native_job")},
                {QStringLiteral("irreversible"), true},
                {QStringLiteral("history"), QStringLiteral("none")},
                {QStringLiteral("eventSeq"), double(state->nextEventSeq - 1)}};
            state->activeOperationId = previousActiveOperation;
            state->activeChangedPaths = previousChangedPaths;
            return finishAccepted(operationResult);
        }

        if (preview.command == QStringLiteral("analysis.run"))
        {
            if (activeModalInteraction())
                return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before starting a solver job."));
            if (isMutationBlocked())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The source model is held by an active legacy task."));
            uint modelID = session->findModelByCommandIdentity(preview.documentId);
            if (modelID == RConstants::eod)
                return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            QString projectFailure;
            if (!session->canMutateDocument(modelID, &projectFailure))
                return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
            auto stamp = session->modelCommandStamp(modelID);
            if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview. Query and preview the command again."));
            QJsonObject validation = SolverManager::getInstance().validateRun(modelID);
            if (!validation.value(QStringLiteral("valid")).toBool()
                || !validation.value(QStringLiteral("errors")).toArray().isEmpty()
                || validation.value(QStringLiteral("sourceStamp")).toObject() != sourceStampJson(stamp, state->sessionEpoch))
                return invalid(QStringLiteral("SOLVER_VALIDATION_FAILED"), tr("The native solver no longer accepts the approved source state."));
            const bool warningsAccepted = preview.arguments.value(QStringLiteral("warningsAccepted")).toBool();
            if (!validation.value(QStringLiteral("warnings")).toArray().isEmpty() && !warningsAccepted)
                return invalid(QStringLiteral("WARNINGS_NOT_ACCEPTED"), tr("The approved command did not accept the current solver warnings."));

            // Snapshot capture acquires/releases the Session mutex internally.
            QString snapshotError;
            const QJsonObject projectSnapshotRef = session->createDocumentSnapshot(modelID, &snapshotError);
            if (projectSnapshotRef.isEmpty())
                return invalid(QStringLiteral("SOURCE_SNAPSHOT_FAILED"), snapshotError);
            const QJsonObject instanceRef = projectSnapshotRef.value(QStringLiteral("instanceRef")).toObject();
            if (instanceRef.value(QStringLiteral("documentId")).toString() != preview.documentId
                || instanceRef.value(QStringLiteral("sessionEpoch")).toString() != preview.sessionEpoch
                || instanceRef.value(QStringLiteral("documentRevision")).toDouble(-1.0) != double(preview.stampGeneration)
                || session->modelCommandStamp(modelID) != stamp)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The source changed while its project snapshot was being captured."));
            const QJsonObject approvedProject = preview.projectSnapshotRef;
            const QJsonObject currentProject = session->documentProjectContext(modelID);
            for (const QString &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
                                         QStringLiteral("sourceRevision")})
                if (approvedProject.value(field) != currentProject.value(field))
                    return invalid(QStringLiteral("PROJECT_CONFLICT"), tr("The persistent project identity changed after preview."));
            if (!approvedProject.value(QStringLiteral("modelSha256")).toString().isEmpty()
                && approvedProject.value(QStringLiteral("modelSha256")) != currentProject.value(QStringLiteral("modelSha256")))
                return invalid(QStringLiteral("PROJECT_CONFLICT"), tr("The accepted project model hash changed after preview."));

            QString submitError;
            const QString jobId = SolverManager::getInstance().submitValidated(
                modelID, projectSnapshotRef, warningsAccepted, &submitError);
            if (jobId.isEmpty())
                return invalid(QStringLiteral("SUBMISSION_REJECTED"), submitError);
            durableSideEffect = true;
            state->jobSources[jobId] = State::JobSource{preview.documentId, preview.sessionEpoch,
                QString::number(preview.stampGeneration), projectSnapshotRef};
            QJsonObject job = SolverManager::getInstance().jobQuery(jobId);
            addHostJobSource(&job, jobId);
            QJsonObject event = state->appendEvent(QStringLiteral("analysis.job_submitted"), preview.documentId,
                {{QStringLiteral("jobId"), jobId},
                 {QStringLiteral("sourceRevision"), QString::number(preview.stampGeneration)},
                 {QStringLiteral("sourceSnapshotRef"), projectSnapshotRef}});
            emit eventPublished(event);
            QJsonObject operationResult{{QStringLiteral("operationId"), operationId},
                {QStringLiteral("command"), preview.command}, {QStringLiteral("jobId"), jobId},
                {QStringLiteral("sourceDocumentId"), preview.documentId},
                {QStringLiteral("sourceSessionEpoch"), preview.sessionEpoch},
                {QStringLiteral("sourceRevision"), QString::number(preview.stampGeneration)},
                {QStringLiteral("sourceSnapshotRef"), projectSnapshotRef},
                {QStringLiteral("job"), job},
                {QStringLiteral("progress"), job.value(QStringLiteral("progress"))},
                {QStringLiteral("progressStatus"), job.value(QStringLiteral("progressStatus"))},
                {QStringLiteral("cancellationState"), job.value(QStringLiteral("cancellationState"))},
                {QStringLiteral("sideEffect"), QStringLiteral("submit_native_solver_job")},
                {QStringLiteral("irreversible"), true},
                {QStringLiteral("history"), QStringLiteral("none")},
                {QStringLiteral("sourceRevisionChanged"), false},
                {QStringLiteral("eventSeq"), double(state->nextEventSeq - 1)}};
            state->activeOperationId = previousActiveOperation;
            state->activeChangedPaths = previousChangedPaths;
            return finishAccepted(operationResult);
        }

        if (activeModalInteraction())
            return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before changing document state."));
        if (isMutationBlocked())
            return invalid(QStringLiteral("MODEL_BUSY"), tr("The target model is held by an active legacy task."));
        uint modelID = session->findModelByCommandIdentity(preview.documentId);
        if (modelID == RConstants::eod)
            return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
        QString projectFailure;
        if (!session->canMutateDocument(modelID, &projectFailure))
            return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
        auto stamp = session->modelCommandStamp(modelID);
        if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
            return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview. Query and preview the command again."));
        const QJsonArray changedPaths = preview.hasSupportedDelta
            ? preview.nativePreview.value(QStringLiteral("changedPaths")).toArray()
            : QJsonArray{QStringLiteral("document.history")};
        bool auditRecorded = true;
        QString historyAction;
        QString corePreStateHash;
        QString corePostStateHash;
        QString coreJournalRevision;
        QString coreJournalRequestId;
        QString coreJournalIdempotencyKey;
        QString auditCommandId;
        QString preStateHash;
        QString postStateHash;
        QString auditError;
        bool auditReconciled = false;
        REntityGroupType changedGroupType = R_ENTITY_GROUP_NONE;
        uint changedGroupIndex = RConstants::eod;
        const bool coreEdit = preview.hasSupportedDelta;
        const bool undo = preview.command == QStringLiteral("document.undo");
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The document could not be changed safely."));
            modelID = session->findModelByCommandIdentity(preview.documentId);
            if (modelID == RConstants::eod)
                return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            stamp = session->modelCommandStamp(modelID);
            if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview."));
            auto &model = session->getModel(modelID);
            preStateHash = RModelCommandService::stateFingerprint(model);
            if (coreEdit)
            {
                const QString currentFingerprint = RModelCommandService::stateFingerprint(model);
                preStateHash = currentFingerprint;
                QJsonObject currentReadSetSource{{QStringLiteral("documentId"), preview.documentId},
                    {QStringLiteral("sessionEpoch"), preview.sessionEpoch},
                    {QStringLiteral("revision"), QString::number(stamp.second)},
                    {QStringLiteral("inputStateFingerprint"), currentFingerprint},
                    {QStringLiteral("readSet"), preview.readSet}};
                const QString currentReadSetHash = QString::fromLatin1(QCryptographicHash::hash(
                    QJsonDocument(currentReadSetSource).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
                if (currentFingerprint != preview.inputFingerprint || currentReadSetHash != preview.readSetHash)
                    return invalid(QStringLiteral("STALE_READ_SET"), tr("The approved model read set changed after preview."));
                const auto currentCandidate = RModelCommandService::preview(model, preview.command,
                    preview.arguments, preview.documentId, QString::number(stamp.second));
                if (!currentCandidate.valid
                    || currentCandidate.fingerprint != preview.nativePreview.value(QStringLiteral("candidateFingerprint")).toString())
                    return invalid(QStringLiteral("STALE_PREVIEW"), tr("The current model no longer produces the approved candidate fingerprint."));
                auto entry = state->entries.find(preview.documentId);
                if (entry == state->entries.end() || !entry->second.editing
                    || entry->second.generation != stamp.second)
                    return invalid(QStringLiteral("SESSION_UNAVAILABLE"), tr("The accepted command session is no longer current."));
                const QJsonObject editingQuery = entry->second.editing->query();
                if (editingQuery.value(QStringLiteral("revision")).toString() != preview.nativeRevision
                    || RModelCommandService::stateFingerprint(entry->second.editing->model()) != currentFingerprint)
                    return invalid(QStringLiteral("STALE_REVISION"), tr("The accepted command session no longer matches the approved live model."));

                coreJournalRequestId = uuid();
                coreJournalIdempotencyKey = uuid();
                QJsonObject nativeRequest{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                    {QStringLiteral("commandId"), preview.command}, {QStringLiteral("commandVersion"), 1},
                    {QStringLiteral("modelId"), editingQuery.value(QStringLiteral("modelId"))},
                    {QStringLiteral("expectedRevision"), preview.nativeRevision},
                    {QStringLiteral("requestId"), coreJournalRequestId},
                    {QStringLiteral("idempotencyKey"), coreJournalIdempotencyKey},
                    {QStringLiteral("parameters"), preview.arguments}, {QStringLiteral("dryRun"), false}};
                const QJsonObject acceptedCore = entry->second.editing->execute(nativeRequest);
                if (acceptedCore.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("accepted")))
                {
                    const QString coreCode = acceptedCore.value(QStringLiteral("code")).toString();
                    return invalid(coreCode == QStringLiteral("journal_failure")
                                       ? QStringLiteral("COMMAND_JOURNAL_FAILURE")
                                       : coreCode.isEmpty() ? QStringLiteral("COMMAND_REJECTED") : coreCode.toUpper(),
                                   acceptedCore.value(QStringLiteral("message")).toString());
                }
                durableSideEffect = true;
                coreJournalRevision = acceptedCore.value(QStringLiteral("revision")).toString();
                auditCommandId = preview.command;
                corePreStateHash = currentFingerprint;
                corePostStateHash = RModelCommandService::stateFingerprint(currentCandidate.candidate);
                if (entry->second.editing->query().value(QStringLiteral("revision")).toString() != coreJournalRevision
                    || RModelCommandService::stateFingerprint(entry->second.editing->model()) != corePostStateHash)
                {
                    QString reconcileError;
                    const bool reconciled = entry->second.editing->recordExternalChange(
                        QStringLiteral("external_edit"), model, &reconcileError);
                    entry->second.editing.reset();
                    return invalid(QStringLiteral("COMMAND_AUDIT_DIVERGED"), reconciled
                        ? tr("The core journal accepted a candidate that did not match the live model; the session was reconciled without changing the GUI model.")
                        : tr("The core journal accepted a candidate that did not match the live model, and reconciliation failed: %1").arg(reconcileError));
                }
                QString applyError;
                if (!RModelCommandService::applyDelta(model, preview.delta, true).valid)
                    return invalid(QStringLiteral("STALE_PREVIEW"), tr("The approved command no longer matches the current model fields."));
                QString undoMessage;
                if (preview.command == QStringLiteral("analysis.modal.configure")) undoMessage = tr("Configure modal analysis");
                else if (preview.command == QStringLiteral("analysis.structural_dynamics.configure")) undoMessage = tr("Configure structural dynamics");
                else if (preview.command == QStringLiteral("entity.material.set")) undoMessage = tr("Set material properties");
                else undoMessage = tr("Set boundary condition");
                if (preview.delta.physicalId
                    && !model.findElementGroupByPhysicalId(preview.delta.physicalId, &changedGroupType, &changedGroupIndex))
                    return invalid(QStringLiteral("STALE_TARGET"), tr("The physical target no longer exists."));
                session->storeCurentModelVersion(modelID, undoMessage);
                if (!commitSupportedDelta(model, preview.delta, &applyError))
                {
                    QString reconcileError;
                    const bool reconciled = entry->second.editing->recordExternalChange(
                        QStringLiteral("external_edit"), model, &reconcileError);
                    entry->second.editing.reset();
                    return invalid(QStringLiteral("COMMAND_COMMIT_DIVERGED"), reconciled
                        ? tr("The core journal accepted the command, but GUI publication failed before changing the model: %1").arg(applyError)
                        : tr("GUI publication failed and core audit reconciliation also failed: %1").arg(reconcileError));
                }
                if (RModelCommandService::stateFingerprint(model) != corePostStateHash)
                {
                    QString reconcileError;
                    const bool reconciled = entry->second.editing->recordExternalChange(
                        QStringLiteral("external_edit"), model, &reconcileError);
                    entry->second.editing.reset();
                    return invalid(QStringLiteral("COMMAND_COMMIT_DIVERGED"), reconciled
                        ? tr("The published model did not match its accepted core audit state.")
                        : tr("The published model differed from its core audit state and reconciliation failed: %1").arg(reconcileError));
                }
                postStateHash = corePostStateHash;
            }
            else
            {
                const uint depth = undo ? model.getUndoStackSize() : model.getRedoStackSize();
                historyAction = undo ? model.getUndoActionMessage() : model.getRedoActionMessage();
                if (depth == 0 || historyAction != preview.nativePreview.value(QStringLiteral("historyAction")).toString())
                    return invalid(QStringLiteral("STALE_PREVIEW"), tr("The selected GUI history item is no longer current."));
                session->getPickList().removeItems(modelID);
                if (undo) model.undo(0); else model.redo(0);
                durableSideEffect = true;
                postStateHash = RModelCommandService::stateFingerprint(model);
            }
            guard.unlock();
        }
        state->activeOperationId = operationId;
        state->activeChangedPaths = changedPaths;
        if (coreEdit && (preview.delta.kind == RModelCommandService::Delta::Kind::Material
                         || preview.delta.kind == RModelCommandService::Delta::Kind::Boundary))
        {
            if (preview.delta.kind == RModelCommandService::Delta::Kind::Material)
                session->setMaterialChanged(modelID, changedGroupType, changedGroupIndex);
            else
                session->setBoundaryConditionChanged(modelID, changedGroupType, changedGroupIndex);
        }
        else if (coreEdit)
        {
            session->setProblemChanged(modelID);
        }
        else
        {
            session->setModelChanged(modelID);
        }
        const QPair<QString, quint64> afterStamp = session->modelCommandStamp(
            session->findModelByCommandIdentity(preview.documentId));
        if (coreEdit)
        {
            auto entry = state->entries.find(preview.documentId);
            if (entry != state->entries.end() && entry->second.editing
                && entry->second.editing->query().value(QStringLiteral("revision")).toString() == coreJournalRevision
                && RModelCommandService::stateFingerprint(entry->second.editing->model())
                    == RModelCommandService::stateFingerprint(session->getModel(modelID)))
            {
                entry->second.generation = afterStamp.second;
                auditCommandId = preview.command;
            }
            else
            {
                auditError = tr("The model changed, but its accepted command journal state did not match the published GUI model.");
                auditRecorded = recordExternalChangeAfterNotification(
                    modelID, QStringLiteral("external_edit"), &auditError);
                auditReconciled = auditRecorded;
                if (auditRecorded) auditCommandId = QStringLiteral("model.external.external_edit");
            }
        }
        else
        {
            const QString externalReason = undo ? QStringLiteral("undo") : QStringLiteral("redo");
            auditRecorded = recordExternalChangeAfterNotification(modelID, externalReason, &auditError);
            auditCommandId = QStringLiteral("model.external.") + externalReason;
        }
        if (!auditRecorded)
            RLogger::error("The GUI model changed but its audit could not be completed: %s\n", auditError.toUtf8().constData());
        state->activeOperationId = previousActiveOperation;
        state->activeChangedPaths = previousChangedPaths;
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
            {QStringLiteral("auditRecorded"), auditRecorded},
            {QStringLiteral("auditCommandId"), auditCommandId},
            {QStringLiteral("preStateHash"), preStateHash},
            {QStringLiteral("postStateHash"), postStateHash},
            {QStringLiteral("auditReconciled"), auditReconciled},
            {QStringLiteral("history"), QStringLiteral("one_gui_undo_group")},
            {QStringLiteral("candidateFingerprint"), preview.nativePreview.value(QStringLiteral("candidateFingerprint"))}};
        if (coreEdit)
        {
            operationResult.insert(QStringLiteral("acceptedStateRevision"), QString::number(afterStamp.second));
            operationResult.insert(QStringLiteral("coreJournalRevision"), coreJournalRevision);
            operationResult.insert(QStringLiteral("coreJournalRequestId"), coreJournalRequestId);
            operationResult.insert(QStringLiteral("coreJournalIdempotencyKey"), coreJournalIdempotencyKey);
            operationResult.insert(QStringLiteral("coreJournalRecorded"), !coreJournalRevision.isEmpty());
        }
        else
            operationResult.insert(QStringLiteral("historyAction"), historyAction);
        if (!auditRecorded)
        {
            operationResult.insert(QStringLiteral("outcome"), QStringLiteral("observed_modified_unlogged"));
            operationResult.insert(QStringLiteral("auditError"), auditError);
            operationResult.insert(QStringLiteral("modelModified"), true);
            return finishModifiedUnlogged(operationResult,
                tr("The GUI model was changed, but its audit record could not be completed: %1").arg(auditError));
        }
        operationResult.insert(QStringLiteral("outcome"), QStringLiteral("accepted_and_audited"));
        return finishAccepted(operationResult);
    }

    return invalid(QStringLiteral("UNSUPPORTED_OPERATION"), tr("This range.host/v1 operation is not implemented by the GUI host."));
}

bool ModelCommandHost::approvePreview(const QString &previewId)
{
    if (QThread::currentThread() != thread() || activeModalInteraction()) return false;
    auto found = state->previews.find(previewId);
    if (found == state->previews.end() || found->second.approved || found->second.consumed
        || found->second.sessionEpoch != state->sessionEpoch) return false;
    if (found->second.command == "analysis.recover")
    {
        if (isMutationBlocked() || SolverManager::getInstance().nativeJobRecoveryQuery(found->second.jobId)
                != found->second.recoveryQualification) return false;
        found->second.approved = true;
        return true;
    }
    if (found->second.command == QStringLiteral("analysis.cancel"))
    {
        const QJsonObject job = SolverManager::getInstance().jobQuery(found->second.jobId);
        const QString jobState = job.value(QStringLiteral("state")).toString();
        const QJsonObject source = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        const auto owned = state->jobSources.find(found->second.jobId);
        if (!job.value(QStringLiteral("found")).toBool()
            || (jobState != QStringLiteral("queued") && jobState != QStringLiteral("running")
                && jobState != QStringLiteral("cancelling"))
            || source.value(QStringLiteral("documentId")).toString() != found->second.jobSourceDocumentId
            || source.value(QStringLiteral("revision")).toString() != found->second.jobSourceRevision
            || (!source.value(QStringLiteral("sessionEpoch")).toString().isEmpty()
                && source.value(QStringLiteral("sessionEpoch")).toString() != found->second.sessionEpoch)
            || owned == state->jobSources.end()
            || owned->second.documentId != found->second.jobSourceDocumentId
            || owned->second.sessionEpoch != found->second.sessionEpoch
            || owned->second.revision != found->second.jobSourceRevision)
            return false;
        found->second.approved = true;
        return true;
    }
    if (found->second.command == QStringLiteral("analysis.run"))
    {
        if (isMutationBlocked()) return false;
        const QString sourceDocumentId = found->second.documentId;
        const QString sourceEpoch = found->second.sessionEpoch;
        const quint64 expectedRevision = found->second.stampGeneration;
        const bool warningsAccepted = found->second.arguments.value(QStringLiteral("warningsAccepted")).toBool();
        uint modelID = session->findModelByCommandIdentity(sourceDocumentId);
        if (modelID == RConstants::eod) return false;
        QPair<QString, quint64> sourceStamp;
        QJsonObject approvedProject;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction()) return false;
            modelID = session->findModelByCommandIdentity(sourceDocumentId);
            if (modelID == RConstants::eod || !session->canMutateDocument(modelID)) return false;
            sourceStamp = session->modelCommandStamp(modelID);
            if (sourceStamp.first != sourceDocumentId || sourceStamp.second != expectedRevision)
                return false;
            approvedProject = found->second.projectSnapshotRef;
            const QJsonObject currentProject = session->documentProjectContext(modelID);
            for (const QString &field : {QStringLiteral("projectId"),
                                         QStringLiteral("persistentDocumentId"),
                                         QStringLiteral("sourceRevision")})
                if (approvedProject.value(field) != currentProject.value(field)) return false;
            if (!approvedProject.value(QStringLiteral("modelSha256")).toString().isEmpty()
                && approvedProject.value(QStringLiteral("modelSha256"))
                    != currentProject.value(QStringLiteral("modelSha256")))
                return false;
        }

        // validateRun also takes the Session mutex. It must run after the
        // host-owned read lock above has been released.
        const QJsonObject validation = SolverManager::getInstance().validateRun(modelID);
        if (!validation.value(QStringLiteral("valid")).toBool()
            || !validation.value(QStringLiteral("errors")).toArray().isEmpty()
            || validation.value(QStringLiteral("sourceStamp")).toObject()
                != sourceStampJson(sourceStamp, sourceEpoch))
            return false;
        if (!validation.value(QStringLiteral("warnings")).toArray().isEmpty()
            && !warningsAccepted)
            return false;

        // Recheck all live GUI gates after validation so that an intervening
        // edit, draft, project change, or legacy task invalidates approval.
        if (isMutationBlocked() || activeModalInteraction()) return false;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction()) return false;
            modelID = session->findModelByCommandIdentity(sourceDocumentId);
            if (modelID == RConstants::eod || !session->canMutateDocument(modelID)) return false;
            if (session->modelCommandStamp(modelID) != sourceStamp) return false;
            const QJsonObject currentProject = session->documentProjectContext(modelID);
            for (const QString &field : {QStringLiteral("projectId"),
                                         QStringLiteral("persistentDocumentId"),
                                         QStringLiteral("sourceRevision")})
                if (approvedProject.value(field) != currentProject.value(field)) return false;
            if (!approvedProject.value(QStringLiteral("modelSha256")).toString().isEmpty()
                && approvedProject.value(QStringLiteral("modelSha256"))
                    != currentProject.value(QStringLiteral("modelSha256")))
                return false;
        }
        found = state->previews.find(previewId);
        if (found == state->previews.end() || found->second.approved || found->second.consumed
            || found->second.sessionEpoch != state->sessionEpoch || found->second.sessionEpoch != sourceEpoch
            || found->second.documentId != sourceDocumentId
            || found->second.stampGeneration != sourceStamp.second)
            return false;
        found->second.approved = true;
        return true;
    }
    if (isMutationBlocked()) return false;
    uint modelID = session->findModelByCommandIdentity(found->second.documentId);
    if (modelID == RConstants::eod) return false;
    TrySessionLock guard(session);
    if (!guard.ownsLock() || isMutationBlocked()) return false;
    modelID = session->findModelByCommandIdentity(found->second.documentId);
    if (modelID == RConstants::eod) return false;
    if (!session->canMutateDocument(modelID)) return false;
    const auto stamp = session->modelCommandStamp(modelID);
    if (stamp.first != found->second.documentId || stamp.second != found->second.stampGeneration) return false;
    if (isCoreEditCommand(found->second.command))
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
    if (error) error->clear();
    if (QThread::currentThread() != thread()) return fail(tr("Model commands require the GUI thread."));
    if (isMutationBlocked() || activeModalInteraction()) return fail(tr("Wait until document interaction and live model tasks finish before changing modal settings."));
    if (modelIDs.isEmpty()) return fail(tr("No model is selected."));
    QList<RModelCommandService::Delta> deltas;
    QList<QPair<QString, quint64>> stamps;
    QList<State::Entry *> entries;
    QList<QJsonObject> nativeRequests;
    QList<QJsonObject> acceptedCommands;
    const QJsonObject arguments = RModelCommandService::modalParameters(setup);
    {
        QMutexLocker guard(session->getMutex());
        QSet<uint> unique;
        // Validate all current live models and their journal sessions first.
        for (const uint id : modelIDs)
        {
            if (unique.contains(id)) return fail(tr("A model was selected more than once."));
            unique.insert(id);
            QString projectFailure;
            if (!session->canMutateDocument(id, &projectFailure)) return fail(projectFailure);
            auto *entry = state->ensure(session, id);
            if (!entry) return fail(state->failure);
            if (!entry->editing->setRunning(false)) return fail(tr("Cannot record model availability."));
            const auto stamp = session->modelCommandStamp(id);
            const auto candidate = RModelCommandService::preview(session->getModel(id),
                QStringLiteral("analysis.modal.configure"), arguments, stamp.first,
                QString::number(stamp.second));
            if (!candidate.valid) return fail(candidate.message);
            const QJsonObject query = entry->editing->query();
            if (RModelCommandService::stateFingerprint(entry->editing->model())
                != RModelCommandService::stateFingerprint(session->getModel(id)))
                return fail(tr("The command journal snapshot does not match the live model."));
            nativeRequests.append(QJsonObject{
                {QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                {QStringLiteral("commandId"), QStringLiteral("analysis.modal.configure")},
                {QStringLiteral("commandVersion"), 1},
                {QStringLiteral("modelId"), query.value(QStringLiteral("modelId"))},
                {QStringLiteral("expectedRevision"), query.value(QStringLiteral("revision"))},
                {QStringLiteral("requestId"), uuid()},
                {QStringLiteral("idempotencyKey"), uuid()},
                {QStringLiteral("parameters"), candidate.normalizedArguments},
                {QStringLiteral("dryRun"), false}});
            deltas.append(candidate.delta);
            stamps.append(stamp);
            entries.append(entry);
        }

        // Journal every candidate before publishing any GUI model. If a later
        // journal append fails, close/reconcile the earlier shadow sessions;
        // no live model has changed yet.
        for (qsizetype i = 0; i < modelIDs.size(); ++i)
        {
            if (session->modelCommandStamp(modelIDs[i]) != stamps[i])
                return fail(tr("A model changed while modal candidates were being validated."));
            if (entries[i]->editing->query().value(QStringLiteral("revision")).toString()
                != nativeRequests[i].value(QStringLiteral("expectedRevision")).toString())
                return fail(tr("A model command journal changed while modal candidates were being validated."));
            const QJsonObject accepted = entries[i]->editing->execute(nativeRequests[i]);
            if (accepted.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("accepted")))
            {
                QStringList reconciliationErrors;
                for (qsizetype previous = 0; previous < i; ++previous)
                {
                    QString reconciliationError;
                    if (!entries[previous]->editing->recordExternalChange(
                            QStringLiteral("external_edit"), session->getModel(modelIDs[previous]),
                            &reconciliationError))
                        reconciliationErrors.append(reconciliationError);
                    entries[previous]->editing.reset();
                }
                const QString message = tr("The modal command journal rejected the operation; no GUI model was changed. %1")
                    .arg(accepted.value(QStringLiteral("message")).toString());
                if (!reconciliationErrors.isEmpty())
                    return fail(message + tr(" Earlier journal candidates were reconciled with errors: %1")
                        .arg(reconciliationErrors.join(QStringLiteral("; "))));
                return fail(message);
            }
            acceptedCommands.append(accepted);
        }

        for (qsizetype i = 0; i < modelIDs.size(); ++i)
        {
            if (session->modelCommandStamp(modelIDs[i]) != stamps[i])
            {
                QStringList reconciliationErrors;
                for (qsizetype prior = 0; prior < entries.size(); ++prior)
                {
                    QString reconciliationError;
                    if (!entries[prior]->editing->recordExternalChange(
                            QStringLiteral("external_edit"), session->getModel(modelIDs[prior]),
                            &reconciliationError))
                        reconciliationErrors.append(reconciliationError);
                    entries[prior]->editing.reset();
                }
                return fail(tr("A model changed after modal commands were journaled; no GUI model was changed. %1")
                    .arg(reconciliationErrors.join(QStringLiteral("; "))));
            }
            QString applyError;
            session->storeCurentModelVersion(modelIDs[i], tr("Configure modal analysis"));
            if (!commitSupportedDelta(session->getModel(modelIDs[i]), deltas[i], &applyError))
            {
                QStringList reconciliationErrors;
                for (qsizetype prior = 0; prior < entries.size(); ++prior)
                {
                    QString reconciliationError;
                    if (!entries[prior]->editing->recordExternalChange(
                            QStringLiteral("external_edit"), session->getModel(modelIDs[prior]),
                            &reconciliationError))
                        reconciliationErrors.append(reconciliationError);
                    entries[prior]->editing.reset();
                }
                return fail(tr("A journaled modal command could not be published to the GUI model: %1 %2")
                    .arg(applyError, reconciliationErrors.join(QStringLiteral("; "))));
            }
        }
    }
    for (qsizetype i = 0; i < modelIDs.size(); ++i)
    {
        session->setProblemChanged(modelIDs[i]);
        const QPair<QString, quint64> stamp = session->modelCommandStamp(modelIDs[i]);
        const QJsonObject accepted = acceptedCommands[i];
        if (!entries[i]->editing
            || entries[i]->editing->query().value(QStringLiteral("revision")).toString()
                != accepted.value(QStringLiteral("revision")).toString()
            || RModelCommandService::stateFingerprint(entries[i]->editing->model())
                != RModelCommandService::stateFingerprint(session->getModel(modelIDs[i])))
        {
            QString auditError;
            const bool reconciled = recordExternalChangeAfterNotification(
                modelIDs[i], QStringLiteral("external_edit"), &auditError);
            if (!reconciled)
            {
                const QString message = tr("Modal settings were applied, but their durable command state could not be synchronized: %1")
                    .arg(auditError);
                if (error) *error = message;
                emit commandRejected(message);
                return false;
            }
            continue;
        }
        entries[i]->generation = stamp.second;
    }
    return true;
}

bool ModelCommandHost::recordArtifactPublished(uint modelID, const QString &path, QString *error,
                                              const RProjectCatalog::PublicationLeasePtr &publicationLease)
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
    const bool closed = closeEditingDurably(stamp.first, error, publicationLease);
    if (closed) entry->editing.reset();
    if (!closed && error) *error = tr("The model was saved and journaled, but its close bookkeeping remains pending: %1").arg(*error);
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

bool ModelCommandHost::recordExternalChangeAfterNotification(uint modelID, const QString &reason,
                                                              QString *error)
{
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("External changes require the GUI thread.");
        return false;
    }
    QString failure;
    bool recorded = true;
    {
        QMutexLocker guard(session->getMutex());
        const auto stamp = session->modelCommandStamp(modelID);
        auto found = state->entries.find(stamp.first);
        if (found == state->entries.end() || !found->second.editing)
        {
            recorded = false;
            failure = tr("No open command session was available to journal the published model change.");
        }
        else
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
