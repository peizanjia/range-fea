#include <rproject_catalog.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <rmodel_command_session.h>
#include <rml_file.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>

namespace
{
constexpr qint64 MaxManifestBytes = 8 * 1024 * 1024;
constexpr qint64 MaxWorkflowStateBytes = 64 * 1024 * 1024;
constexpr qsizetype MaxDraftBytes = 32 * 1024;
constexpr qsizetype MaxLabelBytes = 256;
constexpr qsizetype MaxPathBytes = 4096;
constexpr int MaxDocuments = 4096;
constexpr int MaxJsonDepth = 32;
constexpr int MaxJsonNodes = 200000;
constexpr int MaxNativeLinkDepth = 16;
constexpr qint64 MaxNativeArtifactBytes = qint64(16) * (qint64(1) << 30);
constexpr qint64 MaxNativeLinkBytes = 64 * 1024;
constexpr qsizetype MaxNativeHeaderLineBytes = 8192;
constexpr int MaxOrphanSnapshots = 256;

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

void setErrorCode(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

QString newDocumentId(const QJsonObject &documents)
{
    QString id;
    do id = newId(); while (documents.contains(id));
    return id;
}

bool validUuid(const QString &value)
{
    if (value.size() != 36) return false;
    const QUuid uuid = QUuid::fromString(value);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces).toLower() == value;
}

bool validHash(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}

bool validRevision(const QString &value)
{
    if (value.size() < 2 || value.front() != QLatin1Char('r')) return false;
    const QString digits = value.mid(1);
    if ((digits.size() > 1 && digits.front() == QLatin1Char('0'))) return false;
    for (const QChar c : digits)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    bool ok = false;
    const qulonglong revision = digits.toULongLong(&ok, 10);
    return ok && revision <= 9007199254740991ULL;
}

qulonglong revisionNumber(const QString &value)
{
    bool ok = false;
    const qulonglong revision = value.mid(1).toULongLong(&ok, 10);
    return ok ? revision : std::numeric_limits<qulonglong>::max();
}

bool validLabel(const QString &value)
{
    if (value.trimmed().isEmpty() || value.toUtf8().size() > MaxLabelBytes) return false;
    for (const QChar c : value)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}

bool finiteJson(const QJsonValue &value, int depth = 0, int *nodes = nullptr)
{
    int localNodes = 0;
    if (!nodes) nodes = &localNodes;
    if (depth > MaxJsonDepth || ++(*nodes) > MaxJsonNodes) return false;
    if (value.isDouble()) return std::isfinite(value.toDouble());
    if (value.isArray())
    {
        const auto array = value.toArray();
        if (array.size() > MaxJsonNodes) return false;
        for (const auto &child : array)
            if (!finiteJson(child, depth + 1, nodes)) return false;
    }
    else if (value.isObject())
    {
        const auto object = value.toObject();
        if (object.size() > MaxJsonNodes) return false;
        for (auto it = object.begin(); it != object.end(); ++it)
            if (!finiteJson(it.value(), depth + 1, nodes)) return false;
    }
    return true;
}

bool validDraftMetadata(const QJsonObject &metadata)
{
    const QSet<QString> allowed{QStringLiteral("draftId"), QStringLiteral("status"), QStringLiteral("kind"),
                                QStringLiteral("changedFields"), QStringLiteral("fieldCount")};
    for (auto it = metadata.begin(); it != metadata.end(); ++it)
        if (!allowed.contains(it.key())) return false;
    if (metadata.contains(QStringLiteral("draftId"))
        && (!metadata.value(QStringLiteral("draftId")).isString()
            || !validUuid(metadata.value(QStringLiteral("draftId")).toString()))) return false;
    if (metadata.contains(QStringLiteral("status")))
    {
        const auto status = metadata.value(QStringLiteral("status"));
        if (!status.isString() || (status.toString() != QStringLiteral("none")
                                   && status.toString() != QStringLiteral("pending")
                                   && status.toString() != QStringLiteral("unknown"))) return false;
    }
    if (metadata.contains(QStringLiteral("kind")))
    {
        const auto kind = metadata.value(QStringLiteral("kind"));
        if (!kind.isString() || (kind.toString() != QStringLiteral("analysis")
                                 && kind.toString() != QStringLiteral("selection")
                                 && kind.toString() != QStringLiteral("other"))) return false;
    }
    if (metadata.contains(QStringLiteral("fieldCount")))
    {
        const auto count = metadata.value(QStringLiteral("fieldCount"));
        if (!count.isDouble() || !std::isfinite(count.toDouble()) || count.toDouble() < 0
            || std::floor(count.toDouble()) != count.toDouble() || count.toDouble() > 4096) return false;
    }
    if (metadata.contains(QStringLiteral("changedFields")))
    {
        const auto fields = metadata.value(QStringLiteral("changedFields"));
        if (!fields.isArray() || fields.toArray().size() > 64) return false;
        QSet<QString> unique;
        for (const auto &fieldValue : fields.toArray())
        {
            if (!fieldValue.isString()) return false;
            const QString field = fieldValue.toString();
            if (field.isEmpty() || field.toUtf8().size() > 128 || unique.contains(field)) return false;
            for (const QChar c : field)
                if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                      || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                      || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || c == QLatin1Char('.') || c == QLatin1Char('_')
                      || c == QLatin1Char('-') || c == QLatin1Char('/'))) return false;
            unique.insert(field);
        }
    }
    return finiteJson(QJsonValue(metadata))
        && QJsonDocument(metadata).toJson(QJsonDocument::Compact).size() <= MaxDraftBytes;
}

bool exactKeys(const QJsonObject &object, const QStringList &expected)
{
    QSet<QString> keys;
    for (const QString &key : expected) keys.insert(key);
    if (object.size() != keys.size()) return false;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.contains(it.key())) return false;
    return true;
}

QJsonObject emptyManifest(const QString &projectId)
{
    return {{QStringLiteral("storageVersion"), RProjectCatalog::StorageVersion},
            {QStringLiteral("projectId"), projectId},
            {QStringLiteral("documents"), QJsonObject{}},
            {QStringLiteral("sourcePathIndex"), QJsonObject{}}};
}

QJsonObject documentJson(const RProjectCatalog::Document &document)
{
    QJsonArray sourceArtifacts;
    for (const auto &artifact : document.sourceArtifacts) sourceArtifacts.append(artifact.toJson());
    return {{QStringLiteral("persistentDocumentId"), document.persistentDocumentId},
            {QStringLiteral("label"), document.label},
            {QStringLiteral("sourcePath"), document.sourcePath},
            {QStringLiteral("resolvedSourcePath"), document.resolvedSourcePath},
            {QStringLiteral("sourceArtifacts"), sourceArtifacts},
            {QStringLiteral("originDocumentId"), document.originDocumentId},
            {QStringLiteral("sourceRevision"), document.sourceRevision},
            {QStringLiteral("savedArtifactHash"), document.savedArtifactHash},
            {QStringLiteral("dirty"), document.dirty},
            {QStringLiteral("draftMetadata"), document.draftMetadata},
            {QStringLiteral("tombstone"), document.tombstone},
            {QStringLiteral("modelSha256"), document.modelSha256},
            {QStringLiteral("snapshotSourceRevision"), document.snapshotSourceRevision},
            {QStringLiteral("snapshotRelativePath"), document.snapshotRelativePath},
            {QStringLiteral("snapshotInstanceRef"), document.snapshotInstanceRef},
            {QStringLiteral("orphanSnapshots"), document.orphanSnapshots}};
}

bool validInstanceReference(const QJsonObject &ref)
{
    if (!exactKeys(ref, {QStringLiteral("documentId"), QStringLiteral("sessionEpoch"),
                         QStringLiteral("documentRevision")})) return false;
    const QJsonValue revision = ref.value(QStringLiteral("documentRevision"));
    return ref.value(QStringLiteral("documentId")).isString()
        && ref.value(QStringLiteral("sessionEpoch")).isString()
        && validUuid(ref.value(QStringLiteral("documentId")).toString())
        && validUuid(ref.value(QStringLiteral("sessionEpoch")).toString())
        && revision.isDouble() && std::isfinite(revision.toDouble())
        && revision.toDouble() >= 0 && std::floor(revision.toDouble()) == revision.toDouble()
        && revision.toDouble() <= 9007199254740991.0;
}

bool documentFromJson(const QString &key, const QJsonValue &value,
                      RProjectCatalog::Document &document, QString &error)
{
    if (!value.isObject())
    {
        error = QStringLiteral("A project document record is not an object.");
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("persistentDocumentId"), QStringLiteral("label"),
                            QStringLiteral("sourcePath"), QStringLiteral("resolvedSourcePath"),
                            QStringLiteral("sourceArtifacts"), QStringLiteral("originDocumentId"),
                            QStringLiteral("sourceRevision"), QStringLiteral("savedArtifactHash"),
                            QStringLiteral("dirty"), QStringLiteral("draftMetadata"),
                            QStringLiteral("tombstone"), QStringLiteral("modelSha256"),
                            QStringLiteral("snapshotSourceRevision"),
                            QStringLiteral("snapshotRelativePath"), QStringLiteral("snapshotInstanceRef"),
                            QStringLiteral("orphanSnapshots")}))
    {
        error = QStringLiteral("A project document record has unknown or missing fields.");
        return false;
    }
    if (!object.value(QStringLiteral("persistentDocumentId")).isString()
        || !object.value(QStringLiteral("label")).isString()
        || !object.value(QStringLiteral("sourcePath")).isString()
        || !object.value(QStringLiteral("resolvedSourcePath")).isString()
        || !object.value(QStringLiteral("sourceArtifacts")).isArray()
        || !object.value(QStringLiteral("originDocumentId")).isString()
        || !object.value(QStringLiteral("sourceRevision")).isString()
        || !object.value(QStringLiteral("savedArtifactHash")).isString()
        || !object.value(QStringLiteral("dirty")).isBool()
        || !object.value(QStringLiteral("draftMetadata")).isObject()
        || !object.value(QStringLiteral("tombstone")).isBool()
        || !object.value(QStringLiteral("modelSha256")).isString()
        || !object.value(QStringLiteral("snapshotSourceRevision")).isString()
        || !object.value(QStringLiteral("snapshotRelativePath")).isString()
        || !object.value(QStringLiteral("snapshotInstanceRef")).isObject()
        || !object.value(QStringLiteral("orphanSnapshots")).isArray())
    {
        error = QStringLiteral("A project document record has invalid field types.");
        return false;
    }

    document.persistentDocumentId = object.value(QStringLiteral("persistentDocumentId")).toString();
    document.label = object.value(QStringLiteral("label")).toString();
    document.sourcePath = object.value(QStringLiteral("sourcePath")).toString();
    document.resolvedSourcePath = object.value(QStringLiteral("resolvedSourcePath")).toString();
    const QJsonArray sourceArtifacts = object.value(QStringLiteral("sourceArtifacts")).toArray();
    document.sourceArtifacts.reserve(sourceArtifacts.size());
    for (const QJsonValue &artifactValue : sourceArtifacts)
    {
        if (!artifactValue.isObject())
        {
            error = QStringLiteral("Native source artifact record is not an object.");
            return false;
        }
        const QJsonObject artifactObject = artifactValue.toObject();
        if (!exactKeys(artifactObject, {QStringLiteral("path"), QStringLiteral("sha256")})
            || !artifactObject.value(QStringLiteral("path")).isString()
            || !artifactObject.value(QStringLiteral("sha256")).isString())
        {
            error = QStringLiteral("Native source artifact record has unknown or invalid fields.");
            return false;
        }
        document.sourceArtifacts.append({artifactObject.value(QStringLiteral("path")).toString(),
                                         artifactObject.value(QStringLiteral("sha256")).toString()});
    }
    document.originDocumentId = object.value(QStringLiteral("originDocumentId")).toString();
    document.sourceRevision = object.value(QStringLiteral("sourceRevision")).toString();
    document.savedArtifactHash = object.value(QStringLiteral("savedArtifactHash")).toString();
    document.dirty = object.value(QStringLiteral("dirty")).toBool();
    document.draftMetadata = object.value(QStringLiteral("draftMetadata")).toObject();
    document.tombstone = object.value(QStringLiteral("tombstone")).toBool();
    document.modelSha256 = object.value(QStringLiteral("modelSha256")).toString();
    document.snapshotSourceRevision = object.value(QStringLiteral("snapshotSourceRevision")).toString();
    document.snapshotRelativePath = object.value(QStringLiteral("snapshotRelativePath")).toString();
    document.snapshotInstanceRef = object.value(QStringLiteral("snapshotInstanceRef")).toObject();
    document.orphanSnapshots = object.value(QStringLiteral("orphanSnapshots")).toArray();

    if (!validUuid(key) || document.persistentDocumentId != key
        || !validLabel(document.label) || !validRevision(document.sourceRevision)
        || (!document.originDocumentId.isEmpty() && !validUuid(document.originDocumentId))
        || (!document.savedArtifactHash.isEmpty() && !validHash(document.savedArtifactHash))
        || (!document.modelSha256.isEmpty() && !validHash(document.modelSha256))
        || (!document.snapshotSourceRevision.isEmpty() && !validRevision(document.snapshotSourceRevision))
        || document.sourcePath.toUtf8().size() > MaxPathBytes
        || document.resolvedSourcePath.toUtf8().size() > MaxPathBytes
        || document.snapshotRelativePath.toUtf8().size() > MaxPathBytes
        || !validDraftMetadata(document.draftMetadata)
        || document.sourceArtifacts.size() > MaxNativeLinkDepth + 1
        || document.orphanSnapshots.size() > MaxOrphanSnapshots
        || !finiteJson(QJsonValue(document.orphanSnapshots)))
    {
        error = QStringLiteral("A project document record violates the v1 bounds or identity rules.");
        return false;
    }
    if (!document.sourcePath.isEmpty())
    {
        if (!QDir::isAbsolutePath(document.sourcePath)
            || QDir::cleanPath(document.sourcePath) != document.sourcePath)
        {
            error = QStringLiteral("A project document source path is not a clean absolute path.");
            return false;
        }
    }
    if (document.sourcePath.isEmpty())
    {
        if (!document.resolvedSourcePath.isEmpty() || !document.sourceArtifacts.isEmpty()
            || !document.savedArtifactHash.isEmpty())
        {
            error = QStringLiteral("Unsaved document contains native source binding data.");
            return false;
        }
    }
    else
    {
        if (document.sourceArtifacts.isEmpty()
            || document.sourceArtifacts.first().path != document.sourcePath
            || document.sourceArtifacts.last().path != document.resolvedSourcePath
            || document.sourceArtifacts.last().sha256 != document.savedArtifactHash)
        {
            error = QStringLiteral("Native source binding disagrees with logical path, terminal path, or saved hash.");
            return false;
        }
        QSet<QString> sourcePaths;
        for (const auto &artifact : document.sourceArtifacts)
        {
            const QString extension = QFileInfo(artifact.path).suffix();
            if (artifact.path.toUtf8().size() > MaxPathBytes || !QDir::isAbsolutePath(artifact.path)
                || QDir::cleanPath(artifact.path) != artifact.path
                || (extension != QStringLiteral("ram") && extension != QStringLiteral("rbm"))
                || !validHash(artifact.sha256))
            {
                error = QStringLiteral("Native source artifact path or hash violates v1 bounds.");
                return false;
            }
#ifdef Q_OS_WIN
            const QString pathKey = artifact.path.toCaseFolded();
#else
            const QString pathKey = artifact.path;
#endif
            if (sourcePaths.contains(pathKey))
            {
                error = QStringLiteral("Native source binding repeats an artifact path.");
                return false;
            }
            sourcePaths.insert(pathKey);
        }
    }
    if (document.modelSha256.isEmpty())
    {
        if (!document.snapshotSourceRevision.isEmpty() || !document.snapshotRelativePath.isEmpty()
            || !document.snapshotInstanceRef.isEmpty())
        {
            error = QStringLiteral("A snapshot path or reference exists without a snapshot hash.");
            return false;
        }
    }
    else
    {
        const QString expectedPath = QStringLiteral("models/%1/snapshots/%2/model.rbm")
                                         .arg(document.persistentDocumentId, document.modelSha256);
        if (!validRevision(document.snapshotSourceRevision)
            || document.snapshotRelativePath != expectedPath
            || !exactKeys(document.snapshotInstanceRef,
                          {QStringLiteral("documentId"), QStringLiteral("sessionEpoch"),
                           QStringLiteral("documentRevision")}))
        {
            error = QStringLiteral("A snapshot reference does not match the v1 managed layout.");
            return false;
        }
        const auto &ref = document.snapshotInstanceRef;
        const QJsonValue revision = ref.value(QStringLiteral("documentRevision"));
        if (!ref.value(QStringLiteral("documentId")).isString()
            || !ref.value(QStringLiteral("sessionEpoch")).isString()
            || !validUuid(ref.value(QStringLiteral("documentId")).toString())
            || !validUuid(ref.value(QStringLiteral("sessionEpoch")).toString())
            || !revision.isDouble() || !std::isfinite(revision.toDouble())
            || revision.toDouble() < 0 || std::floor(revision.toDouble()) != revision.toDouble()
            || revision.toDouble() > 9007199254740991.0)
        {
            error = QStringLiteral("A snapshot runtime reference is invalid.");
            return false;
        }
    }
    for (const QJsonValue &orphanValue : document.orphanSnapshots)
    {
        if (!orphanValue.isObject())
        {
            error = QStringLiteral("Orphan snapshot metadata is not an object.");
            return false;
        }
        const QJsonObject orphan = orphanValue.toObject();
        if (!exactKeys(orphan, {QStringLiteral("sourceRevision"), QStringLiteral("modelSha256"),
                                QStringLiteral("relativePath"), QStringLiteral("instanceRef")})
            || !orphan.value(QStringLiteral("sourceRevision")).isString()
            || !orphan.value(QStringLiteral("modelSha256")).isString()
            || !orphan.value(QStringLiteral("relativePath")).isString()
            || !orphan.value(QStringLiteral("instanceRef")).isObject())
        {
            error = QStringLiteral("Orphan snapshot metadata has unknown or invalid fields.");
            return false;
        }
        const QString orphanRevision = orphan.value(QStringLiteral("sourceRevision")).toString();
        const QString orphanHash = orphan.value(QStringLiteral("modelSha256")).toString();
        const QString expectedPath = QStringLiteral("models/%1/snapshots/%2/model.rbm")
                                         .arg(document.persistentDocumentId, orphanHash);
        if (!validRevision(orphanRevision) || revisionNumber(orphanRevision) > revisionNumber(document.sourceRevision)
            || !validHash(orphanHash)
            || orphan.value(QStringLiteral("relativePath")).toString() != expectedPath
            || !validInstanceReference(orphan.value(QStringLiteral("instanceRef")).toObject()))
        {
            error = QStringLiteral("Orphan snapshot metadata violates the v1 path or identity rules.");
            return false;
        }
    }
    return true;
}

bool validateManifest(const QJsonObject &manifest, QString &error)
{
    if (!exactKeys(manifest, {QStringLiteral("storageVersion"), QStringLiteral("projectId"),
                              QStringLiteral("documents"), QStringLiteral("sourcePathIndex")}))
    {
        error = QStringLiteral("Project manifest has unknown or missing top-level fields.");
        return false;
    }
    const QJsonValue version = manifest.value(QStringLiteral("storageVersion"));
    if (!version.isDouble() || version.toInt(-1) != RProjectCatalog::StorageVersion
        || version.toDouble() != double(RProjectCatalog::StorageVersion))
    {
        error = QStringLiteral("Unsupported project manifest storage version.");
        return false;
    }
    if (!manifest.value(QStringLiteral("projectId")).isString()
        || !validUuid(manifest.value(QStringLiteral("projectId")).toString())
        || !manifest.value(QStringLiteral("documents")).isObject()
        || !manifest.value(QStringLiteral("sourcePathIndex")).isObject()
        || !finiteJson(QJsonValue(manifest)))
    {
        error = QStringLiteral("Project manifest has invalid top-level values.");
        return false;
    }

    const QJsonObject docs = manifest.value(QStringLiteral("documents")).toObject();
    const QJsonObject index = manifest.value(QStringLiteral("sourcePathIndex")).toObject();
    if (docs.size() > MaxDocuments
        || index.size() > MaxDocuments * (MaxNativeLinkDepth + 1))
    {
        error = QStringLiteral("Project manifest exceeds the v1 document limit.");
        return false;
    }
    QJsonObject expectedIndex;
    for (auto it = docs.begin(); it != docs.end(); ++it)
    {
        RProjectCatalog::Document document;
        if (!documentFromJson(it.key(), it.value(), document, error)) return false;
        if (!document.originDocumentId.isEmpty() && !docs.contains(document.originDocumentId))
        {
            error = QStringLiteral("Project document origin does not identify a retained document record.");
            return false;
        }
        if (!document.sourcePath.isEmpty() && !document.tombstone)
        {
            for (const auto &artifact : document.sourceArtifacts)
            {
                const QString key =
#ifdef Q_OS_WIN
                    artifact.path.toCaseFolded();
#else
                    artifact.path;
#endif
                if (expectedIndex.contains(key))
                {
                    error = QStringLiteral("Two active project documents claim the same source artifact path.");
                    return false;
                }
                expectedIndex.insert(key, document.persistentDocumentId);
            }
        }
    }
    if (index != expectedIndex)
    {
        error = QStringLiteral("Project source-path index does not match active document records.");
        return false;
    }
    for (auto it = index.begin(); it != index.end(); ++it)
        if (!it.value().isString() || !validUuid(it.value().toString()))
        {
            error = QStringLiteral("Project source-path index contains an invalid identity.");
            return false;
        }
    return true;
}

