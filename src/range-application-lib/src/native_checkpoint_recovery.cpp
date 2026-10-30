#include "native_checkpoint_recovery.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace NativeCheckpointRecovery
{
struct PlanAccess
{
    static Plan make(const QString &directory, const QByteArray &contextJson, const QString &token,
                     const QByteArray &marker, bool latestExists, const QByteArray &latest,
                     bool lastValidExists, const QByteArray &lastValid, bool traceMode,
                     const PiezoAcceptedTrace::Cursor &cursor, const QString &traceHeaderHash,
                     quint64 candidateSequence, const QString &contextHash)
    {
        Plan plan;
        plan.checkpointDirectory = directory;
        plan.contextJson = contextJson;
        plan.stableToken = token;
        plan.pendingMarkerBytes = marker;
        plan.hadLatest = latestExists;
        plan.oldLatestBytes = latest;
        plan.hadLastValid = lastValidExists;
        plan.oldLastValidBytes = lastValid;
        plan.trace = traceMode;
        plan.traceCursor = cursor;
        plan.traceHeaderHash = traceHeaderHash;
        plan.candidateSeq = candidateSequence;
        plan.contextHash = contextHash;
        return plan;
    }

    static const QByteArray &contextJson(const Plan &plan) { return plan.contextJson; }
};

namespace
{
constexpr auto markerName = "pending-publication.json";
constexpr auto traceName = "accepted-trace.rpt";
constexpr qsizetype maxMetadataBytes = 64 * 1024;
constexpr qint64 maxModelBytes = 1024ll * 1024ll * 1024ll;

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toUtf8().constData());
}

QByteArray digest(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
}

QString hexDigest(const QByteArray &bytes)
{
    return QString::fromLatin1(digest(bytes).toHex());
}

bool isLowerHexSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar c : value)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    return true;
}

bool safeBasename(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
           QFileInfo(name).fileName() == name && !name.contains(QLatin1Char('/')) &&
           !name.contains(QLatin1Char('\\'));
}

void requireNoSymlinkComponents(const QString &absolutePath)
{
    const QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(absolutePath));
    QString root, remainder;
    if (normalized.startsWith(QStringLiteral("//")))
    {
        QStringList parts = normalized.mid(2).split(QLatin1Char('/'),Qt::SkipEmptyParts);
        if (parts.size() < 2) fail(QStringLiteral("Checkpoint UNC path has no server/share root."));
        const QString server = parts.takeFirst(), share = parts.takeFirst();
        root = QStringLiteral("//%1/%2/").arg(server,share);
        remainder = parts.join(QLatin1Char('/'));
    }
    else if (normalized.size() >= 3 && normalized[0].isLetter() &&
             normalized[1] == QLatin1Char(':') && normalized[2] == QLatin1Char('/'))
    {
        root = normalized.left(3);
        remainder = normalized.mid(3);
    }
    else if (normalized.startsWith(QLatin1Char('/')))
    {
        root = QStringLiteral("/");
        remainder = normalized.mid(1);
    }
    else fail(QStringLiteral("Checkpoint directory has no absolute filesystem root."));

    QString current = root;
    for (const QString &component : remainder.split(QLatin1Char('/'),Qt::SkipEmptyParts))
    {
        current = QDir(current).filePath(component);
        if (QFileInfo(current).isSymLink())
            fail(QStringLiteral("Checkpoint path contains a symbolic-link component: '%1'.").arg(current));
    }
}

QString canonicalCheckpointDirectory(const QString &directory)
{
    const QFileInfo requested(directory);
    if (!requested.isAbsolute()) fail(QStringLiteral("Checkpoint directory must be absolute."));
    const QString absolute = QDir::cleanPath(requested.absoluteFilePath());
    requireNoSymlinkComponents(absolute);
    QFileInfo canonical(absolute);
    if (!canonical.isDir() || canonical.isSymLink() || canonical.canonicalFilePath().isEmpty())
        fail(QStringLiteral("Checkpoint directory is not a regular directory."));
    const QString result = QDir::cleanPath(canonical.canonicalFilePath());
    requireNoSymlinkComponents(result);
    return result;
}

#ifdef Q_OS_WIN
QString nativeExtendedPath(const QString &value)
{
    auto native = QDir::toNativeSeparators(value);
    if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
    native = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(value).absoluteFilePath()));
    if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    if (native.size() >= 3 && native[0].isLetter() && native[1] == QLatin1Char(':') &&
        native[2] == QLatin1Char('\\'))
        return QStringLiteral("\\\\?\\") + native;
    fail(QStringLiteral("Checkpoint lock path must be absolute."));
}
#endif

QByteArray readRegularFile(const QString &path, qsizetype maximum = -1)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink())
        fail(QStringLiteral("Checkpoint artifact is not a regular file: '%1'.").arg(path));
    if (maximum >= 0 && info.size() > maximum)
        fail(QStringLiteral("Checkpoint metadata exceeds its byte limit: '%1'.").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Cannot read checkpoint artifact '%1': %2.").arg(path,file.errorString()));
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot finish reading checkpoint artifact '%1': %2.").arg(path,file.errorString()));
    return bytes;
}

