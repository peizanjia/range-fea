#include <ragent_conversation_store.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace
{
constexpr auto RecordSchema = "range.agent-conversation-record/v1";
constexpr auto ListSchema = "range.agent-conversation-list/v1";
constexpr auto PageSchema = "range.agent-conversation-page/v1";
constexpr auto ExportSchema = "range.agent-conversation-export/v1";
constexpr auto GenesisSha256 = "0000000000000000000000000000000000000000000000000000000000000000";

constexpr int MaxTitleBytes = 512;
constexpr int MaxEntriesPerAppend = RAgentConversationStore::MaxPageSize;
constexpr int MaxSourceRefsPerEntry = 16;
constexpr int MaxArtifactRefsPerEntry = 16;
constexpr int MaxFactFields = 11;
constexpr int MaxRecordBytes = 20 * 1024 * 1024;
constexpr qint64 MaxConversationBytes = 20LL * 1024 * 1024;
constexpr qint64 MaxProjectBytes = 128LL * 1024 * 1024;
constexpr int MaxRecordsPerConversation = RAgentConversationStore::MaxMessagesPerConversation + 1;
constexpr int MaxOrphanBlobsPerConversation = 16;
constexpr int MaxPhysicalConversationDirectories = 64;
constexpr int MaxFilesPerConversation = 2 * MaxRecordsPerConversation + MaxOrphanBlobsPerConversation;
constexpr int MaxProjectConversationFiles = 33400;
constexpr qint64 MaxProjectManifestBytes = 8LL * 1024 * 1024;
constexpr qint64 MaxResolutionReceiptBytes = 8LL * 1024;

struct CommitMarker
{
    int revision = 0;
    QString commitId;
    QString previousSha256;
    QString blobSha256;
    QString markerFileName;
};

struct LoadedConversation
{
    QString conversationId;
    QString title;
    QString revision;
    int revisionNumber = 0;
    QJsonArray messages;
    QJsonObject branchSource;
    QString latestSha256;
    QJsonArray lastAppendEntries;
    int lastAppendRevision = 0;
    QSet<QString> unresolvedReferences;
    int fileCount = 0;
    qint64 bytesOnDisk = 0;
    int orphanRecordCount = 0;
    bool hasCommittedState = false;
};

struct ProjectScan
{
    QVector<LoadedConversation> conversations;
    QJsonArray diagnostics;
    qint64 bytesOnDisk = 0;
    int physicalDirectoryCount = 0;
    int orphanDirectoryCount = 0;
    int fileCount = 0;
    int unresolvedReferenceCount = 0;
};

QStringList effectLookupKeys(const QJsonObject &entry)
{
    if (entry.value(QStringLiteral("role")).toString() != QStringLiteral("fact")) return {};
    const QJsonObject facts = entry.value(QStringLiteral("facts")).toObject();
    QStringList keys;
    const QString taskId = entry.value(QStringLiteral("taskId")).toString();
    for (const QString &field : {QStringLiteral("idempotencyKey"), QStringLiteral("requestSha256")})
    {
        const QJsonValue value = facts.value(field);
        if (value.isString())
            keys.append(taskId + QLatin1Char('|') + field + QLatin1Char('|') + value.toString());
    }
    return keys;
}

int applyEffectReservation(const QJsonObject &entry, QSet<QString> *unresolved,
                           QSet<QString> *resolvable = nullptr)
{
    if (!unresolved) return 0;
    const QStringList keys = effectLookupKeys(entry);
    if (keys.isEmpty()) return 0;
    const QJsonObject facts = entry.value(QStringLiteral("facts")).toObject();
    const QString status = facts.value(QStringLiteral("status")).toString();
    if (status == QStringLiteral("pending") || status == QStringLiteral("unknown"))
    {
        for (const QString &key : keys) unresolved->insert(key);
        return 0;
    }
    if (status == QStringLiteral("accepted") || status == QStringLiteral("rejected"))
    {
        int resolved = 0;
        for (const QString &key : keys)
        {
            if (!unresolved->contains(key) || (resolvable && !resolvable->remove(key))) continue;
            unresolved->remove(key);
            ++resolved;
        }
        return resolved;
    }
    return 0;
}

QSet<QString> unresolvedEffectReferences(const QJsonArray &messages)
{
    QSet<QString> unresolved;
    for (const QJsonValue &message : messages)
        if (message.isObject()) applyEffectReservation(message.toObject(), &unresolved);
    return unresolved;
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
}

bool samePathText(const QString &left, const QString &right)
{
    const QString a = QDir::cleanPath(QDir::fromNativeSeparators(left));
    const QString b = QDir::cleanPath(QDir::fromNativeSeparators(right));
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

bool exactKeys(const QJsonObject &object, const QStringList &expected)
{
    if (object.size() != expected.size()) return false;
    QSet<QString> keys;
    for (const QString &key : expected) keys.insert(key);
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!keys.contains(it.key())) return false;
    return true;
}

bool hasOnlyKeys(const QJsonObject &object, const QStringList &allowed)
{
    QSet<QString> keys;
    for (const QString &key : allowed) keys.insert(key);
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!keys.contains(it.key())) return false;
    return true;
}

QString canonicalUuid(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    if (!pattern.match(text).hasMatch()) return {};
    const QUuid id(text);
    if (id.isNull()) return {};
    return id.toString(QUuid::WithoutBraces).toLower();
}

bool isCanonicalUuid(const QString &text)
{
    return !canonicalUuid(text).isEmpty() && canonicalUuid(text) == text;
}

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

bool validSha256(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(text).hasMatch();
}

bool validToken(const QString &text, int maxBytes = 128)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:/-]*$"));
    return !text.isEmpty() && text.toUtf8().size() <= maxBytes && pattern.match(text).hasMatch();
}

QString revisionText(int revision)
{
    return QString::number(revision);
}

bool parseRevision(const QString &text, int *revision)
{
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,8})$"));
    if (!pattern.match(text).hasMatch()) return false;
    bool ok = false;
    const uint parsed = text.toUInt(&ok);
    if (!ok || parsed > MaxRecordsPerConversation) return false;
    if (revision) *revision = int(parsed);
    return true;
}

bool validProjectManifest(const QString &workspace, const QString &projectId,
                          QString *error)
{
    const QFileInfo rootInfo(workspace);
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink()
        || !samePathText(rootInfo.canonicalFilePath(), workspace))
        return fail(error, QStringLiteral("The project workspace is missing, linked, or changed."));

    const QString manifestPath = QDir(workspace).filePath(QStringLiteral("project.json"));
    const QFileInfo manifestInfo(manifestPath);
    if (!manifestInfo.exists() || !manifestInfo.isFile() || manifestInfo.isSymLink()
        || !samePathText(manifestInfo.canonicalFilePath(), manifestPath)
        || manifestInfo.size() <= 0 || manifestInfo.size() > MaxProjectManifestBytes)
        return fail(error, QStringLiteral("The project identity file is missing, linked, or out of bounds."));

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("The project identity file could not be read."));
    const QByteArray bytes = file.read(MaxProjectManifestBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.isEmpty() || bytes.size() > MaxProjectManifestBytes)
        return fail(error, QStringLiteral("The project identity file could not be read within its bound."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("The project identity file is corrupt."));

    const QJsonObject manifest = document.object();
    if (!exactKeys(manifest, {QStringLiteral("storageVersion"), QStringLiteral("projectId"),
                              QStringLiteral("documents"), QStringLiteral("sourcePathIndex")})
        || !manifest.value(QStringLiteral("storageVersion")).isDouble()
        || manifest.value(QStringLiteral("storageVersion")).toInt(-1) != RAgentConversationStore::StorageVersion
        || !manifest.value(QStringLiteral("projectId")).isString()
        || canonicalUuid(manifest.value(QStringLiteral("projectId")).toString()) != projectId
        || !manifest.value(QStringLiteral("documents")).isObject()
        || !manifest.value(QStringLiteral("sourcePathIndex")).isObject())
        return fail(error, QStringLiteral("The project identity does not match this conversation store."));
    return true;
}

bool validWorkspaceForInstance(const QString &workspace, const QString &projectId,
                               QString *error)
{
    if (workspace.isEmpty() || projectId.isEmpty() || canonicalUuid(projectId) != projectId)
        return fail(error, QStringLiteral("The conversation store is not configured."));
    return validProjectManifest(workspace, projectId, error);
}

QString conversationsPath(const QString &workspace)
{
    return QDir(workspace).filePath(QStringLiteral("conversations"));
}