class DuplicateKeyScanner
{
public:
    explicit DuplicateKeyScanner(const QByteArray &json) : bytes(json) {}

    bool containsDuplicateKeys()
    {
        skipWhitespace();
        if (!scanValue(0)) return false;
        skipWhitespace();
        return position == bytes.size() && duplicate;
    }

private:
    const QByteArray &bytes;
    qsizetype position = 0;
    int nodeCount = 0;
    bool duplicate = false;

    void skipWhitespace()
    {
        while (position < bytes.size())
        {
            const char c = bytes.at(position);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position;
        }
    }

    bool scanString(QByteArray &token)
    {
        if (position >= bytes.size() || bytes.at(position) != '"') return false;
        const qsizetype start = position++;
        while (position < bytes.size())
        {
            const char c = bytes.at(position++);
            if (c == '"')
            {
                token = bytes.mid(start, position - start);
                return true;
            }
            if (c == '\\')
            {
                if (position >= bytes.size()) return false;
                ++position;
            }
        }
        return false;
    }

    bool decodedString(const QByteArray &token, QString &value)
    {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(QByteArrayLiteral("[") + token
                                                                   + QByteArrayLiteral("]"), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray()
            || document.array().size() != 1 || !document.array().first().isString()) return false;
        value = document.array().first().toString();
        return true;
    }

    bool scanObject(int depth)
    {
        ++position;
        skipWhitespace();
        if (position < bytes.size() && bytes.at(position) == '}')
        {
            ++position;
            return true;
        }
        QSet<QString> keys;
        while (position < bytes.size())
        {
            QByteArray token;
            QString key;
            if (!scanString(token) || !decodedString(token, key)) return false;
            if (keys.contains(key)) duplicate = true;
            keys.insert(key);
            skipWhitespace();
            if (position >= bytes.size() || bytes.at(position++) != ':') return false;
            skipWhitespace();
            if (!scanValue(depth + 1)) return false;
            skipWhitespace();
            if (position >= bytes.size()) return false;
            const char delimiter = bytes.at(position++);
            if (delimiter == '}') return true;
            if (delimiter != ',') return false;
            skipWhitespace();
        }
        return false;
    }

    bool scanArray(int depth)
    {
        ++position;
        skipWhitespace();
        if (position < bytes.size() && bytes.at(position) == ']')
        {
            ++position;
            return true;
        }
        while (position < bytes.size())
        {
            if (!scanValue(depth + 1)) return false;
            skipWhitespace();
            if (position >= bytes.size()) return false;
            const char delimiter = bytes.at(position++);
            if (delimiter == ']') return true;
            if (delimiter != ',') return false;
            skipWhitespace();
        }
        return false;
    }

    bool scanValue(int depth)
    {
        if (depth > MaxJsonDepth || ++nodeCount > MaxJsonNodes) return false;
        skipWhitespace();
        if (position >= bytes.size()) return false;
        const char c = bytes.at(position);
        if (c == '{') return scanObject(depth);
        if (c == '[') return scanArray(depth);
        if (c == '"')
        {
            QByteArray ignored;
            return scanString(ignored);
        }
        const qsizetype start = position;
        while (position < bytes.size())
        {
            const char value = bytes.at(position);
            if (value == ',' || value == ']' || value == '}' || value == ' '
                || value == '\t' || value == '\r' || value == '\n') break;
            ++position;
        }
        return position > start;
    }
};

bool readJsonObject(const QString &path, qint64 maximumBytes, QJsonObject &object, QString &error)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile())
    {
        error = QStringLiteral("Expected a regular non-symbolic JSON file: %1").arg(path);
        return false;
    }
    if (info.size() < 0 || info.size() > maximumBytes)
    {
        error = QStringLiteral("JSON file exceeds its size limit: %1").arg(path);
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("Cannot read JSON file: %1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumBytes)
    {
        error = QStringLiteral("Cannot read bounded JSON file: %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("Invalid JSON object in %1: %2").arg(path, parseError.errorString());
        return false;
    }
    if (DuplicateKeyScanner(bytes).containsDuplicateKeys())
    {
        error = QStringLiteral("JSON object contains duplicate keys: %1").arg(path);
        return false;
    }
    object = document.object();
    if (!finiteJson(QJsonValue(object)))
    {
        error = QStringLiteral("JSON values exceed the finite-value or nesting bounds: %1").arg(path);
        return false;
    }
    return true;
}

bool validWorkspaceRoot(const QString &root, QString &error)
{
    const QFileInfo info(root);
    if (!info.isDir() || info.isSymLink() || info.canonicalFilePath().isEmpty()
        || QDir::cleanPath(QDir::fromNativeSeparators(info.canonicalFilePath())) != root)
    {
        error = QStringLiteral("Project workspace root changed or resolves through a symbolic link.");
        return false;
    }
    return true;
}

bool readManifest(const QString &root, QJsonObject &manifest, QString &error)
{
    if (!validWorkspaceRoot(root, error)) return false;
    const QString path = QDir(root).filePath(QStringLiteral("project.json"));
    if (!readJsonObject(path, MaxManifestBytes, manifest, error)) return false;
    if (!validateManifest(manifest, error))
    {
        error = QStringLiteral("Project manifest is corrupt or unsupported and was preserved: %1").arg(error);
        return false;
    }
    return true;
}

QString pathIndexKey(const QString &canonicalPath)
{
#ifdef Q_OS_WIN
    return canonicalPath.toCaseFolded();
#else
    return canonicalPath;
#endif
}

bool canonicalExistingFile(const QString &path, QString &canonical, QString &error)
{
    if (path.isEmpty() || path.toUtf8().size() > MaxPathBytes)
    {
        error = QStringLiteral("Source path is empty or exceeds the v1 path limit.");
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink())
    {
        error = QStringLiteral("Source path must name an existing regular non-symbolic file.");
        return false;
    }
    canonical = QDir::cleanPath(QDir::fromNativeSeparators(info.canonicalFilePath()));
    if (canonical.isEmpty() || canonical.toUtf8().size() > MaxPathBytes)
    {
        error = QStringLiteral("Could not canonicalize the source path within the v1 limit.");
        return false;
    }
    return true;
}

bool fileHash(const QString &path, QString &hash, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("Cannot read file for SHA-256: %1").arg(path);
        return false;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&file) || file.error() != QFileDevice::NoError)
    {
        error = QStringLiteral("Failed while hashing file: %1").arg(path);
        return false;
    }
    hash = QString::fromLatin1(digest.result().toHex());
    return true;
}

bool readNativeHeader(const QString &path, RFileHeader &header, qint64 &fileSize, QString &error)
{
    const QFileInfo info(path);
    fileSize = info.size();
    if (info.isSymLink() || !info.isFile() || fileSize <= 0 || fileSize > MaxNativeArtifactBytes)
    {
        error = QStringLiteral("Native source artifact is not a bounded regular file.");
        return false;
    }
    const QString extension = info.suffix();
    const bool ascii = extension == QStringLiteral("ram");
    if (!ascii && extension != QStringLiteral("rbm"))
    {
        error = QStringLiteral("Native source artifact must use a .ram or .rbm extension.");
        return false;
    }
    qint64 headerEnd = 0;
    if (ascii)
    {
        QFile preflight(path);
        if (!preflight.open(QIODevice::ReadOnly))
        {
            error = QStringLiteral("Could not open native source header.");
            return false;
        }
        QByteArray headerLines[3];
        for (int lineIndex = 0; lineIndex < 3; ++lineIndex)
        {
            const QByteArray line = preflight.readLine(MaxNativeHeaderLineBytes + 1);
            if (line.isEmpty() || line.size() > MaxNativeHeaderLineBytes || !line.endsWith('\n'))
            {
                error = QStringLiteral("Native ASCII header exceeds the bounded header format.");
                return false;
            }
            const QString decodedLine = QString::fromUtf8(line.constData(), line.size());
            if (decodedLine.toUtf8() != line)
            {
                error = QStringLiteral("Native ASCII header is not valid UTF-8.");
                return false;
            }
            headerLines[lineIndex] = line;
            if (lineIndex == 2)
            {
                QByteArray value = line.trimmed();
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                {
                    value = value.mid(1, value.size() - 2);
                    if (value.contains('"'))
                    {
                        error = QStringLiteral("Native ASCII header contains an invalid quoted path.");
                        return false;
                    }
                }
                else if (value.contains('"') || value.contains(' ') || value.contains('\t'))
                {
                    error = QStringLiteral("Native ASCII header contains an invalid path field.");
                    return false;
                }
                if (value.size() > MaxPathBytes)
                {
                    error = QStringLiteral("Native ASCII header path exceeds the v1 limit.");
                    return false;
                }
            }
        }
        headerEnd = preflight.pos();
        preflight.close();
        const QString versionField = QString::fromUtf8(headerLines[0].trimmed());
        static const QRegularExpression versionPattern(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
        if (!versionPattern.match(versionField).hasMatch())
        {
            error = QStringLiteral("Native ASCII version header is invalid.");
            return false;
        }
        bool typeOk = false;
        const int typeValue = QString::fromUtf8(headerLines[1].trimmed()).toInt(&typeOk);
        if (!typeOk || typeValue <= R_FILE_TYPE_NONE || typeValue >= R_FILE_N_TYPES)
        {
            error = QStringLiteral("Native ASCII file type header is invalid.");
            return false;
        }
        RFile file(path, RFile::ASCII);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            error = QStringLiteral("Could not open native source header.");
            return false;
        }
        try
        {
            RFileIO::readAscii(file, header);
        }
        catch (const RError &)
        {
            error = QStringLiteral("Native ASCII source header is invalid.");
            return false;
        }
    }
    else
    {
        if (fileSize < qint64(4 * sizeof(unsigned int) + 2 * sizeof(int)))
        {
            error = QStringLiteral("Native binary source header is truncated.");
            return false;
        }
        RFile file(path, RFile::BINARY);
        if (!file.open(QIODevice::ReadOnly))
        {
            error = QStringLiteral("Could not open native source header.");
            return false;
        }
        try
        {
            RVersion version;
            RFileType type = R_FILE_TYPE_NONE;
            int informationSize = 0;
            RFileIO::readBinary(file, version);
            RFileIO::readBinary(file, type);
            RFileIO::readBinary(file, informationSize);
            if (informationSize < 0 || informationSize > MaxPathBytes
                || informationSize > file.size() - file.pos())
            {
                error = QStringLiteral("Native binary header information exceeds the v1 bound.");
                return false;
            }
            const QByteArray informationBytes = file.read(informationSize);
            if (informationBytes.size() != informationSize)
            {
                error = QStringLiteral("Native binary source header is truncated.");
                return false;
            }
            const QString information = QString::fromUtf8(informationBytes.constData(), informationBytes.size());
            if (information.toUtf8() != informationBytes)
            {
                error = QStringLiteral("Native binary source header is not valid UTF-8.");
                return false;
            }
            header = RFileHeader(type, version, information);
            headerEnd = file.pos();
        }
        catch (const RError &)
        {
            error = QStringLiteral("Native binary source header is invalid.");
            return false;
        }
    }
    if (header.getType() != R_FILE_TYPE_LINK && header.getType() != R_FILE_TYPE_MODEL)
    {
        error = QStringLiteral("Native source artifact header is neither LINK nor MODEL.");
        return false;
    }
    if (header.getVersion() > RModel::version)
    {
        error = QStringLiteral("Native source artifact uses an unsupported future format version.");
        return false;
    }
    if (header.getType() == R_FILE_TYPE_MODEL && fileSize <= headerEnd)
    {
        error = QStringLiteral("Native MODEL artifact has no body after its header.");
        return false;
    }
    return true;
}

bool sameSourceBinding(const RProjectCatalog::SourceBinding &left,
                       const RProjectCatalog::SourceBinding &right)
{
    if (left.logicalSourcePath != right.logicalSourcePath
        || left.resolvedSourcePath != right.resolvedSourcePath
        || left.savedArtifactHash != right.savedArtifactHash
        || left.sourceArtifacts.size() != right.sourceArtifacts.size()) return false;
    for (qsizetype index = 0; index < left.sourceArtifacts.size(); ++index)
        if (left.sourceArtifacts[index].path != right.sourceArtifacts[index].path
            || left.sourceArtifacts[index].sha256 != right.sourceArtifacts[index].sha256) return false;
    return true;
}

QString pathIndexKey(const QString &canonicalPath);

bool managedCatalogPath(const QString &root, const QString &path)
{
    const QString canonicalRoot = QDir::cleanPath(root);
    const QString pathKey = pathIndexKey(path);
    const QStringList managedFiles{QStringLiteral("project.json"), QStringLiteral("workflow-state.json"),
                                   QStringLiteral(".project.lock"), QStringLiteral(".workflow.lock")};
    for (const QString &file : managedFiles)
        if (pathKey == pathIndexKey(QDir(canonicalRoot).filePath(file))) return true;
    const QStringList managedDirs{QStringLiteral("models"), QStringLiteral("jobs"),
                                  QStringLiteral("operations"), QStringLiteral("conversations"),
                                  QStringLiteral("extensions")};
    for (const QString &directory : managedDirs)
    {
        const QString base = pathIndexKey(QDir(canonicalRoot).filePath(directory));
        if (pathKey == base || pathKey.startsWith(base + QLatin1Char('/'))) return true;
    }
    return false;
}

bool validWorkflowState(const QString &path, QString &projectId)
{
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink() || !info.isFile()) return false;
    QString error;
    QJsonObject state;
    if (!readJsonObject(path, MaxWorkflowStateBytes, state, error)
        || !exactKeys(state, {QStringLiteral("storageVersion"), QStringLiteral("projectId"),
                              QStringLiteral("modelId"), QStringLiteral("sessionId"),
                              QStringLiteral("revision"), QStringLiteral("auditRevision"),
                              QStringLiteral("configuration"), QStringLiteral("jobs"),
                              QStringLiteral("previews"), QStringLiteral("audit"),
                              QStringLiteral("extensions")})) return false;
    const auto version = state.value(QStringLiteral("storageVersion"));
    const auto revision = state.value(QStringLiteral("revision"));
    const auto auditRevision = state.value(QStringLiteral("auditRevision"));
    const auto audit = state.value(QStringLiteral("audit"));
    const QJsonObject configuration = state.value(QStringLiteral("configuration")).toObject();
    const QJsonValue dt = configuration.value(QStringLiteral("dt"));
    const QJsonValue steps = configuration.value(QStringLiteral("steps"));
    const QJsonValue damping = configuration.value(QStringLiteral("targetDamping"));
    const QJsonValue model = configuration.value(QStringLiteral("model"));
    if (!version.isDouble() || version.toDouble() != 1.0
        || !state.value(QStringLiteral("projectId")).isString()
        || !state.value(QStringLiteral("modelId")).isString()
        || !state.value(QStringLiteral("sessionId")).isString()
        || !validUuid(state.value(QStringLiteral("projectId")).toString())
        || !validUuid(state.value(QStringLiteral("modelId")).toString())
        || !validUuid(state.value(QStringLiteral("sessionId")).toString())
        || !revision.isDouble() || !auditRevision.isDouble()
        || !std::isfinite(revision.toDouble()) || !std::isfinite(auditRevision.toDouble())
        || revision.toDouble() < 0 || auditRevision.toDouble() < 0
        || std::floor(revision.toDouble()) != revision.toDouble()
        || std::floor(auditRevision.toDouble()) != auditRevision.toDouble()
        || revision.toDouble() > 9007199254740991.0
        || auditRevision.toDouble() > 9007199254740991.0
        || !state.value(QStringLiteral("configuration")).isObject()
        || (configuration.size() != 3 && configuration.size() != 4)
        || !configuration.contains(QStringLiteral("dt"))
        || !configuration.contains(QStringLiteral("steps"))
        || !configuration.contains(QStringLiteral("targetDamping"))
        || !dt.isDouble() || !std::isfinite(dt.toDouble()) || dt.toDouble() <= 0 || dt.toDouble() > 0.02
        || !steps.isDouble() || !std::isfinite(steps.toDouble()) || std::floor(steps.toDouble()) != steps.toDouble()
        || steps.toDouble() < 1 || steps.toDouble() > 5000
        || !damping.isDouble() || !std::isfinite(damping.toDouble())
        || damping.toDouble() <= 0 || damping.toDouble() > 0.5
        || (configuration.size() == 4
            && (!model.isString() || (model.toString() != QStringLiteral("four-rod")
                                      && model.toString() != QStringLiteral("cantilever-beam"))))
        || !state.value(QStringLiteral("jobs")).isObject()
        || !state.value(QStringLiteral("previews")).isObject()
        || !state.value(QStringLiteral("extensions")).isObject()
        || !audit.isArray() || auditRevision.toDouble() != audit.toArray().size()) return false;
    const QJsonObject jobs = state.value(QStringLiteral("jobs")).toObject();
    const QSet<QString> knownStates{QStringLiteral("succeeded"), QStringLiteral("failed"),
                                    QStringLiteral("cancelled"), QStringLiteral("interrupted"),
                                    QStringLiteral("queued"), QStringLiteral("running"),
                                    QStringLiteral("cancelling")};
    for (auto it = jobs.begin(); it != jobs.end(); ++it)
    {
        if (!validUuid(it.key()) || !it.value().isObject()) return false;
        const auto job = it.value().toObject();
        if (!job.value(QStringLiteral("jobId")).isString()
            || job.value(QStringLiteral("jobId")).toString() != it.key()
            || !job.value(QStringLiteral("state")).isString()
            || !knownStates.contains(job.value(QStringLiteral("state")).toString())) return false;
    }
    projectId = state.value(QStringLiteral("projectId")).toString();
    return true;
}

bool writeManifest(const QString &root, const QJsonObject &manifest, QString &error)
{
    if (!validWorkspaceRoot(root, error)) return false;
    if (!validateManifest(manifest, error)) return false;
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaxManifestBytes)
    {
        error = QStringLiteral("Project manifest would exceed its v1 size limit.");
        return false;
    }
    const QString path = QDir(root).filePath(QStringLiteral("project.json"));
    if (QFileInfo(path).isSymLink())
    {
        error = QStringLiteral("Project manifest path is a symbolic link.");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = QStringLiteral("Could not atomically write project manifest: %1").arg(file.errorString());
        return false;
    }
    return true;
}

std::unique_ptr<QLockFile> acquireLock(const QString &root, QString &error,
                                      QString *errorCode = nullptr)
{
    if (!validWorkspaceRoot(root, error))
    {
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    const QString path = QDir(root).filePath(QStringLiteral(".project.lock"));
    if (QFileInfo(path).isSymLink())
    {
        error = QStringLiteral("Project lock path is a symbolic link.");
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    auto lock = std::make_unique<QLockFile>(path);
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(0))
    {
        error = QStringLiteral("Project workspace is locked by another process.");
        setErrorCode(errorCode, QStringLiteral("catalog_locked"));
        return {};
    }
    return lock;
}

QString snapshotPath(const QString &documentId, const QString &sha256)
{
    return QStringLiteral("models/%1/snapshots/%2/model.rbm").arg(documentId, sha256);
}

bool prepareManagedDirectory(const QString &root, const QString &relativeDirectory,
                             QString &absoluteDirectory, QString &error)
{
    if (!validWorkspaceRoot(root, error)) return false;
    if (relativeDirectory.isEmpty() || QDir::isAbsolutePath(relativeDirectory)
        || QDir::cleanPath(relativeDirectory) != relativeDirectory
        || relativeDirectory.startsWith(QStringLiteral("../"))
        || relativeDirectory == QStringLiteral(".."))
    {
        error = QStringLiteral("Managed path is not a clean workspace-relative directory.");
        return false;
    }
    QString current = root;
    const QStringList components = relativeDirectory.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components)
    {
        if (component == QStringLiteral(".") || component == QStringLiteral(".."))
        {
            error = QStringLiteral("Managed path contains a traversal component.");
            return false;
        }
        current = QDir(current).filePath(component);
        const QFileInfo info(current);
        if (info.exists())
        {
            if (info.isSymLink() || !info.isDir())
            {
                error = QStringLiteral("Managed workspace path contains a symbolic link or non-directory.");
                return false;
            }
        }
        else if (!QDir().mkdir(current))
        {
            const QFileInfo racedDirectory(current);
            if (!racedDirectory.isDir() || racedDirectory.isSymLink())
            {
                error = QStringLiteral("Could not create a managed workspace directory: %1").arg(current);
                return false;
            }
        }
    }
    absoluteDirectory = current;
    return true;
}