QJsonObject parseCanonicalObject(const QByteArray &bytes, const QString &what)
{
    if (bytes.isEmpty() || bytes.size() > maxMetadataBytes)
        fail(QStringLiteral("%1 must contain 1..%2 bytes.").arg(what).arg(maxMetadataBytes));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes,&error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("%1 is not a valid JSON object: %2.").arg(what,error.errorString()));
    const QJsonObject object = document.object();
    if (QJsonDocument(object).toJson(QJsonDocument::Compact) != bytes)
        fail(QStringLiteral("%1 is not canonical JSON.").arg(what));
    return object;
}

void requireOnlyKeys(const QJsonObject &object, QStringList expected, const QString &what)
{
    QStringList actual = object.keys();
    std::sort(actual.begin(),actual.end());
    std::sort(expected.begin(),expected.end());
    if (actual != expected) fail(QStringLiteral("%1 has missing or unknown fields.").arg(what));
}

quint64 integerField(const QJsonObject &object, const QString &key, quint64 maximum,
                     bool positive = false)
{
    const QJsonValue value = object.value(key);
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || !std::isfinite(number) || number < (positive ? 1.0 : 0.0) ||
        std::floor(number) != number || number > double(maximum))
        fail(QStringLiteral("Recovery integer field '%1' is invalid.").arg(key));
    return quint64(number);
}

QString contextDigest(const QByteArray &canonicalContextJson, QJsonObject &context)
{
    context = parseCanonicalObject(canonicalContextJson,QStringLiteral("Native checkpoint context"));
    requireOnlyKeys(context,{"schema","sourceSha256","modelSha256","controlSha256","runtimeSha256",
                             "packageSha256","policy","maxAcceptedSteps","maxSimulationSeconds","dtS"},
                    QStringLiteral("Native checkpoint context"));
    if (context.value(QStringLiteral("schema")).toString() != QStringLiteral("range.native-checkpoint-context/v1"))
        fail(QStringLiteral("Recovery context schema is unsupported."));
    for (const auto &key : {QStringLiteral("sourceSha256"),QStringLiteral("modelSha256"),
                            QStringLiteral("controlSha256"),QStringLiteral("runtimeSha256"),
                            QStringLiteral("packageSha256")})
        if (!isLowerHexSha256(context.value(key).toString()))
            fail(QStringLiteral("Recovery context hash '%1' is invalid.").arg(key));
    const QJsonValue steps = context.value(QStringLiteral("maxAcceptedSteps"));
    const QJsonValue seconds = context.value(QStringLiteral("maxSimulationSeconds"));
    const QJsonValue dt = context.value(QStringLiteral("dtS"));
    if (!steps.isDouble() || steps.toDouble() != 10.0 || !seconds.isDouble() || seconds.toDouble() != 1.0 ||
        !dt.isDouble() || !std::isfinite(dt.toDouble()) || dt.toDouble() <= 0.0 || dt.toDouble() > 1.0 ||
        context.value(QStringLiteral("policy")).toString() != QStringLiteral("accepted-steps-or-sim-time-v1"))
        fail(QStringLiteral("Recovery context differs from the frozen native checkpoint bounds."));
    return hexDigest(QJsonDocument(context).toJson(QJsonDocument::Compact));
}

QByteArray sha256File(const QString &path)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() > maxModelBytes)
        fail(QStringLiteral("Recovery MODEL is not a regular file of at most 1 GiB: '%1'.").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Cannot read recovery MODEL '%1': %2.").arg(path,file.errorString()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot hash recovery MODEL '%1': %2.").arg(path,file.errorString()));
    return hash.result();
}

QString tokenFromManifestName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^rollback-evidence-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\.json$"));
    const auto match = pattern.match(name);
    if (!safeBasename(name) || !match.hasMatch())
        fail(QStringLiteral("Rollback manifest path is invalid."));
    return match.captured(1);
}

PiezoAcceptedTrace::Cursor parseCursor(const QJsonObject &object)
{
    requireOnlyKeys(object,{"byteEnd","lastRecordOffset","sequence","recordSha256","stateSha256"},
                    QStringLiteral("Rollback trace cursor"));
    PiezoAcceptedTrace::Cursor cursor;
    cursor.byteEnd = qint64(integerField(object,QStringLiteral("byteEnd"),PiezoAcceptedTrace::MaxFileBytes));
    const double offset = object.value(QStringLiteral("lastRecordOffset")).toDouble(-2.0);
    if (!object.value(QStringLiteral("lastRecordOffset")).isDouble() || !std::isfinite(offset) ||
        std::floor(offset) != offset || offset < -1.0 || offset > double(cursor.byteEnd))
        fail(QStringLiteral("Rollback trace cursor offset is invalid."));
    cursor.lastRecordOffset = qint64(offset);
    cursor.sequence = integerField(object,QStringLiteral("sequence"),std::numeric_limits<uint>::max());
    for (const auto &key : {QStringLiteral("recordSha256"),QStringLiteral("stateSha256")})
        if (!isLowerHexSha256(object.value(key).toString()))
            fail(QStringLiteral("Rollback trace cursor hash is invalid."));
    cursor.recordSha32 = QByteArray::fromHex(object.value(QStringLiteral("recordSha256")).toString().toLatin1());
    cursor.stateSha32 = QByteArray::fromHex(object.value(QStringLiteral("stateSha256")).toString().toLatin1());
    return cursor;
}

