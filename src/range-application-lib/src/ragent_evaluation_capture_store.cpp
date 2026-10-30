#include <ragent_evaluation_capture_store.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
constexpr auto RecordSchema = "range.agent-evaluation-capture/v1";
constexpr auto GenesisHash = "0000000000000000000000000000000000000000000000000000000000000000";

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool containsForbiddenKey(const QJsonValue &value, int depth = 0)
{
    if (depth > 64) return true;
    static const QSet<QString> forbiddenKeys = {
        QStringLiteral("apikey"),
        QStringLiteral("api_key"),
        QStringLiteral("authorization"),
        QStringLiteral("credentials"),
        QStringLiteral("password"),
        QStringLiteral("reasoning_content"),
        QStringLiteral("messages"),
        QStringLiteral("requestbody"),
        QStringLiteral("responsebody"),
        QStringLiteral("argumentsjson"),
        // Reject common names used for full prompt or raw response fields as
        // well. Content hashes and the sanitized argumentsJson summary remain
        // valid event fields.
        QStringLiteral("prompt"),
        QStringLiteral("fullprompt"),
        QStringLiteral("prompttext"),
        QStringLiteral("rawprompt"),
        QStringLiteral("systemprompt"),
        QStringLiteral("userprompt"),
        QStringLiteral("response"),
        QStringLiteral("rawresponse"),
        QStringLiteral("responsetext"),
        QStringLiteral("rawresponsetext")
    };

    if (value.isObject())
    {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        {
            if (forbiddenKeys.contains(it.key().toCaseFolded()) || containsForbiddenKey(it.value(), depth + 1))
                return true;
        }
    }
    else if (value.isArray())
    {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &item : array)
            if (containsForbiddenKey(item, depth + 1)) return true;
    }
    return false;
}

bool hasReparsePointInPath(const QString &path)
{
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    while (!current.isEmpty())
    {
        const QFileInfo component(current);
        if (component.isSymLink() || component.isJunction()) return true;
#ifdef Q_OS_WIN
        QString nativePath = QDir::toNativeSeparators(current);
        if (!nativePath.startsWith(QStringLiteral("\\\\?\\")))
        {
            if (nativePath.startsWith(QStringLiteral("\\\\")))
                nativePath = QStringLiteral("\\\\?\\UNC\\") + nativePath.mid(2);
            else
                nativePath = QStringLiteral("\\\\?\\") + nativePath;
        }
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        if (attributes == INVALID_FILE_ATTRIBUTES && component.exists()) return true;
#endif
        const QString parent = QDir::cleanPath(component.dir().absolutePath());
        if (parent == current) break;
        current = parent;
    }
    return false;
}

bool readLedger(const QString &path, QByteArray *bytes, QString *error)
{
    if (hasReparsePointInPath(path))
        return fail(error, QStringLiteral("The capture path contains a symbolic link or directory junction."));
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink())
        return fail(error, QStringLiteral("The capture ledger is missing or is not a regular file."));
    if (info.size() < 0 || info.size() > RAgentEvaluationCaptureStore::MaxTotalBytes)
        return fail(error, QStringLiteral("The capture ledger exceeds its storage bound."));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("The capture ledger could not be read."));
    const QByteArray contents = file.read(RAgentEvaluationCaptureStore::MaxTotalBytes + 1);
    if (file.error() != QFileDevice::NoError
        || contents.size() > RAgentEvaluationCaptureStore::MaxTotalBytes
        || contents.size() != info.size())
        return fail(error, QStringLiteral("The capture ledger changed or could not be read within its bound."));
    if (bytes) *bytes = contents;
    return true;
}
}

