#include "rmodel_command_session.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cmath>

#include <rbl_interaction_record_v1.h>
#include <rbl_utils.h>

namespace
{
constexpr qint64 MaxJournalBytes = 64 * 1024 * 1024;
constexpr int MaxJournalRecords = 100000;

struct JournalData
{
    QUuid projectId;
    QVector<RInteractionRecordV1> records;
};

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

QJsonObject sourceSnapshot(const RModelCommandSession::JournalOptions &options)
{
    QJsonObject snapshot;
    if (!options.sourcePath.isEmpty()) snapshot.insert(QStringLiteral("sourcePath"), options.sourcePath);
    if (!options.sourceSha256.isEmpty()) snapshot.insert(QStringLiteral("sourceSha256"), options.sourceSha256);
    return snapshot;
}

bool normalizeJournalPath(const QString &configuredPath, QString &path, QString *error)
{
    QFileInfo info(configuredPath);
    if (info.exists())
    {
        if (!info.isFile() || info.isSymLink())
        {
            if (error) *error = QStringLiteral("Journal path must be a regular non-symbolic file.");
            return false;
        }
        path = info.canonicalFilePath();
        if (!path.isEmpty()) return true;
        if (error) *error = QStringLiteral("Cannot resolve the interaction journal path.");
        return false;
    }
    if (info.isSymLink())
    {
        if (error) *error = QStringLiteral("Journal path cannot be a symbolic link.");
        return false;
    }
    const QString parent = info.dir().canonicalPath();
    if (parent.isEmpty() || info.fileName().isEmpty())
    {
        if (error) *error = QStringLiteral("Journal directory must exist and the file name must be valid.");
        return false;
    }
    path = QDir(parent).filePath(info.fileName());
    return true;
}

bool readJournal(const QString &path, JournalData &data, QString *error)
{
    QFileInfo info(path);
    if (!info.exists())
    {
        if (info.isSymLink())
        {
            if (error) *error = QStringLiteral("Journal path cannot be a symbolic link.");
            return false;
        }
        data.projectId = QUuid::createUuid();
        data.records.clear();
        return true;
    }
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > MaxJournalBytes)
    {
        if (error) *error = QStringLiteral("Existing interaction journal is not a valid bounded file.");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("Cannot read the existing interaction journal.");
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.isEmpty() || bytes.size() > MaxJournalBytes)
    {
        if (error) *error = QStringLiteral("Cannot read the complete interaction journal.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error) *error = QStringLiteral("Existing interaction journal is malformed or truncated.");
        return false;
    }
    const QJsonObject object = document.object();
    if (!exactKeys(object, {QStringLiteral("journalVersion"), QStringLiteral("projectId"),
                            QStringLiteral("records")})
        || !object.value(QStringLiteral("journalVersion")).isDouble()
        || object.value(QStringLiteral("journalVersion")).toDouble() != 1.0
        || !object.value(QStringLiteral("projectId")).isString()
        || !object.value(QStringLiteral("records")).isArray())
    {
        if (error) *error = QStringLiteral("Existing interaction journal has an unsupported envelope.");
        return false;
    }

    const QUuid projectId(object.value(QStringLiteral("projectId")).toString());
    const QJsonArray records = object.value(QStringLiteral("records")).toArray();
    if (projectId.isNull() || records.size() > MaxJournalRecords)
    {
        if (error) *error = QStringLiteral("Existing interaction journal metadata is invalid.");
        return false;
    }

    QVector<RInteractionRecordV1> parsed;
    parsed.reserve(records.size());
    QSet<QUuid> recordIds;
    QUuid previousId;
    for (qsizetype index = 0; index < records.size(); ++index)
    {
        if (!records.at(index).isObject())
        {
            if (error) *error = QStringLiteral("Existing interaction journal contains a non-record entry.");
            return false;
        }
        RInteractionRecordV1 record;
        QString recordError;
        const QByteArray recordBytes = QJsonDocument(records.at(index).toObject()).toJson(QJsonDocument::Compact);
        if (!RInteractionRecordV1::fromJson(recordBytes, record, &recordError)
            || record.projectId != projectId || recordIds.contains(record.recordId)
            || (index == 0 ? !record.parentRecordId.isNull() : record.parentRecordId != previousId))
        {
            if (error) *error = QStringLiteral("Existing interaction journal contains an invalid record chain.");
            return false;
        }
        recordIds.insert(record.recordId);
        previousId = record.recordId;
        parsed.append(std::move(record));
    }

    data.projectId = projectId;
    data.records = std::move(parsed);
    return true;
}

bool sha256File(const QString &path, QString &sha256, QString *error)
{
    QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink())
    {
        if (error) *error = QStringLiteral("Published artifact must be a regular non-symbolic file.");
        return false;
    }
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("Cannot read the published artifact for hashing.");
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
    {
        if (error) *error = QStringLiteral("Cannot hash the complete published artifact.");
        return false;
    }
    sha256 = QString::fromLatin1(hash.result().toHex());
    return true;
}