QByteArray pointerBytesAt(const QString &directory, const QString &name, bool &exists)
{
    const QString path = QDir(directory).filePath(name);
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile()))
        fail(QStringLiteral("Recovery pointer is not a regular file: '%1'.").arg(path));
    exists = info.exists();
    return exists ? readRegularFile(path,maxMetadataBytes) : QByteArray{};
}

void writeAtomic(const QString &path, const QByteArray &bytes)
{
    constexpr int maxAttempts =
#ifdef Q_OS_WIN
        6;
#else
        1;
#endif
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        requireNoSymlinkComponents(QFileInfo(path).absolutePath());
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile()))
            fail(QStringLiteral("Recovery refuses a non-regular pointer target: '%1'.").arg(path));
        QString commitError;
        bool retryableRename = false;
        {
            QSaveFile file(path);
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly))
                fail(QStringLiteral("Cannot stage recovered pointer '%1': %2.").arg(path,file.errorString()));
            if (file.write(bytes) != bytes.size())
            {
                const QString error = file.errorString();
                file.cancelWriting();
                fail(QStringLiteral("Cannot write recovered pointer '%1': %2.").arg(path,error));
            }
            if (file.commit()) return;
            commitError = file.errorString();
#ifdef Q_OS_WIN
            retryableRename = file.error() == QFileDevice::RenameError;
#endif
            file.cancelWriting();
        }
#ifdef Q_OS_WIN
        if (retryableRename && attempt + 1 < maxAttempts)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5 * (1 << attempt)));
            continue;
        }
#endif
        fail(QStringLiteral("Cannot atomically restore pointer '%1': %2.").arg(path,commitError));
    }
}

void restorePointer(const QString &directory, const QString &name, bool exists,
                    const QByteArray &expected)
{
    const QString path = QDir(directory).filePath(name);
    requireNoSymlinkComponents(directory);
    const QFileInfo before(path);
    if (before.isSymLink() || (before.exists() && !before.isFile()))
        fail(QStringLiteral("Recovery refuses a non-regular pointer target: '%1'.").arg(path));
    if (exists)
    {
        writeAtomic(path,expected);
        if (readRegularFile(path,maxMetadataBytes) != expected)
            fail(QStringLiteral("Recovered pointer read-back differs from its snapshot."));
    }
    else
    {
        if (before.exists() && !QFile::remove(path))
            fail(QStringLiteral("Recovery cannot remove a pointer that was originally absent."));
        const QFileInfo after(path);
        if (after.exists() || after.isSymLink())
            fail(QStringLiteral("Recovery did not restore pointer absence."));
    }
}

struct PointerView
{
    quint64 sequence = 0;
    QByteArray traceHeaderHash;
    PiezoAcceptedTrace::Cursor traceCursor;
    QString envelopeFile;
};