bool ensureConversationsDirectory(const QString &workspace, bool createIfMissing,
                                  QString *path, QString *error)
{
    const QFileInfo workspaceInfo(workspace);
    if (!workspaceInfo.isDir() || workspaceInfo.isSymLink()
        || !samePathText(workspaceInfo.canonicalFilePath(), workspace))
        return fail(error, QStringLiteral("The project workspace root changed or is linked."));
    const QString expected = conversationsPath(workspace);
    QFileInfo info(expected);
    if (info.isSymLink())
        return fail(error, QStringLiteral("The managed conversations directory is linked or outside the project."));
    if (!info.exists())
    {
        if (!createIfMissing)
        {
            if (path) *path = expected;
            return true;
        }
        if (!QDir(workspace).mkdir(QStringLiteral("conversations")))
        {
            info.refresh();
            if (!info.exists())
                return fail(error, QStringLiteral("The managed conversations directory could not be created."));
        }
        info.setFile(expected);
    }
    if (!info.isDir() || info.isSymLink() || !samePathText(info.canonicalFilePath(), expected))
        return fail(error, QStringLiteral("The managed conversations directory is linked or outside the project."));
    if (path) *path = expected;
    return true;
}

bool acquireStoreLock(const QString &workspace, std::unique_ptr<QLockFile> *lock,
                      QString *error)
{
    QString managedRoot;
    if (!ensureConversationsDirectory(workspace, true, &managedRoot, error)) return false;
    const QString lockPath = QDir(managedRoot).filePath(QStringLiteral(".conversation-store.lock"));
    const QFileInfo lockInfo(lockPath);
    if (lockInfo.isSymLink() || (lockInfo.exists() && !lockInfo.isFile()))
        return fail(error, QStringLiteral("The conversation store lock path is unsafe."));
    auto candidate = std::make_unique<QLockFile>(lockPath);
    candidate->setStaleLockTime(30000);
    if (!candidate->tryLock(5000))
        return fail(error, QStringLiteral("The conversation store is busy or its lock is unavailable."));
    const QFileInfo rootInfo(managedRoot);
    const QFileInfo lockInfoAfterLock(lockPath);
    if (!rootInfo.isDir() || rootInfo.isSymLink()
        || !samePathText(rootInfo.canonicalFilePath(), managedRoot)
        || !lockInfoAfterLock.isFile() || lockInfoAfterLock.isSymLink()
        || !samePathText(lockInfoAfterLock.canonicalFilePath(), lockPath))
    {
        candidate->unlock();
        return fail(error, QStringLiteral("The conversation store lock boundary changed."));
    }
    if (lock) *lock = std::move(candidate);
    return true;
}

QString redactText(const QString &input, const QStringList &literals,
                   bool redactLiterals)
{
    QString value = input;
    if (redactLiterals)
        for (const QString &literal : literals)
            if (!literal.isEmpty()) value.replace(literal, QStringLiteral("[REDACTED]"), Qt::CaseSensitive);

    static const QRegularExpression assignmentPattern(QString::fromLatin1(
        R"re((?i)(?<![A-Za-z0-9_])(["']?(?:[A-Za-z][A-Za-z0-9]*[_-])?(?:api[_ -]?key|key|access[_-]?token|refresh[_-]?token|token|authorization|auth|password|passwd|client[_-]?secret|secret|credential|approval[_-]?(?:token|id|pointer)|pending[_-]?approval|preview(?:[_-]?(?:id|token|pointer))?)["']?\s*[:=]\s*)(?:\[REDACTED\]|"[^"]*"|'[^']*'|Bearer\s+[^\s,;}\]]+|[^\s,;}\]]+))re"));
    value.replace(assignmentPattern, QStringLiteral("\\1[REDACTED]"));
    static const QRegularExpression unlabelledApiKey(QStringLiteral("(?i)\\bsk-[A-Za-z0-9_-]{16,}\\b"));
    value.replace(unlabelledApiKey, QStringLiteral("[REDACTED]"));
    return value;
}

bool containsRawProviderProtocol(const QString &text)
{
    static const QRegularExpression roleHeader(QStringLiteral(
        "(?im)^\\s*(?:system|developer|tool|private[_ -]?reasoning|chain[_ -]?of[_ -]?thought)\\s*:"));
    static const QRegularExpression toolCallHeader(QStringLiteral("(?im)^\\s*assistant\\s+to=[^\\s]+"));
    static const QRegularExpression serializedMarker(QStringLiteral(
        "(?i)<\\|(?:im_start|im_sep|im_end|fim_prefix|fim_middle|ghissue|ghreview|ghcommentary|meta_sep|meta_start)\\|>"));
    static const QRegularExpression forbiddenStateField(QStringLiteral(
        "(?i)(?:^|[,{\\s])[\\\"']?(?:raw[_-]?runner[_-]?history|system[_-]?prompt|tool[_-]?history|private[_-]?reasoning|chain[_-]?of[_-]?thought|pending[_-]?(?:approval|proposal)|approval(?:[_-]?(?:token|id|pointer))?|preview(?:[_-]?(?:id|token|pointer))?|control[_-]?pointer)[\\\"']?\\s*[:=]"));
    return roleHeader.match(text).hasMatch() || toolCallHeader.match(text).hasMatch()
        || serializedMarker.match(text).hasMatch() || forbiddenStateField.match(text).hasMatch();
}

bool validVisibleText(const QString &text, int maxBytes)
{
    const QByteArray utf8 = text.toUtf8();
    if (utf8.isEmpty() || utf8.size() > maxBytes) return false;
    for (const QChar character : text)
    {
        if (character.isNull()) return false;
        if (character.category() == QChar::Other_Control
            && character != QLatin1Char('\n') && character != QLatin1Char('\r')
            && character != QLatin1Char('\t')) return false;
    }
    return true;
}

bool validateSourceRefs(const QJsonValue &value, QJsonArray *normalized)
{
    if (!value.isArray() || value.toArray().size() > MaxSourceRefsPerEntry) return false;
    QJsonArray result;
    QSet<QByteArray> seen;
    for (const QJsonValue &itemValue : value.toArray())
    {
        if (!itemValue.isObject()) return false;
        const QJsonObject item = itemValue.toObject();
        if (!exactKeys(item, {QStringLiteral("sourceId"), QStringLiteral("revision"), QStringLiteral("sha256")})
            || !item.value(QStringLiteral("sourceId")).isString()
            || !item.value(QStringLiteral("revision")).isString()
            || !item.value(QStringLiteral("sha256")).isString()) return false;
        const QString sourceId = item.value(QStringLiteral("sourceId")).toString();
        const QString revision = item.value(QStringLiteral("revision")).toString();
        const QString sha256 = item.value(QStringLiteral("sha256")).toString();
        if (!validToken(sourceId) || !validToken(revision) || !validSha256(sha256)) return false;
        const QJsonObject normalizedItem{{QStringLiteral("sourceId"), sourceId},
                                         {QStringLiteral("revision"), revision},
                                         {QStringLiteral("sha256"), sha256}};
        const QByteArray key = QJsonDocument(normalizedItem).toJson(QJsonDocument::Compact);
        if (seen.contains(key)) return false;
        seen.insert(key);
        result.append(normalizedItem);
    }
    if (normalized) *normalized = result;
    return true;
}

bool validateArtifactRefs(const QJsonValue &value, QJsonArray *normalized)
{
    if (!value.isArray() || value.toArray().size() > MaxArtifactRefsPerEntry) return false;
    QJsonArray result;
    QSet<QByteArray> seen;
    for (const QJsonValue &itemValue : value.toArray())
    {
        if (!itemValue.isObject()) return false;
        const QJsonObject item = itemValue.toObject();
        if (!exactKeys(item, {QStringLiteral("artifactId"), QStringLiteral("sha256")})
            || !item.value(QStringLiteral("artifactId")).isString()
            || !item.value(QStringLiteral("sha256")).isString()) return false;
        const QString artifactId = item.value(QStringLiteral("artifactId")).toString();
        const QString sha256 = item.value(QStringLiteral("sha256")).toString();
        if (!validToken(artifactId) || !validSha256(sha256)) return false;
        const QJsonObject normalizedItem{{QStringLiteral("artifactId"), artifactId},
                                         {QStringLiteral("sha256"), sha256}};
        const QByteArray key = QJsonDocument(normalizedItem).toJson(QJsonDocument::Compact);
        if (seen.contains(key)) return false;
        seen.insert(key);
        result.append(normalizedItem);
    }
    if (normalized) *normalized = result;
    return true;
}