bool writeJournal(const QString &path, const JournalData &journal, QString *error)
{
    if (journal.records.size() > MaxJournalRecords)
    {
        if (error) *error = QStringLiteral("Interaction journal reached its record limit.");
        return false;
    }
    QJsonArray records;
    for (const RInteractionRecordV1 &record : journal.records)
    {
        QByteArray bytes;
        if (!record.toJson(bytes, error)) return false;
        records.append(QJsonDocument::fromJson(bytes).object());
    }
    const QJsonObject envelope{{QStringLiteral("journalVersion"), 1},
                              {QStringLiteral("projectId"), journal.projectId.toString(QUuid::WithoutBraces)},
                              {QStringLiteral("records"), records}};
    const QByteArray bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaxJournalBytes)
    {
        if (error) *error = QStringLiteral("Interaction journal reached its byte limit.");
        return false;
    }
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
    {
        if (error) *error = QStringLiteral("Cannot atomically publish the interaction journal.");
        return false;
    }
    return true;
}
}

struct RModelCommandSession::State
{
    explicit State(const RModel &source, JournalOptions options)
        : model(source), journal(std::move(options)) {}

    RModel model;
    quint64 generation = 0;
    QVector<RModelCommandService::Delta> undo;
    QVector<RModelCommandService::Delta> redo;
    QMap<QString, QPair<QJsonObject, QJsonObject>> ledger;
    JournalOptions journal;
    Lifecycle lifecycle = Lifecycle::Ready;
    QUuid runId;
    QJsonObject publicationIntent;
    bool publicationRecorded = false;
};

RModelCommandSession::RModelCommandSession(const RModel &model)
    : RModelCommandSession(model, JournalOptions{})
{}

RModelCommandSession::RModelCommandSession(const RModel &model, JournalOptions journal)
    : state(std::make_unique<State>(model, std::move(journal))),
      identity(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      ownerThread(QThread::currentThreadId())
{}

RModelCommandSession::~RModelCommandSession() = default;
const RModel &RModelCommandSession::model() const { return state->model; }

QString RModelCommandSession::revision() const
{
    return identity + QStringLiteral(":") + QString::number(state->generation);
}

QJsonObject RModelCommandSession::modalParameters(const RModalSetup &setup)
{
    return RModelCommandService::modalParameters(setup);
}

QJsonObject RModelCommandSession::query() const
{
    const QJsonObject capabilities = RModelCommandService::capabilities(state->model);
    const RProblemTypeMask taskMask = state->model.getProblemTaskTree().getProblemTypeMask();
    const bool supported = taskMask & R_PROBLEM_STRESS_MODAL;
    QString lifecycle = QStringLiteral("ready");
    if (state->lifecycle == Lifecycle::Running) lifecycle = QStringLiteral("running");
    else if (state->lifecycle == Lifecycle::Closed) lifecycle = QStringLiteral("closed");
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
            {QStringLiteral("modelId"), identity}, {QStringLiteral("revision"), revision()},
            {QStringLiteral("revisionScope"), QStringLiteral("owned-session")},
            {QStringLiteral("modal"), modalParameters(state->model.getProblemSetup().getModalSetup())},
            {QStringLiteral("canConfigureModal"), supported},
            {QStringLiteral("structuralDynamics"), RModelCommandService::structuralDynamicsParameters(
                state->model.getProblemSetup().getStructuralDynamicsSetup())},
            {QStringLiteral("capabilities"), capabilities},
            {QStringLiteral("supportedStateFingerprint"), RModelCommandService::stateFingerprint(state->model)},
            {QStringLiteral("canUndo"), !state->undo.isEmpty()},
            {QStringLiteral("canRedo"), !state->redo.isEmpty()},
            {QStringLiteral("remainingCommands"), MaxCommands - state->ledger.size()},
            {QStringLiteral("lifecycle"), lifecycle},
            {QStringLiteral("busy"), state->lifecycle == Lifecycle::Running}};
}

QJsonObject RModelCommandSession::capabilities() const
{
    return RModelCommandService::capabilities(state->model);
}

QJsonObject RModelCommandSession::context(const QString &sourceId, const QJsonObject &pageRequest) const
{
    return RModelCommandService::context(state->model, sourceId, revision(), pageRequest);
}

QJsonObject RModelCommandSession::targets(const QString &sourceId, const QJsonObject &pageRequest) const
{
    return RModelCommandService::targets(state->model, sourceId, revision(), pageRequest);
}

