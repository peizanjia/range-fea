#include <rtrusted_experiment_service.h>

#include <rtrusted_mechanics_service.h>
#include <rtrusted_convergence_service.h>
#include <rtrusted_verification_service.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
const QString authorizationSchema = QStringLiteral("range.trusted-experiment-authorization/v1");
const QString reportSchema = QStringLiteral("range.verification-report/v1");
constexpr qsizetype maximumProblemSpecBytes = 64 * 1024;
constexpr qsizetype maximumEvidenceBytes = 64 * 1024;
constexpr qsizetype maximumReportBaseBytes = 64 * 1024;
constexpr qsizetype maximumSourceReferenceBytes = 4 * 1024;
constexpr qsizetype maximumResultRecordBytes = 256;
// Prevent unbounded event accumulation and reserve exactly this history size in preflight.
constexpr int maximumHistoryEvents = 96;
constexpr qsizetype maximumHistoryBytes = 384 * 1024;
constexpr int maximumBoundedIdentifierCodeUnits = 256;
constexpr int maximumHistoryTimestampCodeUnits = 32;
constexpr int maximumHistoryEventCodeUnits = 64;
constexpr int maximumHistoryStateCodeUnits = 32;

bool exactKeys(const QJsonObject &object, const QSet<QString> &required,
               const QSet<QString> &optional = {})
{
    for (const QString &key : required)
        if (!object.contains(key)) return false;
    for (const QString &key : object.keys())
        if (!required.contains(key) && !optional.contains(key)) return false;
    return true;
}

bool isSha256(const QString &value)
{
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    });
}

QString compactSha256(const QJsonObject &object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool isInteger(const QJsonValue &value, int minimum, int maximum, int *number = nullptr)
{
    if (!value.isDouble()) return false;
    const double v = value.toDouble();
    if (!std::isfinite(v) || std::floor(v) != v || v < minimum || v > maximum) return false;
    if (number) *number = int(v);
    return true;
}

bool boundedObject(const QJsonObject &object, qsizetype maximumBytes);

QString nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

void appendEvent(QJsonObject &record, const QString &event, const QString &from,
                 const QString &to, const QJsonObject &details = {})
{
    QJsonObject entry{{QStringLiteral("atUtc"), nowUtc()},
                      {QStringLiteral("event"), event},
                      {QStringLiteral("from"), from},
                      {QStringLiteral("to"), to}};
    if (!details.isEmpty()) entry.insert(QStringLiteral("details"), details);
    QJsonArray history = record.value(QStringLiteral("history")).toArray();
    history.append(entry);
    record.insert(QStringLiteral("history"), history);
}

void setState(QJsonObject &record, const QString &state, const QString &event,
              const QJsonObject &details = {})
{
    const QString previous = record.value(QStringLiteral("state")).toString();
    record.insert(QStringLiteral("state"), state);
    appendEvent(record, event, previous, state, details);
}

bool parseSourceBinding(const QJsonValue &value, RTrustedExperimentService::SourceIdentity *source)
{
    if (!source || !value.isObject()) return false;
    const QJsonObject object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("workspaceId"), QStringLiteral("sessionId"),
                            QStringLiteral("sessionEpoch"), QStringLiteral("documentId"),
                            QStringLiteral("documentRevision"), QStringLiteral("snapshotSha256")}))
        return false;
    for (const QString &key : {QStringLiteral("workspaceId"), QStringLiteral("sessionId"),
                               QStringLiteral("sessionEpoch"), QStringLiteral("documentId"),
                               QStringLiteral("snapshotSha256")})
        if (!object.value(key).isString()) return false;
    QString revision;
    const QJsonValue revisionValue = object.value(QStringLiteral("documentRevision"));
    if (revisionValue.isString()) {
        revision = revisionValue.toString();
    } else if (revisionValue.isDouble()) {
        const double numeric = revisionValue.toDouble();
        if (!std::isfinite(numeric) || numeric < 0.0 || std::floor(numeric) != numeric
            || numeric > 9007199254740991.0) return false;
        revision = QString::number(quint64(numeric));
    } else {
        return false;
    }
    source->workspaceId = object.value(QStringLiteral("workspaceId")).toString();
    source->sessionId = object.value(QStringLiteral("sessionId")).toString();
    source->sessionEpoch = object.value(QStringLiteral("sessionEpoch")).toString();
    source->documentId = object.value(QStringLiteral("documentId")).toString();
    source->documentRevision = revision;
    source->snapshotSha256 = object.value(QStringLiteral("snapshotSha256")).toString();
    return source->isValid();
}

QStringList allowedNumericPaths()
{
    return {QStringLiteral("/geometry/segments"), QStringLiteral("/analysis/modes"),
            QStringLiteral("/analysis/timeStep"), QStringLiteral("/analysis/steps")};
}

QJsonValue valueAtPath(const QJsonObject &root, const QString &path)
{
    if (!path.startsWith(QLatin1Char('/'))) return {};
    const QStringList pieces = path.mid(1).split(QLatin1Char('/'));
    if (pieces.size() != 2) return {};
    const QJsonObject parent = root.value(pieces.at(0)).toObject();
    return parent.value(pieces.at(1));
}

bool pathIsInteger(const QString &path)
{
    return path == QLatin1String("/geometry/segments")
        || path == QLatin1String("/analysis/modes")
        || path == QLatin1String("/analysis/steps");
}

bool valueWithinRange(const QJsonValue &value, const QJsonObject &range)
{
    if (!value.isDouble() || !range.value(QStringLiteral("minimum")).isDouble()
        || !range.value(QStringLiteral("maximum")).isDouble()) return false;
    const double number = value.toDouble();
    const double minimum = range.value(QStringLiteral("minimum")).toDouble();
    const double maximum = range.value(QStringLiteral("maximum")).toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum) return false;
    if (pathIsInteger(range.value(QStringLiteral("path")).toString())
        && std::floor(number) != number) return false;
    return true;
}

void collectDiffPaths(const QJsonValue &before, const QJsonValue &after,
                      const QString &path, QStringList *diffs)
{
    if (before == after) return;
    if (before.isObject() && after.isObject()) {
        const QJsonObject a = before.toObject(), b = after.toObject();
        QSet<QString> keys;
        for (const QString &key : a.keys()) keys.insert(key);
        for (const QString &key : b.keys()) keys.insert(key);
        for (const QString &key : keys)
            collectDiffPaths(a.value(key), b.value(key), path + QLatin1Char('/') + key, diffs);
        return;
    }
    if (before.isArray() && after.isArray()) {
        const QJsonArray a = before.toArray(), b = after.toArray();
        if (a.size() != b.size()) {
            diffs->append(path);
            return;
        }
        for (int i = 0; i < a.size(); ++i)
            collectDiffPaths(a.at(i), b.at(i), path + QLatin1Char('/') + QString::number(i), diffs);
        return;
    }
    diffs->append(path);
}

QJsonObject countersOf(const QJsonObject &record)
{
    return record.value(QStringLiteral("counters")).toObject();
}

bool terminalState(const QString &state)
{
    return state == QLatin1String("candidate_ready") || state == QLatin1String("failed")
        || state == QLatin1String("cancelled") || state == QLatin1String("budget_exhausted")
        || state == QLatin1String("not_converged");
}