bool validateBranchOrigin(const QJsonValue &value, QJsonObject *normalized)
{
    if (!value.isObject()) return false;
    const QJsonObject item = value.toObject();
    if (!exactKeys(item, {QStringLiteral("conversationId"), QStringLiteral("messageId")})
        || !item.value(QStringLiteral("conversationId")).isString()
        || !item.value(QStringLiteral("messageId")).isString()) return false;
    const QString conversationId = canonicalUuid(item.value(QStringLiteral("conversationId")).toString());
    const QString messageId = canonicalUuid(item.value(QStringLiteral("messageId")).toString());
    if (conversationId.isEmpty() || messageId.isEmpty()) return false;
    if (normalized)
        *normalized = {{QStringLiteral("conversationId"), conversationId},
                       {QStringLiteral("messageId"), messageId}};
    return true;
}

bool validateFacts(const QJsonValue &value, QJsonObject *normalized)
{
    if (!value.isObject()) return false;
    const QJsonObject item = value.toObject();
    static const QStringList allowed{
        QStringLiteral("status"), QStringLiteral("code"), QStringLiteral("command"),
        QStringLiteral("operationId"), QStringLiteral("jobId"),
        QStringLiteral("sourceDocumentId"), QStringLiteral("sourceRevision"),
        QStringLiteral("acceptedModelSha256"), QStringLiteral("idempotencyKey"),
        QStringLiteral("requestSha256"), QStringLiteral("workflowProjectId")};
    // Lookup identities and hashes remain historical evidence only; no fact
    // field is interpreted as permission to retry or resume an old action.
    if (item.isEmpty() || item.size() > MaxFactFields || !hasOnlyKeys(item, allowed)) return false;

    QJsonObject result;
    for (auto it = item.constBegin(); it != item.constEnd(); ++it)
    {
        if (!it.value().isString()) return false;
        const QString text = it.value().toString();
        if (it.key() == QStringLiteral("operationId") || it.key() == QStringLiteral("sourceDocumentId")
            || it.key() == QStringLiteral("idempotencyKey")
            || it.key() == QStringLiteral("workflowProjectId"))
        {
            const QString id = canonicalUuid(text);
            if (id.isEmpty()) return false;
            result.insert(it.key(), id);
        }
        else if (it.key() == QStringLiteral("acceptedModelSha256")
                 || it.key() == QStringLiteral("requestSha256"))
        {
            if (!validSha256(text)) return false;
            result.insert(it.key(), text);
        }
        else if (!validToken(text)) return false;
        else result.insert(it.key(), text);
    }
    if (normalized) *normalized = result;
    return true;
}

bool containsSecretLiteral(const QJsonValue &value, const QStringList &sensitiveValues)
{
    if (value.isString())
    {
        const QString text = value.toString();
        static const QRegularExpression unlabelledApiKey(QStringLiteral("(?i)\\bsk-[A-Za-z0-9_-]{16,}\\b"));
        if (unlabelledApiKey.match(text).hasMatch()) return true;
        for (const QString &literal : sensitiveValues)
            if (!literal.isEmpty() && text.contains(literal, Qt::CaseSensitive)) return true;
        return false;
    }
    if (value.isArray())
    {
        for (const QJsonValue &item : value.toArray())
            if (containsSecretLiteral(item, sensitiveValues)) return true;
    }
    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            if (containsSecretLiteral(it.value(), sensitiveValues)) return true;
    }
    return false;
}

bool normalizeEntry(const QJsonObject &input, const QStringList &sensitiveValues,
                    bool redactLiterals, QJsonObject *normalized)
{
    static const QStringList allowed{
        QStringLiteral("messageId"), QStringLiteral("role"), QStringLiteral("text"),
        QStringLiteral("taskId"), QStringLiteral("facts"), QStringLiteral("branchOrigin"),
        QStringLiteral("sourceRefs"), QStringLiteral("artifactRefs")};
    if (!hasOnlyKeys(input, allowed)
        || !input.value(QStringLiteral("messageId")).isString()
        || !input.value(QStringLiteral("role")).isString()
        || !input.value(QStringLiteral("text")).isString()) return false;

    const QString role = input.value(QStringLiteral("role")).toString();
    if (role != QStringLiteral("user") && role != QStringLiteral("assistant")
        && role != QStringLiteral("fact")) return false;
    const QString messageId = canonicalUuid(input.value(QStringLiteral("messageId")).toString());
    const QString inputText = input.value(QStringLiteral("text")).toString();
    if (messageId.isEmpty() || !validVisibleText(inputText, RAgentConversationStore::MaxTextBytesPerMessage)
        || containsRawProviderProtocol(inputText)) return false;

    const QString text = redactText(inputText, sensitiveValues,
                                    redactLiterals && role != QStringLiteral("fact"));
    if (!validVisibleText(text, RAgentConversationStore::MaxTextBytesPerMessage)) return false;

    QJsonObject result{{QStringLiteral("messageId"), messageId},
                       {QStringLiteral("role"), role},
                       {QStringLiteral("text"), text}};
    if (input.contains(QStringLiteral("taskId")))
    {
        if (!input.value(QStringLiteral("taskId")).isString()) return false;
        const QString taskId = canonicalUuid(input.value(QStringLiteral("taskId")).toString());
        if (taskId.isEmpty()) return false;
        result.insert(QStringLiteral("taskId"), taskId);
    }
    if (role == QStringLiteral("fact"))
    {
        if (!input.contains(QStringLiteral("facts"))) return false;
        QJsonObject facts;
        if (!validateFacts(input.value(QStringLiteral("facts")), &facts)) return false;
        result.insert(QStringLiteral("facts"), facts);
    }
    else if (input.contains(QStringLiteral("facts"))) return false;

    if (input.contains(QStringLiteral("branchOrigin")))
    {
        QJsonObject origin;
        if (!validateBranchOrigin(input.value(QStringLiteral("branchOrigin")), &origin)) return false;
        result.insert(QStringLiteral("branchOrigin"), origin);
    }
    if (input.contains(QStringLiteral("sourceRefs")))
    {
        QJsonArray refs;
        if (!validateSourceRefs(input.value(QStringLiteral("sourceRefs")), &refs)) return false;
        result.insert(QStringLiteral("sourceRefs"), refs);
    }
    if (input.contains(QStringLiteral("artifactRefs")))
    {
        QJsonArray refs;
        if (!validateArtifactRefs(input.value(QStringLiteral("artifactRefs")), &refs)) return false;
        result.insert(QStringLiteral("artifactRefs"), refs);
    }
    if (containsSecretLiteral(QJsonValue(result), sensitiveValues)) return false;
    if (normalized) *normalized = result;
    return true;
}

bool normalizeEntries(const QJsonArray &input, const QStringList &sensitiveValues,
                      bool redactLiterals, int maxEntries, QJsonArray *normalized)
{
    if (input.isEmpty() || input.size() > maxEntries) return false;
    QJsonArray result;
    QSet<QString> ids;
    for (const QJsonValue &value : input)
    {
        if (!value.isObject()) return false;
        QJsonObject entry;
        if (!normalizeEntry(value.toObject(), sensitiveValues, redactLiterals, &entry)) return false;
        const QString id = entry.value(QStringLiteral("messageId")).toString();
        if (ids.contains(id)) return false;
        ids.insert(id);
        result.append(entry);
    }
    if (normalized) *normalized = result;
    return true;
}

bool validateStoredTitle(const QString &title)
{
    if (!validVisibleText(title, MaxTitleBytes) || containsRawProviderProtocol(title)) return false;
    return redactText(title, {}, false) == title;
}

QString sanitizedTitle(const QString &title, const QStringList &sensitiveValues)
{
    if (!validVisibleText(title, MaxTitleBytes) || containsRawProviderProtocol(title)) return {};
    const QString redacted = redactText(title, sensitiveValues, true);
    if (!validVisibleText(redacted, MaxTitleBytes)) return {};
    return redacted;
}

QJsonObject summary(const LoadedConversation &conversation)
{
    QJsonObject result{{QStringLiteral("conversationId"), conversation.conversationId},
                       {QStringLiteral("title"), conversation.title},
                       {QStringLiteral("revision"), conversation.revision},
                       {QStringLiteral("messageCount"), conversation.messages.size()}};
    if (!conversation.branchSource.isEmpty())
        result.insert(QStringLiteral("branchSource"), conversation.branchSource);
    return result;
}

QJsonObject branchSourceObject(const QString &conversationId, const QString &messageId,
                               const QString &sourceRevision)
{
    return {{QStringLiteral("conversationId"), conversationId},
            {QStringLiteral("upToMessageId"), messageId},
            {QStringLiteral("sourceRevision"), sourceRevision}};
}