bool RModelCommandSession::setRunning(bool running)
{
    if (QThread::currentThreadId() != ownerThread || state->lifecycle == Lifecycle::Closed) return false;
    const Lifecycle next = running ? Lifecycle::Running : Lifecycle::Ready;
    if (state->lifecycle == next) return true;
    const QUuid runId = running ? QUuid::createUuid() : state->runId;
    const QString currentHash = RModelCommandService::stateFingerprint(state->model);
    const QJsonObject snapshot{{QStringLiteral("runId"), runId.toString(QUuid::WithoutBraces)}};
    if (!appendJournalEvent(running ? QStringLiteral("model.run.started") : QStringLiteral("model.run.released"),
                            running ? QStringLiteral("running") : QStringLiteral("ready"),
                            revision(), currentHash, currentHash, QJsonObject{}, snapshot,
                            QJsonArray{}, QJsonArray{})) return false;
    state->runId = running ? runId : QUuid{};
    state->lifecycle = next;
    return true;
}

bool RModelCommandSession::close()
{
    if (QThread::currentThreadId() != ownerThread) return false;
    if (state->lifecycle == Lifecycle::Closed) return true;
    const QString currentHash = RModelCommandService::stateFingerprint(state->model);
    if (!appendJournalEvent(QStringLiteral("model.session.close"), QStringLiteral("closed"),
                            revision(), currentHash, currentHash, QJsonObject{}, QJsonObject{},
                            QJsonArray{}, QJsonArray{})) return false;
    state->lifecycle = Lifecycle::Closed;
    return true;
}

bool RModelCommandSession::appendJournalEvent(const QString &commandId, const QString &resultStatus,
                                               const QString &modelRevision,
                                               const QString &preStateHash,
                                               const QString &postStateHash,
                                               const QJsonObject &parameters,
                                               const QJsonObject &selectionSnapshot,
                                               const QJsonArray &artifactRefs,
                                               const QJsonArray &diagnostics,
                                               QString *error) const
{
    const JournalOptions &options = state->journal;
    if (options.path.isEmpty()) return true;
    if (options.actorId.isEmpty() || options.actorId.size() > 128
        || options.actorId.contains(QLatin1Char('\n')) || options.actorId.contains(QLatin1Char('\r'))
        || options.sourcePath.size() > 4096
        || (options.sourcePath.isEmpty() != options.sourceSha256.isEmpty())
        || (!options.sourceSha256.isEmpty()
            && !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(options.sourceSha256).hasMatch()))
    {
        if (error) *error = QStringLiteral("Trusted interaction journal metadata is invalid.");
        return false;
    }

    QString path;
    if (!normalizeJournalPath(options.path, path, error)) return false;
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
    {
        if (error) *error = QStringLiteral("Interaction journal is busy.");
        return false;
    }

    JournalData journal;
    if (!readJournal(path, journal, error)) return false;
    if (journal.records.size() >= MaxJournalRecords)
    {
        if (error) *error = QStringLiteral("Interaction journal reached its record limit.");
        return false;
    }

    RInteractionRecordV1 record;
    record.recordId = QUuid::createUuid();
    if (!journal.records.isEmpty()) record.parentRecordId = journal.records.constLast().recordId;
    record.timestampUtc = QDateTime::currentDateTimeUtc();
    record.actorType = QStringLiteral("system");
    record.actorId = options.actorId;
    record.sessionId = QUuid(identity);
    record.projectId = journal.projectId;
    record.modelId = QUuid(identity);
    // The v1 token grammar starts with a letter; public revisions retain their
    // historical UUID:generation representation for API compatibility.
    record.modelRevision = QStringLiteral("session:") + modelRevision;
    record.runId = selectionSnapshot.contains(QStringLiteral("runId"))
                       ? QUuid(selectionSnapshot.value(QStringLiteral("runId")).toString()) : state->runId;
    record.workflowStage = QStringLiteral("modal_configuration");
    record.commandId = commandId;
    record.commandVersion = QStringLiteral("1.0.0");
    record.parameters = parameters;
    record.selectionSnapshot = selectionSnapshot;
    // InteractionRecord v1 forbids null payload values. Preserve the exact
    // nullable axis request as bounded JSON text, without changing that schema
    // or substituting zero for an explicitly disabled component.
    if (commandId == QStringLiteral("entity.boundary.set"))
    {
        const QJsonArray axes = parameters.value(QStringLiteral("values")).toArray();
        bool nullableAxis = false;
        for (const QJsonValue &axis : axes) nullableAxis |= axis.isNull();
        if (nullableAxis)
        {
            const QByteArray encoded = QJsonDocument(parameters).toJson(QJsonDocument::Compact);
            if (encoded.size() > 4096)
            {
                if (error) *error = QStringLiteral("Nullable axis parameters exceed the lossless audit encoding limit.");
                return false;
            }
            record.parameters = {{QStringLiteral("json"), QString::fromUtf8(encoded)}};
            record.selectionSnapshot.insert(QStringLiteral("parametersEncoding"), QStringLiteral("json-text/v1"));
        }
    }
    record.selectionSnapshot.insert(QStringLiteral("preRevision"), revision());
    record.selectionSnapshot.insert(QStringLiteral("postRevision"), modelRevision);
    // Hash scope is versioned independently from the v1 interaction envelope.
    // Existing records retain their original modal-only scope when appended.
    record.selectionSnapshot.insert(QStringLiteral("stateHashScope"), QStringLiteral("supported_model_state"));
    record.selectionSnapshot.insert(QStringLiteral("stateHashScopeVersion"), 2);
    if (!options.sourcePath.isEmpty())
        record.selectionSnapshot.insert(QStringLiteral("sourcePath"), options.sourcePath);
    if (!options.sourceSha256.isEmpty())
        record.selectionSnapshot.insert(QStringLiteral("sourceSha256"), options.sourceSha256);
    record.approvalState = QStringLiteral("not_required");
    record.preStateHash = preStateHash;
    record.postStateHash = postStateHash;
    record.resultStatus = resultStatus;
    record.diagnostics = diagnostics;
    record.artifactRefs = artifactRefs;
    record.applicationVersion = RVendor::version().toString();

    journal.records.append(record);
    return writeJournal(path, journal, error);
}