bool ensureManagedFilePath(const QString &root, const QString &relativeFile,
                           QString &absoluteFile, QString &error)
{
    const QString clean = QDir::cleanPath(relativeFile);
    if (clean != relativeFile || QDir::isAbsolutePath(relativeFile)
        || relativeFile.startsWith(QStringLiteral("../")) || relativeFile == QStringLiteral(".."))
    {
        error = QStringLiteral("Managed file path is not workspace-relative.");
        return false;
    }
    const int slash = relativeFile.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
    {
        error = QStringLiteral("Managed file path has no managed directory.");
        return false;
    }
    QString parent;
    if (!prepareManagedDirectory(root, relativeFile.left(slash), parent, error)) return false;
    absoluteFile = QDir(parent).filePath(relativeFile.mid(slash + 1));
    const QFileInfo fileInfo(absoluteFile);
    if (fileInfo.exists() && (fileInfo.isSymLink() || !fileInfo.isFile()))
    {
        error = QStringLiteral("Managed snapshot path is a symbolic link or non-file.");
        return false;
    }
    return true;
}

bool validInstanceRef(const RProjectCatalog::InstanceRef &ref, QString &error)
{
    if (!validUuid(ref.documentId) || !validUuid(ref.sessionEpoch)
        || ref.documentRevision > quint64(9007199254740991ULL))
    {
        error = QStringLiteral("Runtime instanceRef requires UUID documentId/sessionEpoch and a bounded revision.");
        return false;
    }
    return true;
}

bool loadCurrent(const QString &root, const QString &expectedProjectId,
                 QJsonObject &manifest, QString &error)
{
    if (!readManifest(root, manifest, error)) return false;
    if (manifest.value(QStringLiteral("projectId")).toString() != expectedProjectId)
    {
        error = QStringLiteral("Project identity changed after this catalog was opened.");
        return false;
    }
    return true;
}

bool findDocument(const QJsonObject &manifest, const QString &id,
                  RProjectCatalog::Document &document, QString &error)
{
    if (!validUuid(id))
    {
        error = QStringLiteral("Persistent document ID is not a canonical UUID.");
        return false;
    }
    const QJsonValue value = manifest.value(QStringLiteral("documents")).toObject().value(id);
    if (value.isUndefined())
    {
        error = QStringLiteral("Persistent document ID is not registered in this project.");
        return false;
    }
    return documentFromJson(id, value, document, error);
}

bool currentSourceMatches(const RProjectCatalog::Document &document,
                          QString *error, QString *errorCode)
{
    if (document.sourcePath.isEmpty()) return true;
    QString localError, localCode;
    const auto current = RProjectCatalog::resolveSourceBinding(document.sourcePath, &localError, &localCode);
    const RProjectCatalog::SourceBinding saved{document.sourcePath, document.resolvedSourcePath,
                                                document.sourceArtifacts, document.savedArtifactHash};
    if (!current || !sameSourceBinding(*current, saved))
    {
        setError(error, QStringLiteral("Registered native source bytes or LINK targets changed; explicit reload is required."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return false;
    }
    return true;
}

void applySourceBinding(RProjectCatalog::Document &document,
                        const RProjectCatalog::SourceBinding &binding)
{
    document.sourcePath = binding.logicalSourcePath;
    document.resolvedSourcePath = binding.resolvedSourcePath;
    document.sourceArtifacts = binding.sourceArtifacts;
    document.savedArtifactHash = binding.savedArtifactHash;
}

void clearCurrentSnapshot(RProjectCatalog::Document &document)
{
    document.modelSha256.clear();
    document.snapshotSourceRevision.clear();
    document.snapshotRelativePath.clear();
    document.snapshotInstanceRef = {};
}

bool archiveCurrentSnapshot(RProjectCatalog::Document &document, QString &error)
{
    if (document.modelSha256.isEmpty()) return true;
    if (document.orphanSnapshots.size() >= MaxOrphanSnapshots)
    {
        error = QStringLiteral("Document reached its bounded orphan snapshot history limit.");
        return false;
    }
    document.orphanSnapshots.append(QJsonObject{
        {QStringLiteral("sourceRevision"), document.snapshotSourceRevision},
        {QStringLiteral("modelSha256"), document.modelSha256},
        {QStringLiteral("relativePath"), document.snapshotRelativePath},
        {QStringLiteral("instanceRef"), document.snapshotInstanceRef}});
    clearCurrentSnapshot(document);
    return true;
}

bool canonicalPlannedOutputPath(const QString &path, QString &canonical,
                                QString &error)
{
    if (path.isEmpty() || path.toUtf8().size() > MaxPathBytes || !QDir::isAbsolutePath(path))
    {
        error = QStringLiteral("Planned native output path must be an absolute bounded path.");
        return false;
    }
    const QFileInfo info(path);
    if (info.isSymLink())
    {
        error = QStringLiteral("Planned native output path cannot be a symbolic link.");
        return false;
    }
    if (info.exists())
    {
        if (!info.isFile() || info.canonicalFilePath().isEmpty())
        {
            error = QStringLiteral("Planned native output must be a regular file path.");
            return false;
        }
        canonical = QDir::cleanPath(QDir::fromNativeSeparators(info.canonicalFilePath()));
    }
    else
    {
        const QString parent = info.dir().canonicalPath();
        if (parent.isEmpty() || info.fileName().isEmpty()
            || info.fileName() == QStringLiteral(".") || info.fileName() == QStringLiteral(".."))
        {
            error = QStringLiteral("Planned native output parent must exist and resolve safely.");
            return false;
        }
        canonical = QDir::cleanPath(QDir(parent).filePath(info.fileName()));
    }
    const QString extension = QFileInfo(canonical).suffix();
    if (extension != QStringLiteral("ram") && extension != QStringLiteral("rbm"))
    {
        error = QStringLiteral("Planned native output must use a .ram or .rbm extension.");
        return false;
    }
    if (canonical.toUtf8().size() > MaxPathBytes)
    {
        error = QStringLiteral("Planned native output path exceeds the v1 path limit.");
        return false;
    }
    return true;
}

void storeDocument(QJsonObject &manifest, const RProjectCatalog::Document &document)
{
    QJsonObject documents = manifest.value(QStringLiteral("documents")).toObject();
    documents.insert(document.persistentDocumentId, documentJson(document));
    manifest.insert(QStringLiteral("documents"), documents);
}

void rebuildIndex(QJsonObject &manifest)
{
    QJsonObject index;
    const QJsonObject documents = manifest.value(QStringLiteral("documents")).toObject();
    for (auto it = documents.begin(); it != documents.end(); ++it)
    {
        const auto document = it.value().toObject();
        if (document.value(QStringLiteral("sourcePath")).toString().isEmpty()
            || document.value(QStringLiteral("tombstone")).toBool()) continue;
        for (const QJsonValue &artifactValue : document.value(QStringLiteral("sourceArtifacts")).toArray())
        {
            const QString path = artifactValue.toObject().value(QStringLiteral("path")).toString();
            index.insert(pathIndexKey(path), it.key());
        }
    }
    manifest.insert(QStringLiteral("sourcePathIndex"), index);
}

bool commitMutation(const QString &root, QJsonObject &manifest, QString &error)
{
    rebuildIndex(manifest);
    if (!writeManifest(root, manifest, error)) return false;
    return true;
}
}

namespace
{
constexpr qint64 MaxOperationLedgerBytes = 64 * 1024 * 1024;
constexpr int MaxDurableOperations = 8192;
constexpr int MaxOperationEvents = 32;
constexpr int MaxOperationArtifacts = MaxNativeLinkDepth + 2;
constexpr int MaxCloseRetries = 4096;

bool acquireRuntimeLease(const QString &root, const QString &runtimeId,
                         std::unique_ptr<QLockFile> &lease, QString &error)
{
    if (!validUuid(runtimeId))
    {
        error = QStringLiteral("Project runtime identity is invalid.");
        return false;
    }
    QString directory;
    if (!prepareManagedDirectory(root, QStringLiteral("operations/runtimes"), directory, error))
        return false;
    const QString path = QDir(directory).filePath(runtimeId + QStringLiteral(".lock"));
    if (QFileInfo(path).isSymLink())
    {
        error = QStringLiteral("Project runtime lock path is a symbolic link.");
        return false;
    }
    auto candidate = std::make_unique<QLockFile>(path);
    // This lease lives for the catalog lifetime. Disable age-based staleness;
    // QLockFile's owner PID/process-name check decides whether it is abandoned.
    candidate->setStaleLockTime(0);
    if (!candidate->tryLock(0))
    {
        error = QStringLiteral("Could not acquire the project runtime liveness lease.");
        return false;
    }
    lease = std::move(candidate);
    return true;
}

QString operationPhaseName(RProjectCatalog::OperationPhase phase)
{
    switch (phase)
    {
    case RProjectCatalog::OperationPhase::Intent: return QStringLiteral("intent");
    case RProjectCatalog::OperationPhase::Accepted: return QStringLiteral("accepted");
    case RProjectCatalog::OperationPhase::ArtifactPublished: return QStringLiteral("artifact_published");
    case RProjectCatalog::OperationPhase::Observed: return QStringLiteral("observed");
    case RProjectCatalog::OperationPhase::Unknown: return QStringLiteral("unknown");
    case RProjectCatalog::OperationPhase::Conflict: return QStringLiteral("conflict");
    }
    return {};
}

bool operationPhase(const QJsonValue &value, RProjectCatalog::OperationPhase &phase)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text == QStringLiteral("intent")) phase = RProjectCatalog::OperationPhase::Intent;
    else if (text == QStringLiteral("accepted")) phase = RProjectCatalog::OperationPhase::Accepted;
    else if (text == QStringLiteral("artifact_published")) phase = RProjectCatalog::OperationPhase::ArtifactPublished;
    else if (text == QStringLiteral("observed")) phase = RProjectCatalog::OperationPhase::Observed;
    else if (text == QStringLiteral("unknown")) phase = RProjectCatalog::OperationPhase::Unknown;
    else if (text == QStringLiteral("conflict")) phase = RProjectCatalog::OperationPhase::Conflict;
    else return false;
    return true;
}

bool validPhaseTransition(RProjectCatalog::OperationPhase from,
                          RProjectCatalog::OperationPhase to)
{
    using Phase = RProjectCatalog::OperationPhase;
    if (from == to) return true;
    switch (from)
    {
    case Phase::Intent:
        return to == Phase::Accepted || to == Phase::Observed
            || to == Phase::Unknown || to == Phase::Conflict;
    case Phase::Accepted:
        return to == Phase::ArtifactPublished || to == Phase::Observed
            || to == Phase::Unknown || to == Phase::Conflict;
    case Phase::ArtifactPublished:
        return to == Phase::Observed || to == Phase::Unknown || to == Phase::Conflict;
    case Phase::Unknown:
        return to == Phase::ArtifactPublished || to == Phase::Observed || to == Phase::Conflict;
    case Phase::Observed:
        return to == Phase::Unknown || to == Phase::Conflict;
    case Phase::Conflict:
        return false;
    }
    return false;
}

bool validDiagnosticCode(const QString &value)
{
    return value.isEmpty()
        || QRegularExpression(QStringLiteral("^[a-z][a-z0-9_.-]{0,63}$")).match(value).hasMatch();
}

bool safeObservationKey(const QString &key)
{
    QString normalized = key.toCaseFolded();
    normalized.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return !normalized.contains(QStringLiteral("token"))
        && !normalized.contains(QStringLiteral("secret"))
        && !normalized.contains(QStringLiteral("credential"))
        && !normalized.contains(QStringLiteral("password"))
        && !normalized.contains(QStringLiteral("apikey"))
        && normalized != QStringLiteral("auth")
        && !normalized.contains(QStringLiteral("authorization"))
        && !normalized.contains(QStringLiteral("authheader"))
        && !normalized.contains(QStringLiteral("cookie"));
}

bool boundedObservationValue(const QJsonValue &value, int depth, int &nodes)
{
    if (depth > 16 || ++nodes > 4096) return false;
    if (value.isString()) return value.toString().toUtf8().size() <= 16 * 1024;
    if (value.isDouble()) return std::isfinite(value.toDouble());
    if (value.isArray())
    {
        const QJsonArray array = value.toArray();
        if (array.size() > 256) return false;
        for (const QJsonValue &item : array)
            if (!boundedObservationValue(item, depth + 1, nodes)) return false;
        return true;
    }
    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        if (object.size() > 256) return false;
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (it.key().toUtf8().size() > 128 || !safeObservationKey(it.key())
                || !boundedObservationValue(it.value(), depth + 1, nodes)) return false;
        }
        return true;
    }
    return value.isBool() || value.isNull();
}

bool safeHostObservation(const QJsonObject &observation)
{
    if (observation.isEmpty()) return true;
    QStringList keys{QStringLiteral("status"), QStringLiteral("code"), QStringLiteral("result")};
    if (observation.contains(QStringLiteral("message"))) keys.append(QStringLiteral("message"));
    if (!exactKeys(observation, keys)) return false;
    const QJsonValue status = observation.value(QStringLiteral("status"));
    const QJsonValue code = observation.value(QStringLiteral("code"));
    const QJsonValue result = observation.value(QStringLiteral("result"));
    const QJsonValue message = observation.value(QStringLiteral("message"));
    if (!status.isString() || status.toString().trimmed().isEmpty()
        || status.toString().toUtf8().size() > 128
        || (observation.contains(QStringLiteral("code"))
            && (!code.isString() || code.toString().toUtf8().size() > 128))
        || (observation.contains(QStringLiteral("result")) && !result.isObject())
        || (observation.contains(QStringLiteral("message"))
            && (!message.isString() || message.toString().toUtf8().size() > 16 * 1024))) return false;
    int nodes = 0;
    if (!boundedObservationValue(QJsonValue(observation), 0, nodes)) return false;
    return QJsonDocument(observation).toJson(QJsonDocument::Compact).size() <= 64 * 1024;
}

bool validOperationToken(const QString &value)
{
    if (value.trimmed().isEmpty() || value.toUtf8().size() > 128) return false;
    for (const QChar character : value)
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) return false;
    return true;
}

bool validOperationAssociation(const RProjectCatalog::OperationAssociation &association)
{
    return (association.candidateSha256.isEmpty() || validHash(association.candidateSha256))
        && (association.approvalSummarySha256.isEmpty() || validHash(association.approvalSummarySha256))
        && (association.acceptedModelSha256.isEmpty() || validHash(association.acceptedModelSha256))
        && (association.acceptedRevision.isEmpty()
            || validOperationToken(association.acceptedRevision))
        && validDiagnosticCode(association.diagnosticCode)
        && safeHostObservation(association.observation);
}

QJsonObject operationAssociationJson(const RProjectCatalog::OperationAssociation &association)
{
    return {{QStringLiteral("candidateSha256"), association.candidateSha256},
            {QStringLiteral("approvalSummarySha256"), association.approvalSummarySha256},
            {QStringLiteral("acceptedRevision"), association.acceptedRevision},
            {QStringLiteral("acceptedModelSha256"), association.acceptedModelSha256},
            {QStringLiteral("diagnosticCode"), association.diagnosticCode},
            {QStringLiteral("observation"), association.observation}};
}

bool operationAssociationFromJson(const QJsonValue &value,
                                  RProjectCatalog::OperationAssociation &association)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("candidateSha256"), QStringLiteral("approvalSummarySha256"),
                            QStringLiteral("acceptedRevision"), QStringLiteral("acceptedModelSha256"),
                            QStringLiteral("diagnosticCode"), QStringLiteral("observation")})) return false;
    if (!object.value(QStringLiteral("candidateSha256")).isString()
        || !object.value(QStringLiteral("approvalSummarySha256")).isString()
        || !object.value(QStringLiteral("acceptedRevision")).isString()
        || !object.value(QStringLiteral("acceptedModelSha256")).isString()
        || !object.value(QStringLiteral("diagnosticCode")).isString()
        || !object.value(QStringLiteral("observation")).isObject()) return false;
    association.candidateSha256 = object.value(QStringLiteral("candidateSha256")).toString();
    association.approvalSummarySha256 = object.value(QStringLiteral("approvalSummarySha256")).toString();
    association.acceptedRevision = object.value(QStringLiteral("acceptedRevision")).toString();
    association.acceptedModelSha256 = object.value(QStringLiteral("acceptedModelSha256")).toString();
    association.diagnosticCode = object.value(QStringLiteral("diagnosticCode")).toString();
    association.observation = object.value(QStringLiteral("observation")).toObject();
    return validOperationAssociation(association);
}

void mergeOperationAssociation(RProjectCatalog::OperationAssociation &target,
                               const RProjectCatalog::OperationAssociation &update)
{
    if (!update.candidateSha256.isEmpty()) target.candidateSha256 = update.candidateSha256;
    if (!update.approvalSummarySha256.isEmpty()) target.approvalSummarySha256 = update.approvalSummarySha256;
    if (!update.acceptedRevision.isEmpty()) target.acceptedRevision = update.acceptedRevision;
    if (!update.acceptedModelSha256.isEmpty()) target.acceptedModelSha256 = update.acceptedModelSha256;
    if (!update.diagnosticCode.isEmpty()) target.diagnosticCode = update.diagnosticCode;
    if (!update.observation.isEmpty()) target.observation = update.observation;
}

QString operationArtifactStatus(const QString &status)
{
    static const QSet<QString> valid{QStringLiteral("pending"), QStringLiteral("published"),
        QStringLiteral("not_published"), QStringLiteral("unchanged_matches_intended"),
        QStringLiteral("unknown"), QStringLiteral("conflict"), QStringLiteral("unreadable")};
    return valid.contains(status) ? status : QString();
}

QJsonObject operationArtifactJson(const RProjectCatalog::OperationArtifactIntent &artifact)
{
    return {{QStringLiteral("path"), artifact.path},
            {QStringLiteral("expectedPriorExists"), artifact.expectedPriorExists},
            {QStringLiteral("expectedPriorSha256"), artifact.expectedPriorSha256},
            {QStringLiteral("intendedSha256"), artifact.intendedSha256},
            {QStringLiteral("observedStatus"), artifact.observedStatus},
            {QStringLiteral("observedExists"), artifact.observedExists},
            {QStringLiteral("observedSha256"), artifact.observedSha256}};
}

bool operationArtifactFromJson(const QJsonValue &value,
                               RProjectCatalog::OperationArtifactIntent &artifact)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("path"), QStringLiteral("expectedPriorExists"),
                            QStringLiteral("expectedPriorSha256"), QStringLiteral("intendedSha256"),
                            QStringLiteral("observedStatus"), QStringLiteral("observedExists"),
                            QStringLiteral("observedSha256")})) return false;
    if (!object.value(QStringLiteral("path")).isString()
        || !object.value(QStringLiteral("expectedPriorExists")).isBool()
        || !object.value(QStringLiteral("expectedPriorSha256")).isString()
        || !object.value(QStringLiteral("intendedSha256")).isString()
        || !object.value(QStringLiteral("observedStatus")).isString()
        || !object.value(QStringLiteral("observedExists")).isBool()
        || !object.value(QStringLiteral("observedSha256")).isString()) return false;
    artifact.path = object.value(QStringLiteral("path")).toString();
    artifact.expectedPriorExists = object.value(QStringLiteral("expectedPriorExists")).toBool();
    artifact.expectedPriorSha256 = object.value(QStringLiteral("expectedPriorSha256")).toString();
    artifact.intendedSha256 = object.value(QStringLiteral("intendedSha256")).toString();
    artifact.observedStatus = object.value(QStringLiteral("observedStatus")).toString();
    artifact.observedExists = object.value(QStringLiteral("observedExists")).toBool();
    artifact.observedSha256 = object.value(QStringLiteral("observedSha256")).toString();
    const QString priorHash = artifact.expectedPriorSha256;
    const QString intendedHash = artifact.intendedSha256;
    const QString observedHash = artifact.observedSha256;
    const QString status = artifact.observedStatus;
    return !artifact.path.isEmpty() && artifact.path.toUtf8().size() <= MaxPathBytes
        && QDir::isAbsolutePath(artifact.path)
        && QDir::cleanPath(QDir::fromNativeSeparators(artifact.path)) == artifact.path
        && (artifact.expectedPriorExists ? validHash(priorHash) : priorHash.isEmpty())
        && (intendedHash.isEmpty() || validHash(intendedHash))
        && (status.isEmpty() || operationArtifactStatus(status) == status)
        && (artifact.observedExists
                ? (validHash(observedHash) || (observedHash.isEmpty()
                    && (status == QStringLiteral("conflict") || status == QStringLiteral("unreadable"))))
                : observedHash.isEmpty());
}