bool validateBranchSource(const QJsonValue &value, QJsonObject *normalized)
{
    if (!value.isObject()) return false;
    const QJsonObject item = value.toObject();
    if (!exactKeys(item, {QStringLiteral("conversationId"), QStringLiteral("upToMessageId"),
                          QStringLiteral("sourceRevision")})
        || !item.value(QStringLiteral("conversationId")).isString()
        || !item.value(QStringLiteral("upToMessageId")).isString()
        || !item.value(QStringLiteral("sourceRevision")).isString()) return false;
    const QString conversationId = canonicalUuid(item.value(QStringLiteral("conversationId")).toString());
    const QString messageId = canonicalUuid(item.value(QStringLiteral("upToMessageId")).toString());
    const QString revision = item.value(QStringLiteral("sourceRevision")).toString();
    int parsedRevision = 0;
    if (conversationId.isEmpty() || messageId.isEmpty()
        || !parseRevision(revision, &parsedRevision) || parsedRevision < 1) return false;
    if (normalized)
        *normalized = branchSourceObject(conversationId, messageId, revision);
    return true;
}

bool syncFile(QFile &file)
{
#ifdef Q_OS_WIN
    const intptr_t nativeHandle = _get_osfhandle(file.handle());
    return nativeHandle != -1 && FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle)) != 0;
#else
    return ::fsync(file.handle()) == 0;
#endif
}

QByteArray sha256(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool writeImmutableBlob(const QString &path, const QByteArray &bytes, QString *error)
{
    if (bytes.isEmpty() || bytes.size() > MaxRecordBytes)
        return fail(error, QStringLiteral("The conversation record exceeds its frozen size bound."));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail(error, QStringLiteral("The immutable conversation record could not be exclusively created."));
    const qint64 written = output.write(bytes);
    if (written != bytes.size() || !output.flush() || !syncFile(output))
    {
        output.close();
        return fail(error, QStringLiteral("The immutable conversation record could not be durably written."));
    }
    output.close();
    if (output.error() != QFileDevice::NoError)
        return fail(error, QStringLiteral("The immutable conversation record could not be closed."));

    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != bytes.size()
        || !samePathText(info.canonicalFilePath(), path))
        return fail(error, QStringLiteral("The immutable conversation record failed file verification."));
    QFile verification(path);
    if (!verification.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("The immutable conversation record could not be verified."));
    const QByteArray saved = verification.read(MaxRecordBytes + 1);
    if (verification.error() != QFileDevice::NoError || saved != bytes)
        return fail(error, QStringLiteral("The immutable conversation record failed byte verification."));
    return true;
}

bool createExclusiveEmptyMarker(const QString &path, QString *error)
{
    QFile marker(path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail(error, QStringLiteral("The empty conversation commit marker could not be exclusively created."));
    // Exclusive creation of this empty file is the commit publication point.
    if (!marker.flush() || !syncFile(marker))
    {
        // After exclusive creation the marker is never removed: it may already
        // be visible to another reader as the commit decision.
        marker.close();
        return fail(error, QStringLiteral("The conversation commit marker could not be durably published."));
    }
    marker.close();
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != 0
        || !samePathText(info.canonicalFilePath(), path))
        return fail(error, QStringLiteral("The conversation commit marker failed empty-file verification."));
    return true;
}

QString recordFileName(int revision, const QString &commitId)
{
    return QStringLiteral("record-r%1-%2.json").arg(revision, 8, 10, QLatin1Char('0')).arg(commitId);
}

QString markerFileName(int revision, const QString &commitId,
                       const QString &previousSha, const QString &blobSha)
{
    return QStringLiteral("commit-r%1-%2-%3-%4.marker")
        .arg(revision, 8, 10, QLatin1Char('0')).arg(commitId, previousSha, blobSha);
}

bool validTimestamp(const QString &timestamp)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z$"));
    return pattern.match(timestamp).hasMatch()
        && QDateTime::fromString(timestamp, Qt::ISODateWithMs).isValid();
}