bool RModelCommandSession::recordExternalChange(const QString &reason, const RModel &changedModel,
                                               QString *error)
{
    if (error) error->clear();
    if (QThread::currentThreadId() != ownerThread || state->lifecycle == Lifecycle::Closed)
    {
        if (error) *error = QStringLiteral("External changes require an open session on its owning thread.");
        return false;
    }
    // The host mutation has already happened. No failure may leave this old
    // snapshot executable, including a bad host reason or unavailable audit.
    const QString previousRevision = revision();
    state->lifecycle = Lifecycle::Closed;
    if (!QSet<QString>{QStringLiteral("undo"), QStringLiteral("redo"),
                       QStringLiteral("reload"), QStringLiteral("external_edit")}.contains(reason))
    {
        if (error) *error = QStringLiteral("Unsupported external change reason; the stale session was closed.");
        return false;
    }
    const QString before = RModelCommandService::stateFingerprint(state->model);
    const QString after = RModelCommandService::stateFingerprint(changedModel);
    return appendJournalEvent(QStringLiteral("model.external.") + reason, QStringLiteral("external_changed"),
                              QStringLiteral("invalidated:") + previousRevision, before, after,
                              QJsonObject{}, QJsonObject{{QStringLiteral("reason"), reason},
                                                       {QStringLiteral("identityInvalidated"), true}},
                              QJsonArray{}, QJsonArray{}, error);
}

