#include "application.h"
#include "solver_manager.h"
#include "model_command_host.h"
#include "session.h"
#include "solver_setup_checker.h"
#include <rnative_result_broker.h>
#include <rproject_catalog.h>
#include <rml_file_utils.h>
#include <rml_time_solver.h>

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QLockFile>
#include <QMetaObject>
#include <QQueue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <QVector>

#include <cmath>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
constexpr auto nativeJobRecordSchema = "range.native-job-record/v1";
constexpr auto nativeCheckpointContextSchema = "range.native-checkpoint-context/v1";
constexpr auto nativeCheckpointEnvelopeSchema = "range.native-checkpoint/v1";
constexpr auto nativeCheckpointPointerSchema = "range.native-checkpoint-pointer/v1";
constexpr auto nativeCheckpointPolicy = "accepted-steps-or-sim-time-v1";
constexpr qsizetype maxNativeJobRecordBytes = 32 * 1024 * 1024;
constexpr qsizetype maxNativeJobRecordsPerProject = 2000;
constexpr qsizetype maxNativeJobCommitMarkersPerJob = 128;
constexpr qsizetype maxNativeJobRegistryProjectsToScan = 4096;
constexpr auto nativeJobRegistryGenesisSha256 = "0000000000000000000000000000000000000000000000000000000000000000";

QByteArray sha256File(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(chunk);
    }
    return hash.result();
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool isLowerSha256(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool isCanonicalProjectId(const QString &value)
{
    const QUuid id = QUuid::fromString(value);
    return !id.isNull() && id.toString(QUuid::WithoutBraces).compare(value, Qt::CaseInsensitive) == 0;
}

bool isNativeJobId(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{16,32}$"));
    return pattern.match(value).hasMatch();
}

bool hasSymlinkComponent(const QString &absolutePath)
{
    QFileInfo pathInfo(absolutePath);
    QString current = pathInfo.absoluteFilePath();
    while (!current.isEmpty())
    {
        const QFileInfo component(current);
        if (component.isSymLink()) return true;
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return false;
}

QJsonObject persistedNativeJobRecord(const QJsonObject &record)
{
    QJsonObject persisted = record;
    persisted.remove(QStringLiteral("logs"));
    persisted.remove(QStringLiteral("resultFrames"));
    persisted.remove(QStringLiteral("resultFrameSequence"));
    persisted.remove(QStringLiteral("progress"));
    persisted.remove(QStringLiteral("progressEvents"));
    persisted.remove(QStringLiteral("progressDiagnostics"));
    persisted.remove(QStringLiteral("progressPhaseId"));
    persisted.remove(QStringLiteral("progressPhase"));
    persisted.remove(QStringLiteral("progressFraction"));
    persisted.remove(QStringLiteral("progressSequence"));
    persisted.insert(QStringLiteral("logsAvailableAfterReopen"), false);
    persisted.insert(QStringLiteral("resultFramesAvailable"), false);
    persisted.insert(QStringLiteral("resultFramesStatus"), QStringLiteral("unavailable"));
    persisted.insert(QStringLiteral("resultFramesUnavailableReason"),
                     QStringLiteral("Live frame snapshots are process-local; completed frames remain bound by the persisted result manifest."));
    persisted.insert(QStringLiteral("progressAvailable"), false);
    persisted.insert(QStringLiteral("progressStatus"), QStringLiteral("unavailable"));
    persisted.insert(QStringLiteral("progressUnavailableReason"),
                     QStringLiteral("Progress samples are process-local and are not treated as recovery evidence."));
    return persisted;
}

QByteArray nativeNoControlSha256()
{
    return QCryptographicHash::hash(QByteArrayLiteral("range.native-checkpoint.no-control.v1"),
                                    QCryptographicHash::Sha256);
}

bool sameRestartState(const RStructuralDynamicsRestartData &left,
                      const RStructuralDynamicsRestartData &right)
{
    return left.available == right.available &&
           left.completedSteps == right.completedSteps &&
           left.time == right.time && left.fixedTimeStep == right.fixedTimeStep &&
           left.meshNodeCount == right.meshNodeCount &&
           left.meshElementCount == right.meshElementCount &&
           left.physicalDofCount == right.physicalDofCount &&
           left.meshFingerprint == right.meshFingerprint &&
           left.dofFingerprint == right.dofFingerprint &&
           left.systemFingerprint == right.systemFingerprint &&
           left.q == right.q && left.v == right.v && left.a == right.a &&
           left.externalForce == right.externalForce &&
           left.cumulativeExternalWork == right.cumulativeExternalWork &&
           left.cumulativeDampingDissipation == right.cumulativeDampingDissipation &&
           left.initialMechanicalEnergy == right.initialMechanicalEnergy &&
           left.supportResponseAvailable == right.supportResponseAvailable &&
           left.supportFingerprint == right.supportFingerprint &&
           left.supportReactions == right.supportReactions &&
           left.supportPowers == right.supportPowers &&
           left.cumulativeSupportWork == right.cumulativeSupportWork &&
           left.controlAvailable == right.controlAvailable &&
           left.controlProtocolVersion == right.controlProtocolVersion &&
           left.controlFingerprint == right.controlFingerprint &&
           left.controlChannelIds == right.controlChannelIds &&
           left.controlLastCommands == right.controlLastCommands &&
           left.controlPowers == right.controlPowers &&
           left.cumulativeControlWork == right.cumulativeControlWork &&
           left.controlControllerState == right.controlControllerState &&
           left.controlDeterministicSeed == right.controlDeterministicSeed;
}

bool exactKeys(const QJsonObject &object, const QStringList &keys)
{
    if (object.size() != keys.size()) return false;
    for (const QString &key : keys)
        if (!object.contains(key)) return false;
    return true;
}

bool readSmallObject(const QString &path, qsizetype limit, QJsonObject *object, QString *error)
{
    if (error) error->clear();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink() || hasSymlinkComponent(info.absoluteFilePath()) ||
        info.size() < 0 || info.size() > limit)
    {
        if (error) *error = QStringLiteral("A required recovery file is missing, linked, or exceeds its size bound.");
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("A required recovery file could not be read.");
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() > limit)
    {
        if (error) *error = QStringLiteral("A required recovery file could not be read within its size bound.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
    {
        if (error) *error = QStringLiteral("A required recovery file is malformed JSON.");
        return false;
    }
    if (object) *object = parsed.object();
    return true;
}

bool readNativeJobRecordFile(const QString &path, const QString &expectedJobId,
                             QJsonObject *record, QString *error)
{
    if (error) error->clear();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink() || hasSymlinkComponent(info.absoluteFilePath()) ||
        info.size() <= 0 || info.size() > maxNativeJobRecordBytes)
    {
        if (error) *error = QStringLiteral("The native job record is missing, linked, empty, or exceeds its size bound.");
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("The native job record could not be opened.");
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.size() > maxNativeJobRecordBytes)
    {
        if (error) *error = QStringLiteral("The native job record could not be read within its size bound.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject() ||
        compactJson(parsed.object()) != bytes)
    {
        if (error) *error = QStringLiteral("The native job record is malformed or non-canonical JSON.");
        return false;
    }
    QJsonObject parsedRecord = parsed.object();
    const QString savedHash = parsedRecord.value(QStringLiteral("nativeJobRegistrySha256")).toString();
    const int revision = parsedRecord.value(QStringLiteral("nativeJobRegistryRevision")).toInt(-1);
    parsedRecord.remove(QStringLiteral("nativeJobRegistrySha256"));
    if (parsedRecord.value(QStringLiteral("nativeJobRegistrySchema")).toString() !=
            QString::fromLatin1(nativeJobRecordSchema) ||
        parsedRecord.value(QStringLiteral("jobId")).toString() != expectedJobId ||
        revision < 1 || !isLowerSha256(savedHash) || sha256Hex(compactJson(parsedRecord)) != savedHash)
    {
        if (error) *error = QStringLiteral("The native job record schema, identity, revision, or SHA-256 is invalid.");
        return false;
    }
    parsedRecord.insert(QStringLiteral("nativeJobRegistrySha256"), savedHash);
    if (record) *record = parsedRecord;
    return true;
}

QString nativePathAttributes(const QString &path)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return QStringLiteral("win32-error-%1").arg(GetLastError());
    QStringList names;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) names.append(QStringLiteral("directory"));
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) names.append(QStringLiteral("reparse"));
    if (attributes & FILE_ATTRIBUTE_ENCRYPTED) names.append(QStringLiteral("encrypted"));
    if (attributes & FILE_ATTRIBUTE_TEMPORARY) names.append(QStringLiteral("temporary"));
    if (attributes & FILE_ATTRIBUTE_HIDDEN) names.append(QStringLiteral("hidden"));
    if (attributes & FILE_ATTRIBUTE_READONLY) names.append(QStringLiteral("readonly"));
    if (names.isEmpty()) names.append(QStringLiteral("normal"));
    return names.join(QLatin1Char(','));
#else
    Q_UNUSED(path);
    return QStringLiteral("unavailable");
#endif
}

bool nativePathIsEncrypted(const QString &path)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_ENCRYPTED) != 0;
#else
    Q_UNUSED(path);
    return false;
#endif
}

QByteArray sha256Directory(const QString &directoryName)
{
    const QDir root(directoryName);
    if (!root.exists()) return {};
    QStringList relativeFileNames;
    QDirIterator iterator(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) relativeFileNames.append(root.relativeFilePath(iterator.next()));
    std::sort(relativeFileNames.begin(), relativeFileNames.end());
    if (relativeFileNames.isEmpty()) return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &relativeFileName : relativeFileNames)
    {
        hash.addData(relativeFileName.toUtf8());
        hash.addData("\0", 1);
        QFile file(root.filePath(relativeFileName));
        if (!file.open(QIODevice::ReadOnly)) return {};
        while (!file.atEnd())
        {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return {};
            hash.addData(chunk);
        }
        hash.addData("\0", 1);
    }
    return hash.result();
}

struct NativeInputPackageSnapshot
{
    QString inputPath;
    QString packagePath;
    QString inputSha256;
    QString packageSha256;
};

NativeInputPackageSnapshot snapshotNativeInputPackage(const QString &inputPath,
                                                       const QString &packagePath)
{
    NativeInputPackageSnapshot snapshot;
    snapshot.inputPath = QFileInfo(inputPath).absoluteFilePath();
    snapshot.packagePath = QFileInfo(packagePath).absoluteFilePath();
    snapshot.inputSha256 = QString::fromLatin1(sha256File(snapshot.inputPath).toHex());
    snapshot.packageSha256 = QString::fromLatin1(sha256Directory(snapshot.packagePath).toHex());
    return snapshot;
}

QJsonObject nativeInputPackageChangeDetails(const NativeInputPackageSnapshot &before,
                                             const NativeInputPackageSnapshot &after,
                                             const QString &expectedInputSha256,
                                             const QString &expectedPackageSha256)
{
    return {{QStringLiteral("inputSnapshotPath"), before.inputPath},
            {QStringLiteral("expectedInputSnapshotSha256"), expectedInputSha256},
            {QStringLiteral("beforeInputSnapshotSha256"), before.inputSha256},
            {QStringLiteral("afterInputSnapshotSha256"), after.inputSha256},
            {QStringLiteral("inputPackagePath"), before.packagePath},
            {QStringLiteral("expectedInputPackageSha256"), expectedPackageSha256},
            {QStringLiteral("beforeInputPackageSha256"), before.packageSha256},
            {QStringLiteral("afterInputPackageSha256"), after.packageSha256}};
}

bool samePathText(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QDir::cleanPath(left).compare(QDir::cleanPath(right), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(left) == QDir::cleanPath(right);
#endif
}

QString getRunRoot(const QString &resultFileName)
{
    if (resultFileName.isEmpty()) return {};
    return QFileInfo(QDir(QFileInfo(resultFileName).absolutePath()).filePath(QStringLiteral("..")))
            .absoluteFilePath();
}

bool inspectUnverifiedWorkingNativeResult(const QJsonObject &record,
                                          const QString &checkpointPath,
                                          quint64 targetSteps,
                                          double dtS,
                                          QJsonObject *partialResult,
                                          QString *error)
{
    if (partialResult) *partialResult = {};
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    const QString resultPath = record.value(QStringLiteral("resultArtifactRef")).toString();
    const QFileInfo resultInfo(resultPath);
    const QString resultDirectory = resultInfo.absolutePath();
    if (resultPath.isEmpty() || !resultInfo.isAbsolute() || resultInfo.isSymLink() ||
        !resultInfo.isFile() || hasSymlinkComponent(resultPath) ||
        resultInfo.canonicalFilePath() != resultInfo.absoluteFilePath())
        return reject(QStringLiteral("The unverified working result LINK is missing, linked, or non-canonical."));

    QString bindingError;
    QString bindingErrorCode;
    const auto binding = RProjectCatalog::resolveSourceBinding(resultPath, &bindingError, &bindingErrorCode);
    if (!binding || binding->sourceArtifacts.size() < 2 ||
        !samePathText(binding->logicalSourcePath, resultInfo.absoluteFilePath()))
        return reject(QStringLiteral("The unverified working result does not resolve through a validated native LINK: %1")
                      .arg(bindingError));
    const QString resolvedPath = binding->resolvedSourcePath;
    const QFileInfo resolvedInfo(resolvedPath);
    if (resolvedInfo.isSymLink() || !resolvedInfo.isFile() || hasSymlinkComponent(resolvedPath) ||
        resolvedInfo.canonicalFilePath() != resolvedInfo.absoluteFilePath() ||
        !samePathText(resolvedInfo.absolutePath(), resultDirectory))
        return reject(QStringLiteral("The working result LINK resolves outside its private result directory."));

    const QString sourceBase = RFileUtils::getFileNameWithOutTimeStep(resultPath);
    const QFileInfo sourceBaseInfo(sourceBase);
    const QRegularExpression recordPattern(QStringLiteral("^") +
        QRegularExpression::escape(sourceBaseInfo.completeBaseName()) + QStringLiteral("-(\\d{5,})\\.") +
        QRegularExpression::escape(sourceBaseInfo.suffix()) + QStringLiteral("$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch recordMatch = recordPattern.match(resolvedInfo.fileName());
    bool recordNoOk = false;
    const uint linkedRecordNo = recordMatch.captured(1).toUInt(&recordNoOk);
    if (!recordMatch.hasMatch() || !recordNoOk || linkedRecordNo == 0)
        return reject(QStringLiteral("The working result LINK does not resolve to a numbered accepted MODEL frame."));

    Model workingModel;
    RModel checkpointModel;
    RModel originalInput;
    try
    {
        workingModel.read(resultPath);
        checkpointModel.read(checkpointPath);
        originalInput.read(record.value(QStringLiteral("nativeCheckpointInputModelRef")).toString());
    }
    catch (const RError &readError)
    {
        return reject(QStringLiteral("The working result or pinned checkpoint MODEL could not be decoded: %1")
                      .arg(readError.getMessage()));
    }
    catch (const std::exception &readError)
    {
        return reject(QStringLiteral("The working result or pinned checkpoint MODEL could not be decoded: %1")
                      .arg(QString::fromLocal8Bit(readError.what())));
    }
    const RStructuralDynamicsRestartData &state =
            workingModel.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsRestartData &checkpointState =
            checkpointModel.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsRestartData &baselineState =
            originalInput.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsSetup &dynamics = workingModel.getProblemSetup().getStructuralDynamicsSetup();
    const RStructuralDynamicsSetup &originalDynamics = originalInput.getProblemSetup().getStructuralDynamicsSetup();
    const RTimeSolver &timeSolver = workingModel.getTimeSolver();
    try
    {
        state.validate();
        checkpointState.validate();
        if (baselineState.available) baselineState.validate();
    }
    catch (const RError &validationError)
    {
        return reject(QStringLiteral("The working result or checkpoint accepted state is invalid: %1")
                      .arg(validationError.getMessage()));
    }
    const quint64 baselineSteps = baselineState.available ? baselineState.completedSteps : 0;
    const double baselineTime = baselineState.available ? baselineState.time : 0.0;
    const double expectedTime = baselineTime +
            (double(state.completedSteps) - double(baselineSteps)) * dtS;
    const double timeTolerance = 1.0e-12 * qMax(1.0, std::abs(expectedTime));
    if (!state.available || state.completedSteps != quint64(linkedRecordNo) ||
        state.completedSteps != quint64(timeSolver.getCurrentTimeStep()) + 1)
        return reject(QStringLiteral("The working result LINK sequence disagrees with its accepted restart MODEL."));
    if (state.completedSteps < baselineSteps || state.completedSteps > targetSteps ||
        targetSteps != baselineSteps + originalDynamics.getNSteps())
        return reject(QStringLiteral("The working accepted sequence lies outside the frozen original target range."));
    if (!dynamics.getEnabled() || dynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
        workingModel.getProblemTaskTree().getProblemTypeMask() != R_PROBLEM_STRESS ||
        workingModel.getProblemSetup().getStructuralControlSetup().enabled ||
        workingModel.getProblemSetup().getStructuralCableSetup().enabled ||
        workingModel.getProblemSetup().getStructuralPiezoSetup().enabled ||
        workingModel.getProblemSetup().getStructuralThermalControlSetup().enabled || state.controlAvailable)
        return reject(QStringLiteral("The working result MODEL no longer matches the supported uncontrolled Newmark mode."));
    if (state.meshNodeCount != originalInput.getNNodes() || state.meshElementCount != originalInput.getNElements() ||
        state.meshFingerprint != RStructuralDynamicsRestartData::computeMeshFingerprint(workingModel) ||
        state.meshFingerprint != checkpointState.meshFingerprint)
        return reject(QStringLiteral("The working result MODEL mesh no longer matches the frozen input and checkpoint."));
    if (state.dofFingerprint != checkpointState.dofFingerprint ||
        state.systemFingerprint != checkpointState.systemFingerprint)
        return reject(QStringLiteral("The working result MODEL equation order or structural system fingerprint changed."));
    if (std::abs(state.fixedTimeStep - dtS) > 1.0e-12 * qMax(1.0, std::abs(dtS)))
        return reject(QStringLiteral("The working result MODEL fixed time step differs from the checkpoint context."));
    if (std::abs(state.time - timeSolver.getCurrentTime()) > timeTolerance ||
        std::abs(state.time - expectedTime) > timeTolerance)
        return reject(QStringLiteral("The working result MODEL time disagrees with its accepted sequence and frozen input."));

    const QString runRoot = getRunRoot(resultPath);
    const QString journalPath = record.value(QStringLiteral("logArtifactRefs")).toObject()
            .value(QStringLiteral("nativeResultJournal")).toString();
    const QString expectedJournalPath = QDir(runRoot).filePath(QStringLiteral("events/native-results.ndjson"));
    const QFileInfo journalInfo(journalPath);
    if (!samePathText(journalPath, expectedJournalPath) || journalInfo.isSymLink() ||
        !journalInfo.isFile() || hasSymlinkComponent(journalPath) ||
        journalInfo.canonicalFilePath() != journalInfo.absoluteFilePath() || journalInfo.size() > 16 * 1024 * 1024)
        return reject(QStringLiteral("The working result LINK has no bounded canonical native result journal."));
    const QByteArray journalHashBefore = sha256File(journalPath);
    if (journalHashBefore.size() != 32) return reject(QStringLiteral("The working result journal could not be hashed."));
    QFile journal(journalPath);
    if (!journal.open(QIODevice::ReadOnly))
        return reject(QStringLiteral("The working result journal could not be read."));
    const QByteArray journalBytes = journal.readAll();
    if (journal.error() != QFileDevice::NoError || (!journalBytes.isEmpty() && !journalBytes.endsWith('\n')))
        return reject(QStringLiteral("The working result journal ends with an incomplete record."));

    quint64 expectedSequence = 0;
    qint64 verifiedFrameBytes = 0;
    double previousTime = -1.0;
    QSet<uint> seenRecords;
    bool linkedFrameVerified = false;
    for (const QByteArray &line : journalBytes.split('\n'))
    {
        if (line.isEmpty()) continue;
        if (line.size() > 16384 || ++expectedSequence > 10000)
            return reject(QStringLiteral("The working result journal exceeded its record bound."));
        QJsonParseError parseError;
        const QJsonDocument frameDocument = QJsonDocument::fromJson(line, &parseError);
        const QJsonObject frame = frameDocument.object();
        const double sequence = frame.value(QStringLiteral("seq")).toDouble(-1.0);
        const double recordNoValue = frame.value(QStringLiteral("recordNo")).toDouble(-1.0);
        const double timeS = frame.value(QStringLiteral("timeS")).toDouble(-1.0);
        const QString relativePath = frame.value(QStringLiteral("relativePath")).toString();
        const QString frameSha = frame.value(QStringLiteral("sha256")).toString();
        bool recordNoOk = recordNoValue >= 1.0 && recordNoValue <= double(std::numeric_limits<uint>::max()) &&
                std::floor(recordNoValue) == recordNoValue;
        const uint recordNo = recordNoOk ? uint(recordNoValue) : 0;
        const QString expectedFile = QFileInfo(RFileUtils::getFileNameWithTimeStep(resultPath, recordNo)).fileName();
        const QString framePath = QDir(resultDirectory).filePath(relativePath);
        const QFileInfo frameInfo(framePath);
        if (parseError.error != QJsonParseError::NoError || !frameDocument.isObject() ||
            frame.value(QStringLiteral("protocolVersion")).toString() != QStringLiteral("range.native-result-frame/v1") ||
            frame.value(QStringLiteral("jobId")).toString() != record.value(QStringLiteral("jobId")).toString() ||
            sequence != double(expectedSequence) || !recordNoOk || quint64(recordNo) > targetSteps ||
            !std::isfinite(timeS) || timeS < 0.0 ||
            timeS < previousTime || relativePath != expectedFile || QFileInfo(relativePath).fileName() != relativePath ||
            !isLowerSha256(frameSha) || frameInfo.isSymLink() || !frameInfo.isFile() ||
            frameInfo.canonicalFilePath() != frameInfo.absoluteFilePath() ||
            frameInfo.size() <= 0 || frameInfo.size() > 256LL * 1024 * 1024 ||
            verifiedFrameBytes > 512LL * 1024 * 1024 - frameInfo.size() ||
            seenRecords.contains(recordNo) ||
            QString::fromLatin1(sha256File(framePath).toHex()) != frameSha)
            return reject(QStringLiteral("The working result journal contains an invalid or duplicate accepted frame."));
        verifiedFrameBytes += frameInfo.size();
        seenRecords.insert(recordNo);
        if (recordNo == linkedRecordNo && relativePath == resolvedInfo.fileName() &&
            samePathText(framePath, resolvedPath) && frameSha == binding->savedArtifactHash &&
            std::abs(timeS - state.time) <= timeTolerance)
            linkedFrameVerified = true;
        previousTime = timeS;
    }
    if (QString::fromLatin1(sha256File(journalPath).toHex()) != QString::fromLatin1(journalHashBefore.toHex()) ||
        !linkedFrameVerified ||
        QString::fromLatin1(sha256File(resolvedPath).toHex()) != binding->savedArtifactHash)
        return reject(QStringLiteral("The working result LINK does not match a stable hash-bound accepted frame journal entry."));

    if (partialResult)
        *partialResult = {{QStringLiteral("resolvedModelPath"), resolvedPath},
                          {QStringLiteral("sha256"), binding->savedArtifactHash},
                          {QStringLiteral("acceptedSeq"), double(state.completedSteps)},
                          {QStringLiteral("timeS"), state.time},
                          {QStringLiteral("journalSha256"), QString::fromLatin1(journalHashBefore.toHex())}};
    return true;
}

struct NativeJobRegistrySnapshot
{
    QJsonObject record;
    QString recordPath;
    QString latestMarkerPath;
    QString diagnostic;
    qsizetype commitMarkerCount = 0;
    bool found = false;
};

bool readNativeJobRegistryDirectory(const QString &jobDirectory, const QString &jobId,
                                    const QString &projectId, NativeJobRegistrySnapshot *snapshot,
                                    QString *error)
{
    if (error) error->clear();
    if (snapshot) *snapshot = {};
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    const QFileInfo directoryInfo(jobDirectory);
    if (!directoryInfo.exists() || !directoryInfo.isDir() || directoryInfo.isSymLink() ||
        hasSymlinkComponent(directoryInfo.absoluteFilePath()) ||
        !samePathText(directoryInfo.absoluteFilePath(), directoryInfo.canonicalFilePath()))
        return reject(QStringLiteral("The managed native job directory is missing, linked, or non-canonical."));

    static const QRegularExpression markerPattern(
        QStringLiteral("^commit-r([0-9]{8})-([0-9a-f]{32})-([0-9a-f]{64})-([0-9a-f]{64})\\.marker$"));
    static const QRegularExpression blobPattern(
        QStringLiteral("^job-record-r([0-9]{8})-([0-9a-f]{32})\\.json$"));
    struct Marker
    {
        int revision = 0;
        QString uuid;
        QString previousSha256;
        QString blobSha256;
        QString markerPath;
        QString blobPath;
    };
    QVector<Marker> markers;
    QStringList blobPaths;
    bool hasLegacyRecord = false;
    const QString legacyPath = QDir(jobDirectory).filePath(QStringLiteral("job.json"));
    QDirIterator iterator(jobDirectory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    qsizetype inspectedFiles = 0;
    while (iterator.hasNext())
    {
        const QFileInfo info(iterator.next());
        if (++inspectedFiles > 512)
            return reject(QStringLiteral("The native job directory exceeded its bounded file-entry limit."));
        const QString name = info.fileName();
        if (name == QStringLiteral("job.json"))
        {
            hasLegacyRecord = true;
            continue;
        }
        const auto markerMatch = markerPattern.match(name);
        if (markerMatch.hasMatch())
        {
            if (markers.size() >= maxNativeJobCommitMarkersPerJob)
                return reject(QStringLiteral("The native job commit-marker history reached its bounded revision limit."));
            bool revisionOk = false;
            const int revision = markerMatch.captured(1).toInt(&revisionOk);
            if (!revisionOk || revision < 1)
                return reject(QStringLiteral("A native job commit marker has an invalid revision."));
            Marker marker;
            marker.revision = revision;
            marker.uuid = markerMatch.captured(2);
            marker.previousSha256 = markerMatch.captured(3);
            marker.blobSha256 = markerMatch.captured(4);
            marker.markerPath = info.absoluteFilePath();
            marker.blobPath = QDir(jobDirectory).filePath(
                QStringLiteral("job-record-r%1-%2.json").arg(marker.revision, 8, 10, QChar('0')).arg(marker.uuid));
            markers.append(marker);
            continue;
        }
        if (name.startsWith(QStringLiteral("commit-r")))
            return reject(QStringLiteral("A native job commit marker has an invalid filename or hash binding."));
        if (blobPattern.match(name).hasMatch()) blobPaths.append(info.absoluteFilePath());
    }

    std::sort(markers.begin(), markers.end(), [](const Marker &left, const Marker &right)
    {
        return left.revision < right.revision;
    });
    QJsonObject legacyRecord;
    int expectedRevision = 1;
    QString expectedPreviousSha = QString::fromLatin1(nativeJobRegistryGenesisSha256);
    if (hasLegacyRecord)
    {
        QString legacyError;
        if (!readNativeJobRecordFile(legacyPath, jobId, &legacyRecord, &legacyError) ||
            legacyRecord.value(QStringLiteral("projectId")).toString() != projectId)
            return reject(legacyError.isEmpty()
                          ? QStringLiteral("The legacy native job record is not bound to its project.")
                          : legacyError);
        expectedRevision = legacyRecord.value(QStringLiteral("nativeJobRegistryRevision")).toInt(-1) + 1;
        expectedPreviousSha = legacyRecord.value(QStringLiteral("nativeJobRegistrySha256")).toString();
        if (expectedRevision < 2 || !isLowerSha256(expectedPreviousSha))
            return reject(QStringLiteral("The legacy native job record cannot seed a versioned commit chain."));
    }

    QJsonObject latestRecord;
    QString latestRecordPath;
    QString latestMarkerPath;
    QSet<QString> committedBlobs;
    QString previousCommittedSha = expectedPreviousSha;
    for (const Marker &marker : markers)
    {
        if (marker.revision != expectedRevision || marker.previousSha256 != previousCommittedSha)
            return reject(QStringLiteral("The native job commit-marker history has a revision gap or broken previous-SHA chain."));
        const QFileInfo markerInfo(marker.markerPath);
        if (!markerInfo.isFile() || markerInfo.isSymLink() || hasSymlinkComponent(markerInfo.absoluteFilePath()) ||
            markerInfo.size() != 0)
            return reject(QStringLiteral("A native job commit marker is not a regular empty file."));
        QJsonObject blobRecord;
        QString blobError;
        if (!readNativeJobRecordFile(marker.blobPath, jobId, &blobRecord, &blobError))
            return reject(QStringLiteral("A committed native job blob is missing or corrupt: %1").arg(blobError));
        const QString actualBlobSha = QString::fromLatin1(sha256File(marker.blobPath).toHex());
        if (actualBlobSha != marker.blobSha256 ||
            blobRecord.value(QStringLiteral("projectId")).toString() != projectId ||
            blobRecord.value(QStringLiteral("nativeJobRegistryRevision")).toInt(-1) != marker.revision ||
            blobRecord.value(QStringLiteral("nativeJobRegistryPreviousSha256")).toString() != marker.previousSha256 ||
            blobRecord.value(QStringLiteral("nativeJobRecordRef")).toString().isEmpty() ||
            !samePathText(blobRecord.value(QStringLiteral("nativeJobRecordRef")).toString(), marker.blobPath))
            return reject(QStringLiteral("A committed native job blob does not match its marker, project, path, or SHA chain."));
        latestRecord = blobRecord;
        latestRecordPath = marker.blobPath;
        latestMarkerPath = marker.markerPath;
        committedBlobs.insert(QFileInfo(marker.blobPath).fileName());
        previousCommittedSha = blobRecord.value(QStringLiteral("nativeJobRegistrySha256")).toString();
        ++expectedRevision;
    }

    if (markers.isEmpty())
    {
        if (!hasLegacyRecord)
        {
            if (!blobPaths.isEmpty())
                return reject(QStringLiteral("Only uncommitted native job blobs are present; no commit marker publishes them."));
            return reject(QStringLiteral("The native job directory has no committed record."));
        }
        latestRecord = legacyRecord;
        latestRecordPath = legacyPath;
    }
    else if (latestRecord.isEmpty())
    {
        return reject(QStringLiteral("The native job marker history has no committed payload."));
    }

    QStringList orphanedBlobs;
    for (const QString &blobPath : blobPaths)
        if (!committedBlobs.contains(QFileInfo(blobPath).fileName())) orphanedBlobs.append(blobPath);
    if (snapshot)
    {
        snapshot->record = latestRecord;
        snapshot->recordPath = latestRecordPath;
        snapshot->latestMarkerPath = latestMarkerPath;
        snapshot->commitMarkerCount = markers.size();
        snapshot->found = true;
        if (!orphanedBlobs.isEmpty())
            snapshot->diagnostic = QStringLiteral("Ignored %1 uncommitted native job blob(s); prior committed state remains authoritative.")
                    .arg(orphanedBlobs.size());
    }
    return true;
}

bool writeExclusiveNativeRegistryFile(const QString &path, const QByteArray &bytes,
                                     bool requireEncryptedDirectory, QString *error)
{
    if (error) error->clear();
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        if (error) *error = QStringLiteral("Could not exclusively create '%1': %2")
                .arg(path, output.errorString().left(512));
        return false;
    }
    const qint64 written = output.write(bytes);
    if (written != bytes.size() || !output.flush())
    {
        if (error) *error = QStringLiteral("Could not write '%1' (%2 of %3 bytes): %4")
                .arg(path, QString::number(written), QString::number(bytes.size()), output.errorString().left(512));
        output.close();
        QFile::remove(path);
        return false;
    }
#ifdef Q_OS_WIN
    const intptr_t nativeHandle = _get_osfhandle(output.handle());
    if (nativeHandle == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle)))
    {
        if (error) *error = QStringLiteral("Could not flush '%1' (Win32 error %2).")
                .arg(path, QString::number(GetLastError()));
        output.close();
        QFile::remove(path);
        return false;
    }
#else
    if (::fsync(output.handle()) != 0)
    {
        if (error) *error = QStringLiteral("Could not flush '%1' to storage.").arg(path);
        output.close();
        QFile::remove(path);
        return false;
    }
#endif
    output.close();
    if (output.error() != QFileDevice::NoError)
    {
        if (error) *error = QStringLiteral("Could not close '%1': %2")
                .arg(path, output.errorString().left(512));
        QFile::remove(path);
        return false;
    }
    const QFileInfo pathInfo(path);
    const QStorageInfo directoryVolume(pathInfo.absolutePath());
    const QStorageInfo fileVolume(path);
    if (pathInfo.isSymLink() || !pathInfo.isFile() || hasSymlinkComponent(path) ||
        !samePathText(pathInfo.absoluteDir().canonicalPath(), pathInfo.absoluteDir().absolutePath()) ||
        !directoryVolume.isValid() || !fileVolume.isValid() ||
        !samePathText(directoryVolume.rootPath(), fileVolume.rootPath()) ||
        (requireEncryptedDirectory && !nativePathIsEncrypted(path)))
    {
        if (error) *error = QStringLiteral("The new registry file failed regular-file, same-volume, or EFS verification: '%1'.")
                .arg(path);
        QFile::remove(path);
        return false;
    }
    if (QString::fromLatin1(sha256File(path).toHex()) !=
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()))
    {
        if (error) *error = QStringLiteral("The new registry file failed post-write SHA-256 verification: '%1'.")
                .arg(path);
        QFile::remove(path);
        return false;
    }
    return true;
}