QJsonObject makeRecord(const QString &projectId, const QString &conversationId,
                       int revision, const QString &commitId, const QString &previousSha,
                       const QString &kind, const QString &title, const QJsonArray &entries,
                       const QJsonObject &branchSource)
{
    QJsonObject record{{QStringLiteral("schema"), QString::fromLatin1(RecordSchema)},
                       {QStringLiteral("storageVersion"), RAgentConversationStore::StorageVersion},
                       {QStringLiteral("projectId"), projectId},
                       {QStringLiteral("conversationId"), conversationId},
                       {QStringLiteral("revision"), revisionText(revision)},
                       {QStringLiteral("commitId"), commitId},
                       {QStringLiteral("previousSha256"), previousSha},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("createdAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("entries"), entries}};
    if (kind == QStringLiteral("create"))
    {
        record.insert(QStringLiteral("title"), title);
        if (!branchSource.isEmpty()) record.insert(QStringLiteral("branchSource"), branchSource);
    }
    return record;
}

bool createCommit(const QString &conversationDirectory, const QString &projectId,
                  const QString &conversationId, int revision, const QString &previousSha,
                  const QString &kind, const QString &title, const QJsonArray &entries,
                  const QJsonObject &branchSource, qint64 currentConversationBytes,
                  qint64 currentProjectBytes, int currentConversationFiles,
                  int currentProjectFiles, int unresolvedProjectAfter,
                  int unresolvedConversationAfter, int resolvedReferences,
                  QString *newSha, QString *error)
{
    const QString commitId = newUuid();
    const QJsonObject record = makeRecord(projectId, conversationId, revision, commitId,
                                          previousSha, kind, title, entries, branchSource);
    const QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const qint64 unresolvedReservationBytes = qint64(unresolvedProjectAfter) * MaxResolutionReceiptBytes;
    const qint64 unresolvedConversationReservationBytes =
        qint64(unresolvedConversationAfter) * MaxResolutionReceiptBytes;
    const qint64 consumedReservationBytes = qint64(resolvedReferences) * MaxResolutionReceiptBytes;
    if (bytes.isEmpty() || bytes.size() > MaxRecordBytes
        || unresolvedProjectAfter < 0 || unresolvedConversationAfter < 0 || resolvedReferences < 0
        || currentConversationBytes + bytes.size() + unresolvedConversationReservationBytes
            > MaxConversationBytes
        || currentProjectBytes + bytes.size() + unresolvedReservationBytes > MaxProjectBytes
        || (resolvedReferences > 0 && bytes.size() > consumedReservationBytes)
        || currentConversationFiles + 2 > MaxFilesPerConversation
        || currentProjectFiles + 2 > MaxProjectConversationFiles)
        return fail(error, QStringLiteral("The conversation or project storage limit would be exceeded."));

    const QString blobPath = QDir(conversationDirectory).filePath(recordFileName(revision, commitId));
    if (!writeImmutableBlob(blobPath, bytes, error)) return false;
    const QString blobSha = QString::fromLatin1(sha256(bytes));
    const QString markerPath = QDir(conversationDirectory).filePath(
        markerFileName(revision, commitId, previousSha, blobSha));
    if (!createExclusiveEmptyMarker(markerPath, error))
        return false; // Preserve the uncommitted blob for diagnosis.
    if (newSha) *newSha = blobSha;
    return true;
}

bool validateStoredEntry(const QJsonValue &value, QJsonObject *entry)
{
    if (!value.isObject()) return false;
    QJsonObject normalized;
    if (!normalizeEntry(value.toObject(), {}, false, &normalized)) return false;
    // Persisted records must already be canonical and assignment-redacted.
    if (normalized != value.toObject()) return false;
    if (entry) *entry = normalized;
    return true;
}

bool validateRecord(const QByteArray &bytes, const QString &projectId,
                    const QString &conversationId, const CommitMarker &marker,
                    const QString &expectedPreviousSha, LoadedConversation *state,
                    QString *error)
{
    if (bytes.isEmpty() || bytes.size() > MaxRecordBytes)
        return fail(error, QStringLiteral("A committed conversation record is empty or exceeds its bound."));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || QJsonDocument(document.object()).toJson(QJsonDocument::Compact) != bytes)
        return fail(error, QStringLiteral("A committed conversation record is malformed or non-canonical."));

    const QJsonObject record = document.object();
    const QString kind = record.value(QStringLiteral("kind")).toString();
    QStringList expectedKeys{
        QStringLiteral("schema"), QStringLiteral("storageVersion"), QStringLiteral("projectId"),
        QStringLiteral("conversationId"), QStringLiteral("revision"), QStringLiteral("commitId"),
        QStringLiteral("previousSha256"), QStringLiteral("kind"), QStringLiteral("createdAtUtc"),
        QStringLiteral("entries")};
    if (kind == QStringLiteral("create"))
    {
        expectedKeys.append(QStringLiteral("title"));
        if (record.contains(QStringLiteral("branchSource"))) expectedKeys.append(QStringLiteral("branchSource"));
    }
    if (!exactKeys(record, expectedKeys)
        || record.value(QStringLiteral("schema")).toString() != QString::fromLatin1(RecordSchema)
        || !record.value(QStringLiteral("storageVersion")).isDouble()
        || record.value(QStringLiteral("storageVersion")).toInt(-1) != RAgentConversationStore::StorageVersion
        || record.value(QStringLiteral("projectId")).toString() != projectId
        || record.value(QStringLiteral("conversationId")).toString() != conversationId
        || record.value(QStringLiteral("revision")).toString() != revisionText(marker.revision)
        || record.value(QStringLiteral("commitId")).toString() != marker.commitId
        || record.value(QStringLiteral("previousSha256")).toString() != expectedPreviousSha
        || record.value(QStringLiteral("previousSha256")).toString() != marker.previousSha256
        || !validTimestamp(record.value(QStringLiteral("createdAtUtc")).toString())
        || !record.value(QStringLiteral("entries")).isArray())
        return fail(error, QStringLiteral("A committed conversation record has unknown fields or a broken identity binding."));

    const QJsonArray inputEntries = record.value(QStringLiteral("entries")).toArray();
    const int maxEntries = kind == QStringLiteral("create")
        ? RAgentConversationStore::MaxMessagesPerConversation : MaxEntriesPerAppend;
    if ((kind != QStringLiteral("create") && kind != QStringLiteral("append"))
        || inputEntries.size() > maxEntries
        || (kind == QStringLiteral("append") && inputEntries.isEmpty()))
        return fail(error, QStringLiteral("A committed conversation event has an invalid kind or entry count."));

    if (marker.revision == 1)
    {
        if (kind != QStringLiteral("create") || !state || state->hasCommittedState)
            return fail(error, QStringLiteral("The first conversation commit is not a valid creation record."));
        const QString title = record.value(QStringLiteral("title")).toString();
        if (!record.value(QStringLiteral("title")).isString() || !validateStoredTitle(title))
            return fail(error, QStringLiteral("The conversation title is invalid or contains an unredacted assignment."));
        state->title = title;
        if (record.contains(QStringLiteral("branchSource")))
        {
            QJsonObject branchSource;
            if (!validateBranchSource(record.value(QStringLiteral("branchSource")), &branchSource)
                || inputEntries.isEmpty())
                return fail(error, QStringLiteral("The conversation branch source is invalid."));
            state->branchSource = branchSource;
        }
        else if (!inputEntries.isEmpty())
            return fail(error, QStringLiteral("A new conversation cannot start with unlinked history."));
    }
    else if (!state || !state->hasCommittedState || kind != QStringLiteral("append")
             || record.contains(QStringLiteral("title")) || record.contains(QStringLiteral("branchSource")))
        return fail(error, QStringLiteral("A later conversation commit is not an append event."));

    QSet<QString> existingMessageIds;
    for (const QJsonValue &oldValue : state->messages)
        existingMessageIds.insert(oldValue.toObject().value(QStringLiteral("messageId")).toString());

    QJsonArray normalizedEntries;
    for (const QJsonValue &entryValue : inputEntries)
    {
        QJsonObject entry;
        if (!validateStoredEntry(entryValue, &entry))
            return fail(error, QStringLiteral("A committed conversation entry is malformed or contains unredacted data."));
        const QString messageId = entry.value(QStringLiteral("messageId")).toString();
        if (existingMessageIds.contains(messageId))
            return fail(error, QStringLiteral("A committed conversation repeats a message identity."));
        existingMessageIds.insert(messageId);
        if (marker.revision == 1 && !state->branchSource.isEmpty())
        {
            const QJsonObject origin = entry.value(QStringLiteral("branchOrigin")).toObject();
            if (origin.value(QStringLiteral("conversationId")).toString()
                    != state->branchSource.value(QStringLiteral("conversationId")).toString())
                return fail(error, QStringLiteral("A copied branch entry is missing its direct source identity."));
        }
        normalizedEntries.append(entry);
    }
    if (state->messages.size() + normalizedEntries.size()
        > RAgentConversationStore::MaxMessagesPerConversation)
        return fail(error, QStringLiteral("The committed conversation exceeds its message bound."));

    if (marker.revision == 1 && !state->branchSource.isEmpty())
    {
        const QJsonObject lastOrigin = normalizedEntries.last().toObject()
                                           .value(QStringLiteral("branchOrigin")).toObject();
        if (lastOrigin.value(QStringLiteral("messageId")).toString()
            != state->branchSource.value(QStringLiteral("upToMessageId")).toString())
            return fail(error, QStringLiteral("The branch prefix does not end at its recorded source message."));
    }

    for (const QJsonValue &entry : normalizedEntries) state->messages.append(entry);
    if (kind == QStringLiteral("append"))
    {
        state->lastAppendEntries = normalizedEntries;
        state->lastAppendRevision = marker.revision;
    }
    state->revisionNumber = marker.revision;
    state->revision = revisionText(marker.revision);
    state->latestSha256 = marker.blobSha256;
    state->hasCommittedState = true;
    return true;
}

bool loadConversationDirectory(const QString &workspace, const QString &projectId,
                               const QString &conversationId, LoadedConversation *loaded,
                               QString *error)
{
    if (!isCanonicalUuid(conversationId))
        return fail(error, QStringLiteral("The conversation identity is invalid."));
    const QString expectedDirectory = QDir(conversationsPath(workspace)).filePath(conversationId);
    const QFileInfo directoryInfo(expectedDirectory);
    if (!directoryInfo.exists() || !directoryInfo.isDir() || directoryInfo.isSymLink()
        || !samePathText(directoryInfo.canonicalFilePath(), expectedDirectory))
        return fail(error, QStringLiteral("The managed conversation directory is missing, linked, or non-canonical."));

    static const QRegularExpression markerPattern(QStringLiteral(
        "^commit-r([0-9]{8})-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})-([0-9a-f]{64})-([0-9a-f]{64})\\.marker$"));
    static const QRegularExpression blobPattern(QStringLiteral(
        "^record-r([0-9]{8})-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\.json$"));

    QVector<CommitMarker> markers;
    QSet<QString> recordFiles;
    qint64 totalBytes = 0;
    int inspectedFiles = 0;
    QDirIterator iterator(expectedDirectory,
                          QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (iterator.hasNext())
    {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (++inspectedFiles > MaxFilesPerConversation || info.isSymLink() || !info.isFile()
            || !samePathText(info.canonicalFilePath(), info.absoluteFilePath()))
            return fail(error, QStringLiteral("A conversation directory contains an unsafe or unbounded entry."));
        totalBytes += info.size();
        if (totalBytes > MaxConversationBytes)
            return fail(error, QStringLiteral("The conversation directory exceeds its disk bound."));

        const QString name = info.fileName();
        const auto markerMatch = markerPattern.match(name);
        if (markerMatch.hasMatch())
        {
            bool revisionOk = false;
            const int revision = markerMatch.captured(1).toInt(&revisionOk);
            if (!revisionOk || revision < 1 || revision > MaxRecordsPerConversation
                || !isCanonicalUuid(markerMatch.captured(2)))
                return fail(error, QStringLiteral("A conversation commit marker has an invalid identity."));
            markers.append({revision, markerMatch.captured(2), markerMatch.captured(3),
                            markerMatch.captured(4), name});
            continue;
        }
        if (name.startsWith(QStringLiteral("commit-")))
            return fail(error, QStringLiteral("A conversation commit marker has an invalid name or hash binding."));
        const auto blobMatch = blobPattern.match(name);
        if (blobMatch.hasMatch())
        {
            bool revisionOk = false;
            const int revision = blobMatch.captured(1).toInt(&revisionOk);
            if (!revisionOk || revision < 1 || revision > MaxRecordsPerConversation
                || !isCanonicalUuid(blobMatch.captured(2)))
                return fail(error, QStringLiteral("An immutable conversation record has an invalid name."));
            recordFiles.insert(name);
            continue;
        }
        if (name.startsWith(QStringLiteral("record-")))
            return fail(error, QStringLiteral("An immutable conversation record has an invalid name."));
        return fail(error, QStringLiteral("A conversation directory contains an unknown file."));
    }

    if (markers.size() > MaxRecordsPerConversation)
        return fail(error, QStringLiteral("The conversation commit history exceeds its revision bound."));
    std::sort(markers.begin(), markers.end(), [](const CommitMarker &left, const CommitMarker &right)
    {
        return left.revision < right.revision;
    });

    LoadedConversation state;
    state.conversationId = conversationId;
    state.bytesOnDisk = totalBytes;
    state.fileCount = inspectedFiles;
    state.revision = QStringLiteral("0");
    state.latestSha256 = QString::fromLatin1(GenesisSha256);
    QSet<QString> commitIds;
    QSet<QString> committedBlobNames;
    QString previousSha = QString::fromLatin1(GenesisSha256);
    int expectedRevision = 1;
    for (const CommitMarker &marker : markers)
    {
        if (marker.revision != expectedRevision || marker.previousSha256 != previousSha
            || commitIds.contains(marker.commitId))
            return fail(error, QStringLiteral("The conversation commit chain has a gap or broken SHA binding."));
        commitIds.insert(marker.commitId);

        const QString blobName = recordFileName(marker.revision, marker.commitId);
        const QString blobPath = QDir(expectedDirectory).filePath(blobName);
        const QFileInfo blobInfo(blobPath);
        if (!recordFiles.contains(blobName) || !blobInfo.exists() || !blobInfo.isFile()
            || blobInfo.isSymLink() || blobInfo.size() <= 0 || blobInfo.size() > MaxRecordBytes
            || !samePathText(blobInfo.canonicalFilePath(), blobPath))
            return fail(error, QStringLiteral("A committed conversation record is missing or unsafe."));

        const QString markerPath = QDir(expectedDirectory).filePath(marker.markerFileName);
        const QFileInfo markerInfo(markerPath);
        if (!markerInfo.exists() || !markerInfo.isFile() || markerInfo.isSymLink()
            || markerInfo.size() != 0 || !samePathText(markerInfo.canonicalFilePath(), markerPath))
            return fail(error, QStringLiteral("A conversation commit marker is not a verified empty file."));

        QFile blob(blobPath);
        if (!blob.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("A committed conversation record could not be opened."));
        const QByteArray bytes = blob.read(MaxRecordBytes + 1);
        if (blob.error() != QFileDevice::NoError || bytes.size() != blobInfo.size()
            || bytes.isEmpty() || bytes.size() > MaxRecordBytes
            || QString::fromLatin1(sha256(bytes)) != marker.blobSha256)
            return fail(error, QStringLiteral("A committed conversation record failed its SHA-256 check."));
        if (!validateRecord(bytes, projectId, conversationId, marker, previousSha, &state, error))
            return false;
        committedBlobNames.insert(blobName);
        previousSha = marker.blobSha256;
        ++expectedRevision;
    }

    if (markers.isEmpty())
    {
        state.orphanRecordCount = recordFiles.size();
        if (recordFiles.size() > MaxOrphanBlobsPerConversation)
            return fail(error, QStringLiteral("The uncommitted conversation record count exceeds its bound."));
        if (loaded) *loaded = state;
        return true;
    }
    for (const QString &blobName : recordFiles)
        if (!committedBlobNames.contains(blobName)) ++state.orphanRecordCount;
    if (state.orphanRecordCount > MaxOrphanBlobsPerConversation)
        return fail(error, QStringLiteral("The uncommitted conversation record count exceeds its bound."));
    state.unresolvedReferences = unresolvedEffectReferences(state.messages);
    if (loaded) *loaded = state;
    return true;
}

bool scanProject(const QString &workspace, const QString &projectId,
                 bool createDirectory, ProjectScan *scan, QString *error)
{
    if (!validWorkspaceForInstance(workspace, projectId, error)) return false;
    QString root;
    if (!ensureConversationsDirectory(workspace, createDirectory, &root, error)) return false;
    const QFileInfo rootInfo(root);
    if (!rootInfo.exists())
    {
        if (scan) *scan = {};
        return true;
    }

    ProjectScan result;
    QDirIterator iterator(root, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (iterator.hasNext())
    {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.fileName() == QStringLiteral(".conversation-store.lock"))
        {
            if (!info.isFile() || info.isSymLink() || info.size() > 4096
                || !samePathText(info.canonicalFilePath(), info.absoluteFilePath()))
                return fail(error, QStringLiteral("The conversation store lock file is unsafe."));
            continue;
        }
        if (++result.physicalDirectoryCount > MaxPhysicalConversationDirectories
            || !info.isDir() || info.isSymLink() || !isCanonicalUuid(info.fileName())
            || !samePathText(info.canonicalFilePath(), info.absoluteFilePath()))
            return fail(error, QStringLiteral("The managed conversations directory contains an unsafe or unbounded entry."));

        LoadedConversation conversation;
        if (!loadConversationDirectory(workspace, projectId, info.fileName(), &conversation, error)) return false;
        result.bytesOnDisk += conversation.bytesOnDisk;
        result.fileCount += conversation.fileCount;
        if (result.bytesOnDisk > MaxProjectBytes || result.fileCount > MaxProjectConversationFiles)
            return fail(error, QStringLiteral("The project conversation store exceeds its disk or file-count bound."));
        if (!conversation.hasCommittedState)
        {
            ++result.orphanDirectoryCount;
            result.diagnostics.append(QJsonObject{{QStringLiteral("code"), QStringLiteral("orphan_uncommitted_conversation")},
                                                  {QStringLiteral("conversationId"), conversation.conversationId},
                                                  {QStringLiteral("recordCount"), conversation.orphanRecordCount}});
            if (result.orphanDirectoryCount > RAgentConversationStore::MaxLiveConversationsPerProject)
                return fail(error, QStringLiteral("The uncommitted conversation directory count exceeds its bound."));
            continue;
        }
        if (conversation.orphanRecordCount > 0)
            result.diagnostics.append(QJsonObject{{QStringLiteral("code"), QStringLiteral("orphan_uncommitted_records")},
                                                  {QStringLiteral("conversationId"), conversation.conversationId},
                                                  {QStringLiteral("recordCount"), conversation.orphanRecordCount}});
        result.unresolvedReferenceCount += int(conversation.unresolvedReferences.size());
        result.conversations.append(conversation);
    }
    if (result.conversations.size() > RAgentConversationStore::MaxLiveConversationsPerProject)
        return fail(error, QStringLiteral("The project exceeds its live conversation count bound."));
    std::sort(result.conversations.begin(), result.conversations.end(),
              [](const LoadedConversation &left, const LoadedConversation &right)
    {
        return left.conversationId < right.conversationId;
    });
    if (scan) *scan = result;
    return true;
}

const LoadedConversation *findConversation(const ProjectScan &scan, const QString &conversationId)
{
    for (const LoadedConversation &conversation : scan.conversations)
        if (conversation.conversationId == conversationId) return &conversation;
    return nullptr;
}

bool verifyDeletionTarget(const QString &workspace, const QString &conversationId,
                          QString *path, QString *error)
{
    const QString managedRoot = conversationsPath(workspace);
    const QString expected = QDir(managedRoot).filePath(conversationId);
    const QFileInfo managedInfo(managedRoot);
    const QFileInfo targetInfo(expected);
    const QString absolute = normalizedPath(expected);
    const QString rootAbsolute = normalizedPath(managedRoot);
    const QString canonical = QDir::cleanPath(QDir::fromNativeSeparators(targetInfo.canonicalFilePath()));
    if (!isCanonicalUuid(conversationId) || !managedInfo.isDir() || managedInfo.isSymLink()
        || !samePathText(managedInfo.canonicalFilePath(), rootAbsolute)
        || !targetInfo.exists() || !targetInfo.isDir() || targetInfo.isSymLink()
        || !samePathText(absolute, expected) || !samePathText(canonical, absolute)
        || !samePathText(QFileInfo(expected).absolutePath(), rootAbsolute))
        return fail(error, QStringLiteral("The conversation deletion target is not a verified managed directory."));

    QDirIterator iterator(expected, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    int count = 0;
    static const QRegularExpression markerPattern(QStringLiteral(
        "^commit-r[0-9]{8}-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}-[0-9a-f]{64}-[0-9a-f]{64}\\.marker$"));
    static const QRegularExpression blobPattern(QStringLiteral(
        "^record-r[0-9]{8}-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.json$"));
    while (iterator.hasNext())
    {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (++count > MaxFilesPerConversation || !info.isFile() || info.isSymLink()
            || !samePathText(info.canonicalFilePath(), info.absoluteFilePath())
            || (!markerPattern.match(info.fileName()).hasMatch()
                && !blobPattern.match(info.fileName()).hasMatch()))
            return fail(error, QStringLiteral("The conversation directory contains unverified files and was preserved."));
    }
    if (path) *path = absolute;
    return true;
}
}

bool RAgentConversationStore::configure(const QString &workspace, const QString &projectId,
                                        QString *error)
{
    if (error) error->clear();
    workspaceRoot.clear();
    projectIdentity.clear();
    const QString absolute = normalizedPath(workspace);
    const QFileInfo suppliedInfo(absolute);
    if (workspace.trimmed().isEmpty() || workspace.toUtf8().size() > 4096
        || canonicalUuid(projectId).isEmpty()
        || !suppliedInfo.isDir() || suppliedInfo.isSymLink()
        || !samePathText(suppliedInfo.canonicalFilePath(), absolute))
        return fail(error, QStringLiteral("A canonical project workspace and UUID are required."));
    const QString normalizedProjectId = canonicalUuid(projectId);
    if (!validProjectManifest(absolute, normalizedProjectId, error)) return false;
    workspaceRoot = absolute;
    projectIdentity = normalizedProjectId;
    return true;
}

void RAgentConversationStore::setSensitiveValues(const QStringList &values)
{
    sensitiveLiterals.clear();
    for (const QString &value : values)
    {
        if (value.isEmpty() || value.toUtf8().size() > 4096 || sensitiveLiterals.size() >= 64
            || sensitiveLiterals.contains(value)) continue;
        sensitiveLiterals.append(value);
    }
    std::sort(sensitiveLiterals.begin(), sensitiveLiterals.end(), [](const QString &left, const QString &right)
    {
        return left.size() > right.size();
    });
}

QJsonObject RAgentConversationStore::create(const QString &title, QString *error)
{
    if (error) error->clear();
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return {};
    const QString safeTitle = sanitizedTitle(title, sensitiveLiterals);
    if (safeTitle.isEmpty())
    {
        fail(error, QStringLiteral("The conversation title is empty, contains controls, or exceeds its bound."));
        return {};
    }
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return {};
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, true, &scan, error)) return {};
    if (scan.conversations.size() >= MaxLiveConversationsPerProject
        || scan.physicalDirectoryCount >= MaxPhysicalConversationDirectories)
    {
        fail(error, QStringLiteral("The project has reached its bounded conversation count."));
        return {};
    }

    QString root;
    if (!ensureConversationsDirectory(workspaceRoot, true, &root, error)) return {};
    QString id;
    QString directory;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        id = newUuid();
        directory = QDir(root).filePath(id);
        if (!QFileInfo::exists(directory) && QDir(root).mkdir(id)) break;
        id.clear();
    }
    if (id.isEmpty())
    {
        fail(error, QStringLiteral("A unique managed conversation directory could not be created."));
        return {};
    }
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()
        || !samePathText(directoryInfo.canonicalFilePath(), directory))
    {
        fail(error, QStringLiteral("The new conversation directory failed path verification."));
        return {};
    }

    QString latestSha;
    if (!createCommit(directory, projectIdentity, id, 1, QString::fromLatin1(GenesisSha256),
                      QStringLiteral("create"), safeTitle, {}, {}, 0, scan.bytesOnDisk, 0, scan.fileCount,
                      scan.unresolvedReferenceCount, 0, 0, &latestSha, error)) return {};
    LoadedConversation saved;
    if (!loadConversationDirectory(workspaceRoot, projectIdentity, id, &saved, error)
        || !saved.hasCommittedState) return {};
    return summary(saved);
}