bool RModelCommandSession::recordPublicationIntent(const QString &outputPath, const QString &stagedPath,
                                                  QString *error)
{
    if (error) error->clear();
    if (QThread::currentThreadId() != ownerThread || state->lifecycle != Lifecycle::Ready
        || state->journal.path.isEmpty() || (!state->publicationIntent.isEmpty() && !state->publicationRecorded))
    {
        if (error) *error = QStringLiteral("Publication intent requires a ready journaled session without a pending intent.");
        return false;
    }
    QString destination, journalPath, stagedSha256;
    if (!normalizeJournalPath(outputPath, destination, error)
        || !normalizeJournalPath(state->journal.path, journalPath, error)
        || !sha256File(stagedPath, stagedSha256, error)) return false;
    if (destination.size() > 4096 || destination.compare(journalPath, Qt::CaseInsensitive) == 0
        || destination.compare(QFileInfo(stagedPath).canonicalFilePath(), Qt::CaseInsensitive) == 0)
    {
        if (error) *error = QStringLiteral("Publication target must be distinct from journal and staging files.");
        return false;
    }
    const bool existed = QFileInfo::exists(destination);
    QString previousSha256;
    if (existed && !sha256File(destination, previousSha256, error)) return false;
    const QJsonObject intent{{QStringLiteral("publicationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                             {QStringLiteral("outputPath"), destination},
                             {QStringLiteral("expectedOutputSha256"), stagedSha256},
                             {QStringLiteral("previousOutputExisted"), existed},
                             {QStringLiteral("previousOutputSha256"), previousSha256}};
    const QString currentHash = RModelCommandService::stateFingerprint(state->model);
    if (!appendJournalEvent(QStringLiteral("model.artifact.publish_intent"), QStringLiteral("publication_pending"),
                            revision(), currentHash, currentHash, QJsonObject{}, intent,
                            QJsonArray{}, QJsonArray{}, error)) return false;
    state->publicationIntent = intent;
    state->publicationRecorded = false;
    return true;
}

QJsonObject RModelCommandSession::reconcilePublications(const JournalOptions &options)
{
    const auto failure = [](const QString &message) {
        return QJsonObject{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                           {QStringLiteral("status"), QStringLiteral("rejected")},
                           {QStringLiteral("code"), QStringLiteral("reconciliation_failure")},
                           {QStringLiteral("message"), message}};
    };
    QString path, error;
    if (options.path.isEmpty() || !normalizeJournalPath(options.path, path, &error)
        || !QFileInfo::exists(path)) return failure(QStringLiteral("An existing interaction journal is required."));
    QLockFile journalLock(path + QStringLiteral(".lock"));
    journalLock.setStaleLockTime(0);
    if (!journalLock.tryLock(0)) return failure(QStringLiteral("Interaction journal is busy."));
    JournalData journal;
    if (!readJournal(path, journal, &error)) return failure(error);
    QSet<QString> resolved;
    QMap<QString, QString> previousConflicts;
    QSet<QString> intentIds;
    QVector<RInteractionRecordV1> pending;
    for (const auto &record : journal.records)
    {
        const QString id = record.selectionSnapshot.value(QStringLiteral("publicationId")).toString();
        if (record.commandId == QStringLiteral("model.artifact.published")) resolved.insert(id);
        else if (record.commandId == QStringLiteral("model.artifact.reconciled"))
        {
            if (record.resultStatus == QStringLiteral("conflict"))
                previousConflicts.insert(id, record.selectionSnapshot.value(QStringLiteral("observedOutputSha256")).toString());
            else resolved.insert(id);
        }
    }
    for (const auto &record : journal.records)
    {
        if (record.commandId != QStringLiteral("model.artifact.publish_intent")) continue;
        const auto snapshot = record.selectionSnapshot;
        const QString id = snapshot.value(QStringLiteral("publicationId")).toString();
        const QString expected = snapshot.value(QStringLiteral("expectedOutputSha256")).toString();
        const QString previous = snapshot.value(QStringLiteral("previousOutputSha256")).toString();
        const auto hashPattern = QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"));
        if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id || intentIds.contains(id)
            || !snapshot.value(QStringLiteral("outputPath")).isString()
            || !QDir::isAbsolutePath(snapshot.value(QStringLiteral("outputPath")).toString())
            || !hashPattern.match(expected).hasMatch()
            || !snapshot.value(QStringLiteral("previousOutputExisted")).isBool()
            || !snapshot.value(QStringLiteral("previousOutputSha256")).isString()
            || (snapshot.value(QStringLiteral("previousOutputExisted")).toBool()
                    ? !hashPattern.match(previous).hasMatch() : !previous.isEmpty()))
            return failure(QStringLiteral("Publication intent metadata is invalid."));
        intentIds.insert(id);
        if (!resolved.contains(id)) pending.append(record);
    }
    if (pending.size() > 256) return failure(QStringLiteral("Reconciliation is limited to 256 unfinished publications."));
    QJsonArray outcomes;
    const QUuid recoverySession = QUuid::createUuid();
    bool changed = false;
    for (const auto &intent : pending)
    {
        const auto snapshot = intent.selectionSnapshot;
        const QString target = snapshot.value(QStringLiteral("outputPath")).toString();
        QJsonObject outcome{{QStringLiteral("publicationId"), snapshot.value(QStringLiteral("publicationId"))},
                            {QStringLiteral("outputPath"), target},
                            {QStringLiteral("expectedOutputSha256"), snapshot.value(QStringLiteral("expectedOutputSha256"))}};
        QString normalized;
        if (!normalizeJournalPath(target, normalized, &error)
            || normalized.compare(target, Qt::CaseInsensitive) != 0
            || normalized.compare(path, Qt::CaseInsensitive) == 0)
        {
            outcome.insert(QStringLiteral("status"), QStringLiteral("unresolved_path"));
            outcomes.append(outcome);
            continue;
        }
        QLockFile targetLock(target + QStringLiteral(".lock"));
        targetLock.setStaleLockTime(0);
        if (!targetLock.tryLock(0))
        {
            outcome.insert(QStringLiteral("status"), QStringLiteral("busy"));
            outcomes.append(outcome);
            continue;
        }
        QString actual;
        const bool exists = QFileInfo::exists(target);
        if (exists && !sha256File(target, actual, &error))
        {
            outcome.insert(QStringLiteral("status"), QStringLiteral("unreadable"));
            outcomes.append(outcome);
            continue;
        }
        const QString expected = snapshot.value(QStringLiteral("expectedOutputSha256")).toString();
        const QString previous = snapshot.value(QStringLiteral("previousOutputSha256")).toString();
        const bool existed = snapshot.value(QStringLiteral("previousOutputExisted")).toBool();
        QString status = QStringLiteral("conflict");
        if (exists && actual == expected)
            status = existed && previous == expected ? QStringLiteral("already_present") : QStringLiteral("artifact_present");
        else if ((!exists && !existed) || (exists && existed && actual == previous))
            status = QStringLiteral("not_published");
        outcome.insert(QStringLiteral("status"), status);
        outcome.insert(QStringLiteral("observedOutputSha256"), actual);
        const QString publicationId = snapshot.value(QStringLiteral("publicationId")).toString();
        if (status == QStringLiteral("conflict") && previousConflicts.contains(publicationId)
            && previousConflicts.value(publicationId) == actual)
        {
            outcome.insert(QStringLiteral("auditRecorded"), true);
            outcome.insert(QStringLiteral("alreadyRecorded"), true);
            outcomes.append(outcome);
            continue;
        }

        // Preserve the original model/revision identity and bind this observation
        // to the intent. This is not a synthetic solver or configuration success.
        RInteractionRecordV1 record = intent;
        record.recordId = QUuid::createUuid();
        record.parentRecordId = journal.records.constLast().recordId;
        record.timestampUtc = QDateTime::currentDateTimeUtc();
        record.sessionId = recoverySession;
        record.actorId = options.actorId;
        record.commandId = QStringLiteral("model.artifact.reconciled");
        record.resultStatus = status;
        record.selectionSnapshot.insert(QStringLiteral("observedOutputSha256"), actual);
        record.selectionSnapshot.insert(QStringLiteral("observationOnly"), true);
        record.artifactRefs = QJsonArray{};
        if (!actual.isEmpty()) record.artifactRefs.append(QJsonObject{{QStringLiteral("artifactId"), QStringLiteral("observed-output")},
                                                                    {QStringLiteral("sha256"), actual}});
        journal.records.append(record);
        // Publish each observation while still holding that destination's lock.
        if (!writeJournal(path, journal, &error))
        {
            outcome.insert(QStringLiteral("auditRecorded"), false);
            outcomes.append(outcome);
            return {{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                    {QStringLiteral("status"), QStringLiteral("reconciliation_incomplete")},
                    {QStringLiteral("code"), QStringLiteral("journal_failure")},
                    {QStringLiteral("message"), error}, {QStringLiteral("observationOnly"), true},
                    {QStringLiteral("journalUpdated"), changed}, {QStringLiteral("outcomes"), outcomes}};
        }
        changed = true;
        outcome.insert(QStringLiteral("auditRecorded"), true);
        outcomes.append(outcome);
    }
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
            {QStringLiteral("status"), QStringLiteral("reconciled")},
            {QStringLiteral("observationOnly"), true}, {QStringLiteral("journalUpdated"), changed},
            {QStringLiteral("outcomes"), outcomes}};
}

bool RModelCommandSession::recordArtifactPublished(const QString &outputPath,
                                                    QString *outputSha256,
                                                    QString *error)
{
    if (outputSha256) outputSha256->clear();
    if (error) error->clear();
    if (QThread::currentThreadId() != ownerThread)
    {
        if (error) *error = QStringLiteral("Interaction journal access must use the session's owning thread.");
        return false;
    }
    if (state->journal.path.isEmpty())
    {
        if (error) *error = QStringLiteral("No interaction journal is configured.");
        return false;
    }
    QString canonicalOutput;
    QFileInfo outputInfo(outputPath);
    if (!outputInfo.exists() || !outputInfo.isFile() || outputInfo.isSymLink()
        || (canonicalOutput = outputInfo.canonicalFilePath()).isEmpty()
        || canonicalOutput.size() > 4096)
    {
        if (error) *error = QStringLiteral("Published artifact path is not a valid regular file.");
        return false;
    }
    QString sha256;
    if (!sha256File(canonicalOutput, sha256, error)) return false;
    if (outputSha256) *outputSha256 = sha256;
    QJsonObject snapshot{{QStringLiteral("outputPath"), canonicalOutput},
                          {QStringLiteral("outputSha256"), sha256}};
    if (!state->publicationIntent.isEmpty())
    {
        if (state->publicationIntent.value(QStringLiteral("outputPath")).toString() != canonicalOutput
            || state->publicationIntent.value(QStringLiteral("expectedOutputSha256")).toString() != sha256)
        {
            if (error) *error = QStringLiteral("Published artifact does not match the durable publication intent.");
            return false;
        }
        if (state->publicationRecorded) return true;
        snapshot.insert(QStringLiteral("publicationId"), state->publicationIntent.value(QStringLiteral("publicationId")));
    }
    const QJsonArray artifactRefs{QJsonObject{{QStringLiteral("artifactId"), QStringLiteral("model-output")},
                                               {QStringLiteral("sha256"), sha256}}};
    const QString currentStateHash = RModelCommandService::stateFingerprint(state->model);
    const bool written = appendJournalEvent(QStringLiteral("model.artifact.published"),
                                            QStringLiteral("published"), revision(), currentStateHash,
                                            currentStateHash, QJsonObject{}, snapshot, artifactRefs,
                                            QJsonArray{}, error);
    if (written && !state->publicationIntent.isEmpty()) state->publicationRecorded = true;
    return written;
}

QJsonObject RModelCommandSession::execute(const QJsonObject &request)
{
    auto makeResponse = [&](const QString &code, const QString &message) {
        return QJsonObject{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
            {QStringLiteral("status"), QStringLiteral("rejected")},
            {QStringLiteral("requestId"), token(request.value(QStringLiteral("requestId")))
                 ? request.value(QStringLiteral("requestId")) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("modelId"), identity}, {QStringLiteral("revision"), revision()},
            {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    };
    if (QThread::currentThreadId() != ownerThread)
        return makeResponse(QStringLiteral("wrong_thread"), QStringLiteral("Use this editing session only on its owning thread."));

    const QByteArray requestBytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    QJsonObject requestSnapshot = sourceSnapshot(state->journal);
    requestSnapshot.insert(QStringLiteral("commandSha256"), QString::fromLatin1(
        QCryptographicHash::hash(requestBytes, QCryptographicHash::Sha256).toHex()));
    auto rejected = [&](const QString &code, const QString &message) {
        QJsonObject response = makeResponse(code, message);
        QString safeCommand = QStringLiteral("application.request.reject");
        const QString rawCommand = request.value(QStringLiteral("commandId")).toString();
        if (rawCommand == QStringLiteral("analysis.modal.configure")
            || rawCommand == QStringLiteral("model.undo") || rawCommand == QStringLiteral("model.redo"))
            safeCommand = rawCommand;
        const QString currentHash = RModelCommandService::stateFingerprint(state->model);
        const QJsonArray diagnostics{QJsonObject{{QStringLiteral("code"), code},
                                                  {QStringLiteral("severity"), QStringLiteral("warning")},
                                                  {QStringLiteral("message"), message}}};
        QString journalError;
        if (!appendJournalEvent(safeCommand, QStringLiteral("rejected"), revision(), currentHash,
                                currentHash, QJsonObject{}, requestSnapshot,
                                QJsonArray{}, diagnostics, &journalError))
        {
            response.insert(QStringLiteral("code"), QStringLiteral("journal_failure"));
            response.insert(QStringLiteral("message"),
                            QStringLiteral("The interaction journal could not be updated; session state was not changed."));
        }
        return response;
    };

    if (requestBytes.size() > 65536)
        return rejected(QStringLiteral("invalid_request"), QStringLiteral("Request exceeds 64 KiB."));
    if (!exactKeys(request, {QStringLiteral("apiVersion"), QStringLiteral("commandId"),
            QStringLiteral("commandVersion"), QStringLiteral("modelId"), QStringLiteral("expectedRevision"),
            QStringLiteral("requestId"), QStringLiteral("idempotencyKey"), QStringLiteral("parameters"),
            QStringLiteral("dryRun")})
        || request.value(QStringLiteral("apiVersion")) != QJsonValue(QStringLiteral("range.application/v1"))
        || !integer(request.value(QStringLiteral("commandVersion")), 1, 1)
        || !token(request.value(QStringLiteral("requestId")))
        || !token(request.value(QStringLiteral("idempotencyKey")))
        || !token(request.value(QStringLiteral("expectedRevision")))
        || !token(request.value(QStringLiteral("modelId")))
        || !request.value(QStringLiteral("commandId")).isString()
        || !request.value(QStringLiteral("parameters")).isObject()
        || !request.value(QStringLiteral("dryRun")).isBool())
        return rejected(QStringLiteral("invalid_request"), QStringLiteral("Expected the exact version 1 command envelope."));
    if (request.value(QStringLiteral("modelId")).toString() != identity)
        return rejected(QStringLiteral("wrong_model"), QStringLiteral("The model identity belongs to another editing session."));
    if (state->lifecycle == Lifecycle::Closed)
        return rejected(QStringLiteral("session_closed"), QStringLiteral("This editing session is closed."));

    const bool dryRun = request.value(QStringLiteral("dryRun")).toBool();
    if (state->lifecycle == Lifecycle::Running && !dryRun)
        return rejected(QStringLiteral("session_busy"), QStringLiteral("The model cannot be edited while a job is running."));
    if (!state->publicationIntent.isEmpty() && !state->publicationRecorded && !dryRun)
        return rejected(QStringLiteral("publication_pending"), QStringLiteral("A publication must finish before further edits."));

    const QString key = request.value(QStringLiteral("idempotencyKey")).toString();
    const auto existing = state->ledger.constFind(key);
    if (existing != state->ledger.cend())
    {
        if (existing->first != request)
            return rejected(QStringLiteral("idempotency_conflict"), QStringLiteral("This key was used for a different request."));
        if (existing->second.value(QStringLiteral("revision")).toString() != revision())
            return rejected(QStringLiteral("stale_revision"), QStringLiteral("The previously accepted result is no longer current."));
        return existing->second;
    }
    if (request.value(QStringLiteral("expectedRevision")).toString() != revision())
        return rejected(QStringLiteral("stale_revision"), QStringLiteral("Query the current revision before submitting a command."));

    const QString command = request.value(QStringLiteral("commandId")).toString();
    const auto parameters = request.value(QStringLiteral("parameters")).toObject();
    const bool undoCommand = command == QStringLiteral("model.undo");
    const bool redoCommand = command == QStringLiteral("model.redo");
    RModelCommandService::PreviewResult operation;
    if (undoCommand || redoCommand)
    {
        if (!parameters.isEmpty())
            return rejected(QStringLiteral("invalid_parameters"), QStringLiteral("Undo and redo take empty parameters."));
        const auto &history = undoCommand ? state->undo : state->redo;
        if (history.isEmpty())
            return rejected(QStringLiteral("empty_history"), QStringLiteral("No accepted edit is available."));
        operation = RModelCommandService::applyDelta(state->model, history.last(), redoCommand);
    }
    else
    {
        operation = RModelCommandService::preview(state->model, command, parameters, identity, revision());
    }
    if (!operation.valid) return rejected(operation.code, operation.message);

    const QString currentHash = RModelCommandService::stateFingerprint(state->model);
    const QString proposedHash = RModelCommandService::stateFingerprint(operation.candidate);
    QJsonObject response{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
        {QStringLiteral("status"), QStringLiteral("preview")},
        {QStringLiteral("requestId"), request.value(QStringLiteral("requestId"))},
        {QStringLiteral("modelId"), identity}, {QStringLiteral("preRevision"), revision()},
        {QStringLiteral("revision"), revision()}};
    const bool legacyModal = command == QStringLiteral("analysis.modal.configure")
        || ((undoCommand || redoCommand) && operation.delta.kind == RModelCommandService::Delta::Kind::Modal);
    if (legacyModal)
        response.insert(QStringLiteral("modal"), modalParameters(operation.candidate.getProblemSetup().getModalSetup()));
    else
    {
        response.insert(QStringLiteral("normalizedParameters"), operation.normalizedArguments);
        response.insert(QStringLiteral("before"), operation.before);
        response.insert(QStringLiteral("after"), operation.after);
        response.insert(QStringLiteral("changedPaths"), operation.changedPaths);
        response.insert(QStringLiteral("fingerprint"), operation.fingerprint);
        response.insert(QStringLiteral("diagnostics"), operation.diagnostics);
    }
    if (dryRun)
    {
        const QJsonObject journalParameters = (undoCommand || redoCommand) ? QJsonObject{}
            : command == QStringLiteral("analysis.modal.configure") ? parameters : operation.normalizedArguments;
        QString journalError;
        if (!appendJournalEvent(command, QStringLiteral("preview"), revision(), currentHash,
                                proposedHash, journalParameters, requestSnapshot,
                                QJsonArray{}, operation.diagnostics, &journalError))
            return makeResponse(QStringLiteral("journal_failure"),
                                QStringLiteral("The interaction journal could not be updated; session state was not changed."));
        return response;
    }
    if (state->ledger.size() >= MaxCommands)
        return rejected(QStringLiteral("session_limit"), QStringLiteral("Publish or discard this session before opening a new one."));

    // Construct the complete accepted state and all history before writing the
    // journal. Only the final pointer swap publishes the candidate model.
    auto candidate = std::make_unique<State>(*state);
    if (undoCommand)
    {
        const auto delta = candidate->undo.takeLast();
        candidate->redo.append(delta);
    }
    else if (redoCommand)
    {
        const auto delta = candidate->redo.takeLast();
        candidate->undo.append(delta);
    }
    else
    {
        if (operation.delta.kind != RModelCommandService::Delta::Kind::None)
            candidate->undo.append(operation.delta);
        candidate->redo.clear();
    }
    candidate->model = operation.candidate;
    ++candidate->generation;
    response[QStringLiteral("status")] = QStringLiteral("accepted");
    response[QStringLiteral("revision")] = identity + QStringLiteral(":") + QString::number(candidate->generation);
    candidate->ledger.insert(key, {request, response});
    const QJsonObject journalParameters = (undoCommand || redoCommand) ? QJsonObject{}
        : command == QStringLiteral("analysis.modal.configure") ? parameters : operation.normalizedArguments;
    QString journalError;
    if (!appendJournalEvent(command, QStringLiteral("accepted"), response.value(QStringLiteral("revision")).toString(),
                            currentHash, proposedHash, journalParameters,
                            requestSnapshot, QJsonArray{}, operation.diagnostics, &journalError))
        return makeResponse(QStringLiteral("journal_failure"),
                            QStringLiteral("The interaction journal could not be updated; session state was not changed."));
    state.swap(candidate);
    return response;
}