QString timestampText(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

bool parseTimestamp(const QJsonValue &value, QDateTime &time)
{
    if (!value.isString()) return false;
    time = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    return time.isValid() && time.timeSpec() == Qt::UTC;
}

QJsonObject operationEventJson(RProjectCatalog::OperationPhase phase,
                                const RProjectCatalog::OperationAssociation &association,
                                const QDateTime &time)
{
    return {{QStringLiteral("phase"), operationPhaseName(phase)},
            {QStringLiteral("association"), operationAssociationJson(association)},
            {QStringLiteral("atUtc"), timestampText(time)}};
}

QJsonObject durableOperationJson(const RProjectCatalog::DurableOperation &operation,
                                 const QJsonArray &events)
{
    QJsonArray artifacts;
    for (const auto &artifact : operation.artifacts) artifacts.append(operationArtifactJson(artifact));
    return {{QStringLiteral("projectId"), operation.projectId},
            {QStringLiteral("ownerRuntimeId"), operation.ownerRuntimeId},
            {QStringLiteral("operationId"), operation.operationId},
            {QStringLiteral("idempotencyKey"), operation.idempotencyKey},
            {QStringLiteral("normalizedRequestSha256"), operation.normalizedRequestSha256},
            {QStringLiteral("phase"), operationPhaseName(operation.phase)},
            {QStringLiteral("association"), operationAssociationJson(operation.association)},
            {QStringLiteral("artifacts"), artifacts},
            {QStringLiteral("createdAtUtc"), timestampText(operation.createdAtUtc)},
            {QStringLiteral("updatedAtUtc"), timestampText(operation.updatedAtUtc)},
            {QStringLiteral("events"), events}};
}

bool durableOperationFromJson(const QJsonValue &value, const QString &expectedProjectId,
                              RProjectCatalog::DurableOperation &operation)
{
    using Phase = RProjectCatalog::OperationPhase;
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("projectId"), QStringLiteral("ownerRuntimeId"),
            QStringLiteral("operationId"),
            QStringLiteral("idempotencyKey"), QStringLiteral("normalizedRequestSha256"),
            QStringLiteral("phase"), QStringLiteral("association"), QStringLiteral("artifacts"),
            QStringLiteral("createdAtUtc"), QStringLiteral("updatedAtUtc"), QStringLiteral("events")})) return false;
    if (!object.value(QStringLiteral("projectId")).isString()
        || !object.value(QStringLiteral("ownerRuntimeId")).isString()
        || !object.value(QStringLiteral("operationId")).isString()
        || !object.value(QStringLiteral("idempotencyKey")).isString()
        || !object.value(QStringLiteral("normalizedRequestSha256")).isString()
        || !object.value(QStringLiteral("artifacts")).isArray()
        || !object.value(QStringLiteral("events")).isArray()) return false;
    operation.projectId = object.value(QStringLiteral("projectId")).toString();
    operation.ownerRuntimeId = object.value(QStringLiteral("ownerRuntimeId")).toString();
    operation.operationId = object.value(QStringLiteral("operationId")).toString();
    operation.idempotencyKey = object.value(QStringLiteral("idempotencyKey")).toString();
    operation.normalizedRequestSha256 = object.value(QStringLiteral("normalizedRequestSha256")).toString();
    if (operation.projectId != expectedProjectId || !validUuid(operation.ownerRuntimeId)
        || !validUuid(operation.operationId)
        || !validOperationToken(operation.idempotencyKey)
        || !validHash(operation.normalizedRequestSha256)
        || !operationPhase(object.value(QStringLiteral("phase")), operation.phase)
        || !operationAssociationFromJson(object.value(QStringLiteral("association")), operation.association)
        || !parseTimestamp(object.value(QStringLiteral("createdAtUtc")), operation.createdAtUtc)
        || !parseTimestamp(object.value(QStringLiteral("updatedAtUtc")), operation.updatedAtUtc)) return false;
    const QJsonArray artifactArray = object.value(QStringLiteral("artifacts")).toArray();
    if (artifactArray.size() > MaxOperationArtifacts) return false;
    QSet<QString> artifactPaths;
    for (const QJsonValue &artifactValue : artifactArray)
    {
        RProjectCatalog::OperationArtifactIntent artifact;
        if (!operationArtifactFromJson(artifactValue, artifact)) return false;
#ifdef Q_OS_WIN
        const QString key = artifact.path.toCaseFolded();
#else
        const QString key = artifact.path;
#endif
        if (artifactPaths.contains(key)) return false;
        artifactPaths.insert(key);
        operation.artifacts.append(artifact);
    }
    const QJsonArray events = object.value(QStringLiteral("events")).toArray();
    if (events.isEmpty() || events.size() > MaxOperationEvents) return false;
    QDateTime firstAt, lastAt;
    Phase priorPhase = Phase::Intent;
    RProjectCatalog::OperationAssociation lastAssociation;
    for (qsizetype index = 0; index < events.size(); ++index)
    {
        if (!events.at(index).isObject()) return false;
        const QJsonObject event = events.at(index).toObject();
        Phase eventPhase;
        RProjectCatalog::OperationAssociation eventAssociation;
        QDateTime eventAt;
        if (!exactKeys(event, {QStringLiteral("phase"), QStringLiteral("association"), QStringLiteral("atUtc")})
            || !operationPhase(event.value(QStringLiteral("phase")), eventPhase)
            || !operationAssociationFromJson(event.value(QStringLiteral("association")), eventAssociation)
            || !parseTimestamp(event.value(QStringLiteral("atUtc")), eventAt)) return false;
        if (index == 0)
        {
            if (eventPhase != Phase::Intent) return false;
            firstAt = eventAt;
        }
        else if (!validPhaseTransition(priorPhase, eventPhase) || eventAt < lastAt) return false;
        priorPhase = eventPhase;
        lastAt = eventAt;
        lastAssociation = eventAssociation;
    }
    if (priorPhase != operation.phase || firstAt != operation.createdAtUtc
        || lastAt != operation.updatedAtUtc
        || operationAssociationJson(lastAssociation) != operationAssociationJson(operation.association)) return false;
    return true;
}

bool readOperationLedger(const QString &root, const QString &projectId,
                         QJsonArray &operations, QString &error)
{
    const QFileInfo directoryInfo(QDir(root).filePath(QStringLiteral("operations")));
    if (directoryInfo.exists() && (directoryInfo.isSymLink() || !directoryInfo.isDir()))
    {
        error = QStringLiteral("Managed operations path is a symbolic link or non-directory.");
        return false;
    }
    const QString path = QDir(root).filePath(QStringLiteral("operations/ledger.json"));
    const QFileInfo info(path);
    if (!info.exists())
    {
        if (info.isSymLink())
        {
            error = QStringLiteral("Operation ledger path is a symbolic link.");
            return false;
        }
        operations = {};
        return true;
    }
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > MaxOperationLedgerBytes)
    {
        error = QStringLiteral("Existing operation ledger is not a valid bounded regular file.");
        return false;
    }
    QJsonObject envelope;
    if (!readJsonObject(path, MaxOperationLedgerBytes, envelope, error)) return false;
    if (!exactKeys(envelope, {QStringLiteral("ledgerVersion"), QStringLiteral("projectId"),
                              QStringLiteral("operations")})
        || envelope.value(QStringLiteral("ledgerVersion")).toInt(-1) != 1
        || envelope.value(QStringLiteral("projectId")).toString() != projectId
        || !envelope.value(QStringLiteral("operations")).isArray())
    {
        error = QStringLiteral("Operation ledger has an unsupported or mismatched envelope.");
        return false;
    }
    operations = envelope.value(QStringLiteral("operations")).toArray();
    if (operations.size() > MaxDurableOperations)
    {
        error = QStringLiteral("Operation ledger exceeds its record bound.");
        return false;
    }
    QSet<QString> ids, keys;
    for (const QJsonValue &value : operations)
    {
        RProjectCatalog::DurableOperation operation;
        if (!durableOperationFromJson(value, projectId, operation))
        {
            error = QStringLiteral("Operation ledger contains an invalid or unsupported record.");
            return false;
        }
        if (ids.contains(operation.operationId) || keys.contains(operation.idempotencyKey))
        {
            error = QStringLiteral("Operation ledger contains duplicate identity or idempotency keys.");
            return false;
        }
        ids.insert(operation.operationId);
        keys.insert(operation.idempotencyKey);
    }
    return true;
}

bool writeAtomicManagedJson(const QString &root, const QString &relativePath,
                            const QJsonObject &object, qint64 maxBytes, QString &error)
{
    QString path;
    if (!ensureManagedFilePath(root, relativePath, path, error)) return false;
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > maxBytes)
    {
        error = QStringLiteral("Managed durable state reached its byte limit.");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        error = QStringLiteral("Could not atomically publish managed durable state: %1").arg(file.errorString());
        return false;
    }
    return true;
}

bool writeOperationLedger(const QString &root, const QString &projectId,
                          const QJsonArray &operations, QString &error)
{
    const QJsonObject envelope{{QStringLiteral("ledgerVersion"), 1},
                               {QStringLiteral("projectId"), projectId},
                               {QStringLiteral("operations"), operations}};
    return writeAtomicManagedJson(root, QStringLiteral("operations/ledger.json"),
                                  envelope, MaxOperationLedgerBytes, error);
}

bool requestDigest(const QJsonObject &normalizedRequest, QString &digest, QString &error)
{
    const QByteArray bytes = QJsonDocument(normalizedRequest).toJson(QJsonDocument::Compact);
    if (normalizedRequest.isEmpty() || bytes.isEmpty() || bytes.size() > 64 * 1024)
    {
        error = QStringLiteral("Normalized operation request must be a non-empty object within 64 KiB.");
        return false;
    }
    digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return true;
}

RProjectCatalog::DurableOperation operationFromJsonUnchecked(const QJsonObject &object)
{
    RProjectCatalog::DurableOperation operation;
    durableOperationFromJson(object, object.value(QStringLiteral("projectId")).toString(), operation);
    return operation;
}

int findOperationIndex(const QJsonArray &operations, const QString &field, const QString &value)
{
    for (qsizetype index = 0; index < operations.size(); ++index)
        if (operations.at(index).toObject().value(field).toString() == value) return int(index);
    return -1;
}

QJsonArray operationEvents(const QJsonObject &operation)
{
    return operation.value(QStringLiteral("events")).toArray();
}

void appendOperationEvent(QJsonObject &operation, RProjectCatalog::OperationPhase phase,
                          const RProjectCatalog::OperationAssociation &association,
                          const QDateTime &time)
{
    QJsonArray events = operationEvents(operation);
    events.append(operationEventJson(phase, association, time));
    operation.insert(QStringLiteral("events"), events);
    operation.insert(QStringLiteral("phase"), operationPhaseName(phase));
    operation.insert(QStringLiteral("association"), operationAssociationJson(association));
    operation.insert(QStringLiteral("updatedAtUtc"), timestampText(time));
}

bool validCloseRetryId(const QString &value)
{
    return validUuid(value);
}

QJsonArray readCloseRetryRecords(const QString &root, const QString &projectId,
                                 QString &error, bool *valid)
{
    if (valid) *valid = false;
    const QFileInfo directoryInfo(QDir(root).filePath(QStringLiteral("operations")));
    if (directoryInfo.exists() && (directoryInfo.isSymLink() || !directoryInfo.isDir()))
    {
        error = QStringLiteral("Managed operations path is a symbolic link or non-directory.");
        return {};
    }
    const QString path = QDir(root).filePath(QStringLiteral("operations/close-retries.json"));
    const QFileInfo info(path);
    if (!info.exists())
    {
        if (info.isSymLink()) error = QStringLiteral("Close retry queue path is a symbolic link.");
        else if (valid) *valid = true;
        return {};
    }
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > 8 * 1024 * 1024)
    {
        error = QStringLiteral("Existing close retry queue is not a valid bounded regular file.");
        return {};
    }
    QJsonObject envelope;
    if (!readJsonObject(path, 8 * 1024 * 1024, envelope, error)) return {};
    if (!exactKeys(envelope, {QStringLiteral("queueVersion"), QStringLiteral("projectId"),
                              QStringLiteral("records")})
        || envelope.value(QStringLiteral("queueVersion")).toInt(-1) != 1
        || envelope.value(QStringLiteral("projectId")).toString() != projectId
        || !envelope.value(QStringLiteral("records")).isArray())
    {
        error = QStringLiteral("Close retry queue has an unsupported or mismatched envelope.");
        return {};
    }
    const QJsonArray records = envelope.value(QStringLiteral("records")).toArray();
    if (records.size() > MaxCloseRetries)
    {
        error = QStringLiteral("Close retry queue exceeds its record bound.");
        return {};
    }
    QSet<QString> retryIds;
    for (const auto &value : records)
    {
        if (!value.isObject())
        {
            error = QStringLiteral("Close retry queue contains a non-record entry.");
            return {};
        }
        const auto record = value.toObject();
        QDateTime created, completed;
        const bool isComplete = record.value(QStringLiteral("status")).toString() == QStringLiteral("complete");
        const QJsonObject payload = record.value(QStringLiteral("payload")).toObject();
        const QByteArray payloadBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        QString payloadError;
        if (!exactKeys(record, {QStringLiteral("retryId"), QStringLiteral("operationId"),
                QStringLiteral("diagnosticCode"), QStringLiteral("createdAtUtc"),
                QStringLiteral("status"), QStringLiteral("completedAtUtc"),
                QStringLiteral("payload"), QStringLiteral("payloadSha256")})
            || !record.value(QStringLiteral("retryId")).isString()
            || !record.value(QStringLiteral("operationId")).isString()
            || !record.value(QStringLiteral("diagnosticCode")).isString()
            || !record.value(QStringLiteral("payload")).isObject()
            || !record.value(QStringLiteral("payloadSha256")).isString()
            || !validCloseRetryId(record.value(QStringLiteral("retryId")).toString())
            || !validUuid(record.value(QStringLiteral("operationId")).toString())
            || !validDiagnosticCode(record.value(QStringLiteral("diagnosticCode")).toString())
            || payloadBytes.size() > 64 * 1024
            || !RModelCommandSession::validateCloseRetryPayload(payload, &payloadError)
            || payload.value(QStringLiteral("retryId")).toString()
                != record.value(QStringLiteral("retryId")).toString()
            || payload.value(QStringLiteral("operationId")).toString()
                != record.value(QStringLiteral("operationId")).toString()
            || QString::fromLatin1(QCryptographicHash::hash(payloadBytes, QCryptographicHash::Sha256).toHex())
                != record.value(QStringLiteral("payloadSha256")).toString()
            || !parseTimestamp(record.value(QStringLiteral("createdAtUtc")), created)
            || (!isComplete && record.value(QStringLiteral("status")).toString() != QStringLiteral("pending"))
            || (isComplete ? !parseTimestamp(record.value(QStringLiteral("completedAtUtc")), completed)
                           : !record.value(QStringLiteral("completedAtUtc")).isString()
                                 || !record.value(QStringLiteral("completedAtUtc")).toString().isEmpty()))
        {
            error = QStringLiteral("Close retry queue contains an invalid record.");
            return {};
        }
        const QString id = record.value(QStringLiteral("retryId")).toString();
        if (retryIds.contains(id))
        {
            error = QStringLiteral("Close retry queue contains duplicate retry IDs.");
            return {};
        }
        retryIds.insert(id);
    }
    if (valid) *valid = true;
    return records;
}

bool writeCloseRetryRecords(const QString &root, const QString &projectId,
                            const QJsonArray &records, QString &error)
{
    const QJsonObject envelope{{QStringLiteral("queueVersion"), 1},
                               {QStringLiteral("projectId"), projectId},
                               {QStringLiteral("records"), records}};
    return writeAtomicManagedJson(root, QStringLiteral("operations/close-retries.json"),
                                  envelope, 8 * 1024 * 1024, error);
}

}