QJsonObject RAgentConversationStore::append(const QString &conversationId,
                                            const QString &expectedRevision,
                                            const QJsonArray &entries,
                                            QString *error)
{
    if (error) error->clear();
    const QString id = canonicalUuid(conversationId);
    int expected = 0;
    if (id.isEmpty() || !parseRevision(expectedRevision, &expected) || expected < 1)
    {
        fail(error, QStringLiteral("The conversation identity or expected revision is invalid."));
        return {};
    }
    QJsonArray safeEntries;
    if (!normalizeEntries(entries, sensitiveLiterals, true, MaxEntriesPerAppend, &safeEntries))
    {
        fail(error, QStringLiteral("The append entries violate the observable-message schema or bounds."));
        return {};
    }
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return {};
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return {};
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, false, &scan, error)) return {};
    const LoadedConversation *current = findConversation(scan, id);
    if (!current)
    {
        fail(error, QStringLiteral("The conversation was not found."));
        return {};
    }

    if (current->revisionNumber != expected)
    {
        if (current->revisionNumber == expected + 1
            && current->lastAppendRevision == current->revisionNumber
            && current->lastAppendEntries == safeEntries)
            return summary(*current); // Exact retry of the immediately preceding append.
        fail(error, QStringLiteral("The conversation revision changed; no entries were appended."));
        return {};
    }
    QSet<QString> existingIds;
    for (const QJsonValue &message : current->messages)
        existingIds.insert(message.toObject().value(QStringLiteral("messageId")).toString());
    for (const QJsonValue &entry : safeEntries)
        if (existingIds.contains(entry.toObject().value(QStringLiteral("messageId")).toString()))
        {
            fail(error, QStringLiteral("The append reuses an existing message identity."));
            return {};
        }
    QSet<QString> unresolvedAfter = current->unresolvedReferences;
    QSet<QString> resolvableReferences = current->unresolvedReferences;
    QSet<QString> candidateIntroducedReferences;
    int resolvedReferences = 0;
    for (const QJsonValue &entry : safeEntries)
    {
        const QJsonObject entryObject = entry.toObject();
        const QJsonObject entryFacts = entryObject.value(QStringLiteral("facts")).toObject();
        const QString effectStatus = entryFacts.value(QStringLiteral("status")).toString();
        const QStringList lookupKeys = effectLookupKeys(entryObject);
        if (effectStatus == QStringLiteral("pending") || effectStatus == QStringLiteral("unknown"))
        {
            for (const QString &key : lookupKeys)
                if (!resolvableReferences.contains(key)) candidateIntroducedReferences.insert(key);
        }
        else if (effectStatus == QStringLiteral("accepted") || effectStatus == QStringLiteral("rejected"))
        {
            for (const QString &key : lookupKeys)
                if (candidateIntroducedReferences.contains(key) && !resolvableReferences.contains(key))
                {
                    fail(error, QStringLiteral("An effect outcome can resolve only a reservation from an earlier commit."));
                    return {};
                }
        }

        const int resolvedForEntry = applyEffectReservation(entryObject, &unresolvedAfter,
                                                            &resolvableReferences);
        if (resolvedForEntry > 0
            && QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact).size()
                > MaxResolutionReceiptBytes)
        {
            fail(error, QStringLiteral("An effect resolution receipt exceeds its reserved 8 KiB bound."));
            return {};
        }
        resolvedReferences += resolvedForEntry;
    }
    const qsizetype candidateMessageCount = current->messages.size() + safeEntries.size();
    if (candidateMessageCount + unresolvedAfter.size() > MaxMessagesPerConversation)
    {
        fail(error, QStringLiteral("The append would consume capacity reserved for unresolved effect receipts."));
        return {};
    }
    if (current->messages.size() + safeEntries.size() > MaxMessagesPerConversation
        || current->revisionNumber >= MaxRecordsPerConversation)
    {
        fail(error, QStringLiteral("The conversation has reached its frozen message or revision limit."));
        return {};
    }
    const int unresolvedProjectAfter = scan.unresolvedReferenceCount
        - int(current->unresolvedReferences.size()) + int(unresolvedAfter.size());
    const QString directory = QDir(conversationsPath(workspaceRoot)).filePath(id);
    QString latestSha;
    if (!createCommit(directory, projectIdentity, id, current->revisionNumber + 1,
                      current->latestSha256, QStringLiteral("append"), {}, safeEntries, {},
                      current->bytesOnDisk, scan.bytesOnDisk, current->fileCount, scan.fileCount,
                      unresolvedProjectAfter, int(unresolvedAfter.size()), resolvedReferences,
                      &latestSha, error)) return {};
    LoadedConversation saved;
    if (!loadConversationDirectory(workspaceRoot, projectIdentity, id, &saved, error)
        || !saved.hasCommittedState) return {};
    return summary(saved);
}