bool RAgentEvaluationCaptureStore::configure(const QString &path, QString *error)
{
    if (error) error->clear();
    QMutexLocker guard(&mutex);
    if (configured)
        return fail(error, QStringLiteral("The capture store is already configured."));
    if (path.trimmed().isEmpty())
        return fail(error, QStringLiteral("An explicit capture ledger path is required."));

    const QString absolutePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (hasReparsePointInPath(absolutePath))
        return fail(error, QStringLiteral("The capture path contains a symbolic link or directory junction."));
    const QFileInfo targetInfo(absolutePath);
    if (targetInfo.exists() || targetInfo.isSymLink())
        return fail(error, QStringLiteral("The selected capture ledger already exists."));

    const QString parentPath = targetInfo.absolutePath();
    if (!QDir().mkpath(parentPath))
        return fail(error, QStringLiteral("The capture ledger directory could not be created."));
    if (hasReparsePointInPath(absolutePath))
        return fail(error, QStringLiteral("The capture path changed to a symbolic link or directory junction."));

    QLockFile lock(absolutePath + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(5000))
        return fail(error, QStringLiteral("The capture ledger is locked by another writer."));

    // Recheck after acquiring the interprocess lock. NewOnly supplies an
    // exclusive create, so an existing path can never be truncated here.
    const QFileInfo lockedTargetInfo(absolutePath);
    if (hasReparsePointInPath(absolutePath))
        return fail(error, QStringLiteral("The capture path changed to a symbolic link or directory junction."));
    if (lockedTargetInfo.exists() || lockedTargetInfo.isSymLink())
        return fail(error, QStringLiteral("The selected capture ledger already exists."));

    QFile genesis(absolutePath);
    if (!genesis.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail(error, QStringLiteral("A new capture ledger could not be created."));
    if (!genesis.flush() || genesis.error() != QFileDevice::NoError)
        return fail(error, QStringLiteral("The new capture ledger could not be initialized."));
    genesis.close();

    ledgerPath = absolutePath;
    expectedPrefix.clear();
    previousHash = QString::fromLatin1(GenesisHash);
    nextSequence = 1;
    eventCount = 0;
    configured = true;
    return true;
}

bool RAgentEvaluationCaptureStore::append(const QJsonObject &event, QString *error)
{
    if (error) error->clear();
    QMutexLocker guard(&mutex);
    if (!configured)
        return fail(error, QStringLiteral("The capture store is not configured."));
    const QByteArray eventBytes = QJsonDocument(event).toJson(QJsonDocument::Compact);
    if (eventBytes.size() > MaxEventBytes)
        return fail(error, QStringLiteral("The event exceeds its size bound."));
    if (containsForbiddenKey(QJsonValue(event)))
        return fail(error, QStringLiteral("The event contains a field that cannot be captured."));
    if (eventCount >= MaxEvents)
        return fail(error, QStringLiteral("The capture ledger reached its event limit."));
    if (hasReparsePointInPath(ledgerPath))
        return fail(error, QStringLiteral("The capture path contains a symbolic link or directory junction."));

    QLockFile lock(ledgerPath + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(5000))
        return fail(error, QStringLiteral("The capture ledger is locked by another writer."));
    if (hasReparsePointInPath(ledgerPath))
        return fail(error, QStringLiteral("The capture path changed to a symbolic link or directory junction."));

    QByteArray currentPrefix;
    if (!readLedger(ledgerPath, &currentPrefix, error)) return false;
    if (currentPrefix != expectedPrefix)
        return fail(error, QStringLiteral("The capture ledger changed outside this store; append refused."));

    QJsonObject record{{QStringLiteral("schema"), QString::fromLatin1(RecordSchema)},
                       {QStringLiteral("sequence"), nextSequence},
                       {QStringLiteral("previousHash"), previousHash},
                       {QStringLiteral("payload"), event}};
    const QByteArray canonicalRecord = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const QByteArray entryHash = QCryptographicHash::hash(canonicalRecord, QCryptographicHash::Sha256).toHex();
    record.insert(QStringLiteral("entryHash"),
                  QString::fromLatin1(entryHash.constData(), entryHash.size()));
    const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
    if (expectedPrefix.size() + line.size() > MaxTotalBytes)
        return fail(error, QStringLiteral("The capture ledger reached its byte limit."));

    const QByteArray replacement = expectedPrefix + line;
    QFile ledger(ledgerPath);
    if (!ledger.open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Unbuffered))
        return fail(error, QStringLiteral("The capture ledger could not be opened for append."));

    // Append in place so a writer that does not honor the lock file can never
    // have its bytes silently replaced by a staged full-file rewrite.
    if (!ledger.seek(0))
        return fail(error, QStringLiteral("The capture ledger could not be rechecked before append."));
    const QByteArray latestPrefix = ledger.readAll();
    if (ledger.error() != QFileDevice::NoError || latestPrefix != expectedPrefix)
        return fail(error, QStringLiteral("The capture ledger changed outside this store; append refused."));
    if (ledger.write(line) != line.size() || !ledger.flush())
        return fail(error, QStringLiteral("The capture ledger append was incomplete."));
    ledger.close();

    QByteArray committedBytes;
    if (!readLedger(ledgerPath, &committedBytes, error) || committedBytes != replacement)
        return fail(error, QStringLiteral("The committed capture ledger could not be verified."));

    expectedPrefix = replacement;
    previousHash = QString::fromLatin1(entryHash.constData(), entryHash.size());
    ++nextSequence;
    ++eventCount;
    return true;
}