PointerView validatePointer(const QByteArray &pointerBytes, const QString &directory,
                            const QJsonObject &context, const QString &expectedContextHash,
                            bool traceMode, const QString &traceHeaderHash,
                            quint64 candidateSequence, const QString &transactionToken,
                            bool candidatePointer, const PiezoAcceptedTrace::Header *traceHeader)
{
    const QJsonObject pointer = parseCanonicalObject(pointerBytes,QStringLiteral("Checkpoint pointer"));
    requireOnlyKeys(pointer,{"schema","envelopeFile","envelopeSha256"},QStringLiteral("Checkpoint pointer"));
    const QString expectedPointerSchema = traceMode ? QStringLiteral("range.native-checkpoint-pointer/v2") :
                                                       QStringLiteral("range.native-checkpoint-pointer/v1");
    if (pointer.value(QStringLiteral("schema")).toString() != expectedPointerSchema)
        fail(QStringLiteral("Checkpoint pointer schema differs from the pending transaction."));
    const QString envelopeFile = pointer.value(QStringLiteral("envelopeFile")).toString();
    const QString envelopeHash = pointer.value(QStringLiteral("envelopeSha256")).toString();
    if (!safeBasename(envelopeFile) || !isLowerHexSha256(envelopeHash))
        fail(QStringLiteral("Checkpoint pointer envelope reference is invalid."));
    if (candidatePointer)
    {
        const QString expectedName = QStringLiteral("envelope-%1-%2.json")
            .arg(candidateSequence,10,10,QLatin1Char('0')).arg(transactionToken);
        if (envelopeFile != expectedName)
            fail(QStringLiteral("Candidate pointer does not reference this pending transaction."));
    }
    const QByteArray envelopeBytes = readRegularFile(QDir(directory).filePath(envelopeFile),maxMetadataBytes);
    if (hexDigest(envelopeBytes) != envelopeHash)
        fail(QStringLiteral("Checkpoint envelope hash does not match its pointer."));
    const QJsonObject envelope = parseCanonicalObject(envelopeBytes,QStringLiteral("Checkpoint envelope"));

    QJsonObject expected = context;
    expected.remove(QStringLiteral("schema"));
    expected.remove(QStringLiteral("modelSha256"));
    expected[QStringLiteral("schema")] = traceMode ? QStringLiteral("range.native-checkpoint/v2") :
                                                       QStringLiteral("range.native-checkpoint/v1");
    expected[QStringLiteral("inputModelSha256")] = context.value(QStringLiteral("modelSha256"));
    QStringList fields = expected.keys();
    fields << QStringLiteral("acceptedSeq") << QStringLiteral("timeS")
           << QStringLiteral("modelFile") << QStringLiteral("checkpointModelSha256");
    if (traceMode)
        fields << QStringLiteral("modelAcceptedSeq") << QStringLiteral("stateFingerprintSha256")
               << QStringLiteral("traceFile") << QStringLiteral("traceHeaderSha256") << QStringLiteral("traceCursor");
    requireOnlyKeys(envelope,fields,QStringLiteral("Checkpoint envelope"));
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (envelope.value(it.key()) != it.value())
            fail(QStringLiteral("Checkpoint envelope differs from the supplied context."));

    const quint64 sequence = integerField(envelope,QStringLiteral("acceptedSeq"),std::numeric_limits<uint>::max(),!traceMode);
    if ((candidatePointer && sequence != candidateSequence) || (!candidatePointer && sequence >= candidateSequence))
        fail(QStringLiteral("Checkpoint pointer sequence is outside the pending transaction."));
    const QJsonValue timeValue = envelope.value(QStringLiteral("timeS"));
    const QJsonValue dtValue = envelope.value(QStringLiteral("dtS"));
    if (!timeValue.isDouble() || !std::isfinite(timeValue.toDouble()) || !dtValue.isDouble() ||
        dtValue.toDouble() != context.value(QStringLiteral("dtS")).toDouble())
        fail(QStringLiteral("Checkpoint envelope clock is invalid."));

    const QString modelFile = envelope.value(QStringLiteral("modelFile")).toString();
    const QString modelHash = envelope.value(QStringLiteral("checkpointModelSha256")).toString();
    if (!safeBasename(modelFile) || !modelFile.endsWith(QStringLiteral(".rbm")) || !isLowerHexSha256(modelHash))
        fail(QStringLiteral("Checkpoint envelope MODEL reference is invalid."));
    if (candidatePointer && !traceMode)
    {
        const QString expectedModel = QStringLiteral("checkpoint-%1-%2.rbm")
            .arg(candidateSequence,10,10,QLatin1Char('0')).arg(transactionToken);
        if (modelFile != expectedModel)
            fail(QStringLiteral("Candidate pointer does not reference this pending MODEL."));
    }
    const QString modelPath = QDir(directory).filePath(modelFile);
    if (QString::fromLatin1(sha256File(modelPath).toHex()) != modelHash)
        fail(QStringLiteral("Checkpoint envelope MODEL hash is invalid."));

    PointerView result;
    result.sequence = sequence;
    result.envelopeFile = envelopeFile;
    if (traceMode)
    {
        if (!isLowerHexSha256(envelope.value(QStringLiteral("stateFingerprintSha256")).toString()) ||
            envelope.value(QStringLiteral("traceFile")).toString() != QString::fromLatin1(traceName) ||
            envelope.value(QStringLiteral("traceHeaderSha256")).toString() != traceHeaderHash)
            fail(QStringLiteral("Trace envelope header or state identity is invalid."));
        const quint64 modelSequence = integerField(envelope,QStringLiteral("modelAcceptedSeq"),sequence);
        if (modelSequence > sequence) fail(QStringLiteral("Trace full MODEL sequence exceeds accepted sequence."));
        result.traceHeaderHash = QByteArray::fromHex(traceHeaderHash.toLatin1());
        result.traceCursor = parseCursor(envelope.value(QStringLiteral("traceCursor")).toObject());
        if (result.traceCursor.sequence != sequence ||
            result.traceCursor.stateSha32 != QByteArray::fromHex(envelope.value(QStringLiteral("stateFingerprintSha256")).toString().toLatin1()))
            fail(QStringLiteral("Trace pointer cursor differs from its accepted state."));
        if (traceHeader)
        {
            if (traceHeader->headerSha32 != result.traceHeaderHash || result.traceCursor.sequence < traceHeader->baseSeq ||
                result.traceCursor.sequence > traceHeader->targetSteps ||
                !std::isfinite(traceHeader->initialTime + double(sequence) * traceHeader->dt) ||
                timeValue.toDouble() != traceHeader->initialTime + double(sequence) * traceHeader->dt)
                fail(QStringLiteral("Trace envelope clock or header differs from its trace."));
        }
        if (candidatePointer && result.traceCursor.sequence != candidateSequence)
            fail(QStringLiteral("Candidate trace pointer does not identify its candidate sequence."));
    }
    Q_UNUSED(expectedContextHash);
    return result;
}

void setError(QString *error, const QString &message)
{
    if (error) *error = message;
}

void requireLease(const WriterLease &lease, const QString &directory)
{
    if (!lease.ownsLease() || lease.directory() != directory)
        fail(QStringLiteral("Recovery plan requires the active writer lease for its checkpoint directory."));
}

QByteArray transactionTokenBytes(const QByteArray &marker, const QByteArray &manifest,
                                 const QByteArray &latest, const QByteArray &lastValid)
{
    QByteArray material = QByteArrayLiteral("range.native-checkpoint-recovery-plan/v1");
    const auto append = [&material](const QByteArray &part) {
        quint64 size = quint64(part.size());
        for (int i = 7; i >= 0; --i) material.append(char((size >> (8 * i)) & 0xff));
        material.append(part);
    };
    append(marker); append(manifest); append(latest); append(lastValid);
    return digest(material);
}