QJsonObject RAgentConversationStore::list(int offset, int limit, QString *error) const
{
    if (error) error->clear();
    if (offset < 0 || offset > MaxLiveConversationsPerProject || limit < 1 || limit > MaxPageSize)
    {
        fail(error, QStringLiteral("The conversation list page is outside its bounds."));
        return {};
    }
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return {};
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return {};
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, false, &scan, error)) return {};
    QJsonArray items;
    for (int index = offset; index < scan.conversations.size() && items.size() < limit; ++index)
        items.append(summary(scan.conversations.at(index)));
    return {{QStringLiteral("schema"), QString::fromLatin1(ListSchema)},
            {QStringLiteral("projectId"), projectIdentity},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("limit"), limit},
            {QStringLiteral("totalCount"), scan.conversations.size()},
            {QStringLiteral("conversations"), items},
            {QStringLiteral("diagnostics"), scan.diagnostics}};
}

QJsonObject RAgentConversationStore::page(const QString &conversationId, int offset,
                                         int limit, QString *error) const
{
    if (error) error->clear();
    const QString id = canonicalUuid(conversationId);
    if (id.isEmpty() || offset < 0 || offset > MaxMessagesPerConversation
        || limit < 1 || limit > MaxPageSize)
    {
        fail(error, QStringLiteral("The conversation identity or message page is outside its bounds."));
        return {};
    }
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return {};
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return {};
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, false, &scan, error)) return {};
    const LoadedConversation *conversation = findConversation(scan, id);
    if (!conversation)
    {
        fail(error, QStringLiteral("The conversation was not found."));
        return {};
    }
    QJsonArray messages;
    for (int index = offset; index < conversation->messages.size() && messages.size() < limit; ++index)
        messages.append(conversation->messages.at(index));
    QJsonObject result{{QStringLiteral("schema"), QString::fromLatin1(PageSchema)},
                       {QStringLiteral("conversationId"), conversation->conversationId},
                       {QStringLiteral("title"), conversation->title},
                       {QStringLiteral("revision"), conversation->revision},
                       {QStringLiteral("messageCount"), conversation->messages.size()},
                       {QStringLiteral("offset"), offset},
                       {QStringLiteral("messages"), messages},
                       {QStringLiteral("hasMore"), offset + messages.size() < conversation->messages.size()}};
    if (!conversation->branchSource.isEmpty())
        result.insert(QStringLiteral("branchSource"), conversation->branchSource);
    if (conversation->orphanRecordCount > 0)
        result.insert(QStringLiteral("diagnostics"), QJsonArray{QJsonObject{
            {QStringLiteral("code"), QStringLiteral("orphan_uncommitted_records")},
            {QStringLiteral("recordCount"), conversation->orphanRecordCount}}});
    return result;
}

