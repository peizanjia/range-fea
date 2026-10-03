#include "model_command_host.h"

#include "application.h"
#include "session.h"
#include "solver_manager.h"
#include <rmodel_command_session.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QUuid>
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
}

struct ModelCommandHost::State
{
    struct Entry
    {
        quint64 generation = 0;
        std::unique_ptr<RModelCommandSession> editing;
    };
    std::map<QString, Entry> entries;
    QString journal;
    QString failure;

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
    connect(session, &Session::modelRemoved, this, [this](uint) { this->pruneClosedModels(); });
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

bool ModelCommandHost::isBusy()
{
    return SolverManager::getInstance().getNRunning() || SolverManager::getInstance().getNWaiting()
        || RJobManager::getInstance().getNRunning() || RJobManager::getInstance().getNWaiting();
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
    if (isBusy()) return rejected("model_busy", tr("Wait for active model tasks to finish."));
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
    if (isBusy())
    {
        const auto stamp = session->modelCommandStamp(modelID);
        auto found = state->entries.find(stamp.first);
        if (found != state->entries.end() && found->second.editing)
        {
            if (!found->second.editing->setRunning(true)) return rejected("journal_unavailable", tr("Cannot record the busy model."));
            return found->second.editing->execute(request);
        }
        return rejected("model_busy", tr("Wait for active model tasks to finish."));
    }
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

bool ModelCommandHost::configureModal(const QList<uint> &modelIDs, const RModalSetup &setup, QString *error)
{
    auto fail = [&](const QString &message) { if (error) *error = message; emit commandRejected(message); return false; };
    if (QThread::currentThread() != thread()) return fail(tr("Model commands require the GUI thread."));
    if (isBusy()) return fail(tr("Wait for active model tasks to finish before changing modal settings."));
    if (modelIDs.isEmpty()) return fail(tr("No model is selected."));
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