std::optional<RProjectCatalog::SourceBinding> RProjectCatalog::resolveSourceBinding(
    const QString &sourcePath, QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString canonicalPath, localError;
    if (!canonicalExistingFile(sourcePath, canonicalPath, localError))
    {
        setError(error, QStringLiteral("Native source path is invalid or unavailable."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return std::nullopt;
    }
    SourceBinding binding;
    binding.logicalSourcePath = canonicalPath;
    QString currentPath = canonicalPath;
    QSet<QString> visited;
    for (int linkDepth = 0; linkDepth <= MaxNativeLinkDepth; ++linkDepth)
    {
        const QString pathKey = pathIndexKey(currentPath);
        if (visited.contains(pathKey))
        {
            setError(error, QStringLiteral("Native source LINK chain contains a cycle."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return std::nullopt;
        }
        visited.insert(pathKey);
        RFileHeader header;
        qint64 size = 0;
        if (!readNativeHeader(currentPath, header, size, localError))
        {
            setError(error, QStringLiteral("Native source LINK chain or terminal model header is invalid."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return std::nullopt;
        }
        if (header.getType() == R_FILE_TYPE_LINK && size > MaxNativeLinkBytes)
        {
            setError(error, QStringLiteral("Native LINK artifact exceeds the v1 size limit."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return std::nullopt;
        }
        SourceArtifact artifact;
        artifact.path = currentPath;
        if (!fileHash(currentPath, artifact.sha256, localError))
        {
            setError(error, QStringLiteral("Could not hash native source artifact."));
            setErrorCode(errorCode, QStringLiteral("io_error"));
            return std::nullopt;
        }
        binding.sourceArtifacts.append(artifact);
        if (header.getType() == R_FILE_TYPE_MODEL)
        {
            binding.resolvedSourcePath = currentPath;
            binding.savedArtifactHash = artifact.sha256;
            return binding;
        }
        if (linkDepth == MaxNativeLinkDepth)
        {
            setError(error, QStringLiteral("Native source LINK chain exceeds the v1 depth limit."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return std::nullopt;
        }
        const QString relativeTarget = header.getInformation();
        if (relativeTarget.isEmpty() || relativeTarget.toUtf8().size() > MaxPathBytes
            || relativeTarget.contains(QChar(0)) || relativeTarget.contains(QLatin1Char('"')))
        {
            setError(error, QStringLiteral("Native source LINK target path is empty or out of bounds."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return std::nullopt;
        }
        for (const QChar character : relativeTarget)
            if (character.unicode() < 0x20 || character.unicode() == 0x7f)
            {
                setError(error, QStringLiteral("Native source LINK target contains unsupported control characters."));
                setErrorCode(errorCode, QStringLiteral("invalid_argument"));
                return std::nullopt;
            }
        const QString targetPath = RFileUtils::findLinkTargetFileName(currentPath, relativeTarget);
        if (!canonicalExistingFile(targetPath, currentPath, localError))
        {
            setError(error, QStringLiteral("Native source LINK target is unavailable or invalid."));
            setErrorCode(errorCode, QStringLiteral("source_conflict"));
            return std::nullopt;
        }
    }
    setError(error, QStringLiteral("Native source LINK chain could not be resolved."));
    setErrorCode(errorCode, QStringLiteral("invalid_argument"));
    return std::nullopt;
}

QJsonObject RProjectCatalog::SourceArtifact::toJson() const
{
    return {{QStringLiteral("path"), path}, {QStringLiteral("sha256"), sha256}};
}

bool RProjectCatalog::SourceBinding::operator==(const SourceBinding &other) const
{
    return sameSourceBinding(*this, other);
}

QJsonObject RProjectCatalog::InstanceRef::toJson() const
{
    return {{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), sessionEpoch},
            {QStringLiteral("documentRevision"), double(documentRevision)}};
}

QJsonObject RProjectCatalog::SnapshotArtifact::toJson() const
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("projectId"), projectId},
            {QStringLiteral("persistentDocumentId"), persistentDocumentId},
            {QStringLiteral("sourceRevision"), sourceRevision},
            {QStringLiteral("modelSha256"), modelSha256},
            {QStringLiteral("relativePath"), relativePath},
            {QStringLiteral("instanceRef"), instanceRef.toJson()}};
}

RProjectCatalog::RProjectCatalog(QString canonicalWorkspace, QString projectId,
                                 QString runtimeId, std::unique_ptr<QLockFile> runtimeLock)
    : root(std::move(canonicalWorkspace)), projectIdentity(std::move(projectId)),
      runtimeIdentity(std::move(runtimeId)), runtimeLease(std::move(runtimeLock)),
      ownerThread(QThread::currentThreadId())
{
}

RProjectCatalog::PublicationLease::PublicationLease(
    QString projectId, QString workspace, QString lockFilePath, QString documentId,
    QString revision, QStringList reserved, Qt::HANDLE owner, std::unique_ptr<QLockFile> lock,
    DocumentInstanceLeasePtr heldInstanceLease)
    : projectIdentity(std::move(projectId)), workspaceRoot(std::move(workspace)),
      lockPath(std::move(lockFilePath)), documentIdentity(std::move(documentId)),
      sourceRevision(std::move(revision)), paths(std::move(reserved)), ownerThread(owner),
      lockFile(std::move(lock)), instanceLease(std::move(heldInstanceLease))
{
}

RProjectCatalog::PublicationLease::~PublicationLease() = default;
QString RProjectCatalog::PublicationLease::persistentDocumentId() const { return documentIdentity; }
QString RProjectCatalog::PublicationLease::expectedSourceRevision() const { return sourceRevision; }
QStringList RProjectCatalog::PublicationLease::reservedPaths() const { return paths; }

RProjectCatalog::DocumentInstanceLease::DocumentInstanceLease(
    QString projectId, QString workspace, QString instanceLockPath, QString documentId,
    InstanceRef instanceRef, Qt::HANDLE owner, std::unique_ptr<QLockFile> lock)
    : projectIdentity(std::move(projectId)), workspaceRoot(std::move(workspace)),
      lockPath(std::move(instanceLockPath)), documentIdentity(std::move(documentId)),
      runtimeInstance(std::move(instanceRef)), ownerThread(owner), lockFile(std::move(lock))
{
}

RProjectCatalog::DocumentInstanceLease::~DocumentInstanceLease() = default;
QString RProjectCatalog::DocumentInstanceLease::persistentDocumentId() const { return documentIdentity; }
QString RProjectCatalog::DocumentInstanceLease::runtimeDocumentId() const { return runtimeInstance.documentId; }
QString RProjectCatalog::DocumentInstanceLease::sessionEpoch() const { return runtimeInstance.sessionEpoch; }

std::unique_ptr<RProjectCatalog> RProjectCatalog::open(const QString &workspace, QString *error)
{
    if (error) error->clear();
    QString localError;
    if (workspace.trimmed().isEmpty() || workspace.toUtf8().size() > MaxPathBytes)
    {
        setError(error, QStringLiteral("An explicit bounded project workspace path is required."));
        return {};
    }
    const QString absolute = QDir::cleanPath(QFileInfo(workspace).absoluteFilePath());
    if (!QDir().mkpath(absolute))
    {
        setError(error, QStringLiteral("Could not create project workspace directory."));
        return {};
    }
    const QFileInfo workspaceInfo(absolute);
    if (!workspaceInfo.isDir() || workspaceInfo.canonicalFilePath().isEmpty())
    {
        setError(error, QStringLiteral("Project workspace must resolve to a directory."));
        return {};
    }
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(workspaceInfo.canonicalFilePath()));
    const auto createCatalog = [&](const QString &projectId) -> std::unique_ptr<RProjectCatalog> {
        const QString runtimeId = newId();
        std::unique_ptr<QLockFile> lease;
        if (!acquireRuntimeLease(root, runtimeId, lease, localError))
        {
            setError(error, localError);
            return {};
        }
        return std::unique_ptr<RProjectCatalog>(new RProjectCatalog(
            root, projectId, runtimeId, std::move(lease)));
    };
    const QString manifestPath = QDir(root).filePath(QStringLiteral("project.json"));
    if (QFileInfo(manifestPath).exists())
    {
        QJsonObject manifest;
        if (!readManifest(root, manifest, localError))
        {
            setError(error, localError);
            return {};
        }
        return createCatalog(manifest.value(QStringLiteral("projectId")).toString());
    }

    auto lock = acquireLock(root, localError);
    if (!lock)
    {
        setError(error, localError);
        return {};
    }
    if (QFileInfo(manifestPath).exists())
    {
        QJsonObject manifest;
        if (!readManifest(root, manifest, localError))
        {
            setError(error, localError);
            return {};
        }
        return createCatalog(manifest.value(QStringLiteral("projectId")).toString());
    }
    QString projectId;
    const QString workflowState = QDir(root).filePath(QStringLiteral("workflow-state.json"));
    if (!validWorkflowState(workflowState, projectId)) projectId = newId();
    QJsonObject manifest = emptyManifest(projectId);
    if (!writeManifest(root, manifest, localError))
    {
        setError(error, localError);
        return {};
    }
    return createCatalog(projectId);
}

QString RProjectCatalog::workspace() const { return root; }
QString RProjectCatalog::projectId() const { return projectIdentity; }
QString RProjectCatalog::runtimeId() const { return runtimeIdentity; }

bool RProjectCatalog::validateInstanceLease(const QString &persistentDocumentId,
                                            const InstanceRef *instanceRef,
                                            const DocumentInstanceLeasePtr &lease,
                                            QString *error, QString *errorCode) const
{
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Document instance validation must run on the catalog owner thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    QString instanceError;
    if (instanceRef && !validInstanceRef(*instanceRef, instanceError))
    {
        setError(error, instanceError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    const QString expectedLockPath = QDir(root).filePath(
        QStringLiteral("models/%1/.instance.lock").arg(persistentDocumentId));
    if (lease)
    {
        if (!validUuid(persistentDocumentId) || !lease->lockFile || !lease->lockFile->isLocked()
            || lease->projectIdentity != projectIdentity || lease->workspaceRoot != root
            || lease->documentIdentity != persistentDocumentId || lease->ownerThread != ownerThread
            || QThread::currentThreadId() != lease->ownerThread || lease->lockPath != expectedLockPath)
        {
            setError(error, QStringLiteral("Document instance lease does not belong to this workspace, document, or owner thread."));
            setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
            return false;
        }
        if (instanceRef && (instanceRef->documentId != lease->runtimeInstance.documentId
                            || instanceRef->sessionEpoch != lease->runtimeInstance.sessionEpoch))
        {
            setError(error, QStringLiteral("Runtime documentId/sessionEpoch does not match its active instance lease."));
            setErrorCode(errorCode, QStringLiteral("instance_mismatch"));
            return false;
        }
    }
    const QString modelsPath = QDir(root).filePath(QStringLiteral("models"));
    const QFileInfo modelsInfo(modelsPath);
    if (!modelsInfo.exists())
    {
        if (!lease) return true;
        setError(error, QStringLiteral("Document instance lease directory disappeared."));
        setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
        return false;
    }
    if (modelsInfo.isSymLink() || !modelsInfo.isDir())
    {
        setError(error, QStringLiteral("Document instance lock parent is not a safe managed directory."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    const QString documentDirectory = QDir(modelsPath).filePath(persistentDocumentId);
    const QFileInfo documentDirectoryInfo(documentDirectory);
    if (!documentDirectoryInfo.exists())
    {
        if (!lease) return true;
        setError(error, QStringLiteral("Document instance lease directory disappeared."));
        setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
        return false;
    }
    if (documentDirectoryInfo.isSymLink() || !documentDirectoryInfo.isDir())
    {
        setError(error, QStringLiteral("Document instance lock directory is not a safe managed directory."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (QFileInfo(expectedLockPath).isSymLink())
    {
        setError(error, QStringLiteral("Document instance lock path is a symbolic link."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    auto probe = std::make_unique<QLockFile>(expectedLockPath);
    probe->setStaleLockTime(30000);
    if (probe->tryLock(0))
    {
        probe->unlock();
        if (lease)
        {
            setError(error, QStringLiteral("Document instance lease is no longer active."));
            setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
            return false;
        }
        return true;
    }
    if (!lease)
    {
        setError(error, QStringLiteral("Persistent document is active in another runtime instance."));
        setErrorCode(errorCode, QStringLiteral("document_in_use"));
        return false;
    }
    return true;
}

RProjectCatalog::DocumentInstanceLeasePtr RProjectCatalog::acquireDocumentInstance(
    const QString &persistentDocumentId, const InstanceRef &instanceRef,
    QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Document instances must be acquired on the catalog owner thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    if (!validUuid(persistentDocumentId) || !validInstanceRef(instanceRef, localError))
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Persistent document ID is invalid.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    auto projectLock = acquireLock(root, localError, errorCode);
    if (!projectLock)
    {
        setError(error, localError);
        return {};
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return {};
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError) || document.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Cannot open a tombstoned project document.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    QString documentDirectory;
    const QString relativeDirectory = QStringLiteral("models/%1").arg(persistentDocumentId);
    if (!prepareManagedDirectory(root, relativeDirectory, documentDirectory, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    const QString lockPath = QDir(documentDirectory).filePath(QStringLiteral(".instance.lock"));
    if (QFileInfo(lockPath).isSymLink())
    {
        setError(error, QStringLiteral("Document instance lock path is a symbolic link."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    auto instanceLock = std::make_unique<QLockFile>(lockPath);
    instanceLock->setStaleLockTime(30000);
    if (!instanceLock->tryLock(0))
    {
        setError(error, QStringLiteral("Persistent document is active in another runtime instance."));
        setErrorCode(errorCode, QStringLiteral("document_in_use"));
        return {};
    }
    return DocumentInstanceLeasePtr(new DocumentInstanceLease(projectIdentity, root, lockPath,
                                                                persistentDocumentId, instanceRef,
                                                                ownerThread, std::move(instanceLock)));
}

QJsonObject RProjectCatalog::query(QString *error) const
{
    if (error) error->clear();
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog reads must run on their owning thread."));
        return {};
    }
    QJsonObject manifest;
    QString localError;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        return {};
    }
    QJsonArray documents;
    const QJsonObject records = manifest.value(QStringLiteral("documents")).toObject();
    for (auto it = records.begin(); it != records.end(); ++it) documents.append(it.value());
    return {{QStringLiteral("protocolVersion"), QStringLiteral("range.project/v1")},
            {QStringLiteral("storageVersion"), StorageVersion},
            {QStringLiteral("projectId"), projectIdentity},
            {QStringLiteral("documents"), documents},
            {QStringLiteral("sourcePathIndex"), manifest.value(QStringLiteral("sourcePathIndex"))},
            {QStringLiteral("capability"), capabilityDescription()}};
}

std::optional<RProjectCatalog::Document> RProjectCatalog::document(
    const QString &persistentDocumentId, QString *error) const
{
    if (error) error->clear();
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog reads must run on their owning thread."));
        return std::nullopt;
    }
    QJsonObject manifest;
    QString localError;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        return std::nullopt;
    }
    RProjectCatalog::Document result;
    if (!findDocument(manifest, persistentDocumentId, result, localError))
    {
        if (!manifest.value(QStringLiteral("documents")).toObject().contains(persistentDocumentId))
            return std::nullopt;
        setError(error, localError);
        return std::nullopt;
    }
    return result;
}

QString RProjectCatalog::registerDocument(const QString &sourcePath, const QString &label,
                                          const QString &originDocumentId, QString *error,
                                          QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    QString localError, sourceErrorCode;
    const auto binding = resolveSourceBinding(sourcePath, &localError, &sourceErrorCode);
    if (!binding)
    {
        auto lock = acquireLock(root, localError, errorCode);
        if (!lock)
        {
            setError(error, localError);
            return {};
        }
        QJsonObject manifest;
        if (!loadCurrent(root, projectIdentity, manifest, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
            return {};
        }
        QString canonicalInput;
        if (!canonicalExistingFile(sourcePath, canonicalInput, localError))
            canonicalInput = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(sourcePath).absoluteFilePath()));
        const bool previouslyOwned = !canonicalInput.isEmpty()
            && manifest.value(QStringLiteral("sourcePathIndex")).toObject()
                   .contains(pathIndexKey(canonicalInput));
        setError(error, previouslyOwned
                            ? QStringLiteral("Registered native source bytes or LINK target became invalid; explicit reload is required.")
                            : QStringLiteral("Native source binding is invalid or unavailable."));
        setErrorCode(errorCode, previouslyOwned ? QStringLiteral("source_conflict")
                                                : (sourceErrorCode.isEmpty()
                                                       ? QStringLiteral("invalid_argument") : sourceErrorCode));
        return {};
    }
    return registerDocument(*binding, label, originDocumentId, error, errorCode);
}

QString RProjectCatalog::registerDocument(const SourceBinding &sourceBinding, const QString &label,
                                          const QString &originDocumentId, QString *error,
                                          QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    if (!validLabel(label)
        || (!originDocumentId.isEmpty() && !validUuid(originDocumentId)))
    {
        setError(error, QStringLiteral("Registration label or optional origin ID violates v1 bounds."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return {};
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return {};
    }
    const auto currentBinding = resolveSourceBinding(sourceBinding.logicalSourcePath, &localError, errorCode);
    if (!currentBinding || !sameSourceBinding(*currentBinding, sourceBinding))
    {
        setError(error, QStringLiteral("Native source LINK chain changed after the caller read its binding."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return {};
    }
    for (const auto &artifact : sourceBinding.sourceArtifacts)
        if (managedCatalogPath(root, artifact.path))
        {
            setError(error, QStringLiteral("Native source cannot be registered from a catalog-managed path."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return {};
        }
    QJsonObject documents = manifest.value(QStringLiteral("documents")).toObject();
    if (!originDocumentId.isEmpty())
    {
        Document origin;
        if (!findDocument(manifest, originDocumentId, origin, localError) || origin.tombstone)
        {
            setError(error, localError.isEmpty() ? QStringLiteral("Origin document is tombstoned.") : localError);
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return {};
        }
    }
    const QJsonObject pathIndex = manifest.value(QStringLiteral("sourcePathIndex")).toObject();
    QSet<QString> owners;
    for (const auto &artifact : sourceBinding.sourceArtifacts)
    {
        const QString ownerId = pathIndex.value(pathIndexKey(artifact.path)).toString();
        if (!ownerId.isEmpty()) owners.insert(ownerId);
    }
    if (owners.size() > 1)
    {
        setError(error, QStringLiteral("Native source LINK chain aliases multiple persistent documents."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return {};
    }
    if (!owners.isEmpty())
    {
        const QString existingId = *owners.cbegin();
        Document existing;
        if (!findDocument(manifest, existingId, existing, localError) || existing.tombstone)
        {
            setError(error, QStringLiteral("Source-path index refers to an invalid or tombstoned document."));
            setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
            return {};
        }
        if (existing.sourcePath != sourceBinding.logicalSourcePath
            && !currentSourceMatches(existing, error, errorCode)) return {};
        bool matchingSharedArtifacts = sourceBinding.resolvedSourcePath == existing.resolvedSourcePath
            && sourceBinding.savedArtifactHash == existing.savedArtifactHash;
        for (const auto &currentArtifact : sourceBinding.sourceArtifacts)
        {
            const auto old = std::find_if(existing.sourceArtifacts.cbegin(), existing.sourceArtifacts.cend(),
                                          [&currentArtifact](const SourceArtifact &candidate) {
                                              return candidate.path == currentArtifact.path;
                                          });
            if (old != existing.sourceArtifacts.cend() && old->sha256 != currentArtifact.sha256)
                matchingSharedArtifacts = false;
        }
        if (existing.sourcePath == sourceBinding.logicalSourcePath
            && !sameSourceBinding(SourceBinding{existing.sourcePath, existing.resolvedSourcePath,
                                                existing.sourceArtifacts, existing.savedArtifactHash}, sourceBinding))
            matchingSharedArtifacts = false;
        if (!matchingSharedArtifacts)
        {
            setError(error, QStringLiteral("Registered native source bytes or LINK targets changed; explicit reload is required."));
            setErrorCode(errorCode, QStringLiteral("source_conflict"));
            return {};
        }
        // Labels are display metadata. Opening a known source never renames an
        // active document as a side effect; use renameDocument explicitly.
        return existingId;
    }
    if (documents.size() >= MaxDocuments)
    {
        setError(error, QStringLiteral("Project reached the v1 document limit."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    Document added;
    added.persistentDocumentId = newDocumentId(documents);
    added.label = label;
    applySourceBinding(added, sourceBinding);
    added.originDocumentId = originDocumentId;
    added.sourceRevision = QStringLiteral("r0");
    storeDocument(manifest, added);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return {};
    }
    return added.persistentDocumentId;
}
QString RProjectCatalog::createDocument(const QString &label, const QString &originDocumentId,
                                        QString *error)
{
    if (error) error->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        return {};
    }
    if (!validLabel(label)
        || (!originDocumentId.isEmpty() && !validUuid(originDocumentId)))
    {
        setError(error, QStringLiteral("Unsaved document requires a valid label and optional canonical origin ID."));
        return {};
    }
    auto lock = acquireLock(root, localError);
    if (!lock)
    {
        setError(error, localError);
        return {};
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        return {};
    }
    QJsonObject documents = manifest.value(QStringLiteral("documents")).toObject();
    if (documents.size() >= MaxDocuments)
    {
        setError(error, QStringLiteral("Project reached the v1 document limit."));
        return {};
    }
    if (!originDocumentId.isEmpty())
    {
        Document origin;
        if (!findDocument(manifest, originDocumentId, origin, localError) || origin.tombstone)
        {
            setError(error, localError.isEmpty() ? QStringLiteral("Origin document is tombstoned.") : localError);
            return {};
        }
    }
    Document document;
    document.persistentDocumentId = newDocumentId(documents);
    document.label = label;
    document.originDocumentId = originDocumentId;
    document.sourceRevision = QStringLiteral("r0");
    document.dirty = true;
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        return {};
    }
    return document.persistentDocumentId;
}

QString RProjectCatalog::createCopy(const QString &originDocumentId, const QString &label,
                                    QString *error)
{
    if (error) error->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        return {};
    }
    if (!validLabel(label) || !validUuid(originDocumentId))
    {
        setError(error, QStringLiteral("Copy requires a valid label and canonical origin document ID."));
        return {};
    }
    auto lock = acquireLock(root, localError);
    if (!lock)
    {
        setError(error, localError);
        return {};
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        return {};
    }
    Document origin;
    if (!findDocument(manifest, originDocumentId, origin, localError) || origin.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Origin document is tombstoned.") : localError);
        return {};
    }
    QJsonObject documents = manifest.value(QStringLiteral("documents")).toObject();
    if (documents.size() >= MaxDocuments)
    {
        setError(error, QStringLiteral("Project reached the v1 document limit."));
        return {};
    }
    Document copy;
    copy.persistentDocumentId = newDocumentId(documents);
    copy.label = label;
    copy.originDocumentId = originDocumentId;
    copy.sourceRevision = QStringLiteral("r0");
    copy.dirty = true;
    storeDocument(manifest, copy);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        return {};
    }
    return copy.persistentDocumentId;
}

bool RProjectCatalog::recordAccepted(const QString &persistentDocumentId,
                                    const QString &sourceRevision, bool dirty,
                                    const QJsonObject &draftMetadata, QString *error,
                                    const QString &expectedSourceRevision, QString *errorCode,
                                    const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!validRevision(sourceRevision)
        || (!expectedSourceRevision.isEmpty() && !validRevision(expectedSourceRevision))
        || !validDraftMetadata(draftMetadata))
    {
        setError(error, QStringLiteral("Accepted revision or draft metadata violates v1 bounds."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return false;
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return false;
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError) || document.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Cannot accept a tombstoned document.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!expectedSourceRevision.isEmpty() && document.sourceRevision != expectedSourceRevision)
    {
        setError(error, QStringLiteral("Persistent sourceRevision advanced since this runtime accepted its local version."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    if (!validateInstanceLease(persistentDocumentId, nullptr, instanceLease, error, errorCode)) return false;
    if (!currentSourceMatches(document, error, errorCode)) return false;
    if (revisionNumber(sourceRevision) < revisionNumber(document.sourceRevision))
    {
        setError(error, QStringLiteral("Accepted sourceRevision must be monotonic for a persistent document."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    if (revisionNumber(sourceRevision) > revisionNumber(document.sourceRevision))
    {
        if (!archiveCurrentSnapshot(document, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("metadata_limit"));
            return false;
        }
    }
    document.sourceRevision = sourceRevision;
    document.dirty = dirty;
    document.draftMetadata = draftMetadata;
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

bool RProjectCatalog::renameDocument(const QString &persistentDocumentId, const QString &label,
                                     QString *error, const QString &expectedSourceRevision,
                                     QString *errorCode,
                                     const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!validLabel(label)
        || (!expectedSourceRevision.isEmpty() && !validRevision(expectedSourceRevision)))
    {
        setError(error, QStringLiteral("Document label is empty, contains controls, or exceeds 256 UTF-8 bytes."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return false;
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return false;
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!expectedSourceRevision.isEmpty() && document.sourceRevision != expectedSourceRevision)
    {
        setError(error, QStringLiteral("Persistent sourceRevision advanced before rename."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    if (!validateInstanceLease(persistentDocumentId, nullptr, instanceLease, error, errorCode)) return false;
    document.label = label;
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

bool RProjectCatalog::acceptReload(const QString &persistentDocumentId, const QString &sourcePath,
                                   const QString &newSourceRevision, QString *error,
                                   const QString &expectedSourceRevision, QString *errorCode,
                                   bool *reopenedSavedSource,
                                   const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    if (reopenedSavedSource) *reopenedSavedSource = false;
    QString sourceErrorCode;
    const auto binding = resolveSourceBinding(sourcePath, error, &sourceErrorCode);
    if (!binding)
    {
        setErrorCode(errorCode, sourceErrorCode);
        return false;
    }
    return acceptReload(*binding, persistentDocumentId, newSourceRevision, error,
                        expectedSourceRevision, errorCode, reopenedSavedSource, instanceLease);
}

bool RProjectCatalog::acceptReload(const SourceBinding &sourceBinding,
                                   const QString &persistentDocumentId,
                                   const QString &newSourceRevision, QString *error,
                                   const QString &expectedSourceRevision, QString *errorCode,
                                   bool *reopenedSavedSource,
                                   const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    if (reopenedSavedSource) *reopenedSavedSource = false;
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!validRevision(newSourceRevision)
        || (!expectedSourceRevision.isEmpty() && !validRevision(expectedSourceRevision)))
    {
        setError(error, QStringLiteral("Reload sourceRevision must be canonical rN within v1 bounds."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return false;
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return false;
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError) || document.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Cannot reload a tombstoned document.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!expectedSourceRevision.isEmpty() && document.sourceRevision != expectedSourceRevision)
    {
        setError(error, QStringLiteral("Persistent sourceRevision advanced since this runtime loaded its local version."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    if (!validateInstanceLease(persistentDocumentId, nullptr, instanceLease, error, errorCode)) return false;
    QString resolveError, resolveCode;
    const auto currentBinding = resolveSourceBinding(sourceBinding.logicalSourcePath, &resolveError, &resolveCode);
    if (!currentBinding || !sameSourceBinding(*currentBinding, sourceBinding))
    {
        setError(error, QStringLiteral("Native source LINK chain changed after the caller read its binding."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return false;
    }
    for (const auto &artifact : sourceBinding.sourceArtifacts)
        if (managedCatalogPath(root, artifact.path))
        {
            setError(error, QStringLiteral("Reload source cannot use a catalog-managed path."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return false;
        }
    const qulonglong beforeRevision = revisionNumber(document.sourceRevision);
    const qulonglong afterRevision = revisionNumber(newSourceRevision);
    const bool reopenedSaved = document.dirty
        && sourceBinding.savedArtifactHash == document.savedArtifactHash;
    if (afterRevision < beforeRevision || (reopenedSaved && afterRevision == beforeRevision))
    {
        setError(error, QStringLiteral("Reload sourceRevision must advance the persisted local version."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    if (sourceBinding.savedArtifactHash != document.savedArtifactHash
        && afterRevision == beforeRevision)
    {
        setError(error, QStringLiteral("Changed source bytes require an increased monotonic sourceRevision."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return false;
    }
    const QJsonObject sourceIndex = manifest.value(QStringLiteral("sourcePathIndex")).toObject();
    for (const auto &artifact : sourceBinding.sourceArtifacts)
    {
        const QString collision = sourceIndex.value(pathIndexKey(artifact.path)).toString();
        if (!collision.isEmpty() && collision != persistentDocumentId)
        {
            setError(error, QStringLiteral("Reload source path is already owned by another persistent document."));
            setErrorCode(errorCode, QStringLiteral("source_conflict"));
            return false;
        }
    }
    if (afterRevision > beforeRevision)
    {
        if (!archiveCurrentSnapshot(document, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("metadata_limit"));
            return false;
        }
    }
    applySourceBinding(document, sourceBinding);
    document.sourceRevision = newSourceRevision;
    document.dirty = false;
    document.draftMetadata = {};
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    if (reopenedSavedSource) *reopenedSavedSource = reopenedSaved;
    return true;
}
bool RProjectCatalog::recordSave(const QString &persistentDocumentId, const QString &sourcePath,
                                 QString *error, const QString &expectedSourceRevision,
                                 QString *errorCode,
                                 const DocumentInstanceLeasePtr &instanceLease)
{
    return recordSave(persistentDocumentId, sourcePath, PublicationLeasePtr{}, expectedSourceRevision,
                      error, errorCode, instanceLease);
}

bool RProjectCatalog::recordSave(const QString &persistentDocumentId, const QString &sourcePath,
                                 const PublicationLeasePtr &lease,
                                 const QString &expectedSourceRevision,
                                 QString *error, QString *errorCode,
                                 const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    QString localError;
    QString sourceErrorCode;
    const auto binding = resolveSourceBinding(sourcePath, &localError, &sourceErrorCode);
    if (!binding)
    {
        setError(error, localError);
        setErrorCode(errorCode, sourceErrorCode);
        return false;
    }
    return recordSave(persistentDocumentId, *binding, lease, expectedSourceRevision,
                      error, errorCode, instanceLease);
}

bool RProjectCatalog::recordSave(const QString &persistentDocumentId,
                                 const SourceBinding &expectedWrittenBinding,
                                 const PublicationLeasePtr &lease,
                                 const QString &expectedSourceRevision,
                                 QString *error, QString *errorCode,
                                 const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    std::unique_ptr<QLockFile> directLock;
    if (lease)
    {
        if (!lease->lockFile || lease->projectIdentity != projectIdentity
            || lease->workspaceRoot != root
            || lease->lockPath != QDir(root).filePath(QStringLiteral(".project.lock"))
            || !lease->lockFile->isLocked()
            || lease->ownerThread != ownerThread || lease->documentIdentity != persistentDocumentId
            || lease->sourceRevision != expectedSourceRevision
            || QThread::currentThreadId() != lease->ownerThread)
        {
            setError(error, QStringLiteral("Publication lease does not match this catalog, document, or owner thread."));
            setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
            return false;
        }
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return false;
        }
    }
    QString sourceErrorCode;
    const auto currentBinding = resolveSourceBinding(expectedWrittenBinding.logicalSourcePath,
                                                     &localError, &sourceErrorCode);
    if (!currentBinding || !sameSourceBinding(*currentBinding, expectedWrittenBinding))
    {
        setError(error, QStringLiteral("Actual native output no longer matches the worker's written source binding."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return false;
    }
    for (const auto &artifact : currentBinding->sourceArtifacts)
        if (managedCatalogPath(root, artifact.path))
        {
            setError(error, QStringLiteral("Native save cannot publish into a catalog-managed path."));
            setErrorCode(errorCode, QStringLiteral("output_path_conflict"));
            return false;
        }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return false;
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError) || document.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Cannot save a tombstoned document.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!expectedSourceRevision.isEmpty() && document.sourceRevision != expectedSourceRevision)
    {
        setError(error, QStringLiteral("Persistent sourceRevision advanced while the save was being prepared."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    const DocumentInstanceLeasePtr heldInstanceLease = lease ? lease->instanceLease : DocumentInstanceLeasePtr{};
    if (lease && instanceLease && lease->instanceLease != instanceLease)
    {
        setError(error, QStringLiteral("Publication and document instance leases do not match."));
        setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
        return false;
    }
    const DocumentInstanceLeasePtr &effectiveInstanceLease = instanceLease ? instanceLease : heldInstanceLease;
    if (!validateInstanceLease(persistentDocumentId, nullptr, effectiveInstanceLease, error, errorCode)) return false;
    if (lease)
    {
        for (const auto &artifact : currentBinding->sourceArtifacts)
            if (!lease->paths.contains(artifact.path))
            {
                setError(error, QStringLiteral("Published native source artifact was not included in the output lease."));
                setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
                return false;
            }
    }
    const QJsonObject sourceIndex = manifest.value(QStringLiteral("sourcePathIndex")).toObject();
    for (const auto &artifact : currentBinding->sourceArtifacts)
    {
        const QString collision = sourceIndex.value(pathIndexKey(artifact.path)).toString();
        if (!collision.isEmpty() && collision != persistentDocumentId)
        {
            setError(error, QStringLiteral("Native save target is already owned by another persistent document."));
            setErrorCode(errorCode, QStringLiteral("output_path_conflict"));
            return false;
        }
    }
    applySourceBinding(document, *currentBinding);
    document.dirty = false;
    document.draftMetadata = {};
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

RProjectCatalog::PublicationLeasePtr RProjectCatalog::reservePublication(
    const QString &persistentDocumentId, const QStringList &actualOutputPaths,
    const QString &expectedSourceRevision, QString *error, QString *errorCode,
    const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Publication leases must be acquired on the catalog owner thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    if (!validUuid(persistentDocumentId) || !validRevision(expectedSourceRevision)
        || actualOutputPaths.isEmpty() || actualOutputPaths.size() > MaxNativeLinkDepth + 2)
    {
        setError(error, QStringLiteral("Publication lease request violates v1 identity, revision, or path-count bounds."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    QStringList canonicalPaths;
    QSet<QString> uniquePaths;
    for (const QString &path : actualOutputPaths)
    {
        QString canonicalPath;
        if (!canonicalPlannedOutputPath(path, canonicalPath, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return {};
        }
        if (managedCatalogPath(root, canonicalPath))
        {
            setError(error, QStringLiteral("Native output cannot target a catalog-managed path."));
            setErrorCode(errorCode, QStringLiteral("output_path_conflict"));
            return {};
        }
        const QString key = pathIndexKey(canonicalPath);
        if (uniquePaths.contains(key)) continue;
        uniquePaths.insert(key);
        canonicalPaths.append(canonicalPath);
    }
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return {};
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return {};
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError) || document.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Cannot reserve a tombstoned document.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return {};
    }
    if (document.sourceRevision != expectedSourceRevision)
    {
        setError(error, QStringLiteral("Persistent sourceRevision advanced since this runtime prepared its save."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return {};
    }
    if (!validateInstanceLease(persistentDocumentId, nullptr, instanceLease, error, errorCode)) return {};
    if (!currentSourceMatches(document, error, errorCode)) return {};

    const QJsonObject sourceIndex = manifest.value(QStringLiteral("sourcePathIndex")).toObject();
    for (const QString &path : canonicalPaths)
    {
        const QString ownerId = sourceIndex.value(pathIndexKey(path)).toString();
        if (!ownerId.isEmpty() && ownerId != persistentDocumentId)
        {
            setError(error, QStringLiteral("Native output path is owned by another persistent document."));
            setErrorCode(errorCode, QStringLiteral("output_path_conflict"));
            return {};
        }
    }
    return PublicationLeasePtr(new PublicationLease(projectIdentity, root,
                                                       QDir(root).filePath(QStringLiteral(".project.lock")),
                                                       persistentDocumentId,
                                                       expectedSourceRevision, canonicalPaths,
                                                       ownerThread, std::move(lock), instanceLease));
}

bool RProjectCatalog::tombstone(const QString &persistentDocumentId, QString *error,
                                QString *errorCode,
                                const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return false;
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return false;
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!validateInstanceLease(persistentDocumentId, nullptr, instanceLease, error, errorCode)) return false;
    if (document.tombstone) return true;
    document.tombstone = true;
    document.dirty = false;
    document.draftMetadata = {};
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

bool RProjectCatalog::captureSnapshot(const QString &persistentDocumentId,
                                      const RModel &detachedModel,
                                      const InstanceRef &instanceRef,
                                      SnapshotArtifact *artifact, QString *error,
                                      const QString &expectedSourceRevision,
                                      QString *errorCode,
                                      const DocumentInstanceLeasePtr &instanceLease)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Project catalog mutations must run on their owning thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!expectedSourceRevision.isEmpty() && !validRevision(expectedSourceRevision))
    {
        setError(error, QStringLiteral("Expected sourceRevision must use canonical rN syntax."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!artifact || !validInstanceRef(instanceRef, localError))
    {
        setError(error, artifact ? localError : QStringLiteral("Snapshot artifact output is required."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    *artifact = {};
    auto lock = acquireLock(root, localError, errorCode);
    if (!lock)
    {
        setError(error, localError);
        return false;
    }
    QJsonObject manifest;
    if (!loadCurrent(root, projectIdentity, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_manifest"));
        return false;
    }
    Document document;
    if (!findDocument(manifest, persistentDocumentId, document, localError) || document.tombstone)
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Cannot snapshot a tombstoned document.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!expectedSourceRevision.isEmpty() && document.sourceRevision != expectedSourceRevision)
    {
        setError(error, QStringLiteral("Document sourceRevision changed before snapshot publication."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    if (!validateInstanceLease(persistentDocumentId, &instanceRef, instanceLease, error, errorCode)) return false;
    if (!currentSourceMatches(document, error, errorCode)) return false;
    RModel frozenModel(detachedModel);
    QString directory;
    const QString provisionalDirectory = QStringLiteral("models/%1/snapshots/.staging")
                                             .arg(persistentDocumentId);
    if (!prepareManagedDirectory(root, provisionalDirectory, directory, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    const QString stagingName = QStringLiteral("staged-%1.rbm").arg(newId());
    const QString stagingPath = QDir(directory).filePath(stagingName);
    try
    {
        frozenModel.write(stagingPath, false);
    }
    catch (const RError &exception)
    {
        QFile::remove(stagingPath);
        setError(error, exception.getMessage());
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    catch (const std::exception &exception)
    {
        QFile::remove(stagingPath);
        setError(error, QString::fromUtf8(exception.what()));
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    catch (...)
    {
        QFile::remove(stagingPath);
        setError(error, QStringLiteral("Unknown error while serializing the detached model."));
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    QString sha256;
    if (!QFileInfo(stagingPath).isFile() || QFileInfo(stagingPath).isSymLink()
        || !fileHash(stagingPath, sha256, localError))
    {
        QFile::remove(stagingPath);
        setError(error, localError.isEmpty() ? QStringLiteral("Native model serializer produced no regular file.")
                                             : localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    if (!currentSourceMatches(document, error, errorCode))
    {
        QFile::remove(stagingPath);
        return false;
    }
    if (!document.modelSha256.isEmpty()
        && document.snapshotSourceRevision == document.sourceRevision
        && document.modelSha256 != sha256)
    {
        QFile::remove(stagingPath);
        setError(error, QStringLiteral("The same accepted sourceRevision produced different model bytes."));
        setErrorCode(errorCode, QStringLiteral("revision_conflict"));
        return false;
    }
    const QString relative = snapshotPath(persistentDocumentId, sha256);
    QString publishedPath;
    if (!ensureManagedFilePath(root, relative, publishedPath, localError))
    {
        QFile::remove(stagingPath);
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    if (QFileInfo(publishedPath).exists())
    {
        QString existingHash;
        if (!fileHash(publishedPath, existingHash, localError) || existingHash != sha256)
        {
            QFile::remove(stagingPath);
            setError(error, localError.isEmpty()
                                ? QStringLiteral("Hash-addressed snapshot path contains different bytes; it was preserved.")
                                : localError);
            setErrorCode(errorCode, QStringLiteral("io_error"));
            return false;
        }
        QFile::remove(stagingPath);
    }
    else if (!QFile::rename(stagingPath, publishedPath))
    {
        if (QFileInfo(publishedPath).exists())
        {
            QString existingHash;
            if (!fileHash(publishedPath, existingHash, localError) || existingHash != sha256)
            {
                QFile::remove(stagingPath);
                setError(error, localError.isEmpty()
                                    ? QStringLiteral("Concurrent snapshot publication has different bytes.")
                                    : localError);
                setErrorCode(errorCode, QStringLiteral("io_error"));
                return false;
            }
            QFile::remove(stagingPath);
        }
        else
        {
            QFile::remove(stagingPath);
            setError(error, QStringLiteral("Could not publish immutable model snapshot."));
            setErrorCode(errorCode, QStringLiteral("io_error"));
            return false;
        }
    }

    if (!document.modelSha256.isEmpty()
        && document.snapshotSourceRevision == document.sourceRevision
        && document.snapshotInstanceRef != instanceRef.toJson())
    {
        if (!archiveCurrentSnapshot(document, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("metadata_limit"));
            return false;
        }
    }
    document.modelSha256 = sha256;
    document.snapshotSourceRevision = document.sourceRevision;
    document.snapshotRelativePath = relative;
    document.snapshotInstanceRef = instanceRef.toJson();
    storeDocument(manifest, document);
    if (!commitMutation(root, manifest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    SnapshotArtifact result;
    result.id = QStringLiteral("sha256:%1").arg(sha256);
    result.projectId = projectIdentity;
    result.persistentDocumentId = persistentDocumentId;
    result.sourceRevision = document.sourceRevision;
    result.modelSha256 = sha256;
    result.relativePath = relative;
    result.instanceRef = instanceRef;
    *artifact = result;
    return true;
}

QJsonObject RProjectCatalog::capabilityDescription()
{
    const QJsonObject workspaceLayout{
        {QStringLiteral("manifest"), QStringLiteral("project.json")},
        {QStringLiteral("instanceLockPattern"), QStringLiteral("models/<persistentDocumentId>/.instance.lock")},
        {QStringLiteral("snapshotPattern"),
         QStringLiteral("models/<persistentDocumentId>/snapshots/<sha256>/model.rbm")},
        {QStringLiteral("sourcePathIndex"),
         QStringLiteral("canonical absolute paths for each LINK and terminal model artifact to persistentDocumentId")}};
    const QJsonObject identityRules{
        {QStringLiteral("sourceRevision"), QStringLiteral("monotonic rN, N <= 2^53-1")},
        {QStringLiteral("labels"), QStringLiteral("display-only; duplicate matches remain distinct candidates")},
        {QStringLiteral("runtimeOwner"), QStringLiteral("one live writer lease per persistent document across processes")},
        {QStringLiteral("copy"), QStringLiteral("new persistent ID with originDocumentId")},
        {QStringLiteral("tombstone"), QStringLiteral("IDs are retained and never reused")}};
    const QJsonObject supportedModels{
        {QStringLiteral("artifactTypes"), QJsonArray{QStringLiteral("MODEL"), QStringLiteral("LINK")}},
        {QStringLiteral("scope"), QStringLiteral("Native MODEL/LINK identity, source-chain hashing, and immutable binary MODEL snapshot storage")},
        {QStringLiteral("solverCompatibility"), QStringLiteral("not evaluated for arbitrary model, element, and analysis combinations")}};
    const QJsonObject supportedElements{
        {QStringLiteral("scope"), QStringLiteral("Opaque preservation of element records carried by native MODEL artifacts")},
        {QStringLiteral("solverCompatibility"), QStringLiteral("not evaluated; no element formulation allowlist is declared")}};
    const QJsonObject supportedAnalyses{
        {QStringLiteral("scope"), QStringLiteral("Opaque preservation of analysis setup data carried by native MODEL artifacts")},
        {QStringLiteral("execution"), QStringLiteral("not provided by the project catalog")},
        {QStringLiteral("numericalAcceptance"), QStringLiteral("not inferred")}};
    const QJsonObject unitConvention{
        {QStringLiteral("system"), QStringLiteral("SI")},
        {QStringLiteral("length"), QStringLiteral("m")},
        {QStringLiteral("mass"), QStringLiteral("kg")},
        {QStringLiteral("time"), QStringLiteral("s")},
        {QStringLiteral("temperature"), QStringLiteral("K")},
        {QStringLiteral("angle"), QStringLiteral("rad")},
        {QStringLiteral("conversion"), false},
        {QStringLiteral("numericalAcceptance"), false}};
    const QJsonObject snapshotExecution{
        {QStringLiteral("execution"), QStringLiteral("synchronous on the catalog owner thread")},
        {QStringLiteral("independentCancellationApi"), false}};
    const QJsonObject limits{
        {QStringLiteral("manifestBytes"), double(MaxManifestBytes)},
        {QStringLiteral("documents"), MaxDocuments},
        {QStringLiteral("jsonDepth"), MaxJsonDepth},
        {QStringLiteral("jsonNodes"), MaxJsonNodes},
        {QStringLiteral("labelUtf8Bytes"), int(MaxLabelBytes)},
        {QStringLiteral("sourcePathUtf8Bytes"), int(MaxPathBytes)},
        {QStringLiteral("draftMetadataBytes"), int(MaxDraftBytes)},
        {QStringLiteral("nativeLinkDepth"), MaxNativeLinkDepth},
        {QStringLiteral("nativeSourceArtifactsPerDocument"), MaxNativeLinkDepth + 1},
        {QStringLiteral("nativeLinkBytes"), double(MaxNativeLinkBytes)},
        {QStringLiteral("nativeArtifactBytes"), double(MaxNativeArtifactBytes)},
        {QStringLiteral("orphanSnapshotsPerDocument"), MaxOrphanSnapshots}};
    const QJsonObject draftMetadataFields{
        {QStringLiteral("draftId"), QStringLiteral("canonical UUID")},
        {QStringLiteral("status"), QJsonArray{QStringLiteral("none"), QStringLiteral("pending"),
                                               QStringLiteral("unknown")}},
        {QStringLiteral("kind"), QJsonArray{QStringLiteral("analysis"), QStringLiteral("selection"),
                                             QStringLiteral("other")}},
        {QStringLiteral("changedFields"), QStringLiteral("up to 64 identifier strings; values are excluded")},
        {QStringLiteral("fieldCount"), QStringLiteral("integer from 0 through 4096")}};
    const QJsonObject sourceBindingShape{
        {QStringLiteral("sourcePath"), QStringLiteral("canonical logical source alias")},
        {QStringLiteral("resolvedSourcePath"), QStringLiteral("canonical terminal MODEL path")},
        {QStringLiteral("sourceArtifacts"), QStringLiteral("ordered LINK chain and terminal records: path,sha256")},
        {QStringLiteral("savedArtifactHash"), QStringLiteral("terminal MODEL raw-byte SHA-256")}};
    const QJsonObject orphanSnapshotShape{
        {QStringLiteral("fields"), QJsonArray{QStringLiteral("sourceRevision"),
                                              QStringLiteral("modelSha256"),
                                              QStringLiteral("relativePath"),
                                              QStringLiteral("instanceRef")}},
        {QStringLiteral("maximumPerDocument"), MaxOrphanSnapshots},
        {QStringLiteral("retention"), QStringLiteral("metadata and immutable file remain after revision advances")}};

    const QString exampleProjectId = QStringLiteral("00000000-0000-4000-8000-000000000001");
    const QString exampleDocumentId = QStringLiteral("00000000-0000-4000-8000-000000000002");
    const QString exampleRuntimeDocumentId = QStringLiteral("00000000-0000-4000-8000-000000000003");
    const QString exampleSessionEpoch = QStringLiteral("00000000-0000-4000-8000-000000000004");
    const QJsonObject queryExample{
        {QStringLiteral("operation"), QStringLiteral("project.query")},
        {QStringLiteral("request"), QJsonObject{
             {QStringLiteral("workspace"), QStringLiteral("<path>")},
             {QStringLiteral("query"), true}}}};
    const QJsonObject acceptExample{
        {QStringLiteral("operation"), QStringLiteral("document.accept")},
        {QStringLiteral("request"), QJsonObject{
             {QStringLiteral("persistentDocumentId"), exampleDocumentId},
             {QStringLiteral("sourceRevision"), QStringLiteral("r1")},
             {QStringLiteral("dirty"), true},
             {QStringLiteral("draftMetadata"), QJsonObject{
                  {QStringLiteral("status"), QStringLiteral("pending")},
                  {QStringLiteral("kind"), QStringLiteral("analysis")},
                  {QStringLiteral("changedFields"), QJsonArray{QStringLiteral("modal.tolerance")}},
                  {QStringLiteral("fieldCount"), 1}}}}}};
    const QJsonObject snapshotExample{
        {QStringLiteral("operation"), QStringLiteral("document.snapshot")},
        {QStringLiteral("request"), QJsonObject{
             {QStringLiteral("persistentDocumentId"), exampleDocumentId},
             {QStringLiteral("expectedSourceRevision"), QStringLiteral("r1")},
             {QStringLiteral("instanceRef"), QJsonObject{
                  {QStringLiteral("documentId"), exampleRuntimeDocumentId},
                  {QStringLiteral("sessionEpoch"), exampleSessionEpoch},
                  {QStringLiteral("documentRevision"), 0}}}}}};
    const QJsonObject documentRecordExample{
        {QStringLiteral("persistentDocumentId"), exampleDocumentId},
        {QStringLiteral("label"), QStringLiteral("Untitled")},
        {QStringLiteral("sourcePath"), QString()},
        {QStringLiteral("resolvedSourcePath"), QString()},
        {QStringLiteral("sourceArtifacts"), QJsonArray{}},
        {QStringLiteral("originDocumentId"), QString()},
        {QStringLiteral("sourceRevision"), QStringLiteral("r0")},
        {QStringLiteral("savedArtifactHash"), QString()},
        {QStringLiteral("dirty"), true},
        {QStringLiteral("draftMetadata"), QJsonObject{}},
        {QStringLiteral("tombstone"), false},
        {QStringLiteral("modelSha256"), QString()},
        {QStringLiteral("snapshotSourceRevision"), QString()},
        {QStringLiteral("snapshotRelativePath"), QString()},
        {QStringLiteral("snapshotInstanceRef"), QJsonObject{}},
        {QStringLiteral("orphanSnapshots"), QJsonArray{}}};
    const QJsonObject manifestExample{
        {QStringLiteral("storageVersion"), StorageVersion},
        {QStringLiteral("projectId"), exampleProjectId},
        {QStringLiteral("documents"), QJsonObject{{exampleDocumentId, documentRecordExample}}},
        {QStringLiteral("sourcePathIndex"), QJsonObject{}}};
    const QJsonArray examples{queryExample, acceptExample, snapshotExample};
    const QJsonArray refusals{QStringLiteral("unknown schema versions or fields"),
                              QStringLiteral("invalid or escaped managed paths"),
                              QStringLiteral("corrupt manifests"),
                              QStringLiteral("changed registered source bytes without explicit reload"),
                              QStringLiteral("invalid runtime instance references")};

    return {{QStringLiteral("protocolVersion"), QStringLiteral("range.project/v1")},
            {QStringLiteral("storageVersion"), StorageVersion},
            {QStringLiteral("workspaceLayout"), workspaceLayout},
            {QStringLiteral("identityRules"), identityRules},
            {QStringLiteral("supportedModels"), supportedModels},
            {QStringLiteral("supportedElements"), supportedElements},
            {QStringLiteral("supportedAnalyses"), supportedAnalyses},
            {QStringLiteral("unitConvention"), unitConvention},
            {QStringLiteral("unitConventionScope"),
             QStringLiteral("Expected native convention only; the catalog does not validate physical units in model data.")},
            {QStringLiteral("snapshotExecution"), snapshotExecution},
            {QStringLiteral("readOnlyOperations"),
             QJsonArray{QStringLiteral("project.query"), QStringLiteral("document.query")}},
            {QStringLiteral("writeOperations"),
             QJsonArray{QStringLiteral("project.create"), QStringLiteral("document.instance.acquire"),
                        QStringLiteral("document.create"),
                        QStringLiteral("document.register"), QStringLiteral("document.copy"),
                        QStringLiteral("document.rename"), QStringLiteral("document.accept"),
                        QStringLiteral("document.save.reserve"), QStringLiteral("document.save.commit"),
                        QStringLiteral("document.reload"),
                        QStringLiteral("document.tombstone"), QStringLiteral("document.snapshot")}},
            {QStringLiteral("limits"), limits},
            {QStringLiteral("draftMetadataFields"), draftMetadataFields},
            {QStringLiteral("sourceBindingShape"), sourceBindingShape},
            {QStringLiteral("orphanSnapshotShape"), orphanSnapshotShape},
            {QStringLiteral("manifestExample"), manifestExample},
            {QStringLiteral("examples"), examples},
            {QStringLiteral("refusals"), refusals}};
}

bool RProjectCatalog::validatePublicationLease(const PublicationLeasePtr &lease,
                                              QString *error, QString *errorCode) const
{
    if (!lease || !lease->lockFile || !lease->lockFile->isLocked()
        || lease->projectIdentity != projectIdentity || lease->workspaceRoot != root
        || lease->lockPath != QDir(root).filePath(QStringLiteral(".project.lock"))
        || lease->ownerThread != ownerThread || QThread::currentThreadId() != ownerThread
        || !validUuid(lease->documentIdentity) || !validRevision(lease->sourceRevision)
        || lease->paths.isEmpty())
    {
        setError(error, QStringLiteral("Publication lease does not belong to this workspace or owner thread."));
        setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
        return false;
    }
    QString localError;
    if (!validWorkspaceRoot(root, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    return true;
}

RProjectCatalog::BeginOperationResult RProjectCatalog::beginOperation(
    const QString &idempotencyKey, const QJsonObject &normalizedRequest,
    const OperationAssociation &association, const PublicationLeasePtr &publicationLease,
    QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    BeginOperationResult result;
    QString localError, digest;
    if (QThread::currentThreadId() != ownerThread || !validOperationToken(idempotencyKey)
        || !validOperationAssociation(association) || !association.observation.isEmpty()
        || !requestDigest(normalizedRequest, digest, localError))
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Operation identity, owner thread, or association is invalid.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return result;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return result;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return result;
        }
    }

    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return result;
    }
    const int existingIndex = findOperationIndex(operations, QStringLiteral("idempotencyKey"), idempotencyKey);
    if (existingIndex >= 0)
    {
        const QJsonObject existing = operations.at(existingIndex).toObject();
        result.operation = operationFromJsonUnchecked(existing);
        if (result.operation.normalizedRequestSha256 != digest)
        {
            result.disposition = OperationDisposition::Conflict;
            setError(error, QStringLiteral("This project idempotency key is already bound to a different normalized request."));
            setErrorCode(errorCode, QStringLiteral("idempotency_conflict"));
            return result;
        }
        result.disposition = OperationDisposition::Reused;
        return result;
    }
    if (operations.size() >= MaxDurableOperations)
    {
        setError(error, QStringLiteral("Project operation ledger reached its record limit."));
        setErrorCode(errorCode, QStringLiteral("ledger_limit"));
        return result;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    DurableOperation operation;
    operation.projectId = projectIdentity;
    operation.ownerRuntimeId = runtimeIdentity;
    operation.operationId = newId();
    operation.idempotencyKey = idempotencyKey;
    operation.normalizedRequestSha256 = digest;
    operation.phase = OperationPhase::Intent;
    operation.association = association;
    operation.createdAtUtc = now;
    operation.updatedAtUtc = now;
    QJsonArray events{operationEventJson(OperationPhase::Intent, association, now)};
    operations.append(durableOperationJson(operation, events));
    if (!writeOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return result;
    }
    result.disposition = OperationDisposition::Created;
    result.operation = std::move(operation);
    return result;
}

bool RProjectCatalog::advanceOperation(const QString &operationId, OperationPhase phase,
                                       const OperationAssociation &association,
                                       const PublicationLeasePtr &publicationLease,
                                       QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !validUuid(operationId)
        || operationPhaseName(phase).isEmpty()
        || !validOperationAssociation(association))
    {
        setError(error, QStringLiteral("Operation ID, phase, owner thread, or association is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return false;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return false;
        }
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return false;
    }
    const int index = findOperationIndex(operations, QStringLiteral("operationId"), operationId);
    if (index < 0)
    {
        setError(error, QStringLiteral("Durable operation was not found in this project."));
        setErrorCode(errorCode, QStringLiteral("not_found"));
        return false;
    }
    QJsonObject object = operations.at(index).toObject();
    DurableOperation operation = operationFromJsonUnchecked(object);
    if (!validPhaseTransition(operation.phase, phase))
    {
        setError(error, QStringLiteral("Durable operation phase transition is not allowed."));
        setErrorCode(errorCode, QStringLiteral("invalid_transition"));
        return false;
    }
    OperationAssociation updateAssociation = association;
    if (phase != OperationPhase::Observed) updateAssociation.observation = {};
    OperationAssociation nextAssociation = operation.association;
    mergeOperationAssociation(nextAssociation, updateAssociation);
    if (phase == OperationPhase::Observed && nextAssociation.observation.isEmpty()
        && operation.artifacts.isEmpty())
    {
        setError(error, QStringLiteral("A no-artifact operation requires its original trusted-host observation."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if ((phase == OperationPhase::Accepted || phase == OperationPhase::Observed)
        && updateAssociation.diagnosticCode.isEmpty())
        nextAssociation.diagnosticCode.clear();
    const bool phaseUnchanged = operation.phase == phase;
    const bool associationUnchanged = operationAssociationJson(nextAssociation)
        == operationAssociationJson(operation.association);
    if (phaseUnchanged && associationUnchanged) return true;
    QJsonArray events = operationEvents(object);
    if (events.size() >= MaxOperationEvents)
    {
        setError(error, QStringLiteral("Durable operation reached its event limit."));
        setErrorCode(errorCode, QStringLiteral("ledger_limit"));
        return false;
    }
    QDateTime now = QDateTime::currentDateTimeUtc();
    if (now < operation.updatedAtUtc) now = operation.updatedAtUtc;
    operation.phase = phase;
    operation.association = nextAssociation;
    operation.updatedAtUtc = now;
    appendOperationEvent(object, phase, nextAssociation, now);
    object = durableOperationJson(operation, operationEvents(object));
    operations[index] = object;
    if (!writeOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

bool RProjectCatalog::markUnfinishedOperationsUnknown(
    const PublicationLeasePtr &publicationLease, QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Operation recovery must run on the catalog owner thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return false;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return false;
        }
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return false;
    }
    bool changed = false;
    for (qsizetype index = 0; index < operations.size(); ++index)
    {
        QJsonObject object = operations.at(index).toObject();
        DurableOperation operation = operationFromJsonUnchecked(object);
        if (operation.ownerRuntimeId == runtimeIdentity) continue;
        if (operation.phase != OperationPhase::Intent
            && operation.phase != OperationPhase::Accepted
            && operation.phase != OperationPhase::ArtifactPublished) continue;
        const QString runtimeDirectory = QDir(root).filePath(QStringLiteral("operations/runtimes"));
        const QFileInfo runtimeDirectoryInfo(runtimeDirectory);
        if (!runtimeDirectoryInfo.isDir() || runtimeDirectoryInfo.isSymLink())
        {
            setError(error, QStringLiteral("Project runtime liveness directory is unavailable or unsafe."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return false;
        }
        const QString ownerLockPath = QDir(runtimeDirectory).filePath(
            operation.ownerRuntimeId + QStringLiteral(".lock"));
        if (QFileInfo(ownerLockPath).isSymLink())
        {
            setError(error, QStringLiteral("Recorded project runtime lock path is a symbolic link."));
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return false;
        }
        QLockFile ownerProbe(ownerLockPath);
        ownerProbe.setStaleLockTime(0);
        if (!ownerProbe.tryLock(0)) continue;
        ownerProbe.unlock();
        const QJsonArray events = operationEvents(object);
        if (events.size() >= MaxOperationEvents)
        {
            setError(error, QStringLiteral("An unfinished operation reached its event limit and could not be marked unknown."));
            setErrorCode(errorCode, QStringLiteral("ledger_limit"));
            return false;
        }
        OperationAssociation association = operation.association;
        association.diagnosticCode = QStringLiteral("interrupted_unobserved");
        QDateTime now = QDateTime::currentDateTimeUtc();
        if (now < operation.updatedAtUtc) now = operation.updatedAtUtc;
        operation.phase = OperationPhase::Unknown;
        operation.association = association;
        operation.updatedAtUtc = now;
        appendOperationEvent(object, OperationPhase::Unknown, association, now);
        operations[index] = durableOperationJson(operation, operationEvents(object));
        changed = true;
    }
    if (!changed) return true;
    if (!writeOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

std::optional<RProjectCatalog::DurableOperation> RProjectCatalog::queryOperation(
    const QString &operationId, const PublicationLeasePtr &publicationLease,
    QString *error, QString *errorCode) const
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !validUuid(operationId))
    {
        setError(error, QStringLiteral("Operation ID or owner thread is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return std::nullopt;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return std::nullopt;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return std::nullopt;
        }
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return std::nullopt;
    }
    const int index = findOperationIndex(operations, QStringLiteral("operationId"), operationId);
    if (index < 0)
    {
        setError(error, QStringLiteral("Durable operation was not found in this project."));
        setErrorCode(errorCode, QStringLiteral("not_found"));
        return std::nullopt;
    }
    return operationFromJsonUnchecked(operations.at(index).toObject());
}

std::optional<RProjectCatalog::DurableOperation> RProjectCatalog::queryOperationByIdempotencyKey(
    const QString &idempotencyKey, const PublicationLeasePtr &publicationLease,
    QString *error, QString *errorCode) const
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !validOperationToken(idempotencyKey))
    {
        setError(error, QStringLiteral("Operation idempotency key or owner thread is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return std::nullopt;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return std::nullopt;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return std::nullopt;
        }
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return std::nullopt;
    }
    const int index = findOperationIndex(operations, QStringLiteral("idempotencyKey"), idempotencyKey);
    if (index < 0) return std::nullopt;
    return operationFromJsonUnchecked(operations.at(index).toObject());
}

RProjectCatalog::OperationPage RProjectCatalog::queryOperations(
    int limit, const QString &afterOperationId,
    const PublicationLeasePtr &publicationLease, QString *error, QString *errorCode) const
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    OperationPage page;
    QString localError;
    if (QThread::currentThreadId() != ownerThread || limit < 1 || limit > 200
        || (!afterOperationId.isEmpty() && !validUuid(afterOperationId)))
    {
        setError(error, QStringLiteral("Operation page limit, cursor, or owner thread is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return page;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return page;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return page;
        }
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return page;
    }
    qsizetype start = 0;
    if (!afterOperationId.isEmpty())
    {
        const int cursorIndex = findOperationIndex(operations, QStringLiteral("operationId"), afterOperationId);
        if (cursorIndex < 0)
        {
            setError(error, QStringLiteral("Operation cursor is not present in this project ledger."));
            setErrorCode(errorCode, QStringLiteral("invalid_cursor"));
            return page;
        }
        start = qsizetype(cursorIndex) + 1;
    }
    const qsizetype end = qMin(operations.size(), start + qsizetype(limit));
    page.operations.reserve(end - start);
    for (qsizetype index = start; index < end; ++index)
        page.operations.append(operationFromJsonUnchecked(operations.at(index).toObject()));
    if (end < operations.size() && end > start)
        page.nextCursor = page.operations.constLast().operationId;
    return page;
}

RProjectCatalog::BeginOperationResult RProjectCatalog::beginSavePublication(
    const QString &persistentDocumentId, const QString &idempotencyKey,
    const QJsonObject &normalizedRequest, const QString &expectedSourceRevision,
    const QVector<OperationArtifactIntent> &artifacts,
    const PublicationLeasePtr &publicationLease,
    const OperationAssociation &association, QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    BeginOperationResult result;
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !publicationLease
        || !validatePublicationLease(publicationLease, error, errorCode))
    {
        if (!publicationLease && error) *error = QStringLiteral("A live publication lease is required before recording a save intent.");
        if (!publicationLease && errorCode) *errorCode = QStringLiteral("lease_mismatch");
        return result;
    }
    if (!validUuid(persistentDocumentId) || publicationLease->documentIdentity != persistentDocumentId
        || !validRevision(expectedSourceRevision) || publicationLease->sourceRevision != expectedSourceRevision
        || !validOperationToken(idempotencyKey)
        || !validOperationAssociation(association) || !association.observation.isEmpty()
        || normalizedRequest.isEmpty()
        || artifacts.isEmpty() || artifacts.size() > MaxOperationArtifacts)
    {
        setError(error, QStringLiteral("Save intent identity, revision, association, or artifact count is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return result;
    }

    QSet<QString> reservedPaths;
    for (const QString &reserved : publicationLease->paths) reservedPaths.insert(pathIndexKey(reserved));
    QSet<QString> uniquePaths;
    QJsonArray normalizedArtifacts;
    QVector<OperationArtifactIntent> checkedArtifacts;
    checkedArtifacts.reserve(artifacts.size());
    for (const OperationArtifactIntent &input : artifacts)
    {
        QString canonical;
        if (!canonicalPlannedOutputPath(input.path, canonical, localError)
            || managedCatalogPath(root, canonical))
        {
            setError(error, localError.isEmpty() ? QStringLiteral("Save intent cannot target a managed workspace path.") : localError);
            setErrorCode(errorCode, QStringLiteral("invalid_argument"));
            return result;
        }
        const QString key = pathIndexKey(canonical);
        if (!reservedPaths.contains(key) || uniquePaths.contains(key)
            || (input.expectedPriorExists ? !validHash(input.expectedPriorSha256)
                                          : !input.expectedPriorSha256.isEmpty())
            || (!input.intendedSha256.isEmpty() && !validHash(input.intendedSha256))
            || !input.observedStatus.isEmpty() || input.observedExists || !input.observedSha256.isEmpty())
        {
            setError(error, QStringLiteral("Save artifact intent is not bound to the active lease or has invalid hashes."));
            setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
            return result;
        }
        OperationArtifactIntent checked;
        checked.path = canonical;
        checked.expectedPriorExists = input.expectedPriorExists;
        checked.expectedPriorSha256 = input.expectedPriorSha256;
        checked.intendedSha256 = input.intendedSha256;
        checkedArtifacts.append(checked);
        uniquePaths.insert(key);
        normalizedArtifacts.append(QJsonObject{
            {QStringLiteral("path"), canonical},
            {QStringLiteral("expectedPriorExists"), input.expectedPriorExists},
            {QStringLiteral("expectedPriorSha256"), input.expectedPriorSha256}});
    }
    QJsonObject digestRequest{{QStringLiteral("request"), normalizedRequest},
        {QStringLiteral("persistentDocumentId"), persistentDocumentId},
        {QStringLiteral("expectedSourceRevision"), expectedSourceRevision},
        {QStringLiteral("artifacts"), normalizedArtifacts}};
    QString digest;
    if (!requestDigest(digestRequest, digest, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return result;
    }

    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return result;
    }
    const int existingIndex = findOperationIndex(operations, QStringLiteral("idempotencyKey"), idempotencyKey);
    if (existingIndex >= 0)
    {
        result.operation = operationFromJsonUnchecked(operations.at(existingIndex).toObject());
        if (result.operation.normalizedRequestSha256 != digest)
        {
            result.disposition = OperationDisposition::Conflict;
            setError(error, QStringLiteral("This project idempotency key is already bound to a different save intent."));
            setErrorCode(errorCode, QStringLiteral("idempotency_conflict"));
            return result;
        }
        if (result.operation.artifacts.size() != checkedArtifacts.size())
        {
            setError(error, QStringLiteral("The saved operation intent is incomplete; it cannot be replayed."));
            setErrorCode(errorCode, QStringLiteral("unknown_operation"));
            return result;
        }
        for (qsizetype i = 0; i < checkedArtifacts.size(); ++i)
        {
            auto &stored = result.operation.artifacts[i];
            const auto &requested = checkedArtifacts.at(i);
            if (pathIndexKey(stored.path) != pathIndexKey(requested.path)
                || stored.expectedPriorExists != requested.expectedPriorExists
                || stored.expectedPriorSha256 != requested.expectedPriorSha256
                || (!requested.intendedSha256.isEmpty() && !stored.intendedSha256.isEmpty()
                    && requested.intendedSha256 != stored.intendedSha256))
            {
                result.disposition = OperationDisposition::Conflict;
                setError(error, QStringLiteral("The existing operation has different per-file publication evidence."));
                setErrorCode(errorCode, QStringLiteral("idempotency_conflict"));
                return result;
            }
        }
        result.disposition = OperationDisposition::Reused;
        return result;
    }
    if (operations.size() >= MaxDurableOperations)
    {
        setError(error, QStringLiteral("Project operation ledger reached its record limit."));
        setErrorCode(errorCode, QStringLiteral("ledger_limit"));
        return result;
    }
    // A reused key returns its original operation without rechecking current
    // file bytes; a completed publication must never be mistaken for a new
    // prior-state conflict or executed a second time.
    for (const OperationArtifactIntent &artifact : checkedArtifacts)
    {
        const QFileInfo info(artifact.path);
        if (info.isSymLink() || info.exists() != artifact.expectedPriorExists)
        {
            setError(error, QStringLiteral("Save target existence changed after the expected prior state was captured."));
            setErrorCode(errorCode, QStringLiteral("source_conflict"));
            return result;
        }
        if (artifact.expectedPriorExists)
        {
            QString actualPrior;
            if (!fileHash(artifact.path, actualPrior, localError)
                || actualPrior != artifact.expectedPriorSha256)
            {
                setError(error, localError.isEmpty()
                    ? QStringLiteral("Save target bytes changed after the expected prior hash was captured.")
                    : localError);
                setErrorCode(errorCode, QStringLiteral("source_conflict"));
                return result;
            }
        }
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    DurableOperation operation;
    operation.projectId = projectIdentity;
    operation.ownerRuntimeId = runtimeIdentity;
    operation.operationId = newId();
    operation.idempotencyKey = idempotencyKey;
    operation.normalizedRequestSha256 = digest;
    operation.phase = OperationPhase::Intent;
    operation.association = association;
    operation.artifacts = checkedArtifacts;
    operation.createdAtUtc = now;
    operation.updatedAtUtc = now;
    QJsonArray events{operationEventJson(OperationPhase::Intent, association, now)};
    operations.append(durableOperationJson(operation, events));
    if (!writeOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return result;
    }
    result.disposition = OperationDisposition::Created;
    result.operation = std::move(operation);
    return result;
}

bool RProjectCatalog::bindSaveArtifactHash(const QString &operationId, const QString &path,
                                           const QString &intendedSha256,
                                           const PublicationLeasePtr &publicationLease,
                                           QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError, canonical;
    if (QThread::currentThreadId() != ownerThread || !validUuid(operationId)
        || !validHash(intendedSha256) || !canonicalPlannedOutputPath(path, canonical, localError))
    {
        setError(error, localError.isEmpty() ? QStringLiteral("Operation, path, or intended artifact hash is invalid.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    if (!publicationLease || !validatePublicationLease(publicationLease, error, errorCode))
    {
        if (!publicationLease && error) *error = QStringLiteral("A live publication lease is required to bind artifact bytes.");
        if (!publicationLease && errorCode) *errorCode = QStringLiteral("lease_mismatch");
        return false;
    }
    bool pathIsLeased = false;
    for (const QString &reservedPath : publicationLease->paths)
        pathIsLeased |= pathIndexKey(reservedPath) == pathIndexKey(canonical);
    if (!pathIsLeased)
    {
        setError(error, QStringLiteral("Intended artifact path is not held by the publication lease."));
        setErrorCode(errorCode, QStringLiteral("lease_mismatch"));
        return false;
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return false;
    }
    const int index = findOperationIndex(operations, QStringLiteral("operationId"), operationId);
    if (index < 0)
    {
        setError(error, QStringLiteral("Durable save operation was not found."));
        setErrorCode(errorCode, QStringLiteral("not_found"));
        return false;
    }
    QJsonObject object = operations.at(index).toObject();
    DurableOperation operation = operationFromJsonUnchecked(object);
    if (operation.phase != OperationPhase::Intent && operation.phase != OperationPhase::Accepted
        && operation.phase != OperationPhase::Unknown)
    {
        setError(error, QStringLiteral("Artifact bytes can only be bound before publication is observed."));
        setErrorCode(errorCode, QStringLiteral("invalid_transition"));
        return false;
    }
    int artifactIndex = -1;
    for (qsizetype i = 0; i < operation.artifacts.size(); ++i)
        if (pathIndexKey(operation.artifacts.at(i).path) == pathIndexKey(canonical))
        {
            artifactIndex = int(i);
            break;
        }
    if (artifactIndex < 0)
    {
        setError(error, QStringLiteral("Artifact path is not part of this durable save intent."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    OperationArtifactIntent &artifact = operation.artifacts[artifactIndex];
    // Binding the same staged bytes again is read-only. Publication may have
    // replaced the predecessor meanwhile; reconcileSavePublication checks
    // the current target against this intended hash separately.
    if (!artifact.intendedSha256.isEmpty())
    {
        if (artifact.intendedSha256 != intendedSha256)
        {
            setError(error, QStringLiteral("Intended artifact hash is already bound to different bytes."));
            setErrorCode(errorCode, QStringLiteral("idempotency_conflict"));
            return false;
        }
        return true;
    }
    const QFileInfo info(canonical);
    if (info.isSymLink() || info.exists() != artifact.expectedPriorExists)
    {
        setError(error, QStringLiteral("Save target changed before intended bytes were bound."));
        setErrorCode(errorCode, QStringLiteral("source_conflict"));
        return false;
    }
    if (artifact.expectedPriorExists)
    {
        QString actualPrior;
        if (!fileHash(canonical, actualPrior, localError)
            || actualPrior != artifact.expectedPriorSha256)
        {
            setError(error, localError.isEmpty()
                ? QStringLiteral("Save target bytes changed before intended bytes were bound.") : localError);
            setErrorCode(errorCode, QStringLiteral("source_conflict"));
            return false;
        }
    }
    artifact.intendedSha256 = intendedSha256;
    object = durableOperationJson(operation, operationEvents(object));
    operations[index] = object;
    if (!writeOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

bool RProjectCatalog::reconcileSavePublication(const QString &operationId,
                                               const PublicationLeasePtr &publicationLease,
                                               QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !validUuid(operationId))
    {
        setError(error, QStringLiteral("Operation ID or owner thread is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return false;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return false;
        }
    }
    QJsonArray operations;
    if (!readOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_operation_ledger"));
        return false;
    }
    const int index = findOperationIndex(operations, QStringLiteral("operationId"), operationId);
    if (index < 0)
    {
        setError(error, QStringLiteral("Durable save operation was not found."));
        setErrorCode(errorCode, QStringLiteral("not_found"));
        return false;
    }
    QJsonObject object = operations.at(index).toObject();
    DurableOperation operation = operationFromJsonUnchecked(object);
    if (operation.artifacts.isEmpty())
    {
        OperationAssociation association = operation.association;
        association.diagnosticCode = QStringLiteral("incomplete_save_intent");
        if (operation.phase != OperationPhase::Unknown)
        {
            if (!validPhaseTransition(operation.phase, OperationPhase::Unknown))
            {
                setError(error, QStringLiteral("Operation state cannot reconcile an incomplete save intent."));
                setErrorCode(errorCode, QStringLiteral("invalid_transition"));
                return false;
            }
            QDateTime now = QDateTime::currentDateTimeUtc();
            if (now < operation.updatedAtUtc) now = operation.updatedAtUtc;
            operation.phase = OperationPhase::Unknown;
            operation.association = association;
            operation.updatedAtUtc = now;
            appendOperationEvent(object, OperationPhase::Unknown, association, now);
        }
        object = durableOperationJson(operation, operationEvents(object));
        operations[index] = object;
        if (!writeOperationLedger(root, projectIdentity, operations, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("io_error"));
            return false;
        }
        return true;
    }

    bool anyConflict = false;
    bool anyUnknown = false;
    int publishedCount = 0;
    int notPublishedCount = 0;
    for (OperationArtifactIntent &artifact : operation.artifacts)
    {
        const QFileInfo info(artifact.path);
        artifact.observedExists = info.exists() || info.isSymLink();
        artifact.observedSha256.clear();
        artifact.observedStatus.clear();
        if (info.isSymLink())
        {
            artifact.observedStatus = QStringLiteral("conflict");
            anyConflict = true;
            continue;
        }
        QString canonical;
        if (!canonicalPlannedOutputPath(artifact.path, canonical, localError)
            || pathIndexKey(canonical) != pathIndexKey(artifact.path)
            || managedCatalogPath(root, canonical))
        {
            artifact.observedExists = false;
            artifact.observedStatus = QStringLiteral("unknown");
            anyUnknown = true;
            continue;
        }
        if (!info.exists())
        {
            if (artifact.expectedPriorExists)
            {
                artifact.observedStatus = QStringLiteral("conflict");
                anyConflict = true;
            }
            else
            {
                artifact.observedStatus = QStringLiteral("not_published");
                ++notPublishedCount;
            }
            continue;
        }
        QString actual;
        if (!fileHash(canonical, actual, localError))
        {
            artifact.observedStatus = QStringLiteral("unreadable");
            anyUnknown = true;
            continue;
        }
        artifact.observedSha256 = actual;
        if (!artifact.intendedSha256.isEmpty() && actual == artifact.intendedSha256)
        {
            if (artifact.expectedPriorExists && actual == artifact.expectedPriorSha256)
            {
                artifact.observedStatus = QStringLiteral("unchanged_matches_intended");
                ++notPublishedCount;
            }
            else
            {
                artifact.observedStatus = QStringLiteral("published");
                ++publishedCount;
            }
        }
        else if (artifact.expectedPriorExists && actual == artifact.expectedPriorSha256)
        {
            artifact.observedStatus = QStringLiteral("not_published");
            ++notPublishedCount;
        }
        else if (!artifact.intendedSha256.isEmpty() || artifact.expectedPriorExists)
        {
            artifact.observedStatus = QStringLiteral("conflict");
            anyConflict = true;
        }
        else
        {
            // A newly found file without an intended hash cannot prove this
            // process published it; preserve the unresolved state.
            artifact.observedStatus = QStringLiteral("unknown");
            anyUnknown = true;
        }
    }

    bool acceptedWasRecorded = false;
    for (const QJsonValue &eventValue : operationEvents(object))
        acceptedWasRecorded |= eventValue.toObject().value(QStringLiteral("phase")).toString()
            == QStringLiteral("accepted");
    OperationPhase targetPhase = OperationPhase::Unknown;
    OperationAssociation association = operation.association;
    if (operation.phase == OperationPhase::Conflict)
    {
        targetPhase = OperationPhase::Conflict;
        if (association.diagnosticCode.isEmpty())
            association.diagnosticCode = QStringLiteral("publication_conflict");
    }
    else if (anyConflict)
    {
        targetPhase = OperationPhase::Conflict;
        association.diagnosticCode = QStringLiteral("publication_conflict");
    }
    else if (anyUnknown)
    {
        targetPhase = OperationPhase::Unknown;
        association.diagnosticCode = QStringLiteral("publication_unknown");
    }
    else if (publishedCount > 0 && notPublishedCount > 0)
    {
        targetPhase = OperationPhase::Unknown;
        association.diagnosticCode = QStringLiteral("partial_publication");
    }
    else if (!acceptedWasRecorded)
    {
        if (publishedCount == 0 && notPublishedCount == operation.artifacts.size())
        {
            targetPhase = OperationPhase::Observed;
            association.diagnosticCode = QStringLiteral("artifacts_not_published");
        }
        else
        {
            targetPhase = OperationPhase::Unknown;
            association.diagnosticCode = QStringLiteral("accepted_state_unverified");
        }
    }
    else
    {
        targetPhase = OperationPhase::Observed;
        association.diagnosticCode = publishedCount == 0
            ? QStringLiteral("artifacts_not_published") : QString();
    }

    const auto persistPhase = [&](OperationPhase phase, OperationAssociation nextAssociation,
                                  QJsonObject &record, DurableOperation &current,
                                  QJsonArray &events, QString &failure) -> bool {
        if (!validPhaseTransition(current.phase, phase))
        {
            failure = QStringLiteral("Operation state cannot record the publication observation.");
            return false;
        }
        if (current.phase == phase && operationAssociationJson(current.association)
                == operationAssociationJson(nextAssociation)) return true;
        if (events.size() >= MaxOperationEvents)
        {
            failure = QStringLiteral("Durable operation reached its event limit.");
            return false;
        }
        QDateTime now = QDateTime::currentDateTimeUtc();
        if (now < current.updatedAtUtc) now = current.updatedAtUtc;
        current.phase = phase;
        current.association = std::move(nextAssociation);
        current.updatedAtUtc = now;
        appendOperationEvent(record, phase, current.association, now);
        record = durableOperationJson(current, operationEvents(record));
        events = operationEvents(record);
        return true;
    };

    QJsonArray events = operationEvents(object);
    if (operation.phase != OperationPhase::Conflict && !anyConflict && !anyUnknown
        && publishedCount > 0 && notPublishedCount == 0
        && acceptedWasRecorded && operation.phase != OperationPhase::ArtifactPublished
        && operation.phase != OperationPhase::Observed)
    {
        OperationAssociation publishedAssociation = association;
        publishedAssociation.diagnosticCode.clear();
        if (!persistPhase(OperationPhase::ArtifactPublished, publishedAssociation,
                          object, operation, events, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("invalid_transition"));
            return false;
        }
        targetPhase = OperationPhase::Observed;
        association = publishedAssociation;
    }
    if (!persistPhase(targetPhase, association, object, operation, events, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("invalid_transition"));
        return false;
    }
    // The phase event records the operation outcome. The per-file observations
    // are updated in the same atomic ledger publication.
    object = durableOperationJson(operation, operationEvents(object));
    QJsonArray storedArtifacts;
    for (const auto &artifact : operation.artifacts) storedArtifacts.append(operationArtifactJson(artifact));
    object.insert(QStringLiteral("artifacts"), storedArtifacts);
    operations[index] = object;
    if (!writeOperationLedger(root, projectIdentity, operations, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

bool RProjectCatalog::queueCloseRetry(const QString &retryId, const QString &operationId,
                                      const QString &diagnosticCode,
                                      const QJsonObject &payload,
                                      const PublicationLeasePtr &publicationLease,
                                      QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !validCloseRetryId(retryId)
        || !validUuid(operationId) || !validDiagnosticCode(diagnosticCode)
        || diagnosticCode.isEmpty()
        || !RModelCommandSession::validateCloseRetryPayload(payload, &localError)
        || payload.value(QStringLiteral("retryId")).toString() != retryId
        || payload.value(QStringLiteral("operationId")).toString() != operationId)
    {
        setError(error, localError.isEmpty()
            ? QStringLiteral("Close retry identity, payload, or diagnostic code is invalid.") : localError);
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return false;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return false;
        }
    }
    bool valid = false;
    QJsonArray records = readCloseRetryRecords(root, projectIdentity, localError, &valid);
    if (!valid)
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_close_queue"));
        return false;
    }
    for (const QJsonValue &value : records)
    {
        const QJsonObject record = value.toObject();
        if (record.value(QStringLiteral("retryId")).toString() != retryId) continue;
        if (record.value(QStringLiteral("operationId")).toString() == operationId
            && record.value(QStringLiteral("diagnosticCode")).toString() == diagnosticCode
            && record.value(QStringLiteral("payload")).toObject() == payload) return true;
        setError(error, QStringLiteral("Close retry ID is already associated with a different operation."));
        setErrorCode(errorCode, QStringLiteral("idempotency_conflict"));
        return false;
    }
    if (records.size() >= MaxCloseRetries)
    {
        setError(error, QStringLiteral("Durable close retry queue reached its record limit."));
        setErrorCode(errorCode, QStringLiteral("queue_limit"));
        return false;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    records.append(QJsonObject{{QStringLiteral("retryId"), retryId},
        {QStringLiteral("operationId"), operationId},
        {QStringLiteral("diagnosticCode"), diagnosticCode},
        {QStringLiteral("createdAtUtc"), timestampText(now)},
        {QStringLiteral("status"), QStringLiteral("pending")},
        {QStringLiteral("completedAtUtc"), QString()},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("payloadSha256"), QString::fromLatin1(
             QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                      QCryptographicHash::Sha256).toHex())}});
    if (!writeCloseRetryRecords(root, projectIdentity, records, localError))
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("io_error"));
        return false;
    }
    return true;
}

QVector<RProjectCatalog::CloseRetry> RProjectCatalog::pendingCloseRetries(
    const PublicationLeasePtr &publicationLease, QString *error, QString *errorCode) const
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QVector<CloseRetry> result;
    QString localError;
    if (QThread::currentThreadId() != ownerThread)
    {
        setError(error, QStringLiteral("Close retry queue must be read on the catalog owner thread."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return result;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return result;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return result;
        }
    }
    bool valid = false;
    const QJsonArray records = readCloseRetryRecords(root, projectIdentity, localError, &valid);
    if (!valid)
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_close_queue"));
        return result;
    }
    for (const QJsonValue &value : records)
    {
        const QJsonObject record = value.toObject();
        if (record.value(QStringLiteral("status")).toString() != QStringLiteral("pending")) continue;
        CloseRetry retry;
        retry.retryId = record.value(QStringLiteral("retryId")).toString();
        retry.operationId = record.value(QStringLiteral("operationId")).toString();
        retry.diagnosticCode = record.value(QStringLiteral("diagnosticCode")).toString();
        retry.status = record.value(QStringLiteral("status")).toString();
        retry.payload = record.value(QStringLiteral("payload")).toObject();
        parseTimestamp(record.value(QStringLiteral("createdAtUtc")), retry.createdAtUtc);
        result.append(std::move(retry));
    }
    return result;
}

bool RProjectCatalog::completeCloseRetry(const QString &retryId,
                                         const PublicationLeasePtr &publicationLease,
                                         QString *error, QString *errorCode)
{
    if (error) error->clear();
    if (errorCode) errorCode->clear();
    QString localError;
    if (QThread::currentThreadId() != ownerThread || !validCloseRetryId(retryId))
    {
        setError(error, QStringLiteral("Close retry ID or owner thread is invalid."));
        setErrorCode(errorCode, QStringLiteral("invalid_argument"));
        return false;
    }
    std::unique_ptr<QLockFile> directLock;
    if (publicationLease)
    {
        if (!validatePublicationLease(publicationLease, error, errorCode)) return false;
    }
    else
    {
        directLock = acquireLock(root, localError, errorCode);
        if (!directLock)
        {
            setError(error, localError);
            return false;
        }
    }
    bool valid = false;
    QJsonArray records = readCloseRetryRecords(root, projectIdentity, localError, &valid);
    if (!valid)
    {
        setError(error, localError);
        setErrorCode(errorCode, QStringLiteral("corrupt_close_queue"));
        return false;
    }
    for (qsizetype index = 0; index < records.size(); ++index)
    {
        QJsonObject record = records.at(index).toObject();
        if (record.value(QStringLiteral("retryId")).toString() != retryId) continue;
        if (record.value(QStringLiteral("status")).toString() == QStringLiteral("complete")) return true;
        record.insert(QStringLiteral("status"), QStringLiteral("complete"));
        record.insert(QStringLiteral("completedAtUtc"), timestampText(QDateTime::currentDateTimeUtc()));
        records[index] = record;
        if (!writeCloseRetryRecords(root, projectIdentity, records, localError))
        {
            setError(error, localError);
            setErrorCode(errorCode, QStringLiteral("io_error"));
            return false;
        }
        return true;
    }
    setError(error, QStringLiteral("Close retry item was not found."));
    setErrorCode(errorCode, QStringLiteral("not_found"));
    return false;
}