QJsonObject RAgentConversationStore::branch(const QString &conversationId,
                                           const QString &upToMessageId,
                                           const QString &title, QString *error)
{
    if (error) error->clear();
    const QString sourceId = canonicalUuid(conversationId);
    const QString sourceMessageId = canonicalUuid(upToMessageId);
    const QString safeTitle = sanitizedTitle(title, sensitiveLiterals);
    if (sourceId.isEmpty() || sourceMessageId.isEmpty() || safeTitle.isEmpty())
    {
        fail(error, QStringLiteral("The branch identity, title, or message boundary is invalid."));
        return {};
    }
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return {};
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return {};
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, true, &scan, error)) return {};
    const LoadedConversation *source = findConversation(scan, sourceId);
    if (!source)
    {
        fail(error, QStringLiteral("The source conversation was not found."));
        return {};
    }
    int end = -1;
    for (int index = 0; index < source->messages.size(); ++index)
        if (source->messages.at(index).toObject().value(QStringLiteral("messageId")).toString() == sourceMessageId)
        {
            end = index;
            break;
        }
    if (end < 0)
    {
        fail(error, QStringLiteral("The branch boundary message was not found."));
        return {};
    }
    if (scan.conversations.size() >= MaxLiveConversationsPerProject
        || scan.physicalDirectoryCount >= MaxPhysicalConversationDirectories)
    {
        fail(error, QStringLiteral("The project has reached its bounded conversation count."));
        return {};
    }

    QJsonArray copiedEntries;
    for (int index = 0; index <= end; ++index)
    {
        const QJsonObject oldEntry = source->messages.at(index).toObject();
        QJsonObject copied = oldEntry;
        copied.insert(QStringLiteral("messageId"), newUuid());
        copied.insert(QStringLiteral("branchOrigin"), QJsonObject{
            {QStringLiteral("conversationId"), sourceId},
            {QStringLiteral("messageId"), oldEntry.value(QStringLiteral("messageId")).toString()}});
        QJsonObject normalized;
        if (!normalizeEntry(copied, sensitiveLiterals, true, &normalized))
        {
            fail(error, QStringLiteral("The observable branch prefix could not be copied safely."));
            return {};
        }
        copiedEntries.append(normalized);
    }
    const QSet<QString> copiedReservations = unresolvedEffectReferences(copiedEntries);
    if (copiedEntries.size() + copiedReservations.size() > MaxMessagesPerConversation)
    {
        fail(error, QStringLiteral("The branch prefix cannot fit its unresolved effect receipt reservations."));
        return {};
    }
    const QJsonObject sourceReference = branchSourceObject(sourceId, sourceMessageId, source->revision);

    QString root;
    if (!ensureConversationsDirectory(workspaceRoot, true, &root, error)) return {};
    QString id;
    QString directory;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        id = newUuid();
        directory = QDir(root).filePath(id);
        if (!QFileInfo::exists(directory) && QDir(root).mkdir(id)) break;
        id.clear();
    }
    if (id.isEmpty())
    {
        fail(error, QStringLiteral("A unique managed conversation directory could not be created."));
        return {};
    }
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()
        || !samePathText(directoryInfo.canonicalFilePath(), directory))
    {
        fail(error, QStringLiteral("The new branch directory failed path verification."));
        return {};
    }
    QString latestSha;
    if (!createCommit(directory, projectIdentity, id, 1, QString::fromLatin1(GenesisSha256),
                      QStringLiteral("create"), safeTitle, copiedEntries, sourceReference,
                      0, scan.bytesOnDisk, 0, scan.fileCount,
                      scan.unresolvedReferenceCount + int(copiedReservations.size()),
                      int(copiedReservations.size()), 0,
                      &latestSha, error)) return {};
    LoadedConversation saved;
    if (!loadConversationDirectory(workspaceRoot, projectIdentity, id, &saved, error)
        || !saved.hasCommittedState) return {};
    return summary(saved);
}

QJsonObject RAgentConversationStore::exportConversation(const QString &conversationId,
                                                       QString *error) const
{
    if (error) error->clear();
    const QString id = canonicalUuid(conversationId);
    if (id.isEmpty())
    {
        fail(error, QStringLiteral("The conversation identity is invalid."));
        return {};
    }
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return {};
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return {};
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, false, &scan, error)) return {};
    const LoadedConversation *conversation = findConversation(scan, id);
    if (!conversation)
    {
        fail(error, QStringLiteral("The conversation was not found."));
        return {};
    }
    QJsonObject result{{QStringLiteral("schema"), QString::fromLatin1(ExportSchema)},
                       {QStringLiteral("storageVersion"), StorageVersion},
                       {QStringLiteral("projectId"), projectIdentity},
                       {QStringLiteral("conversationId"), conversation->conversationId},
                       {QStringLiteral("title"), conversation->title},
                       {QStringLiteral("revision"), conversation->revision},
                       {QStringLiteral("messageCount"), conversation->messages.size()},
                       {QStringLiteral("messages"), conversation->messages}};
    if (!conversation->branchSource.isEmpty())
        result.insert(QStringLiteral("branchSource"), conversation->branchSource);
    if (conversation->orphanRecordCount > 0)
        result.insert(QStringLiteral("diagnostics"), QJsonArray{QJsonObject{
            {QStringLiteral("code"), QStringLiteral("orphan_uncommitted_records")},
            {QStringLiteral("recordCount"), conversation->orphanRecordCount}}});
    return result;
}

bool RAgentConversationStore::remove(const QString &conversationId,
                                     const QString &expectedRevision, QString *error)
{
    if (error) error->clear();
    const QString id = canonicalUuid(conversationId);
    int expected = 0;
    if (id.isEmpty() || !parseRevision(expectedRevision, &expected) || expected < 1)
        return fail(error, QStringLiteral("The conversation identity or expected revision is invalid."));
    if (!validWorkspaceForInstance(workspaceRoot, projectIdentity, error)) return false;
    std::unique_ptr<QLockFile> lock;
    if (!acquireStoreLock(workspaceRoot, &lock, error)) return false;
    ProjectScan scan;
    if (!scanProject(workspaceRoot, projectIdentity, false, &scan, error)) return false;
    const LoadedConversation *conversation = findConversation(scan, id);
    if (!conversation)
        return fail(error, QStringLiteral("The conversation was not found."));
    if (conversation->revisionNumber != expected)
        return fail(error, QStringLiteral("The conversation revision changed; it was not deleted."));

    QString target;
    if (!verifyDeletionTarget(workspaceRoot, id, &target, error)) return false;
    QDir directory(target);
    if (!directory.removeRecursively())
        return fail(error, QStringLiteral("The verified conversation directory could not be removed."));
    if (QFileInfo::exists(target))
        return fail(error, QStringLiteral("The conversation directory remains after deletion."));
    return true;
}

QString RAgentConversationStore::workspace() const
{
    return workspaceRoot;
}

QString RAgentConversationStore::projectId() const
{
    return projectIdentity;
}