bool createExclusiveNativeCommitMarker(const QString &path, QString *error)
{
    if (error) error->clear();
    QFile marker(path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        if (error) *error = QStringLiteral("Could not exclusively create empty commit marker '%1': %2")
                .arg(path, marker.errorString().left(512));
        return false;
    }
    // The exclusive namespace creation is the publication point. The marker
    // is empty by construction; never delete it after CreateNew succeeds.
    marker.close();
    const QFileInfo markerInfo(path);
    const QStorageInfo directoryVolume(markerInfo.absolutePath());
    const QStorageInfo markerVolume(path);
    if (!markerInfo.isFile() || markerInfo.isSymLink() || markerInfo.size() != 0 ||
        hasSymlinkComponent(path) || !directoryVolume.isValid() || !markerVolume.isValid() ||
        !samePathText(directoryVolume.rootPath(), markerVolume.rootPath()))
    {
        if (error) *error = QStringLiteral("The exclusively created commit marker failed empty-file and same-volume verification: '%1'.")
                .arg(path);
        return false;
    }
    return true;
}

bool validResultSourceBinding(const QJsonObject &binding, const QString &rawResultSha256,
                              const QString &reportedPath, const QString &registeredPath)
{
    const QString logicalPath = binding.value(QStringLiteral("logicalSourcePath")).toString();
    const QString resolvedPath = binding.value(QStringLiteral("resolvedSourcePath")).toString();
    const QString savedArtifactHash = binding.value(QStringLiteral("savedArtifactHash")).toString();
    const QJsonArray artifacts = binding.value(QStringLiteral("sourceArtifacts")).toArray();
    if (logicalPath.isEmpty() || resolvedPath.isEmpty() || artifacts.isEmpty() ||
        artifacts.size() > 32 ||
        !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(rawResultSha256).hasMatch() ||
        !samePathText(logicalPath, reportedPath) || !samePathText(logicalPath, registeredPath))
        return false;

    QString firstPath;
    QString firstHash;
    QString lastPath;
    QString lastHash;
    for (qsizetype index = 0; index < artifacts.size(); ++index)
    {
        const QJsonObject artifact = artifacts.at(index).toObject();
        const QString path = artifact.value(QStringLiteral("path")).toString();
        const QString hash = artifact.value(QStringLiteral("sha256")).toString();
        if (path.isEmpty() || !QFileInfo(path).isAbsolute() ||
            !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(hash).hasMatch() ||
            QFileInfo(path).isSymLink() || !samePathText(QFileInfo(path).canonicalFilePath(), path) ||
            QString::fromLatin1(sha256File(path).toHex()) != hash)
            return false;
        if (index == 0)
        {
            firstPath = path;
            firstHash = hash;
        }
        if (index + 1 == artifacts.size())
        {
            lastPath = path;
            lastHash = hash;
        }
    }
    return samePathText(firstPath, logicalPath) && firstHash == rawResultSha256 &&
            samePathText(lastPath, resolvedPath) && lastHash == savedArtifactHash;
}

bool validNativeResultManifestDescriptor(const QJsonObject &descriptor,
                                         const QJsonObject &jobRecord,
                                         const QString &jobId,
                                         QString *error)
{
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();

    const QJsonObject provenance = jobRecord.value(QStringLiteral("nativeResultProvenance")).toObject();
    const QJsonObject manifest = descriptor.value(QStringLiteral("resultManifest")).toObject();
    const QString manifestPath = descriptor.value(QStringLiteral("resultManifestRef")).toString();
    const QString manifestSha256 = descriptor.value(QStringLiteral("resultManifestSha256")).toString();
    const QString artifactId = descriptor.value(QStringLiteral("resultArtifactId")).toString();
    const QString resultPath = jobRecord.value(QStringLiteral("resultArtifactRef")).toString();
    const QString resultSha256 = jobRecord.value(QStringLiteral("resultArtifactSha256")).toString();
    if (descriptor.value(QStringLiteral("jobId")).toString() != jobId ||
        descriptor.value(QStringLiteral("provenanceFrozen")).toBool() !=
            jobRecord.value(QStringLiteral("provenanceFrozen")).toBool() ||
        !jobRecord.value(QStringLiteral("provenanceFrozen")).toBool() ||
        descriptor.value(QStringLiteral("nativeResultCatalogAvailable")).toBool() != true ||
        descriptor.value(QStringLiteral("resultProvenance")).toObject() != provenance ||
        manifest.isEmpty() || resultPath.isEmpty() || artifactId.isEmpty())
        return reject(QStringLiteral("The published result descriptor does not match the queued job provenance."));

    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression artifactPattern(QStringLiteral("^artifact-[0-9a-f]{32}$"));
    if (!sha256Pattern.match(manifestSha256).hasMatch() ||
        !artifactPattern.match(artifactId).hasMatch())
        return reject(QStringLiteral("The published result descriptor has an invalid hash or artifact ID."));

    const QString resultDirectory = QFileInfo(resultPath).absoluteDir().canonicalPath();
    if (resultDirectory.isEmpty())
        return reject(QStringLiteral("The registered native result directory is no longer available."));
    const QString expectedManifestPath = QDir(resultDirectory).filePath(QStringLiteral("native-result.manifest.json"));
    const QFileInfo manifestInfo(manifestPath);
    if (manifestInfo.isSymLink() || QFileInfo(manifestInfo.absolutePath()).isSymLink() ||
        !manifestInfo.exists() || !manifestInfo.isFile() ||
        !samePathText(manifestInfo.absoluteFilePath(), expectedManifestPath) ||
        !samePathText(manifestInfo.absoluteDir().canonicalPath(), resultDirectory) ||
        manifestInfo.size() < 0 || manifestInfo.size() > 16 * 1024 * 1024)
        return reject(QStringLiteral("The published result manifest is outside the job's private result directory or exceeds its size limit."));

    QFile manifestFile(manifestInfo.absoluteFilePath());
    if (!manifestFile.open(QIODevice::ReadOnly))
        return reject(QStringLiteral("The published result manifest could not be read back."));
    const QByteArray manifestBytes = manifestFile.readAll();
    if (manifestFile.error() != QFileDevice::NoError ||
        QString::fromLatin1(QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex()) != manifestSha256)
        return reject(QStringLiteral("The published result manifest bytes do not match their SHA-256."));

    QJsonParseError parseError;
    const QJsonDocument parsedManifest = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsedManifest.isObject() ||
        parsedManifest.object() != manifest)
        return reject(QStringLiteral("The copied result manifest descriptor does not match the immutable manifest file."));

    const QJsonObject latest = manifest.value(QStringLiteral("logicalLatest")).toObject();
    const QJsonObject terminal = manifest.value(QStringLiteral("terminalModel")).toObject();
    const QJsonObject binding = jobRecord.value(QStringLiteral("resultSourceBinding")).toObject();
    const QString terminalRelativePath = terminal.value(QStringLiteral("relativePath")).toString();
    const QString terminalPath = QDir(resultDirectory).filePath(terminalRelativePath);
    if (!jobRecord.value(QStringLiteral("resultSourceBindingVerified")).toBool() ||
        !validResultSourceBinding(binding, resultSha256, resultPath, resultPath) ||
        terminalRelativePath.isEmpty() || QFileInfo(terminalRelativePath).fileName() != terminalRelativePath ||
        !samePathText(terminalPath, binding.value(QStringLiteral("resolvedSourcePath")).toString()) ||
        terminal.value(QStringLiteral("sha256")).toString() != binding.value(QStringLiteral("savedArtifactHash")).toString())
        return reject(QStringLiteral("The manifest terminal MODEL does not match the artifact accepted by result readback."));
    const QJsonObject analysis = manifest.value(QStringLiteral("analysis")).toObject();
    const QJsonArray frames = manifest.value(QStringLiteral("frames")).toArray();
    const QString analysisType = analysis.value(QStringLiteral("type")).toString();
    if (manifest.value(QStringLiteral("apiVersion")).toString() != QStringLiteral("range.native-results/v1") ||
        manifest.value(QStringLiteral("jobId")).toString() != jobId ||
        manifest.value(QStringLiteral("artifactId")).toString() != artifactId ||
        manifest.value(QStringLiteral("sourceRuntimeStamp")).toObject() !=
            provenance.value(QStringLiteral("sourceRuntimeStamp")).toObject() ||
        manifest.value(QStringLiteral("sourceSnapshotRef")).toObject() !=
            provenance.value(QStringLiteral("sourceSnapshotRef")).toObject() ||
        manifest.value(QStringLiteral("inputSnapshotSha256")).toString() !=
            provenance.value(QStringLiteral("inputSnapshotSha256")).toString() ||
        manifest.value(QStringLiteral("inputPackageSha256")).toString() !=
            provenance.value(QStringLiteral("inputPackageSha256")).toString() ||
        manifest.value(QStringLiteral("runtime")).toObject() != provenance.value(QStringLiteral("runtime")).toObject() ||
        manifest.value(QStringLiteral("control")).toObject() != provenance.value(QStringLiteral("control")).toObject() ||
        manifest.value(QStringLiteral("proofScope")).toString() !=
            provenance.value(QStringLiteral("proofScope")).toString() ||
        descriptor.value(QStringLiteral("resultArtifactId")).toString() != manifest.value(QStringLiteral("artifactId")).toString() ||
        latest.value(QStringLiteral("relativePath")).toString() != QFileInfo(resultPath).fileName() ||
        latest.value(QStringLiteral("sha256")).toString() != resultSha256 ||
        (analysisType != QStringLiteral("structural-dynamics") &&
         analysisType != QStringLiteral("static") && analysisType != QStringLiteral("modal")) ||
        (analysisType == QStringLiteral("structural-dynamics") && frames.isEmpty()) ||
        (analysisType != QStringLiteral("structural-dynamics") && !frames.isEmpty()))
        return reject(QStringLiteral("The published result manifest does not describe the registered job and latest artifact."));

    const QByteArray currentResultSha256 = sha256File(resultPath);
    if (currentResultSha256.isEmpty() || QString::fromLatin1(currentResultSha256.toHex()) != resultSha256)
        return reject(QStringLiteral("The logical latest artifact changed after manifest publication."));
    const QJsonObject catalog = RNativeResultBroker::query(manifestPath, manifestSha256, QStringLiteral("catalog"), {});
    if (!catalog.value(QStringLiteral("valid")).toBool())
        return reject(catalog.value(QStringLiteral("message")).toString());
    return true;
}

QString logicalNativeJobId(const QString &jobId, const QJsonObject &record)
{
    const QString logicalId = record.value(QStringLiteral("logicalJobId")).toString();
    return logicalId.isEmpty() ? jobId : logicalId;
}

QString managedLogicalRecoveryLockPath(const QString &nativeJobRegistryRoot,
                                       const QString &projectId,
                                       const QString &logicalJobId,
                                       bool createDirectories,
                                       QString *error)
{
    if (error) error->clear();
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return QString();
    };
    if ((projectId != QStringLiteral("unlinked") && !isCanonicalProjectId(projectId)) ||
        !isNativeJobId(logicalJobId) || nativeJobRegistryRoot.isEmpty())
        return reject(QStringLiteral("The logical recovery lock identity is invalid."));

    const QFileInfo registryRootInfo(nativeJobRegistryRoot);
    const QString appLocalRoot = registryRootInfo.absolutePath();
    const QString lockRoot = QDir(appLocalRoot).filePath(QStringLiteral("native-job-recovery-locks"));
    if (createDirectories && !QDir().mkpath(lockRoot))
        return reject(QStringLiteral("The managed logical recovery lock directory could not be created."));
    const QFileInfo lockRootInfo(lockRoot);
    if (lockRootInfo.exists() &&
        (!lockRootInfo.isDir() || lockRootInfo.isSymLink() || hasSymlinkComponent(lockRoot) ||
         !samePathText(lockRoot, lockRootInfo.canonicalFilePath())))
        return reject(QStringLiteral("The managed logical recovery lock root is linked or non-canonical."));

    const QString projectLockDirectory = QDir(lockRoot).filePath(projectId);
    if (createDirectories && !QDir().mkpath(projectLockDirectory))
        return reject(QStringLiteral("The project logical recovery lock directory could not be created."));
    const QFileInfo projectLockInfo(projectLockDirectory);
    if (projectLockInfo.exists() &&
        (!projectLockInfo.isDir() || projectLockInfo.isSymLink() || hasSymlinkComponent(projectLockDirectory) ||
         !samePathText(projectLockDirectory, projectLockInfo.canonicalFilePath())))
        return reject(QStringLiteral("The project logical recovery lock path is linked or non-canonical."));

    const QString lockPath = QDir(projectLockDirectory).filePath(logicalJobId + QStringLiteral(".lock"));
    const QFileInfo logicalLockInfo(lockPath);
    if (logicalLockInfo.isSymLink() || (logicalLockInfo.exists() && !logicalLockInfo.isFile()) ||
        hasSymlinkComponent(lockPath))
        return reject(QStringLiteral("The logical recovery lock file is linked."));
    return lockPath;
}

QString logicalRecoveryHeadJobId(const QHash<QString,QJsonObject> &records,
                                 const QString &projectId,
                                 const QString &logicalJobId,
                                 QString *error)
{
    if (error) error->clear();
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return QString();
    };
    QStringList members;
    QString rootJobId;
    QHash<QString,QStringList> children;
    for (auto it = records.cbegin(); it != records.cend(); ++it)
    {
        const QString candidateId = it.key();
        const QJsonObject candidate = it.value();
        if (candidate.value(QStringLiteral("projectId")).toString() != projectId ||
            logicalNativeJobId(candidateId, candidate) != logicalJobId)
            continue;
        members.append(candidateId);
        const QString parentId = candidate.value(QStringLiteral("recoverySourceJobId")).toString();
        if (parentId.isEmpty())
        {
            if (candidateId != logicalJobId || !rootJobId.isEmpty())
                return reject(QStringLiteral("The logical recovery lineage has an invalid or duplicate root."));
            rootJobId = candidateId;
            continue;
        }
        const auto parent = records.constFind(parentId);
        if (parent == records.cend() || parentId == candidateId ||
            parent.value().value(QStringLiteral("projectId")).toString() != projectId ||
            logicalNativeJobId(parentId, parent.value()) != logicalJobId)
            return reject(QStringLiteral("The logical recovery lineage contains a missing or mismatched parent."));
        children[parentId].append(candidateId);
        if (children.value(parentId).size() > 1)
            return reject(QStringLiteral("The logical recovery lineage contains competing successor branches."));
    }
    if (members.isEmpty() || rootJobId.isEmpty())
        return reject(QStringLiteral("The logical recovery lineage has no persisted root."));

    QSet<QString> visited;
    QString current = rootJobId;
    while (true)
    {
        if (visited.contains(current))
            return reject(QStringLiteral("The logical recovery lineage contains a cycle."));
        visited.insert(current);
        const QStringList successors = children.value(current);
        if (successors.isEmpty()) break;
        if (successors.size() != 1)
            return reject(QStringLiteral("The logical recovery lineage has multiple persisted heads."));
        current = successors.first();
    }
    if (visited.size() != members.size())
        return reject(QStringLiteral("The logical recovery lineage contains disconnected persisted attempts."));
    return current;
}

bool refreshNativeProjectJobRecords(const QString &nativeJobRegistryRoot,
                                    const QString &projectId,
                                    QHash<QString,QJsonObject> *records,
                                    QHash<QString,QString> *uncommittedDiagnostics,
                                    QString *error)
{
    if (error) error->clear();
    if (records) records->clear();
    if (uncommittedDiagnostics) uncommittedDiagnostics->clear();
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    if (!records || nativeJobRegistryRoot.isEmpty() ||
        (projectId != QStringLiteral("unlinked") && !isCanonicalProjectId(projectId)))
        return reject(QStringLiteral("The logical recovery registry identity is invalid."));

    const QFileInfo registryInfo(nativeJobRegistryRoot);
    if (!registryInfo.isDir() || registryInfo.isSymLink() ||
        hasSymlinkComponent(registryInfo.absoluteFilePath()) ||
        !samePathText(registryInfo.absoluteFilePath(), registryInfo.canonicalFilePath()))
        return reject(QStringLiteral("The managed native job registry is unavailable or non-canonical."));
    const QString projectDirectory = QDir(registryInfo.absoluteFilePath()).filePath(projectId);
    const QFileInfo projectInfo(projectDirectory);
    if (!projectInfo.isDir() || projectInfo.isSymLink() || hasSymlinkComponent(projectDirectory) ||
        !samePathText(projectDirectory, projectInfo.canonicalFilePath()))
        return reject(QStringLiteral("The logical recovery project registry is missing, linked, or non-canonical."));

    const QString ownerRoot = QDir(registryInfo.absolutePath()).filePath(QStringLiteral("native-job-owners"));
    QDirIterator jobDirectories(projectDirectory, QDir::Dirs | QDir::NoDotAndDotDot);
    qsizetype scannedDirectories = 0;
    while (jobDirectories.hasNext())
    {
        if (++scannedDirectories > maxNativeJobRecordsPerProject)
            return reject(QStringLiteral("The logical recovery project registry exceeds its bounded record scan."));
        const QFileInfo jobInfo(jobDirectories.next());
        const QString jobId = jobInfo.fileName();
        if (!isNativeJobId(jobId)) continue;
        if (jobInfo.isSymLink() || hasSymlinkComponent(jobInfo.absoluteFilePath()))
            return reject(QStringLiteral("A native job directory in the logical recovery project is linked."));

        NativeJobRegistrySnapshot snapshot;
        QString readError;
        if (!readNativeJobRegistryDirectory(jobInfo.absoluteFilePath(), jobId, projectId,
                                            &snapshot, &readError))
        {
            if (readError == QStringLiteral("The native job directory has no committed record.") ||
                readError == QStringLiteral("Only uncommitted native job blobs are present; no commit marker publishes them."))
            {
                if (uncommittedDiagnostics) uncommittedDiagnostics->insert(jobId, readError);
                continue;
            }
            return reject(QStringLiteral("A project native job could not be refreshed under its logical recovery lease: %1")
                          .arg(readError));
        }
        const QJsonObject record = snapshot.record;
        const QString ownerId = record.value(QStringLiteral("ownerInstanceId")).toString();
        const QString ownerLockPath = record.value(QStringLiteral("ownerLockRef")).toString();
        const QString expectedOwnerLock = QDir(ownerRoot).filePath(ownerId + QStringLiteral(".lock"));
        if (record.value(QStringLiteral("projectId")).toString() != projectId ||
            !samePathText(record.value(QStringLiteral("nativeJobRecordRef")).toString(), snapshot.recordPath) ||
            !isCanonicalProjectId(ownerId) || !QFileInfo(ownerLockPath).isAbsolute() ||
            !samePathText(ownerLockPath, expectedOwnerLock) || hasSymlinkComponent(ownerLockPath))
            return reject(QStringLiteral("A refreshed native job record has an invalid project, owner, or path binding."));
        records->insert(jobId, record);
    }
    return true;
}

void mergeNativeJobTransientFields(QJsonObject *record, const QJsonObject &previouslyLoaded)
{
    if (!record) return;
    static const QStringList transientFields{
        QStringLiteral("logs"), QStringLiteral("logsAvailableAfterReopen"),
        QStringLiteral("resultFrames"), QStringLiteral("resultFramesAvailable"),
        QStringLiteral("resultFramesStatus"), QStringLiteral("resultFramesUnavailableReason"),
        QStringLiteral("resultFrameSequence"), QStringLiteral("progressAvailable"),
        QStringLiteral("progressStatus"), QStringLiteral("progressUnavailableReason"),
        QStringLiteral("progressEvents"), QStringLiteral("progressDiagnostics"),
        QStringLiteral("progressPhaseId"), QStringLiteral("progressPhase"),
        QStringLiteral("progressFraction"), QStringLiteral("progressSequence")};
    for (const QString &field : transientFields)
        if (previouslyLoaded.contains(field)) record->insert(field, previouslyLoaded.value(field));
}
}

SolverManager::SolverManager(QObject *parent)
    : RJobManager(parent)
    , taskServer(0)
{
    this->nativeJobOwnerInstanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString appLocalRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!appLocalRoot.isEmpty() && QDir().mkpath(QDir(appLocalRoot).filePath(QStringLiteral("native-job-owners"))))
    {
        const QFileInfo rootInfo(QDir(appLocalRoot).filePath(QStringLiteral("native-job-owners")));
        if (rootInfo.isDir() && !rootInfo.isSymLink() && !rootInfo.canonicalFilePath().isEmpty())
        {
            this->nativeJobRegistryRoot = QDir::cleanPath(
                QFileInfo(QDir(appLocalRoot).filePath(QStringLiteral("native-jobs"))).absoluteFilePath());
            const QString ownerRoot = rootInfo.canonicalFilePath();
            this->nativeJobOwnerLock.reset(new QLockFile(
                QDir(ownerRoot).filePath(this->nativeJobOwnerInstanceId + QStringLiteral(".lock"))));
            this->nativeJobOwnerLock->setStaleLockTime(0);
            if (!this->nativeJobOwnerLock->tryLock(0))
            {
                this->nativeJobOwnerInstanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                this->nativeJobOwnerLock.reset(new QLockFile(
                    QDir(ownerRoot).filePath(this->nativeJobOwnerInstanceId + QStringLiteral(".lock"))));
                this->nativeJobOwnerLock->setStaleLockTime(0);
                if (!this->nativeJobOwnerLock->tryLock(0)) this->nativeJobOwnerLock.reset();
            }
        }
    }
    this->taskServerName = "RANGE_SOLVER_TASK_SERVER_" + Application::instance()->getSession()->getID() +
            "_" + this->nativeJobOwnerInstanceId;
    this->taskServer = new QLocalServer(this);
    if (!this->taskServer->listen(this->taskServerName))
    {
        RLogger::warning("Local server failed to listen: %s\n", this->taskServer->errorString().toUtf8().constData());
    }
    RLogger::info("Listening on: %s\n", this->taskServer->serverName().toUtf8().constData());
    QObject::connect(this->taskServer, &QLocalServer::newConnection, this, &SolverManager::onSolverTaskNewConnection);
}

SolverManager &SolverManager::getInstance()
{
    static SolverManager solverManager;
    return solverManager;
}

bool SolverManager::ensureUniqueNativeTaskId(SolverTaskID *taskId) const
{
    if (!taskId) return false;
    bool unique = false;
    if (!this->invokeOnOwner([this, taskId, &unique]()
    {
        constexpr int maximumCandidates = 4096;
        for (int candidate = 0; candidate < maximumCandidates; ++candidate)
        {
            const QString jobId = taskId->toString();
            if (isNativeJobId(jobId) && !this->nativeJobs.contains(jobId) &&
                !this->nativeJobIdExistsOnDiskOnOwner(jobId))
            {
                unique = true;
                return;
            }
            taskId->generate();
        }
    }))
        return false;
    return unique;
}

bool SolverManager::nativeJobIdExistsOnDiskOnOwner(const QString &jobId) const
{
    if (QThread::currentThread() != this->thread() || this->nativeJobRegistryRoot.isEmpty()) return true;
    const QFileInfo registryInfo(this->nativeJobRegistryRoot);
    if (!registryInfo.exists()) return false;
    if (!registryInfo.isDir() || registryInfo.isSymLink() || hasSymlinkComponent(registryInfo.absoluteFilePath()))
        return true;

    QDirIterator projects(registryInfo.absoluteFilePath(), QDir::Dirs | QDir::NoDotAndDotDot);
    qsizetype scannedProjects = 0;
    while (projects.hasNext())
    {
        const QFileInfo projectInfo(projects.next());
        if (++scannedProjects > maxNativeJobRegistryProjectsToScan) return true;
        if (projectInfo.isSymLink() || !projectInfo.isDir()) return true;
        const QFileInfo jobDirectory(QDir(projectInfo.absoluteFilePath()).filePath(jobId));
        if (jobDirectory.exists() || jobDirectory.isSymLink()) return true;
    }
    return false;
}

const QString &SolverManager::getTaskServerName() const
{
    return this->taskServerName;
}

void SolverManager::submit(SolverTask *solverTask)
{
    QString error;
    if (this->submitValidated(solverTask, QJsonObject(), true, &error).isEmpty() && !error.isEmpty())
        RLogger::error("Solver task was rejected: %s\n", error.toUtf8().constData());
}

bool SolverManager::invokeOnOwner(const std::function<void()> &callback) const
{
    if (QThread::currentThread() == this->thread())
    {
        callback();
        return true;
    }
    return QMetaObject::invokeMethod(const_cast<SolverManager*>(this), callback, Qt::BlockingQueuedConnection);
}

QJsonObject SolverManager::validateRun(uint modelID) const
{
    QJsonObject result;
    if (!this->invokeOnOwner([this, modelID, &result]() { result = this->validateRunOnOwner(modelID); }))
        return {{QStringLiteral("valid"), false},
                {QStringLiteral("errors"), QJsonArray{QStringLiteral("Solver validation could not run on the manager thread.")}},
                {QStringLiteral("warnings"), QJsonArray{}}};
    return result;
}

QJsonObject SolverManager::validateRunOnOwner(uint modelID) const
{
    QJsonArray errors;
    QJsonArray warnings;
    QJsonObject sourceStampJson;
    QString modelName;
    bool modelReadable = false;
    Session *session = Application::instance()->getSession();
    if (QThread::currentThread() != session->thread())
    {
        errors.append(QStringLiteral("Solver validation must run on the document thread."));
    }
    else
    {
        RJobManager &legacyJobs = RJobManager::getInstance();
        if (legacyJobs.getNRunning() > 0 || legacyJobs.getNWaiting() > 0 || this->hasLiveModelTasks())
        {
            errors.append(QStringLiteral("Solver validation is unavailable while a legacy or live-model job is active or queued."));
        }
        else if (!session->trylock(0))
        {
            errors.append(QStringLiteral("Solver validation was rejected because the session is in use."));
        }
        else
        {
            struct SessionUnlockGuard
            {
                Session *session;
                ~SessionUnlockGuard() { if (session) session->unlock(); }
            } unlockGuard{session};

            if (legacyJobs.getNRunning() > 0 || legacyJobs.getNWaiting() > 0 || this->hasLiveModelTasks())
            {
                errors.append(QStringLiteral("Solver validation became unavailable while a legacy or live-model job was starting."));
            }
            else if (modelID >= session->getNModels())
            {
                errors.append(QStringLiteral("The selected model is no longer open."));
            }
            else
            {
                const QPair<QString,quint64> sourceStamp = session->modelCommandStamp(modelID);
                sourceStampJson = {{QStringLiteral("documentId"), sourceStamp.first},
                                   {QStringLiteral("revision"), QString::number(sourceStamp.second)},
                                   {QStringLiteral("sessionEpoch"), session->getModelCommandHost()->sessionEpoch()}};
                if (sourceStamp.first.isEmpty())
                {
                    errors.append(QStringLiteral("The selected model has no stable runtime identity."));
                }
                else if (this->isSolverQueued(sourceStamp.first))
                {
                    errors.append(QStringLiteral("A native solver job is already queued for this document."));
                }
                else
                {
                    const Model &model = session->getModel(modelID);
                    modelName = model.getName();
                    modelReadable = true;
                    SolverSetupChecker checker(model);
                    QStringList modelWarnings;
                    QStringList modelErrors;
                    checker.perform(modelWarnings, modelErrors);
                    for (const QString &message : modelErrors) errors.append(message);
                    for (const QString &message : modelWarnings) warnings.append(message);
                    const QString solverPath = Application::instance()->getApplicationSettings()->getSolverPath();
                    if (solverPath.isEmpty() || !QFileInfo::exists(solverPath))
                        errors.append(QStringLiteral("The configured native solver executable is unavailable."));
                }
            }
        }
    }
    return {{QStringLiteral("valid"), errors.isEmpty()},
            {QStringLiteral("modelId"), int(modelID)},
            {QStringLiteral("modelName"), modelName},
            {QStringLiteral("modelReadable"), modelReadable},
            {QStringLiteral("errors"), errors},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("sourceStamp"), sourceStampJson}};
}