QJsonObject failureEvidence(const QString &code, const QString &message)
{
    return {{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

void finishActiveAttempt(QJsonObject *record, const QString &status,
                         const QString &code, const QString &message)
{
    if (!record) return;
    QJsonObject active = record->value(QStringLiteral("activeAttempt")).toObject();
    const QString attemptId = active.value(QStringLiteral("attemptId")).toString();
    if (attemptId.isEmpty() || active.value(QStringLiteral("status")).toString() != QLatin1String("running"))
        return;
    active.insert(QStringLiteral("status"), status);
    active.insert(QStringLiteral("finishedAtUtc"), nowUtc());
    active.insert(QStringLiteral("failure"), failureEvidence(code, message));
    QJsonArray attempts = record->value(QStringLiteral("attempts")).toArray();
    for (int i = 0; i < attempts.size(); ++i) {
        QJsonObject saved = attempts.at(i).toObject();
        if (saved.value(QStringLiteral("attemptId")).toString() != attemptId) continue;
        saved.insert(QStringLiteral("status"), status);
        saved.insert(QStringLiteral("finishedAtUtc"), active.value(QStringLiteral("finishedAtUtc")));
        saved.insert(QStringLiteral("failure"), active.value(QStringLiteral("failure")));
        attempts.replace(i, saved);
        break;
    }
    record->insert(QStringLiteral("attempts"), attempts);
    record->insert(QStringLiteral("activeAttempt"), QJsonObject{});
}

bool validSolveIdentity(const QJsonObject &identity)
{
    if (!exactKeys(identity, {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                              QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256"),
                              QStringLiteral("sourceSnapshotRef")})) return false;
    for (const QString &key : {QStringLiteral("artifactId"), QStringLiteral("jobId")})
        if (!identity.value(key).isString() || identity.value(key).toString().isEmpty()
            || identity.value(key).toString().size() > maximumBoundedIdentifierCodeUnits) return false;
    if (!isSha256(identity.value(QStringLiteral("inputSnapshotSha256")).toString())
        || !isSha256(identity.value(QStringLiteral("inputPackageSha256")).toString())) return false;
    const QJsonObject sourceRef = identity.value(QStringLiteral("sourceSnapshotRef")).toObject();
    if (sourceRef.isEmpty() || !isSha256(sourceRef.value(QStringLiteral("modelSha256")).toString())
        || !boundedObject(sourceRef, maximumSourceReferenceBytes)) return false;
    for (const QString &key : {QStringLiteral("projectId"), QStringLiteral("sourceRevision"),
                               QStringLiteral("modelFingerprint")}) {
        if (!sourceRef.contains(key)) continue;
        if (!sourceRef.value(key).isString()
            || sourceRef.value(key).toString().size() > maximumBoundedIdentifierCodeUnits) return false;
    }
    return true;
}

QString normalizedSpecSha256(const QJsonObject &spec)
{
    const QByteArray bytes = QJsonDocument(spec).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool boundedObject(const QJsonObject &object, qsizetype maximumBytes)
{
    return RTrustedRecordSerialization::compact(object).size() <= maximumBytes;
}

QString maximallyEscapedString(int codeUnits)
{
    // U+0001 has a six-byte JSON escape, making the length bounds conservative
    // for every accepted UTF-16 string, including control characters.
    return QString(codeUnits, QChar(0x0001));
}

QJsonObject payloadOfExactCompactSize(qsizetype bytes)
{
    QJsonObject payload{{QStringLiteral("boundedPayload"), QString()}};
    const qsizetype emptySize = RTrustedRecordSerialization::compact(payload).size();
    if (bytes < emptySize) return {};
    payload.insert(QStringLiteral("boundedPayload"), QString(bytes - emptySize, QLatin1Char('x')));
    return RTrustedRecordSerialization::compact(payload).size() == bytes ? payload : QJsonObject{};
}

bool padObjectToCompactSize(QJsonObject *object, qsizetype bytes)
{
    if (!object) return false;
    QJsonObject metrics = object->value(QStringLiteral("metrics")).toObject();
    const QString paddingKey = QStringLiteral("boundedPayload");
    metrics.insert(paddingKey, QString());
    object->insert(QStringLiteral("metrics"), metrics);
    const qsizetype emptySize = RTrustedRecordSerialization::compact(*object).size();
    if (bytes < emptySize) return false;
    metrics.insert(paddingKey, QString(bytes - emptySize, QLatin1Char('x')));
    object->insert(QStringLiteral("metrics"), metrics);
    return RTrustedRecordSerialization::compact(*object).size() == bytes;
}

QJsonObject maximumSourceReference()
{
    QJsonObject source{{QStringLiteral("modelSha256"), QString(64, QLatin1Char('f'))},
                       {QStringLiteral("boundedPayload"), QString()}};
    const qsizetype emptySize = RTrustedRecordSerialization::compact(source).size();
    source.insert(QStringLiteral("boundedPayload"),
                  QString(maximumSourceReferenceBytes - emptySize, QLatin1Char('x')));
    return source;
}

QJsonObject maximumSolveIdentity()
{
    return {{QStringLiteral("artifactId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
            {QStringLiteral("jobId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
            {QStringLiteral("inputSnapshotSha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("inputPackageSha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("sourceSnapshotRef"), maximumSourceReference()}};
}

QJsonObject maximumFailureEvidence()
{
    return {{QStringLiteral("code"), maximallyEscapedString(128)},
            {QStringLiteral("message"), maximallyEscapedString(2048)}};
}

QJsonObject maximumTransientReportBase()
{
    QJsonObject identity{{QStringLiteral("artifactId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
        {QStringLiteral("jobId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
        {QStringLiteral("inputSnapshotSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("inputPackageSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("sourceSnapshotRef"), maximumSourceReference()},
        {QStringLiteral("inputModelSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("inputModelFingerprint"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
        {QStringLiteral("manifestSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("normalizedProblemSpecSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("physicalSummarySha256"), QString(64, QLatin1Char('f'))}};
    QJsonObject report{{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("schema"), QStringLiteral("range.verification-report/v1")},
        {QStringLiteral("status"), QStringLiteral("fail")},
        {QStringLiteral("passed"), false},
        {QStringLiteral("identity"), identity},
        {QStringLiteral("metrics"), QJsonObject{}},
        {QStringLiteral("thresholds"), QJsonObject{{QStringLiteral("displacementRelative"), 1.0},
            {QStringLiteral("frequencyRelative"), 1.0},
            {QStringLiteral("effectiveDisplacementRelative"), 1.0},
            {QStringLiteral("effectiveFrequencyRelative"), 1.0}}}};
    padObjectToCompactSize(&report, maximumReportBaseBytes);
    return report;
}

QJsonObject maximumResultRecord(int recordNo)
{
    QJsonObject result{{QStringLiteral("recordNo"), recordNo},
        {QStringLiteral("timeS"), std::numeric_limits<double>::max()},
        {QStringLiteral("originKind"), QString()},
        {QStringLiteral("modelSha256"), QString(64, QLatin1Char('f'))}};
    QString largestOrigin;
    qsizetype largestSize = 0;
    for (int escapedCodeUnits = 0; escapedCodeUnits <= 64; ++escapedCodeUnits) {
        const QString candidate = maximallyEscapedString(escapedCodeUnits)
            + QString(64 - escapedCodeUnits, QLatin1Char('x'));
        result.insert(QStringLiteral("originKind"), candidate);
        const qsizetype candidateSize = RTrustedRecordSerialization::compact(result).size();
        if (candidateSize <= maximumResultRecordBytes && candidateSize > largestSize) {
            largestOrigin = candidate;
            largestSize = candidateSize;
        }
    }
    result.insert(QStringLiteral("originKind"), largestOrigin);
    return result;
}

QJsonArray maximumDisplacementSummaries(const QJsonObject &problemSpec,
                                        int frames, int displacementQois,
                                        bool includeSamples)
{
    QJsonArray result;
    QJsonArray observables = problemSpec.value(QStringLiteral("observables")).toArray();
    QJsonArray selectors;
    QStringList components;
    for (const QJsonValue &value : observables) {
        const QJsonObject observable = value.toObject();
        if (observable.value(QStringLiteral("quantity")).toString() != QLatin1String("displacement"))
            continue;
        selectors.append(observable.value(QStringLiteral("selector")).toObject());
        components.append(observable.value(QStringLiteral("component")).toString());
    }
    if (selectors.isEmpty()) {
        selectors.append(payloadOfExactCompactSize(maximumProblemSpecBytes));
        components.append(QStringLiteral("ux"));
    }
    const QJsonValue maximumNumber(std::numeric_limits<double>::max());
    const QJsonValue minimumNumber(-std::numeric_limits<double>::max());
    for (int i = 0; i < displacementQois; ++i) {
        const QJsonObject selector = selectors.at(i % selectors.size()).toObject();
        const QString component = components.at(i % components.size());
        QJsonArray values;
        QJsonArray times;
        for (int sample = 0; sample < frames; ++sample) {
            values.append(minimumNumber);
            times.append(maximumNumber);
        }
        QJsonObject entry{{QStringLiteral("selector"), selector},
            {QStringLiteral("component"), component},
            {QStringLiteral("coordinateM"), QJsonArray{minimumNumber, minimumNumber, minimumNumber}},
            {QStringLiteral("values"), includeSamples ? QJsonValue(values) : QJsonValue(QJsonArray{})}};
        if (includeSamples) entry.insert(QStringLiteral("timesS"), times);
        else {
            entry.insert(QStringLiteral("sampleCount"), frames);
            entry.insert(QStringLiteral("firstValue"), minimumNumber);
            entry.insert(QStringLiteral("lastValue"), minimumNumber);
            entry.insert(QStringLiteral("minimumValue"), -std::numeric_limits<double>::max());
            entry.insert(QStringLiteral("maximumValue"), maximumNumber);
            entry.insert(QStringLiteral("maximumAbsoluteValue"), maximumNumber);
            entry.insert(QStringLiteral("rmsValue"), maximumNumber);
            entry.insert(QStringLiteral("startTimeS"), maximumNumber);
            entry.insert(QStringLiteral("endTimeS"), maximumNumber);
            entry.remove(QStringLiteral("values"));
        }
        result.append(entry);
    }
    return result;
}

QJsonObject maximumTransientReport(const QJsonObject &problemSpec,
                                  int frames, int displacementQois)
{
    QJsonObject report = maximumTransientReportBase();
    QJsonObject identity = report.value(QStringLiteral("identity")).toObject();
    QJsonArray resultRecords;
    for (int frame = 0; frame < frames; ++frame)
        resultRecords.append(maximumResultRecord(frame + 1));
    identity.insert(QStringLiteral("resultRecords"), resultRecords);
    report.insert(QStringLiteral("identity"), identity);
    report.insert(QStringLiteral("convergenceMeasurements"), QJsonObject{
        {QStringLiteral("analysisKind"), QStringLiteral("linearTransient")},
        {QStringLiteral("expectedDisplacementCount"), displacementQois},
        {QStringLiteral("displacements"), maximumDisplacementSummaries(
             problemSpec, frames, displacementQois, true)}});
    return report;
}

QJsonObject maximumEvent(const QString &event, const QJsonObject &details = {})
{
    QJsonObject result{{QStringLiteral("atUtc"), QString(maximumHistoryTimestampCodeUnits, QLatin1Char('x'))},
        {QStringLiteral("event"), QString(maximumHistoryEventCodeUnits, QLatin1Char('x'))},
        {QStringLiteral("from"), QString(maximumHistoryStateCodeUnits, QLatin1Char('x'))},
        {QStringLiteral("to"), QString(maximumHistoryStateCodeUnits, QLatin1Char('x'))}};
    if (!event.isEmpty()) result.insert(QStringLiteral("event"), event);
    if (!details.isEmpty()) result.insert(QStringLiteral("details"), details);
    return result;
}

QJsonArray maximumHistory()
{
    QJsonArray history;
    for (int i = 0; i < maximumHistoryEvents; ++i)
        history.append(maximumEvent(QString(maximumHistoryEventCodeUnits, QLatin1Char('x'))));

    // The durable history has both a hard event-count and aggregate-byte cap.
    // Fill a details string to that cap so the record shape covers any event mix.
    QJsonObject last = history.last().toObject();
    QJsonObject details{{QStringLiteral("boundedPayload"), QString()}};
    last.insert(QStringLiteral("details"), details);
    history[history.size() - 1] = last;
    const qsizetype emptySize = QJsonDocument(history).toJson(QJsonDocument::Compact).size();
    if (emptySize > maximumHistoryBytes) return {};
    details.insert(QStringLiteral("boundedPayload"),
                   QString(maximumHistoryBytes - emptySize, QLatin1Char('x')));
    last.insert(QStringLiteral("details"), details);
    history[history.size() - 1] = last;
    return history;
}

QJsonObject maximumConvergenceChange(const QJsonObject &problemSpec, int displacementQois)
{
    QJsonArray qoiChanges;
    const QJsonArray observables = problemSpec.value(QStringLiteral("observables")).toArray();
    for (const QJsonValue &value : observables) {
        const QJsonObject observable = value.toObject();
        if (observable.value(QStringLiteral("quantity")).toString() != QLatin1String("displacement"))
            continue;
        const QString selector = QString::fromUtf8(QJsonDocument(
            observable.value(QStringLiteral("selector")).toObject()).toJson(QJsonDocument::Compact));
        const QString key = selector + QLatin1Char('|')
            + observable.value(QStringLiteral("component")).toString();
        qoiChanges.append(QJsonObject{{QStringLiteral("key"), key},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("status"), QStringLiteral("defined")},
            {QStringLiteral("belowTolerance"), false},
            {QStringLiteral("relativeChange"), std::numeric_limits<double>::max()},
            {QStringLiteral("absoluteChange"), std::numeric_limits<double>::max()},
            {QStringLiteral("fineScale"), std::numeric_limits<double>::max()}});
    }
    while (qoiChanges.size() < displacementQois)
        qoiChanges.append(QJsonObject{{QStringLiteral("key"), QString(64, QLatin1Char('x'))},
            {QStringLiteral("quantity"), QStringLiteral("displacement")},
            {QStringLiteral("status"), QStringLiteral("defined")},
            {QStringLiteral("belowTolerance"), false},
            {QStringLiteral("relativeChange"), std::numeric_limits<double>::max()},
            {QStringLiteral("absoluteChange"), std::numeric_limits<double>::max()},
            {QStringLiteral("fineScale"), std::numeric_limits<double>::max()}});
    return {{QStringLiteral("status"), QStringLiteral("compared")},
        {QStringLiteral("analysisKind"), QStringLiteral("linearTransient")},
        {QStringLiteral("fixedToleranceRelative"), 0.01},
        {QStringLiteral("qoiChanges"), qoiChanges},
        {QStringLiteral("belowTolerance"), false},
        {QStringLiteral("studyKind"), QStringLiteral("timeStep")},
        {QStringLiteral("aggregateRelativeChange"), std::numeric_limits<double>::max()}};
}

QJsonObject maximumConvergenceLevel(const QJsonObject &problemSpec,
                                    int frames, int displacementQois)
{
    const QJsonObject report = maximumTransientReportBase();
    QJsonObject cost = payloadOfExactCompactSize(maximumEvidenceBytes);
    cost.insert(QStringLiteral("solveElapsedMs"), std::numeric_limits<double>::max());
    cost.insert(QStringLiteral("verificationElapsedMs"), std::numeric_limits<double>::max());
    cost.insert(QStringLiteral("manifestBytes"), std::numeric_limits<double>::max());
    cost.insert(QStringLiteral("nodeCount"), std::numeric_limits<double>::max());
    cost.insert(QStringLiteral("elementCount"), std::numeric_limits<double>::max());
    QJsonObject summary{{QStringLiteral("analysisKind"), QStringLiteral("linearTransient")},
        {QStringLiteral("expectedDisplacementCount"), displacementQois},
        {QStringLiteral("displacements"), maximumDisplacementSummaries(
             problemSpec, frames, displacementQois, false)}};
    QJsonObject level{{QStringLiteral("level"), RTrustedExperimentService::HardSolveLimit},
        {QStringLiteral("numericalConfiguration"), payloadOfExactCompactSize(1024)},
        {QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("verificationPassed"), false},
        {QStringLiteral("verificationReport"), report},
        {QStringLiteral("measurementSummary"), summary},
        {QStringLiteral("measurementSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("artifactId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
        {QStringLiteral("jobId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
        {QStringLiteral("manifestSha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("cost"), cost}};
    level.insert(QStringLiteral("changeFromPrevious"),
                 maximumConvergenceChange(problemSpec, displacementQois));
    return level;
}

QJsonObject maximumTransientRecordShape(const QJsonObject &problemSpec,
                                        int frames, int displacementQois)
{
    // Construct the durable schema shape using maximum-size witnesses for
    // each capped field, then serialize it with the same compact encoder used
    // by saveRecord. This deliberately counts JSON escaping and nested keys.
    const QJsonObject maximumSpec = payloadOfExactCompactSize(maximumProblemSpecBytes);
    QJsonArray ranges;
    for (const QString &path : {QStringLiteral("/geometry/segments"),
                                QStringLiteral("/analysis/modes"),
                                QStringLiteral("/analysis/timeStep"),
                                QStringLiteral("/analysis/steps")})
        ranges.append(QJsonObject{{QStringLiteral("path"), path},
            {QStringLiteral("minimum"), -std::numeric_limits<double>::max()},
            {QStringLiteral("maximum"), std::numeric_limits<double>::max()}});
    QJsonArray levels;
    for (int level = 0; level < 4; ++level)
        levels.append(maximumConvergenceLevel(problemSpec, frames, displacementQois));

    QJsonArray attempts;
    QJsonArray failedConfigurations;
    for (int i = 0; i < RTrustedExperimentService::HardSolveLimit; ++i) {
        QJsonObject attempt{{QStringLiteral("attemptId"), QString(36, QLatin1Char('f'))},
            {QStringLiteral("configurationKey"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("numericalConfigDigest"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("inputSnapshotSha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("inputPackageSha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("status"), QStringLiteral("completed")},
            {QStringLiteral("startedAtUtc"), QStringLiteral("9999-12-31T23:59:59.999Z")},
            {QStringLiteral("finishedAtUtc"), QStringLiteral("9999-12-31T23:59:59.999Z")},
            {QStringLiteral("submissionClaimed"), true},
            {QStringLiteral("submissionClaimedAtUtc"), QStringLiteral("9999-12-31T23:59:59.999Z")},
            {QStringLiteral("solveIdentity"), maximumSolveIdentity()}};
        attempts.append(attempt);
        failedConfigurations.append(QJsonObject{{QStringLiteral("key"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("numericalConfigDigest"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("inputSnapshotSha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("inputPackageSha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("failure"), maximumFailureEvidence()}});
    }

    const QJsonArray history = maximumHistory();
    if (history.isEmpty()) return {};

    QJsonObject authorization{{QStringLiteral("schema"), QStringLiteral("range.trusted-experiment-authorization/v1")},
        {QStringLiteral("experimentId"), QString(36, QLatin1Char('f'))},
        {QStringLiteral("sourceBinding"), QJsonObject{
            {QStringLiteral("workspaceId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
            {QStringLiteral("sessionId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
            {QStringLiteral("sessionEpoch"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
            {QStringLiteral("documentId"), maximallyEscapedString(maximumBoundedIdentifierCodeUnits)},
            {QStringLiteral("documentRevision"), maximallyEscapedString(64)},
            {QStringLiteral("snapshotSha256"), QString(64, QLatin1Char('f'))}}},
        {QStringLiteral("problemSpecDigest"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("numericalConfigDigest"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("experimentPolicy"), QJsonObject{
            {QStringLiteral("allowedNumericalRanges"), ranges},
            {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 8},
                {QStringLiteral("repairs"), 3}, {QStringLiteral("modelTurns"), 12}}},
            {QStringLiteral("stopConditions"), QJsonObject{
                {QStringLiteral("requirePassingVerification"), true},
                {QStringLiteral("stopOnSourceChange"), true},
                {QStringLiteral("stopOnUnclassifiedFailure"), true}}},
            {QStringLiteral("convergenceStudy"), QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("timeStep")},
                {QStringLiteral("maximumLevels"), 4}}}}},
        {QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('f'))}};

    QJsonObject record{{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("experimentId"), QString(36, QLatin1Char('f'))},
        {QStringLiteral("state"), QStringLiteral("not_converged")},
        {QStringLiteral("authorization"), authorization},
        {QStringLiteral("problemSpecInput"), maximumSpec},
        {QStringLiteral("problemSpec"), maximumSpec},
        {QStringLiteral("candidateProblemSpecInput"), maximumSpec},
        {QStringLiteral("candidateProblemSpec"), maximumSpec},
        {QStringLiteral("pendingRepairProblemSpecInput"), maximumSpec},
        {QStringLiteral("pendingRepairProblemSpec"), maximumSpec},
        {QStringLiteral("candidateNumericalConfigDigest"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("counters"), QJsonObject{{QStringLiteral("solves"), 8},
            {QStringLiteral("repairs"), 3}, {QStringLiteral("modelTurns"), 12}}},
        {QStringLiteral("history"), history},
        {QStringLiteral("attempts"), attempts},
        {QStringLiteral("failedConfigurations"), failedConfigurations},
        {QStringLiteral("activeAttempt"), attempts.last()},
        {QStringLiteral("latestSolveIdentity"), maximumSolveIdentity()},
        {QStringLiteral("latestSolveAttemptId"), QString(36, QLatin1Char('f'))},
        {QStringLiteral("verificationRequestDigest"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("verificationPassed"), false},
        {QStringLiteral("verificationReport"), maximumTransientReport(problemSpec, frames, displacementQois)},
        {QStringLiteral("convergenceLevels"), levels},
        {QStringLiteral("convergenceChanges"), QJsonArray{
            maximumConvergenceChange(problemSpec, displacementQois),
            maximumConvergenceChange(problemSpec, displacementQois),
            maximumConvergenceChange(problemSpec, displacementQois)}},
        {QStringLiteral("convergence"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("timeStep")},
            {QStringLiteral("status"), QStringLiteral("not_converged")},
            {QStringLiteral("toleranceRelative"), 0.01},
            {QStringLiteral("requiredConsecutiveChanges"), 2},
            {QStringLiteral("maximumLevels"), 4},
            {QStringLiteral("levelsCompleted"), 4},
            {QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("adjacentChanges"), QJsonArray{
                maximumConvergenceChange(problemSpec, displacementQois),
                maximumConvergenceChange(problemSpec, displacementQois),
                maximumConvergenceChange(problemSpec, displacementQois)}},
            {QStringLiteral("cost"), QJsonObject{{QStringLiteral("solveElapsedMs"), std::numeric_limits<double>::max()},
                {QStringLiteral("verificationElapsedMs"), std::numeric_limits<double>::max()},
                {QStringLiteral("manifestBytes"), std::numeric_limits<double>::max()},
                {QStringLiteral("solveCount"), 8}}},
            {QStringLiteral("stopReason"), maximallyEscapedString(128)}}},
        {QStringLiteral("cancelRequested"), true},
        {QStringLiteral("repairInProgress"), true},
        {QStringLiteral("sourceInvalidated"), true},
        {QStringLiteral("terminalCode"), maximallyEscapedString(128)},
        {QStringLiteral("createdAtUtc"), QStringLiteral("9999-12-31T23:59:59.999Z")}};
    return record;
}

QJsonObject compactVerificationReport(const QJsonObject &report)
{
    QJsonObject compact = report;
    compact.remove(QStringLiteral("convergenceMeasurements"));
    QJsonObject identity = compact.value(QStringLiteral("identity")).toObject();
    identity.remove(QStringLiteral("resultRecords"));
    compact.insert(QStringLiteral("identity"), identity);
    return compact;
}

QJsonObject summarizeConvergenceMeasurements(const QJsonObject &measurements)
{
    QJsonObject summary{{QStringLiteral("analysisKind"), measurements.value(QStringLiteral("analysisKind"))},
        {QStringLiteral("expectedDisplacementCount"), measurements.value(QStringLiteral("expectedDisplacementCount"))}};
    if (measurements.contains(QStringLiteral("mode")))
        summary.insert(QStringLiteral("mode"), measurements.value(QStringLiteral("mode")));
    if (measurements.contains(QStringLiteral("frequencyHz")))
        summary.insert(QStringLiteral("frequencyHz"), measurements.value(QStringLiteral("frequencyHz")));
    if (measurements.contains(QStringLiteral("strainEnergyJ")))
        summary.insert(QStringLiteral("strainEnergyJ"), measurements.value(QStringLiteral("strainEnergyJ")));

    QJsonArray displacementSummaries;
    for (const QJsonValue &entry : measurements.value(QStringLiteral("displacements")).toArray()) {
        const QJsonObject item = entry.toObject();
        const QJsonArray values = item.value(QStringLiteral("values")).toArray();
        if (values.isEmpty()) continue;
        double minimum = values.first().toDouble();
        double maximum = minimum;
        double maximumAbsolute = std::abs(minimum);
        double scale = 0.0;
        double scaledSquares = 0.0;
        for (const QJsonValue &value : values) {
            const double sample = value.toDouble();
            minimum = std::min(minimum, sample);
            maximum = std::max(maximum, sample);
            const double magnitude = std::abs(sample);
            maximumAbsolute = std::max(maximumAbsolute, magnitude);
            if (magnitude > scale) {
                const double ratio = scale == 0.0 ? 0.0 : scale / magnitude;
                scaledSquares = scaledSquares * ratio * ratio + 1.0;
                scale = magnitude;
            } else if (scale > 0.0) {
                const double ratio = magnitude / scale;
                scaledSquares += ratio * ratio;
            }
        }
        const double rms = scale == 0.0 ? 0.0 : scale * std::sqrt(scaledSquares / double(values.size()));
        QJsonObject displacementSummary{{QStringLiteral("selector"), item.value(QStringLiteral("selector"))},
            {QStringLiteral("component"), item.value(QStringLiteral("component"))},
            {QStringLiteral("coordinateM"), item.value(QStringLiteral("coordinateM"))},
            {QStringLiteral("sampleCount"), values.size()},
            {QStringLiteral("firstValue"), values.first()},
            {QStringLiteral("lastValue"), values.last()},
            {QStringLiteral("minimumValue"), minimum},
            {QStringLiteral("maximumValue"), maximum},
            {QStringLiteral("maximumAbsoluteValue"), maximumAbsolute},
            {QStringLiteral("rmsValue"), rms}};
        if (item.contains(QStringLiteral("timesS"))) {
            const QJsonArray times = item.value(QStringLiteral("timesS")).toArray();
            if (!times.isEmpty()) {
                displacementSummary.insert(QStringLiteral("startTimeS"), times.first());
                displacementSummary.insert(QStringLiteral("endTimeS"), times.last());
            }
        }
        displacementSummaries.append(displacementSummary);
    }
    summary.insert(QStringLiteral("displacements"), displacementSummaries);
    return summary;
}

bool boundedConvergenceVerificationReport(const QJsonObject &report,
                                          const QJsonObject &problemSpec)
{
    const QJsonObject analysis = problemSpec.value(QStringLiteral("analysis")).toObject();
    const QString analysisKind = analysis.value(QStringLiteral("kind")).toString();
    const int maximumResultRecords = analysisKind == QLatin1String("linearTransient")
        ? analysis.value(QStringLiteral("steps")).toInt(-1)
        : analysisKind == QLatin1String("modal")
            ? analysis.value(QStringLiteral("modes")).toInt(-1) : 1;
    const int minimumRecordNo = analysisKind == QLatin1String("static") ? 0 : 1;
    const int maximumRecordNo = analysisKind == QLatin1String("static")
        ? 0 : maximumResultRecords;
    if (maximumResultRecords < 1
        || maximumResultRecords > RTrustedConvergenceService::MaximumTransientFrames) return false;
    QJsonObject base = report;
    base.remove(QStringLiteral("convergenceMeasurements"));
    QJsonObject identity = base.value(QStringLiteral("identity")).toObject();
    const QJsonArray resultRecords = identity.take(QStringLiteral("resultRecords")).toArray();
    base.insert(QStringLiteral("identity"), identity);
    if (!boundedObject(base, maximumReportBaseBytes)
        || resultRecords.size() > maximumResultRecords) return false;
    for (const QJsonValue &value : resultRecords) {
        if (!value.isObject()) return false;
        const QJsonObject item = value.toObject();
        if (!exactKeys(item, {QStringLiteral("recordNo"), QStringLiteral("timeS"),
                              QStringLiteral("originKind"), QStringLiteral("modelSha256")})
            || !isInteger(item.value(QStringLiteral("recordNo")), minimumRecordNo,
                          maximumRecordNo)
            || !item.value(QStringLiteral("timeS")).isDouble()
            || !std::isfinite(item.value(QStringLiteral("timeS")).toDouble())
            || !item.value(QStringLiteral("originKind")).isString()
            || item.value(QStringLiteral("originKind")).toString().size() > 64
            || !isSha256(item.value(QStringLiteral("modelSha256")).toString())
            || !boundedObject(item, maximumResultRecordBytes)) return false;
    }

    const QJsonValue measurementsValue = report.value(QStringLiteral("convergenceMeasurements"));
    if (measurementsValue.isUndefined()) return true;
    if (!measurementsValue.isObject()) return false;
    const QJsonObject measurements = measurementsValue.toObject();
    if (measurements.value(QStringLiteral("analysisKind")).toString() != QLatin1String("linearTransient"))
        return boundedObject(measurements,
            RTrustedConvergenceService::MaximumConvergenceMeasurementBytes);
    if (!exactKeys(measurements, {QStringLiteral("analysisKind"),
                                  QStringLiteral("expectedDisplacementCount"),
                                  QStringLiteral("displacements")})
        || measurements.value(QStringLiteral("analysisKind")).toString() != QLatin1String("linearTransient")
        || !isInteger(measurements.value(QStringLiteral("expectedDisplacementCount")), 0, 64))
        return false;
    const QJsonArray displacements = measurements.value(QStringLiteral("displacements")).toArray();
    int expectedDisplacements = 0;
    for (const QJsonValue &value : problemSpec.value(QStringLiteral("observables")).toArray())
        if (value.toObject().value(QStringLiteral("quantity")).toString() == QLatin1String("displacement"))
            ++expectedDisplacements;
    if (displacements.size() > expectedDisplacements) return false;
    for (const QJsonValue &value : displacements) {
        if (!value.isObject()) return false;
        const QJsonObject displacement = value.toObject();
        const QJsonArray values = displacement.value(QStringLiteral("values")).toArray();
        const QJsonArray times = displacement.value(QStringLiteral("timesS")).toArray();
        if (!exactKeys(displacement, {QStringLiteral("selector"), QStringLiteral("component"),
                                      QStringLiteral("coordinateM"), QStringLiteral("values"),
                                      QStringLiteral("timesS")})
            || !displacement.value(QStringLiteral("selector")).isObject()
            || !boundedObject(displacement.value(QStringLiteral("selector")).toObject(), maximumProblemSpecBytes)
            || !displacement.value(QStringLiteral("component")).isString()
            || displacement.value(QStringLiteral("component")).toString().size() > 8
            || !displacement.value(QStringLiteral("coordinateM")).isArray()
            || displacement.value(QStringLiteral("coordinateM")).toArray().size() != 3
            || values.size() > maximumResultRecords
            || times.size() > maximumResultRecords
            || (!times.isEmpty() && times.size() != values.size())) return false;
        for (const QJsonValue &coordinate : displacement.value(QStringLiteral("coordinateM")).toArray())
            if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble())) return false;
        for (const QJsonValue &sample : values)
            if (!sample.isDouble() || !std::isfinite(sample.toDouble())) return false;
        for (const QJsonValue &time : times)
            if (!time.isDouble() || !std::isfinite(time.toDouble())) return false;
    }
    return true;
}

bool validateRanges(const QJsonArray &ranges, const QJsonObject &normalized,
                    QString *error)
{
    if (ranges.size() > 4) {
        if (error) *error = QStringLiteral("At most four numerical fields may be authorized.");
        return false;
    }
    QSet<QString> seen;
    const QStringList allow = allowedNumericPaths();
    for (int i = 0; i < ranges.size(); ++i) {
        if (!ranges.at(i).isObject()) {
            if (error) *error = QStringLiteral("Each allowed numerical range must be an object.");
            return false;
        }
        const QJsonObject range = ranges.at(i).toObject();
        if (!exactKeys(range, {QStringLiteral("path"), QStringLiteral("minimum"), QStringLiteral("maximum")})
            || !range.value(QStringLiteral("path")).isString()
            || !range.value(QStringLiteral("minimum")).isDouble()
            || !range.value(QStringLiteral("maximum")).isDouble()) {
            if (error) *error = QStringLiteral("Numerical ranges require exactly path, minimum and maximum.");
            return false;
        }
        const QString path = range.value(QStringLiteral("path")).toString();
        const double minimum = range.value(QStringLiteral("minimum")).toDouble();
        const double maximum = range.value(QStringLiteral("maximum")).toDouble();
        if (!allow.contains(path) || seen.contains(path) || !std::isfinite(minimum)
            || !std::isfinite(maximum) || minimum > maximum) {
            if (error) *error = QStringLiteral("Numerical range path or bounds are invalid: %1.").arg(path);
            return false;
        }
        if (pathIsInteger(path) && (std::floor(minimum) != minimum || std::floor(maximum) != maximum)) {
            if (error) *error = QStringLiteral("Integer numerical fields require integer bounds: %1.").arg(path);
            return false;
        }
        if (!valueWithinRange(valueAtPath(normalized, path), range)) {
            if (error) *error = QStringLiteral("The authorized range must contain the initial value: %1.").arg(path);
            return false;
        }
        seen.insert(path);
    }
    return true;
}

bool studyRangesCoverLevels(const QJsonObject &spec, const QJsonObject &study,
                            const QJsonArray &ranges, QString *error)
{
    QHash<QString, QJsonObject> byPath;
    for (const QJsonValue &value : ranges) {
        const QJsonObject range = value.toObject();
        byPath.insert(range.value(QStringLiteral("path")).toString(), range);
    }
    const int refinements = study.value(QStringLiteral("maximumLevels")).toInt() - 1;
    const QString kind = study.value(QStringLiteral("kind")).toString();
    const auto sameBound = [](double left, double right) {
        return std::isfinite(left) && std::isfinite(right)
            && std::abs(left - right) <= std::max(1.0e-14,
                1.0e-12 * std::max(std::abs(left), std::abs(right)));
    };
    const auto requireExactRange = [&](const QString &path, double minimum, double maximum) {
        const auto it = byPath.constFind(path);
        const double authorizedMinimum = it == byPath.cend()
            ? std::numeric_limits<double>::quiet_NaN()
            : it->value(QStringLiteral("minimum")).toDouble();
        const double authorizedMaximum = it == byPath.cend()
            ? std::numeric_limits<double>::quiet_NaN()
            : it->value(QStringLiteral("maximum")).toDouble();
        if (it == byPath.cend() || !it->value(QStringLiteral("minimum")).isDouble()
            || !it->value(QStringLiteral("maximum")).isDouble()
            || authorizedMinimum > minimum || authorizedMaximum < maximum
            || !sameBound(authorizedMinimum, minimum) || !sameBound(authorizedMaximum, maximum)) {
            if (error) *error = QStringLiteral("The authorized range for %1 must match the planned study bounds exactly.").arg(path);
            return false;
        }
        return true;
    };
    if (kind == QLatin1String("mesh")) {
        const double initial = spec.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("segments")).toDouble();
        const double maximum = initial * std::pow(2.0, refinements);
        return std::isfinite(maximum) && ranges.size() == 1
            && byPath.size() == 1
            && requireExactRange(QStringLiteral("/geometry/segments"), initial, maximum);
    }
    const QJsonObject analysis = spec.value(QStringLiteral("analysis")).toObject();
    const double dt = analysis.value(QStringLiteral("timeStep")).toDouble();
    const double steps = analysis.value(QStringLiteral("steps")).toDouble();
    const double refinedDt = dt / std::pow(2.0, refinements);
    const double refinedSteps = steps * std::pow(2.0, refinements);
    return std::isfinite(refinedDt) && std::isfinite(refinedSteps)
        && ranges.size() == 2 && byPath.size() == 2
        && requireExactRange(QStringLiteral("/analysis/timeStep"), refinedDt, dt)
        && requireExactRange(QStringLiteral("/analysis/steps"), steps, refinedSteps);
}

bool candidateWithinAuthorization(const QJsonObject &normalizedCandidate,
                                 const QJsonObject &authorization,
                                 QString *error)
{
    const QString physicalDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::physicalSummary(normalizedCandidate));
    if (physicalDigest != authorization.value(QStringLiteral("physicalSummaryDigest")).toString()) {
        if (error) *error = QStringLiteral("The repair changes the authorized physical problem.");
        return false;
    }
    const QJsonArray ranges = authorization.value(QStringLiteral("experimentPolicy")).toObject()
        .value(QStringLiteral("allowedNumericalRanges")).toArray();
    for (const QJsonValue &entry : ranges) {
        const QJsonObject range = entry.toObject();
        if (!valueWithinRange(valueAtPath(normalizedCandidate,
                                          range.value(QStringLiteral("path")).toString()), range)) {
            if (error) *error = QStringLiteral("A numerical value lies outside its authorized range: %1.")
                .arg(range.value(QStringLiteral("path")).toString());
            return false;
        }
    }
    return true;
}

bool convergenceCandidateWithinPlan(const QJsonObject &candidateInput,
                                    const QJsonObject &normalizedCandidate,
                                    const QJsonObject &record,
                                    const QJsonObject &authorization,
                                    QString *error)
{
    const QJsonObject policy = authorization.value(QStringLiteral("experimentPolicy")).toObject();
    const QJsonObject study = policy.value(QStringLiteral("convergenceStudy")).toObject();
    if (study.isEmpty()) return true;

    const QJsonObject baseInput = record.value(QStringLiteral("problemSpecInput")).toObject();
    const auto validatedStudy = RTrustedConvergenceService::validateStudy(baseInput, study);
    if (!validatedStudy.ok) {
        if (error) *error = QStringLiteral("The persisted convergence plan no longer validates: %1")
            .arg(validatedStudy.code);
        return false;
    }
    const QJsonObject baseline = validatedStudy.data.value(QStringLiteral("normalizedProblemSpec")).toObject();
    const int maximumLevels = validatedStudy.data.value(QStringLiteral("maximumLevels")).toInt();
    const QString kind = validatedStudy.data.value(QStringLiteral("kind")).toString();
    bool plannedLevel = false;
    if (kind == QLatin1String("mesh")) {
        const int baselineSegments = baseline.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("segments")).toInt(-1);
        const int candidateSegments = normalizedCandidate.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("segments")).toInt(-1);
        for (int level = 0; level < maximumLevels && baselineSegments > 0; ++level) {
            const qint64 expected = qint64(baselineSegments) * (qint64(1) << level);
            if (expected <= std::numeric_limits<int>::max() && candidateSegments == int(expected)) {
                plannedLevel = true;
                break;
            }
        }
    } else {
        const QJsonObject baselineAnalysis = baseline.value(QStringLiteral("analysis")).toObject();
        const QJsonObject candidateAnalysis = normalizedCandidate.value(QStringLiteral("analysis")).toObject();
        const double baselineTimeStep = baselineAnalysis.value(QStringLiteral("timeStep")).toDouble();
        const int baselineSteps = baselineAnalysis.value(QStringLiteral("steps")).toInt(-1);
        const double candidateTimeStep = candidateAnalysis.value(QStringLiteral("timeStep")).toDouble();
        const int candidateSteps = candidateAnalysis.value(QStringLiteral("steps")).toInt(-1);
        for (int level = 0; level < maximumLevels && baselineSteps > 0; ++level) {
            const double factor = double(qint64(1) << level);
            if (candidateSteps == baselineSteps * int(factor)
                && std::abs(candidateTimeStep - baselineTimeStep / factor)
                    <= std::max(1.0e-14, 1.0e-12 * std::max(std::abs(candidateTimeStep),
                                                            std::abs(baselineTimeStep / factor)))) {
                plannedLevel = true;
                break;
            }
        }
    }
    if (!plannedLevel) {
        if (error) *error = QStringLiteral("The candidate is outside the baseline and refinement levels declared by the convergence plan.");
        return false;
    }

    const QJsonObject analysis = normalizedCandidate.value(QStringLiteral("analysis")).toObject();
    if (analysis.value(QStringLiteral("kind")).toString() == QLatin1String("linearTransient")) {
        const int steps = analysis.value(QStringLiteral("steps")).toInt(-1);
        int displacementQois = 0;
        for (const QJsonValue &value : normalizedCandidate.value(QStringLiteral("observables")).toArray())
            if (value.toObject().value(QStringLiteral("quantity")).toString() == QLatin1String("displacement"))
                ++displacementQois;
        if (steps < 1 || displacementQois < 1
            || RTrustedConvergenceService::estimateMaximumTransientRecordBytes(
                   candidateInput, steps + 1, displacementQois)
                > RTrustedConvergenceService::MaximumDurableRecordBytes) {
            if (error) *error = QStringLiteral("The candidate's maximum transient convergence evidence exceeds the durable record limit.");
            return false;
        }
    }
    return true;
}
}

QByteArray RTrustedRecordSerialization::compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

qsizetype RTrustedRecordSerialization::maximumTransientRecordBytes(
    const QJsonObject &problemSpec, int frames, int displacementQois)
{
    if (frames < 1 || frames > RTrustedConvergenceService::MaximumTransientFrames
        || displacementQois < 1 || displacementQois > 64
        || !boundedObject(problemSpec, maximumProblemSpecBytes))
        return std::numeric_limits<qsizetype>::max();
    const auto validation = RTrustedMechanicsService::validate(problemSpec);
    if (!validation.valid || !boundedObject(validation.normalizedSpec, maximumProblemSpecBytes))
        return std::numeric_limits<qsizetype>::max();
    const QJsonObject normalizedSpec = validation.normalizedSpec;
    if (normalizedSpec.value(QStringLiteral("analysis")).toObject()
               .value(QStringLiteral("kind")).toString() != QLatin1String("linearTransient"))
        return std::numeric_limits<qsizetype>::max();
    const QJsonValue stepsValue = normalizedSpec.value(QStringLiteral("analysis")).toObject()
        .value(QStringLiteral("steps"));
    if (!stepsValue.isDouble() || !std::isfinite(stepsValue.toDouble())
        || std::floor(stepsValue.toDouble()) != stepsValue.toDouble()
        || stepsValue.toDouble() < 1.0
        || stepsValue.toDouble() >= double(RTrustedConvergenceService::MaximumTransientFrames)
        || frames < int(stepsValue.toDouble()) + 1)
        return std::numeric_limits<qsizetype>::max();
    int normalizedDisplacementCount = 0;
    for (const QJsonValue &value : normalizedSpec.value(QStringLiteral("observables")).toArray()) {
        const QJsonObject observable = value.toObject();
        if (observable.value(QStringLiteral("quantity")).toString() != QLatin1String("displacement"))
            continue;
        ++normalizedDisplacementCount;
        if (RTrustedRecordSerialization::compact(
                observable.value(QStringLiteral("selector")).toObject()).size()
            > RTrustedConvergenceService::MaximumSelectorIdentityBytes)
            return std::numeric_limits<qsizetype>::max();
    }
    if (normalizedDisplacementCount != displacementQois) return std::numeric_limits<qsizetype>::max();
    const QJsonObject shape = maximumTransientRecordShape(normalizedSpec, frames, displacementQois);
    if (shape.isEmpty()) return std::numeric_limits<qsizetype>::max();
    return compact(shape).size();
}

QJsonObject RTrustedExperimentService::SourceIdentity::toJson() const
{
    return {{QStringLiteral("workspaceId"), workspaceId},
            {QStringLiteral("sessionId"), sessionId},
            {QStringLiteral("sessionEpoch"), sessionEpoch},
            {QStringLiteral("documentId"), documentId},
            {QStringLiteral("documentRevision"), documentRevision},
            {QStringLiteral("snapshotSha256"), snapshotSha256}};
}

bool RTrustedExperimentService::SourceIdentity::isValid() const
{
    return !workspaceId.trimmed().isEmpty() && workspaceId.size() <= maximumBoundedIdentifierCodeUnits
        && !sessionId.trimmed().isEmpty() && sessionId.size() <= maximumBoundedIdentifierCodeUnits
        && !sessionEpoch.trimmed().isEmpty() && sessionEpoch.size() <= maximumBoundedIdentifierCodeUnits
        && !documentId.trimmed().isEmpty() && documentId.size() <= maximumBoundedIdentifierCodeUnits
        && !documentRevision.trimmed().isEmpty() && documentRevision.size() <= 64
        && isSha256(snapshotSha256);
}

RTrustedExperimentService::RTrustedExperimentService(const QString &storageDirectory)
    : storageDirectory{QDir(storageDirectory).absolutePath()}
{
    QDir().mkpath(this->storageDirectory);
}

const QStringList &RTrustedExperimentService::lifecycleStates()
{
    static const QStringList states{
        QStringLiteral("authorized"), QStringLiteral("prepared"), QStringLiteral("diagnosing"),
        QStringLiteral("solving"), QStringLiteral("verifying"), QStringLiteral("repairing"),
        QStringLiteral("candidate_ready"), QStringLiteral("failed"), QStringLiteral("cancelled"),
        QStringLiteral("needs_confirmation"), QStringLiteral("budget_exhausted"),
        QStringLiteral("not_converged")};
    return states;
}

RTrustedExperimentService::Outcome RTrustedExperimentService::success(const QJsonObject &data)
{
    return {true, QStringLiteral("OK"), QString(), data};
}

RTrustedExperimentService::Outcome RTrustedExperimentService::failure(const QString &code,
                                                                        const QString &message,
                                                                        const QJsonObject &data)
{
    return {false, code, message, data};
}

bool RTrustedExperimentService::isExperimentId(const QString &experimentId)
{
    const QUuid uuid(experimentId);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == experimentId;
}

QString RTrustedExperimentService::recordPath(const QString &experimentId) const
{
    return QDir(this->storageDirectory).filePath(experimentId + QStringLiteral(".json"));
}

bool RTrustedExperimentService::lockRecord(const QString &experimentId, QLockFile *lock,
                                            QString *error) const
{
    if (!lock || !isExperimentId(experimentId)) {
        if (error) *error = QStringLiteral("Invalid experiment identifier.");
        return false;
    }
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(5000)) {
        if (error) *error = QStringLiteral("The experiment record is locked by another operation.");
        return false;
    }
    return true;
}

bool RTrustedExperimentService::loadRecord(const QString &experimentId, QJsonObject *record,
                                             QString *error) const
{
    if (!record || !isExperimentId(experimentId)) {
        if (error) *error = QStringLiteral("Invalid experiment identifier.");
        return false;
    }
    QFile file(recordPath(experimentId));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Experiment record does not exist or cannot be read.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || document.object().value(QStringLiteral("experimentId")).toString() != experimentId
        || document.object().value(QStringLiteral("schemaVersion")).toInt() != SchemaVersion) {
        if (error) *error = QStringLiteral("Experiment record is invalid or has an unsupported schema.");
        return false;
    }
    *record = document.object();
    QJsonObject authorization = record->value(QStringLiteral("authorization")).toObject();
    const QString authorizationDigest = authorization.take(QStringLiteral("authorizationDigest")).toString();
    if (authorization.value(QStringLiteral("schema")).toString() != authorizationSchema
        || authorization.value(QStringLiteral("experimentId")).toString() != experimentId
        || !isSha256(authorizationDigest) || compactSha256(authorization) != authorizationDigest
        || record->value(QStringLiteral("authorization")).toObject()
            .value(QStringLiteral("authorizationDigest")).toString() != authorizationDigest) {
        if (error) *error = QStringLiteral("Persisted authorization identity or digest is invalid.");
        return false;
    }
    return true;
}

bool RTrustedExperimentService::saveRecord(const QString &experimentId, const QJsonObject &record,
                                             QString *error) const
{
    if (!isExperimentId(experimentId)
        || record.value(QStringLiteral("experimentId")).toString() != experimentId) {
        if (error) *error = QStringLiteral("Record identity mismatch.");
        return false;
    }
    const QJsonValue historyValue = record.value(QStringLiteral("history"));
    bool historyValid = historyValue.isArray()
        && historyValue.toArray().size() <= maximumHistoryEvents
        && QJsonDocument(historyValue.toArray()).toJson(QJsonDocument::Compact).size()
            <= maximumHistoryBytes;
    if (historyValid) {
        for (const QJsonValue &value : historyValue.toArray()) {
            if (!value.isObject()) {
                historyValid = false;
                break;
            }
            const QJsonObject event = value.toObject();
            const QString timestamp = event.value(QStringLiteral("atUtc")).toString();
            const QString name = event.value(QStringLiteral("event")).toString();
            const QString from = event.value(QStringLiteral("from")).toString();
            const QString to = event.value(QStringLiteral("to")).toString();
            if (!event.value(QStringLiteral("atUtc")).isString()
                || !event.value(QStringLiteral("event")).isString()
                || !event.value(QStringLiteral("from")).isString()
                || !event.value(QStringLiteral("to")).isString()
                || timestamp.size() > maximumHistoryTimestampCodeUnits
                || name.isEmpty() || name.size() > maximumHistoryEventCodeUnits
                || from.size() > maximumHistoryStateCodeUnits
                || to.size() > maximumHistoryStateCodeUnits
                || (event.contains(QStringLiteral("details"))
                    && !event.value(QStringLiteral("details")).isObject())) {
                historyValid = false;
                break;
            }
        }
    }
    if (!historyValid) {
        if (error) *error = QStringLiteral("Experiment history exceeds its durable event-count or byte bound.");
        return false;
    }
    const QByteArray bytes = RTrustedRecordSerialization::compact(record);
    if (bytes.size() > RTrustedConvergenceService::MaximumDurableRecordBytes) {
        if (error) *error = QStringLiteral("Experiment record exceeds the durable storage size limit.");
        return false;
    }
    QSaveFile file(recordPath(experimentId));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot create the atomic experiment record.");
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Atomic experiment record commit failed.");
        return false;
    }
    return true;
}

bool RTrustedExperimentService::sourceMatches(const QJsonObject &record,
                                                const SourceIdentity &currentSource) const
{
    if (!currentSource.isValid()) return false;
    const QJsonObject bound = record.value(QStringLiteral("authorization")).toObject()
        .value(QStringLiteral("sourceBinding")).toObject();
    return bound == currentSource.toJson();
}

bool RTrustedExperimentService::authorizeMutation(QJsonObject *record,
                                                   const SourceIdentity &currentSource,
                                                   QString *errorCode,
                                                   QString *errorMessage) const
{
    if (!record) return false;
    if (record->value(QStringLiteral("cancelRequested")).toBool()
        || record->value(QStringLiteral("state")).toString() == QLatin1String("cancelled")) {
        if (errorCode) *errorCode = QStringLiteral("EXPERIMENT_CANCELLED");
        if (errorMessage) *errorMessage = QStringLiteral("The experiment has been cancelled.");
        return false;
    }
    if (!sourceMatches(*record, currentSource)) {
        record->insert(QStringLiteral("sourceInvalidated"), true);
        record->insert(QStringLiteral("terminalCode"), QStringLiteral("SOURCE_STALE"));
        finishActiveAttempt(record, QStringLiteral("source_invalidated"),
                            QStringLiteral("SOURCE_STALE"),
                            QStringLiteral("The authorized source changed while the solve attempt was active."));
        setState(*record, QStringLiteral("failed"), QStringLiteral("source-invalidated"));
        QString ignored;
        if (!saveRecord(record->value(QStringLiteral("experimentId")).toString(), *record, &ignored)) {
            if (errorCode) *errorCode = QStringLiteral("EXPERIMENT_STORAGE_FAILED");
            if (errorMessage) *errorMessage = QStringLiteral("Source became stale and invalidation could not be persisted: ") + ignored;
        } else {
            if (errorCode) *errorCode = QStringLiteral("SOURCE_STALE");
            if (errorMessage) *errorMessage = QStringLiteral("The source workspace, session, document, revision or snapshot changed.");
        }
        return false;
    }
    return true;
}

RTrustedExperimentService::Outcome RTrustedExperimentService::authorize(
    const QJsonObject &request, const SourceIdentity &currentSource)
{
    return authorizeInternal(request, currentSource,
                             QUuid::createUuid().toString(QUuid::WithoutBraces), nullptr, true);
}

RTrustedExperimentService::Outcome RTrustedExperimentService::previewAuthorization(
    const QJsonObject &request, const SourceIdentity &currentSource)
{
    return authorizeInternal(request, currentSource,
                             QUuid::createUuid().toString(QUuid::WithoutBraces), nullptr, false);
}

RTrustedExperimentService::Outcome RTrustedExperimentService::authorizePreview(
    const QJsonObject &request, const SourceIdentity &currentSource,
    const QString &experimentId, const QJsonObject &expectedAuthorization)
{
    return authorizeInternal(request, currentSource, experimentId, &expectedAuthorization, true);
}

RTrustedExperimentService::Outcome RTrustedExperimentService::authorizeInternal(
    const QJsonObject &request, const SourceIdentity &currentSource,
    const QString &experimentId, const QJsonObject *expectedAuthorization, bool persist)
{
    QMutexLocker threadLock(&mutex);
    if (!isExperimentId(experimentId))
        return failure(QStringLiteral("EXPERIMENT_ID_INVALID"),
                       QStringLiteral("The preview experiment identifier is invalid."));
    if (!exactKeys(request, {QStringLiteral("schemaVersion"), QStringLiteral("sourceBinding"),
                             QStringLiteral("problemSpec"), QStringLiteral("budgets")},
                   {QStringLiteral("allowedNumericalRanges"),
                    QStringLiteral("stopConditions"), QStringLiteral("convergenceStudy")})
        || !isInteger(request.value(QStringLiteral("schemaVersion")), SchemaVersion, SchemaVersion)
        || !request.value(QStringLiteral("problemSpec")).isObject())
        return failure(QStringLiteral("EXPERIMENT_SPEC_INVALID"),
                       QStringLiteral("ExperimentSpec fields or schema version are invalid."));
    if (!boundedObject(request, 1024 * 1024))
        return failure(QStringLiteral("EXPERIMENT_SPEC_TOO_LARGE"),
                       QStringLiteral("ExperimentSpec exceeds the 1 MiB input limit."));
    const QJsonObject requestedProblemSpec = request.value(QStringLiteral("problemSpec")).toObject();
    if (!boundedObject(requestedProblemSpec, maximumProblemSpecBytes))
        return failure(QStringLiteral("PROBLEM_SPEC_TOO_LARGE"),
                       QStringLiteral("A durable ProblemSpec input must be at most 64 KiB."));

    SourceIdentity boundSource;
    if (!parseSourceBinding(request.value(QStringLiteral("sourceBinding")), &boundSource)
        || !currentSource.isValid() || boundSource.toJson() != currentSource.toJson())
        return failure(QStringLiteral("SOURCE_STALE"),
                       QStringLiteral("Authorization requires an exact current source binding."));

    const auto validation = RTrustedMechanicsService::validate(request.value(QStringLiteral("problemSpec")).toObject());
    if (!validation.valid)
        return failure(QStringLiteral("PROBLEM_SPEC_INVALID"),
                       QStringLiteral("The problem specification is not valid."),
                       {{QStringLiteral("issues"), validation.issues}});
    if (!boundedObject(validation.normalizedSpec, maximumProblemSpecBytes))
        return failure(QStringLiteral("PROBLEM_SPEC_TOO_LARGE"),
                       QStringLiteral("The normalized durable ProblemSpec must be at most 64 KiB."));

    QJsonArray ranges;
    if (request.contains(QStringLiteral("allowedNumericalRanges"))) {
        if (!request.value(QStringLiteral("allowedNumericalRanges")).isArray())
            return failure(QStringLiteral("EXPERIMENT_SPEC_INVALID"),
                           QStringLiteral("allowedNumericalRanges must be an array."));
        ranges = request.value(QStringLiteral("allowedNumericalRanges")).toArray();
    }
    QString detail;
    if (!validateRanges(ranges, validation.normalizedSpec, &detail))
        return failure(QStringLiteral("NUMERICAL_RANGE_INVALID"), detail);

    QJsonObject convergenceStudy;
    if (request.contains(QStringLiteral("convergenceStudy"))) {
        if (!request.value(QStringLiteral("convergenceStudy")).isObject())
            return failure(QStringLiteral("CONVERGENCE_STUDY_INVALID"),
                           QStringLiteral("convergenceStudy must be an object."));
        convergenceStudy = request.value(QStringLiteral("convergenceStudy")).toObject();
        const auto studyValidation = RTrustedConvergenceService::validateStudy(
            request.value(QStringLiteral("problemSpec")).toObject(), convergenceStudy);
        if (!studyValidation.ok)
            return failure(studyValidation.code, studyValidation.message, studyValidation.data);
        if (!studyRangesCoverLevels(validation.normalizedSpec, convergenceStudy, ranges, &detail))
            return failure(QStringLiteral("CONVERGENCE_RANGE_NOT_AUTHORIZED"), detail);
    }

    int solveBudget = 0, repairBudget = 0, modelTurnBudget = 0;
    if (!request.value(QStringLiteral("budgets")).isObject())
        return failure(QStringLiteral("BUDGET_INVALID"), QStringLiteral("An explicit budgets object is required."));
    const QJsonObject budgets = request.value(QStringLiteral("budgets")).toObject();
    if (!exactKeys(budgets, {QStringLiteral("solves"), QStringLiteral("repairs"), QStringLiteral("modelTurns")})
        || !isInteger(budgets.value(QStringLiteral("solves")), 1, HardSolveLimit, &solveBudget)
        || !isInteger(budgets.value(QStringLiteral("repairs")), 0, HardRepairLimit, &repairBudget)
        || !isInteger(budgets.value(QStringLiteral("modelTurns")), 1, HardModelTurnLimit, &modelTurnBudget))
        return failure(QStringLiteral("BUDGET_INVALID"),
                       QStringLiteral("Budgets must be explicit integers within the 8/3/12 hard caps."));

    QJsonObject stopConditions{{QStringLiteral("requirePassingVerification"), true},
                               {QStringLiteral("stopOnSourceChange"), true},
                               {QStringLiteral("stopOnUnclassifiedFailure"), true}};
    if (request.contains(QStringLiteral("stopConditions"))) {
        if (!request.value(QStringLiteral("stopConditions")).isObject())
            return failure(QStringLiteral("EXPERIMENT_SPEC_INVALID"),
                           QStringLiteral("stopConditions must be an object."));
        stopConditions = request.value(QStringLiteral("stopConditions")).toObject();
        if (!exactKeys(stopConditions, {QStringLiteral("requirePassingVerification"),
                                        QStringLiteral("stopOnSourceChange"),
                                        QStringLiteral("stopOnUnclassifiedFailure")})
            || stopConditions.value(QStringLiteral("requirePassingVerification")).toBool(false) != true
            || stopConditions.value(QStringLiteral("stopOnSourceChange")).toBool(false) != true
            || stopConditions.value(QStringLiteral("stopOnUnclassifiedFailure")).toBool(false) != true)
            return failure(QStringLiteral("STOP_CONDITIONS_INVALID"),
                           QStringLiteral("Verification, source-change and unclassified-failure stops are mandatory."));
    }

    const QJsonObject physics = RTrustedMechanicsService::physicalSummary(validation.normalizedSpec);
    const QJsonObject numerical = RTrustedMechanicsService::numericalSummary(validation.normalizedSpec);
    const QString problemDigest = RTrustedMechanicsService::summaryDigest(validation.normalizedSpec);
    const QString physicalDigest = RTrustedMechanicsService::summaryDigest(physics);
    const QString numericalDigest = RTrustedMechanicsService::summaryDigest(numerical);
    QJsonObject policy{{QStringLiteral("allowedNumericalRanges"), ranges},
                             {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), solveBudget},
                                                                      {QStringLiteral("repairs"), repairBudget},
                                                                      {QStringLiteral("modelTurns"), modelTurnBudget}}},
                             {QStringLiteral("stopConditions"), stopConditions}};
    if (!convergenceStudy.isEmpty())
        policy.insert(QStringLiteral("convergenceStudy"), convergenceStudy);
    QJsonObject authorization{{QStringLiteral("schema"), authorizationSchema},
                              {QStringLiteral("experimentId"), experimentId},
                              {QStringLiteral("sourceBinding"), boundSource.toJson()},
                              {QStringLiteral("problemSpecDigest"), problemDigest},
                              {QStringLiteral("physicalSummaryDigest"), physicalDigest},
                              {QStringLiteral("numericalConfigDigest"), numericalDigest},
                              {QStringLiteral("experimentPolicy"), policy}};
    const QString authorizationDigest = compactSha256(authorization);
    authorization.insert(QStringLiteral("authorizationDigest"), authorizationDigest);

    if (expectedAuthorization && authorization != *expectedAuthorization)
        return failure(QStringLiteral("EXPERIMENT_PREVIEW_MISMATCH"),
                       QStringLiteral("The request, source or authorization differs from the Host preview."));

    if (!persist)
        return success({{QStringLiteral("experimentId"), experimentId},
                        {QStringLiteral("authorization"), authorization},
                        {QStringLiteral("state"), QStringLiteral("preview")},
                        {QStringLiteral("problemSpecInput"), request.value(QStringLiteral("problemSpec"))},
                        {QStringLiteral("problemSpec"), validation.normalizedSpec}});

    QJsonObject record{{QStringLiteral("schemaVersion"), SchemaVersion},
                       {QStringLiteral("experimentId"), experimentId},
                       {QStringLiteral("state"), QStringLiteral("authorized")},
                       {QStringLiteral("authorization"), authorization},
                       {QStringLiteral("problemSpecInput"), request.value(QStringLiteral("problemSpec"))},
                       {QStringLiteral("problemSpec"), validation.normalizedSpec},
                       {QStringLiteral("candidateProblemSpecInput"), request.value(QStringLiteral("problemSpec"))},
                       {QStringLiteral("candidateProblemSpec"), validation.normalizedSpec},
                       {QStringLiteral("counters"), QJsonObject{{QStringLiteral("solves"), 0},
                                                                {QStringLiteral("repairs"), 0},
                                                                {QStringLiteral("modelTurns"), 0}}},
                       {QStringLiteral("history"), QJsonArray{}},
                       {QStringLiteral("attempts"), QJsonArray{}},
                       {QStringLiteral("failedConfigurations"), QJsonArray{}},
                       {QStringLiteral("convergenceLevels"), QJsonArray{}},
                       {QStringLiteral("convergenceChanges"), QJsonArray{}},
                       {QStringLiteral("convergence"), convergenceStudy.isEmpty() ? QJsonObject{} : QJsonObject{
                           {QStringLiteral("kind"), convergenceStudy.value(QStringLiteral("kind"))},
                           {QStringLiteral("status"), QStringLiteral("in_progress")},
                           {QStringLiteral("toleranceRelative"), 0.01},
                           {QStringLiteral("requiredConsecutiveChanges"), 2},
                           {QStringLiteral("maximumLevels"), convergenceStudy.value(QStringLiteral("maximumLevels"))},
                           {QStringLiteral("physicalSummaryDigest"), physicalDigest}}},
                       {QStringLiteral("cancelRequested"), false},
                       {QStringLiteral("repairInProgress"), false},
                       {QStringLiteral("createdAtUtc"), nowUtc()}};
    appendEvent(record, QStringLiteral("authorized"), QString(), QStringLiteral("authorized"),
                {{QStringLiteral("authorizationDigest"), authorizationDigest}});
    if (expectedAuthorization)
        setState(record, QStringLiteral("prepared"), QStringLiteral("created"));
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString lockError;
    if (!lockRecord(experimentId, &lock, &lockError))
        return failure(QStringLiteral("EXPERIMENT_STORAGE_LOCK_FAILED"), lockError);
    if (QFile::exists(recordPath(experimentId)))
        return failure(QStringLiteral("EXPERIMENT_ALREADY_EXISTS"),
                       QStringLiteral("The preview experiment identifier already has a durable record."));
    QString saveError;
    if (!saveRecord(experimentId, record, &saveError))
        return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), saveError);
    return success({{QStringLiteral("experimentId"), experimentId},
                    {QStringLiteral("authorization"), authorization},
                    {QStringLiteral("state"), record.value(QStringLiteral("state"))}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::create(
    const QString &experimentId, const SourceIdentity &currentSource)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message))
        return failure(code, message, {{QStringLiteral("state"), record.value(QStringLiteral("state"))}});
    const QString state = record.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("authorized")) {
        setState(record, QStringLiteral("prepared"), QStringLiteral("created"));
        if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    } else if (state != QLatin1String("prepared") && !terminalState(state)
               && state != QLatin1String("diagnosing") && state != QLatin1String("solving")
               && state != QLatin1String("verifying") && state != QLatin1String("repairing")
               && state != QLatin1String("needs_confirmation")) {
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("Experiment cannot be created from its current state."));
    }
    return success({{QStringLiteral("experimentId"), experimentId},
                    {QStringLiteral("state"), record.value(QStringLiteral("state"))},
                    {QStringLiteral("authorization"), record.value(QStringLiteral("authorization"))}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::run(
    const QString &experimentId, const SourceIdentity &currentSource)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message))
        return failure(code, message, {{QStringLiteral("state"), record.value(QStringLiteral("state"))}});
    QString state = record.value(QStringLiteral("state")).toString();
    const bool idempotent = state != QLatin1String("prepared");
    if (state == QLatin1String("prepared")) {
        setState(record, QStringLiteral("diagnosing"), QStringLiteral("run-requested"));
        state = QStringLiteral("diagnosing");
        if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    } else if (state != QLatin1String("diagnosing") && state != QLatin1String("solving")
               && state != QLatin1String("verifying") && state != QLatin1String("repairing")
               && state != QLatin1String("needs_confirmation") && !terminalState(state)) {
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("Experiment cannot run from its current state."));
    }
    return success({{QStringLiteral("experimentId"), experimentId},
                    {QStringLiteral("state"), state},
                    {QStringLiteral("idempotent"), idempotent}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::query(const QString &experimentId) const
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    return success({{QStringLiteral("experiment"), record}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::cancel(const QString &experimentId)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    const QString state = record.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("cancelled")) return success({{QStringLiteral("state"), state}, {QStringLiteral("idempotent"), true}});
    if (state == QLatin1String("candidate_ready") || state == QLatin1String("failed")
        || state == QLatin1String("budget_exhausted") || state == QLatin1String("not_converged"))
        return failure(QStringLiteral("EXPERIMENT_TERMINAL"), QStringLiteral("A terminal experiment cannot be cancelled."),
                       {{QStringLiteral("state"), state}});
    record.insert(QStringLiteral("cancelRequested"), true);
    finishActiveAttempt(&record, QStringLiteral("cancelled"), QStringLiteral("EXPERIMENT_CANCELLED"),
                        QStringLiteral("The experiment was cancelled before the active solve could be accepted."));
    setState(record, QStringLiteral("cancelled"), QStringLiteral("cancel-requested"));
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("state"), QStringLiteral("cancelled")},
                    {QStringLiteral("cancelRequested"), true}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::beginModelTurn(
    const QString &experimentId, const SourceIdentity &currentSource)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("diagnosing")
        && record.value(QStringLiteral("state")).toString() != QLatin1String("repairing"))
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("A model turn may start only during diagnosis or repair."));
    QJsonObject policy = record.value(QStringLiteral("authorization")).toObject()
        .value(QStringLiteral("experimentPolicy")).toObject();
    const int limit = policy.value(QStringLiteral("budgets")).toObject().value(QStringLiteral("modelTurns")).toInt();
    QJsonObject counters = countersOf(record);
    const int used = counters.value(QStringLiteral("modelTurns")).toInt();
    if (used >= limit) {
        setState(record, QStringLiteral("budget_exhausted"), QStringLiteral("model-turn-budget-exhausted"),
                 {{QStringLiteral("limit"), limit}});
        record.insert(QStringLiteral("terminalCode"), QStringLiteral("MODEL_TURN_BUDGET_EXHAUSTED"));
        if (!saveRecord(experimentId, record, &error))
            return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
        return failure(QStringLiteral("BUDGET_EXHAUSTED"), QStringLiteral("Model-turn budget exhausted."));
    }
    counters.insert(QStringLiteral("modelTurns"), used + 1);
    record.insert(QStringLiteral("counters"), counters);
    appendEvent(record, QStringLiteral("model-turn-started"), record.value(QStringLiteral("state")).toString(),
                record.value(QStringLiteral("state")).toString(), {{QStringLiteral("turn"), used + 1}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("modelTurn"), used + 1}, {QStringLiteral("limit"), limit}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::beginSolve(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QJsonObject &problemSpec, const QString &inputSnapshotSha256,
    const QString &inputPackageSha256)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("diagnosing"))
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("A solve may start only after diagnosis."));
    if (!isSha256(inputSnapshotSha256) || !isSha256(inputPackageSha256))
        return failure(QStringLiteral("INPUT_IDENTITY_INVALID"), QStringLiteral("Solve input snapshot/package SHA-256 is invalid."));
    const auto validation = RTrustedMechanicsService::validate(problemSpec);
    if (!validation.valid || validation.normalizedSpec != record.value(QStringLiteral("candidateProblemSpec")).toObject())
        return failure(QStringLiteral("CANDIDATE_SPEC_MISMATCH"), QStringLiteral("Solve ProblemSpec differs from the current authorized candidate."));
    const QJsonObject authorization = record.value(QStringLiteral("authorization")).toObject();
    if (!convergenceCandidateWithinPlan(problemSpec, validation.normalizedSpec, record,
                                        authorization, &message))
        return failure(QStringLiteral("CONVERGENCE_PLAN_BOUNDS_EXCEEDED"), message);
    if (!candidateWithinAuthorization(validation.normalizedSpec, authorization, &message))
        return failure(QStringLiteral("PHYSICS_OR_RANGE_MISMATCH"), message);
    const QString physicalDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::physicalSummary(validation.normalizedSpec));
    const QString numericalDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::numericalSummary(validation.normalizedSpec));
    const QString keyMaterial = physicalDigest + QLatin1Char('|') + numericalDigest + QLatin1Char('|')
        + inputSnapshotSha256 + QLatin1Char('|') + inputPackageSha256;
    const QString configurationKey = QString::fromLatin1(QCryptographicHash::hash(
        keyMaterial.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QJsonArray failed = record.value(QStringLiteral("failedConfigurations")).toArray();
    for (const QJsonValue &entry : failed)
        if (entry.toObject().value(QStringLiteral("key")).toString() == configurationKey)
            return failure(QStringLiteral("FAILED_CONFIGURATION_REPEATED"),
                           QStringLiteral("This physical, numerical and input identity already failed."));
    QJsonObject counters = countersOf(record);
    const int used = counters.value(QStringLiteral("solves")).toInt();
    const int limit = authorization.value(QStringLiteral("experimentPolicy")).toObject()
        .value(QStringLiteral("budgets")).toObject().value(QStringLiteral("solves")).toInt();
    if (used >= limit) {
        setState(record, QStringLiteral("budget_exhausted"), QStringLiteral("solve-budget-exhausted"),
                 {{QStringLiteral("limit"), limit}});
        record.insert(QStringLiteral("terminalCode"), QStringLiteral("SOLVE_BUDGET_EXHAUSTED"));
        if (!saveRecord(experimentId, record, &error))
            return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
        return failure(QStringLiteral("BUDGET_EXHAUSTED"), QStringLiteral("Solve budget exhausted."));
    }
    const QString attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject attempt{{QStringLiteral("attemptId"), attemptId},
                        {QStringLiteral("configurationKey"), configurationKey},
                        {QStringLiteral("physicalSummaryDigest"), physicalDigest},
                        {QStringLiteral("numericalConfigDigest"), numericalDigest},
                        {QStringLiteral("inputSnapshotSha256"), inputSnapshotSha256},
                        {QStringLiteral("inputPackageSha256"), inputPackageSha256},
                        {QStringLiteral("status"), QStringLiteral("running")},
                        {QStringLiteral("startedAtUtc"), nowUtc()}};
    QJsonArray attempts = record.value(QStringLiteral("attempts")).toArray();
    attempts.append(attempt);
    record.insert(QStringLiteral("attempts"), attempts);
    record.insert(QStringLiteral("activeAttempt"), attempt);
    counters.insert(QStringLiteral("solves"), used + 1);
    record.insert(QStringLiteral("counters"), counters);
    setState(record, QStringLiteral("solving"), QStringLiteral("solve-started"),
             {{QStringLiteral("attemptId"), attemptId}, {QStringLiteral("solve"), used + 1}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("attemptId"), attemptId},
                    {QStringLiteral("configurationKey"), configurationKey},
                    {QStringLiteral("solveNumber"), used + 1},
                    {QStringLiteral("authorization"), authorization}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::finishSolve(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QString &attemptId, bool succeeded, const QJsonObject &solveIdentity,
    const QString &failureDisposition, const QString &failureCode, const QString &failureMessage)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("solving"))
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("No solve is currently active."));
    QJsonObject attempt = record.value(QStringLiteral("activeAttempt")).toObject();
    if (attempt.isEmpty() || attempt.value(QStringLiteral("attemptId")).toString() != attemptId
        || attempt.value(QStringLiteral("status")).toString() != QLatin1String("running"))
        return failure(QStringLiteral("ATTEMPT_IDENTITY_MISMATCH"), QStringLiteral("Solve completion does not match the active attempt."));
    QJsonArray attempts = record.value(QStringLiteral("attempts")).toArray();
    for (int i = 0; i < attempts.size(); ++i)
        if (attempts.at(i).toObject().value(QStringLiteral("attemptId")).toString() == attemptId) {
            QJsonObject savedAttempt = attempts.at(i).toObject();
            savedAttempt.insert(QStringLiteral("finishedAtUtc"), nowUtc());
            if (succeeded) {
                if (!validSolveIdentity(solveIdentity)
                    || solveIdentity.value(QStringLiteral("inputSnapshotSha256")).toString()
                        != attempt.value(QStringLiteral("inputSnapshotSha256")).toString()
                    || solveIdentity.value(QStringLiteral("inputPackageSha256")).toString()
                        != attempt.value(QStringLiteral("inputPackageSha256")).toString())
                    return failure(QStringLiteral("SOLVE_IDENTITY_INVALID"), QStringLiteral("Successful solve requires a complete identity matching its input snapshot."));
                savedAttempt.insert(QStringLiteral("status"), QStringLiteral("completed"));
                savedAttempt.insert(QStringLiteral("solveIdentity"), solveIdentity);
                record.insert(QStringLiteral("latestSolveIdentity"), solveIdentity);
                record.insert(QStringLiteral("latestSolveAttemptId"), attemptId);
                attempts.replace(i, savedAttempt);
                record.insert(QStringLiteral("attempts"), attempts);
                record.insert(QStringLiteral("activeAttempt"), QJsonObject{});
                setState(record, QStringLiteral("verifying"), QStringLiteral("solve-completed"),
                         {{QStringLiteral("attemptId"), attemptId}});
            } else {
                if (failureDisposition != QLatin1String("numericRepairAllowed")
                    && failureDisposition != QLatin1String("needsConfirmation")
                    && failureDisposition != QLatin1String("stop"))
                    return failure(QStringLiteral("FAILURE_DISPOSITION_INVALID"), QStringLiteral("Failure disposition must be explicit."));
                if (failureCode.trimmed().isEmpty() || failureCode.size() > 128 || failureMessage.size() > 2048)
                    return failure(QStringLiteral("FAILURE_EVIDENCE_INVALID"), QStringLiteral("Failure evidence exceeds its bounded size."));
                savedAttempt.insert(QStringLiteral("status"), QStringLiteral("failed"));
                savedAttempt.insert(QStringLiteral("failure"), failureEvidence(failureCode, failureMessage));
                attempts.replace(i, savedAttempt);
                record.insert(QStringLiteral("attempts"), attempts);
                record.insert(QStringLiteral("activeAttempt"), QJsonObject{});
                QJsonArray failed = record.value(QStringLiteral("failedConfigurations")).toArray();
                failed.append(QJsonObject{{QStringLiteral("key"), attempt.value(QStringLiteral("configurationKey"))},
                                         {QStringLiteral("physicalSummaryDigest"), attempt.value(QStringLiteral("physicalSummaryDigest"))},
                                         {QStringLiteral("numericalConfigDigest"), attempt.value(QStringLiteral("numericalConfigDigest"))},
                                         {QStringLiteral("inputSnapshotSha256"), attempt.value(QStringLiteral("inputSnapshotSha256"))},
                                         {QStringLiteral("inputPackageSha256"), attempt.value(QStringLiteral("inputPackageSha256"))},
                                         {QStringLiteral("failure"), failureEvidence(failureCode, failureMessage)}});
                record.insert(QStringLiteral("failedConfigurations"), failed);
                const int repairUsed = countersOf(record).value(QStringLiteral("repairs")).toInt();
                const int repairLimit = record.value(QStringLiteral("authorization")).toObject()
                    .value(QStringLiteral("experimentPolicy")).toObject().value(QStringLiteral("budgets"))
                    .toObject().value(QStringLiteral("repairs")).toInt();
                if (failureDisposition == QLatin1String("numericRepairAllowed") && repairUsed < repairLimit)
                    setState(record, QStringLiteral("repairing"), QStringLiteral("solve-failed-repair-allowed"),
                             failureEvidence(failureCode, failureMessage));
                else if (failureDisposition == QLatin1String("needsConfirmation"))
                    setState(record, QStringLiteral("needs_confirmation"), QStringLiteral("solve-failed-needs-confirmation"),
                             failureEvidence(failureCode, failureMessage));
                else {
                    setState(record, QStringLiteral("failed"), QStringLiteral("solve-failed-stopped"),
                             failureEvidence(failureCode, failureMessage));
                    record.insert(QStringLiteral("terminalCode"), failureCode);
                }
            }
            if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
            return success({{QStringLiteral("state"), record.value(QStringLiteral("state"))},
                            {QStringLiteral("attemptId"), attemptId}});
        }
    return failure(QStringLiteral("ATTEMPT_NOT_FOUND"), QStringLiteral("Active solve attempt is missing from history."));
}

RTrustedExperimentService::Outcome RTrustedExperimentService::claimSolveSubmission(
    const QString &experimentId, const SourceIdentity &currentSource, const QString &attemptId)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    QJsonObject active = record.value(QStringLiteral("activeAttempt")).toObject();
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("solving")
        || active.value(QStringLiteral("attemptId")).toString() != attemptId
        || active.value(QStringLiteral("status")).toString() != QLatin1String("running"))
        return failure(QStringLiteral("ATTEMPT_IDENTITY_MISMATCH"),
                       QStringLiteral("Submission claim does not match the active solve attempt."));
    if (active.value(QStringLiteral("submissionClaimed")).toBool())
        return failure(QStringLiteral("ATTEMPT_ALREADY_SUBMITTED"),
                       QStringLiteral("This solve attempt has already consumed its one-shot submission claim."));
    active.insert(QStringLiteral("submissionClaimed"), true);
    active.insert(QStringLiteral("submissionClaimedAtUtc"), nowUtc());
    record.insert(QStringLiteral("activeAttempt"), active);
    QJsonArray attempts = record.value(QStringLiteral("attempts")).toArray();
    for (int i = 0; i < attempts.size(); ++i) {
        QJsonObject saved = attempts.at(i).toObject();
        if (saved.value(QStringLiteral("attemptId")).toString() != attemptId) continue;
        saved.insert(QStringLiteral("submissionClaimed"), true);
        saved.insert(QStringLiteral("submissionClaimedAtUtc"), active.value(QStringLiteral("submissionClaimedAtUtc")));
        attempts.replace(i, saved);
        break;
    }
    record.insert(QStringLiteral("attempts"), attempts);
    appendEvent(record, QStringLiteral("solve-submission-claimed"), QStringLiteral("solving"),
                QStringLiteral("solving"), {{QStringLiteral("attemptId"), attemptId}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("attemptId"), attemptId}, {QStringLiteral("submissionClaimed"), true}});
}

bool RTrustedExperimentService::proposeAuthorizedMeshRefinement(
    const QJsonObject &problemSpec, const QJsonObject &authorization,
    const QJsonObject &verificationReport, QJsonObject *repairedProblemSpec, QString *reason)
{
    if (!repairedProblemSpec || verificationReport.value(QStringLiteral("status")).toString() != QLatin1String("fail"))
        return false;
    const QJsonObject metrics = verificationReport.value(QStringLiteral("metrics")).toObject();
    bool precisionFailure = false;
    QString failedMetric;
    for (const QString &metricKey : metrics.keys()) {
        const QJsonObject metric = metrics.value(metricKey).toObject();
        if (!metric.value(QStringLiteral("required")).toBool()
            || metric.value(QStringLiteral("status")).toString() == QLatin1String("pass")) continue;
        if (metric.value(QStringLiteral("status")).toString() != QLatin1String("fail")
            || (metricKey != QLatin1String("displacement") && metricKey != QLatin1String("frequency")))
            return false;
        const QJsonObject evidence = metric.value(QStringLiteral("evidence")).toObject();
        const QString errorKey = metricKey == QLatin1String("frequency")
                ? QStringLiteral("relativeError") : QStringLiteral("maximumRelativeError");
        const double observedError = evidence.value(errorKey).toDouble(-1.0);
        const double limit = metricKey == QLatin1String("frequency")
                ? verificationReport.value(QStringLiteral("thresholds")).toObject()
                    .value(QStringLiteral("effectiveFrequencyRelative")).toDouble(-1.0)
                : evidence.value(QStringLiteral("effectiveRelativeLimit")).toDouble(-1.0);
        if (!std::isfinite(observedError) || !std::isfinite(limit) || observedError <= limit) return false;
        precisionFailure = true;
        failedMetric = metricKey;
    }
    if (!precisionFailure) return false;

    const QJsonArray ranges = authorization.value(QStringLiteral("experimentPolicy")).toObject()
        .value(QStringLiteral("allowedNumericalRanges")).toArray();
    for (const QJsonValue &value : ranges) {
        const QJsonObject range = value.toObject();
        if (range.value(QStringLiteral("path")).toString() != QLatin1String("/geometry/segments")) continue;
        const QJsonObject geometry = problemSpec.value(QStringLiteral("geometry")).toObject();
        const int current = geometry.value(QStringLiteral("segments")).toInt(-1);
        const double minimum = range.value(QStringLiteral("minimum")).toDouble(-1.0);
        const double maximum = range.value(QStringLiteral("maximum")).toDouble(-1.0);
        if (current < 1 || !std::isfinite(minimum) || !std::isfinite(maximum)
            || std::floor(minimum) != minimum || std::floor(maximum) != maximum
            || current < minimum || current >= maximum) return false;
        const int next = qMin(int(maximum), qMax(current + 1, current * 2));
        if (next <= current) return false;
        QJsonObject refinedGeometry = geometry;
        refinedGeometry.insert(QStringLiteral("segments"), next);
        *repairedProblemSpec = problemSpec;
        repairedProblemSpec->insert(QStringLiteral("geometry"), refinedGeometry);
        if (reason)
            *reason = QStringLiteral("The independent %1 reference exceeded its fixed precision limit; authorized uniform mesh refinement increased segments from %2 to %3.")
                .arg(failedMetric).arg(current).arg(next);
        return true;
    }
    return false;
}

RTrustedExperimentService::Outcome RTrustedExperimentService::beginRepair(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QJsonObject &repairedProblemSpec)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("repairing")
        || record.value(QStringLiteral("repairInProgress")).toBool())
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("A numerical repair is not currently permitted."));
    const QJsonObject authorization = record.value(QStringLiteral("authorization")).toObject();
    if (!boundedObject(repairedProblemSpec, maximumProblemSpecBytes))
        return failure(QStringLiteral("PROBLEM_SPEC_TOO_LARGE"),
                       QStringLiteral("A durable repair ProblemSpec input must be at most 64 KiB."));
    const auto validation = RTrustedMechanicsService::validate(repairedProblemSpec);
    if (!validation.valid) return failure(QStringLiteral("PROBLEM_SPEC_INVALID"), QStringLiteral("Repaired ProblemSpec is invalid."));
    if (!boundedObject(validation.normalizedSpec, maximumProblemSpecBytes))
        return failure(QStringLiteral("PROBLEM_SPEC_TOO_LARGE"),
                       QStringLiteral("The normalized durable repair ProblemSpec must be at most 64 KiB."));
    const QJsonObject current = record.value(QStringLiteral("candidateProblemSpec")).toObject();
    QStringList diffPaths;
    collectDiffPaths(current, validation.normalizedSpec, QString(), &diffPaths);
    const QJsonArray changedPaths = QJsonArray::fromStringList(diffPaths);
    if (!boundedObject(QJsonObject{{QStringLiteral("changedPaths"), changedPaths}}, maximumEvidenceBytes))
        return failure(QStringLiteral("REPAIR_DIFF_TOO_LARGE"),
                       QStringLiteral("Repair path evidence exceeds the 64 KiB durable bound."));
    if (!convergenceCandidateWithinPlan(repairedProblemSpec, validation.normalizedSpec,
                                        record, authorization, &message))
        return failure(QStringLiteral("CONVERGENCE_PLAN_BOUNDS_EXCEEDED"), message);
    const QJsonArray ranges = authorization.value(QStringLiteral("experimentPolicy")).toObject()
        .value(QStringLiteral("allowedNumericalRanges")).toArray();
    QSet<QString> allowed;
    for (const QJsonValue &value : ranges) allowed.insert(value.toObject().value(QStringLiteral("path")).toString());
    bool unauthorizedDiff = diffPaths.isEmpty();
    for (const QString &path : diffPaths)
        if (!allowed.contains(path)) unauthorizedDiff = true;
    if (unauthorizedDiff || !candidateWithinAuthorization(validation.normalizedSpec, authorization, &message)) {
        const QString reason = diffPaths.isEmpty() ? QStringLiteral("A repair must change an authorized numerical field.")
                                                   : QStringLiteral("Repair includes a physics change or an unauthorized numerical field.");
        setState(record, QStringLiteral("needs_confirmation"), QStringLiteral("repair-rejected"),
                 {{QStringLiteral("reason"), reason}, {QStringLiteral("changedPaths"), changedPaths}});
        record.insert(QStringLiteral("terminalCode"), QStringLiteral("REPAIR_NEEDS_CONFIRMATION"));
        if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
        return failure(QStringLiteral("REPAIR_NEEDS_CONFIRMATION"), reason,
                       {{QStringLiteral("state"), QStringLiteral("needs_confirmation")}});
    }
    QJsonObject counters = countersOf(record);
    const int used = counters.value(QStringLiteral("repairs")).toInt();
    const int limit = authorization.value(QStringLiteral("experimentPolicy")).toObject()
        .value(QStringLiteral("budgets")).toObject().value(QStringLiteral("repairs")).toInt();
    if (used >= limit) {
        setState(record, QStringLiteral("budget_exhausted"), QStringLiteral("repair-budget-exhausted"),
                 {{QStringLiteral("limit"), limit}});
        record.insert(QStringLiteral("terminalCode"), QStringLiteral("REPAIR_BUDGET_EXHAUSTED"));
        if (!saveRecord(experimentId, record, &error))
            return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
        return failure(QStringLiteral("BUDGET_EXHAUSTED"), QStringLiteral("Repair budget exhausted."));
    }
    counters.insert(QStringLiteral("repairs"), used + 1);
    record.insert(QStringLiteral("counters"), counters);
    // Verification idempotency is per completed child result, not per whole
    // experiment. A repaired candidate must be allowed to bind its own result.
    record.remove(QStringLiteral("verificationRequestDigest"));
    record.insert(QStringLiteral("pendingRepairProblemSpecInput"), repairedProblemSpec);
    record.insert(QStringLiteral("pendingRepairProblemSpec"), validation.normalizedSpec);
    record.insert(QStringLiteral("repairInProgress"), true);
    appendEvent(record, QStringLiteral("repair-started"), QStringLiteral("repairing"), QStringLiteral("repairing"),
                {{QStringLiteral("repair"), used + 1},
                 {QStringLiteral("changedPaths"), changedPaths}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("repairNumber"), used + 1},
                    {QStringLiteral("problemSpec"), validation.normalizedSpec}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::finishRepair(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QJsonObject &repairedProblemSpec, const QJsonObject &observedPhysicalSummary)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("repairing")
        || !record.value(QStringLiteral("repairInProgress")).toBool())
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("No numerical repair is being verified."));
    const auto validation = RTrustedMechanicsService::validate(repairedProblemSpec);
    if (!validation.valid
        || validation.normalizedSpec != record.value(QStringLiteral("pendingRepairProblemSpec")).toObject()) {
        return failure(QStringLiteral("REPAIR_CANDIDATE_MISMATCH"), QStringLiteral("Repair completion differs from its persisted candidate."));
    }
    const QJsonObject expectedPhysical = RTrustedMechanicsService::physicalSummary(validation.normalizedSpec);
    if (RTrustedMechanicsService::summaryDigest(observedPhysicalSummary)
            != RTrustedMechanicsService::summaryDigest(expectedPhysical)
        || RTrustedMechanicsService::summaryDigest(expectedPhysical)
            != record.value(QStringLiteral("authorization")).toObject()
                .value(QStringLiteral("physicalSummaryDigest")).toString()) {
        setState(record, QStringLiteral("needs_confirmation"), QStringLiteral("repair-observed-physics-mismatch"));
        record.insert(QStringLiteral("repairInProgress"), false);
        record.insert(QStringLiteral("terminalCode"), QStringLiteral("OBSERVED_PHYSICS_MISMATCH"));
        if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
        return failure(QStringLiteral("OBSERVED_PHYSICS_MISMATCH"),
                       QStringLiteral("Observed candidate physics differs from the authorized physical summary."));
    }
    record.insert(QStringLiteral("candidateProblemSpec"), validation.normalizedSpec);
    record.insert(QStringLiteral("candidateProblemSpecInput"),
                  record.value(QStringLiteral("pendingRepairProblemSpecInput")));
    record.remove(QStringLiteral("pendingRepairProblemSpec"));
    record.remove(QStringLiteral("pendingRepairProblemSpecInput"));
    record.insert(QStringLiteral("repairInProgress"), false);
    const QString numericDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::numericalSummary(validation.normalizedSpec));
    record.insert(QStringLiteral("candidateNumericalConfigDigest"), numericDigest);
    setState(record, QStringLiteral("diagnosing"), QStringLiteral("repair-verified"),
             {{QStringLiteral("numericalConfigDigest"), numericDigest},
              {QStringLiteral("observedPhysicalSummaryDigest"), RTrustedMechanicsService::summaryDigest(observedPhysicalSummary)}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("state"), QStringLiteral("diagnosing")},
                    {QStringLiteral("candidateNumericalConfigDigest"), numericDigest}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::markNeedsConfirmation(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QString &reason, const QJsonObject &evidence)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    const QString state = record.value(QStringLiteral("state")).toString();
    if (terminalState(state) || state == QLatin1String("needs_confirmation"))
        return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("Confirmation cannot be requested from this state."));
    if (reason.trimmed().isEmpty() || reason.size() > 2048)
        return failure(QStringLiteral("CONFIRMATION_REASON_INVALID"), QStringLiteral("A bounded confirmation reason is required."));
    if (!boundedObject(evidence, 64 * 1024))
        return failure(QStringLiteral("CONFIRMATION_EVIDENCE_TOO_LARGE"), QStringLiteral("Confirmation evidence exceeds 64 KiB."));
    record.insert(QStringLiteral("repairInProgress"), false);
    setState(record, QStringLiteral("needs_confirmation"), QStringLiteral("confirmation-required"),
             {{QStringLiteral("reason"), reason}, {QStringLiteral("evidence"), evidence}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("state"), QStringLiteral("needs_confirmation")},
                    {QStringLiteral("reason"), reason}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::stop(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QString &code, const QString &message, const QJsonObject &evidence)
{
    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString authCode, authMessage;
    if (!authorizeMutation(&record, currentSource, &authCode, &authMessage)) return failure(authCode, authMessage);
    const QString state = record.value(QStringLiteral("state")).toString();
    if (terminalState(state)) return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("A terminal experiment cannot be stopped again."));
    if (code.trimmed().isEmpty() || code.size() > 128 || message.size() > 2048)
        return failure(QStringLiteral("STOP_REASON_INVALID"), QStringLiteral("A bounded stop code and message are required."));
    if (!boundedObject(evidence, 64 * 1024))
        return failure(QStringLiteral("STOP_EVIDENCE_TOO_LARGE"), QStringLiteral("Stop evidence exceeds 64 KiB."));
    record.insert(QStringLiteral("terminalCode"), code);
    setState(record, QStringLiteral("failed"), QStringLiteral("stopped"),
             {{QStringLiteral("code"), code}, {QStringLiteral("message"), message},
              {QStringLiteral("evidence"), evidence}});
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("state"), QStringLiteral("failed")}, {QStringLiteral("terminalCode"), code}});
}

RTrustedExperimentService::Outcome RTrustedExperimentService::verifyCandidate(
    const QString &experimentId, const SourceIdentity &currentSource,
    const QJsonObject &expectedIdentity, const QString &manifestPath,
    const QString &manifestSha256, const QJsonObject &costEvidence)
{
    if (!isSha256(manifestSha256)
        || !exactKeys(expectedIdentity, {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                                         QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256"),
                                         QStringLiteral("sourceSnapshotRef")})
        || !validSolveIdentity(expectedIdentity))
        return failure(QStringLiteral("VERIFICATION_IDENTITY_INVALID"), QStringLiteral("Verifier input identity is incomplete."));
    if (!boundedObject(costEvidence, maximumEvidenceBytes))
        return failure(QStringLiteral("VERIFICATION_COST_EVIDENCE_TOO_LARGE"),
                       QStringLiteral("Cost evidence exceeds the 64 KiB durable bound."));

    QJsonObject candidateInput;
    QJsonObject candidateNormalized;
    QJsonObject convergenceStudy;
    QString requestDigest;
    {
        QMutexLocker threadLock(&mutex);
        QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
        QString error;
        if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
        QJsonObject record;
        if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
        QString code, message;
        if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
        const QString state = record.value(QStringLiteral("state")).toString();
        if (state != QLatin1String("verifying"))
            return failure(QStringLiteral("TRANSITION_INVALID"), QStringLiteral("Candidate verification requires a completed solve."));
        const QJsonObject activeIdentity = record.value(QStringLiteral("latestSolveIdentity")).toObject();
        if (activeIdentity != expectedIdentity)
            return failure(QStringLiteral("VERIFICATION_IDENTITY_MISMATCH"), QStringLiteral("Verifier identity differs from the completed solve."));
        requestDigest = RTrustedMechanicsService::summaryDigest(
            QJsonObject{{QStringLiteral("expectedIdentity"), expectedIdentity},
                        {QStringLiteral("manifestSha256"), manifestSha256}});
        const QString previousRequest = record.value(QStringLiteral("verificationRequestDigest")).toString();
        if (!previousRequest.isEmpty() && previousRequest != requestDigest)
            return failure(QStringLiteral("VERIFICATION_REQUEST_MISMATCH"), QStringLiteral("A different result identity is already being verified."));
        record.insert(QStringLiteral("verificationRequestDigest"), requestDigest);
        if (previousRequest.isEmpty())
            appendEvent(record, QStringLiteral("verification-started"), state, state,
                        {{QStringLiteral("requestDigest"), requestDigest}});
        candidateInput = record.value(QStringLiteral("candidateProblemSpecInput")).toObject();
        candidateNormalized = record.value(QStringLiteral("candidateProblemSpec")).toObject();
        convergenceStudy = record.value(QStringLiteral("authorization")).toObject()
            .value(QStringLiteral("experimentPolicy")).toObject()
            .value(QStringLiteral("convergenceStudy")).toObject();
        if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    }

    // This is the only path that can attach a VerificationReport to an experiment.
    QElapsedTimer verificationClock;
    verificationClock.start();
    const QJsonObject report = RTrustedVerificationService::verify(candidateInput, expectedIdentity,
                                                                    manifestPath, manifestSha256,
                                                                    !convergenceStudy.isEmpty());
    const qint64 verificationElapsedMs = verificationClock.elapsed();
    if (!convergenceStudy.isEmpty()
        && !boundedConvergenceVerificationReport(report, candidateNormalized))
        return failure(QStringLiteral("VERIFICATION_REPORT_TOO_LARGE"),
                       QStringLiteral("The transient verification report exceeds its preflighted durable shape."));
    const QJsonObject reportIdentity = report.value(QStringLiteral("identity")).toObject();
    const bool identityBound = report.value(QStringLiteral("schema")).toString() == reportSchema
        && reportIdentity.value(QStringLiteral("artifactId")) == expectedIdentity.value(QStringLiteral("artifactId"))
        && reportIdentity.value(QStringLiteral("jobId")) == expectedIdentity.value(QStringLiteral("jobId"))
        && reportIdentity.value(QStringLiteral("inputSnapshotSha256")) == expectedIdentity.value(QStringLiteral("inputSnapshotSha256"))
        && reportIdentity.value(QStringLiteral("inputPackageSha256")) == expectedIdentity.value(QStringLiteral("inputPackageSha256"))
        && reportIdentity.value(QStringLiteral("sourceSnapshotRef")) == expectedIdentity.value(QStringLiteral("sourceSnapshotRef"))
        && reportIdentity.value(QStringLiteral("normalizedProblemSpecSha256")).toString() == normalizedSpecSha256(candidateNormalized)
        && reportIdentity.value(QStringLiteral("physicalSummarySha256")).toString()
            == RTrustedMechanicsService::summaryDigest(RTrustedMechanicsService::physicalSummary(candidateNormalized));
    const bool passed = identityBound && report.value(QStringLiteral("passed")).isBool()
        && report.value(QStringLiteral("passed")).toBool()
        && report.value(QStringLiteral("status")).toString() == QLatin1String("pass");

    QMutexLocker threadLock(&mutex);
    QLockFile lock(recordPath(experimentId) + QStringLiteral(".lock"));
    QString error;
    if (!lockRecord(experimentId, &lock, &error)) return failure(QStringLiteral("EXPERIMENT_LOCK_FAILED"), error);
    QJsonObject record;
    if (!loadRecord(experimentId, &record, &error)) return failure(QStringLiteral("EXPERIMENT_NOT_FOUND"), error);
    QString code, message;
    if (!authorizeMutation(&record, currentSource, &code, &message)) return failure(code, message);
    if (record.value(QStringLiteral("state")).toString() != QLatin1String("verifying")
        || record.value(QStringLiteral("verificationRequestDigest")).toString() != requestDigest)
        return failure(QStringLiteral("VERIFICATION_STATE_CHANGED"), QStringLiteral("Experiment state changed during verification."));
    const QJsonObject previousReport = record.value(QStringLiteral("verificationReport")).toObject();
    const QJsonObject previousMeasurements = previousReport.value(QStringLiteral("convergenceMeasurements")).toObject();
    const bool previousVerificationPassed = record.value(QStringLiteral("verificationPassed")).toBool();
    record.insert(QStringLiteral("verificationReport"), report);
    record.insert(QStringLiteral("verificationPassed"), passed);
    if (!convergenceStudy.isEmpty()) {
        QJsonArray levels = record.value(QStringLiteral("convergenceLevels")).toArray();
        QJsonArray changes = record.value(QStringLiteral("convergenceChanges")).toArray();
        const QJsonObject measurements = report.value(QStringLiteral("convergenceMeasurements")).toObject();
        const auto selfComparison = RTrustedConvergenceService::compareMeasurements(
            convergenceStudy.value(QStringLiteral("kind")).toString(), measurements, measurements);
        const int levelIndex = levels.size();
        const QJsonObject levelSpec = record.value(QStringLiteral("candidateProblemSpec")).toObject();
        const int segments = levelSpec.value(QStringLiteral("geometry")).toObject()
            .value(QStringLiteral("segments")).toInt();
        const bool beam = levelSpec.value(QStringLiteral("modelType")).toString() == QLatin1String("cantileverBeam");
        QJsonObject cost = costEvidence;
        cost.insert(QStringLiteral("verificationElapsedMs"), double(verificationElapsedMs));
        cost.insert(QStringLiteral("manifestBytes"), double(QFileInfo(manifestPath).size()));
        cost.insert(QStringLiteral("nodeCount"), segments + 1);
        cost.insert(QStringLiteral("elementCount"), segments);
        cost.insert(QStringLiteral("totalDofCount"), (segments + 1) * (beam ? 6 : 1));
        const QJsonObject analysis = levelSpec.value(QStringLiteral("analysis")).toObject();
        if (analysis.value(QStringLiteral("kind")).toString() == QLatin1String("linearTransient")) {
            cost.insert(QStringLiteral("timeStepS"), analysis.value(QStringLiteral("timeStep")));
            cost.insert(QStringLiteral("steps"), analysis.value(QStringLiteral("steps")));
            cost.insert(QStringLiteral("recordCount"), analysis.value(QStringLiteral("steps")));
        } else {
            cost.insert(QStringLiteral("recordCount"), 1);
        }
        QJsonObject level{{QStringLiteral("level"), levelIndex + 1},
            {QStringLiteral("numericalConfiguration"), RTrustedMechanicsService::numericalSummary(levelSpec)},
            {QStringLiteral("physicalSummaryDigest"),
                RTrustedMechanicsService::summaryDigest(RTrustedMechanicsService::physicalSummary(levelSpec))},
            {QStringLiteral("verificationPassed"), passed},
            {QStringLiteral("verificationReport"), compactVerificationReport(report)},
            {QStringLiteral("measurementSummary"), summarizeConvergenceMeasurements(measurements)},
            {QStringLiteral("measurementSha256"), compactSha256(measurements)},
            {QStringLiteral("artifactId"), expectedIdentity.value(QStringLiteral("artifactId"))},
            {QStringLiteral("jobId"), expectedIdentity.value(QStringLiteral("jobId"))},
            {QStringLiteral("manifestSha256"), manifestSha256},
            {QStringLiteral("cost"), cost}};
        bool comparisonFailed = false;
        if (passed && previousVerificationPassed && !previousMeasurements.isEmpty() && !levels.isEmpty()
            && levels.last().toObject().value(QStringLiteral("verificationPassed")).toBool()) {
            const auto comparison = RTrustedConvergenceService::compareMeasurements(
                convergenceStudy.value(QStringLiteral("kind")).toString(),
                previousMeasurements, measurements);
            if (comparison.ok) {
                const QJsonObject change = comparison.data;
                level.insert(QStringLiteral("changeFromPrevious"), change);
                changes.append(change);
            } else {
                changes = {};
                comparisonFailed = true;
                level.insert(QStringLiteral("comparisonError"), QJsonObject{
                    {QStringLiteral("code"), comparison.code},
                    {QStringLiteral("message"), comparison.message}});
            }
        } else {
            // A failed or skipped verification breaks the consecutive-level chain.
            changes = {};
        }
        levels.append(level);
        record.insert(QStringLiteral("convergenceLevels"), levels);
        record.insert(QStringLiteral("convergenceChanges"), changes);

        QJsonObject convergence = record.value(QStringLiteral("convergence")).toObject();
        convergence.insert(QStringLiteral("levelsCompleted"), levels.size());
        convergence.insert(QStringLiteral("adjacentChanges"), changes);
        const auto convergenceCosts = [&]() {
            double solveMs = 0.0, verifyMs = 0.0, manifestBytes = 0.0;
            for (const QJsonValue &entry : levels) {
                const QJsonObject entryCost = entry.toObject().value(QStringLiteral("cost")).toObject();
                solveMs += entryCost.value(QStringLiteral("solveElapsedMs")).toDouble();
                verifyMs += entryCost.value(QStringLiteral("verificationElapsedMs")).toDouble();
                manifestBytes += entryCost.value(QStringLiteral("manifestBytes")).toDouble();
            }
            return QJsonObject{{QStringLiteral("solveElapsedMs"), solveMs},
                {QStringLiteral("verificationElapsedMs"), verifyMs},
                {QStringLiteral("manifestBytes"), manifestBytes},
                {QStringLiteral("solveCount"), levels.size()}};
        };
        convergence.insert(QStringLiteral("cost"), convergenceCosts());
        const QJsonObject policy = record.value(QStringLiteral("authorization")).toObject()
            .value(QStringLiteral("experimentPolicy")).toObject();
        const QJsonObject budgets = policy.value(QStringLiteral("budgets")).toObject();
        const int maximumLevels = convergenceStudy.value(QStringLiteral("maximumLevels")).toInt();
        const int solvesUsed = countersOf(record).value(QStringLiteral("solves")).toInt();
        const int repairsUsed = countersOf(record).value(QStringLiteral("repairs")).toInt();
        const int solveLimit = budgets.value(QStringLiteral("solves")).toInt();
        const int repairLimit = budgets.value(QStringLiteral("repairs")).toInt();
        const bool measurementValid = selfComparison.ok;
        const auto convergenceTerminal = [&](const QString &status, const QString &reason,
                                             const QString &state, const QString &event) {
            convergence.insert(QStringLiteral("status"), status);
            convergence.insert(QStringLiteral("stopReason"), reason);
            record.insert(QStringLiteral("convergence"), convergence);
            record.insert(QStringLiteral("terminalCode"), reason.toUpper());
            setState(record, state, event,
                     {{QStringLiteral("stopReason"), reason},
                      {QStringLiteral("levelsCompleted"), levels.size()}});
        };
        if (passed && !measurementValid) {
            convergenceTerminal(QStringLiteral("not_converged"), QStringLiteral("measurement_unavailable"),
                                QStringLiteral("not_converged"), QStringLiteral("convergence-measurement-unavailable"));
        } else if (passed && comparisonFailed) {
            convergenceTerminal(QStringLiteral("not_converged"), QStringLiteral("measurement_comparison_invalid"),
                                QStringLiteral("not_converged"), QStringLiteral("convergence-comparison-invalid"));
        } else if (passed && RTrustedConvergenceService::isConverged(changes)) {
            convergence.insert(QStringLiteral("status"), QStringLiteral("converged"));
            convergence.insert(QStringLiteral("stopReason"), QStringLiteral("two_consecutive_changes_below_tolerance"));
            record.insert(QStringLiteral("convergence"), convergence);
            setState(record, QStringLiteral("candidate_ready"), QStringLiteral("convergence-accepted"),
                     {{QStringLiteral("levelsCompleted"), levels.size()},
                      {QStringLiteral("stopReason"), QStringLiteral("two_consecutive_changes_below_tolerance")}});
        } else if (levels.size() >= maximumLevels) {
            convergenceTerminal(QStringLiteral("not_converged"), QStringLiteral("maximum_levels_reached"),
                                QStringLiteral("not_converged"), QStringLiteral("convergence-level-cap"));
        } else if (solvesUsed >= solveLimit) {
            convergenceTerminal(QStringLiteral("not_converged"), QStringLiteral("solve_budget_exhausted"),
                                QStringLiteral("budget_exhausted"), QStringLiteral("convergence-solve-budget-exhausted"));
        } else if (repairsUsed >= repairLimit) {
            convergenceTerminal(QStringLiteral("not_converged"), QStringLiteral("repair_budget_exhausted"),
                                QStringLiteral("budget_exhausted"), QStringLiteral("convergence-repair-budget-exhausted"));
        } else if (!passed) {
            convergence.insert(QStringLiteral("status"), QStringLiteral("in_progress"));
            convergence.insert(QStringLiteral("stopReason"), QStringLiteral("verification_failed"));
            record.insert(QStringLiteral("convergence"), convergence);
            setState(record, QStringLiteral("repairing"), QStringLiteral("convergence-verification-failed"));
        } else {
            convergence.insert(QStringLiteral("status"), QStringLiteral("in_progress"));
            convergence.insert(QStringLiteral("stopReason"), QString());
            record.insert(QStringLiteral("convergence"), convergence);
            setState(record, QStringLiteral("repairing"), QStringLiteral("convergence-level-verified"),
                     {{QStringLiteral("levelsCompleted"), levels.size()}});
        }
    } else if (passed) {
        setState(record, QStringLiteral("candidate_ready"), QStringLiteral("verification-passed"),
                 {{QStringLiteral("artifactId"), expectedIdentity.value(QStringLiteral("artifactId"))},
                  {QStringLiteral("jobId"), expectedIdentity.value(QStringLiteral("jobId"))}});
    } else {
        const int repairs = countersOf(record).value(QStringLiteral("repairs")).toInt();
        const int repairLimit = record.value(QStringLiteral("authorization")).toObject()
            .value(QStringLiteral("experimentPolicy")).toObject().value(QStringLiteral("budgets"))
            .toObject().value(QStringLiteral("repairs")).toInt();
        if (repairs < repairLimit)
            setState(record, QStringLiteral("repairing"), QStringLiteral("verification-failed-repair-available"));
        else
            setState(record, QStringLiteral("failed"), QStringLiteral("verification-failed"));
    }
    if (!saveRecord(experimentId, record, &error)) return failure(QStringLiteral("EXPERIMENT_STORAGE_FAILED"), error);
    return success({{QStringLiteral("state"), record.value(QStringLiteral("state"))},
                    {QStringLiteral("verificationReport"), report},
                    {QStringLiteral("verificationPassed"), passed}});
}