bool sameCursor(const PiezoAcceptedTrace::Cursor &a, const PiezoAcceptedTrace::Cursor &b)
{
    return a.byteEnd == b.byteEnd && a.lastRecordOffset == b.lastRecordOffset &&
           a.sequence == b.sequence && a.recordSha32 == b.recordSha32 && a.stateSha32 == b.stateSha32;
}

struct PlanData
{
    QString directory;
    QByteArray contextJson;
    QString stableToken;
    QByteArray pendingMarkerBytes;
    bool hadLatest = false;
    QByteArray oldLatestBytes;
    bool hadLastValid = false;
    QByteArray oldLastValidBytes;
    bool trace = false;
    PiezoAcceptedTrace::Cursor traceCursor;
    QString traceHeaderHash;
    quint64 candidateSeq = 0;
    QString contextHash;
};

struct Inspected
{
    PlanData data;
    QByteArray manifestBytes;
};

Inspected inspectImpl(const QString &directory, const QByteArray &contextJson)
{
    Inspected result;
    result.data.directory = canonicalCheckpointDirectory(directory);
    result.data.contextJson = contextJson;
    QJsonObject context;
    result.data.contextHash = contextDigest(contextJson,context);
    const QString markerPath = QDir(result.data.directory).filePath(QString::fromLatin1(markerName));
    const QFileInfo markerInfo(markerPath);
    if (markerInfo.isSymLink()) fail(QStringLiteral("Pending checkpoint marker cannot be a symbolic link."));
    if (!markerInfo.exists())
    {
        result.data.stableToken = QString();
        return result;
    }
    if (!markerInfo.isFile()) fail(QStringLiteral("Pending checkpoint marker is not a regular file."));
    result.data.pendingMarkerBytes = readRegularFile(markerPath,maxMetadataBytes);
    const QJsonObject marker = parseCanonicalObject(result.data.pendingMarkerBytes,QStringLiteral("Pending checkpoint marker"));
    const QString schema = marker.value(QStringLiteral("schema")).toString();
    const bool traceMode = schema == QStringLiteral("range.native-checkpoint-pending-publication/v2");
    if (!traceMode && schema != QStringLiteral("range.native-checkpoint-pending-publication/v1"))
        fail(QStringLiteral("Pending checkpoint marker schema is unsupported."));
    requireOnlyKeys(marker,{"schema","candidateSeq","contextSha256","rollbackManifestFile","rollbackManifestSha256"},
                    QStringLiteral("Pending checkpoint marker"));
    result.data.candidateSeq = integerField(marker,QStringLiteral("candidateSeq"),std::numeric_limits<uint>::max(),true);
    if (marker.value(QStringLiteral("contextSha256")).toString() != result.data.contextHash)
        fail(QStringLiteral("Pending checkpoint marker context differs from the supplied context."));
    const QString manifestName = marker.value(QStringLiteral("rollbackManifestFile")).toString();
    const QString token = tokenFromManifestName(manifestName);
    const QString manifestHash = marker.value(QStringLiteral("rollbackManifestSha256")).toString();
    if (!isLowerHexSha256(manifestHash)) fail(QStringLiteral("Pending rollback manifest hash is invalid."));
    result.manifestBytes = readRegularFile(QDir(result.data.directory).filePath(manifestName),maxMetadataBytes);
    if (hexDigest(result.manifestBytes) != manifestHash)
        fail(QStringLiteral("Pending rollback manifest hash does not match its marker."));
    const QJsonObject manifest = parseCanonicalObject(result.manifestBytes,QStringLiteral("Rollback evidence manifest"));
    result.data.trace = traceMode;
    QStringList manifestFields{"schema","candidateSeq","sourceSha256","contextSha256","latest","lastvalid"};
    if (traceMode) manifestFields << "traceFile" << "traceHeaderSha256" << "oldTraceCursor";
    requireOnlyKeys(manifest,manifestFields,QStringLiteral("Rollback evidence manifest"));
    const QString expectedManifestSchema = traceMode ? QStringLiteral("range.native-checkpoint-rollback-evidence/v2") :
                                                       QStringLiteral("range.native-checkpoint-rollback-evidence/v1");
    if (manifest.value(QStringLiteral("schema")).toString() != expectedManifestSchema ||
        integerField(manifest,QStringLiteral("candidateSeq"),std::numeric_limits<uint>::max(),true) != result.data.candidateSeq ||
        manifest.value(QStringLiteral("sourceSha256")).toString() != context.value(QStringLiteral("sourceSha256")).toString() ||
        manifest.value(QStringLiteral("contextSha256")).toString() != result.data.contextHash)
        fail(QStringLiteral("Rollback manifest is not bound to the pending context and sequence."));

    const auto loadBackup = [&](const QString &key, bool &exists, QByteArray &bytes) {
        const QJsonObject descriptor = manifest.value(key).toObject();
        if (manifest.value(key).type() != QJsonValue::Object)
            fail(QStringLiteral("Rollback pointer descriptor is invalid."));
        const bool hasFile = descriptor.contains(QStringLiteral("file"));
        const bool hasHash = descriptor.contains(QStringLiteral("sha256"));
        exists = descriptor.value(QStringLiteral("exists")).toBool(false);
        requireOnlyKeys(descriptor,exists ? QStringList{"exists","file","sha256"} : QStringList{"exists"},
                        QStringLiteral("Rollback pointer descriptor"));
        if (descriptor.value(QStringLiteral("exists")).type() != QJsonValue::Bool || hasFile != exists || hasHash != exists)
            fail(QStringLiteral("Rollback pointer descriptor existence proof is inconsistent."));
        if (!exists) return;
        const QString file = descriptor.value(QStringLiteral("file")).toString();
        const QString expectedName = QStringLiteral("rollback-%1-%2.pointer").arg(token,key);
        const QString hash = descriptor.value(QStringLiteral("sha256")).toString();
        if (file != expectedName || !safeBasename(file) || !isLowerHexSha256(hash))
            fail(QStringLiteral("Rollback pointer evidence path or hash is invalid."));
        bytes = readRegularFile(QDir(result.data.directory).filePath(file),maxMetadataBytes);
        if (hexDigest(bytes) != hash) fail(QStringLiteral("Rollback pointer evidence hash is invalid."));
    };
    loadBackup(QStringLiteral("latest"),result.data.hadLatest,result.data.oldLatestBytes);
    loadBackup(QStringLiteral("lastvalid"),result.data.hadLastValid,result.data.oldLastValidBytes);
    if (!result.data.hadLatest && result.data.hadLastValid)
        fail(QStringLiteral("Rollback evidence has lastvalid without its latest pointer."));

    const QString tracePath = QDir(result.data.directory).filePath(QString::fromLatin1(traceName));
    std::optional<PiezoAcceptedTrace::Header> traceHeader;
    if (traceMode)
    {
        if (!result.data.hadLatest)
            fail(QStringLiteral("Interrupted trace baseline has no prior latest pointer; automatic recovery is safely unavailable."));
        result.data.traceHeaderHash = manifest.value(QStringLiteral("traceHeaderSha256")).toString();
        if (!isLowerHexSha256(result.data.traceHeaderHash) ||
            manifest.value(QStringLiteral("traceFile")).toString() != QString::fromLatin1(traceName))
            fail(QStringLiteral("Rollback trace identity is invalid."));
        result.data.traceCursor = parseCursor(manifest.value(QStringLiteral("oldTraceCursor")).toObject());
        const QFileInfo traceInfo(tracePath);
        if (traceInfo.isSymLink() || !traceInfo.isFile())
            fail(QStringLiteral("Rollback trace is missing or is not a regular file."));
        QFile trace(tracePath);
        if (!trace.open(QIODevice::ReadOnly)) fail(QStringLiteral("Rollback trace cannot be opened."));
        const auto header = PiezoAcceptedTrace::readHeader(trace);
        if (header.headerSha32 != QByteArray::fromHex(result.data.traceHeaderHash.toLatin1()) ||
            header.contextSha256 != QByteArray::fromHex(result.data.contextHash.toLatin1()))
            fail(QStringLiteral("Rollback trace header differs from its evidence manifest."));
        const auto prefix = PiezoAcceptedTrace::verifyPrefix(trace,result.data.traceCursor,true);
        if (prefix.header.headerSha32 != header.headerSha32 || result.data.traceCursor.sequence >= result.data.candidateSeq)
            fail(QStringLiteral("Rollback trace prefix does not precede the pending candidate."));
        traceHeader = header;
    }

    const auto verifySnapshot = [&](bool exists, const QByteArray &bytes, bool candidate) {
        if (!exists) return PointerView{};
        return validatePointer(bytes,result.data.directory,context,result.data.contextHash,
                              traceMode,result.data.traceHeaderHash,result.data.candidateSeq,token,candidate,
                              traceHeader ? &*traceHeader : nullptr);
    };
    const PointerView oldLatest = verifySnapshot(result.data.hadLatest,result.data.oldLatestBytes,false);
    const PointerView oldLastValid = verifySnapshot(result.data.hadLastValid,result.data.oldLastValidBytes,false);
    if (result.data.hadLatest && oldLatest.sequence + 1 != result.data.candidateSeq)
        fail(QStringLiteral("Pending candidate sequence does not follow the old latest pointer."));
    if (result.data.hadLastValid && oldLastValid.sequence >= oldLatest.sequence)
        fail(QStringLiteral("Rollback lastvalid pointer is not older than the old latest pointer."));
    if (traceMode && (!sameCursor(oldLatest.traceCursor,result.data.traceCursor) ||
                      oldLatest.traceHeaderHash != QByteArray::fromHex(result.data.traceHeaderHash.toLatin1())))
        fail(QStringLiteral("Rollback trace cursor differs from the old latest pointer."));

    bool currentLatestExists = false, currentLastValidExists = false;
    const QByteArray currentLatest = pointerBytesAt(result.data.directory,QStringLiteral("latest.json"),currentLatestExists);
    const QByteArray currentLastValid = pointerBytesAt(result.data.directory,QStringLiteral("lastvalid.json"),currentLastValidExists);
    const bool latestIsOld = currentLatestExists == result.data.hadLatest &&
                             (!currentLatestExists || currentLatest == result.data.oldLatestBytes);
    const bool latestIsCandidate = currentLatestExists && !latestIsOld;
    const bool lastIsOld = currentLastValidExists == result.data.hadLastValid &&
                           (!currentLastValidExists || currentLastValid == result.data.oldLastValidBytes);
    const bool lastIsOldLatest = result.data.hadLatest && currentLastValidExists &&
                                 currentLastValid == result.data.oldLatestBytes;
    if (latestIsCandidate)
        (void)validatePointer(currentLatest,result.data.directory,context,result.data.contextHash,
                              traceMode,result.data.traceHeaderHash,result.data.candidateSeq,token,true,
                              traceHeader ? &*traceHeader : nullptr);
    if ((!latestIsOld && !latestIsCandidate) || (!lastIsOld && !lastIsOldLatest) ||
        (!result.data.hadLatest && currentLastValidExists))
        fail(QStringLiteral("Current checkpoint pointers are outside the pending transaction states."));

    // A process may die between either pointer restore or after trace truncation;
    // all such combinations still retain the marker and the bound old authority.
    result.data.stableToken = QString::fromLatin1(transactionTokenBytes(
        result.data.pendingMarkerBytes,result.manifestBytes,result.data.oldLatestBytes,
        result.data.oldLastValidBytes).toHex());

    if (readRegularFile(markerPath,maxMetadataBytes) != result.data.pendingMarkerBytes ||
        readRegularFile(QDir(result.data.directory).filePath(manifestName),maxMetadataBytes) != result.manifestBytes ||
        (result.data.hadLatest && readRegularFile(QDir(result.data.directory).filePath(
             QStringLiteral("rollback-%1-latest.pointer").arg(token)),maxMetadataBytes) != result.data.oldLatestBytes) ||
        (result.data.hadLastValid && readRegularFile(QDir(result.data.directory).filePath(
             QStringLiteral("rollback-%1-lastvalid.pointer").arg(token)),maxMetadataBytes) != result.data.oldLastValidBytes))
        fail(QStringLiteral("Recovery evidence changed during inspection."));
    return result;
}