QString SolverManager::submitValidated(uint modelID, const QJsonObject &projectSnapshotRef,
                                       bool warningsAccepted, QString *error)
{
    if (error) error->clear();
    QString jobId;
    QString localError;
    const bool invoked = this->invokeOnOwner([this, modelID, projectSnapshotRef, warningsAccepted,
                                               &jobId, &localError]()
    {
        const QJsonObject validation = this->validateRunOnOwner(modelID);
        const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
        if (!validation.value(QStringLiteral("valid")).toBool() || !errors.isEmpty())
        {
            localError = errors.isEmpty() ? QStringLiteral("Native solver validation failed.")
                                          : errors.first().toString();
            return;
        }
        const QJsonArray warnings = validation.value(QStringLiteral("warnings")).toArray();
        if (!warnings.isEmpty() && !warningsAccepted)
        {
            localError = QStringLiteral("Native solver warnings require explicit acceptance.");
            return;
        }
        auto *task = new SolverTask(Application::instance()->getApplicationSettings(), modelID);
        jobId = this->submitTaskOnOwner(task, projectSnapshotRef, warningsAccepted, &localError);
    });
    if (!invoked) localError = QStringLiteral("Solver submission could not run on the manager thread.");
    if (error) *error = localError;
    return jobId;
}

QString SolverManager::submitValidated(SolverTask *solverTask, const QJsonObject &projectSnapshotRef,
                                       bool warningsAccepted, QString *error)
{
    if (error) error->clear();
    if (!solverTask)
    {
        if (error) *error = QStringLiteral("No native solver task was supplied.");
        return {};
    }
    QString jobId;
    QString localError;
    const bool invoked = this->invokeOnOwner([this, solverTask, projectSnapshotRef, warningsAccepted,
                                               &jobId, &localError]()
    {
        jobId = this->submitTaskOnOwner(solverTask, projectSnapshotRef, warningsAccepted, &localError);
    });
    if (!invoked)
    {
        localError = QStringLiteral("Solver submission could not run on the manager thread.");
        solverTask->deleteLater();
    }
    if (error) *error = localError;
    return jobId;
}

QString SolverManager::submitTaskOnOwner(SolverTask *solverTask, const QJsonObject &projectSnapshotRef,
                                         bool warningsAccepted, QString *error)
{
    if (error) error->clear();
    if (!solverTask)
    {
        if (error) *error = QStringLiteral("No native solver task was supplied.");
        return {};
    }
    Session *session = Application::instance()->getSession();
    if (QThread::currentThread() != this->thread() || QThread::currentThread() != session->thread())
    {
        if (error) *error = QStringLiteral("Solver snapshots must be captured on the document and manager thread.");
        solverTask->deleteLater();
        return {};
    }
    const auto reject = [solverTask, error](const QString &message)
    {
        if (error) *error = message;
        solverTask->deleteLater();
        return QString();
    };
    const QString sourceDocumentId = solverTask->getSourceDocumentId();
    const uint currentModelID = session->findModelByCommandIdentity(sourceDocumentId);
    if (sourceDocumentId.isEmpty() || currentModelID == RConstants::eod)
        return reject(QStringLiteral("The solver task target was closed or no longer has its captured runtime identity."));
    const QPair<QString,quint64> currentStamp = session->modelCommandStamp(currentModelID);
    if (sourceDocumentId != currentStamp.first)
        return reject(QStringLiteral("The solver task's source identity no longer matches the selected document."));
    if (!solverTask->rebindModelIDForSubmit(currentModelID))
        return reject(QStringLiteral("The solver task cannot be rebound after execution has started."));
    const bool inputAlreadyCaptured = !solverTask->getSourceStamp().first.isEmpty();
    if (inputAlreadyCaptured && solverTask->getSourceStamp() != currentStamp)
        return reject(QStringLiteral("The pre-captured solver input belongs to a stale source revision."));

    QString projectLink;
    if (inputAlreadyCaptured)
    {
        if (!projectSnapshotRef.isEmpty())
            return reject(QStringLiteral("Linked tasks must be captured from the registered frozen snapshot by the manager."));
        projectLink = QStringLiteral("unlinked");
    }
    else
    {
        const QJsonObject validation = this->validateRunOnOwner(currentModelID);
        const QJsonArray validationErrors = validation.value(QStringLiteral("errors")).toArray();
        if (!validation.value(QStringLiteral("valid")).toBool() || !validationErrors.isEmpty())
            return reject(validationErrors.isEmpty() ? QStringLiteral("Native solver validation failed.")
                                                     : validationErrors.first().toString());
        const QJsonArray validationWarnings = validation.value(QStringLiteral("warnings")).toArray();
        if (!validationWarnings.isEmpty() && !warningsAccepted)
            return reject(QStringLiteral("Native solver warnings require explicit acceptance."));

        QString refError;
        projectLink = this->validateProjectSnapshotRef(currentModelID, currentStamp, projectSnapshotRef, &refError);
        if (projectLink.isEmpty()) return reject(refError);
    }

    if (this->isSolverQueued(currentStamp.first))
        return reject(QStringLiteral("A native solver job is already queued for this document."));

    const bool linkedProjectSnapshot = projectLink == QStringLiteral("linked");
    bool frozenSnapshotVerified = false;
    QString frozenSnapshotPath;
    QString frozenSnapshotSha256;
    std::unique_ptr<RModel> frozenModelBody;
    if (inputAlreadyCaptured)
    {
        if (solverTask->getInputDerivedFrom() != QStringLiteral("live_unlinked") ||
            solverTask->getFrozenSnapshotVerified() ||
            !solverTask->getFrozenSnapshotPath().isEmpty() ||
            !solverTask->getFrozenSnapshotSha256().isEmpty())
        {
            return reject(QStringLiteral("An unlinked pre-captured task has inconsistent input provenance."));
        }

        const QString inputPath = solverTask->getInputSnapshotFileName();
        const QString inputDirectory = QFileInfo(inputPath).absolutePath();
        if (solverTask->getInputSnapshotSha256().isEmpty() || solverTask->getInputPackageSha256().isEmpty() ||
            !QFileInfo(inputPath).isFile() || sha256File(inputPath) != solverTask->getInputSnapshotSha256() ||
            sha256Directory(inputDirectory) != solverTask->getInputPackageSha256())
            return reject(QStringLiteral("The pre-captured solver input/package changed or failed its immutable hash check."));

        std::unique_ptr<Model> capturedInput = solverTask->copyCapturedInputForValidation();
        if (!capturedInput)
            return reject(QStringLiteral("The pre-captured solver input is no longer available for safe pre-queue validation."));
        SolverSetupChecker checker(*capturedInput);
        QStringList capturedWarnings;
        QStringList capturedErrors;
        checker.perform(capturedWarnings, capturedErrors);
        if (!capturedErrors.isEmpty()) return reject(capturedErrors.first());
        if (!capturedWarnings.isEmpty() && !warningsAccepted)
            return reject(QStringLiteral("Native solver warnings require explicit acceptance."));
        const QString solverPath = Application::instance()->getApplicationSettings()->getSolverPath();
        if (solverPath.isEmpty() || !QFileInfo::exists(solverPath))
            return reject(QStringLiteral("The configured native solver executable is unavailable."));
    }
    else
    {
        if (linkedProjectSnapshot)
        {
            frozenModelBody = std::make_unique<RModel>();
            QString frozenError;
            if (!this->loadVerifiedFrozenSnapshot(projectSnapshotRef, frozenModelBody.get(),
                                                  &frozenSnapshotPath, &frozenSnapshotSha256,
                                                  &frozenError))
                return reject(frozenError);
            frozenSnapshotVerified = true;
            Model frozenValidationModel;
            static_cast<RModel&>(frozenValidationModel) = *frozenModelBody;
            SolverSetupChecker frozenChecker(frozenValidationModel);
            QStringList frozenWarningList;
            QStringList frozenErrors;
            frozenChecker.perform(frozenWarningList, frozenErrors);
            if (!frozenErrors.isEmpty()) return reject(frozenErrors.first());
            if (!frozenWarningList.isEmpty() && !warningsAccepted)
                return reject(QStringLiteral("Frozen native solver warnings require explicit acceptance."));
        }
        RJobManager &legacyJobs = RJobManager::getInstance();
        if (legacyJobs.getNRunning() > 0 || legacyJobs.getNWaiting() > 0)
            return reject(QStringLiteral("Solver input snapshots are unavailable while a legacy model job is active or queued."));

        QElapsedTimer captureTimer;
        captureTimer.start();
        if (!session->trylock())
            return reject(QStringLiteral("Solver input snapshot was rejected because the session is in use."));
        struct SessionUnlockGuard
        {
            Session *session;
            ~SessionUnlockGuard() { if (session) session->unlock(); }
        } unlockGuard{session};
        if (legacyJobs.getNRunning() > 0 || legacyJobs.getNWaiting() > 0)
            return reject(QStringLiteral("Solver input snapshots are unavailable while a legacy model job is active or queued."));
        const uint lockedModelID = session->findModelByCommandIdentity(solverTask->getSourceDocumentId());
        if (lockedModelID == RConstants::eod || lockedModelID != currentModelID ||
            session->modelCommandStamp(lockedModelID) != currentStamp)
            return reject(QStringLiteral("The solver task target changed before snapshot capture."));

        std::unique_ptr<Model> modelSnapshot;
        QString originalDocumentFileName;
        try
        {
            if (linkedProjectSnapshot)
                originalDocumentFileName = session->getModel(lockedModelID).getFileName();
            else
                modelSnapshot = std::make_unique<Model>(session->getModel(lockedModelID));
        }
        catch (const std::exception &exception)
        {
            return reject(QStringLiteral("Could not prepare the model for solver snapshot: %1")
                          .arg(QString::fromLocal8Bit(exception.what())));
        }
        if (session->findModelByCommandIdentity(currentStamp.first) != lockedModelID ||
            session->modelCommandStamp(lockedModelID) != currentStamp)
            return reject(QStringLiteral("The solver task source changed while its snapshot was being staged."));
        unlockGuard.session->unlock();
        unlockGuard.session = nullptr;

        if (linkedProjectSnapshot)
        {
            modelSnapshot = std::make_unique<Model>();
            static_cast<RModel&>(*modelSnapshot) = *frozenModelBody;
            modelSnapshot->setFileName(originalDocumentFileName);
        }
        if (session->findModelByCommandIdentity(currentStamp.first) != currentModelID ||
            session->modelCommandStamp(currentModelID) != currentStamp)
            return reject(QStringLiteral("The solver task source changed after its frozen input was loaded."));
        if (!solverTask->captureInputSnapshot(std::move(modelSnapshot), currentStamp, currentModelID,
                                              linkedProjectSnapshot ? QStringLiteral("frozen_project_snapshot")
                                                                    : QStringLiteral("live_unlinked"),
                                              frozenSnapshotPath, frozenSnapshotSha256))
            return reject(QStringLiteral("Failed to capture the solver input snapshot for the selected document."));

        const qint64 elapsed = captureTimer.elapsed();
        const qint64 bytes = QFileInfo(solverTask->getInputSnapshotFileName()).size();
        if (elapsed >= 1000)
            RLogger::warning("Solver input snapshot capture took %lld ms (%lld bytes).\n",
                             static_cast<long long>(elapsed), static_cast<long long>(bytes));
        else
            RLogger::info("Solver input snapshot captured in %lld ms (%lld bytes).\n",
                          static_cast<long long>(elapsed), static_cast<long long>(bytes));
    }

    const QString jobId = solverTask->getTaskID().toString();
    if (jobId.isEmpty() || this->nativeJobs.contains(jobId))
    {
        if (error) *error = QStringLiteral("The native solver job ID is empty or already registered.");
        solverTask->deleteLater();
        return {};
    }

    frozenSnapshotVerified = solverTask->getFrozenSnapshotVerified();
    frozenSnapshotPath = solverTask->getFrozenSnapshotPath();
    frozenSnapshotSha256 = solverTask->getFrozenSnapshotSha256();
    QString registryError;
    if (!this->registerNativeTask(solverTask, jobId, projectSnapshotRef, projectLink,
                                  frozenSnapshotVerified, frozenSnapshotPath, frozenSnapshotSha256,
                                  &registryError))
        return reject(registryError.isEmpty()
                      ? QStringLiteral("The native job could not be persisted before queueing.")
                      : registryError);
    this->lastLogFileName = solverTask->getLogFileName();
    this->RJobManager::submit(solverTask);
    return jobId;
}

QString SolverManager::validateProjectSnapshotRef(uint modelID,
                                                  const QPair<QString,quint64> &sourceStamp,
                                                  const QJsonObject &projectSnapshotRef,
                                                  QString *error) const
{
    if (error) error->clear();
    if (projectSnapshotRef.isEmpty()) return QStringLiteral("unlinked");
    auto reject = [error](const QString &message) { if (error) *error = message; return QString(); };

    const QString projectId = projectSnapshotRef.value(QStringLiteral("projectId")).toString();
    const QString persistentDocumentId = projectSnapshotRef.value(QStringLiteral("persistentDocumentId")).toString();
    const QString sourceRevision = projectSnapshotRef.value(QStringLiteral("sourceRevision")).toString();
    const QString modelSha256 = projectSnapshotRef.value(QStringLiteral("modelSha256")).toString();
    const QString relativePath = projectSnapshotRef.value(QStringLiteral("relativePath")).toString();
    const QJsonObject instanceRef = projectSnapshotRef.value(QStringLiteral("instanceRef")).toObject();
    const auto isCanonicalUuid = [](const QString &value)
    {
        const QUuid id = QUuid::fromString(value);
        return !id.isNull() && id.toString(QUuid::WithoutBraces).compare(value, Qt::CaseInsensitive) == 0;
    };
    if (!isCanonicalUuid(projectId) || !isCanonicalUuid(persistentDocumentId))
        return reject(QStringLiteral("The frozen project reference has an invalid project or persistent document ID."));
    if (!QRegularExpression(QStringLiteral("^r(0|[1-9][0-9]*)$")).match(sourceRevision).hasMatch())
        return reject(QStringLiteral("The frozen project reference has an invalid source revision."));
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(modelSha256).hasMatch())
        return reject(QStringLiteral("The frozen project reference has an invalid model SHA-256."));
    if (relativePath.isEmpty())
        return reject(QStringLiteral("The frozen project reference has no managed relativePath."));
    const QString instanceDocumentId = instanceRef.value(QStringLiteral("documentId")).toString();
    const QString sessionEpoch = instanceRef.value(QStringLiteral("sessionEpoch")).toString();
    const QJsonValue instanceRevisionValue = instanceRef.value(QStringLiteral("documentRevision"));
    if (!isCanonicalUuid(instanceDocumentId) || !isCanonicalUuid(sessionEpoch) || !instanceRevisionValue.isDouble())
        return reject(QStringLiteral("The frozen project reference has an invalid runtime instanceRef."));
    const double instanceRevisionNumber = instanceRevisionValue.toDouble();
    constexpr double maxExactJsonInteger = 9007199254740991.0;
    if (!std::isfinite(instanceRevisionNumber) || instanceRevisionNumber < 0.0 ||
        instanceRevisionNumber > maxExactJsonInteger || std::floor(instanceRevisionNumber) != instanceRevisionNumber)
        return reject(QStringLiteral("The frozen project reference has an invalid runtime revision."));

    Session *session = Application::instance()->getSession();
    const QPair<QString,quint64> currentStamp = session->modelCommandStamp(modelID);
    if (currentStamp != sourceStamp || instanceDocumentId != currentStamp.first ||
        quint64(instanceRevisionNumber) != currentStamp.second ||
        sessionEpoch != session->getModelCommandHost()->sessionEpoch())
        return reject(QStringLiteral("The frozen project reference belongs to a different runtime document, revision, or session."));

    const QJsonObject documentContext = session->documentProjectContext(modelID);
    if (documentContext.value(QStringLiteral("projectId")).toString() != projectId ||
        documentContext.value(QStringLiteral("persistentDocumentId")).toString() != persistentDocumentId ||
        documentContext.value(QStringLiteral("sourceRevision")).toString() != sourceRevision ||
        documentContext.value(QStringLiteral("modelSha256")).toString() != modelSha256)
        return reject(QStringLiteral("The frozen project reference does not match the current document's accepted project snapshot."));
    QString pathError;
    if (this->resolveFrozenSnapshotPath(projectSnapshotRef, &pathError).isEmpty())
        return reject(pathError);
    return QStringLiteral("linked");
}

QString SolverManager::resolveFrozenSnapshotPath(const QJsonObject &projectSnapshotRef, QString *error) const
{
    if (error) error->clear();
    const QString persistentDocumentId = projectSnapshotRef.value(QStringLiteral("persistentDocumentId")).toString();
    const QString modelSha256 = projectSnapshotRef.value(QStringLiteral("modelSha256")).toString();
    const QString relativePath = projectSnapshotRef.value(QStringLiteral("relativePath")).toString();
    const QString expectedRelativePath = QStringLiteral("models/%1/snapshots/%2/model.rbm")
            .arg(persistentDocumentId, modelSha256);
    if (relativePath != expectedRelativePath)
    {
        if (error) *error = QStringLiteral("The frozen snapshot path does not match its managed document ID and SHA-256.");
        return {};
    }

    Session *session = Application::instance()->getSession();
    const QFileInfo workspaceInfo(session->projectWorkspace());
    const QString workspaceAbsolute = workspaceInfo.absoluteFilePath();
    const QString workspaceCanonical = workspaceInfo.canonicalFilePath();
    if (!workspaceInfo.exists() || !workspaceInfo.isDir() || workspaceInfo.isSymLink() ||
        workspaceCanonical.isEmpty() || !samePathText(workspaceAbsolute, workspaceCanonical))
    {
        if (error) *error = QStringLiteral("The project workspace is unavailable or is not a canonical directory.");
        return {};
    }

    QString current = workspaceCanonical;
    const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.size() != 5 || components.at(0) != QStringLiteral("models") ||
        components.at(1) != persistentDocumentId || components.at(2) != QStringLiteral("snapshots") ||
        components.at(3) != modelSha256 || components.at(4) != QStringLiteral("model.rbm"))
    {
        if (error) *error = QStringLiteral("The frozen snapshot path contains an unexpected managed path component.");
        return {};
    }
    for (qsizetype index = 0; index < components.size(); ++index)
    {
        current = QDir(current).filePath(components.at(index));
        const QFileInfo componentInfo(current);
        const bool finalComponent = index + 1 == components.size();
        if (!componentInfo.exists() || componentInfo.isSymLink() ||
            (finalComponent ? !componentInfo.isFile() : !componentInfo.isDir()))
        {
            if (error) *error = QStringLiteral("The managed frozen snapshot path is missing, linked, or has an invalid type.");
            return {};
        }
        const QString canonicalComponent = componentInfo.canonicalFilePath();
        if (canonicalComponent.isEmpty() || !samePathText(current, canonicalComponent))
        {
            if (error) *error = QStringLiteral("The managed frozen snapshot path resolves through a non-canonical component.");
            return {};
        }
        current = canonicalComponent;
    }
    if (QFileInfo(current).size() <= 0)
    {
        if (error) *error = QStringLiteral("The frozen project snapshot is empty.");
        return {};
    }
    return QFileInfo(current).absoluteFilePath();
}

bool SolverManager::loadVerifiedFrozenSnapshot(const QJsonObject &projectSnapshotRef,
                                               RModel *frozenModel, QString *frozenPath,
                                               QString *frozenSha256, QString *error) const
{
    if (error) error->clear();
    if (frozenPath) frozenPath->clear();
    if (frozenSha256) frozenSha256->clear();
    if (!frozenModel)
    {
        if (error) *error = QStringLiteral("No native model was provided for frozen snapshot loading.");
        return false;
    }
    QString resolveError;
    const QString path = this->resolveFrozenSnapshotPath(projectSnapshotRef, &resolveError);
    if (path.isEmpty())
    {
        if (error) *error = resolveError;
        return false;
    }
    const QString expectedHash = projectSnapshotRef.value(QStringLiteral("modelSha256")).toString();
    const QByteArray beforeRead = sha256File(path);
    if (beforeRead.isEmpty() || QString::fromLatin1(beforeRead.toHex()) != expectedHash)
    {
        if (error) *error = QStringLiteral("The frozen project snapshot bytes do not match modelSha256.");
        return false;
    }
    try
    {
        // Read the native RModel body directly. Model::read performs GUI-side
        // consolidation and metadata normalization that would break snapshot provenance.
        frozenModel->read(path);
    }
    catch (const RError &readError)
    {
        if (error) *error = QStringLiteral("Could not read the frozen native model: %1").arg(readError.getMessage());
        return false;
    }
    catch (const std::exception &readError)
    {
        if (error) *error = QStringLiteral("Could not read the frozen native model: %1").arg(QString::fromLocal8Bit(readError.what()));
        return false;
    }
    QString afterReadPathError;
    const QString afterReadPath = this->resolveFrozenSnapshotPath(projectSnapshotRef, &afterReadPathError);
    const QByteArray afterRead = afterReadPath.isEmpty() ? QByteArray() : sha256File(afterReadPath);
    if (afterReadPath.isEmpty() || !samePathText(path, afterReadPath) || afterRead.isEmpty() ||
        afterRead != beforeRead || QString::fromLatin1(afterRead.toHex()) != expectedHash)
    {
        if (error) *error = afterReadPathError.isEmpty()
                ? QStringLiteral("The frozen project snapshot changed while it was being read.")
                : afterReadPathError;
        return false;
    }
    if (frozenPath) *frozenPath = path;
    if (frozenSha256) *frozenSha256 = QString::fromLatin1(afterRead.toHex());
    return true;
}