bool inspectInto(const QString &directory, const QByteArray &context,
                 Plan *plan, bool *found, QString *error)
{
    try
    {
        auto result = inspectImpl(directory,context);
        if (result.data.stableToken.isEmpty())
        {
            if (found) *found = false;
            if (error) error->clear();
            return true;
        }
        if (plan)
            *plan = PlanAccess::make(result.data.directory,result.data.contextJson,result.data.stableToken,
                result.data.pendingMarkerBytes,result.data.hadLatest,result.data.oldLatestBytes,
                result.data.hadLastValid,result.data.oldLastValidBytes,result.data.trace,
                result.data.traceCursor,result.data.traceHeaderHash,result.data.candidateSeq,result.data.contextHash);
        if (found) *found = true;
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        if (found) *found = false;
        setError(error,QString::fromUtf8(exception.what()));
        return false;
    }
}

bool verifyRestoredState(const Plan &plan, const WriterLease &lease, QString *error)
{
    Plan fresh;
    bool found = false;
    if (!inspectInto(plan.directory(),PlanAccess::contextJson(plan),&fresh,&found,error) || !found)
    {
        if (error && error->isEmpty()) setError(error,QStringLiteral("Pending recovery marker disappeared before completion."));
        return false;
    }
    if (fresh.token() != plan.token())
    {
        setError(error,QStringLiteral("Pending recovery plan changed after inspection."));
        return false;
    }
    requireLease(lease,plan.directory());
    bool latestExists = false, lastValidExists = false;
    const auto latest = pointerBytesAt(plan.directory(),QStringLiteral("latest.json"),latestExists);
    const auto lastValid = pointerBytesAt(plan.directory(),QStringLiteral("lastvalid.json"),lastValidExists);
    if (latestExists != plan.latestExists() || latest != plan.latestBytes() ||
        lastValidExists != plan.lastValidExists() || lastValid != plan.lastValidBytes())
    {
        setError(error,QStringLiteral("Checkpoint pointers have not been restored to their bound snapshots."));
        return false;
    }
    if (plan.traceMode())
    {
        const QString tracePath = QDir(plan.directory()).filePath(QString::fromLatin1(traceName));
        QFile trace(tracePath);
        if (QFileInfo(tracePath).isSymLink() || !trace.open(QIODevice::ReadOnly) ||
            trace.size() != plan.oldTraceCursor().byteEnd)
        {
            setError(error,QStringLiteral("Trace has not been restored to its exact committed boundary."));
            return false;
        }
        const auto prefix = PiezoAcceptedTrace::verifyPrefix(trace,plan.oldTraceCursor());
        if (QString::fromLatin1(prefix.header.headerSha32.toHex()) != plan.traceHeaderSha256())
        {
            setError(error,QStringLiteral("Restored trace header identity differs from the recovery plan."));
            return false;
        }
    }
    return true;
}
}

WriterLease::WriterLease(const QString &directory)
    : canonicalDirectory(canonicalCheckpointDirectory(directory))
{
    const QString lockPath = QDir(this->canonicalDirectory).filePath(QStringLiteral(".native-checkpoint.lock"));
    if (QFileInfo(lockPath).isSymLink()) fail(QStringLiteral("Native checkpoint lock cannot be a symbolic link."));
#ifdef Q_OS_WIN
    this->lock = std::make_unique<QLockFile>(nativeExtendedPath(lockPath));
#else
    this->lock = std::make_unique<QLockFile>(lockPath);
#endif
    this->lock->setStaleLockTime(0);
    if (!this->lock->tryLock(0))
        fail(QStringLiteral("Native checkpoint directory is already locked or has a stale writer lock."));
}

WriterLease::~WriterLease() = default;
WriterLease::WriterLease(WriterLease &&) noexcept = default;
WriterLease &WriterLease::operator=(WriterLease &&) noexcept = default;
const QString &WriterLease::directory() const noexcept { return this->canonicalDirectory; }
bool WriterLease::ownsLease() const noexcept { return this->lock && this->lock->isLocked(); }

std::optional<Plan> Plan::inspect(const QString &directory, const QByteArray &canonicalContextJson,
                                  QString *error)
{
    Plan plan;
    bool found = false;
    QString localError;
    QString *targetError = error ? error : &localError;
    if (!inspectInto(directory,canonicalContextJson,&plan,&found,targetError))
    {
        if (!error) fail(localError.isEmpty() ? QStringLiteral("Pending recovery inspection failed.") : localError);
        return std::nullopt;
    }
    if (!found) return std::nullopt;
    return plan;
}