bool SolverManager::registerNativeTask(SolverTask *solverTask, const QString &jobId,
                                       const QJsonObject &projectSnapshotRef, const QString &projectLink,
                                       bool frozenSnapshotVerified, const QString &frozenSnapshotPath,
                                       const QString &frozenSnapshotSha256, QString *error)
{
    if (error) error->clear();
    if (!solverTask || this->nativeJobRegistryRoot.isEmpty() || !this->nativeJobOwnerLock ||
        !this->nativeJobOwnerLock->isLocked())
    {
        if (error) *error = QStringLiteral("The managed native job registry or process-owner lock is unavailable.");
        return false;
    }
    if (solverTask->isNativeRecoveryTask() && !solverTask->hasNativeRecoveryLogicalLock())
    {
        if (error) *error = QStringLiteral("A native recovery attempt cannot be registered without its logical lineage lease.");
        return false;
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QPair<QString,quint64> sourceStamp = solverTask->getSourceStamp();
    const QString sessionEpoch = Application::instance()->getSession()->getModelCommandHost()->sessionEpoch();
    const QString inputPath = QFileInfo(solverTask->getInputSnapshotFileName()).absoluteFilePath();
    const QString packagePath = QFileInfo(inputPath).absolutePath();
    const QJsonObject logArtifactRefs{{QStringLiteral("solverLog"), solverTask->getLogFileName()},
                                      {QStringLiteral("convergence"), solverTask->getConvergenceFileName()},
                                      {QStringLiteral("monitoring"), solverTask->getMonitoringFileName()},
                                      {QStringLiteral("nativeProgressJournal"), solverTask->getNativeProgressFileName()},
                                      {QStringLiteral("nativeResultJournal"), solverTask->getNativeResultJournalFileName()}};
    QJsonObject sourceRuntimeStamp{{QStringLiteral("documentId"), sourceStamp.first},
                                   {QStringLiteral("revision"), QString::number(sourceStamp.second)},
                                   {QStringLiteral("sessionEpoch"), sessionEpoch}};
    QString nativeCheckpointDirectory;
    QJsonObject nativeCheckpointContext;
    QString nativeCheckpointUnsupportedReason = QStringLiteral(
        "Periodic recovery checkpoints are only enabled for uncontrolled Newmark structural dynamics.");
    std::unique_ptr<Model> checkpointModel = solverTask->copyCapturedInputForValidation();
    bool supportedCheckpointRun = false;
    if (checkpointModel)
    {
        const RProblemSetup &problemSetup = checkpointModel->getProblemSetup();
        const RStructuralDynamicsSetup &dynamics = problemSetup.getStructuralDynamicsSetup();
        const RProblemTypeMask problemTypes = checkpointModel->getProblemTaskTree().getProblemTypeMask();
        supportedCheckpointRun = problemTypes == R_PROBLEM_STRESS && dynamics.getEnabled() &&
                dynamics.getMethod() == RStructuralTimeIntegrationMethod::Newmark &&
                !problemSetup.getStructuralControlSetup().enabled &&
                !problemSetup.getStructuralCableSetup().enabled &&
                !problemSetup.getStructuralPiezoSetup().enabled &&
                !problemSetup.getStructuralThermalControlSetup().enabled;
        if (problemSetup.getStructuralControlSetup().enabled)
            nativeCheckpointUnsupportedReason = QStringLiteral(
                "The GUI native controller transport has no revision-pinned executable and arguments; accepted checkpoints are unsupported for this controlled job.");
        if (supportedCheckpointRun)
        {
            QString preparationError;
            if (!solverTask->prepareNativeCheckpointInput(&preparationError))
            {
                supportedCheckpointRun = false;
                nativeCheckpointUnsupportedReason = preparationError.isEmpty()
                        ? QStringLiteral("The exact native solver input could not be frozen for checkpoint verification.")
                        : preparationError;
            }
        }
    }
    if (solverTask->isNativeRecoveryTask() && !supportedCheckpointRun)
    {
        if (error) *error = QStringLiteral("A native recovery attempt cannot be queued without its checkpoint seed: %1")
                .arg(nativeCheckpointUnsupportedReason);
        return false;
    }
    const QJsonObject sourceSnapshotRef = projectSnapshotRef.isEmpty()
            ? QJsonObject{{QStringLiteral("kind"), QStringLiteral("gui-native-input-snapshot/v1")},
                          {QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp},
                          {QStringLiteral("inputDerivedFrom"), solverTask->getInputDerivedFrom()},
                          {QStringLiteral("inputSnapshotSha256"), QString::fromLatin1(solverTask->getInputSnapshotSha256().toHex())},
                          {QStringLiteral("inputPackageSha256"), QString::fromLatin1(solverTask->getInputPackageSha256().toHex())}}
            : projectSnapshotRef;

    if (supportedCheckpointRun)
    {
            const RStructuralDynamicsSetup &dynamics = checkpointModel->getProblemSetup().getStructuralDynamicsSetup();
            const QString inputSha256 = QString::fromLatin1(solverTask->getInputSnapshotSha256().toHex());
            const QString solverInputSha256 = QString::fromLatin1(solverTask->getNativeCheckpointModelSha256().toHex());
            const QString sourceSha256 = projectSnapshotRef.value(QStringLiteral("modelSha256")).toString().isEmpty()
                    ? inputSha256
                    : projectSnapshotRef.value(QStringLiteral("modelSha256")).toString();
            const QByteArray runtimeHash = sha256File(Application::instance()->getApplicationSettings()->getSolverPath());
            const QByteArray packageHash = sha256Directory(packagePath);
            if (isLowerSha256(sourceSha256) && isLowerSha256(inputSha256) && isLowerSha256(solverInputSha256) &&
                runtimeHash.size() == 32 &&
                packageHash == solverTask->getInputPackageSha256() && std::isfinite(dynamics.getTimeStep()) &&
                dynamics.getTimeStep() > 0.0)
            {
                nativeCheckpointDirectory = QDir::cleanPath(
                    QDir(getRunRoot(solverTask->getResultArtifactFileName()))
                        .filePath(QStringLiteral("checkpoints")));
                nativeCheckpointContext = {{QStringLiteral("schema"), QString::fromLatin1(nativeCheckpointContextSchema)},
                                           {QStringLiteral("sourceSha256"), sourceSha256},
                                           {QStringLiteral("modelSha256"), solverInputSha256},
                                           {QStringLiteral("controlSha256"), QString::fromLatin1(nativeNoControlSha256().toHex())},
                                           {QStringLiteral("runtimeSha256"), QString::fromLatin1(runtimeHash.toHex())},
                                           {QStringLiteral("packageSha256"), QString::fromLatin1(packageHash.toHex())},
                                           {QStringLiteral("policy"), QString::fromLatin1(nativeCheckpointPolicy)},
                                           {QStringLiteral("maxAcceptedSteps"), 10},
                                           {QStringLiteral("maxSimulationSeconds"), 1.0},
                                           {QStringLiteral("dtS"), dynamics.getTimeStep()}};
                const QByteArray canonicalContext = compactJson(nativeCheckpointContext);
                QString checkpointEnableError;
                if (!solverTask->enableNativeAcceptedCheckpoints(nativeCheckpointDirectory, canonicalContext,
                                                                 &checkpointEnableError))
                {
                    if (solverTask->isNativeRecoveryTask())
                    {
                        if (error) *error = QStringLiteral("The accepted-state recovery seed could not be committed before queueing: %1")
                                .arg(checkpointEnableError);
                        return false;
                    }
                    nativeCheckpointDirectory.clear();
                    nativeCheckpointContext = {};
                    nativeCheckpointUnsupportedReason = QStringLiteral(
                        "The native checkpoint context could not be pinned to the queued solver arguments.");
                }
                else
                {
                    nativeCheckpointUnsupportedReason.clear();
                }
            }
            else
            {
                nativeCheckpointUnsupportedReason = QStringLiteral(
                    "The input package, model, source, runtime, or Newmark time step failed checkpoint pinning.");
                if (solverTask->isNativeRecoveryTask())
                {
                    if (error) *error = QStringLiteral("The native recovery checkpoint context could not be pinned before queueing.");
                    return false;
                }
            }
        }

    const bool provenanceFrozen = solverTask->freezeNativeResultProvenance(sourceRuntimeStamp, sourceSnapshotRef);
    if (!provenanceFrozen)
    {
        if (error) *error = QStringLiteral("Native job source and runtime provenance could not be frozen before queueing.");
        return false;
    }
    const bool nativeResultCatalogExpected = solverTask->expectsNativeResultCatalog();
    const bool linkedSnapshot = projectLink == QStringLiteral("linked") && frozenSnapshotVerified;
    const QString projectId = projectSnapshotRef.value(QStringLiteral("projectId")).toString().isEmpty()
            ? QStringLiteral("unlinked")
            : projectSnapshotRef.value(QStringLiteral("projectId")).toString();
    const QJsonObject recoveryProvenance = solverTask->getNativeRecoveryProvenance();
    const QJsonValue modelIdAtSubmit = solverTask->isNativeRecoveryTask()
            ? QJsonValue(QJsonValue::Null) : QJsonValue(int(solverTask->getModelID()));
    const QString logicalSourceModelFile = solverTask->isNativeRecoveryTask()
            ? recoveryProvenance.value(QStringLiteral("logicalSourceModelFileRef")).toString()
            : solverTask->getSourceModelFileName();
    QJsonObject record{{QStringLiteral("jobId"), jobId},
                       {QStringLiteral("jobManagerId"), int(solverTask->getID())},
                       {QStringLiteral("modelIdAtSubmit"), modelIdAtSubmit},
                       {QStringLiteral("projectId"), projectId},
                       {QStringLiteral("persistentDocumentId"), projectSnapshotRef.value(QStringLiteral("persistentDocumentId"))},
                       {QStringLiteral("state"), QStringLiteral("queued")},
                       {QStringLiteral("createdAt"), now},
                       {QStringLiteral("updatedAt"), now},
                       {QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp},
                       {QStringLiteral("sourceModelFileRef"), logicalSourceModelFile},
                       {QStringLiteral("sourceSnapshotRef"), sourceSnapshotRef},
                       {QStringLiteral("nativeResultProvenance"), solverTask->getNativeResultProvenance()},
                       {QStringLiteral("provenanceFrozen"), provenanceFrozen},
                       {QStringLiteral("nativeResultCatalogExpected"), nativeResultCatalogExpected},
                       {QStringLiteral("projectLink"), projectLink},
                       {QStringLiteral("projectSnapshotRef"), projectSnapshotRef},
                       {QStringLiteral("inputDerivedFrom"), linkedSnapshot
                                                               ? QStringLiteral("frozen_project_snapshot")
                                                               : QStringLiteral("live_unlinked")},
                       {QStringLiteral("frozenSnapshotVerified"), linkedSnapshot},
                       {QStringLiteral("frozenSnapshotPath"), linkedSnapshot ? frozenSnapshotPath : QString()},
                       {QStringLiteral("frozenSnapshotSha256"), linkedSnapshot
                                                                    ? QJsonValue(frozenSnapshotSha256)
                                                                    : QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("inputSnapshotRef"), inputPath},
                       {QStringLiteral("inputSnapshotSha256"), QString::fromLatin1(solverTask->getInputSnapshotSha256().toHex())},
                       {QStringLiteral("inputPackageRef"), packagePath},
                       {QStringLiteral("inputPackageSha256"), QString::fromLatin1(solverTask->getInputPackageSha256().toHex())},
                       {QStringLiteral("nativeCheckpointEnabled"), !nativeCheckpointContext.isEmpty()},
                       {QStringLiteral("nativeCheckpointDirectory"), nativeCheckpointDirectory},
                       {QStringLiteral("nativeCheckpointContext"), nativeCheckpointContext},
                       {QStringLiteral("nativeCheckpointInputModelRef"), solverTask->getNativeCheckpointInputModelFileName()},
                       {QStringLiteral("nativeCheckpointInputModelSha256"), QString::fromLatin1(solverTask->getNativeCheckpointModelSha256().toHex())},
                       {QStringLiteral("nativeCheckpointUnsupportedReason"), nativeCheckpointUnsupportedReason},
                       {QStringLiteral("resultArtifactRef"), solverTask->getResultArtifactFileName()},
                       {QStringLiteral("resultArtifactSha256"), QJsonValue::Null},
                       {QStringLiteral("resultArtifactVerified"), false},
                       {QStringLiteral("resultSourceBinding"), QJsonObject{}},
                       {QStringLiteral("resultSourceBindingVerified"), false},
                       {QStringLiteral("resultArtifactId"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("resultManifestRef"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("resultManifestSha256"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("resultManifestVerified"), false},
                       {QStringLiteral("resultManifestDescriptorReported"), false},
                       {QStringLiteral("nativeResultCatalogCode"), QString()},
                       {QStringLiteral("nativeResultCatalogAvailable"), false},
                       {QStringLiteral("nativeResultCatalogMessage"), QString()},
                       {QStringLiteral("processStarted"), false},
                       {QStringLiteral("processExited"), false},
                       {QStringLiteral("processExitCode"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("nativeProcessId"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("resultFrames"), QJsonArray{}},
                       {QStringLiteral("resultFrameSequence"), 0},
                       {QStringLiteral("resultFramesAvailable"), false},
                       {QStringLiteral("resultFramesStatus"), QStringLiteral("pending")},
                       {QStringLiteral("logArtifactRefs"), logArtifactRefs},
                       {QStringLiteral("progress"), QJsonValue::Null},
                       {QStringLiteral("progressAvailable"), false},
                       {QStringLiteral("progressStatus"), QStringLiteral("pending")},
                       {QStringLiteral("progressPhaseId"), QJsonValue::Null},
                       {QStringLiteral("progressPhase"), QJsonValue::Null},
                       {QStringLiteral("progressFraction"), QJsonValue::Null},
                       {QStringLiteral("progressSequence"), QJsonValue::Null},
                       {QStringLiteral("progressEvents"), QJsonArray{}},
                       {QStringLiteral("progressDiagnostics"), QJsonArray{}},
                       {QStringLiteral("logs"), QJsonArray{}},
                       {QStringLiteral("resultReadbackComplete"), false},
                       {QStringLiteral("cancellationRequested"), false}};

    if (!recoveryProvenance.isEmpty())
    {
        const QJsonObject checkpointSeed = solverTask->getNativeCheckpointSeedProvenance();
        if (solverTask->isNativeRecoveryTask() && checkpointSeed.isEmpty())
        {
            if (error) *error = QStringLiteral("The native recovery attempt has no durable checkpoint seed proof.");
            return false;
        }
        record.insert(QStringLiteral("recovery"), recoveryProvenance);
        if (!checkpointSeed.isEmpty()) record.insert(QStringLiteral("nativeCheckpointSeed"), checkpointSeed);
        record.insert(QStringLiteral("logicalJobId"), recoveryProvenance.value(QStringLiteral("logicalJobId")));
        record.insert(QStringLiteral("recoverySourceJobId"), recoveryProvenance.value(QStringLiteral("recoverySourceJobId")));
        record.insert(QStringLiteral("selectedEnvelopeSha256"), recoveryProvenance.value(QStringLiteral("selectedEnvelopeSha256")));
        record.insert(QStringLiteral("recoveryReplayRange"), recoveryProvenance.value(QStringLiteral("replayRange")));
    }

    record.insert(QStringLiteral("ownerInstanceId"), this->nativeJobOwnerInstanceId);
    record.insert(QStringLiteral("ownerPid"), double(QCoreApplication::instance()
        ? QCoreApplication::instance()->applicationPid() : 0));
    record.insert(QStringLiteral("ownerLockRef"), this->nativeJobOwnerLock->fileName());
    QString persistenceError;
    if (!this->persistNativeJobRecord(jobId, &record, true, &persistenceError))
    {
        if (error) *error = persistenceError;
        return false;
    }
    this->nativeJobs.insert(jobId, record);
    this->nativeJobOrder.append(jobId);
    this->nativeJobRecordPaths.insert(jobId, record.value(QStringLiteral("nativeJobRecordRef")).toString());
    this->nativeJobRecordHashes.insert(jobId, record.value(QStringLiteral("nativeJobRegistrySha256")).toString());
    this->nativeTasks.insert(jobId, solverTask);

    QObject::connect(solverTask, &SolverTask::nativeResultFrameReported, this,
                     [this, jobId](const QString &reportedJobId, const QJsonObject &frame)
    {
        const QJsonObject record = this->nativeJobs.value(jobId);
        if (reportedJobId != jobId || frame.value(QStringLiteral("jobId")).toString() != jobId ||
            frame.value(QStringLiteral("protocolVersion")).toString() != QStringLiteral("range.native-result-frame/v1") ||
            record.value(QStringLiteral("resultFramesStatus")).toString() == QStringLiteral("unavailable")) return;
        QJsonArray frames = record.value(QStringLiteral("resultFrames")).toArray();
        if (frame.value(QStringLiteral("seq")).toDouble() != frames.size() + 1 || frames.size() >= 10000)
        {
            this->updateNativeJob(jobId, {{QStringLiteral("resultFramesAvailable"), false},
                                          {QStringLiteral("resultFramesStatus"), QStringLiteral("unavailable")},
                                          {QStringLiteral("resultFramesUnavailableReason"), QStringLiteral("Native frame sequence or count is invalid.")}});
            return;
        }
        frames.append(frame);
        this->updateNativeJob(jobId, {{QStringLiteral("resultFrames"), frames},
                                      {QStringLiteral("resultFrameSequence"), frame.value(QStringLiteral("seq"))},
                                      {QStringLiteral("resultFramesAvailable"), true},
                                      {QStringLiteral("resultFramesStatus"), QStringLiteral("available")}});
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::nativeResultFramesUnavailable, this,
                     [this, jobId](const QString &reportedJobId, const QString &reason)
    {
        if (reportedJobId != jobId) return;
        this->updateNativeJob(jobId, {{QStringLiteral("resultFramesAvailable"), false},
                                      {QStringLiteral("resultFramesStatus"), QStringLiteral("unavailable")},
                                      {QStringLiteral("resultFramesUnavailableReason"), reason}});
    }, Qt::QueuedConnection);

    QObject::connect(solverTask, &SolverTask::nativeProcessStarted, this,
                     [this, jobId, solverTask](const QString &reportedJobId, qint64 processId)
    {
        if (reportedJobId != jobId || this->nativeTasks.value(jobId).data() != solverTask) return;
        const QJsonObject record = this->nativeJobs.value(jobId);
        const QString state = record.value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("queued") && state != QStringLiteral("running") &&
            state != QStringLiteral("cancelling")) return;

        const QString savedProcessId = record.value(QStringLiteral("nativeProcessId")).toString();
        if (record.value(QStringLiteral("processStarted")).toBool())
        {
            if (processId > 0 && savedProcessId == QString::number(processId)) return;
            this->nativeJobDiagnostics.insert(jobId,
                QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_PROCESS_START_CONFLICT")},
                            {QStringLiteral("message"), QStringLiteral("The owned native task reported conflicting process-start identities.")},
                            {QStringLiteral("recordRef"), this->nativeJobRecordPaths.value(jobId)}});
            return;
        }

        QJsonObject fields{{QStringLiteral("processStarted"), true},
                           {QStringLiteral("processExited"), false},
                           {QStringLiteral("nativeProcessId"), processId > 0
                                ? QJsonValue(QString::number(processId)) : QJsonValue(QJsonValue::Null)},
                           {QStringLiteral("nativeProcessStartedAt"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                           {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        this->updateNativeJob(jobId, fields);
    }, Qt::QueuedConnection);

    QObject::connect(solverTask, &RJob::started, this, [this, jobId]()
    {
        QJsonObject record = this->nativeJobs.value(jobId);
        const QString state = record.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("queued"))
            this->updateNativeJob(jobId, {{QStringLiteral("state"), QStringLiteral("running")},
                                          {QStringLiteral("startedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                          {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::nativeProgressReported, this,
                     [this, jobId](const QString &reportedJobId, const QJsonObject &progressEvent)
    {
        if (reportedJobId != jobId || progressEvent.value(QStringLiteral("jobId")).toString() != jobId ||
            progressEvent.value(QStringLiteral("protocolVersion")).toString() != QStringLiteral("range.native-progress/v1"))
        {
            this->appendNativeJobLog(jobId, QStringLiteral("progress-diagnostic"),
                                     QStringLiteral("Discarded native progress payload with a wrong job ID or protocol."));
            return;
        }
        QJsonArray events = this->nativeJobs.value(jobId).value(QStringLiteral("progressEvents")).toArray();
        QJsonObject event = progressEvent;
        event.insert(QStringLiteral("receivedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        events.append(event);
        constexpr qsizetype maxProgressEvents = 64;
        while (events.size() > maxProgressEvents) events.removeAt(0);
        this->updateNativeJob(jobId, {{QStringLiteral("progress"), event},
                                      {QStringLiteral("progressAvailable"), true},
                                      {QStringLiteral("progressStatus"), QStringLiteral("available")},
                                      {QStringLiteral("progressPhaseId"), progressEvent.value(QStringLiteral("phaseId"))},
                                      {QStringLiteral("progressPhase"), progressEvent.value(QStringLiteral("kind"))},
                                      {QStringLiteral("progressFraction"), progressEvent.value(QStringLiteral("fraction"))},
                                      {QStringLiteral("progressSequence"), progressEvent.value(QStringLiteral("seq"))},
                                      {QStringLiteral("progressEvents"), events},
                                      {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::nativeProgressUnavailable, this,
                     [this, jobId](const QString &reportedJobId, const QString &reason)
    {
        if (reportedJobId != jobId) return;
        this->updateNativeJob(jobId, {{QStringLiteral("progressAvailable"), false},
                                      {QStringLiteral("progressStatus"), QStringLiteral("unavailable")},
                                      {QStringLiteral("progressUnavailableReason"), reason},
                                      {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::nativeProgressDiagnostic, this,
                     [this, jobId](const QString &reportedJobId, const QString &message)
    {
        if (reportedJobId != jobId) return;
        QJsonArray diagnostics = this->nativeJobs.value(jobId).value(QStringLiteral("progressDiagnostics")).toArray();
        diagnostics.append(QJsonObject{{QStringLiteral("message"), message},
                                       {QStringLiteral("receivedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
        constexpr qsizetype maxProgressDiagnostics = 32;
        while (diagnostics.size() > maxProgressDiagnostics) diagnostics.removeAt(0);
        this->updateNativeJob(jobId, {{QStringLiteral("progressDiagnostics"), diagnostics},
                                      {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::readyReadStandardOutput, this,
                     [this, jobId](const QString &message)
    {
        this->appendNativeJobLog(jobId, QStringLiteral("stdout"), message);
        this->onReadyReadStandardOutput(message);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::readyReadStandardError, this,
                     [this, jobId](const QString &message)
    {
        this->appendNativeJobLog(jobId, QStringLiteral("stderr"), message);
        this->onReadyReadStandardError(message);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::resultArtifactCreated, this,
                     [this, jobId](const QString &fileName, const QString &sha256,
                                   const QJsonObject &sourceBinding)
    {
        const QString actualPath = QFileInfo(fileName).absoluteFilePath();
        const QString reportedCanonicalPath = QFileInfo(fileName).canonicalFilePath();
        const QString expectedPath = this->nativeJobs.value(jobId)
                .value(QStringLiteral("resultArtifactRef")).toString();
        const QString expectedCanonicalPath = QFileInfo(expectedPath).canonicalFilePath();
        const bool verified = !actualPath.isEmpty() && !reportedCanonicalPath.isEmpty() &&
                !expectedCanonicalPath.isEmpty() &&
                validResultSourceBinding(sourceBinding, sha256, reportedCanonicalPath, expectedCanonicalPath);
        QJsonObject fields{{QStringLiteral("resultArtifactSha256"), verified ? QJsonValue(sha256) : QJsonValue(QJsonValue::Null)},
                           {QStringLiteral("resultArtifactVerified"), verified},
                           {QStringLiteral("resultSourceBindingVerified"), verified},
                           {QStringLiteral("resultReadbackComplete"), verified},
                           {QStringLiteral("resultArtifactValidationError"), verified ? QString() : QStringLiteral("Worker result path, raw SHA-256, or source chain did not match the registered output.")},
                           {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        if (verified)
        {
            fields.insert(QStringLiteral("resultArtifactRef"), actualPath);
            fields.insert(QStringLiteral("resultSourceBinding"), sourceBinding);
        }
        this->updateNativeJob(jobId, fields);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::nativeResultManifestPublished, this,
                     [this, jobId](const QString &reportedJobId, const QJsonObject &descriptor)
    {
        if (reportedJobId != jobId) return;
        const QJsonObject currentRecord = this->nativeJobs.value(jobId);
        QString validationError;
        const bool verified = validNativeResultManifestDescriptor(descriptor, currentRecord,
                                                                  jobId, &validationError);
        QJsonObject fields{{QStringLiteral("resultManifestDescriptorReported"), true},
                           {QStringLiteral("resultManifestVerified"), verified},
                           {QStringLiteral("nativeResultCatalogAvailable"), verified},
                           {QStringLiteral("nativeResultCatalogMessage"), verified
                                ? QStringLiteral("The immutable native result manifest was verified.")
                                : validationError},
                           {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        if (verified)
        {
            fields.insert(QStringLiteral("resultArtifactId"), descriptor.value(QStringLiteral("resultArtifactId")));
            this->nativeResultDescriptors.insert(jobId, descriptor);
            fields.insert(QStringLiteral("resultManifestRef"), QFileInfo(
                descriptor.value(QStringLiteral("resultManifestRef")).toString()).absoluteFilePath());
            fields.insert(QStringLiteral("resultManifestSha256"), descriptor.value(QStringLiteral("resultManifestSha256")));
        }
        else
        {
            fields.insert(QStringLiteral("resultManifestValidationError"), validationError);
        }
        this->updateNativeJob(jobId, fields);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::nativeResultCatalogUnavailable, this,
                     [this, jobId](const QString &reportedJobId, const QString &code, const QString &message)
    {
        if (reportedJobId != jobId || code != QStringLiteral("UNSUPPORTED_ANALYSIS_MODE")) return;
        this->updateNativeJob(jobId, {{QStringLiteral("nativeResultCatalogCode"), code},
                                      {QStringLiteral("nativeResultCatalogAvailable"), false},
                                      {QStringLiteral("nativeResultCatalogMessage"), message},
                                      {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &SolverTask::terminalReported, this,
                     [this, jobId, solverTask](const QString &reportedJobId, const QString &reportedState,
                                   bool processStarted, bool processExited, int exitCode,
                                   bool resultReadbackComplete, const QString &resultArtifactPath,
                                   const QString &resultArtifactSha256, bool resultArtifactVerified,
                                   const QString &message)
    {
        if (reportedJobId != jobId) return;
        const QString registeredResultPath = this->nativeJobs.value(jobId)
                .value(QStringLiteral("resultArtifactRef")).toString();
        const QJsonObject currentRecord = this->nativeJobs.value(jobId);
        const QString actualResultPath = resultArtifactPath.isEmpty()
                ? QString() : QFileInfo(resultArtifactPath).absoluteFilePath();
        const QString actualCanonicalPath = actualResultPath.isEmpty()
                ? QString() : QFileInfo(actualResultPath).canonicalFilePath();
        const QString registeredCanonicalPath = QFileInfo(registeredResultPath).canonicalFilePath();
        const bool validResultHash = QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
                .match(resultArtifactSha256).hasMatch();
        const bool registeredSourceBinding = currentRecord.value(QStringLiteral("resultSourceBindingVerified")).toBool() &&
                validResultSourceBinding(currentRecord.value(QStringLiteral("resultSourceBinding")).toObject(),
                                         resultArtifactSha256, actualCanonicalPath, registeredCanonicalPath);
        const bool verifiedArtifact = resultReadbackComplete && resultArtifactVerified &&
                !actualResultPath.isEmpty() && actualResultPath == QFileInfo(registeredResultPath).absoluteFilePath() &&
                validResultHash && currentRecord.value(QStringLiteral("resultReadbackComplete")).toBool() &&
                currentRecord.value(QStringLiteral("resultArtifactVerified")).toBool() &&
                currentRecord.value(QStringLiteral("resultArtifactSha256")).toString() == resultArtifactSha256 &&
                registeredSourceBinding;
        QString manifestError;
        const bool verifiedManifest = currentRecord.value(QStringLiteral("resultManifestDescriptorReported")).toBool() &&
                currentRecord.value(QStringLiteral("resultManifestVerified")).toBool() &&
                currentRecord.value(QStringLiteral("nativeResultCatalogAvailable")).toBool() &&
                validNativeResultManifestDescriptor(this->nativeResultDescriptors.value(jobId),
                                                    currentRecord, jobId, &manifestError);
        const bool explicitlyUnsupported = !currentRecord.value(QStringLiteral("nativeResultCatalogExpected")).toBool() &&
                currentRecord.value(QStringLiteral("nativeResultCatalogCode")).toString() ==
                QStringLiteral("UNSUPPORTED_ANALYSIS_MODE");
        const bool catalogEvidenceComplete = verifiedManifest || explicitlyUnsupported;
        const bool completeEvidence = processStarted && processExited && exitCode == 0 &&
                verifiedArtifact && catalogEvidenceComplete;
        QString state = reportedState;
        if (state == QStringLiteral("succeeded") && !completeEvidence)
            state = QStringLiteral("failed");
        if (state == QStringLiteral("cancelled") && processStarted && !processExited)
            state = QStringLiteral("failed");
        if (state != QStringLiteral("succeeded") && state != QStringLiteral("failed") && state != QStringLiteral("cancelled"))
            state = QStringLiteral("failed");
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QString terminalMessage = message;
        if (state == QStringLiteral("failed") && message.isEmpty() && !catalogEvidenceComplete)
        {
            terminalMessage = currentRecord.value(QStringLiteral("nativeResultCatalogMessage")).toString();
            if (terminalMessage.isEmpty())
                terminalMessage = QStringLiteral("Native result publication did not provide a verified manifest descriptor.");
        }
        QJsonObject fields{{QStringLiteral("state"), state},
                           {QStringLiteral("finishedAt"), now},
                           {QStringLiteral("updatedAt"), now},
                           {QStringLiteral("processStarted"), processStarted},
                           {QStringLiteral("processExited"), processExited},
                           {QStringLiteral("processExitCode"), exitCode},
                           {QStringLiteral("resultReadbackComplete"), verifiedArtifact},
                           {QStringLiteral("resultArtifactSha256"), verifiedArtifact
                                                                        ? QJsonValue(resultArtifactSha256)
                                                                        : QJsonValue(QJsonValue::Null)},
                           {QStringLiteral("resultArtifactVerified"), verifiedArtifact},
                           {QStringLiteral("resultSourceBindingVerified"), verifiedArtifact},
                           {QStringLiteral("resultManifestVerified"), verifiedManifest},
                           {QStringLiteral("nativeResultCatalogAvailable"), verifiedManifest},
                           {QStringLiteral("message"), terminalMessage}};
        if (verifiedArtifact)
            fields.insert(QStringLiteral("resultArtifactRef"), actualResultPath);
        else if (!actualResultPath.isEmpty())
            fields.insert(QStringLiteral("resultArtifactReportedPath"), actualResultPath);
        if (this->nativeJobs.value(jobId).value(QStringLiteral("progressStatus")).toString() == QStringLiteral("pending"))
        {
            fields.insert(QStringLiteral("progressAvailable"), false);
            fields.insert(QStringLiteral("progressStatus"), QStringLiteral("unavailable"));
            fields.insert(QStringLiteral("progressUnavailableReason"), QStringLiteral("No valid native progress journal was reported for this task."));
        }
        const bool terminalPersisted = this->updateNativeJob(jobId, fields);
        const QString persistedState = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (terminalPersisted &&
            (persistedState == QStringLiteral("succeeded") || persistedState == QStringLiteral("failed") ||
             persistedState == QStringLiteral("cancelled")))
        {
            solverTask->releaseNativeRecoveryLogicalLock();
            this->nativeRecoveryLogicalLocks.remove(jobId);
        }
        if (state == QStringLiteral("succeeded") && verifiedArtifact)
            this->lastResultArtifactFileName = actualResultPath;
        this->nativeTasks.remove(jobId);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &RJob::failed, this, [this, jobId, solverTask]()
    {
        const QString state = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("queued") && state != QStringLiteral("running") && state != QStringLiteral("cancelling")) return;
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const bool terminalPersisted = this->updateNativeJob(jobId,
            {{QStringLiteral("state"), state == QStringLiteral("cancelling")
                                          ? QStringLiteral("cancelled") : QStringLiteral("failed")},
             {QStringLiteral("finishedAt"), now},
             {QStringLiteral("updatedAt"), now},
             {QStringLiteral("message"), QStringLiteral("Native solver ended without complete terminal evidence.")}});
        const QString persistedState = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (terminalPersisted &&
            (persistedState == QStringLiteral("succeeded") || persistedState == QStringLiteral("failed") ||
             persistedState == QStringLiteral("cancelled")))
        {
            solverTask->releaseNativeRecoveryLogicalLock();
            this->nativeRecoveryLogicalLocks.remove(jobId);
        }
        this->nativeTasks.remove(jobId);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &RJob::finished, this, [this, jobId, solverTask]()
    {
        const QString state = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("queued") && state != QStringLiteral("running") && state != QStringLiteral("cancelling")) return;
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const bool terminalPersisted = this->updateNativeJob(jobId,
            {{QStringLiteral("state"), QStringLiteral("failed")},
             {QStringLiteral("finishedAt"), now},
             {QStringLiteral("updatedAt"), now},
             {QStringLiteral("message"), QStringLiteral("Native solver returned without complete terminal evidence.")}});
        const QString persistedState = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (terminalPersisted &&
            (persistedState == QStringLiteral("succeeded") || persistedState == QStringLiteral("failed") ||
             persistedState == QStringLiteral("cancelled")))
        {
            solverTask->releaseNativeRecoveryLogicalLock();
            this->nativeRecoveryLogicalLocks.remove(jobId);
        }
        this->nativeTasks.remove(jobId);
    }, Qt::QueuedConnection);

    return true;
}

bool SolverManager::persistNativeJobRecord(const QString &jobId, QJsonObject *record,
                                          bool createOnly, QString *error)
{
    if (error) error->clear();
    if (!record || !isNativeJobId(jobId) || record->value(QStringLiteral("jobId")).toString() != jobId ||
        this->nativeJobRegistryRoot.isEmpty())
    {
        if (error) *error = QStringLiteral("The native job registry identity or root is invalid.");
        return false;
    }
    QString projectId = record->value(QStringLiteral("projectId")).toString();
    if (projectId != QStringLiteral("unlinked") && !isCanonicalProjectId(projectId))
    {
        if (error) *error = QStringLiteral("The native job record has no valid project identity.");
        return false;
    }

    const QString registryRoot = QDir::cleanPath(QFileInfo(this->nativeJobRegistryRoot).absoluteFilePath());
    if (!QDir().mkpath(registryRoot) || hasSymlinkComponent(registryRoot) ||
        QFileInfo(registryRoot).canonicalFilePath().isEmpty() ||
        !samePathText(registryRoot, QFileInfo(registryRoot).canonicalFilePath()))
    {
        if (error) *error = QStringLiteral("The managed native job registry root is unavailable or non-canonical.");
        return false;
    }
    const QString projectDirectory = QDir(registryRoot).filePath(projectId);
    const QString jobDirectory = QDir(projectDirectory).filePath(jobId);
    if (QFileInfo(projectDirectory).isSymLink() ||
        (!QFileInfo(projectDirectory).exists() && !QDir().mkpath(projectDirectory)) ||
        hasSymlinkComponent(projectDirectory) ||
        !samePathText(QFileInfo(projectDirectory).absoluteFilePath(), QFileInfo(projectDirectory).canonicalFilePath()) ||
        (createOnly && QDir(projectDirectory).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >=
         maxNativeJobRecordsPerProject))
    {
        if (error) *error = QStringLiteral("The native job project directory is unavailable, non-canonical, or at its record limit.");
        return false;
    }
    const bool jobDirectoryAlreadyExisted = QFileInfo(jobDirectory).exists();
    if (createOnly && jobDirectoryAlreadyExisted)
    {
        if (error) *error = QStringLiteral("A managed native job directory already exists for this ID; it was preserved.");
        return false;
    }
    if (QFileInfo(jobDirectory).isSymLink() ||
        (!QFileInfo(jobDirectory).exists() && !QDir().mkpath(jobDirectory)) ||
        hasSymlinkComponent(jobDirectory) ||
        !samePathText(QFileInfo(jobDirectory).absoluteFilePath(), QFileInfo(jobDirectory).canonicalFilePath()))
    {
        if (error) *error = QStringLiteral("The native job record directory is unavailable or escapes its managed root.");
        return false;
    }
    const QString legacyRecordPath = QDir(jobDirectory).filePath(QStringLiteral("job.json"));
    const QString lockPath = QDir(jobDirectory).filePath(QStringLiteral(".job-record.lock"));
    if (QFileInfo(lockPath).isSymLink() || QFileInfo(legacyRecordPath).isSymLink())
    {
        if (error) *error = QStringLiteral("The native job registry lock or record cannot be a symbolic link.");
        return false;
    }
    const QString priorRecordPath = record->value(QStringLiteral("nativeJobRecordRef")).toString();
    if (!priorRecordPath.isEmpty() &&
        !samePathText(QFileInfo(priorRecordPath).absolutePath(), jobDirectory))
    {
        if (error) *error = QStringLiteral("The native job record path escaped its managed job directory.");
        return false;
    }

    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
    {
        if (error) *error = QStringLiteral("The native job record is locked by another writer.");
        return false;
    }

    NativeJobRegistrySnapshot currentSnapshot;
    bool hasCurrentRecord = false;
    if (createOnly)
    {
        const QStringList entries = QDir(jobDirectory).entryList(
            QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &entry : entries)
        {
            if (entry != QFileInfo(lockPath).fileName())
            {
                if (error) *error = QStringLiteral("A native job registry artifact already exists for this ID; it was preserved.");
                return false;
            }
        }
    }
    else
    {
        QString readError;
        if (!readNativeJobRegistryDirectory(jobDirectory, jobId, projectId, &currentSnapshot, &readError))
        {
            if (error) *error = readError;
            return false;
        }
        const QString expectedHash = record->value(QStringLiteral("nativeJobRegistrySha256")).toString();
        if (expectedHash.isEmpty() ||
            currentSnapshot.record.value(QStringLiteral("nativeJobRegistrySha256")).toString() != expectedHash ||
            currentSnapshot.record.value(QStringLiteral("projectId")).toString() != projectId ||
            (!priorRecordPath.isEmpty() && !samePathText(priorRecordPath, currentSnapshot.recordPath)))
        {
            if (error) *error = QStringLiteral("The native job record was externally replaced; the newer bytes were preserved.");
            return false;
        }
        hasCurrentRecord = true;
    }

    QJsonObject persisted = persistedNativeJobRecord(*record);
    persisted.remove(QStringLiteral("nativeJobRegistrySha256"));
    persisted.remove(QStringLiteral("nativeJobRegistryPreviousSha256"));
    persisted.remove(QStringLiteral("nativeJobRecordCommitMarkerRef"));
    const int priorRevision = hasCurrentRecord
            ? currentSnapshot.record.value(QStringLiteral("nativeJobRegistryRevision")).toInt(-1)
            : 0;
    const QString previousSha256 = hasCurrentRecord
            ? currentSnapshot.record.value(QStringLiteral("nativeJobRegistrySha256")).toString()
            : QString::fromLatin1(nativeJobRegistryGenesisSha256);
    if (priorRevision >= std::numeric_limits<int>::max())
    {
        if (error) *error = QStringLiteral("The native job registry revision limit was reached.");
        return false;
    }
    persisted.insert(QStringLiteral("nativeJobRegistrySchema"), QString::fromLatin1(nativeJobRecordSchema));
    persisted.insert(QStringLiteral("nativeJobRegistryRevision"), priorRevision + 1);
    persisted.insert(QStringLiteral("nativeJobRegistryPreviousSha256"), previousSha256);

    bool immutableMarkerStore = false;
#ifdef Q_OS_WIN
    immutableMarkerStore = nativePathIsEncrypted(jobDirectory);
#endif
    immutableMarkerStore = immutableMarkerStore || currentSnapshot.commitMarkerCount > 0;
    QString recordPath = legacyRecordPath;
    QString blobUuid;
    QString markerPath;
    QString blobSha256;
    if (immutableMarkerStore)
    {
        if (currentSnapshot.commitMarkerCount >= maxNativeJobCommitMarkersPerJob)
        {
            if (error) *error = QStringLiteral("The native job commit-marker history reached its bounded revision limit; history was not truncated.");
            return false;
        }
        blobUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLower();
        recordPath = QDir(jobDirectory).filePath(
            QStringLiteral("job-record-r%1-%2.json").arg(priorRevision + 1, 8, 10, QChar('0')).arg(blobUuid));
        persisted.insert(QStringLiteral("nativeJobRecordRef"), recordPath);
    }
    else
    {
        persisted.insert(QStringLiteral("nativeJobRecordRef"), recordPath);
    }
    persisted.insert(QStringLiteral("nativeJobRegistrySha256"), sha256Hex(compactJson(persisted)));
    const QByteArray bytes = compactJson(persisted);
    if (bytes.isEmpty() || bytes.size() > maxNativeJobRecordBytes)
    {
        if (error) *error = QStringLiteral("The native job record exceeded its bounded storage size.");
        return false;
    }

    if (immutableMarkerStore)
    {
        QString writeError;
        const bool requireEncrypted = nativePathIsEncrypted(jobDirectory);
        if (!writeExclusiveNativeRegistryFile(recordPath, bytes, requireEncrypted, &writeError))
        {
            if (error) *error = writeError;
            return false;
        }

        // Re-read the committed head immediately before publishing the marker.
        // A blob without a marker is deliberately invisible and can be diagnosed
        // without replacing the prior committed state.
        if (hasCurrentRecord)
        {
            NativeJobRegistrySnapshot beforeCommit;
            QString rereadError;
            if (!readNativeJobRegistryDirectory(jobDirectory, jobId, projectId, &beforeCommit, &rereadError) ||
                beforeCommit.record.value(QStringLiteral("nativeJobRegistrySha256")).toString() != previousSha256)
            {
                if (error) *error = rereadError.isEmpty()
                        ? QStringLiteral("The prior committed native job SHA changed before its marker; newer bytes were preserved.")
                        : rereadError;
                return false;
            }
        }
        else
        {
            const QStringList entries = QDir(jobDirectory).entryList(
                QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &entry : entries)
            {
                if (entry != QFileInfo(lockPath).fileName() && entry != QFileInfo(recordPath).fileName())
                {
                    if (error) *error = QStringLiteral("Unexpected native job artifacts appeared before the first commit marker; they were preserved.");
                    return false;
                }
            }
        }

        blobSha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const QString markerName = QStringLiteral("commit-r%1-%2-%3-%4.marker")
                .arg(priorRevision + 1, 8, 10, QChar('0'))
                .arg(blobUuid, previousSha256, blobSha256);
        markerPath = QDir(jobDirectory).filePath(markerName);
        if (QFileInfo::exists(markerPath) || QFileInfo(markerPath).isSymLink())
        {
            if (error) *error = QStringLiteral("The native job commit marker path already exists; no record was overwritten.");
            return false;
        }
        if (!createExclusiveNativeCommitMarker(markerPath, &writeError))
        {
            if (error) *error = QStringLiteral("The job blob is uncommitted because its empty commit marker could not be created: %1")
                    .arg(writeError);
            return false;
        }

        NativeJobRegistrySnapshot committed;
        QString committedError;
        if (!readNativeJobRegistryDirectory(jobDirectory, jobId, projectId, &committed, &committedError) ||
            committed.record.value(QStringLiteral("nativeJobRegistrySha256")).toString() !=
                persisted.value(QStringLiteral("nativeJobRegistrySha256")).toString() ||
            !samePathText(committed.recordPath, recordPath))
        {
            if (error) *error = committedError.isEmpty()
                    ? QStringLiteral("The new commit marker did not publish the expected immutable registry blob.")
                    : committedError;
            return false;
        }
        if (!committed.diagnostic.isEmpty())
            this->nativeJobDiagnostics.insert(jobId,
                QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_JOB_UNCOMMITTED_BLOB")},
                            {QStringLiteral("message"), committed.diagnostic},
                            {QStringLiteral("recordRef"), committed.recordPath}});
    }
    else
    {
        if (createOnly)
        {
            if (QFileInfo::exists(recordPath) || QFileInfo(recordPath).isSymLink())
            {
                if (error) *error = QStringLiteral("A native job record already exists for this job ID.");
                return false;
            }
        }
        else
        {
            NativeJobRegistrySnapshot beforeWrite;
            QString rereadError;
            if (!readNativeJobRegistryDirectory(jobDirectory, jobId, projectId, &beforeWrite, &rereadError) ||
                beforeWrite.record.value(QStringLiteral("nativeJobRegistrySha256")).toString() != previousSha256)
            {
                if (error) *error = rereadError.isEmpty()
                        ? QStringLiteral("The prior native job SHA changed before replacement; newer bytes were preserved.")
                        : rereadError;
                return false;
            }
        }
        QSaveFile output(recordPath);
        if (!output.open(QIODevice::WriteOnly))
        {
            if (error) *error = QStringLiteral("Could not open QSaveFile '%1' (error %2): %3")
                    .arg(recordPath, QString::number(int(output.error())), output.errorString().left(512));
            return false;
        }
        const qint64 written = output.write(bytes);
        if (written != bytes.size())
        {
            if (error) *error = QStringLiteral("QSaveFile wrote %1 of %2 bytes to '%3' (error %4): %5")
                    .arg(QString::number(written), QString::number(bytes.size()), recordPath,
                         QString::number(int(output.error())), output.errorString().left(512));
            output.cancelWriting();
            return false;
        }
        if (!output.commit())
        {
            if (error) *error = QStringLiteral("QSaveFile commit failed for '%1' (error %2): %3")
                    .arg(recordPath, QString::number(int(output.error())), output.errorString().left(512));
            return false;
        }
        if (sha256File(recordPath) != QCryptographicHash::hash(bytes, QCryptographicHash::Sha256))
        {
            if (error) *error = QStringLiteral("The native job record failed post-write SHA-256 verification.");
            return false;
        }
    }

    record->insert(QStringLiteral("nativeJobRecordRef"), recordPath);
    record->insert(QStringLiteral("nativeJobRegistrySchema"), QString::fromLatin1(nativeJobRecordSchema));
    record->insert(QStringLiteral("nativeJobRegistryRevision"), priorRevision + 1);
    record->insert(QStringLiteral("nativeJobRegistryPreviousSha256"), previousSha256);
    record->insert(QStringLiteral("nativeJobRegistrySha256"), persisted.value(QStringLiteral("nativeJobRegistrySha256")));
    this->nativeJobRecordPaths.insert(jobId, recordPath);
    this->nativeJobRecordHashes.insert(jobId, record->value(QStringLiteral("nativeJobRegistrySha256")).toString());
    return true;
}

void SolverManager::loadNativeJobsFromDiskOnOwner()
{
    if (QThread::currentThread() != this->thread() || this->nativeJobRegistryRoot.isEmpty()) return;
    Session *session = Application::instance()->getSession();
    if (QThread::currentThread() != session->thread()) return;

    QStringList projectIds{QStringLiteral("unlinked")};
    const QJsonObject project = session->projectContext();
    const QString activeProjectId = project.value(QStringLiteral("projectId")).toString();
    if (project.value(QStringLiteral("availability")).toString() == QStringLiteral("available") &&
        isCanonicalProjectId(activeProjectId) && !projectIds.contains(activeProjectId))
        projectIds.append(activeProjectId);

    for (const QString &projectId : projectIds)
    {
        const QString projectDirectory = QDir(this->nativeJobRegistryRoot).filePath(projectId);
        const QFileInfo projectInfo(projectDirectory);
        if (!projectInfo.exists()) continue;
        if (!projectInfo.isDir() || projectInfo.isSymLink() || hasSymlinkComponent(projectInfo.absoluteFilePath()))
        {
            this->nativeJobDiagnostics.insert(QStringLiteral("registry:%1").arg(projectId),
                QJsonObject{{QStringLiteral("code"), QStringLiteral("REGISTRY_PATH_INVALID")},
                            {QStringLiteral("message"), QStringLiteral("A project native-job registry path is linked or has an invalid type.")}});
            continue;
        }
        const QFileInfoList jobDirectories = QDir(projectDirectory).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        qsizetype loadedCount = 0;
        for (const QFileInfo &jobInfo : jobDirectories)
        {
            if (loadedCount++ >= maxNativeJobRecordsPerProject) break;
            const QString jobId = jobInfo.fileName();
            if (!isNativeJobId(jobId)) continue;
            if (this->nativeJobs.contains(jobId)) continue;
            NativeJobRegistrySnapshot registrySnapshot;
            QString readError;
            if (!readNativeJobRegistryDirectory(jobInfo.absoluteFilePath(), jobId, projectId,
                                                &registrySnapshot, &readError))
            {
                this->nativeJobDiagnostics.insert(jobId,
                    QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_JOB_RECORD_INVALID")},
                                {QStringLiteral("message"), readError},
                                {QStringLiteral("registryDirectory"), jobInfo.absoluteFilePath()}});
                continue;
            }
            QJsonObject record = registrySnapshot.record;
            const QString recordPath = registrySnapshot.recordPath;
            if (
                record.value(QStringLiteral("projectId")).toString() != projectId ||
                !samePathText(record.value(QStringLiteral("nativeJobRecordRef")).toString(), recordPath))
            {
                readError = QStringLiteral("The native job identity does not match its managed directory or committed blob.");
                this->nativeJobDiagnostics.insert(jobId,
                    QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_JOB_RECORD_INVALID")},
                                {QStringLiteral("message"), readError},
                                {QStringLiteral("recordRef"), recordPath}});
                continue;
            }
            const QString ownerId = record.value(QStringLiteral("ownerInstanceId")).toString();
            const QString ownerLockPath = record.value(QStringLiteral("ownerLockRef")).toString();
            const QString ownerRoot = QDir(QFileInfo(this->nativeJobRegistryRoot).absolutePath())
                    .filePath(QStringLiteral("native-job-owners"));
            const QString expectedOwnerLock = QDir(ownerRoot).filePath(ownerId + QStringLiteral(".lock"));
            if (!isCanonicalProjectId(ownerId) || !QFileInfo(ownerLockPath).isAbsolute() ||
                !samePathText(ownerLockPath, expectedOwnerLock) || hasSymlinkComponent(ownerLockPath))
            {
                this->nativeJobDiagnostics.insert(jobId,
                    QJsonObject{{QStringLiteral("code"), QStringLiteral("OWNER_BINDING_INVALID")},
                                {QStringLiteral("message"), QStringLiteral("The persisted job owner lock is not bound to a managed process identity.")},
                                {QStringLiteral("recordRef"), recordPath}});
                continue;
            }

            this->nativeJobs.insert(jobId, record);
            this->nativeJobOrder.append(jobId);
            this->nativeJobRecordPaths.insert(jobId, recordPath);
            this->nativeJobRecordHashes.insert(jobId,
                record.value(QStringLiteral("nativeJobRegistrySha256")).toString());
            if (!registrySnapshot.diagnostic.isEmpty())
                this->nativeJobDiagnostics.insert(jobId,
                    QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_JOB_UNCOMMITTED_BLOB")},
                                {QStringLiteral("message"), registrySnapshot.diagnostic},
                                {QStringLiteral("recordRef"), recordPath}});

            QLockFile ownerLock(ownerLockPath);
            ownerLock.setStaleLockTime(0);
            const bool ownerLockAvailable = ownerLock.tryLock(0);
            if (ownerLockAvailable) ownerLock.unlock();
            const QString state = record.value(QStringLiteral("state")).toString();
            if (ownerLockAvailable && (state == QStringLiteral("queued") || state == QStringLiteral("running") ||
                                       state == QStringLiteral("cancelling")))
            {
                record.insert(QStringLiteral("state"), QStringLiteral("interrupted"));
                record.insert(QStringLiteral("interruptedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                record.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                record.insert(QStringLiteral("interruptedReason"),
                              QStringLiteral("The owning GUI process lock is no longer held; the native job was not re-executed."));
                QString persistError;
                if (!this->persistNativeJobRecord(jobId, &record, false, &persistError))
                {
                    this->nativeJobDiagnostics.insert(jobId,
                        QJsonObject{{QStringLiteral("code"), QStringLiteral("INTERRUPTION_PERSIST_FAILED")},
                                    {QStringLiteral("message"), persistError},
                                    {QStringLiteral("recordRef"), recordPath}});
                }
                this->nativeJobs.insert(jobId, record);
            }

        }
    }
}

bool SolverManager::validateNativeJobRecordOnDisk(const QString &jobId, QString *error) const
{
    if (error) error->clear();
    const QString path = this->nativeJobRecordPaths.value(jobId);
    if (path.isEmpty())
    {
        if (error) *error = QStringLiteral("No durable native job record is registered for this ID.");
        return false;
    }
    const QJsonObject memoryRecord = this->nativeJobs.value(jobId);
    const QString projectId = memoryRecord.value(QStringLiteral("projectId")).toString();
    NativeJobRegistrySnapshot registrySnapshot;
    const QString jobDirectory = QFileInfo(path).absolutePath();
    if (!readNativeJobRegistryDirectory(jobDirectory, jobId, projectId, &registrySnapshot, error)) return false;
    const QJsonObject &diskRecord = registrySnapshot.record;
    if (diskRecord.value(QStringLiteral("nativeJobRegistrySha256")).toString() !=
            this->nativeJobRecordHashes.value(jobId) ||
        !samePathText(registrySnapshot.recordPath, path) ||
        diskRecord.value(QStringLiteral("projectId")).toString() !=
            memoryRecord.value(QStringLiteral("projectId")).toString() ||
        persistedNativeJobRecord(memoryRecord) != diskRecord)
    {
        if (error) *error = QStringLiteral("The native job record changed outside its registered writer; external bytes were preserved.");
        return false;
    }
    return true;
}

bool SolverManager::revalidateNativeTerminalArtifactsOnOwner(const QString &jobId, QString *error)
{
    if (error) error->clear();
    auto recordIt = this->nativeJobs.find(jobId);
    if (recordIt == this->nativeJobs.end())
    {
        if (error) *error = QStringLiteral("The native job is unavailable for terminal artifact verification.");
        return false;
    }
    QJsonObject record = recordIt.value();
    const QString resultPath = record.value(QStringLiteral("resultArtifactRef")).toString();
    const QString resultSha = record.value(QStringLiteral("resultArtifactSha256")).toString();
    const QJsonObject binding = record.value(QStringLiteral("resultSourceBinding")).toObject();
    if (!record.value(QStringLiteral("resultArtifactVerified")).toBool() ||
        !record.value(QStringLiteral("resultSourceBindingVerified")).toBool() ||
        !validResultSourceBinding(binding, resultSha, resultPath, resultPath))
    {
        if (error) *error = QStringLiteral("The terminal native result artifact or its source chain no longer verifies.");
        return false;
    }

    if (record.value(QStringLiteral("resultManifestDescriptorReported")).toBool())
    {
        QJsonObject descriptor = this->nativeResultDescriptors.value(jobId);
        if (descriptor.isEmpty())
        {
            const QString manifestPath = record.value(QStringLiteral("resultManifestRef")).toString();
            const QString manifestSha = record.value(QStringLiteral("resultManifestSha256")).toString();
            QJsonObject manifest;
            QString readError;
            if (!isLowerSha256(manifestSha) || !readSmallObject(manifestPath, 16 * 1024 * 1024, &manifest, &readError) ||
                sha256File(manifestPath) != QByteArray::fromHex(manifestSha.toLatin1()))
            {
                if (error) *error = readError.isEmpty()
                        ? QStringLiteral("The terminal result manifest bytes changed or no longer match their registry hash.")
                        : readError;
                return false;
            }
            descriptor = {{QStringLiteral("jobId"), jobId},
                          {QStringLiteral("provenanceFrozen"), record.value(QStringLiteral("provenanceFrozen"))},
                          {QStringLiteral("nativeResultCatalogAvailable"), true},
                          {QStringLiteral("resultProvenance"), record.value(QStringLiteral("nativeResultProvenance"))},
                          {QStringLiteral("resultManifest"), manifest},
                          {QStringLiteral("resultManifestRef"), manifestPath},
                          {QStringLiteral("resultManifestSha256"), manifestSha},
                          {QStringLiteral("resultArtifactId"), record.value(QStringLiteral("resultArtifactId"))}};
        }
        QString manifestError;
        if (!validNativeResultManifestDescriptor(descriptor, record, jobId, &manifestError))
        {
            if (error) *error = manifestError;
            return false;
        }
        this->nativeResultDescriptors.insert(jobId, descriptor);
    }
    else if (record.value(QStringLiteral("nativeResultCatalogExpected")).toBool())
    {
        if (error) *error = QStringLiteral("The terminal job expected a native result manifest but did not persist one.");
        return false;
    }
    return true;
}

bool SolverManager::updateNativeJob(const QString &jobId, const QJsonObject &fields)
{
    auto it = this->nativeJobs.find(jobId);
    if (it == this->nativeJobs.end()) return false;
    static const QSet<QString> transientFields{
        QStringLiteral("logs"), QStringLiteral("resultFrames"), QStringLiteral("resultFrameSequence"),
        QStringLiteral("resultFramesAvailable"), QStringLiteral("resultFramesStatus"),
        QStringLiteral("resultFramesUnavailableReason"), QStringLiteral("progress"),
        QStringLiteral("progressAvailable"), QStringLiteral("progressStatus"),
        QStringLiteral("progressPhaseId"), QStringLiteral("progressPhase"),
        QStringLiteral("progressFraction"), QStringLiteral("progressSequence"),
        QStringLiteral("progressEvents"), QStringLiteral("progressDiagnostics"),
        QStringLiteral("progressUnavailableReason"), QStringLiteral("updatedAt")};
    bool durableChange = false;
    for (auto field = fields.begin(); field != fields.end(); ++field)
        if (!transientFields.contains(field.key())) durableChange = true;
    QJsonObject record = it.value();
    for (auto field = fields.begin(); field != fields.end(); ++field)
    {
        if (!durableChange && field.key() == QStringLiteral("updatedAt")) continue;
        record.insert(field.key(), field.value());
    }
    QString persistenceError;
    bool persistenceSucceeded = true;
    if (durableChange && !this->persistNativeJobRecord(jobId, &record, false, &persistenceError))
    {
        persistenceSucceeded = false;
        this->nativeJobDiagnostics.insert(jobId,
            QJsonObject{{QStringLiteral("code"), QStringLiteral("NATIVE_JOB_PERSISTENCE_FAILED")},
                        {QStringLiteral("message"), persistenceError},
                        {QStringLiteral("recordRef"), this->nativeJobRecordPaths.value(jobId)}});
    }
    else if (durableChange)
    {
        this->nativeJobDiagnostics.remove(jobId);
    }
    it.value() = record;
    if (durableChange && persistenceSucceeded)
        emit this->nativeJobUpdated(jobId, fields);
    return persistenceSucceeded;
}

void SolverManager::appendNativeJobLog(const QString &jobId, const QString &stream, const QString &message)
{
    auto it = this->nativeJobs.find(jobId);
    if (it == this->nativeJobs.end()) return;
    QJsonArray logs = it.value().value(QStringLiteral("logs")).toArray();
    logs.append(QJsonObject{{QStringLiteral("stream"), stream},
                            {QStringLiteral("text"), message.left(2048)},
                            {QStringLiteral("receivedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    constexpr qsizetype maxLogEntries = 64;
    while (logs.size() > maxLogEntries) logs.removeAt(0);
    this->updateNativeJob(jobId, {{QStringLiteral("logs"), logs},
                                  {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
}

QJsonObject SolverManager::jobQuery(const QString &jobId) const
{
    QJsonObject result;
    if (!this->invokeOnOwner([this, &result, &jobId]()
    {
        const_cast<SolverManager*>(this)->loadNativeJobsFromDiskOnOwner();
        if (!this->nativeJobs.contains(jobId))
        {
            result = {{QStringLiteral("found"), false}, {QStringLiteral("jobId"), jobId}};
            const QJsonObject diagnostic = this->nativeJobDiagnostics.value(jobId);
            if (!diagnostic.isEmpty()) result.insert(QStringLiteral("diagnostic"), diagnostic);
        }
        else
        {
            QString registryError;
            const bool registryValid = this->validateNativeJobRecordOnDisk(jobId, &registryError);
            QString artifactError;
            const QString state = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
            const bool terminalState = state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
                    state == QStringLiteral("cancelled") ||
                    (state == QStringLiteral("interrupted") && this->nativeJobs.value(jobId)
                     .value(QStringLiteral("resultManifestDescriptorReported")).toBool());
            const bool artifactsValid = !terminalState ||
                    const_cast<SolverManager*>(this)->revalidateNativeTerminalArtifactsOnOwner(jobId, &artifactError);
            result = this->nativeJobs.value(jobId);
            result.insert(QStringLiteral("found"), true);
            if (!registryValid || !artifactsValid)
            {
                result.insert(QStringLiteral("eligibility"), QStringLiteral("conflict"));
                result.insert(QStringLiteral("registryDiagnostic"), registryValid ? artifactError : registryError);
            }
            const QJsonObject diagnostic = this->nativeJobDiagnostics.value(jobId);
            if (!diagnostic.isEmpty()) result.insert(QStringLiteral("diagnostic"), diagnostic);
        }
    }))
        return {{QStringLiteral("found"), false}, {QStringLiteral("error"), QStringLiteral("wrong_thread")}};
    return result;
}

QJsonObject SolverManager::jobsQuery(int offset, int limit) const
{
    if (offset < 0 || limit < 1 || limit > 200)
        return {{QStringLiteral("error"), QStringLiteral("invalid_pagination")},
                {QStringLiteral("message"), QStringLiteral("offset must be non-negative and limit must be 1 through 200.")}};
    QJsonObject result;
    if (!this->invokeOnOwner([this, offset, limit, &result]()
    {
        const_cast<SolverManager*>(this)->loadNativeJobsFromDiskOnOwner();
        QString activeProjectId = QStringLiteral("unlinked");
        Session *session = Application::instance()->getSession();
        if (QThread::currentThread() == session->thread())
        {
            const QJsonObject project = session->projectContext();
            const QString candidate = project.value(QStringLiteral("projectId")).toString();
            if (project.value(QStringLiteral("availability")).toString() == QStringLiteral("available") &&
                isCanonicalProjectId(candidate)) activeProjectId = candidate;
        }
        QStringList visibleJobIds;
        for (const QString &jobId : this->nativeJobOrder)
        {
            const QString projectId = this->nativeJobs.value(jobId).value(QStringLiteral("projectId")).toString();
            if (projectId == QStringLiteral("unlinked") || projectId == activeProjectId)
                visibleJobIds.append(jobId);
        }
        const int total = visibleJobIds.size();
        const int end = offset >= total ? total : qMin(total, offset + limit);
        QJsonArray jobs;
        for (int index = offset; index < end; ++index)
        {
            const QString jobId = visibleJobIds.at(index);
            QString registryError;
            const bool registryValid = this->validateNativeJobRecordOnDisk(jobId, &registryError);
            QString artifactError;
            const QString state = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
            const bool terminalState = state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
                    state == QStringLiteral("cancelled") ||
                    (state == QStringLiteral("interrupted") && this->nativeJobs.value(jobId)
                     .value(QStringLiteral("resultManifestDescriptorReported")).toBool());
            const bool artifactsValid = !terminalState ||
                    const_cast<SolverManager*>(this)->revalidateNativeTerminalArtifactsOnOwner(jobId, &artifactError);
            QJsonObject record = this->nativeJobs.value(jobId);
            record.insert(QStringLiteral("found"), true);
            if (!registryValid || !artifactsValid)
            {
                record.insert(QStringLiteral("eligibility"), QStringLiteral("conflict"));
                record.insert(QStringLiteral("registryDiagnostic"), registryValid ? artifactError : registryError);
            }
            const QJsonObject diagnostic = this->nativeJobDiagnostics.value(jobId);
            if (!diagnostic.isEmpty()) record.insert(QStringLiteral("diagnostic"), diagnostic);
            jobs.append(record);
        }
        QJsonArray diagnostics;
        for (auto it = this->nativeJobDiagnostics.cbegin(); it != this->nativeJobDiagnostics.cend(); ++it)
        {
            QJsonObject diagnostic = it.value();
            diagnostic.insert(QStringLiteral("jobId"), it.key());
            diagnostics.append(diagnostic);
        }
        result = {{QStringLiteral("jobs"), jobs},
                  {QStringLiteral("offset"), offset},
                  {QStringLiteral("limit"), limit},
                  {QStringLiteral("total"), total},
                  {QStringLiteral("hasMore"), end < total},
                  {QStringLiteral("diagnostics"), diagnostics}};
    }))
        return {{QStringLiteral("error"), QStringLiteral("wrong_thread")}};
    return result;
}

QJsonObject SolverManager::nativeJobRecoveryQuery(const QString &jobId) const
{
    QJsonObject result;
    if (!this->invokeOnOwner([this, &result, &jobId]()
    {
        auto *self = const_cast<SolverManager*>(this);
        self->loadNativeJobsFromDiskOnOwner();
        result = self->nativeJobRecoveryQueryOnOwner(jobId);
    }))
        return {{QStringLiteral("found"), false},
                {QStringLiteral("eligibility"), QStringLiteral("conflict")},
                {QStringLiteral("error"), QStringLiteral("wrong_thread")}};
    return result;
}

QJsonObject SolverManager::nativeJobRecoveryQueryOnOwner(const QString &jobId,
                                                         bool logicalRecoveryLockHeldByCaller)
{
    const auto qualified = [&jobId](const QString &eligibility, const QString &reason,
                                    const QJsonObject &extra = QJsonObject{})
    {
        QJsonObject response{{QStringLiteral("found"), true},
                             {QStringLiteral("jobId"), jobId},
                             {QStringLiteral("eligibility"), eligibility},
                             {QStringLiteral("status"), eligibility},
                             {QStringLiteral("reason"), reason}};
        for (auto it = extra.begin(); it != extra.end(); ++it) response.insert(it.key(), it.value());
        return response;
    };
    if (!isNativeJobId(jobId))
        return {{QStringLiteral("found"), false}, {QStringLiteral("jobId"), jobId},
                {QStringLiteral("eligibility"), QStringLiteral("conflict")},
                {QStringLiteral("status"), QStringLiteral("conflict")},
                {QStringLiteral("reason"), QStringLiteral("The native job ID is invalid.")}};
    if (!this->nativeJobs.contains(jobId))
    {
        const QJsonObject diagnostic = this->nativeJobDiagnostics.value(jobId);
        return {{QStringLiteral("found"), false}, {QStringLiteral("jobId"), jobId},
                {QStringLiteral("eligibility"), QStringLiteral("conflict")},
                {QStringLiteral("status"), QStringLiteral("conflict")},
                {QStringLiteral("reason"), diagnostic.isEmpty()
                    ? QStringLiteral("The native job is not registered in the active project context.")
                    : diagnostic.value(QStringLiteral("message")).toString()},
                {QStringLiteral("diagnostic"), diagnostic}};
    }

    const QJsonObject cachedRecord = this->nativeJobs.value(jobId);
    const QString projectId = cachedRecord.value(QStringLiteral("projectId")).toString();
    const QString cachedLogicalId = logicalNativeJobId(jobId, cachedRecord);
    QHash<QString,QJsonObject> projectRecords;
    QHash<QString,QString> uncommittedDiagnostics;
    QString refreshError;
    if (!refreshNativeProjectJobRecords(this->nativeJobRegistryRoot, projectId,
                                        &projectRecords, &uncommittedDiagnostics, &refreshError))
        return qualified(QStringLiteral("conflict"), refreshError,
                         {{QStringLiteral("projectId"), projectId},
                          {QStringLiteral("logicalJobId"), cachedLogicalId}});
    for (auto it = uncommittedDiagnostics.cbegin(); it != uncommittedDiagnostics.cend(); ++it)
        this->nativeJobDiagnostics.insert(it.key(), QJsonObject{
            {QStringLiteral("code"), QStringLiteral("NATIVE_JOB_UNCOMMITTED_BLOB")},
            {QStringLiteral("message"), it.value()}});
    if (!projectRecords.contains(jobId))
        return qualified(QStringLiteral("conflict"),
                         this->nativeJobDiagnostics.value(jobId).value(QStringLiteral("message")).toString().isEmpty()
                             ? QStringLiteral("The source native job is missing from its persisted project registry.")
                             : this->nativeJobDiagnostics.value(jobId).value(QStringLiteral("message")).toString(),
                         {{QStringLiteral("projectId"), projectId},
                          {QStringLiteral("logicalJobId"), cachedLogicalId}});
    const QJsonObject refreshedRecord = projectRecords.value(jobId);
    if (refreshedRecord.value(QStringLiteral("projectId")).toString() != projectId ||
        logicalNativeJobId(jobId, refreshedRecord) != cachedLogicalId ||
        refreshedRecord.value(QStringLiteral("recoverySourceJobId")) !=
            cachedRecord.value(QStringLiteral("recoverySourceJobId")) ||
        refreshedRecord.value(QStringLiteral("selectedEnvelopeSha256")) !=
            cachedRecord.value(QStringLiteral("selectedEnvelopeSha256")))
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The persisted source job changed its logical recovery identity."),
                         {{QStringLiteral("projectId"), projectId},
                          {QStringLiteral("logicalJobId"), cachedLogicalId}});

    for (auto it = projectRecords.cbegin(); it != projectRecords.cend(); ++it)
    {
        QJsonObject merged = it.value();
        const QJsonObject previouslyLoaded = this->nativeJobs.value(it.key());
        mergeNativeJobTransientFields(&merged, previouslyLoaded);
        this->nativeJobs.insert(it.key(), merged);
        this->nativeJobRecordPaths.insert(it.key(),
            it.value().value(QStringLiteral("nativeJobRecordRef")).toString());
        this->nativeJobRecordHashes.insert(it.key(),
            it.value().value(QStringLiteral("nativeJobRegistrySha256")).toString());
        this->nativeJobDiagnostics.remove(it.key());
        if (!this->nativeJobOrder.contains(it.key())) this->nativeJobOrder.append(it.key());
    }
    QJsonObject record = this->nativeJobs.value(jobId);

    QString headError;
    const QString latestAttemptId = logicalRecoveryHeadJobId(projectRecords, projectId,
                                                              cachedLogicalId, &headError);
    if (latestAttemptId.isEmpty())
        return qualified(QStringLiteral("conflict"), headError,
                         {{QStringLiteral("projectId"), projectId},
                          {QStringLiteral("logicalJobId"), cachedLogicalId}});
    if (latestAttemptId != jobId)
        return qualified(QStringLiteral("reconcile_required"),
                         QStringLiteral("A newer persisted attempt exists for this logical native job; qualify its latest interrupted attempt instead."),
                         {{QStringLiteral("projectId"), projectId},
                          {QStringLiteral("logicalJobId"), cachedLogicalId},
                          {QStringLiteral("latestAttemptJobId"), latestAttemptId}});

    QString logicalLockError;
    const QString logicalLockPath = managedLogicalRecoveryLockPath(
        this->nativeJobRegistryRoot, projectId, cachedLogicalId, false, &logicalLockError);
    if (logicalLockPath.isEmpty())
        return qualified(QStringLiteral("conflict"), logicalLockError,
                         {{QStringLiteral("projectId"), projectId},
                          {QStringLiteral("logicalJobId"), cachedLogicalId}});
    if (!logicalRecoveryLockHeldByCaller && QFileInfo(logicalLockPath).exists())
    {
        if (QFileInfo(logicalLockPath).isSymLink() || hasSymlinkComponent(logicalLockPath))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The logical native recovery lock is linked."),
                             {{QStringLiteral("projectId"), projectId},
                              {QStringLiteral("logicalJobId"), cachedLogicalId}});
        QLockFile logicalLock(logicalLockPath);
        logicalLock.setStaleLockTime(0);
        if (!logicalLock.tryLock(0))
            return qualified(QStringLiteral("reconcile_required"),
                             QStringLiteral("Another GUI process is staging or executing the latest attempt for this logical native job."),
                             {{QStringLiteral("projectId"), projectId},
                              {QStringLiteral("logicalJobId"), cachedLogicalId},
                              {QStringLiteral("latestAttemptJobId"), latestAttemptId}});
        logicalLock.unlock();
    }

    QString registryError;
    if (!this->validateNativeJobRecordOnDisk(jobId, &registryError))
        return qualified(QStringLiteral("conflict"), registryError,
                         {{QStringLiteral("projectId"), record.value(QStringLiteral("projectId"))},
                          {QStringLiteral("sourceRuntimeStamp"), record.value(QStringLiteral("sourceRuntimeStamp"))},
                          {QStringLiteral("sourceSnapshotRef"), record.value(QStringLiteral("sourceSnapshotRef"))}});

    QString terminalError;
    const QString state = record.value(QStringLiteral("state")).toString();
    const bool terminalState = state == QStringLiteral("succeeded") || state == QStringLiteral("failed") ||
            state == QStringLiteral("cancelled") ||
            (state == QStringLiteral("interrupted") &&
             record.value(QStringLiteral("resultManifestDescriptorReported")).toBool());
    if (terminalState && !this->revalidateNativeTerminalArtifactsOnOwner(jobId, &terminalError))
        return qualified(QStringLiteral("conflict"), terminalError,
                         {{QStringLiteral("projectId"), record.value(QStringLiteral("projectId"))},
                          {QStringLiteral("sourceRuntimeStamp"), record.value(QStringLiteral("sourceRuntimeStamp"))},
                          {QStringLiteral("sourceSnapshotRef"), record.value(QStringLiteral("sourceSnapshotRef"))}});

    if (state == QStringLiteral("interrupted"))
    {
        const bool locallyOwned = this->nativeTasks.contains(jobId) &&
                !this->nativeTasks.value(jobId).isNull() && !this->nativeTasks.value(jobId)->isFinished();
        QLockFile ownerLock(record.value(QStringLiteral("ownerLockRef")).toString());
        ownerLock.setStaleLockTime(0);
        const bool ownerAvailable = ownerLock.tryLock(0);
        if (ownerAvailable) ownerLock.unlock();
        if (locallyOwned || !ownerAvailable)
            return qualified(QStringLiteral("reconcile_required"),
                             QStringLiteral("The persisted interrupted-job owner lock is still held; recovery waits for that GUI process to exit."),
                             {{QStringLiteral("projectId"), record.value(QStringLiteral("projectId"))},
                              {QStringLiteral("sourceRuntimeStamp"), record.value(QStringLiteral("sourceRuntimeStamp"))},
                              {QStringLiteral("sourceSnapshotRef"), record.value(QStringLiteral("sourceSnapshotRef"))}});
    }

    if (state == QStringLiteral("queued") || state == QStringLiteral("running") || state == QStringLiteral("cancelling"))
    {
        const bool locallyOwned = this->nativeTasks.contains(jobId) && !this->nativeTasks.value(jobId).isNull();
        QLockFile ownerLock(record.value(QStringLiteral("ownerLockRef")).toString());
        ownerLock.setStaleLockTime(0);
        const bool ownerAvailable = ownerLock.tryLock(0);
        if (ownerAvailable) ownerLock.unlock();
        if (locallyOwned || !ownerAvailable)
            return qualified(QStringLiteral("reconcile_required"),
                             QStringLiteral("The queued or running job still has a live process owner; the job was left untouched."),
                             {{QStringLiteral("projectId"), record.value(QStringLiteral("projectId"))},
                              {QStringLiteral("state"), state},
                              {QStringLiteral("sourceRuntimeStamp"), record.value(QStringLiteral("sourceRuntimeStamp"))},
                              {QStringLiteral("sourceSnapshotRef"), record.value(QStringLiteral("sourceSnapshotRef"))}});
        record.insert(QStringLiteral("state"), QStringLiteral("interrupted"));
        record.insert(QStringLiteral("interruptedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        record.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        record.insert(QStringLiteral("interruptedReason"),
                      QStringLiteral("The process-owner lock is no longer held; the job will not be re-executed automatically."));
        QString persistError;
        if (!this->persistNativeJobRecord(jobId, &record, false, &persistError))
            return qualified(QStringLiteral("conflict"), persistError,
                             {{QStringLiteral("projectId"), record.value(QStringLiteral("projectId"))},
                              {QStringLiteral("sourceRuntimeStamp"), record.value(QStringLiteral("sourceRuntimeStamp"))},
                              {QStringLiteral("sourceSnapshotRef"), record.value(QStringLiteral("sourceSnapshotRef"))}});
        this->nativeJobs.insert(jobId, record);
    }

    record = this->nativeJobs.value(jobId);
    QJsonObject qualificationFacts{{QStringLiteral("projectId"), record.value(QStringLiteral("projectId"))},
                                   {QStringLiteral("persistentDocumentId"), record.value(QStringLiteral("persistentDocumentId"))},
                                   {QStringLiteral("sourceRuntimeStamp"), record.value(QStringLiteral("sourceRuntimeStamp"))},
                                   {QStringLiteral("sourceSnapshotRef"), record.value(QStringLiteral("sourceSnapshotRef"))},
                                   {QStringLiteral("nativeCheckpointEnabled"), record.value(QStringLiteral("nativeCheckpointEnabled"))}};
    if (record.contains(QStringLiteral("nativeCheckpointSeed")))
        qualificationFacts.insert(QStringLiteral("nativeCheckpointSeed"), record.value(QStringLiteral("nativeCheckpointSeed")));
    if (record.contains(QStringLiteral("recovery")))
        qualificationFacts.insert(QStringLiteral("recovery"), record.value(QStringLiteral("recovery")));
    bool inspectPartialResult = false;
    if (record.value(QStringLiteral("state")).toString() == QStringLiteral("interrupted"))
    {
        const QString resultPath = record.value(QStringLiteral("resultArtifactRef")).toString();
        const QString manifestPath = record.value(QStringLiteral("resultManifestRef")).toString();
        const QString defaultManifestPath = QDir(QFileInfo(resultPath).absolutePath())
                .filePath(QStringLiteral("native-result.manifest.json"));
        const bool manifestPresent = (!manifestPath.isEmpty() && QFileInfo(manifestPath).exists()) ||
                (!resultPath.isEmpty() && QFileInfo(defaultManifestPath).exists());
        const QFileInfo resultInfo(resultPath);
        bool unverifiedResultArtifact = !resultPath.isEmpty() && resultInfo.exists();
        if (!resultPath.isEmpty() && resultInfo.isSymLink())
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The interrupted job's existing result artifact is linked or non-canonical."),
                             qualificationFacts);
        if (unverifiedResultArtifact &&
            (!resultInfo.isFile() || hasSymlinkComponent(resultPath) ||
             resultInfo.canonicalFilePath() != resultInfo.absoluteFilePath()))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The interrupted job's existing result artifact is linked or non-canonical."),
                             qualificationFacts);
        if (unverifiedResultArtifact && record.value(QStringLiteral("nativeCheckpointEnabled")).toBool() &&
            !record.value(QStringLiteral("nativeCheckpointInputModelSha256")).toString().isEmpty() &&
            QString::fromLatin1(sha256File(resultPath).toHex()) ==
                record.value(QStringLiteral("nativeCheckpointInputModelSha256")).toString())
        {
            // SolverTask publishes an initial copy of the frozen input before
            // starting the process. It is not a result when its bytes still
            // equal the pinned checkpoint input and no terminal evidence exists.
            unverifiedResultArtifact = false;
        }
        const bool terminalEvidence = record.value(QStringLiteral("resultArtifactVerified")).toBool() ||
                record.value(QStringLiteral("resultManifestDescriptorReported")).toBool() || manifestPresent;
        if (terminalEvidence)
        {
            QString artifactError;
            if ((record.value(QStringLiteral("resultArtifactVerified")).toBool() ||
                 record.value(QStringLiteral("resultManifestDescriptorReported")).toBool()) &&
                !this->revalidateNativeTerminalArtifactsOnOwner(jobId, &artifactError))
                return qualified(QStringLiteral("conflict"), artifactError, qualificationFacts);
            if (manifestPresent && !record.value(QStringLiteral("resultArtifactVerified")).toBool() &&
                !record.value(QStringLiteral("resultManifestDescriptorReported")).toBool())
                return qualified(QStringLiteral("conflict"),
                                 QStringLiteral("An unregistered result manifest appeared before recovery qualification."),
                                 qualificationFacts);
            return qualified(QStringLiteral("reconcile_required"),
                             QStringLiteral("A verified result artifact or manifest exists; reconcile it before considering any new attempt."),
                             qualificationFacts);
        }
        inspectPartialResult = unverifiedResultArtifact;
    }
    if (record.value(QStringLiteral("state")).toString() != QStringLiteral("interrupted"))
    {
        QJsonObject extra = qualificationFacts;
        extra.insert(QStringLiteral("state"), record.value(QStringLiteral("state")));
        return qualified(QStringLiteral("recompute_required"),
                         QStringLiteral("Recovery qualification applies only to interrupted native jobs; completed or terminal jobs are preserved as facts."),
                         extra);
    }

    if (!record.value(QStringLiteral("nativeCheckpointEnabled")).toBool())
        return qualified(QStringLiteral("recompute_required"),
                         record.value(QStringLiteral("nativeCheckpointUnsupportedReason")).toString().isEmpty()
                            ? QStringLiteral("This native job has no enabled periodic accepted-state checkpoint channel.")
                            : record.value(QStringLiteral("nativeCheckpointUnsupportedReason")).toString(),
                         qualificationFacts);

    const QJsonObject context = record.value(QStringLiteral("nativeCheckpointContext")).toObject();
    const QStringList contextKeys{QStringLiteral("schema"), QStringLiteral("sourceSha256"),
                                  QStringLiteral("modelSha256"), QStringLiteral("controlSha256"),
                                  QStringLiteral("runtimeSha256"), QStringLiteral("packageSha256"),
                                  QStringLiteral("policy"), QStringLiteral("maxAcceptedSteps"),
                                  QStringLiteral("maxSimulationSeconds"), QStringLiteral("dtS")};
    if (!exactKeys(context, contextKeys) ||
        context.value(QStringLiteral("schema")).toString() != QString::fromLatin1(nativeCheckpointContextSchema) ||
        !isLowerSha256(context.value(QStringLiteral("sourceSha256")).toString()) ||
        !isLowerSha256(context.value(QStringLiteral("modelSha256")).toString()) ||
        !isLowerSha256(context.value(QStringLiteral("controlSha256")).toString()) ||
        !isLowerSha256(context.value(QStringLiteral("runtimeSha256")).toString()) ||
        !isLowerSha256(context.value(QStringLiteral("packageSha256")).toString()) ||
        context.value(QStringLiteral("policy")).toString() != QString::fromLatin1(nativeCheckpointPolicy) ||
        context.value(QStringLiteral("maxAcceptedSteps")).toInt(-1) != 10 ||
        context.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) != 1.0 ||
        !context.value(QStringLiteral("dtS")).isDouble() ||
        !std::isfinite(context.value(QStringLiteral("dtS")).toDouble()) ||
        context.value(QStringLiteral("dtS")).toDouble() <= 0.0)
        return qualified(QStringLiteral("conflict"), QStringLiteral("The persisted native checkpoint context is malformed or outside the accepted schema."),
                         qualificationFacts);

    const QString resultArtifactPath = record.value(QStringLiteral("resultArtifactRef")).toString();
    const QString runRoot = getRunRoot(resultArtifactPath);
    const QString expectedInputDirectory = QDir(runRoot).filePath(QStringLiteral("input"));
    const QString expectedInputPath = QDir(expectedInputDirectory).filePath(QFileInfo(resultArtifactPath).fileName());
    const QString inputPath = record.value(QStringLiteral("inputSnapshotRef")).toString();
    const QString packagePath = record.value(QStringLiteral("inputPackageRef")).toString();
    const QString checkpointInputPath = record.value(QStringLiteral("nativeCheckpointInputModelRef")).toString();
    const QString checkpointInputName = QStringLiteral("native-checkpoint-input.") + QFileInfo(checkpointInputPath).suffix();
    const QString expectedCheckpointInputPath = QDir(expectedInputDirectory).filePath(checkpointInputName);
    const QString inputSha = QString::fromLatin1(sha256File(inputPath).toHex());
    const QString packageSha = QString::fromLatin1(sha256Directory(packagePath).toHex());
    const QString expectedInputSha = record.value(QStringLiteral("inputSnapshotSha256")).toString();
    const QString expectedPackageSha = record.value(QStringLiteral("inputPackageSha256")).toString();
    const QString checkpointInputSha = QString::fromLatin1(sha256File(checkpointInputPath).toHex());
    const QString expectedCheckpointInputSha =
            record.value(QStringLiteral("nativeCheckpointInputModelSha256")).toString();
    const QString contextModelSha = context.value(QStringLiteral("modelSha256")).toString();
    const QString contextPackageSha = context.value(QStringLiteral("packageSha256")).toString();
    QStringList inputBindingFailures;
    if (resultArtifactPath.isEmpty() || !QFileInfo(resultArtifactPath).isAbsolute() || runRoot.isEmpty())
        inputBindingFailures.append(QStringLiteral("result_artifact_path"));
    if (!samePathText(packagePath, expectedInputDirectory))
        inputBindingFailures.append(QStringLiteral("input_package_path"));
    if (!samePathText(inputPath, expectedInputPath))
        inputBindingFailures.append(QStringLiteral("input_snapshot_path"));
    if (!samePathText(checkpointInputPath, expectedCheckpointInputPath))
        inputBindingFailures.append(QStringLiteral("checkpoint_input_path"));
    if (QFileInfo(checkpointInputPath).suffix().compare(RModel::getDefaultFileExtension(false), Qt::CaseInsensitive) != 0 &&
        QFileInfo(checkpointInputPath).suffix().compare(RModel::getDefaultFileExtension(true), Qt::CaseInsensitive) != 0)
        inputBindingFailures.append(QStringLiteral("checkpoint_input_suffix"));
    if (inputPath.isEmpty() || hasSymlinkComponent(inputPath))
        inputBindingFailures.append(QStringLiteral("input_snapshot_link_or_missing_path"));
    if (packagePath.isEmpty() || hasSymlinkComponent(packagePath))
        inputBindingFailures.append(QStringLiteral("input_package_link_or_missing_path"));
    if (inputSha != expectedInputSha)
        inputBindingFailures.append(QStringLiteral("input_snapshot_sha256"));
    if (packageSha != expectedPackageSha)
        inputBindingFailures.append(QStringLiteral("input_package_sha256"));
    if (checkpointInputPath.isEmpty() || hasSymlinkComponent(checkpointInputPath) ||
        !samePathText(QFileInfo(checkpointInputPath).absolutePath(), packagePath))
        inputBindingFailures.append(QStringLiteral("checkpoint_input_directory_shape"));
    if (checkpointInputSha != expectedCheckpointInputSha)
        inputBindingFailures.append(QStringLiteral("checkpoint_input_model_sha256"));
    if (expectedCheckpointInputSha != contextModelSha)
        inputBindingFailures.append(QStringLiteral("context_model_sha256"));
    if (packageSha != contextPackageSha)
        inputBindingFailures.append(QStringLiteral("context_package_sha256"));
    if (!inputBindingFailures.isEmpty())
    {
        const QJsonObject details{{QStringLiteral("failedPredicates"), inputBindingFailures.join(QStringLiteral(","))},
                                  {QStringLiteral("resultArtifactPath"), resultArtifactPath},
                                  {QStringLiteral("expectedInputDirectory"), expectedInputDirectory},
                                  {QStringLiteral("inputSnapshotPath"), inputPath},
                                  {QStringLiteral("expectedInputSnapshotPath"), expectedInputPath},
                                  {QStringLiteral("inputPackagePath"), packagePath},
                                  {QStringLiteral("checkpointInputPath"), checkpointInputPath},
                                  {QStringLiteral("expectedCheckpointInputPath"), expectedCheckpointInputPath},
                                  {QStringLiteral("inputSnapshotSha256"), inputSha},
                                  {QStringLiteral("expectedInputSnapshotSha256"), expectedInputSha},
                                  {QStringLiteral("inputPackageSha256"), packageSha},
                                  {QStringLiteral("expectedInputPackageSha256"), expectedPackageSha},
                                  {QStringLiteral("checkpointInputSha256"), checkpointInputSha},
                                  {QStringLiteral("expectedCheckpointInputSha256"), expectedCheckpointInputSha},
                                  {QStringLiteral("contextModelSha256"), contextModelSha},
                                  {QStringLiteral("contextPackageSha256"), contextPackageSha}};
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The frozen native input binding is invalid: %1")
                             .arg(QString::fromUtf8(compactJson(details))),
                         qualificationFacts);
    }

    QString expectedSourceSha = inputSha;
    if (record.value(QStringLiteral("frozenSnapshotVerified")).toBool())
    {
        const QJsonObject snapshotRef = record.value(QStringLiteral("projectSnapshotRef")).toObject();
        const QString projectId = record.value(QStringLiteral("projectId")).toString();
        const QString persistentDocumentId = record.value(QStringLiteral("persistentDocumentId")).toString();
        const QString sourceRevision = snapshotRef.value(QStringLiteral("sourceRevision")).toString();
        const QString snapshotSha = snapshotRef.value(QStringLiteral("modelSha256")).toString();
        const QString relativePath = snapshotRef.value(QStringLiteral("relativePath")).toString();
        const QFileInfo workspaceInfo(Application::instance()->getSession()->projectWorkspace());
        const QString workspace = workspaceInfo.canonicalFilePath();
        const QJsonObject activeProject = Application::instance()->getSession()->projectContext();
        const QString expectedRelative = QStringLiteral("models/%1/snapshots/%2/model.rbm")
                .arg(persistentDocumentId, snapshotSha);
        const QString expectedPath = workspace.isEmpty() ? QString()
                : QDir(workspace).filePath(expectedRelative);
        const QString frozenPath = record.value(QStringLiteral("frozenSnapshotPath")).toString();
        if (!isCanonicalProjectId(projectId) || !isCanonicalProjectId(persistentDocumentId) ||
            snapshotRef.value(QStringLiteral("persistentDocumentId")).toString() != persistentDocumentId ||
            activeProject.value(QStringLiteral("availability")).toString() != QStringLiteral("available") ||
            activeProject.value(QStringLiteral("projectId")).toString() != projectId ||
            snapshotRef.value(QStringLiteral("projectId")).toString() != projectId ||
            record.value(QStringLiteral("projectId")).toString() != projectId ||
            !QRegularExpression(QStringLiteral("^r(0|[1-9][0-9]*)$")).match(sourceRevision).hasMatch() ||
            !isLowerSha256(snapshotSha) || relativePath != expectedRelative || expectedPath.isEmpty() ||
            !samePathText(frozenPath, expectedPath) || hasSymlinkComponent(frozenPath) ||
            QString::fromLatin1(sha256File(frozenPath).toHex()) != snapshotSha ||
            record.value(QStringLiteral("frozenSnapshotSha256")).toString() != snapshotSha)
            return qualified(QStringLiteral("conflict"), QStringLiteral("The linked frozen project source no longer matches its project, document, path, and SHA-256 binding."),
                             qualificationFacts);
        expectedSourceSha = snapshotSha;
    }
    else
    {
        const QJsonObject sourceRef = record.value(QStringLiteral("sourceSnapshotRef")).toObject();
        if (record.value(QStringLiteral("projectId")).toString() != QStringLiteral("unlinked") ||
            record.value(QStringLiteral("inputDerivedFrom")).toString() != QStringLiteral("live_unlinked") ||
            sourceRef.value(QStringLiteral("kind")).toString() != QStringLiteral("gui-native-input-snapshot/v1") ||
            sourceRef.value(QStringLiteral("inputDerivedFrom")).toString() != QStringLiteral("live_unlinked") ||
            sourceRef.value(QStringLiteral("inputSnapshotSha256")).toString() != inputSha ||
            sourceRef.value(QStringLiteral("inputPackageSha256")).toString() != packageSha ||
            sourceRef.value(QStringLiteral("sourceRuntimeStamp")).toObject() !=
                record.value(QStringLiteral("sourceRuntimeStamp")).toObject())
            return qualified(QStringLiteral("conflict"), QStringLiteral("The unlinked source binding no longer matches its frozen input snapshot and runtime identity."),
                             qualificationFacts);
    }

    const QString solverPath = Application::instance()->getApplicationSettings()->getSolverPath();
    const QString currentRuntimeSha = QString::fromLatin1(sha256File(solverPath).toHex());
    if (currentRuntimeSha != context.value(QStringLiteral("runtimeSha256")).toString() ||
        context.value(QStringLiteral("controlSha256")).toString() !=
            QString::fromLatin1(nativeNoControlSha256().toHex()) ||
        expectedSourceSha != context.value(QStringLiteral("sourceSha256")).toString())
        return qualified(QStringLiteral("conflict"), QStringLiteral("The current solver runtime, control mode, or source binding differs from the checkpoint context."),
                         qualificationFacts);

    const QString expectedCheckpointDirectory = QDir(runRoot).filePath(QStringLiteral("checkpoints"));
    const QString checkpointDirectory = record.value(QStringLiteral("nativeCheckpointDirectory")).toString();
    if (runRoot.isEmpty() || !QFileInfo(runRoot).isDir() || hasSymlinkComponent(runRoot) ||
        (!QFileInfo(runRoot).canonicalFilePath().isEmpty() &&
         !samePathText(runRoot, QFileInfo(runRoot).canonicalFilePath())) ||
        !samePathText(checkpointDirectory, expectedCheckpointDirectory))
        return qualified(QStringLiteral("conflict"), QStringLiteral("The checkpoint directory is missing, linked, or outside the original job run directory."),
                         qualificationFacts);
    if (!QFileInfo(checkpointDirectory).exists())
        return qualified(QStringLiteral("recompute_required"),
                         QStringLiteral("The solver stopped before creating its native checkpoint directory."),
                         qualificationFacts);
    if (!QFileInfo(checkpointDirectory).isDir() || hasSymlinkComponent(checkpointDirectory) ||
        !samePathText(checkpointDirectory, QFileInfo(checkpointDirectory).canonicalFilePath()))
        return qualified(QStringLiteral("conflict"), QStringLiteral("The checkpoint directory is linked or non-canonical."),
                         qualificationFacts);

    const QJsonObject recoveryFacts = record.value(QStringLiteral("recovery")).toObject();
    const QJsonObject checkpointSeed = record.value(QStringLiteral("nativeCheckpointSeed")).toObject();
    const QString expectedSeedPointerPath = QDir(checkpointDirectory).filePath(QStringLiteral("latest.json"));
    if (!recoveryFacts.isEmpty())
    {
        const QStringList seedKeys{QStringLiteral("schema"), QStringLiteral("sourceJobId"),
                                   QStringLiteral("sourceEnvelopeRef"), QStringLiteral("sourceEnvelopeSha256"),
                                   QStringLiteral("sourceCheckpointModelRef"),
                                   QStringLiteral("sourceCheckpointModelSha256"),
                                   QStringLiteral("seedModelRef"), QStringLiteral("seedModelSha256"),
                                   QStringLiteral("seedEnvelopeRef"), QStringLiteral("seedEnvelopeSha256"),
                                   QStringLiteral("seedPointerRef"), QStringLiteral("inputModelSha256"),
                                   QStringLiteral("packageSha256"), QStringLiteral("acceptedSeq"),
                                   QStringLiteral("timeS"), QStringLiteral("dtS")};
        const QString seedModelPath = checkpointSeed.value(QStringLiteral("seedModelRef")).toString();
        const QString seedEnvelopePath = checkpointSeed.value(QStringLiteral("seedEnvelopeRef")).toString();
        const QString seedModelSha = checkpointSeed.value(QStringLiteral("seedModelSha256")).toString();
        const QString seedEnvelopeSha = checkpointSeed.value(QStringLiteral("seedEnvelopeSha256")).toString();
        const double seedAcceptedSeq = checkpointSeed.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
        const double seedTime = checkpointSeed.value(QStringLiteral("timeS")).toDouble(-1.0);
        const double seedDt = checkpointSeed.value(QStringLiteral("dtS")).toDouble(-1.0);
        if (!exactKeys(checkpointSeed, seedKeys) ||
            checkpointSeed.value(QStringLiteral("schema")).toString() != QStringLiteral("range.native-checkpoint-seed/v1") ||
            checkpointSeed.value(QStringLiteral("sourceJobId")).toString() !=
                recoveryFacts.value(QStringLiteral("recoverySourceJobId")).toString() ||
            checkpointSeed.value(QStringLiteral("sourceEnvelopeRef")).toString() !=
                recoveryFacts.value(QStringLiteral("selectedEnvelopePath")).toString() ||
            checkpointSeed.value(QStringLiteral("sourceEnvelopeSha256")).toString() !=
                recoveryFacts.value(QStringLiteral("selectedEnvelopeSha256")).toString() ||
            !QFileInfo(checkpointSeed.value(QStringLiteral("sourceEnvelopeRef")).toString()).isAbsolute() ||
            checkpointSeed.value(QStringLiteral("sourceCheckpointModelSha256")).toString() !=
                recoveryFacts.value(QStringLiteral("selectedCheckpointSha256")).toString() ||
            checkpointSeed.value(QStringLiteral("sourceCheckpointModelRef")).toString() !=
                recoveryFacts.value(QStringLiteral("selectedCheckpointPath")).toString() ||
            checkpointSeed.value(QStringLiteral("inputModelSha256")).toString() !=
                context.value(QStringLiteral("modelSha256")).toString() ||
            checkpointSeed.value(QStringLiteral("packageSha256")).toString() !=
                context.value(QStringLiteral("packageSha256")).toString() ||
            checkpointSeed.value(QStringLiteral("seedModelSha256")).toString() !=
                checkpointSeed.value(QStringLiteral("sourceCheckpointModelSha256")).toString() ||
            !isLowerSha256(checkpointSeed.value(QStringLiteral("sourceEnvelopeSha256")).toString()) ||
            !isLowerSha256(checkpointSeed.value(QStringLiteral("sourceCheckpointModelSha256")).toString()) ||
            !isLowerSha256(seedModelSha) || !isLowerSha256(seedEnvelopeSha) ||
            !std::isfinite(seedAcceptedSeq) || seedAcceptedSeq < 1.0 ||
            seedAcceptedSeq > double(std::numeric_limits<uint>::max()) || std::floor(seedAcceptedSeq) != seedAcceptedSeq ||
            !std::isfinite(seedTime) || seedTime < 0.0 || !std::isfinite(seedDt) || seedDt <= 0.0 ||
            seedAcceptedSeq != recoveryFacts.value(QStringLiteral("acceptedSeq")).toDouble(-2.0) ||
            seedTime != recoveryFacts.value(QStringLiteral("checkpointTimeS")).toDouble(-2.0) ||
            seedDt != context.value(QStringLiteral("dtS")).toDouble(-2.0) ||
            !QFileInfo(seedModelPath).isAbsolute() || !QFileInfo(seedEnvelopePath).isAbsolute() ||
            !samePathText(QFileInfo(seedModelPath).absolutePath(), checkpointDirectory) ||
            !samePathText(QFileInfo(seedEnvelopePath).absolutePath(), checkpointDirectory) ||
            !samePathText(checkpointSeed.value(QStringLiteral("seedPointerRef")).toString(), expectedSeedPointerPath) ||
            QFileInfo(seedModelPath).isSymLink() || QFileInfo(seedEnvelopePath).isSymLink() ||
            hasSymlinkComponent(seedModelPath) || hasSymlinkComponent(seedEnvelopePath) ||
            QFileInfo(seedModelPath).size() <= 0 || QFileInfo(seedModelPath).size() > 512LL * 1024 * 1024 ||
            QString::fromLatin1(sha256File(seedModelPath).toHex()) != seedModelSha)
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The persisted recovery checkpoint seed proof or immutable MODEL no longer matches its source and new context."),
                             qualificationFacts);

        QJsonObject seedEnvelope;
        QString seedEnvelopeError;
        if (!readSmallObject(seedEnvelopePath, 64 * 1024, &seedEnvelope, &seedEnvelopeError))
            return qualified(QStringLiteral("conflict"), seedEnvelopeError, qualificationFacts);
        QFile seedEnvelopeFile(seedEnvelopePath);
        if (!seedEnvelopeFile.open(QIODevice::ReadOnly))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The immutable recovery seed envelope could not be read."),
                             qualificationFacts);
        const QByteArray seedEnvelopeBytes = seedEnvelopeFile.readAll();
        const QStringList seedEnvelopeKeys{QStringLiteral("schema"), QStringLiteral("sourceSha256"),
            QStringLiteral("inputModelSha256"), QStringLiteral("controlSha256"),
            QStringLiteral("runtimeSha256"), QStringLiteral("packageSha256"),
            QStringLiteral("policy"), QStringLiteral("maxAcceptedSteps"),
            QStringLiteral("maxSimulationSeconds"), QStringLiteral("acceptedSeq"),
            QStringLiteral("timeS"), QStringLiteral("dtS"), QStringLiteral("modelFile"),
            QStringLiteral("checkpointModelSha256")};
        if (seedEnvelopeFile.error() != QFileDevice::NoError ||
            !exactKeys(seedEnvelope, seedEnvelopeKeys) ||
            compactJson(seedEnvelope) != seedEnvelopeBytes || sha256Hex(seedEnvelopeBytes) != seedEnvelopeSha ||
            seedEnvelope.value(QStringLiteral("schema")).toString() != QString::fromLatin1(nativeCheckpointEnvelopeSchema) ||
            seedEnvelope.value(QStringLiteral("sourceSha256")).toString() != context.value(QStringLiteral("sourceSha256")).toString() ||
            seedEnvelope.value(QStringLiteral("inputModelSha256")).toString() != context.value(QStringLiteral("modelSha256")).toString() ||
            seedEnvelope.value(QStringLiteral("controlSha256")).toString() != context.value(QStringLiteral("controlSha256")).toString() ||
            seedEnvelope.value(QStringLiteral("runtimeSha256")).toString() != context.value(QStringLiteral("runtimeSha256")).toString() ||
            seedEnvelope.value(QStringLiteral("packageSha256")).toString() != context.value(QStringLiteral("packageSha256")).toString() ||
            seedEnvelope.value(QStringLiteral("policy")).toString() != context.value(QStringLiteral("policy")).toString() ||
            seedEnvelope.value(QStringLiteral("maxAcceptedSteps")).toDouble(-1.0) !=
                context.value(QStringLiteral("maxAcceptedSteps")).toDouble() ||
            seedEnvelope.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) !=
                context.value(QStringLiteral("maxSimulationSeconds")).toDouble() ||
            seedEnvelope.value(QStringLiteral("acceptedSeq")).toDouble(-1.0) !=
                checkpointSeed.value(QStringLiteral("acceptedSeq")).toDouble(-2.0) ||
            seedEnvelope.value(QStringLiteral("timeS")).toDouble(-1.0) !=
                checkpointSeed.value(QStringLiteral("timeS")).toDouble(-2.0) ||
            seedEnvelope.value(QStringLiteral("dtS")).toDouble(-1.0) != context.value(QStringLiteral("dtS")).toDouble() ||
            seedEnvelope.value(QStringLiteral("modelFile")).toString() != QFileInfo(seedModelPath).fileName() ||
            seedEnvelope.value(QStringLiteral("checkpointModelSha256")).toString() != seedModelSha)
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The recovery seed envelope does not match its new checkpoint context or source state proof."),
                             qualificationFacts);

        RModel seedModel;
        RModel recoveryInputModel;
        try
        {
            seedModel.read(seedModelPath);
            recoveryInputModel.read(checkpointInputPath);
        }
        catch (const RError &readError)
        {
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The immutable recovery seed or queued input MODEL could not be decoded: %1")
                                 .arg(readError.getMessage()), qualificationFacts);
        }
        const RStructuralDynamicsRestartData &seedRestart =
                seedModel.getProblemSetup().getStructuralDynamicsRestartData();
        const RStructuralDynamicsRestartData &inputRestart =
                recoveryInputModel.getProblemSetup().getStructuralDynamicsRestartData();
        try
        {
            seedRestart.validate();
            inputRestart.validate();
        }
        catch (const RError &validationError)
        {
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The recovery seed or queued input restart state is invalid: %1")
                                 .arg(validationError.getMessage()), qualificationFacts);
        }
        if (!sameRestartState(seedRestart, inputRestart) || !seedRestart.available ||
            seedRestart.completedSteps != quint64(seedAcceptedSeq) ||
            std::abs(seedRestart.time - seedTime) > 1.0e-12 * qMax(1.0, std::abs(seedTime)) ||
            std::abs(seedRestart.fixedTimeStep - seedDt) > 1.0e-12 * qMax(1.0, std::abs(seedDt)))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The immutable recovery seed no longer matches the exact queued accepted-state input."),
                             qualificationFacts);
    }
    else if (record.value(QStringLiteral("nativeCheckpointSeed")).isObject())
    {
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The native job has a checkpoint seed without recovery ancestry."),
                         qualificationFacts);
    }

    const QString writerLockPath = QDir(checkpointDirectory).filePath(QStringLiteral(".native-checkpoint.lock"));
    if (QFileInfo(writerLockPath).exists())
    {
        if (QFileInfo(writerLockPath).isSymLink() || hasSymlinkComponent(writerLockPath))
            return qualified(QStringLiteral("conflict"), QStringLiteral("The native checkpoint writer lock is linked."), qualificationFacts);
        QLockFile writerLock(writerLockPath);
        writerLock.setStaleLockTime(0);
        const bool lockAvailable = writerLock.tryLock(0);
        if (lockAvailable) writerLock.unlock();
        else return qualified(QStringLiteral("reconcile_required"),
                              QStringLiteral("A native checkpoint writer still holds the checkpoint directory lock."),
                              qualificationFacts);
    }

    const auto readPointer = [&](const QString &pointerName, QJsonObject *envelopeOut,
                                 QString *envelopePathOut, QString *envelopeHashOut,
                                 QString *checkpointPathOut, QString *checkpointHashOut,
                                 QString *readError) -> bool
    {
        const QString pointerPath = QDir(checkpointDirectory).filePath(pointerName);
        QJsonObject pointer;
        if (!readSmallObject(pointerPath, 64 * 1024, &pointer, readError)) return false;
        QFile pointerFile(pointerPath);
        if (!pointerFile.open(QIODevice::ReadOnly)) return false;
        const QByteArray pointerBytes = pointerFile.readAll();
        if (pointerFile.error() != QFileDevice::NoError || compactJson(pointer) != pointerBytes ||
            !exactKeys(pointer, {QStringLiteral("schema"), QStringLiteral("envelopeFile"), QStringLiteral("envelopeSha256")}) ||
            pointer.value(QStringLiteral("schema")).toString() != QString::fromLatin1(nativeCheckpointPointerSchema))
        {
            if (readError) *readError = QStringLiteral("The native checkpoint pointer schema or canonical encoding is invalid.");
            return false;
        }
        const QString envelopeName = pointer.value(QStringLiteral("envelopeFile")).toString();
        const QString envelopeSha = pointer.value(QStringLiteral("envelopeSha256")).toString();
        if (envelopeName.isEmpty() || QFileInfo(envelopeName).fileName() != envelopeName ||
            envelopeName.contains(QLatin1Char('/')) || envelopeName.contains(QLatin1Char('\\')) ||
            !isLowerSha256(envelopeSha))
        {
            if (readError) *readError = QStringLiteral("The native checkpoint pointer has an invalid envelope reference.");
            return false;
        }
        const QString envelopePath = QDir(checkpointDirectory).filePath(envelopeName);
        QJsonObject envelope;
        if (!readSmallObject(envelopePath, 64 * 1024, &envelope, readError)) return false;
        QFile envelopeFile(envelopePath);
        if (!envelopeFile.open(QIODevice::ReadOnly)) return false;
        const QByteArray envelopeBytes = envelopeFile.readAll();
        if (envelopeFile.error() != QFileDevice::NoError || compactJson(envelope) != envelopeBytes ||
            sha256Hex(envelopeBytes) != envelopeSha)
        {
            if (readError) *readError = QStringLiteral("The native checkpoint envelope is non-canonical or does not match its pointer hash.");
            return false;
        }
        const QStringList envelopeKeys{QStringLiteral("schema"), QStringLiteral("sourceSha256"),
                                       QStringLiteral("inputModelSha256"), QStringLiteral("controlSha256"),
                                       QStringLiteral("runtimeSha256"), QStringLiteral("packageSha256"),
                                       QStringLiteral("policy"), QStringLiteral("maxAcceptedSteps"),
                                       QStringLiteral("maxSimulationSeconds"), QStringLiteral("acceptedSeq"),
                                       QStringLiteral("timeS"), QStringLiteral("dtS"),
                                       QStringLiteral("modelFile"), QStringLiteral("checkpointModelSha256")};
        if (!exactKeys(envelope, envelopeKeys) ||
            envelope.value(QStringLiteral("schema")).toString() != QString::fromLatin1(nativeCheckpointEnvelopeSchema) ||
            envelope.value(QStringLiteral("sourceSha256")).toString() != context.value(QStringLiteral("sourceSha256")).toString() ||
            envelope.value(QStringLiteral("inputModelSha256")).toString() != context.value(QStringLiteral("modelSha256")).toString() ||
            envelope.value(QStringLiteral("controlSha256")).toString() != context.value(QStringLiteral("controlSha256")).toString() ||
            envelope.value(QStringLiteral("runtimeSha256")).toString() != context.value(QStringLiteral("runtimeSha256")).toString() ||
            envelope.value(QStringLiteral("packageSha256")).toString() != context.value(QStringLiteral("packageSha256")).toString() ||
            envelope.value(QStringLiteral("policy")).toString() != context.value(QStringLiteral("policy")).toString() ||
            envelope.value(QStringLiteral("maxAcceptedSteps")).toDouble(-1.0) != 10.0 ||
            envelope.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) != 1.0 ||
            envelope.value(QStringLiteral("dtS")).toDouble(-1.0) != context.value(QStringLiteral("dtS")).toDouble())
        {
            if (readError) *readError = QStringLiteral("The native checkpoint envelope does not match its accepted context.");
            return false;
        }
        const QJsonValue seqValue = envelope.value(QStringLiteral("acceptedSeq"));
        const QJsonValue timeValue = envelope.value(QStringLiteral("timeS"));
        const QString modelName = envelope.value(QStringLiteral("modelFile")).toString();
        const QString modelHash = envelope.value(QStringLiteral("checkpointModelSha256")).toString();
        if (!seqValue.isDouble() || seqValue.toDouble() < 1.0 || std::floor(seqValue.toDouble()) != seqValue.toDouble() ||
            !timeValue.isDouble() || !std::isfinite(timeValue.toDouble()) ||
            !modelName.endsWith(QStringLiteral(".rbm")) || QFileInfo(modelName).fileName() != modelName ||
            modelName.contains(QLatin1Char('/')) || modelName.contains(QLatin1Char('\\')) || !isLowerSha256(modelHash))
        {
            if (readError) *readError = QStringLiteral("The native checkpoint envelope has invalid accepted-state fields.");
            return false;
        }
        const QString modelPath = QDir(checkpointDirectory).filePath(modelName);
        if (hasSymlinkComponent(modelPath) ||
            QString::fromLatin1(sha256File(modelPath).toHex()) != modelHash)
        {
            if (readError) *readError = QStringLiteral("The checkpoint MODEL is missing or its SHA-256 does not match the envelope.");
            return false;
        }
        try
        {
            RModel inputModel;
            inputModel.read(checkpointInputPath);
            const RProblemSetup &inputSetup = inputModel.getProblemSetup();
            const RStructuralDynamicsSetup &inputDynamics = inputSetup.getStructuralDynamicsSetup();
            if (inputModel.getProblemTaskTree().getProblemTypeMask() != R_PROBLEM_STRESS ||
                !inputDynamics.getEnabled() ||
                inputDynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
                inputSetup.getStructuralControlSetup().enabled || inputSetup.getStructuralCableSetup().enabled ||
                inputSetup.getStructuralPiezoSetup().enabled || inputSetup.getStructuralThermalControlSetup().enabled ||
                std::abs(inputDynamics.getTimeStep() - context.value(QStringLiteral("dtS")).toDouble()) >
                    1.0e-12 * qMax(1.0, std::abs(context.value(QStringLiteral("dtS")).toDouble())))
            {
                if (readError) *readError = QStringLiteral("The frozen input no longer describes an uncontrolled Newmark structural-dynamics run.");
                return false;
            }
            const RStructuralDynamicsRestartData &initialRestart =
                    inputSetup.getStructuralDynamicsRestartData();
            if (initialRestart.available) initialRestart.validate();
            RModel checkpointModel;
            checkpointModel.read(modelPath);
            const RStructuralDynamicsRestartData &restart =
                    checkpointModel.getProblemSetup().getStructuralDynamicsRestartData();
            const RTimeSolver &checkpointTimeSolver = checkpointModel.getTimeSolver();
            restart.validate();
            const quint64 baselineSequence = initialRestart.available ? initialRestart.completedSteps : 0;
            const double baselineTime = initialRestart.available ? initialRestart.time : 0.0;
            const double expectedTime = baselineTime +
                    (seqValue.toDouble() - double(baselineSequence)) * context.value(QStringLiteral("dtS")).toDouble();
            if (!restart.available || restart.completedSteps != quint64(seqValue.toDouble()) ||
                seqValue.toDouble() < double(baselineSequence) ||
                checkpointModel.getNNodes() != inputModel.getNNodes() ||
                checkpointModel.getNElements() != inputModel.getNElements() ||
                std::abs(restart.time - timeValue.toDouble()) >
                    1.0e-12 * qMax(1.0, std::abs(timeValue.toDouble())) ||
                std::abs(restart.time - expectedTime) >
                    1.0e-12 * qMax(1.0, std::abs(expectedTime)) ||
                std::abs(restart.fixedTimeStep - context.value(QStringLiteral("dtS")).toDouble()) >
                    1.0e-12 * qMax(1.0, std::abs(context.value(QStringLiteral("dtS")).toDouble())) ||
                restart.completedSteps != quint64(checkpointTimeSolver.getCurrentTimeStep()) + 1 ||
                std::abs(checkpointTimeSolver.getCurrentTime() - restart.time) >
                    1.0e-12 * qMax(1.0, std::abs(restart.time)))
            {
                if (readError) *readError = QStringLiteral("The checkpoint MODEL accepted state disagrees with its envelope sequence, time, or time step.");
                return false;
            }
        }
        catch (const RError &exception)
        {
            if (readError) *readError = QStringLiteral("The checkpoint MODEL could not be decoded: %1")
                    .arg(exception.getMessage());
            return false;
        }
        catch (const std::exception &exception)
        {
            if (readError) *readError = QStringLiteral("The checkpoint MODEL could not be decoded: %1")
                    .arg(QString::fromLocal8Bit(exception.what()));
            return false;
        }
        if (envelopeOut) *envelopeOut = envelope;
        if (envelopePathOut) *envelopePathOut = envelopePath;
        if (envelopeHashOut) *envelopeHashOut = envelopeSha;
        if (checkpointPathOut) *checkpointPathOut = modelPath;
        if (checkpointHashOut) *checkpointHashOut = modelHash;
        return true;
    };

    const QString latestPath = QDir(checkpointDirectory).filePath(QStringLiteral("latest.json"));
    const QString lastValidPath = QDir(checkpointDirectory).filePath(QStringLiteral("lastvalid.json"));
    if (!QFileInfo::exists(latestPath) && !QFileInfo::exists(lastValidPath))
    {
        if (record.contains(QStringLiteral("nativeCheckpointSeed")))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The committed recovery checkpoint seed exists, but its current latest pointer is missing."),
                             qualificationFacts);
        return qualified(QStringLiteral("recompute_required"),
                         QStringLiteral("No complete accepted-state checkpoint was published before interruption."),
                         qualificationFacts);
    }
    if (!QFileInfo::exists(latestPath))
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The current native checkpoint pointer is missing while a previous pointer exists; its accepted-step loss cannot be measured safely."),
                         qualificationFacts);

    QJsonObject envelope;
    QString envelopePath;
    QString envelopeSha;
    QString checkpointPath;
    QString checkpointSha;
    QString checkpointError;
    const bool checkpointValid = readPointer(QStringLiteral("latest.json"), &envelope, &envelopePath,
                                              &envelopeSha, &checkpointPath, &checkpointSha, &checkpointError);
    if (!checkpointValid)
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The current native checkpoint pointer does not verify: %1")
                             .arg(checkpointError), qualificationFacts);
    const double acceptedSequence = envelope.value(QStringLiteral("acceptedSeq")).toDouble();
    const double acceptedTime = envelope.value(QStringLiteral("timeS")).toDouble();
    quint64 targetSteps = 0;
    try
    {
        RModel originalInput;
        originalInput.read(checkpointInputPath);
        const RProblemSetup &originalSetup = originalInput.getProblemSetup();
        const RStructuralDynamicsSetup &originalDynamics = originalSetup.getStructuralDynamicsSetup();
        const RStructuralDynamicsRestartData &initialRestart = originalSetup.getStructuralDynamicsRestartData();
        if (initialRestart.available) initialRestart.validate();
        const quint64 baselineSteps = initialRestart.available ? initialRestart.completedSteps : 0;
        const quint64 plannedSteps = originalDynamics.getNSteps();
        if (!originalDynamics.getEnabled() || originalDynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
            plannedSteps == 0 || baselineSteps > std::numeric_limits<quint64>::max() - plannedSteps ||
            baselineSteps + plannedSteps > std::numeric_limits<uint>::max() ||
            acceptedSequence > double(baselineSteps + plannedSteps))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The original frozen input has an invalid target or the selected checkpoint exceeds it."),
                             qualificationFacts);
        targetSteps = baselineSteps + plannedSteps;
    }
    catch (const RError &readError)
    {
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The original frozen input no longer provides a valid recovery target: %1")
                             .arg(readError.getMessage()), qualificationFacts);
    }
    catch (const std::exception &readError)
    {
        return qualified(QStringLiteral("conflict"),
                         QStringLiteral("The original frozen input no longer provides a valid recovery target: %1")
                             .arg(QString::fromLocal8Bit(readError.what())), qualificationFacts);
    }
    const quint64 remainingSteps = targetSteps - quint64(acceptedSequence);
    QJsonObject checkpoint{{QStringLiteral("checkpointPath"), checkpointPath},
                           {QStringLiteral("checkpointSha256"), checkpointSha},
                           {QStringLiteral("envelopePath"), envelopePath},
                           {QStringLiteral("envelopeSha256"), envelopeSha},
                           {QStringLiteral("acceptedSeq"), acceptedSequence},
                           {QStringLiteral("targetSteps"), double(targetSteps)},
                           {QStringLiteral("remainingSteps"), double(remainingSteps)},
                           {QStringLiteral("timeS"), acceptedTime},
                           {QStringLiteral("dtS"), envelope.value(QStringLiteral("dtS"))},
                           {QStringLiteral("usedLastValid"), false},
                           {QStringLiteral("context"), context},
                           {QStringLiteral("replayRange"), QJsonObject{
                               {QStringLiteral("afterAcceptedSeq"), acceptedSequence},
                               {QStringLiteral("targetSteps"), double(targetSteps)},
                               {QStringLiteral("remainingSteps"), double(remainingSteps)},
                               {QStringLiteral("maxAcceptedSteps"), 10},
                               {QStringLiteral("maxSimulationSeconds"), 1.0}}}};
    QJsonObject extra = qualificationFacts;
    extra.insert(QStringLiteral("checkpoint"), checkpoint);
    extra.insert(QStringLiteral("state"), QStringLiteral("interrupted"));
    if (inspectPartialResult)
    {
        QJsonObject partialResult;
        QString partialError;
        if (!inspectUnverifiedWorkingNativeResult(record, checkpointPath, targetSteps,
                                                   context.value(QStringLiteral("dtS")).toDouble(),
                                                   &partialResult, &partialError))
            return qualified(QStringLiteral("conflict"), partialError, extra);
        extra.insert(QStringLiteral("partialResult"), partialResult);
        const double partialAccepted = partialResult.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
        const double acceptedLoss = qMax(0.0, partialAccepted - acceptedSequence);
        const double simulationLoss = qMax(0.0,
            partialResult.value(QStringLiteral("timeS")).toDouble() - acceptedTime);
        if (acceptedLoss > 10.0 || simulationLoss > 1.0 + 1.0e-12 * qMax(1.0, std::abs(acceptedTime)))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The working result exceeds the fixed accepted-checkpoint replay-loss bound."),
                             extra);
        partialResult.insert(QStringLiteral("checkpointAcceptedSeq"), acceptedSequence);
        partialResult.insert(QStringLiteral("discardedAcceptedSteps"), acceptedLoss);
        partialResult.insert(QStringLiteral("discardedSimulationSeconds"), simulationLoss);
        extra.insert(QStringLiteral("partialResult"), partialResult);
        if (partialAccepted == double(targetSteps))
        {
            partialResult.insert(QStringLiteral("remainingSteps"), 0);
            extra.insert(QStringLiteral("partialResult"), partialResult);
            return qualified(QStringLiteral("reconcile_required"),
                             QStringLiteral("The verified working result LINK already reaches the original target; reconcile the result before a new attempt."),
                             extra);
        }
        if (partialAccepted > double(targetSteps))
            return qualified(QStringLiteral("conflict"),
                             QStringLiteral("The verified working result LINK exceeds the original accepted-step target."),
                             extra);
    }
    if (remainingSteps == 0)
        return qualified(QStringLiteral("reconcile_required"),
                         QStringLiteral("The selected accepted checkpoint already reaches the original target; reconcile the existing result."),
                         extra);
    return qualified(QStringLiteral("recoverable"),
                     QStringLiteral("A complete accepted-state checkpoint matches the frozen model, package, control mode, solver runtime, source binding, and checkpoint policy."),
                     extra);
}

bool SolverManager::requestNativeAcceptedBoundaryPause(const QString &jobId,
                                                       uint acceptedSequence,
                                                       QString *error)
{
    if (error) error->clear();
    bool requested = false;
    QString localError;
    const bool invoked = this->invokeOnOwner([this, &jobId, acceptedSequence, &requested, &localError]()
    {
        requested = this->requestNativeAcceptedBoundaryPauseOnOwner(jobId, acceptedSequence, &localError);
    });
    if (!invoked) localError = QStringLiteral("The accepted-boundary request could not run on the manager thread.");
    if (error) *error = localError;
    return invoked && requested;
}

bool SolverManager::requestNativeAcceptedBoundaryPauseOnOwner(const QString &jobId,
                                                              uint acceptedSequence,
                                                              QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != this->thread() || !isNativeJobId(jobId) || acceptedSequence == 0)
    {
        if (error) *error = QStringLiteral("The accepted-boundary request identity or sequence is invalid.");
        return false;
    }
    const QJsonObject record = this->nativeJobs.value(jobId);
    if (record.isEmpty() || record.value(QStringLiteral("state")).toString() != QStringLiteral("queued"))
    {
        if (error) *error = QStringLiteral("An accepted-boundary pause can only be requested for an owned queued native task.");
        return false;
    }
    if (!record.value(QStringLiteral("nativeCheckpointEnabled")).toBool())
    {
        if (error) *error = QStringLiteral("Accepted-boundary pauses require the pinned native checkpoint channel.");
        return false;
    }
    const QPointer<SolverTask> task = this->nativeTasks.value(jobId);
    if (!task || task->isFinished())
    {
        if (error) *error = QStringLiteral("The queued native task is no longer owned by this manager.");
        return false;
    }
    return task->requestNativeAcceptedBoundaryPause(
        acceptedSequence,
        [this, jobId, acceptedSequence](const QString &statePath, QString *persistError)
        {
            const bool committed = this->updateNativeJob(jobId,
                {{QStringLiteral("nativeExecutionPauseRequestedSeq"), double(acceptedSequence)},
                 {QStringLiteral("nativeExecutionStateRef"), statePath},
                 {QStringLiteral("nativeExecutionPauseStatus"), QStringLiteral("requested")},
                 {QStringLiteral("nativeExecutionPauseRequestedAt"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
            if (!committed && persistError)
            {
                *persistError = this->nativeJobDiagnostics.value(jobId)
                        .value(QStringLiteral("message")).toString();
                if (persistError->isEmpty())
                    *persistError = QStringLiteral("The accepted-boundary request did not commit to the native job registry.");
            }
            return committed;
        }, error);
}

QString SolverManager::submitNativeRecovery(const QString &jobId,
                                            const QString &checkpointSha256,
                                            uint remainingSteps,
                                            QString *error)
{
    if (error) error->clear();
    QString submittedJobId;
    QString localError;
    const bool invoked = this->invokeOnOwner([this, &jobId, &checkpointSha256, remainingSteps,
                                               &submittedJobId, &localError]()
    {
        submittedJobId = this->submitNativeRecoveryOnOwner(jobId, checkpointSha256,
                                                             remainingSteps, &localError);
    });
    if (!invoked) localError = QStringLiteral("Native recovery submission could not run on the manager thread.");
    if (error) *error = localError;
    return submittedJobId;
}

QString SolverManager::submitNativeRecoveryOnOwner(const QString &jobId,
                                                   const QString &checkpointSha256,
                                                   uint remainingSteps,
                                                   QString *error)
{
    if (error) error->clear();
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return QString();
    };
    if (QThread::currentThread() != this->thread() || !isNativeJobId(jobId) ||
        !isLowerSha256(checkpointSha256))
        return reject(QStringLiteral("The native recovery identity or selected checkpoint SHA-256 is invalid."));

    if (!this->nativeJobs.contains(jobId))
        return reject(QStringLiteral("conflict: the source native job has no fresh registered recovery preview."));
    const QJsonObject cachedSourceRecord = this->nativeJobs.value(jobId);
    const QString sourceProjectId = cachedSourceRecord.value(QStringLiteral("projectId")).toString();
    const QString sourceLogicalJobId = logicalNativeJobId(jobId, cachedSourceRecord);
    QString logicalLockError;
    const QString logicalLockPath = managedLogicalRecoveryLockPath(
        this->nativeJobRegistryRoot, sourceProjectId, sourceLogicalJobId, true, &logicalLockError);
    if (logicalLockPath.isEmpty()) return reject(QStringLiteral("conflict: %1").arg(logicalLockError));
    auto logicalRecoveryLock = std::make_shared<QLockFile>(logicalLockPath);
    logicalRecoveryLock->setStaleLockTime(0);
    if (!logicalRecoveryLock->tryLock(0))
        return reject(QStringLiteral("reconcile_required: another GUI process is staging or executing an attempt for this logical native job."));

    const QJsonObject qualification = this->nativeJobRecoveryQueryOnOwner(jobId, true);
    if (qualification.value(QStringLiteral("eligibility")).toString() != QStringLiteral("recoverable"))
    {
        const QString eligibility = qualification.value(QStringLiteral("eligibility")).toString();
        return reject(QStringLiteral("%1: %2")
                      .arg(eligibility.isEmpty() ? QStringLiteral("conflict") : eligibility,
                           qualification.value(QStringLiteral("reason")).toString()));
    }
    const QJsonObject checkpoint = qualification.value(QStringLiteral("checkpoint")).toObject();
    if (checkpoint.value(QStringLiteral("checkpointSha256")).toString() != checkpointSha256)
        return reject(QStringLiteral("The selected checkpoint changed after the fresh recovery preview."));
    if (checkpoint.value(QStringLiteral("remainingSteps")).toDouble(-1.0) != double(remainingSteps))
        return reject(QStringLiteral("The requested remaining step count does not match the qualified checkpoint target."));

    const QJsonObject originalRecord = this->nativeJobs.value(jobId);
    const QString originalRegistrySha = originalRecord.value(QStringLiteral("nativeJobRegistrySha256")).toString();
    const QString checkpointPath = checkpoint.value(QStringLiteral("checkpointPath")).toString();
    const QString envelopePath = checkpoint.value(QStringLiteral("envelopePath")).toString();
    const QString envelopeSha256 = checkpoint.value(QStringLiteral("envelopeSha256")).toString();
    const QString inputPath = originalRecord.value(QStringLiteral("nativeCheckpointInputModelRef")).toString();
    const QJsonObject sourceRuntimeStamp = originalRecord.value(QStringLiteral("sourceRuntimeStamp")).toObject();
    const QString sourceDocumentId = sourceRuntimeStamp.value(QStringLiteral("documentId")).toString();
    bool revisionOk = false;
    const quint64 sourceRevision = sourceRuntimeStamp.value(QStringLiteral("revision")).toString().toULongLong(&revisionOk);
    if (originalRegistrySha.isEmpty() || sourceDocumentId.isEmpty() || !revisionOk ||
        QString::number(sourceRevision) != sourceRuntimeStamp.value(QStringLiteral("revision")).toString() ||
        !QFileInfo(checkpointPath).isAbsolute() || !QFileInfo(envelopePath).isAbsolute() ||
        !QFileInfo(inputPath).isAbsolute() || !isLowerSha256(envelopeSha256) ||
        QString::fromLatin1(sha256File(checkpointPath).toHex()) != checkpointSha256 ||
        QString::fromLatin1(sha256File(envelopePath).toHex()) != envelopeSha256)
        return reject(QStringLiteral("The selected checkpoint, envelope, or original source identity changed after qualification."));

    const double acceptedValue = checkpoint.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
    const double checkpointTime = checkpoint.value(QStringLiteral("timeS")).toDouble(-1.0);
    if (!std::isfinite(acceptedValue) || acceptedValue < 1.0 ||
        acceptedValue > double(std::numeric_limits<uint>::max()) || std::floor(acceptedValue) != acceptedValue ||
        !std::isfinite(checkpointTime) || checkpointTime < 0.0)
        return reject(QStringLiteral("The selected checkpoint has an invalid accepted sequence or time."));
    const quint64 acceptedSteps = quint64(acceptedValue);

    Model originalInput;
    Model recoveryModel;
    try
    {
        originalInput.read(inputPath);
        recoveryModel.read(checkpointPath);
    }
    catch (const RError &readError)
    {
        return reject(QStringLiteral("The frozen native input or selected checkpoint could not be decoded: %1")
                      .arg(readError.getMessage()));
    }
    catch (const std::exception &readError)
    {
        return reject(QStringLiteral("The frozen native input or selected checkpoint could not be decoded: %1")
                      .arg(QString::fromLocal8Bit(readError.what())));
    }

    const RProblemSetup &initialSetup = originalInput.getProblemSetup();
    const RStructuralDynamicsSetup &initialDynamics = initialSetup.getStructuralDynamicsSetup();
    const RStructuralDynamicsRestartData &initialRestart = initialSetup.getStructuralDynamicsRestartData();
    const RStructuralDynamicsRestartData &acceptedRestart =
            recoveryModel.getProblemSetup().getStructuralDynamicsRestartData();
    if (!initialDynamics.getEnabled() || initialDynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
        initialSetup.getStructuralControlSetup().enabled || initialSetup.getStructuralCableSetup().enabled ||
        initialSetup.getStructuralPiezoSetup().enabled || initialSetup.getStructuralThermalControlSetup().enabled ||
        !acceptedRestart.available || acceptedRestart.completedSteps != acceptedSteps ||
        std::abs(acceptedRestart.time - checkpointTime) > 1.0e-12 * qMax(1.0, std::abs(checkpointTime)))
        return reject(QStringLiteral("The selected restart MODEL no longer matches the qualified uncontrolled Newmark checkpoint."));
    const quint64 baselineSteps = initialRestart.available ? initialRestart.completedSteps : 0;
    const quint64 originalAdditionalSteps = initialDynamics.getNSteps();
    if (originalAdditionalSteps == 0 || baselineSteps > std::numeric_limits<quint64>::max() - originalAdditionalSteps)
        return reject(QStringLiteral("The frozen native input has an invalid original target step count."));
    const quint64 targetAcceptedSteps = baselineSteps + originalAdditionalSteps;
    if (acceptedSteps < baselineSteps || acceptedSteps > targetAcceptedSteps)
        return reject(QStringLiteral("The selected checkpoint is outside the original accepted-step range."));
    const quint64 computedRemainingSteps = targetAcceptedSteps - acceptedSteps;
    if (computedRemainingSteps == 0)
        return reject(QStringLiteral("reconcile_required: the selected accepted checkpoint already reaches the original target; reconcile the existing result."));
    if (computedRemainingSteps > std::numeric_limits<uint>::max() ||
        remainingSteps == 0 || quint64(remainingSteps) != computedRemainingSteps)
        return reject(QStringLiteral("The requested remaining step count does not equal the pinned original target minus the selected accepted sequence."));

    const QString resultArtifactPath = originalRecord.value(QStringLiteral("resultArtifactRef")).toString();
    const QString originalRunRoot = getRunRoot(resultArtifactPath);
    const QString expectedJournalPath = QDir(originalRunRoot).filePath(QStringLiteral("events/native-results.ndjson"));
    const QString registeredJournalPath = originalRecord.value(QStringLiteral("logArtifactRefs")).toObject()
            .value(QStringLiteral("nativeResultJournal")).toString();
    const QString manifestPath = QDir(QFileInfo(resultArtifactPath).absolutePath())
            .filePath(QStringLiteral("native-result.manifest.json"));
    if (resultArtifactPath.isEmpty() || originalRunRoot.isEmpty() || hasSymlinkComponent(originalRunRoot) ||
        !samePathText(registeredJournalPath, expectedJournalPath))
        return reject(QStringLiteral("The interrupted job's result journal is missing or outside its immutable run directory."));
    if (QFileInfo(manifestPath).exists() ||
        (!originalRecord.value(QStringLiteral("resultManifestRef")).toString().isEmpty() &&
         QFileInfo(originalRecord.value(QStringLiteral("resultManifestRef")).toString()).exists()))
        return reject(QStringLiteral("reconcile_required: a result manifest exists and must be reconciled before a new attempt."));

    const QFileInfo journalInfo(expectedJournalPath);
    if (journalInfo.isSymLink() || (journalInfo.exists() &&
        (!journalInfo.isFile() || journalInfo.canonicalFilePath() != journalInfo.absoluteFilePath() ||
         journalInfo.size() > 16 * 1024 * 1024)))
        return reject(QStringLiteral("The interrupted job's native result journal is linked or exceeds its verification bound."));
    const bool journalPresent = journalInfo.exists();
    const QString journalSha = journalPresent
            ? QString::fromLatin1(sha256File(expectedJournalPath).toHex()) : QString();
    if (journalPresent && !isLowerSha256(journalSha))
        return reject(QStringLiteral("The interrupted job's native result journal could not be hashed."));

    if (this->isSolverQueued(sourceDocumentId))
        return reject(QStringLiteral("A live native solver task still uses the interrupted job's logical source identity."));

    recoveryModel.setFileName(inputPath);
    recoveryModel.getProblemSetup().setRestart(true);
    recoveryModel.getProblemSetup().getStructuralDynamicsSetup().setNSteps(remainingSteps);
    const QJsonObject originalProjectRef = originalRecord.value(QStringLiteral("projectSnapshotRef")).toObject();
    const bool linkedSnapshot = originalRecord.value(QStringLiteral("frozenSnapshotVerified")).toBool();
    if (linkedSnapshot && originalProjectRef.isEmpty())
        return reject(QStringLiteral("The frozen project source binding is missing from the interrupted job."));

    QString sourceArtifactPath = linkedSnapshot
            ? originalRecord.value(QStringLiteral("frozenSnapshotPath")).toString()
            : originalRecord.value(QStringLiteral("sourceModelFileRef")).toString();
    QString artifactRoot;
    if (!sourceArtifactPath.isEmpty() && QFileInfo(sourceArtifactPath).isAbsolute())
        artifactRoot = QFileInfo(sourceArtifactPath).absolutePath();
    else
    {
        artifactRoot = Application::instance()->getApplicationSettings()->getDataDir();
        if (artifactRoot.isEmpty()) artifactRoot = QDir::tempPath();
    }
    if (QFileInfo(artifactRoot).isSymLink() || hasSymlinkComponent(artifactRoot) ||
        !QFileInfo(artifactRoot).isDir() || QFileInfo(artifactRoot).canonicalFilePath().isEmpty() ||
        !samePathText(artifactRoot, QFileInfo(artifactRoot).canonicalFilePath()))
        return reject(QStringLiteral("The native recovery artifact root is unavailable or non-canonical."));

    const QString logicalJobId = originalRecord.value(QStringLiteral("logicalJobId")).toString().isEmpty()
            ? jobId : originalRecord.value(QStringLiteral("logicalJobId")).toString();
    const QJsonObject originalRecovery = originalRecord.value(QStringLiteral("recovery")).toObject();
    const QJsonObject logicalSourceRuntimeStamp = originalRecovery.value(QStringLiteral("logicalSourceRuntimeStamp")).toObject().isEmpty()
            ? sourceRuntimeStamp
            : originalRecovery.value(QStringLiteral("logicalSourceRuntimeStamp")).toObject();
    QJsonObject replayRange = checkpoint.value(QStringLiteral("replayRange")).toObject();
    replayRange.insert(QStringLiteral("targetAcceptedSeq"), double(targetAcceptedSteps));
    replayRange.insert(QStringLiteral("remainingSteps"), double(computedRemainingSteps));
    replayRange.insert(QStringLiteral("recoverySourceJobId"), jobId);
    const QJsonObject recoveryProvenance{{QStringLiteral("logicalJobId"), logicalJobId},
                                         {QStringLiteral("recoverySourceJobId"), jobId},
                                         {QStringLiteral("logicalSourceRuntimeStamp"), logicalSourceRuntimeStamp},
                                         {QStringLiteral("selectedEnvelopePath"), envelopePath},
                                         {QStringLiteral("selectedEnvelopeSha256"), envelopeSha256},
                                         {QStringLiteral("selectedCheckpointSha256"), checkpointSha256},
                                         {QStringLiteral("selectedCheckpointPath"), checkpointPath},
                                         {QStringLiteral("acceptedSeq"), double(acceptedSteps)},
                                         {QStringLiteral("checkpointTimeS"), checkpointTime},
                                         {QStringLiteral("targetAcceptedSeq"), double(targetAcceptedSteps)},
                                         {QStringLiteral("remainingSteps"), double(computedRemainingSteps)},
                                         {QStringLiteral("replayRange"), replayRange},
                                         {QStringLiteral("sourceResultArtifactRef"), resultArtifactPath},
                                         {QStringLiteral("sourceResultJournalRef"), expectedJournalPath},
                                         {QStringLiteral("sourceResultJournalPresent"), journalPresent},
                                         {QStringLiteral("sourceResultJournalSha256"), journalSha},
                                         {QStringLiteral("sourceInputSnapshotRef"), originalRecord.value(QStringLiteral("inputSnapshotRef"))},
                                         {QStringLiteral("sourceInputSnapshotSha256"), originalRecord.value(QStringLiteral("inputSnapshotSha256"))},
                                         {QStringLiteral("sourceInputPackageRef"), originalRecord.value(QStringLiteral("inputPackageRef"))},
                                         {QStringLiteral("sourceInputPackageSha256"), originalRecord.value(QStringLiteral("inputPackageSha256"))},
                                         {QStringLiteral("sourcePartialResult"), qualification.value(QStringLiteral("partialResult"))},
                                         {QStringLiteral("logicalSourceModelFileRef"),
                                          originalRecord.value(QStringLiteral("sourceModelFileRef"))}};

    const QString originalFrozenInputPath = originalRecord.value(QStringLiteral("inputSnapshotRef")).toString();
    const QString originalFrozenPackagePath = originalRecord.value(QStringLiteral("inputPackageRef")).toString();
    const QString expectedOriginalFrozenInputSha =
            originalRecord.value(QStringLiteral("inputSnapshotSha256")).toString();
    const QString expectedOriginalFrozenPackageSha =
            originalRecord.value(QStringLiteral("inputPackageSha256")).toString();
    const NativeInputPackageSnapshot frozenInputBefore =
            snapshotNativeInputPackage(originalFrozenInputPath, originalFrozenPackagePath);
    if (frozenInputBefore.inputSha256 != expectedOriginalFrozenInputSha ||
        frozenInputBefore.packageSha256 != expectedOriginalFrozenPackageSha)
    {
        return reject(QStringLiteral("The source job's frozen input failed its pre-stage verification: %1")
                      .arg(QString::fromUtf8(QJsonDocument(nativeInputPackageChangeDetails(
                          frozenInputBefore, frozenInputBefore, expectedOriginalFrozenInputSha,
                          expectedOriginalFrozenPackageSha)).toJson(QJsonDocument::Compact))));
    }

    auto *task = new SolverTask(Application::instance()->getApplicationSettings(), recoveryModel,
                                 {sourceDocumentId, sourceRevision}, recoveryProvenance, artifactRoot,
                                 this->taskServerName, logicalRecoveryLock);
    const QString newJobId = task->getTaskID().toString();
    this->nativeRecoveryLogicalLocks.insert(newJobId, logicalRecoveryLock);
    const auto discardUnqueuedTask = [this, task, newJobId]()
    {
        task->releaseNativeRecoveryLogicalLock();
        this->nativeRecoveryLogicalLocks.remove(newJobId);
        task->deleteLater();
    };
    const QString derivedFrom = linkedSnapshot ? QStringLiteral("frozen_project_snapshot")
                                               : QStringLiteral("live_unlinked");
    const bool snapshotCaptured = task->captureInputSnapshot(
        recoveryModel, {sourceDocumentId, sourceRevision}, RConstants::eod, derivedFrom,
        linkedSnapshot ? originalRecord.value(QStringLiteral("frozenSnapshotPath")).toString() : QString(),
        linkedSnapshot ? originalRecord.value(QStringLiteral("frozenSnapshotSha256")).toString() : QString());
    const NativeInputPackageSnapshot frozenInputAfter =
            snapshotNativeInputPackage(originalFrozenInputPath, originalFrozenPackagePath);
    if (frozenInputAfter.inputSha256 != frozenInputBefore.inputSha256 ||
        frozenInputAfter.packageSha256 != frozenInputBefore.packageSha256)
    {
        discardUnqueuedTask();
        return reject(QStringLiteral("The source job's frozen input changed during recovery staging: %1")
                      .arg(QString::fromUtf8(QJsonDocument(nativeInputPackageChangeDetails(
                          frozenInputBefore, frozenInputAfter, expectedOriginalFrozenInputSha,
                          expectedOriginalFrozenPackageSha)).toJson(QJsonDocument::Compact))));
    }
    if (!snapshotCaptured)
    {
        discardUnqueuedTask();
        return reject(QStringLiteral("The selected accepted checkpoint could not be staged as a new detached input snapshot."));
    }

    std::unique_ptr<Model> capturedInput = task->copyCapturedInputForValidation();
    if (!capturedInput)
    {
        discardUnqueuedTask();
        return reject(QStringLiteral("The detached native recovery input was not available for setup validation."));
    }
    SolverSetupChecker checker(*capturedInput);
    QStringList warnings;
    QStringList errors;
    checker.perform(warnings, errors);
    if (!errors.isEmpty())
    {
        discardUnqueuedTask();
        return reject(QStringLiteral("The accepted checkpoint no longer passes native solver setup validation: %1")
                      .arg(errors.first()));
    }

    const QJsonObject rechecked = this->nativeJobRecoveryQueryOnOwner(jobId, true);
    const QJsonObject recheckedCheckpoint = rechecked.value(QStringLiteral("checkpoint")).toObject();
    const QStringList recheckChanges = [&]()
    {
        QStringList changed;
        if (rechecked.value(QStringLiteral("eligibility")).toString() != QStringLiteral("recoverable"))
            changed.append(QStringLiteral("eligibility"));
        if (recheckedCheckpoint.value(QStringLiteral("checkpointSha256")).toString() != checkpointSha256)
            changed.append(QStringLiteral("checkpointSha256"));
        if (recheckedCheckpoint.value(QStringLiteral("envelopeSha256")).toString() != envelopeSha256)
            changed.append(QStringLiteral("envelopeSha256"));
        if (this->nativeJobs.value(jobId).value(QStringLiteral("nativeJobRegistrySha256")).toString() != originalRegistrySha)
            changed.append(QStringLiteral("nativeJobRegistrySha256"));
        if (journalPresent && QString::fromLatin1(sha256File(expectedJournalPath).toHex()) != journalSha)
            changed.append(QStringLiteral("nativeResultJournalSha256"));
        return changed;
    }();
    if (!recheckChanges.isEmpty())
    {
        discardUnqueuedTask();
        QString message = QStringLiteral("The interrupted job qualification changed while staging: %1.")
                .arg(recheckChanges.join(QStringLiteral(", ")));
        if (recheckChanges.contains(QStringLiteral("eligibility")))
        {
            const QString eligibility = rechecked.value(QStringLiteral("eligibility")).toString();
            const QString reason = rechecked.value(QStringLiteral("reason")).toString();
            message += QStringLiteral(" Final qualification: %1%2")
                    .arg(eligibility.isEmpty() ? QStringLiteral("conflict") : eligibility,
                         reason.isEmpty() ? QString() : QStringLiteral(" — ") + reason);
            const double originalAcceptedSeq = checkpoint.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
            const double currentAcceptedSeq = recheckedCheckpoint.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
            if (originalAcceptedSeq >= 0.0 && currentAcceptedSeq >= 0.0 &&
                originalAcceptedSeq != currentAcceptedSeq)
                message += QStringLiteral(" Selected checkpoint sequence changed from %1 to %2.")
                        .arg(originalAcceptedSeq, 0, 'f', 0).arg(currentAcceptedSeq, 0, 'f', 0);
        }
        return reject(message);
    }

    if (!this->registerNativeTask(task, newJobId, originalProjectRef,
                                  linkedSnapshot ? QStringLiteral("linked") : QStringLiteral("unlinked"),
                                  linkedSnapshot,
                                  linkedSnapshot ? originalRecord.value(QStringLiteral("frozenSnapshotPath")).toString()
                                                 : QString(),
                                  linkedSnapshot ? originalRecord.value(QStringLiteral("frozenSnapshotSha256")).toString()
                                                 : QString(), error))
    {
        discardUnqueuedTask();
        return {};
    }
    this->lastLogFileName = task->getLogFileName();
    this->RJobManager::submit(task);
    return newJobId;
}

QJsonObject SolverManager::nativeResultManifestForSource(const QString &sourcePath) const
{
    QJsonObject result{{QStringLiteral("known"), false},
                       {QStringLiteral("valid"), false}};
    const bool invoked = this->invokeOnOwner([this, &result, &sourcePath]()
    {
        const_cast<SolverManager*>(this)->loadNativeJobsFromDiskOnOwner();
        for (const QString &jobId : this->nativeJobOrder)
        {
            QJsonObject record = this->nativeJobs.value(jobId);
            if (samePathText(record.value(QStringLiteral("resultArtifactRef")).toString(), sourcePath) ||
                samePathText(record.value(QStringLiteral("resultSourceBinding")).toObject()
                             .value(QStringLiteral("resolvedSourcePath")).toString(), sourcePath))
            {
                result = {{QStringLiteral("known"), true},
                          {QStringLiteral("valid"), false},
                          {QStringLiteral("jobId"), jobId}};
                QString registryError;
                QString artifactError;
                if (!this->validateNativeJobRecordOnDisk(jobId, &registryError) ||
                    !const_cast<SolverManager*>(this)->revalidateNativeTerminalArtifactsOnOwner(jobId, &artifactError))
                {
                    result.insert(QStringLiteral("error"), registryError.isEmpty() ? artifactError : registryError);
                    return;
                }
                const QJsonObject descriptor = this->nativeResultDescriptors.value(jobId);
                if (descriptor.isEmpty())
                {
                    result.insert(QStringLiteral("error"),
                                  QStringLiteral("The registered source job has no verified terminal result manifest."));
                    return;
                }
                result.insert(QStringLiteral("valid"), true);
                result.insert(QStringLiteral("resultManifestRef"), descriptor.value(QStringLiteral("resultManifestRef")));
                result.insert(QStringLiteral("resultManifestSha256"), descriptor.value(QStringLiteral("resultManifestSha256")));
                return;
            }
        }
    });
    if (!invoked)
        return {{QStringLiteral("known"), true},
                {QStringLiteral("valid"), false},
                {QStringLiteral("error"), QStringLiteral("The managed source-job lookup could not run on its owner thread.")}};
    return result;
}

QJsonObject SolverManager::nativeResultQuery(const QString &jobId, const QString &operation,
                                           const QJsonObject &arguments) const
{
    const auto failure = [](const QString &code, const QString &message)
    {
        return QJsonObject{{QStringLiteral("valid"), false}, {QStringLiteral("code"), code},
                           {QStringLiteral("message"), message}};
    };
    QJsonObject result;
    if (!this->invokeOnOwner([this, &result, &jobId, &operation, &arguments, &failure]()
    {
        const_cast<SolverManager*>(this)->loadNativeJobsFromDiskOnOwner();
        if (!this->nativeJobs.contains(jobId))
        {
            result = failure(QStringLiteral("UNKNOWN_NATIVE_JOB"), QStringLiteral("Unknown native solver job ID."));
            return;
        }
        QJsonObject record = this->nativeJobs.value(jobId);
        QString registryError;
        if (!this->validateNativeJobRecordOnDisk(jobId, &registryError))
        {
            result = failure(QStringLiteral("JOB_REGISTRY_CONFLICT"), registryError);
            return;
        }
        if (record.value(QStringLiteral("state")).toString() == QStringLiteral("succeeded") ||
            record.value(QStringLiteral("state")).toString() == QStringLiteral("failed") ||
            record.value(QStringLiteral("state")).toString() == QStringLiteral("cancelled"))
        {
            QString artifactError;
            if (!const_cast<SolverManager*>(this)->revalidateNativeTerminalArtifactsOnOwner(jobId, &artifactError))
            {
                result = failure(QStringLiteral("ARTIFACT_CHANGED"), artifactError);
                return;
            }
            record = this->nativeJobs.value(jobId);
        }
        QJsonObject brokerArguments = arguments;
        if (brokerArguments.contains(QStringLiteral("artifactId")) &&
            (!brokerArguments.value(QStringLiteral("artifactId")).isString() ||
             brokerArguments.value(QStringLiteral("artifactId")).toString().isEmpty()))
        {
            result = failure(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("artifactId must be a non-empty string."));
            return;
        }
        const QString requestedArtifact = brokerArguments.take(QStringLiteral("artifactId")).toString();
        const QJsonObject provenance = record.value(QStringLiteral("nativeResultProvenance")).toObject();
        const QString mode = provenance.value(QStringLiteral("control")).toObject().value(QStringLiteral("enabled")).toBool()
                ? QStringLiteral("active") : QStringLiteral("passive");
        const QByteArray artifactSeed = jobId.toUtf8() + '\n' + mode.toUtf8() + '\n' +
                provenance.value(QStringLiteral("inputPackageSha256")).toString().toLatin1();
        const QString artifactId = QStringLiteral("artifact-") + QString::fromLatin1(QCryptographicHash::hash(
                    artifactSeed, QCryptographicHash::Sha256).toHex()).left(32);
        QJsonArray liveFrames = record.value(QStringLiteral("resultFrames")).toArray();
        bool artifactMatches = requestedArtifact.isEmpty() || requestedArtifact == artifactId;
        if (requestedArtifact.startsWith(artifactId + QStringLiteral("-live-")))
        {
            bool validSequence = false;
            const int frozenSequence = requestedArtifact.mid(artifactId.size() + 6).toInt(&validSequence);
            artifactMatches = validSequence && frozenSequence > 0 && frozenSequence <= liveFrames.size() &&
                    requestedArtifact == artifactId + QStringLiteral("-live-") + QString::number(frozenSequence);
            if (artifactMatches) while (liveFrames.size() > frozenSequence) liveFrames.removeLast();
        }
        if (!artifactMatches)
        {
            result = failure(QStringLiteral("ARTIFACT_ID_MISMATCH"), QStringLiteral("The artifact does not belong to the requested job."));
            return;
        }
        if (record.value(QStringLiteral("state")).toString() == QStringLiteral("succeeded") &&
            record.value(QStringLiteral("resultManifestVerified")).toBool())
        {
            QString error;
            if (!validNativeResultManifestDescriptor(this->nativeResultDescriptors.value(jobId), record, jobId, &error))
                result = failure(QStringLiteral("ARTIFACT_CHANGED"), error);
            else if (requestedArtifact.startsWith(artifactId + QStringLiteral("-live-")))
                result = RNativeResultBroker::queryLive(QFileInfo(record.value(QStringLiteral("resultArtifactRef")).toString()).absolutePath(),
                                                        provenance, liveFrames, operation, brokerArguments);
            else
                result = RNativeResultBroker::query(record.value(QStringLiteral("resultManifestRef")).toString(),
                                                    record.value(QStringLiteral("resultManifestSha256")).toString(),
                                                    operation, brokerArguments);
            return;
        }
        const QString state = record.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("queued"))
        {
            result = failure(QStringLiteral("RESULT_PENDING"), QStringLiteral("The native job is queued and has no persisted frames yet."));
            return;
        }
        if (state == QStringLiteral("succeeded") &&
            !record.value(QStringLiteral("nativeResultCatalogExpected")).toBool() &&
            record.value(QStringLiteral("nativeResultCatalogCode")).toString() == QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"))
        {
            result = failure(QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"), record.value(QStringLiteral("nativeResultCatalogMessage")).toString());
            return;
        }
        if (state != QStringLiteral("running") && state != QStringLiteral("cancelling"))
        {
            result = failure(QStringLiteral("RESULT_NOT_AVAILABLE"), QStringLiteral("This job has no verified completed native result."));
            return;
        }
        if (!record.value(QStringLiteral("resultFramesAvailable")).toBool())
        {
            result = failure(QStringLiteral("RESULT_PENDING"), QStringLiteral("No trusted persisted native frames are available yet."));
            return;
        }
        result = RNativeResultBroker::queryLive(QFileInfo(record.value(QStringLiteral("resultArtifactRef")).toString()).absolutePath(),
                                                provenance, liveFrames,
                                                operation, brokerArguments);
    }))
        return failure(QStringLiteral("WRONG_THREAD"), QStringLiteral("Native result query could not reach the registry owner."));
    return result;
}

bool SolverManager::cancelJob(const QString &jobId, QString *error)
{
    if (error) error->clear();
    bool cancelled = false;
    QString localError;
    if (!this->invokeOnOwner([this, &cancelled, &localError, &jobId]()
    {
        cancelled = this->cancelJobOnOwner(jobId, &localError);
    }))
        localError = QStringLiteral("Cancellation could not run on the manager thread.");
    if (error) *error = localError;
    return cancelled;
}

bool SolverManager::cancelJobOnOwner(const QString &jobId, QString *error)
{
    if (error) error->clear();
    auto recordIt = this->nativeJobs.find(jobId);
    if (recordIt == this->nativeJobs.end())
    {
        if (error) *error = QStringLiteral("Unknown native solver job ID.");
        return false;
    }
    const QString state = recordIt->value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("succeeded") || state == QStringLiteral("failed") || state == QStringLiteral("cancelled"))
    {
        if (error) *error = QStringLiteral("The native solver job is already terminal.");
        return false;
    }
    QPointer<SolverTask> task = this->nativeTasks.value(jobId);
    if (!task)
    {
        if (error) *error = QStringLiteral("The native solver task is no longer available for cancellation.");
        return false;
    }

    task->kill(); // Only flips the atomic request; QProcess remains worker-owned.
    bool removedFromManagerQueue = false;
    if (state == QStringLiteral("queued"))
    {
        QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
        QQueue<RJob*> remaining;
        while (!this->waitingJobs.isEmpty())
        {
            RJob *candidate = this->waitingJobs.dequeue();
            if (candidate == task.data()) removedFromManagerQueue = true;
            else remaining.enqueue(candidate);
        }
        this->waitingJobs = remaining;
    }
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (removedFromManagerQueue)
    {
        const bool terminalPersisted = this->updateNativeJob(jobId,
            {{QStringLiteral("state"), QStringLiteral("cancelled")},
             {QStringLiteral("cancellationRequested"), true},
             {QStringLiteral("cancelRequestedAt"), now},
             {QStringLiteral("finishedAt"), now},
             {QStringLiteral("updatedAt"), now},
             {QStringLiteral("processStarted"), false},
             {QStringLiteral("processExited"), false},
             {QStringLiteral("resultReadbackComplete"), false},
             {QStringLiteral("message"), QStringLiteral("Cancelled before the native solver process started.")}});
        if (terminalPersisted &&
            this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString() == QStringLiteral("cancelled"))
        {
            task->releaseNativeRecoveryLogicalLock();
            this->nativeRecoveryLogicalLocks.remove(jobId);
        }
        this->nativeTasks.remove(jobId);
        task->deleteLater();
        return true;
    }

    this->updateNativeJob(jobId, {{QStringLiteral("state"), QStringLiteral("cancelling")},
                                  {QStringLiteral("cancellationRequested"), true},
                                  {QStringLiteral("cancelRequestedAt"), now},
                                  {QStringLiteral("updatedAt"), now}});
    return true;
}

bool SolverManager::isSolverQueued(const QString &sourceDocumentId) const
{
    if (sourceDocumentId.isEmpty()) return false;
    bool queued = false;
    const bool invoked = this->invokeOnOwner([this, &sourceDocumentId, &queued]()
    {
        const auto isActive = [](const QString &state)
        {
            return state == QStringLiteral("queued") || state == QStringLiteral("running") ||
                   state == QStringLiteral("cancelling");
        };
        for (const QString &jobId : this->nativeJobOrder)
        {
            const QJsonObject record = this->nativeJobs.value(jobId);
            if (record.value(QStringLiteral("sourceRuntimeStamp")).toObject()
                    .value(QStringLiteral("documentId")).toString() == sourceDocumentId &&
                isActive(record.value(QStringLiteral("state")).toString()))
            {
                queued = true;
                return;
            }
        }

        QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
        const auto matches = [&sourceDocumentId](RJob *job)
        {
            auto *solverTask = dynamic_cast<SolverTask*>(job);
            return solverTask && !solverTask->isFinished() &&
                    !solverTask->getSourceDocumentId().isEmpty() &&
                    solverTask->getSourceDocumentId() == sourceDocumentId;
        };
        for (RJob *job : this->waitingJobs) if (matches(job)) { queued = true; return; }
        for (RJob *job : this->waitingParallelJobs) if (matches(job)) { queued = true; return; }
        for (RJob *job : this->runningJobs) if (matches(job)) { queued = true; return; }
    });
    return invoked && queued;
}

bool SolverManager::hasLiveModelTasks() const
{
    QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
    const auto holdsLiveModel = [](RJob *job)
    {
        if (!job || job->isFinished()) return false;
        const auto *solverTask = dynamic_cast<const SolverTask*>(job);
        return !solverTask || solverTask->getSourceStamp().first.isEmpty();
    };

    for (RJob *job : this->runningJobs)
        if (holdsLiveModel(job)) return true;
    for (RJob *job : this->waitingJobs)
        if (holdsLiveModel(job)) return true;
    for (RJob *job : this->waitingParallelJobs)
        if (holdsLiveModel(job)) return true;
    return false;
}

void SolverManager::stopRunningTasks()
{
    QList<SolverTask*> solverTasks;
    {
        QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
        for (RJob *job : this->runningJobs)
        {
            if (auto *solverTask = dynamic_cast<SolverTask*>(job)) solverTasks.append(solverTask);
        }
    }
    for (SolverTask *solverTask : solverTasks)
    {
        RLogger::info("Stopping solver task (#%s).\n",solverTask->getTaskID().toString().toUtf8().constData());
        QString message = solverTask->getStopCommand();
        foreach (QLocalSocket *localSocket, this->taskClients)
        {
            QByteArray block;
            QDataStream out(&block, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_8);
            RLogger::info("Sendig signal: %s\n",message.toUtf8().constData());
            out << quint32(message.size());
            out << message;
            localSocket->write(block);
            localSocket->flush();
        }
    }
}

void SolverManager::killRunningTasks()
{
    QList<SolverTask*> solverTasks;
    {
        QMutexLocker<QRecursiveMutex> locker(&this->jobsMutex);
        for (RJob *job : this->runningJobs)
        {
            if (auto *solverTask = dynamic_cast<SolverTask*>(job)) solverTasks.append(solverTask);
        }
    }
    for (SolverTask *solverTask : solverTasks)
    {
        RLogger::info("Killing solver task (#%s).\n",solverTask->getTaskID().toString().toUtf8().constData());
        solverTask->kill();
    }
}

const QString &SolverManager::getLastLogFileName() const
{
    return this->lastLogFileName;
}

const QString &SolverManager::getLastResultArtifactFileName() const
{
    return this->lastResultArtifactFileName;
}

void SolverManager::stopServer()
{
    QObject::disconnect(this->taskServer, &QLocalServer::newConnection, this, &SolverManager::onSolverTaskNewConnection);
    this->taskServer->close();
}

void SolverManager::onReadyReadStandardOutput(const QString &message)
{
    emit this->readyReadStandardOutput(message);
}

void SolverManager::onReadyReadStandardError(const QString &message)
{
    emit this->readyReadStandardError(message);
}

void SolverManager::onSolverTaskNewConnection()
{
    RLogger::info("Task client connected\n");
    while (this->taskServer->hasPendingConnections())
    {
        this->taskClients.append(this->taskServer->nextPendingConnection());
    }
}