bool Plan::restore(const WriterLease &lease, QString *error) const
{
    try
    {
        requireLease(lease,this->checkpointDirectory);
        Plan current;
        QString inspectError;
        bool found = false;
        if (!inspectInto(this->checkpointDirectory,this->contextJson,&current,&found,&inspectError) || !found)
            fail(inspectError.isEmpty() ? QStringLiteral("Pending recovery marker is missing.") : inspectError);
        if (current.token() != this->stableToken)
            fail(QStringLiteral("Pending recovery plan changed after inspection."));

        if (this->trace)
        {
            const QString path = QDir(this->checkpointDirectory).filePath(QString::fromLatin1(traceName));
            requireNoSymlinkComponents(this->checkpointDirectory);
            QFile trace(path);
            if (QFileInfo(path).isSymLink() || !trace.open(QIODevice::ReadWrite))
                fail(QStringLiteral("Recovery trace cannot be opened for truncation."));
            const auto prefix = PiezoAcceptedTrace::verifyPrefix(trace,this->traceCursor,true);
            if (QString::fromLatin1(prefix.header.headerSha32.toHex()) != this->traceHeaderHash)
                fail(QStringLiteral("Recovery trace prefix header differs from the pending evidence."));
            if (trace.size() != this->traceCursor.byteEnd)
            {
                if (!trace.resize(this->traceCursor.byteEnd) || !trace.flush())
                    fail(QStringLiteral("Recovery could not truncate the uncommitted trace tail."));
            }
            const auto readback = PiezoAcceptedTrace::verifyPrefix(trace,this->traceCursor);
            if (QString::fromLatin1(readback.header.headerSha32.toHex()) != this->traceHeaderHash ||
                trace.size() != this->traceCursor.byteEnd)
                fail(QStringLiteral("Recovered trace prefix failed its exact read-back verification."));
        }

        // Restore latest before lastvalid; the marker remains the durable abort
        // barrier if the process stops between these two atomic replacements.
        restorePointer(this->checkpointDirectory,QStringLiteral("latest.json"),this->hadLatest,this->oldLatestBytes);
        Plan afterLatest;
        if (!inspectInto(this->checkpointDirectory,this->contextJson,&afterLatest,&found,&inspectError) || !found ||
            afterLatest.token() != this->stableToken)
            fail(inspectError.isEmpty() ? QStringLiteral("Recovery plan changed after latest restoration.") : inspectError);
        restorePointer(this->checkpointDirectory,QStringLiteral("lastvalid.json"),this->hadLastValid,this->oldLastValidBytes);
        if (!verifyRestoredState(*this,lease,&inspectError))
            fail(inspectError.isEmpty() ? QStringLiteral("Recovered checkpoint state failed verification.") : inspectError);
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        setError(error,QString::fromUtf8(exception.what()));
        return false;
    }
}

bool Plan::finish(const WriterLease &lease, QString *error) const
{
    try
    {
        requireLease(lease,this->checkpointDirectory);
        if (!verifyRestoredState(*this,lease,error)) return false;
        const QString markerPath = QDir(this->checkpointDirectory).filePath(QString::fromLatin1(markerName));
        if (readRegularFile(markerPath,maxMetadataBytes) != this->pendingMarkerBytes)
            fail(QStringLiteral("Refusing to remove a different pending recovery marker."));
        // This is intentionally the final filesystem operation.
        if (!QFile::remove(markerPath)) fail(QStringLiteral("Cannot remove the recovered pending marker."));
        if (error) error->clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        setError(error,QString::fromUtf8(exception.what()));
        return false;
    }
}

const QString &Plan::directory() const noexcept { return this->checkpointDirectory; }
const QString &Plan::token() const noexcept { return this->stableToken; }
const QByteArray &Plan::markerBytes() const noexcept { return this->pendingMarkerBytes; }
bool Plan::latestExists() const noexcept { return this->hadLatest; }
const QByteArray &Plan::latestBytes() const noexcept { return this->oldLatestBytes; }
bool Plan::lastValidExists() const noexcept { return this->hadLastValid; }
const QByteArray &Plan::lastValidBytes() const noexcept { return this->oldLastValidBytes; }
bool Plan::traceMode() const noexcept { return this->trace; }
const PiezoAcceptedTrace::Cursor &Plan::oldTraceCursor() const noexcept { return this->traceCursor; }
const QString &Plan::traceHeaderSha256() const noexcept { return this->traceHeaderHash; }
quint64 Plan::candidateSequence() const noexcept { return this->candidateSeq; }
const QString &Plan::contextSha256() const noexcept { return this->contextHash; }
}
