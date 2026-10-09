#include <QDir>
#include <QCryptographicHash>
#include <QJsonParseError>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QMap>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>

#include <rml_file_utils.h>
#include <rml_structural_control_setup.h>
#include <rml_view_factor_matrix.h>
#include <rbl_utils.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "model_io.h"
#include "solver_manager.h"
#include "solver_task.h"
#include "application.h"
#include <rnative_result_broker.h>

namespace
{
QString makeRunArtifactFileName(const Model &model,
                                const ApplicationSettings *applicationSettings,
                                const SolverTaskID &taskID,
                                const QString &artifactRoot = QString(),
                                bool nativeRecoveryAttempt = false)
{
    const QString sourceFileName = RFileUtils::getFileNameWithOutTimeStep(model.getFileName());
    const QFileInfo sourceInfo(sourceFileName);

    QString sourceDirectory = artifactRoot.isEmpty() ? sourceInfo.absolutePath() : artifactRoot;
    if (sourceFileName.isEmpty())
    {
        sourceDirectory = applicationSettings->getDataDir();
        if (sourceDirectory.isEmpty()) sourceDirectory = QDir::tempPath();
    }

    QString baseName = nativeRecoveryAttempt
            ? QStringLiteral("recovery-solver-input")
            : sourceInfo.completeBaseName();
    if (baseName.isEmpty()) baseName = QFileInfo(model.getName()).fileName();
    if (baseName.isEmpty()) baseName = QStringLiteral("model");
    if (!nativeRecoveryAttempt &&
        baseName.compare(QStringLiteral("native-checkpoint-input"), Qt::CaseInsensitive) == 0)
        baseName = QStringLiteral("native-checkpoint-input-result");

    const QString sourceExtension = sourceInfo.suffix();
    QString extension;
    if (nativeRecoveryAttempt)
    {
        extension = RModel::getDefaultFileExtension(true);
    }
    else if (sourceExtension.compare(RModel::getDefaultFileExtension(false), Qt::CaseInsensitive) == 0)
    {
        extension = RModel::getDefaultFileExtension(false);
    }
    else if (sourceExtension.compare(RModel::getDefaultFileExtension(true), Qt::CaseInsensitive) == 0)
    {
        extension = RModel::getDefaultFileExtension(true);
    }
    else
    {
        extension = RModel::getDefaultFileExtension(true);
    }

    const QString uniqueRunId = taskID.toString() + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString runDirectory = QDir(sourceDirectory).filePath(QStringLiteral(".range-runs/") + uniqueRunId);
    return QFileInfo(QDir(runDirectory).filePath(QStringLiteral("result/") + baseName + "." + extension)).absoluteFilePath();
}

QString getRunRoot(const QString &resultFileName)
{
    return QFileInfo(QDir(QFileInfo(resultFileName).absolutePath()).filePath(QStringLiteral(".."))).absoluteFilePath();
}

bool writeProvenance(const QString &runRoot,
                     const SolverTaskID &taskID,
                     const QPair<QString, quint64> &sourceStamp,
                     const QString &sourceModelFileName,
                     const QString &inputFileName,
                     const QByteArray &inputSha256,
                     const QByteArray &inputPackageSha256)
{
    QJsonObject provenance;
    provenance.insert(QStringLiteral("taskId"), taskID.toString());
    provenance.insert(QStringLiteral("sourceDocumentId"), sourceStamp.first);
    provenance.insert(QStringLiteral("sourceRevision"), QString::number(sourceStamp.second));
    provenance.insert(QStringLiteral("sourceModelFile"), sourceModelFileName);
    provenance.insert(QStringLiteral("inputModelFile"), inputFileName);
    provenance.insert(QStringLiteral("inputModelSha256"), QString::fromLatin1(inputSha256.toHex()));
    provenance.insert(QStringLiteral("inputPackageSha256"), QString::fromLatin1(inputPackageSha256.toHex()));

    QSaveFile file(QDir(runRoot).filePath(QStringLiteral("provenance.json")));
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(QJsonDocument(provenance).toJson(QJsonDocument::Indented)) < 0) return false;
    return file.commit();
}

QByteArray sha256File(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk;
    while (!file.atEnd())
    {
        chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) return {};
        hash.addData(chunk);
    }
    return hash.result();
}

QByteArray sha256Directory(const QString &directoryName)
{
    const QDir root(directoryName);
    if (!root.exists()) return {};

    QStringList relativeFileNames;
    QDirIterator iterator(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        relativeFileNames.append(root.relativeFilePath(iterator.next()));
    }
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
            if (chunk.isEmpty() && file.error() != QFile::NoError) return {};
            hash.addData(chunk);
        }
        hash.addData("\0", 1);
    }
    return hash.result();
}

QJsonObject sourceBindingJson(const RProjectCatalog::SourceBinding &sourceBinding)
{
    QJsonArray sourceArtifacts;
    for (const auto &artifact : sourceBinding.sourceArtifacts)
        sourceArtifacts.append(QJsonObject{{QStringLiteral("path"), artifact.path},
                                           {QStringLiteral("sha256"), artifact.sha256}});
    return {{QStringLiteral("logicalSourcePath"), sourceBinding.logicalSourcePath},
            {QStringLiteral("resolvedSourcePath"), sourceBinding.resolvedSourcePath},
            {QStringLiteral("savedArtifactHash"), sourceBinding.savedArtifactHash},
            {QStringLiteral("sourceArtifacts"), sourceArtifacts}};
}

QString absoluteModelPath(const QString &fileName)
{
    if (fileName.isEmpty()) return {};
    return QFileInfo(RFileUtils::getFileNameWithOutTimeStep(fileName)).absoluteFilePath();
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

bool samePathText(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty()) return false;
    return QDir::cleanPath(QFileInfo(left).absoluteFilePath()).compare(
        QDir::cleanPath(QFileInfo(right).absoluteFilePath()), Qt::CaseInsensitive) == 0;
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

bool flushNativeRecoveryFile(QFile &file, QString *error)
{
    if (!file.flush())
    {
        if (error) *error = file.errorString();
        return false;
    }
#ifdef Q_OS_WIN
    const intptr_t nativeHandle = _get_osfhandle(int(file.handle()));
    if (nativeHandle == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle)))
    {
        if (error) *error = QStringLiteral("Could not flush the native recovery file to stable storage.");
        return false;
    }
#else
    if (::fsync(int(file.handle())) != 0)
    {
        if (error) *error = QStringLiteral("Could not flush the native recovery file to stable storage.");
        return false;
    }
#endif
    return true;
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonArray normalizedSolverArguments(const QStringList &arguments)
{
    QJsonArray normalized;
    for (const QString &argument : arguments)
    {
        if (argument.startsWith(QStringLiteral("--file=")))
            normalized.append(QStringLiteral("--file=<result-artifact>"));
        else if (argument.startsWith(QStringLiteral("--log-file=")))
            normalized.append(QStringLiteral("--log-file=<solver-log>"));
        else if (argument.startsWith(QStringLiteral("--convergence-file=")))
            normalized.append(QStringLiteral("--convergence-file=<convergence-log>"));
        else if (argument.startsWith(QStringLiteral("--monitoring-file=")))
            normalized.append(QStringLiteral("--monitoring-file=<monitoring-log>"));
        else if (argument.startsWith(QStringLiteral("--task-id=")))
            normalized.append(QStringLiteral("--task-id=<native-job-id>"));
        else if (argument.startsWith(QStringLiteral("--task-server=")))
            normalized.append(QStringLiteral("--task-server=<local-task-server>"));
        else
            normalized.append(argument);
    }
    return normalized;
}

QJsonObject nativeControlProvenance(const RStructuralControlSetup &setup,
                                    const QByteArray &inputSnapshotSha256,
                                    const QJsonArray &normalizedArguments)
{
    QString controllerVersionSha256;
    if (setup.enabled)
    {
        QJsonArray channelConditioning;
        for (const RStructuralControlChannelConditioning &channel : setup.channelConditioning)
        {
            channelConditioning.append(QJsonObject{
                {QStringLiteral("actuatorId"), channel.actuatorId},
                {QStringLiteral("delaySteps"), double(channel.delaySteps)},
                {QStringLiteral("holdSteps"), double(channel.holdSteps)},
                {QStringLiteral("noiseAmplitude"), channel.noiseAmplitude}});
        }
        const QJsonObject declaredController{
            {QStringLiteral("scope"), QStringLiteral("controller-declaration-and-input-snapshot/v1")},
            {QStringLiteral("controllerId"), setup.controllerId},
            {QStringLiteral("controlProtocolVersion"), double(setup.controlProtocolVersion)},
            {QStringLiteral("deterministicSeed"), QString::number(setup.deterministicSeed)},
            {QStringLiteral("channelConditioning"), channelConditioning},
            {QStringLiteral("inputSnapshotSha256"), QString::fromLatin1(inputSnapshotSha256.toHex())},
            {QStringLiteral("normalizedArguments"), normalizedArguments}};
        controllerVersionSha256 = sha256Hex(QJsonDocument(declaredController).toJson(QJsonDocument::Compact));
    }
    return {{QStringLiteral("enabled"), setup.enabled},
            {QStringLiteral("controllerId"), setup.enabled ? setup.controllerId : QString()},
            {QStringLiteral("controllerVersionSha256"), controllerVersionSha256}};
}

bool supportsNativeResultCatalog(const Model &model)
{
    const RProblemTypeMask problemTypes = model.getProblemTaskTree().getProblemTypeMask();
    const bool dynamics = model.getProblemSetup().getStructuralDynamicsSetup().getEnabled();
    const bool timeSolver = model.getTimeSolver().getEnabled();
    if (problemTypes == R_PROBLEM_STRESS_MODAL) return !dynamics;
    if (problemTypes != R_PROBLEM_STRESS) return false;
    // RSolver enables and configures its time solver from the validated
    // second-order dynamics setup before stepping, including fresh inputs
    // whose stored RTimeSolver is still disabled.
    if (dynamics) return true;
    return !timeSolver;
}

void setRecordNumber(RModel &model, uint recordNumber)
{
    const uint problemType = model.getProblemTaskTree().getProblemTypeMask();
    if ((problemType & R_PROBLEM_ACOUSTICS) &&
        model.getProblemSetup().getAcousticSetup().getAnalysisType() == R_ACOUSTIC_ANALYSIS_HARMONIC)
    {
        model.getProblemSetup().getAcousticSetup().setFrequencyIndex(recordNumber - 1);
    }
    else if (model.getTimeSolver().getEnabled())
    {
        model.getTimeSolver().setCurrentTimeStep(recordNumber - 1);
    }
    else if (problemType & R_PROBLEM_STRESS_MODAL)
    {
        model.getProblemSetup().getModalSetup().setMode(recordNumber - 1);
    }
}

bool stageViewFactorReferences(Model &inputModel,
                               Model &resultModel,
                               const QString &sourceModelFileName,
                               const QString &inputFileName,
                               const QString &resultFileName,
                               QString *error)
{
    if (!(inputModel.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_RADIATIVE_HEAT)) return true;

    const QString configuredName = inputModel.getProblemSetup().getRadiationSetup().getViewFactorMatrixFile();
    if (configuredName.isEmpty())
    {
        if (error) *error = QStringLiteral("Radiative heat input has no view-factor matrix path.");
        return false;
    }

    const QFileInfo configuredInfo(configuredName);
    const QString sourceDirectory = sourceModelFileName.isEmpty()
            ? QDir::currentPath()
            : QFileInfo(sourceModelFileName).absolutePath();
    const QString configuredAbsolute = configuredInfo.isAbsolute()
            ? configuredInfo.absoluteFilePath()
            : QDir(sourceDirectory).absoluteFilePath(configuredName);
    const QString sourceBase = RFileUtils::getFileNameWithOutTimeStep(configuredAbsolute);
    const QFileInfo sourceBaseInfo(sourceBase);
    const QString extension = sourceBaseInfo.suffix();
    if (extension.compare(RViewFactorMatrix::getDefaultFileExtension(false), Qt::CaseInsensitive) != 0 &&
        extension.compare(RViewFactorMatrix::getDefaultFileExtension(true), Qt::CaseInsensitive) != 0)
    {
        if (error) *error = QStringLiteral("Unsupported view-factor matrix reference: %1").arg(configuredName);
        return false;
    }

    const QString inputBase = QDir(QFileInfo(inputFileName).absolutePath())
            .filePath(QStringLiteral("references/") + sourceBaseInfo.fileName());
    const QString resultBase = QDir(QFileInfo(resultFileName).absolutePath())
            .filePath(QStringLiteral("references/") + sourceBaseInfo.fileName());
    const uint lastInputStep = inputModel.getTimeSolver().getEnabled()
            ? inputModel.getTimeSolver().getCurrentTimeStep()
            : 0;

    try
    {
        if (QFileInfo::exists(sourceBase))
        {
            RViewFactorMatrix matrix;
            matrix.read(sourceBase);
            matrix.write(inputBase);
            matrix.write(resultBase);
        }
        for (uint step = 0; step <= lastInputStep; ++step)
        {
            const QString sourceStep = RRadiationSetup::findRecentViewFactorMatrixFile(sourceBase, step);
            if (sourceStep.isEmpty()) continue;

            RViewFactorMatrix matrix;
            matrix.read(sourceStep);
            matrix.write(RFileUtils::getFileNameWithTimeStep(inputBase, step));
            matrix.write(RFileUtils::getFileNameWithTimeStep(resultBase, step));
        }
    }
    catch (const RError &readError)
    {
        if (error) *error = QStringLiteral("Could not snapshot view-factor matrix '%1': %2")
                .arg(configuredName, readError.getMessage());
        return false;
    }

    inputModel.getProblemSetup().getRadiationSetup().setViewFactorMatrixFile(inputBase);
    resultModel.getProblemSetup().getRadiationSetup().setViewFactorMatrixFile(resultBase);
    return true;
}

bool stageRestartHistory(const Model &model,
                         const QString &sourceModelFileName,
                         const QString &inputFileName,
                         QString *error)
{
    if (!model.getProblemSetup().getRestart() || sourceModelFileName.isEmpty()) return true;

    const QFileInfo sourceInfo(sourceModelFileName);
    const QString sourceBase = RFileUtils::getFileNameWithOutTimeStep(sourceModelFileName);
    const QString sourceExtension = QFileInfo(sourceBase).suffix();
    const QString sourceBaseName = QFileInfo(sourceBase).completeBaseName();
    const QString inputHistoryBase = QDir(QFileInfo(inputFileName).absolutePath())
            .filePath(QStringLiteral("history/") + QFileInfo(inputFileName).fileName());
    const QRegularExpression recordPattern(QStringLiteral("^") + QRegularExpression::escape(sourceBaseName) +
                                           QStringLiteral("-(\\d{5,})\\.") +
                                           QRegularExpression::escape(sourceExtension) + QStringLiteral("$"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QFileInfoList candidates = QDir(sourceInfo.absolutePath()).entryInfoList(
            QDir::Files | QDir::Readable, QDir::Name);

    QMap<uint, QString> sourceRecords;
    for (const QFileInfo &candidate : candidates)
    {
        const QRegularExpressionMatch match = recordPattern.match(candidate.fileName());
        if (!match.hasMatch()) continue;
        bool ok = false;
        const uint recordNumber = match.captured(1).toUInt(&ok);
        if (ok && recordNumber > 0) sourceRecords.insert(recordNumber, candidate.absoluteFilePath());
    }

    QJsonArray historyOrigins;
    const QString manifestPath = QDir(sourceInfo.absolutePath()).filePath(QStringLiteral("native-result.manifest.json"));
    const bool hasManifest = QFileInfo::exists(manifestPath);
    const QJsonObject registeredManifest = SolverManager::getInstance().nativeResultManifestForSource(sourceModelFileName);
    const bool knownRegisteredSource = registeredManifest.value(QStringLiteral("known")).toBool();
    const bool registeredManifestValid = registeredManifest.value(QStringLiteral("valid")).toBool();
    if (knownRegisteredSource && !registeredManifestValid)
    {
        if (error) *error = registeredManifest.value(QStringLiteral("error")).toString().isEmpty()
                ? QStringLiteral("The registered restart source has no verified terminal manifest.")
                : registeredManifest.value(QStringLiteral("error")).toString();
        return false;
    }
    const QString manifestHash = registeredManifestValid
            ? registeredManifest.value(QStringLiteral("resultManifestSha256")).toString()
            : QString::fromLatin1(sha256File(manifestPath).toHex());
    if (registeredManifestValid && (!hasManifest ||
        QFileInfo(manifestPath).canonicalFilePath() != QFileInfo(registeredManifest.value(QStringLiteral("resultManifestRef")).toString()).canonicalFilePath()))
    {
        if (error) *error = QStringLiteral("The registered restart manifest is missing or its path changed.");
        return false;
    }
    QJsonObject ancestor;
    if (!sourceRecords.isEmpty() && hasManifest)
    {
        const QJsonObject catalog = RNativeResultBroker::query(manifestPath, manifestHash, QStringLiteral("catalog"), {});
        QFile manifestFile(manifestPath);
        if (!catalog.value(QStringLiteral("valid")).toBool() || !manifestFile.open(QIODevice::ReadOnly) ||
            manifestFile.size() > 16 * 1024 * 1024)
        {
            if (error) *error = QStringLiteral("Restart history manifest could not be verified.");
            return false;
        }
        ancestor = QJsonDocument::fromJson(manifestFile.readAll()).object();
    }
    const QString restartHash = QString::fromLatin1(sha256File(sourceModelFileName).toHex());

    try
    {
        for (auto it = sourceRecords.cbegin(); it != sourceRecords.cend(); ++it)
        {
            const QString originalHash = QString::fromLatin1(sha256File(it.value()).toHex());
            QJsonObject history{{QStringLiteral("recordNo"), double(it.key())},
                                {QStringLiteral("originalFrameSha256"), originalHash}};
            if (hasManifest)
            {
                QJsonObject originalFrame;
                for (const QJsonValue &frame : ancestor.value(QStringLiteral("frames")).toArray())
                    if (frame.toObject().value(QStringLiteral("recordNo")).toDouble() == double(it.key()) &&
                        frame.toObject().value(QStringLiteral("sha256")).toString() == originalHash &&
                        frame.toObject().value(QStringLiteral("relativePath")).toString() == QFileInfo(it.value()).fileName())
                        originalFrame = frame.toObject();
                if (originalFrame.isEmpty())
                {
                    if (error) *error = QStringLiteral("Restart frame does not match its original result manifest.");
                    return false;
                }
                history.insert(QStringLiteral("originKind"), QStringLiteral("reused-history"));
                history.insert(QStringLiteral("origin"), originalFrame.value(QStringLiteral("origin")));
                history.insert(QStringLiteral("originManifestPath"), QFileInfo(manifestPath).canonicalFilePath());
                history.insert(QStringLiteral("originManifestSha256"), manifestHash);
                history.insert(QStringLiteral("ancestryValidationScope"), !registeredManifestValid
                               ? QStringLiteral("captured-source-input") : QStringLiteral("registered-native-job-manifest"));
            }
            else
            {
                history.insert(QStringLiteral("originKind"), QStringLiteral("legacy-history-unattributed"));
                history.insert(QStringLiteral("origin"), QJsonValue(QJsonValue::Null));
                history.insert(QStringLiteral("sourceRestartSha256"), restartHash);
                history.insert(QStringLiteral("ancestryValidationScope"), QStringLiteral("copied-restart-input-bytes"));
            }
            RModel record;
            record.read(it.value());
            setRecordNumber(record, it.key());
            record.write(RFileUtils::getFileNameWithTimeStep(inputHistoryBase, it.key()), true);
            if (QString::fromLatin1(sha256File(it.value()).toHex()) != originalHash)
            {
                if (error) *error = QStringLiteral("Restart frame changed during input capture.");
                return false;
            }
            historyOrigins.append(history);
        }
    }
    catch (const RError &readError)
    {
        if (error) *error = QStringLiteral("Could not snapshot restart record: %1").arg(readError.getMessage());
        return false;
    }
    if (hasManifest && !sourceRecords.isEmpty() && QString::fromLatin1(sha256File(manifestPath).toHex()) != manifestHash)
    {
        if (error) *error = QStringLiteral("Restart result manifest changed during input capture.");
        return false;
    }
    if (!historyOrigins.isEmpty())
    {
        QSaveFile index(QDir(QFileInfo(inputHistoryBase).absolutePath()).filePath(QStringLiteral("origin-index.json")));
        if (!index.open(QIODevice::WriteOnly) || index.write(QJsonDocument(historyOrigins).toJson(QJsonDocument::Compact)) < 0 || !index.commit())
        {
            if (error) *error = QStringLiteral("Restart history provenance index could not be captured.");
            return false;
        }
    }
    return true;
}

bool stageNativeRecoveryHistory(const Model &model,
                                const QJsonObject &recoveryProvenance,
                                const QString &inputFileName,
                                QString *error)
{
    const RStructuralDynamicsRestartData &restart =
            model.getProblemSetup().getStructuralDynamicsRestartData();
    const QString sourceJobId = recoveryProvenance.value(QStringLiteral("recoverySourceJobId")).toString();
    const QString sourceResult = recoveryProvenance.value(QStringLiteral("sourceResultArtifactRef")).toString();
    const QString journalPath = recoveryProvenance.value(QStringLiteral("sourceResultJournalRef")).toString();
    const QString expectedJournalSha = recoveryProvenance.value(QStringLiteral("sourceResultJournalSha256")).toString();
    const QString sourceInputSnapshotPath = recoveryProvenance.value(QStringLiteral("sourceInputSnapshotRef")).toString();
    const QString sourceInputSnapshotSha = recoveryProvenance.value(QStringLiteral("sourceInputSnapshotSha256")).toString();
    const QString sourceInputPackagePath = recoveryProvenance.value(QStringLiteral("sourceInputPackageRef")).toString();
    const QString sourceInputPackageSha = recoveryProvenance.value(QStringLiteral("sourceInputPackageSha256")).toString();
    const QFileInfo targetInputInfo(inputFileName);
    const QString targetInputPackagePath = targetInputInfo.absolutePath();
    const bool journalExpected = recoveryProvenance.value(QStringLiteral("sourceResultJournalPresent")).toBool();
    const QString selectedEnvelopeSha = recoveryProvenance.value(QStringLiteral("selectedEnvelopeSha256")).toString();
    const QString selectedCheckpointSha = recoveryProvenance.value(QStringLiteral("selectedCheckpointSha256")).toString();
    const double acceptedSeq = recoveryProvenance.value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
    const double checkpointTime = recoveryProvenance.value(QStringLiteral("checkpointTimeS")).toDouble(-1.0);
    const auto validSha = [](const QString &value)
    {
        return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(value).hasMatch();
    };
    if (!model.getProblemSetup().getRestart() || !restart.available || sourceJobId.isEmpty() ||
        !QFileInfo(sourceResult).isAbsolute() || !QFileInfo(journalPath).isAbsolute() ||
        !QFileInfo(sourceInputSnapshotPath).isAbsolute() || !QFileInfo(sourceInputPackagePath).isAbsolute() ||
        !QFileInfo(inputFileName).isAbsolute() || !QFileInfo(targetInputPackagePath).isDir() ||
        QFileInfo(targetInputPackagePath).isSymLink() || hasSymlinkComponent(targetInputPackagePath) ||
        !samePathText(targetInputPackagePath, QFileInfo(targetInputPackagePath).canonicalFilePath()) ||
        samePathText(targetInputPackagePath, sourceInputPackagePath) ||
        (journalExpected ? !validSha(expectedJournalSha) : !expectedJournalSha.isEmpty()) ||
        !validSha(sourceInputSnapshotSha) || !validSha(sourceInputPackageSha) ||
        !validSha(selectedEnvelopeSha) || !validSha(selectedCheckpointSha) ||
        !std::isfinite(acceptedSeq) || acceptedSeq < 1.0 || std::floor(acceptedSeq) != acceptedSeq ||
        quint64(acceptedSeq) != restart.completedSteps || !std::isfinite(checkpointTime) ||
        std::abs(checkpointTime - restart.time) > 1.0e-12 * qMax(1.0, std::abs(restart.time)))
    {
        if (error) *error = QStringLiteral("The detached recovery model or its selected checkpoint ancestry is invalid.");
        return false;
    }
    if (!samePathText(QFileInfo(sourceInputSnapshotPath).absolutePath(), sourceInputPackagePath) ||
        hasSymlinkComponent(sourceInputSnapshotPath) || hasSymlinkComponent(sourceInputPackagePath) ||
        QString::fromLatin1(sha256File(sourceInputSnapshotPath).toHex()) != sourceInputSnapshotSha ||
        QString::fromLatin1(sha256Directory(sourceInputPackagePath).toHex()) != sourceInputPackageSha)
    {
        if (error) *error = QStringLiteral("The interrupted job's frozen input package changed while recovery history was staged.");
        return false;
    }

    const QFileInfo sourceResultInfo(sourceResult);
    const QString sourceBase = RFileUtils::getFileNameWithOutTimeStep(sourceResult);
    const QFileInfo sourceBaseInfo(sourceBase);
    const QString resultDirectory = sourceBaseInfo.absolutePath();
    if (sourceBase.isEmpty() || sourceBaseInfo.suffix().isEmpty() ||
        !samePathText(sourceResultInfo.absolutePath(), resultDirectory) ||
        !samePathText(QFileInfo(journalPath).absolutePath(),
                      QFileInfo(QDir(resultDirectory).filePath(QStringLiteral("../events/native-results.ndjson"))).absolutePath()))
    {
        if (error) *error = QStringLiteral("The original native result-frame journal is outside the interrupted job run.");
        return false;
    }

    QMap<uint, QJsonObject> verifiedPrefix;
    QSet<uint> verifiedRecordNumbers;
    const QFileInfo journalInfo(journalPath);
    if (journalInfo.exists())
    {
        if (journalInfo.isSymLink() || !journalInfo.isFile() ||
            journalInfo.canonicalFilePath() != journalInfo.absoluteFilePath() || journalInfo.size() > 16 * 1024 * 1024)
        {
            if (error) *error = QStringLiteral("The original native result-frame journal is linked or exceeds its read bound.");
            return false;
        }
        if (!journalExpected)
        {
            if (error) *error = QStringLiteral("An original native result-frame journal appeared after recovery qualification.");
            return false;
        }
        const QByteArray journalSha = sha256File(journalPath);
        if (journalSha.size() != 32 || QString::fromLatin1(journalSha.toHex()) != expectedJournalSha)
        {
            if (error) *error = QStringLiteral("The original native result-frame journal changed after recovery qualification.");
            return false;
        }
        QFile journal(journalPath);
        if (!journal.open(QIODevice::ReadOnly))
        {
            if (error) *error = QStringLiteral("The original native result-frame journal could not be read.");
            return false;
        }
        const QByteArray bytes = journal.readAll();
        if (journal.error() != QFileDevice::NoError || (!bytes.isEmpty() && !bytes.endsWith('\n')))
        {
            if (error) *error = QStringLiteral("The original native result-frame journal ends with an incomplete record.");
            return false;
        }

        quint64 sequence = 0;
        qint64 verifiedFrameBytes = 0;
        double previousTime = -1.0;
        const QString expectedStem = sourceBaseInfo.completeBaseName();
        const QString expectedExtension = sourceBaseInfo.suffix();
        for (const QByteArray &line : bytes.split('\n'))
        {
            if (line.isEmpty()) continue;
            if (line.size() > 16384 || ++sequence > 10000)
            {
                if (error) *error = QStringLiteral("The original native result-frame journal exceeded its record bound.");
                return false;
            }
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            const QJsonObject frame = document.object();
            const double frameSequence = frame.value(QStringLiteral("seq")).toDouble(-1.0);
            const double recordNoValue = frame.value(QStringLiteral("recordNo")).toDouble(-1.0);
            const double timeS = frame.value(QStringLiteral("timeS")).toDouble(-1.0);
            const QString relativePath = frame.value(QStringLiteral("relativePath")).toString();
            const QString frameSha = frame.value(QStringLiteral("sha256")).toString();
            bool recordNoOk = false;
            recordNoOk = recordNoValue >= 1.0 && recordNoValue <= double(std::numeric_limits<uint>::max()) &&
                    std::floor(recordNoValue) == recordNoValue;
            const uint recordNo = recordNoOk ? uint(recordNoValue) : 0;
            const QString expectedFileName = QFileInfo(
                RFileUtils::getFileNameWithTimeStep(sourceBase, recordNo)).fileName();
            const QString framePath = QDir(resultDirectory).filePath(relativePath);
            const QFileInfo frameInfo(framePath);
            if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
                frame.value(QStringLiteral("protocolVersion")).toString() != QStringLiteral("range.native-result-frame/v1") ||
                frame.value(QStringLiteral("jobId")).toString() != sourceJobId ||
                frameSequence != double(sequence) || !recordNoOk || relativePath != expectedFileName ||
                QFileInfo(relativePath).fileName() != relativePath ||
                !QRegularExpression(QStringLiteral("^") + QRegularExpression::escape(expectedStem) +
                                    QStringLiteral("-\\d{5,}\\.") + QRegularExpression::escape(expectedExtension) +
                                    QStringLiteral("$")).match(relativePath).hasMatch() ||
                !std::isfinite(timeS) || timeS < 0.0 || timeS < previousTime ||
                !validSha(frameSha) || frameInfo.isSymLink() || !frameInfo.isFile() ||
                frameInfo.canonicalFilePath() != frameInfo.absoluteFilePath() ||
                frameInfo.size() <= 0 || frameInfo.size() > 256LL * 1024 * 1024 ||
                verifiedFrameBytes > 512LL * 1024 * 1024 - frameInfo.size() ||
                QString::fromLatin1(sha256File(framePath).toHex()) != frameSha || verifiedPrefix.contains(recordNo))
            {
                if (error) *error = QStringLiteral("The original native result-frame journal contains an invalid or duplicate frame binding.");
                return false;
            }
            verifiedFrameBytes += frameInfo.size();
            previousTime = timeS;
            verifiedRecordNumbers.insert(recordNo);
            const double tolerance = 1.0e-12 * qMax(1.0, std::abs(checkpointTime));
            if (recordNoValue <= acceptedSeq && timeS <= checkpointTime + tolerance)
                verifiedPrefix.insert(recordNo, frame);
        }
        if (QString::fromLatin1(sha256File(journalPath).toHex()) != expectedJournalSha)
        {
            if (error) *error = QStringLiteral("The original native result-frame journal changed while its prefix was staged.");
            return false;
        }
    }
    else if (journalExpected)
    {
        if (error) *error = QStringLiteral("The original native result-frame journal disappeared after recovery qualification.");
        return false;
    }

    QMap<uint, QJsonObject> inheritedOrigins;
    QMap<uint, QString> inheritedFramePaths;
    QMap<uint, QString> inheritedResultFramePaths;
    QMap<uint, double> inheritedFrameTimes;
    const QString sourceInputHistoryBase = QDir(sourceInputPackagePath)
            .filePath(QStringLiteral("history/") + QFileInfo(sourceInputSnapshotPath).fileName());
    const QString targetInputHistoryBase = QDir(targetInputPackagePath)
            .filePath(QStringLiteral("history/") + targetInputInfo.fileName());
    const QString sourceInputHistoryIndexPath = QDir(QFileInfo(sourceInputHistoryBase).absolutePath())
            .filePath(QStringLiteral("origin-index.json"));
    const QFileInfo inputHistoryIndexInfo(sourceInputHistoryIndexPath);
    if (inputHistoryIndexInfo.isSymLink() || hasSymlinkComponent(sourceInputHistoryIndexPath))
    {
        if (error) *error = QStringLiteral("The interrupted job's inherited history index is linked.");
        return false;
    }
    if (inputHistoryIndexInfo.exists())
    {
        if (inputHistoryIndexInfo.isSymLink() || !inputHistoryIndexInfo.isFile() ||
            hasSymlinkComponent(sourceInputHistoryIndexPath) ||
            inputHistoryIndexInfo.canonicalFilePath() != inputHistoryIndexInfo.absoluteFilePath() ||
            inputHistoryIndexInfo.size() > 16 * 1024 * 1024)
        {
            if (error) *error = QStringLiteral("The interrupted job's inherited history index is linked or exceeds its read bound.");
            return false;
        }
        QFile inputHistoryIndex(sourceInputHistoryIndexPath);
        if (!inputHistoryIndex.open(QIODevice::ReadOnly))
        {
            if (error) *error = QStringLiteral("The interrupted job's inherited history index could not be read.");
            return false;
        }
        const QByteArray indexBytes = inputHistoryIndex.readAll();
        const QJsonDocument indexDocument = QJsonDocument::fromJson(indexBytes);
        if (inputHistoryIndex.error() != QFileDevice::NoError || !indexDocument.isArray() ||
            indexDocument.array().size() > 10000)
        {
            if (error) *error = QStringLiteral("The interrupted job's inherited history index has an invalid schema or size.");
            return false;
        }
        for (const QJsonValue &value : indexDocument.array())
        {
            const QJsonObject origin = value.toObject();
            const double recordNoValue = origin.value(QStringLiteral("recordNo")).toDouble(-1.0);
            if (!value.isObject() || !std::isfinite(recordNoValue) || recordNoValue < 1.0 ||
                recordNoValue > double(std::numeric_limits<uint>::max()) || std::floor(recordNoValue) != recordNoValue ||
                !validSha(origin.value(QStringLiteral("originalFrameSha256")).toString()))
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history index contains an invalid frame proof.");
                return false;
            }
            const uint recordNo = uint(recordNoValue);
            const QString inheritedPath = RFileUtils::getFileNameWithTimeStep(sourceInputHistoryBase, recordNo);
            const QFileInfo inheritedInfo(inheritedPath);
            if (inheritedOrigins.contains(recordNo) || inheritedInfo.isSymLink() || !inheritedInfo.isFile() ||
                hasSymlinkComponent(inheritedPath) || inheritedInfo.canonicalFilePath() != inheritedInfo.absoluteFilePath())
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history frame is duplicated or unavailable.");
                return false;
            }
            const QString inheritedSha = QString::fromLatin1(sha256File(inheritedPath).toHex());
            if (!validSha(inheritedSha))
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history frame could not be hashed.");
                return false;
            }
            RModel inheritedModel;
            try
            {
                inheritedModel.read(inheritedPath);
            }
            catch (const RError &readError)
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history frame could not be decoded: %1")
                        .arg(readError.getMessage());
                return false;
            }
            catch (const std::exception &readError)
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history frame could not be decoded: %1")
                        .arg(QString::fromLocal8Bit(readError.what()));
                return false;
            }
            const RStructuralDynamicsRestartData &inheritedRestart =
                    inheritedModel.getProblemSetup().getStructuralDynamicsRestartData();
            const double inheritedTime = inheritedModel.getTimeSolver().getCurrentTime();
            try { inheritedRestart.validate(); }
            catch (const RError &validationError)
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history frame has invalid accepted state: %1")
                        .arg(validationError.getMessage());
                return false;
            }
            if (!inheritedRestart.available || inheritedRestart.completedSteps != recordNo ||
                !std::isfinite(inheritedTime) || inheritedRestart.meshFingerprint != restart.meshFingerprint ||
                inheritedRestart.dofFingerprint != restart.dofFingerprint ||
                inheritedRestart.systemFingerprint != restart.systemFingerprint ||
                std::abs(inheritedRestart.time - inheritedTime) > 1.0e-12 * qMax(1.0, std::abs(inheritedTime)) ||
                std::abs(inheritedRestart.fixedTimeStep - restart.fixedTimeStep) >
                    1.0e-12 * qMax(1.0, std::abs(restart.fixedTimeStep)))
            {
                if (error) *error = QStringLiteral("The interrupted job's inherited history frame does not match its pinned structural system.");
                return false;
            }
            inheritedOrigins.insert(recordNo, origin);
            inheritedFramePaths.insert(recordNo, inheritedPath);
            inheritedFrameTimes.insert(recordNo, inheritedTime);
        }
    }

    const QRegularExpression sourceRecordPattern(QStringLiteral("^") +
        QRegularExpression::escape(sourceBaseInfo.completeBaseName()) + QStringLiteral("-(\\d{5,})\\.") +
        QRegularExpression::escape(sourceBaseInfo.suffix()) + QStringLiteral("$"),
        QRegularExpression::CaseInsensitiveOption);
    const QFileInfoList sourceFiles = QDir(resultDirectory).entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &sourceFile : sourceFiles)
    {
        const QRegularExpressionMatch match = sourceRecordPattern.match(sourceFile.fileName());
        if (!match.hasMatch()) continue;
        bool recordNoOk = false;
        const uint recordNo = match.captured(1).toUInt(&recordNoOk);
        if (!recordNoOk)
        {
            if (error) *error = QStringLiteral("An original numbered result frame has an invalid record number.");
            return false;
        }
        if (verifiedRecordNumbers.contains(recordNo) && inheritedOrigins.contains(recordNo))
        {
            if (error) *error = QStringLiteral("A numbered result frame is claimed by both the native journal and inherited input history.");
            return false;
        }
        if (!verifiedRecordNumbers.contains(recordNo) && inheritedOrigins.contains(recordNo))
        {
            const QString inputFrameSha = QString::fromLatin1(sha256File(inheritedFramePaths.value(recordNo)).toHex());
            const QString resultFrameSha = QString::fromLatin1(sha256File(sourceFile.absoluteFilePath()).toHex());
            if (inputFrameSha.isEmpty() || inputFrameSha != resultFrameSha)
            {
                if (error) *error = QStringLiteral("An inherited result frame no longer matches its frozen input-package copy.");
                return false;
            }
            verifiedRecordNumbers.insert(recordNo);
            inheritedResultFramePaths.insert(recordNo, sourceFile.absoluteFilePath());
            continue;
        }
        if (!verifiedRecordNumbers.contains(recordNo))
        {
            if (error) *error = QStringLiteral(
                "An original numbered result frame has no verified native result-journal entry "
                "(sourceFrame=%1, recordNo=%2, sourceResultBase=%3, targetInput=%4, verifiedJournalFrames=%5).")
                    .arg(sourceFile.absoluteFilePath(), QString::number(recordNo), sourceBase,
                         QFileInfo(inputFileName).absoluteFilePath(),
                         QString::number(verifiedRecordNumbers.size()));
            return false;
        }
    }

    QJsonArray historyOrigins;
    try
    {
        for (auto it = verifiedPrefix.cbegin(); it != verifiedPrefix.cend(); ++it)
        {
            const QJsonObject frame = it.value();
            const QString sourceFrame = QDir(resultDirectory).filePath(frame.value(QStringLiteral("relativePath")).toString());
            const QString sourceFrameSha = frame.value(QStringLiteral("sha256")).toString();
            RModel record;
            record.read(sourceFrame);
            setRecordNumber(record, it.key());
            const QString inputHistoryPath = RFileUtils::getFileNameWithTimeStep(targetInputHistoryBase, it.key());
            record.write(inputHistoryPath);
            if (QString::fromLatin1(sha256File(sourceFrame).toHex()) != sourceFrameSha)
            {
                if (error) *error = QStringLiteral("An original result frame changed while its recovery prefix was staged.");
                return false;
            }
            const QJsonObject prefixOrigin{{QStringLiteral("sourceJobId"), sourceJobId},
                                           {QStringLiteral("sourceResultJournalSha256"), expectedJournalSha},
                                           {QStringLiteral("sourceFrameSequence"), frame.value(QStringLiteral("seq"))},
                                           {QStringLiteral("sourceRecordNo"), double(it.key())},
                                           {QStringLiteral("sourceTimeS"), frame.value(QStringLiteral("timeS"))},
                                           {QStringLiteral("sourceFrameSha256"), sourceFrameSha},
                                           {QStringLiteral("selectedEnvelopeSha256"), selectedEnvelopeSha},
                                           {QStringLiteral("acceptedSeq"), acceptedSeq}};
            historyOrigins.append(QJsonObject{{QStringLiteral("recordNo"), double(it.key())},
                                               {QStringLiteral("originalFrameSha256"), sourceFrameSha},
                                               {QStringLiteral("originKind"), QStringLiteral("legacy-history-unattributed")},
                                               {QStringLiteral("origin"), QJsonValue(QJsonValue::Null)},
                                               {QStringLiteral("sourceRestartSha256"),
                                                selectedCheckpointSha},
                                               {QStringLiteral("recoveryPrefixOrigin"), prefixOrigin},
                                               {QStringLiteral("ancestryValidationScope"),
                                                QStringLiteral("selected-checkpoint-native-result-journal")}});
        }
        for (auto it = inheritedResultFramePaths.cbegin(); it != inheritedResultFramePaths.cend(); ++it)
        {
            const uint recordNo = it.key();
            const double frameTime = inheritedFrameTimes.value(recordNo, -1.0);
            const double tolerance = 1.0e-12 * qMax(1.0, std::abs(checkpointTime));
            if (double(recordNo) > acceptedSeq || frameTime < 0.0 || frameTime > checkpointTime + tolerance)
                continue;
            const QString sourceFrame = it.value();
            const QString sourceFrameSha = QString::fromLatin1(sha256File(sourceFrame).toHex());
            if (!validSha(sourceFrameSha) || sourceFrameSha !=
                    QString::fromLatin1(sha256File(inheritedFramePaths.value(recordNo)).toHex()))
            {
                if (error) *error = QStringLiteral("An inherited result-prefix frame changed after package verification.");
                return false;
            }
            RModel inheritedFrame;
            inheritedFrame.read(sourceFrame);
            setRecordNumber(inheritedFrame, recordNo);
            const QString inputHistoryPath = RFileUtils::getFileNameWithTimeStep(targetInputHistoryBase, recordNo);
            inheritedFrame.write(inputHistoryPath);
            if (QString::fromLatin1(sha256File(sourceFrame).toHex()) != sourceFrameSha)
            {
                if (error) *error = QStringLiteral("An inherited result-prefix frame changed while it was staged.");
                return false;
            }
            QJsonObject inheritedOrigin = inheritedOrigins.value(recordNo);
            inheritedOrigin.insert(QStringLiteral("recoveryInheritance"), QJsonObject{
                {QStringLiteral("sourceJobId"), sourceJobId},
                {QStringLiteral("sourceInputPackageSha256"), sourceInputPackageSha},
                {QStringLiteral("sourceInputFrameSha256"), sourceFrameSha},
                {QStringLiteral("sourceResultJournalSha256"), expectedJournalSha},
                {QStringLiteral("selectedEnvelopeSha256"), selectedEnvelopeSha},
                {QStringLiteral("acceptedSeq"), acceptedSeq}});
            historyOrigins.append(inheritedOrigin);
        }
    }
    catch (const RError &readError)
    {
        if (error) *error = QStringLiteral("Could not stage the verified native recovery result prefix: %1")
                .arg(readError.getMessage());
        return false;
    }
    catch (const std::exception &readError)
    {
        if (error) *error = QStringLiteral("Could not stage the verified native recovery result prefix: %1")
                .arg(QString::fromLocal8Bit(readError.what()));
        return false;
    }
    if (!historyOrigins.isEmpty())
    {
        QSaveFile index(QDir(QFileInfo(targetInputHistoryBase).absolutePath()).filePath(QStringLiteral("origin-index.json")));
        if (!index.open(QIODevice::WriteOnly) ||
            index.write(QJsonDocument(historyOrigins).toJson(QJsonDocument::Compact)) < 0 || !index.commit())
        {
            if (error) *error = QStringLiteral("Recovery result-prefix provenance could not be staged.");
            return false;
        }
    }
    if (QString::fromLatin1(sha256File(sourceInputSnapshotPath).toHex()) != sourceInputSnapshotSha ||
        QString::fromLatin1(sha256Directory(sourceInputPackagePath).toHex()) != sourceInputPackageSha)
    {
        if (error) *error = QStringLiteral("The interrupted job's frozen input package changed while recovery history was staged.");
        return false;
    }
    return true;
}

bool copyRestartHistoryToResult(const QString &inputFileName,
                                const QString &resultFileName,
                                QJsonArray *reusedHistory,
                                QString *error)
{
    const QFileInfo inputInfo(inputFileName);
    const QString inputBaseName = inputInfo.completeBaseName();
    const QString inputExtension = inputInfo.suffix();
    const QString inputHistoryBase = QDir(inputInfo.absolutePath())
            .filePath(QStringLiteral("history/") + inputInfo.fileName());
    const QFileInfo historyBaseInfo(inputHistoryBase);
    const QRegularExpression recordPattern(QStringLiteral("^") + QRegularExpression::escape(inputBaseName) +
                                           QStringLiteral("-(\\d{5,})\\.") +
                                           QRegularExpression::escape(inputExtension) + QStringLiteral("$"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QFileInfoList candidates = QDir(historyBaseInfo.absolutePath()).entryInfoList(
            QDir::Files | QDir::Readable, QDir::Name);
    QMap<uint, QString> inputRecords;
    for (const QFileInfo &candidate : candidates)
    {
        const QRegularExpressionMatch match = recordPattern.match(candidate.fileName());
        if (!match.hasMatch()) continue;
        bool ok = false;
        const uint recordNumber = match.captured(1).toUInt(&ok);
        if (ok && recordNumber > 0) inputRecords.insert(recordNumber, candidate.absoluteFilePath());
    }
    QJsonArray origins;
    if (!inputRecords.isEmpty())
    {
        QFile index(QDir(historyBaseInfo.absolutePath()).filePath(QStringLiteral("origin-index.json")));
        if (!index.open(QIODevice::ReadOnly) || index.size() > 16 * 1024 * 1024)
        {
            if (error) *error = QStringLiteral("Restart history has no captured provenance index.");
            return false;
        }
        origins = QJsonDocument::fromJson(index.readAll()).array();
    }

    try
    {
        for (auto it = inputRecords.cbegin(); it != inputRecords.cend(); ++it)
        {
            RModel record;
            record.read(it.value());
            setRecordNumber(record, it.key());
            const QString copiedPath = RFileUtils::getFileNameWithTimeStep(resultFileName, it.key());
            record.write(copiedPath, true);
            QJsonObject origin;
            for (const QJsonValue &entry : origins)
                if (entry.toObject().value(QStringLiteral("recordNo")).toDouble() == double(it.key())) origin = entry.toObject();
            if (origin.isEmpty())
            {
                if (error) *error = QStringLiteral("Restart record has no provenance entry.");
                return false;
            }
            origin.insert(QStringLiteral("relativePath"), QFileInfo(copiedPath).fileName());
            origin.insert(QStringLiteral("sha256"), QString::fromLatin1(sha256File(copiedPath).toHex()));
            reusedHistory->append(origin);
        }
    }
    catch (const RError &readError)
    {
        if (error) *error = QStringLiteral("Could not prepare restart result records: %1").arg(readError.getMessage());
        return false;
    }
    return true;
}
}

SolverTask::SolverTask(const ApplicationSettings *applicationSettings, uint modelID)
    : applicationSettings{applicationSettings}
    , modelID{modelID}
    , solverProcess{nullptr}
{
    this->setBlocking(false);
    this->taskID.generate();
    SolverManager &solverManager = SolverManager::getInstance();
    if (!solverManager.ensureUniqueNativeTaskId(&this->taskID))
        RLogger::error("Could not reserve an unused persistent native solver task ID.\n");
    this->solverExecutable = applicationSettings->getSolverPath();

    Session *session = Application::instance()->getSession();
    Model &rModel = session->getModel(this->modelID);
    this->sourceDocumentId = session->modelCommandStamp(this->modelID).first;

    this->initializeTaskPaths(rModel);
}

SolverTask::SolverTask(const ApplicationSettings *applicationSettings, const Model &model,
                       const QPair<QString, quint64> &sourceStamp,
                       const QJsonObject &recoveryProvenance,
                       const QString &artifactRoot,
                       const QString &taskServerName,
                       std::shared_ptr<QLockFile> logicalRecoveryLock)
    : applicationSettings{applicationSettings}
    , modelID{RConstants::eod}
    , sourceDocumentId{sourceStamp.first}
    , solverProcess{nullptr}
    , nativeRecoveryProvenance{recoveryProvenance}
    , nativeRecoveryArtifactRoot{QFileInfo(artifactRoot).absoluteFilePath()}
    , nativeRecoveryLogicalLock{std::move(logicalRecoveryLock)}
{
    this->setBlocking(false);
    this->taskID.generate();
    SolverManager &solverManager = SolverManager::getInstance();
    if (!solverManager.ensureUniqueNativeTaskId(&this->taskID))
        RLogger::error("Could not reserve an unused persistent native solver task ID.\n");
    this->initializeTaskPaths(model, this->nativeRecoveryArtifactRoot, taskServerName);
}

void SolverTask::initializeTaskPaths(const Model &rModel, const QString &artifactRoot,
                                     const QString &taskServerName)
{
    this->solverExecutable = this->applicationSettings->getSolverPath();

    this->modelFileName = makeRunArtifactFileName(rModel, this->applicationSettings, this->taskID,
                                                  artifactRoot, this->isNativeRecoveryTask());
    this->inputSnapshotFileName = this->modelFileName;
    this->resultArtifactFileName = this->modelFileName;

    const QFileInfo artifactInfo(this->modelFileName);
    const QString artifactDirectory = QDir(artifactInfo.absolutePath()).absoluteFilePath(QStringLiteral(".."));
    const QString artifactBaseName = artifactInfo.completeBaseName();
    const QString extension = artifactInfo.suffix();
    this->inputSnapshotFileName = QDir(artifactDirectory).filePath(QStringLiteral("input/") + artifactBaseName + "." + extension);
    const QString logDirectory = QDir(artifactDirectory).filePath(QStringLiteral("logs"));
    this->logFileName = QDir(logDirectory).filePath(artifactBaseName + ".log");
    this->convergenceFileName = QDir(logDirectory).filePath(artifactBaseName + ".cvg");
    this->monitoringFileName = QDir(logDirectory).filePath(artifactBaseName + ".mon");
    this->nativeProgressFileName = QDir(artifactDirectory)
            .filePath(QStringLiteral("events/native-progress.ndjson"));
    this->nativeResultJournalFileName = QDir(artifactDirectory)
            .filePath(QStringLiteral("events/native-results.ndjson"));

    this->solverArguments.append("--file=" + this->modelFileName);
    this->solverArguments.append("--log-file=" + this->logFileName);
    this->solverArguments.append("--convergence-file=" + this->convergenceFileName);
    this->solverArguments.append("--monitoring-file=" + this->monitoringFileName);
    const QString selectedTaskServerName = taskServerName.isEmpty()
            ? SolverManager::getInstance().getTaskServerName() : taskServerName;
    this->solverArguments.append("--nthreads=" + QString::number(this->applicationSettings->getNThreads()));
    this->solverArguments.append("--task-id=" + this->taskID.toString());
    this->solverArguments.append("--task-server=" + selectedTaskServerName);
    if (this->isNativeRecoveryTask()) this->solverArguments.append(QStringLiteral("--restart"));
}

bool SolverTask::captureInputSnapshot(const Model &model,
                                      const QPair<QString, quint64> &sourceStamp,
                                      uint currentModelID,
                                      const QString &inputDerivedFrom,
                                      const QString &frozenSnapshotPath,
                                      const QString &frozenSnapshotSha256)
{
    return this->captureInputSnapshot(std::make_unique<Model>(model), sourceStamp, currentModelID,
                                     inputDerivedFrom, frozenSnapshotPath, frozenSnapshotSha256);
}

bool SolverTask::captureInputSnapshot(std::unique_ptr<Model> modelSnapshot,
                                      const QPair<QString, quint64> &sourceStamp,
                                      uint currentModelID,
                                      const QString &inputDerivedFrom,
                                      const QString &frozenSnapshotPath,
                                      const QString &frozenSnapshotSha256)
{
    const QString derivedFrom = inputDerivedFrom.isEmpty()
            ? QStringLiteral("live_unlinked") : inputDerivedFrom;
    const bool frozenVerified = derivedFrom == QStringLiteral("frozen_project_snapshot") &&
            !frozenSnapshotPath.isEmpty() &&
            QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(frozenSnapshotSha256).hasMatch();
    const bool validUnlinked = derivedFrom == QStringLiteral("live_unlinked") &&
            frozenSnapshotPath.isEmpty() && frozenSnapshotSha256.isEmpty();
    if (QThread::currentThread() != this->thread() ||
        sourceStamp.first.isEmpty() || sourceStamp.first != this->sourceDocumentId ||
        !this->sourceStamp.first.isEmpty() || !modelSnapshot ||
        (!frozenVerified && !validUnlinked))
    {
        return false;
    }

    this->sourceModelFileName = absoluteModelPath(modelSnapshot->getFileName());
    this->inputSnapshot = std::move(modelSnapshot);
    this->resultSnapshot = std::make_unique<Model>(*this->inputSnapshot);

    const QString runRoot = QDir::cleanPath(getRunRoot(this->resultArtifactFileName));
    const QString inputDirectory = QFileInfo(this->inputSnapshotFileName).absolutePath();
    const QString resultDirectory = QFileInfo(this->resultArtifactFileName).absolutePath();
    const QString logDirectory = QFileInfo(this->logFileName).absolutePath();
    if (!QDir().mkpath(inputDirectory) || !QDir().mkpath(resultDirectory) ||
        !QDir().mkpath(logDirectory) ||
        !QDir().mkpath(QDir(inputDirectory).filePath(QStringLiteral("references"))) ||
        !QDir().mkpath(QDir(resultDirectory).filePath(QStringLiteral("references"))) ||
        !QDir().mkpath(QDir(inputDirectory).filePath(QStringLiteral("history"))))
    {
        RLogger::error("Could not create the solver snapshot artifact directories.\n");
        this->inputSnapshot.reset();
        this->resultSnapshot.reset();
        return false;
    }

    QString dependencyError;
    const bool restartHistoryStaged = this->isNativeRecoveryTask()
            ? stageNativeRecoveryHistory(*this->inputSnapshot, this->nativeRecoveryProvenance,
                                         this->inputSnapshotFileName, &dependencyError)
            : stageRestartHistory(*this->inputSnapshot, this->sourceModelFileName,
                                  this->inputSnapshotFileName, &dependencyError);
    if (!stageViewFactorReferences(*this->inputSnapshot,
                                   *this->resultSnapshot,
                                   this->sourceModelFileName,
                                   this->inputSnapshotFileName,
                                   this->resultArtifactFileName,
                                   &dependencyError) ||
        !restartHistoryStaged)
    {
        RLogger::error("Could not capture solver input dependencies: %s\n", dependencyError.toUtf8().constData());
        this->inputSnapshot.reset();
        this->resultSnapshot.reset();
        return false;
    }

    this->inputSnapshot->setFileName(this->inputSnapshotFileName);
    this->resultSnapshot->setFileName(this->resultArtifactFileName);

    try
    {
        ModelIO::save(*this->inputSnapshot, this->inputSnapshotFileName, false);
    }
    catch (const RError &error)
    {
        RLogger::error("Could not serialize solver input snapshot '%s': %s\n",
                       this->inputSnapshotFileName.toUtf8().constData(),
                       error.getMessage().toUtf8().constData());
        this->inputSnapshot.reset();
        this->resultSnapshot.reset();
        return false;
    }

    this->inputSnapshotSha256 = sha256File(this->inputSnapshotFileName);
    this->inputPackageSha256 = sha256Directory(inputDirectory);
    if (this->inputSnapshotSha256.isEmpty() || this->inputPackageSha256.isEmpty() ||
        !writeProvenance(runRoot,
                         this->taskID,
                         sourceStamp,
                         this->sourceModelFileName,
                         this->inputSnapshotFileName,
                         this->inputSnapshotSha256,
                         this->inputPackageSha256))
    {
        RLogger::error("Could not hash or write provenance for solver input snapshot '%s'.\n",
                       this->inputSnapshotFileName.toUtf8().constData());
        this->inputSnapshot.reset();
        this->resultSnapshot.reset();
        return false;
    }

    this->modelID = currentModelID;
    this->sourceStamp = sourceStamp;
    this->inputDerivedFrom = derivedFrom;
    this->frozenSnapshotPath = frozenVerified ? QFileInfo(frozenSnapshotPath).absoluteFilePath() : QString();
    this->frozenSnapshotSha256 = frozenVerified ? frozenSnapshotSha256 : QString();
    this->frozenSnapshotVerified = frozenVerified;
    return true;
}

std::unique_ptr<Model> SolverTask::copyCapturedInputForValidation() const
{
    if (QThread::currentThread() != this->thread() ||
        this->performStarted.load(std::memory_order_acquire) || !this->inputSnapshot)
        return {};
    return std::make_unique<Model>(*this->inputSnapshot);
}

const QString &SolverTask::getInputDerivedFrom() const
{
    return this->inputDerivedFrom;
}

const QString &SolverTask::getFrozenSnapshotPath() const
{
    return this->frozenSnapshotPath;
}

const QString &SolverTask::getFrozenSnapshotSha256() const
{
    return this->frozenSnapshotSha256;
}

bool SolverTask::getFrozenSnapshotVerified() const
{
    return this->frozenSnapshotVerified;
}

const QJsonObject &SolverTask::getNativeRecoveryProvenance() const
{
    return this->nativeRecoveryProvenance;
}

bool SolverTask::isNativeRecoveryTask() const
{
    return !this->nativeRecoveryProvenance.isEmpty();
}

bool SolverTask::hasNativeRecoveryLogicalLock() const
{
    return this->isNativeRecoveryTask() && this->nativeRecoveryLogicalLock &&
            this->nativeRecoveryLogicalLock->isLocked();
}

void SolverTask::releaseNativeRecoveryLogicalLock()
{
    if (this->nativeRecoveryLogicalLock && this->nativeRecoveryLogicalLock->isLocked())
        this->nativeRecoveryLogicalLock->unlock();
}

const QString &SolverTask::getSourceDocumentId() const
{
    return this->sourceDocumentId;
}

const QPair<QString, quint64> &SolverTask::getSourceStamp() const
{
    return this->sourceStamp;
}

const QString &SolverTask::getSourceModelFileName() const
{
    return this->sourceModelFileName;
}

const QString &SolverTask::getInputSnapshotFileName() const
{
    return this->inputSnapshotFileName;
}

const QByteArray &SolverTask::getInputSnapshotSha256() const
{
    return this->inputSnapshotSha256;
}

const QByteArray &SolverTask::getInputPackageSha256() const
{
    return this->inputPackageSha256;
}

bool SolverTask::freezeNativeResultProvenance(const QJsonObject &sourceRuntimeStamp,
                                             const QJsonObject &sourceSnapshotRef)
{
    if (QThread::currentThread() != this->thread() || this->performStarted.load(std::memory_order_acquire) ||
        this->nativeResultProvenanceFrozen || !this->inputSnapshot || sourceRuntimeStamp.isEmpty() ||
        sourceSnapshotRef.isEmpty() || this->inputSnapshotSha256.size() != 32 ||
        this->inputPackageSha256.size() != 32)
    {
        return false;
    }

    const QByteArray executableSha256 = sha256File(this->solverExecutable);
    if (executableSha256.size() != 32) return false;

    QJsonObject control;
    QJsonObject runtimeDescriptor;
    try
    {
        const QJsonArray normalizedArguments = normalizedSolverArguments(this->solverArguments);
        control = nativeControlProvenance(this->inputSnapshot->getProblemSetup().getStructuralControlSetup(),
                                          this->inputSnapshotSha256,
                                          normalizedArguments);
        runtimeDescriptor = QJsonObject{
            {QStringLiteral("solverExecutableSha256"), QString::fromLatin1(executableSha256.toHex())},
            {QStringLiteral("applicationVersion"), QString::fromLatin1(VERSION)},
            {QStringLiteral("nativeModelFormatVersion"), RModel::version.toString()},
            {QStringLiteral("qtCompileVersion"), QString::fromLatin1(QT_VERSION_STR)},
            {QStringLiteral("qtRuntimeVersion"), QString::fromLatin1(qVersion())},
            {QStringLiteral("platform"), QJsonObject{
                {QStringLiteral("productType"), QSysInfo::productType()},
                {QStringLiteral("kernelType"), QSysInfo::kernelType()},
                {QStringLiteral("buildAbi"), QSysInfo::buildAbi()},
                {QStringLiteral("currentCpuArchitecture"), QSysInfo::currentCpuArchitecture()}}},
            {QStringLiteral("normalizedArguments"), normalizedArguments},
            {QStringLiteral("control"), control}};
    }
    catch (...)
    {
        return false;
    }

    const QString runtimeFingerprint = sha256Hex(QJsonDocument(runtimeDescriptor).toJson(QJsonDocument::Compact));
    const QString proofScope = QStringLiteral("solver-and-declared-controller-runtime/v1");
    QJsonObject provenance{
        {QStringLiteral("jobId"), this->taskID.toString()},
        {QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp},
        {QStringLiteral("sourceSnapshotRef"), sourceSnapshotRef},
        {QStringLiteral("inputSnapshotSha256"), QString::fromLatin1(this->inputSnapshotSha256.toHex())},
        {QStringLiteral("inputPackageSha256"), QString::fromLatin1(this->inputPackageSha256.toHex())},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("fingerprintSha256"), runtimeFingerprint},
            {QStringLiteral("scope"), proofScope}}},
        {QStringLiteral("control"), control},
        {QStringLiteral("proofScope"), proofScope}};
    if (this->isNativeRecoveryTask())
        provenance.insert(QStringLiteral("recovery"), this->nativeRecoveryProvenance);

    this->nativeResultProvenance = provenance;
    this->frozenSolverExecutableSha256 = executableSha256;
    this->nativeResultProvenanceFrozen = true;
    return true;
}

bool SolverTask::prepareNativeCheckpointInput(QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != this->thread() ||
        this->performStarted.load(std::memory_order_acquire) || !this->inputSnapshot ||
        !this->resultSnapshot || this->sourceStamp.first.isEmpty() ||
        !this->nativeCheckpointInputModelFileName.isEmpty())
    {
        if (error) *error = QStringLiteral("The detached solver input is unavailable for checkpoint preparation.");
        return false;
    }
    const QString inputDirectory = QFileInfo(this->inputSnapshotFileName).absolutePath();
    const QString extension = QFileInfo(this->resultArtifactFileName).suffix();
    if (extension.compare(RModel::getDefaultFileExtension(false), Qt::CaseInsensitive) != 0 &&
        extension.compare(RModel::getDefaultFileExtension(true), Qt::CaseInsensitive) != 0)
    {
        if (error) *error = QStringLiteral("The solver input extension is unsupported for accepted-state checkpoints.");
        return false;
    }
    const QString frozenModelPath = QDir(inputDirectory).filePath(
        QStringLiteral("native-checkpoint-input.") + extension);
    if (QFileInfo(frozenModelPath).exists() || QFileInfo(frozenModelPath).isSymLink())
    {
        if (error) *error = QStringLiteral("The immutable checkpoint input path already exists.");
        return false;
    }

    QTemporaryDir stagingDirectory(QDir(inputDirectory).filePath(QStringLiteral(".checkpoint-input-XXXXXX")));
    if (!stagingDirectory.isValid())
    {
        if (error) *error = QStringLiteral("A private checkpoint input staging directory could not be created.");
        return false;
    }
    const QString stagingPath = QDir(stagingDirectory.path()).filePath(
        QStringLiteral("solver-input.") + extension);
    try
    {
        Model preparedResult(*this->resultSnapshot);
        preparedResult.setFileName(this->resultArtifactFileName);
        ModelIO::save(preparedResult, stagingPath, false);
    }
    catch (const RError &saveError)
    {
        if (error) *error = QStringLiteral("The exact native solver input could not be staged: %1")
                .arg(saveError.getMessage());
        return false;
    }
    catch (const std::exception &saveError)
    {
        if (error) *error = QStringLiteral("The exact native solver input could not be staged: %1")
                .arg(QString::fromLocal8Bit(saveError.what()));
        return false;
    }
    const QByteArray modelHash = sha256File(stagingPath);
    if (modelHash.size() != 32 || !QFile::rename(stagingPath, frozenModelPath) ||
        sha256File(frozenModelPath) != modelHash)
    {
        if (error) *error = QStringLiteral("The staged native solver input failed hashing or atomic publication.");
        QFile::remove(frozenModelPath);
        QFile::remove(stagingPath);
        return false;
    }

    const QByteArray packageHash = sha256Directory(inputDirectory);
    if (packageHash.isEmpty() || !writeProvenance(getRunRoot(this->resultArtifactFileName),
                                                   this->taskID, this->sourceStamp,
                                                   this->sourceModelFileName, this->inputSnapshotFileName,
                                                   this->inputSnapshotSha256, packageHash))
    {
        QFile::remove(frozenModelPath);
        if (error) *error = QStringLiteral("The checkpoint model package could not be rebound to its durable provenance record.");
        return false;
    }
    this->nativeCheckpointInputModelFileName = QFileInfo(frozenModelPath).absoluteFilePath();
    this->nativeCheckpointModelSha256 = modelHash;
    this->inputPackageSha256 = packageHash;
    return true;
}

const QString &SolverTask::getNativeCheckpointInputModelFileName() const
{
    return this->nativeCheckpointInputModelFileName;
}

const QByteArray &SolverTask::getNativeCheckpointModelSha256() const
{
    return this->nativeCheckpointModelSha256;
}

bool SolverTask::enableNativeAcceptedCheckpoints(const QString &directory,
                                                const QByteArray &canonicalContextJson,
                                                QString *error)
{
    if (error) error->clear();
    QMutexLocker<QMutex> configurationLocker(&this->nativeExecutionConfigurationMutex);
    if (QThread::currentThread() != this->thread() ||
        this->performStarted.load(std::memory_order_acquire) ||
        !QFileInfo(directory).isAbsolute() || canonicalContextJson.isEmpty() ||
        canonicalContextJson.size() > 16 * 1024)
        return false;
    for (const QString &argument : this->solverArguments)
        if (argument.startsWith(QStringLiteral("--native-checkpoint-dir=")) ||
            argument.startsWith(QStringLiteral("--native-checkpoint-context-json=")))
            return false;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(canonicalContextJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("schema")).toString() !=
            QStringLiteral("range.native-checkpoint-context/v1") ||
        document.object().value(QStringLiteral("modelSha256")).toString() !=
            QString::fromLatin1(this->nativeCheckpointModelSha256.toHex()) ||
        document.object().value(QStringLiteral("packageSha256")).toString() !=
            QString::fromLatin1(sha256Directory(QFileInfo(this->inputSnapshotFileName).absolutePath()).toHex()) ||
        this->nativeCheckpointInputModelFileName.isEmpty() ||
        sha256File(this->nativeCheckpointInputModelFileName) != this->nativeCheckpointModelSha256 ||
        document.toJson(QJsonDocument::Compact) != canonicalContextJson)
        return false;

    if (this->isNativeRecoveryTask() &&
        !this->seedNativeAcceptedCheckpoint(directory, document.object(), error))
        return false;

    this->solverArguments.append(QStringLiteral("--native-checkpoint-dir=") +
                                 QDir::cleanPath(QFileInfo(directory).absoluteFilePath()));
    this->solverArguments.append(QStringLiteral("--native-checkpoint-context-json=") +
                                 QString::fromUtf8(canonicalContextJson));
    return true;
}

bool SolverTask::requestNativeAcceptedBoundaryPause(
    uint acceptedSequence,
    const std::function<bool(const QString &, QString *)> &persistRequest,
    QString *error)
{
    if (error) error->clear();
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    std::unique_lock<QMutex> configurationLocker(this->nativeExecutionConfigurationMutex, std::try_to_lock);
    if (!configurationLocker.owns_lock())
        return reject(QStringLiteral("The native task execution configuration is busy; retry only while it remains queued."));
    if (QThread::currentThread() != this->thread() ||
        this->performStarted.load(std::memory_order_acquire) ||
        !this->inputSnapshot || !persistRequest || acceptedSequence == 0 ||
        this->nativeExecutionBoundaryPauseSequence != 0)
        return reject(QStringLiteral("An accepted-boundary pause can only be requested once before native process start."));

    int checkpointDirectoryArguments = 0;
    int checkpointContextArguments = 0;
    for (const QString &argument : this->solverArguments)
    {
        if (argument.startsWith(QStringLiteral("--native-checkpoint-dir="))) ++checkpointDirectoryArguments;
        if (argument.startsWith(QStringLiteral("--native-checkpoint-context-json="))) ++checkpointContextArguments;
        if (argument.startsWith(QStringLiteral("--pause-at-accepted-seq=")) ||
            argument.startsWith(QStringLiteral("--native-execution-state=")))
            return reject(QStringLiteral("The native task already has an accepted-boundary request."));
    }
    if (checkpointDirectoryArguments != 1 || checkpointContextArguments != 1)
        return reject(QStringLiteral("Accepted-boundary pauses require exactly one pinned native checkpoint context."));

    const RProblemSetup &problemSetup = this->inputSnapshot->getProblemSetup();
    const RStructuralDynamicsSetup &dynamics = problemSetup.getStructuralDynamicsSetup();
    const RStructuralDynamicsRestartData &initialRestart = problemSetup.getStructuralDynamicsRestartData();
    if (this->inputSnapshot->getProblemTaskTree().getProblemTypeMask() != R_PROBLEM_STRESS ||
        !dynamics.getEnabled() || dynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
        problemSetup.getStructuralControlSetup().enabled || problemSetup.getStructuralCableSetup().enabled ||
        problemSetup.getStructuralPiezoSetup().enabled || problemSetup.getStructuralThermalControlSetup().enabled)
        return reject(QStringLiteral("Accepted-boundary pauses are supported only for uncontrolled Newmark structural dynamics."));

    quint64 baselineSteps = 0;
    if (initialRestart.available)
    {
        try { initialRestart.validate(); }
        catch (const RError &validationError)
        {
            return reject(QStringLiteral("The queued restart baseline is invalid: %1")
                          .arg(validationError.getMessage()));
        }
        baselineSteps = initialRestart.completedSteps;
    }
    const quint64 plannedSteps = dynamics.getNSteps();
    if (plannedSteps == 0 || baselineSteps > std::numeric_limits<quint64>::max() - plannedSteps)
        return reject(QStringLiteral("The queued Newmark run has an invalid accepted-step target."));
    const quint64 targetSteps = baselineSteps + plannedSteps;
    if (quint64(acceptedSequence) <= baselineSteps || quint64(acceptedSequence) > targetSteps)
        return reject(QStringLiteral("The requested accepted sequence must be after the baseline and no later than the pinned target."));

    const QString runRoot = getRunRoot(this->resultArtifactFileName);
    const QFileInfo runRootInfo(runRoot);
    if (runRoot.isEmpty() || !runRootInfo.isDir() || runRootInfo.isSymLink() ||
        hasSymlinkComponent(runRoot) || !samePathText(runRoot, runRootInfo.canonicalFilePath()))
        return reject(QStringLiteral("The native execution-state run directory is unavailable or non-canonical."));
    const QString eventsDirectory = QDir(runRoot).filePath(QStringLiteral("events"));
    if (!QDir().mkpath(eventsDirectory) || QFileInfo(eventsDirectory).isSymLink() ||
        hasSymlinkComponent(eventsDirectory) ||
        !samePathText(eventsDirectory, QFileInfo(eventsDirectory).canonicalFilePath()))
        return reject(QStringLiteral("The native execution-state directory is unavailable or non-canonical."));
    const QString statePath = QDir::cleanPath(QFileInfo(
        QDir(eventsDirectory).filePath(QStringLiteral("native-execution-state.json"))).absoluteFilePath());
    if (QFileInfo::exists(statePath) || QFileInfo(statePath).isSymLink() || hasSymlinkComponent(statePath))
        return reject(QStringLiteral("The native execution-state path already exists or is linked."));

    if (!persistRequest(statePath, error))
    {
        this->nativeExecutionBoundaryConfigurationRejected = true;
        if (error && error->isEmpty())
            *error = QStringLiteral("The accepted-boundary request could not be durably recorded before solver start.");
        return false;
    }
    this->solverArguments.append(QStringLiteral("--pause-at-accepted-seq=") + QString::number(acceptedSequence));
    this->solverArguments.append(QStringLiteral("--native-execution-state=") + statePath);
    this->nativeExecutionBoundaryPauseSequence = acceptedSequence;
    this->nativeExecutionStateFileName = statePath;
    return true;
}

const QString &SolverTask::getNativeExecutionStateFileName() const
{
    return this->nativeExecutionStateFileName;
}

const QJsonObject &SolverTask::getNativeCheckpointSeedProvenance() const
{
    return this->nativeCheckpointSeedProvenance;
}

bool SolverTask::seedNativeAcceptedCheckpoint(const QString &directory,
                                             const QJsonObject &checkpointContext,
                                             QString *error)
{
    if (error) error->clear();
    const auto reject = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    const QString sourceJobId = this->nativeRecoveryProvenance
            .value(QStringLiteral("recoverySourceJobId")).toString();
    const QString sourceEnvelopePath = this->nativeRecoveryProvenance
            .value(QStringLiteral("selectedEnvelopePath")).toString();
    const QString sourceEnvelopeSha = this->nativeRecoveryProvenance
            .value(QStringLiteral("selectedEnvelopeSha256")).toString();
    const QString sourceModelPath = this->nativeRecoveryProvenance
            .value(QStringLiteral("selectedCheckpointPath")).toString();
    const QString sourceModelSha = this->nativeRecoveryProvenance
            .value(QStringLiteral("selectedCheckpointSha256")).toString();
    const double acceptedSeq = this->nativeRecoveryProvenance
            .value(QStringLiteral("acceptedSeq")).toDouble(-1.0);
    const double acceptedTime = this->nativeRecoveryProvenance
            .value(QStringLiteral("checkpointTimeS")).toDouble(-1.0);
    const auto validSha = [](const QString &value)
    {
        return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(value).hasMatch();
    };
    const QString inputDirectory = QFileInfo(this->nativeCheckpointInputModelFileName).absolutePath();
    const QString expectedDirectory = QDir(getRunRoot(this->resultArtifactFileName))
            .filePath(QStringLiteral("checkpoints"));
    const double dtS = checkpointContext.value(QStringLiteral("dtS")).toDouble(-1.0);
    QStringList expectedContextKeys{QStringLiteral("schema"), QStringLiteral("sourceSha256"),
        QStringLiteral("modelSha256"), QStringLiteral("controlSha256"),
        QStringLiteral("runtimeSha256"), QStringLiteral("packageSha256"),
        QStringLiteral("policy"), QStringLiteral("maxAcceptedSteps"),
        QStringLiteral("maxSimulationSeconds"), QStringLiteral("dtS")};
    QStringList actualContextKeys = checkpointContext.keys();
    expectedContextKeys.sort();
    actualContextKeys.sort();
    const QByteArray expectedNoControlSha = QCryptographicHash::hash(
        QByteArrayLiteral("range.native-checkpoint.no-control.v1"), QCryptographicHash::Sha256);
    if (!this->isNativeRecoveryTask() || !this->nativeCheckpointSeedProvenance.isEmpty() ||
        QThread::currentThread() != this->thread() || this->performStarted.load(std::memory_order_acquire) ||
        !QFileInfo(directory).isAbsolute() || !samePathText(directory, expectedDirectory) ||
        !validSha(sourceEnvelopeSha) || !validSha(sourceModelSha) ||
        !QFileInfo(sourceEnvelopePath).isAbsolute() || !QFileInfo(sourceModelPath).isAbsolute() ||
        sourceJobId.isEmpty() || !std::isfinite(acceptedSeq) || acceptedSeq < 1.0 ||
        acceptedSeq > double(std::numeric_limits<uint>::max()) || std::floor(acceptedSeq) != acceptedSeq ||
        !std::isfinite(acceptedTime) || acceptedTime < 0.0 || !std::isfinite(dtS) || dtS <= 0.0 ||
        !this->inputSnapshot || !this->resultSnapshot || this->nativeCheckpointInputModelFileName.isEmpty() ||
        this->nativeCheckpointModelSha256.size() != 32 ||
        checkpointContext.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("range.native-checkpoint-context/v1") ||
        checkpointContext.value(QStringLiteral("modelSha256")).toString() !=
            QString::fromLatin1(this->nativeCheckpointModelSha256.toHex()) ||
        checkpointContext.value(QStringLiteral("packageSha256")).toString() !=
            QString::fromLatin1(sha256Directory(inputDirectory).toHex()) ||
        actualContextKeys != expectedContextKeys ||
        !validSha(checkpointContext.value(QStringLiteral("sourceSha256")).toString()) ||
        !validSha(checkpointContext.value(QStringLiteral("modelSha256")).toString()) ||
        !validSha(checkpointContext.value(QStringLiteral("controlSha256")).toString()) ||
        !validSha(checkpointContext.value(QStringLiteral("runtimeSha256")).toString()) ||
        !validSha(checkpointContext.value(QStringLiteral("packageSha256")).toString()) ||
        checkpointContext.value(QStringLiteral("controlSha256")).toString() !=
            QString::fromLatin1(expectedNoControlSha.toHex()) ||
        checkpointContext.value(QStringLiteral("runtimeSha256")).toString() !=
            QString::fromLatin1(sha256File(this->solverExecutable).toHex()) ||
        checkpointContext.value(QStringLiteral("policy")).toString() !=
            QStringLiteral("accepted-steps-or-sim-time-v1") ||
        checkpointContext.value(QStringLiteral("maxAcceptedSteps")).toInt(-1) != 10 ||
        checkpointContext.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) != 1.0)
        return reject(QStringLiteral("The recovery checkpoint seed is not bound to this task's exact input and context."));

    const QFileInfo sourceModelInfo(sourceModelPath);
    const QFileInfo sourceEnvelopeInfo(sourceEnvelopePath);
    if (sourceModelInfo.isSymLink() || !sourceModelInfo.isFile() || hasSymlinkComponent(sourceModelPath) ||
        sourceModelInfo.canonicalFilePath() != sourceModelInfo.absoluteFilePath() ||
        sourceModelInfo.size() <= 0 || sourceModelInfo.size() > 512LL * 1024 * 1024 ||
        sourceEnvelopeInfo.isSymLink() || !sourceEnvelopeInfo.isFile() || hasSymlinkComponent(sourceEnvelopePath) ||
        sourceEnvelopeInfo.canonicalFilePath() != sourceEnvelopeInfo.absoluteFilePath() ||
        !samePathText(sourceModelInfo.absolutePath(), sourceEnvelopeInfo.absolutePath()) ||
        QString::fromLatin1(sha256File(sourceModelPath).toHex()) != sourceModelSha ||
        QString::fromLatin1(sha256File(sourceEnvelopePath).toHex()) != sourceEnvelopeSha)
        return reject(QStringLiteral("The selected source checkpoint MODEL or envelope changed before it could be seeded."));

    QFile sourceEnvelopeFile(sourceEnvelopePath);
    if (!sourceEnvelopeFile.open(QIODevice::ReadOnly) || sourceEnvelopeFile.size() > 64 * 1024)
        return reject(QStringLiteral("The selected source checkpoint envelope could not be read within its bound."));
    const QByteArray sourceEnvelopeBytes = sourceEnvelopeFile.readAll();
    QJsonParseError sourceEnvelopeParse;
    const QJsonDocument sourceEnvelopeDocument = QJsonDocument::fromJson(sourceEnvelopeBytes, &sourceEnvelopeParse);
    const QJsonObject sourceEnvelope = sourceEnvelopeDocument.object();
    QStringList sourceEnvelopeKeys{QStringLiteral("schema"), QStringLiteral("sourceSha256"),
        QStringLiteral("inputModelSha256"), QStringLiteral("controlSha256"),
        QStringLiteral("runtimeSha256"), QStringLiteral("packageSha256"),
        QStringLiteral("policy"), QStringLiteral("maxAcceptedSteps"),
        QStringLiteral("maxSimulationSeconds"), QStringLiteral("acceptedSeq"),
        QStringLiteral("timeS"), QStringLiteral("dtS"), QStringLiteral("modelFile"),
        QStringLiteral("checkpointModelSha256")};
    QStringList actualEnvelopeKeys = sourceEnvelope.keys();
    sourceEnvelopeKeys.sort();
    actualEnvelopeKeys.sort();
    if (sourceEnvelopeFile.error() != QFileDevice::NoError ||
        sourceEnvelopeParse.error != QJsonParseError::NoError || !sourceEnvelopeDocument.isObject() ||
        QJsonDocument(sourceEnvelope).toJson(QJsonDocument::Compact) != sourceEnvelopeBytes ||
        actualEnvelopeKeys != sourceEnvelopeKeys ||
        sourceEnvelope.value(QStringLiteral("schema")).toString() != QStringLiteral("range.native-checkpoint/v1") ||
        sourceEnvelope.value(QStringLiteral("policy")).toString() != QStringLiteral("accepted-steps-or-sim-time-v1") ||
        sourceEnvelope.value(QStringLiteral("maxAcceptedSteps")).toInt(-1) != 10 ||
        sourceEnvelope.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) != 1.0 ||
        sourceEnvelope.value(QStringLiteral("acceptedSeq")).toDouble(-1.0) != acceptedSeq ||
        sourceEnvelope.value(QStringLiteral("timeS")).toDouble(-1.0) != acceptedTime ||
        sourceEnvelope.value(QStringLiteral("dtS")).toDouble(-1.0) != dtS ||
        sourceEnvelope.value(QStringLiteral("checkpointModelSha256")).toString() != sourceModelSha ||
        sourceEnvelope.value(QStringLiteral("modelFile")).toString() != sourceModelInfo.fileName())
        return reject(QStringLiteral("The selected source checkpoint envelope does not prove the requested accepted state."));

    RModel selectedModel;
    RModel queuedInputModel;
    try
    {
        selectedModel.read(sourceModelPath);
        queuedInputModel.read(this->nativeCheckpointInputModelFileName);
    }
    catch (const RError &readError)
    {
        return reject(QStringLiteral("The selected checkpoint or exact queued input could not be decoded: %1")
                      .arg(readError.getMessage()));
    }
    const RStructuralDynamicsRestartData &selectedRestart =
            selectedModel.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsRestartData &queuedRestart =
            queuedInputModel.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsSetup &queuedDynamics =
            queuedInputModel.getProblemSetup().getStructuralDynamicsSetup();
    try
    {
        selectedRestart.validate();
        queuedRestart.validate();
    }
    catch (const RError &validationError)
    {
        return reject(QStringLiteral("The selected or queued accepted restart state is invalid: %1")
                      .arg(validationError.getMessage()));
    }
    if (!selectedRestart.available || selectedRestart.completedSteps != quint64(acceptedSeq) ||
        !sameRestartState(selectedRestart, queuedRestart) ||
        selectedModel.getNNodes() != queuedInputModel.getNNodes() ||
        selectedModel.getNElements() != queuedInputModel.getNElements() ||
        !queuedDynamics.getEnabled() || queuedDynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
        queuedInputModel.getProblemSetup().getStructuralControlSetup().enabled ||
        queuedInputModel.getProblemSetup().getStructuralCableSetup().enabled ||
        queuedInputModel.getProblemSetup().getStructuralPiezoSetup().enabled ||
        queuedInputModel.getProblemSetup().getStructuralThermalControlSetup().enabled ||
        std::abs(selectedRestart.time - acceptedTime) > 1.0e-12 * qMax(1.0, std::abs(acceptedTime)) ||
        std::abs(selectedRestart.fixedTimeStep - dtS) > 1.0e-12 * qMax(1.0, std::abs(dtS)))
        return reject(QStringLiteral("The exact queued recovery input does not preserve the selected checkpoint q/v/a/work state."));

    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink() || hasSymlinkComponent(directory) ||
        QFileInfo(directory).canonicalFilePath().isEmpty() ||
        !samePathText(QFileInfo(directory).canonicalFilePath(), directory) ||
        !QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                   QDir::NoDotAndDotDot).isEmpty())
        return reject(QStringLiteral("The new recovery checkpoint directory is not empty, private, or canonical."));

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLower();
    const QString seedModelName = QStringLiteral("checkpoint-seed-%1.rbm").arg(token);
    const QString seedModelPath = QDir(directory).filePath(seedModelName);
    const QString seedEnvelopeName = QStringLiteral("envelope-seed-%1.json").arg(token);
    const QString seedEnvelopePath = QDir(directory).filePath(seedEnvelopeName);
    const QString latestPath = QDir(directory).filePath(QStringLiteral("latest.json"));
    const auto writeNewFile = [error](const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            if (error) *error = QStringLiteral("Could not exclusively create recovery checkpoint file '%1': %2")
                    .arg(path, file.errorString());
            return false;
        }
        if (file.write(bytes) != bytes.size() || !flushNativeRecoveryFile(file, error)) return false;
        file.close();
        return true;
    };

    QFile sourceModel(sourceModelPath);
    QFile seedModel(seedModelPath);
    if (!sourceModel.open(QIODevice::ReadOnly) || !seedModel.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return reject(QStringLiteral("The selected checkpoint MODEL could not be staged as an immutable seed."));
    char buffer[64 * 1024];
    qint64 copiedBytes = 0;
    while (!sourceModel.atEnd())
    {
        const qint64 bytesRead = sourceModel.read(buffer, qint64(sizeof(buffer)));
        if (bytesRead < 0 || copiedBytes > 512LL * 1024 * 1024 - bytesRead ||
            seedModel.write(buffer, bytesRead) != bytesRead)
            return reject(QStringLiteral("The selected checkpoint MODEL seed copy exceeded its bound or failed."));
        copiedBytes += bytesRead;
    }
    if (sourceModel.error() != QFileDevice::NoError ||
        QString::fromLatin1(sha256File(sourceModelPath).toHex()) != sourceModelSha ||
        !flushNativeRecoveryFile(seedModel, error))
        return reject(QStringLiteral("The selected checkpoint MODEL changed or could not be flushed during seed publication."));
    seedModel.close();
    const QString seedModelSha = QString::fromLatin1(sha256File(seedModelPath).toHex());
    if (seedModelSha != sourceModelSha || !samePathText(QFileInfo(seedModelPath).absolutePath(), directory))
        return reject(QStringLiteral("The immutable recovery seed MODEL failed its source hash read-back."));

    QJsonObject seedEnvelope{{QStringLiteral("schema"), QStringLiteral("range.native-checkpoint/v1")},
        {QStringLiteral("sourceSha256"), checkpointContext.value(QStringLiteral("sourceSha256"))},
        {QStringLiteral("inputModelSha256"), checkpointContext.value(QStringLiteral("modelSha256"))},
        {QStringLiteral("controlSha256"), checkpointContext.value(QStringLiteral("controlSha256"))},
        {QStringLiteral("runtimeSha256"), checkpointContext.value(QStringLiteral("runtimeSha256"))},
        {QStringLiteral("packageSha256"), checkpointContext.value(QStringLiteral("packageSha256"))},
        {QStringLiteral("policy"), checkpointContext.value(QStringLiteral("policy"))},
        {QStringLiteral("maxAcceptedSteps"), checkpointContext.value(QStringLiteral("maxAcceptedSteps"))},
        {QStringLiteral("maxSimulationSeconds"), checkpointContext.value(QStringLiteral("maxSimulationSeconds"))},
        {QStringLiteral("acceptedSeq"), acceptedSeq},
        {QStringLiteral("timeS"), acceptedTime},
        {QStringLiteral("dtS"), dtS},
        {QStringLiteral("modelFile"), seedModelName},
        {QStringLiteral("checkpointModelSha256"), seedModelSha}};
    const QByteArray seedEnvelopeBytes = QJsonDocument(seedEnvelope).toJson(QJsonDocument::Compact);
    if (seedEnvelopeBytes.isEmpty() || seedEnvelopeBytes.size() > 64 * 1024 ||
        !writeNewFile(seedEnvelopePath, seedEnvelopeBytes))
        return reject(QStringLiteral("The recovery seed envelope could not be exclusively published."));
    const QString seedEnvelopeSha = QString::fromLatin1(sha256File(seedEnvelopePath).toHex());
    if (!validSha(seedEnvelopeSha))
        return reject(QStringLiteral("The recovery seed envelope failed its hash read-back."));
    const QJsonObject pointer{{QStringLiteral("schema"), QStringLiteral("range.native-checkpoint-pointer/v1")},
                              {QStringLiteral("envelopeFile"), seedEnvelopeName},
                              {QStringLiteral("envelopeSha256"), seedEnvelopeSha}};
    const QByteArray pointerBytes = QJsonDocument(pointer).toJson(QJsonDocument::Compact);
    if (!writeNewFile(latestPath, pointerBytes))
        return reject(QStringLiteral("The recovery seed pointer could not be atomically made visible."));

    QFile verifyPointerFile(latestPath);
    if (!verifyPointerFile.open(QIODevice::ReadOnly) || verifyPointerFile.readAll() != pointerBytes ||
        QString::fromLatin1(sha256File(seedModelPath).toHex()) != seedModelSha ||
        QString::fromLatin1(sha256File(seedEnvelopePath).toHex()) != seedEnvelopeSha)
        return reject(QStringLiteral("The recovery seed pointer, envelope, or MODEL failed post-publication verification."));

    this->nativeCheckpointSeedProvenance = QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("range.native-checkpoint-seed/v1")},
        {QStringLiteral("sourceJobId"), sourceJobId},
        {QStringLiteral("sourceEnvelopeRef"), sourceEnvelopePath},
        {QStringLiteral("sourceEnvelopeSha256"), sourceEnvelopeSha},
        {QStringLiteral("sourceCheckpointModelRef"), sourceModelPath},
        {QStringLiteral("sourceCheckpointModelSha256"), sourceModelSha},
        {QStringLiteral("seedModelRef"), seedModelPath},
        {QStringLiteral("seedModelSha256"), seedModelSha},
        {QStringLiteral("seedEnvelopeRef"), seedEnvelopePath},
        {QStringLiteral("seedEnvelopeSha256"), seedEnvelopeSha},
        {QStringLiteral("seedPointerRef"), latestPath},
        {QStringLiteral("inputModelSha256"), checkpointContext.value(QStringLiteral("modelSha256"))},
        {QStringLiteral("packageSha256"), checkpointContext.value(QStringLiteral("packageSha256"))},
        {QStringLiteral("acceptedSeq"), acceptedSeq},
        {QStringLiteral("timeS"), acceptedTime},
        {QStringLiteral("dtS"), dtS}};
    return true;
}

const QJsonObject &SolverTask::getNativeResultProvenance() const
{
    return this->nativeResultProvenance;
}

bool SolverTask::expectsNativeResultCatalog() const
{
    return this->inputSnapshot && supportsNativeResultCatalog(*this->inputSnapshot);
}

const QString &SolverTask::getResultArtifactFileName() const
{
    return this->resultArtifactFileName;
}

const QString &SolverTask::getConvergenceFileName() const
{
    return this->convergenceFileName;
}

const QString &SolverTask::getMonitoringFileName() const
{
    return this->monitoringFileName;
}

const QString &SolverTask::getNativeProgressFileName() const
{
    return this->nativeProgressFileName;
}

const QString &SolverTask::getNativeResultJournalFileName() const
{
    return this->nativeResultJournalFileName;
}

uint SolverTask::getModelID() const
{
    return this->modelID;
}

bool SolverTask::rebindModelIDForSubmit(uint currentModelID)
{
    if (QThread::currentThread() != this->thread() ||
        this->performStarted.load(std::memory_order_acquire))
        return false;
    this->modelID = currentModelID;
    return true;
}

const SolverTaskID &SolverTask::getTaskID() const
{
    return this->taskID;
}

SolverTaskID &SolverTask::getTaskID()
{
    return this->taskID;
}

const QString SolverTask::getStopCommand() const
{
    return QString(this->taskID.toString() + ":STOP");
}

void SolverTask::kill()
{
    RLogger::info("Killing solver task (#%s).\n",this->taskID.toString().toUtf8().constData());
    this->killRequested.store(true, std::memory_order_release);
}

const QString &SolverTask::getLogFileName() const
{
    return this->logFileName;
}

QString SolverTask::getCommandLine() const
{
    QString cmdLine = this->solverExecutable;
    for (int i=0;i<this->solverArguments.size();i++)
    {
        cmdLine += QString(" \"") + this->solverArguments.at(i) + QString("\"");
    }
    return cmdLine;
}

int SolverTask::perform()
{
    bool boundaryConfigurationRejected = false;
    {
        QMutexLocker<QMutex> configurationLocker(&this->nativeExecutionConfigurationMutex);
        this->performStarted.store(true, std::memory_order_release);
        boundaryConfigurationRejected = this->nativeExecutionBoundaryConfigurationRejected;
    }
    bool loggerIndented = false;
    bool processStarted = false;
    bool processExited = false;
    bool resultReadbackComplete = false;
    int processExitCode = -1;
    QString resultArtifactPath;
    QString resultArtifactSha256;
    QJsonObject resultSourceBinding;
    QJsonArray reusedHistory;
    QSet<QString> producedFramePaths;
    bool resultArtifactVerified = false;
    const auto reportTerminal = [this, &processStarted, &processExited, &processExitCode,
                                 &resultReadbackComplete, &resultArtifactPath,
                                 &resultArtifactSha256, &resultArtifactVerified]
                                (const QString &state, const QString &message)
    {
        emit this->terminalReported(this->taskID.toString(), state, processStarted,
                                    processExited, processExitCode, resultReadbackComplete,
                                    resultArtifactPath, resultArtifactSha256,
                                    resultArtifactVerified, message);
    };
    QByteArray progressTail;
    qint64 progressOffset = 0;
    quint64 lastProgressSequence = 0;
    quint64 lastProgressPhase = 0;
    quint64 activeProgressPhase = 0;
    QString activeProgressMessage;
    bool progressFileObserved = false;
    bool progressEventObserved = false;
    bool progressUnavailableReported = false;
    bool progressProtocolInvalid = false;
    const auto progressDiagnostic = [this](const QString &message)
    {
        emit this->nativeProgressDiagnostic(this->taskID.toString(), message);
    };
    const auto progressUnavailable = [this, &progressUnavailableReported](const QString &message)
    {
        if (progressUnavailableReported) return;
        progressUnavailableReported = true;
        emit this->nativeProgressUnavailable(this->taskID.toString(), message);
    };
    const auto acceptProgressLine = [this, &lastProgressSequence, &lastProgressPhase,
                                     &activeProgressPhase, &activeProgressMessage,
                                     &progressEventObserved, &progressProtocolInvalid,
                                     &progressDiagnostic, &progressUnavailable](QByteArray line)
    {
        if (progressProtocolInvalid) return;
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) return;
        if (line.size() > 16384)
        {
            progressDiagnostic(QStringLiteral("Rejected native progress line exceeding 16 KiB."));
            progressProtocolInvalid = true;
            progressUnavailable(QStringLiteral("Native progress journal contains an oversized line."));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            progressDiagnostic(QStringLiteral("Rejected malformed native progress JSON line."));
            progressProtocolInvalid = true;
            progressUnavailable(QStringLiteral("Native progress journal contains invalid JSON."));
            return;
        }
        const QJsonObject event = document.object();
        const QSet<QString> expectedKeys{QStringLiteral("protocolVersion"), QStringLiteral("jobId"),
                                         QStringLiteral("seq"), QStringLiteral("phaseId"),
                                         QStringLiteral("kind"), QStringLiteral("message"),
                                         QStringLiteral("pulse"), QStringLiteral("fraction")};
        bool exactSchema = event.size() == expectedKeys.size();
        for (auto field = event.begin(); exactSchema && field != event.end(); ++field)
            exactSchema = expectedKeys.contains(field.key());
        if (!exactSchema ||
            event.value(QStringLiteral("protocolVersion")).toString() != QStringLiteral("range.native-progress/v1") ||
            event.value(QStringLiteral("jobId")).toString() != this->taskID.toString() ||
            !event.value(QStringLiteral("message")).isString() ||
            !event.value(QStringLiteral("pulse")).isBool())
        {
            progressDiagnostic(QStringLiteral("Rejected native progress record with a wrong job ID or schema."));
            progressProtocolInvalid = true;
            progressUnavailable(QStringLiteral("Native progress journal contains a record with an invalid job ID or schema."));
            return;
        }
        const auto parsePositiveInteger = [](const QJsonValue &value, quint64 *number)
        {
            if (!value.isDouble()) return false;
            const double raw = value.toDouble();
            constexpr double maxExactJsonInteger = 9007199254740991.0;
            if (!std::isfinite(raw) || raw < 1.0 || raw > maxExactJsonInteger || std::floor(raw) != raw)
                return false;
            if (number) *number = quint64(raw);
            return true;
        };
        quint64 sequence = 0;
        quint64 phase = 0;
        if (!parsePositiveInteger(event.value(QStringLiteral("seq")), &sequence) ||
            !parsePositiveInteger(event.value(QStringLiteral("phaseId")), &phase))
        {
            progressDiagnostic(QStringLiteral("Rejected native progress record with a non-positive sequence or phase ID."));
            progressProtocolInvalid = true;
            progressUnavailable(QStringLiteral("Native progress journal has an invalid sequence or phase ID."));
            return;
        }
        const QString kind = event.value(QStringLiteral("kind")).toString();
        if (kind != QStringLiteral("phase_started") && kind != QStringLiteral("fraction") &&
            kind != QStringLiteral("phase_finished"))
        {
            progressDiagnostic(QStringLiteral("Rejected native progress record with an unknown event kind."));
            progressProtocolInvalid = true;
            progressUnavailable(QStringLiteral("Native progress journal has an unknown event kind."));
            return;
        }
        const QJsonValue fractionValue = event.value(QStringLiteral("fraction"));
        const bool pulse = event.value(QStringLiteral("pulse")).toBool();
        double fraction = 0.0;
        if (pulse)
        {
            if (!fractionValue.isNull())
            {
                progressDiagnostic(QStringLiteral("Rejected pulsed native progress record with a numeric fraction."));
                progressProtocolInvalid = true;
                progressUnavailable(QStringLiteral("Native progress journal violates the pulse/fraction schema."));
                return;
            }
        }
        else
        {
            if (!fractionValue.isDouble())
            {
                progressDiagnostic(QStringLiteral("Rejected determinate native progress record without a fraction."));
                progressProtocolInvalid = true;
                progressUnavailable(QStringLiteral("Native progress journal violates the pulse/fraction schema."));
                return;
            }
            fraction = fractionValue.toDouble();
            if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
            {
                progressDiagnostic(QStringLiteral("Rejected native progress fraction outside 0..1."));
                progressProtocolInvalid = true;
                progressUnavailable(QStringLiteral("Native progress journal contains a fraction outside 0..1."));
                return;
            }
        }
        if (sequence <= lastProgressSequence)
        {
            progressDiagnostic(QStringLiteral("Rejected duplicate or out-of-order native progress sequence."));
            progressProtocolInvalid = true;
            progressUnavailable(QStringLiteral("Native progress journal has a duplicate or out-of-order sequence."));
            return;
        }
        if (sequence != lastProgressSequence + 1)
            progressDiagnostic(QStringLiteral("Native progress sequence gap detected."));
        if (kind == QStringLiteral("phase_started"))
        {
            if ((lastProgressPhase > 0 && phase <= lastProgressPhase) ||
                (!pulse && fraction != 0.0))
            {
                progressDiagnostic(QStringLiteral("Rejected invalid native progress phase start."));
                progressProtocolInvalid = true;
                progressUnavailable(QStringLiteral("Native progress journal has an invalid phase start."));
                return;
            }
            if (lastProgressPhase > 0 && phase != lastProgressPhase + 1)
                progressDiagnostic(QStringLiteral("Native progress phase ID gap detected."));
            activeProgressPhase = phase;
            activeProgressMessage = event.value(QStringLiteral("message")).toString();
            lastProgressPhase = phase;
        }
        else
        {
            // Print callbacks retain the current phase message. Finalize
            // callbacks carry their own message (commonly "Done"), and a
            // later Initialize starts a new superseding phase ID.
            if (activeProgressPhase == 0 || phase != activeProgressPhase ||
                (kind == QStringLiteral("fraction") &&
                 event.value(QStringLiteral("message")).toString() != activeProgressMessage) ||
                (kind == QStringLiteral("phase_finished") && !pulse && fraction != 1.0))
            {
                progressDiagnostic(QStringLiteral("Rejected native progress record outside its active phase."));
                progressProtocolInvalid = true;
                progressUnavailable(QStringLiteral("Native progress journal has an invalid phase transition."));
                return;
            }
            if (kind == QStringLiteral("phase_finished")) activeProgressPhase = 0;
        }
        lastProgressSequence = sequence;
        progressEventObserved = true;
        emit this->nativeProgressReported(this->taskID.toString(), event);
    };
    const auto pollNativeProgress = [this, &progressTail, &progressOffset, &progressFileObserved,
                                     &progressProtocolInvalid, &acceptProgressLine,
                                     &progressDiagnostic, &progressUnavailable]()
    {
        if (progressProtocolInvalid) return;
        const QFileInfo journalInfo(this->nativeProgressFileName);
        if (!journalInfo.exists()) return;
        progressFileObserved = true;
        if (!journalInfo.isFile() || journalInfo.isSymLink())
        {
            progressUnavailable(QStringLiteral("Native progress journal is not a regular file."));
            progressProtocolInvalid = true;
            return;
        }
        constexpr qint64 maxJournalBytes = 16 * 1024 * 1024;
        constexpr qint64 maxReadBytesPerPoll = 64 * 1024;
        if (journalInfo.size() > maxJournalBytes)
        {
            progressUnavailable(QStringLiteral("Native progress journal exceeded its 16 MiB budget."));
            progressProtocolInvalid = true;
            return;
        }
        if (journalInfo.size() < progressOffset)
        {
            progressUnavailable(QStringLiteral("Native progress journal was truncated during the run."));
            progressProtocolInvalid = true;
            progressTail.clear();
            progressOffset = journalInfo.size();
            return;
        }
        QFile journal(this->nativeProgressFileName);
        if (!journal.open(QIODevice::ReadOnly) || !journal.seek(progressOffset))
        {
            progressUnavailable(QStringLiteral("Native progress journal could not be read."));
            progressProtocolInvalid = true;
            return;
        }
        const QByteArray bytes = journal.read(maxReadBytesPerPoll);
        if (journal.error() != QFile::NoError)
        {
            progressUnavailable(QStringLiteral("Native progress journal read failed."));
            progressProtocolInvalid = true;
            return;
        }
        progressOffset += bytes.size();
        progressTail.append(bytes);
        int newline = -1;
        while ((newline = progressTail.indexOf('\n')) >= 0)
        {
            const QByteArray line = progressTail.left(newline);
            progressTail.remove(0, newline + 1);
            acceptProgressLine(line);
            if (progressProtocolInvalid) return;
        }
        if (progressTail.size() > 16384)
        {
            progressDiagnostic(QStringLiteral("Discarded oversized partial native progress line."));
            progressUnavailable(QStringLiteral("Native progress journal has an oversized partial line."));
            progressProtocolInvalid = true;
            progressTail.clear();
        }
    };
    const auto drainNativeProgress = [this, &progressOffset, &progressProtocolInvalid,
                                      &pollNativeProgress]()
    {
        constexpr int maxDrainPolls = 256; // 16 MiB at 64 KiB per poll.
        for (int poll = 0; poll < maxDrainPolls && !progressProtocolInvalid; ++poll)
        {
            const QFileInfo journalInfo(this->nativeProgressFileName);
            if (!journalInfo.exists() || journalInfo.size() <= progressOffset) return;
            const qint64 previousOffset = progressOffset;
            pollNativeProgress();
            if (progressOffset == previousOffset) return;
        }
    };
    QByteArray resultTail;
    qint64 resultOffset = 0;
    quint64 resultSequence = 0;
    bool resultJournalInvalid = false;
    const auto resultUnavailable = [this, &resultJournalInvalid](const QString &reason)
    {
        if (!resultJournalInvalid) emit this->nativeResultFramesUnavailable(this->taskID.toString(), reason);
        resultJournalInvalid = true;
    };
    const auto pollNativeResults = [this, &resultTail, &resultOffset, &resultSequence,
                                     &resultJournalInvalid, &resultUnavailable, &producedFramePaths]()
    {
        if (resultJournalInvalid) return;
        const QFileInfo info(this->nativeResultJournalFileName);
        if (!info.exists()) return;
        if (info.isSymLink() || !info.isFile() || info.canonicalFilePath() != info.absoluteFilePath() ||
            info.size() > 16 * 1024 * 1024 || info.size() < resultOffset)
        {
            resultUnavailable(QStringLiteral("The native result journal path or size is invalid."));
            return;
        }
        QFile journal(this->nativeResultJournalFileName);
        if (!journal.open(QIODevice::ReadOnly) || !journal.seek(resultOffset))
        {
            resultUnavailable(QStringLiteral("The native result journal could not be read."));
            return;
        }
        const QByteArray bytes = journal.read(64 * 1024);
        if (journal.error() != QFileDevice::NoError)
        {
            resultUnavailable(QStringLiteral("The native result journal read failed."));
            return;
        }
        resultOffset += bytes.size();
        resultTail.append(bytes);
        for (qsizetype newline = resultTail.indexOf('\n'); newline >= 0; newline = resultTail.indexOf('\n'))
        {
            const QByteArray line = resultTail.left(newline);
            resultTail.remove(0, newline + 1);
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            const QJsonObject frame = document.object();
            const double sequence = frame.value(QStringLiteral("seq")).toDouble(-1);
            const double recordNo = frame.value(QStringLiteral("recordNo")).toDouble(-1);
            const QString relativePath = frame.value(QStringLiteral("relativePath")).toString();
            const QFileInfo frameInfo(QDir(QFileInfo(this->resultArtifactFileName).absolutePath()).filePath(relativePath));
            const QString hash = frame.value(QStringLiteral("sha256")).toString();
            if (line.size() > 16384 || error.error != QJsonParseError::NoError || !document.isObject() ||
                frame.value(QStringLiteral("protocolVersion")).toString() != QStringLiteral("range.native-result-frame/v1") ||
                frame.value(QStringLiteral("jobId")).toString() != this->taskID.toString() ||
                sequence != double(resultSequence + 1) || sequence > double((quint64(1) << 53) - 1) ||
                recordNo < 0 || recordNo > double(std::numeric_limits<uint>::max()) || std::floor(recordNo) != recordNo ||
                !frame.value(QStringLiteral("timeS")).isDouble() || !std::isfinite(frame.value(QStringLiteral("timeS")).toDouble()) ||
                relativePath.isEmpty() || QFileInfo(relativePath).fileName() != relativePath ||
                frameInfo.isSymLink() || !frameInfo.isFile() || frameInfo.canonicalFilePath() != frameInfo.absoluteFilePath() ||
                !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(hash).hasMatch() ||
                QString::fromLatin1(sha256File(frameInfo.absoluteFilePath()).toHex()) != hash)
            {
                resultUnavailable(QStringLiteral("The native result journal contains an invalid sequence, frame path or SHA-256."));
                return;
            }
            ++resultSequence;
            producedFramePaths.insert(relativePath);
            emit this->nativeResultFrameReported(this->taskID.toString(), frame);
        }
        if (resultTail.size() > 16384)
            resultUnavailable(QStringLiteral("The native result journal contains an oversized partial record."));
    };
    const auto drainNativeResults = [this, &resultOffset, &resultJournalInvalid, &pollNativeResults]()
    {
        for (int poll = 0; poll < 256 && !resultJournalInvalid; ++poll)
        {
            if (QFileInfo(this->nativeResultJournalFileName).size() <= resultOffset) return;
            const qint64 previousOffset = resultOffset;
            pollNativeResults();
            if (resultOffset == previousOffset) return;
        }
    };
    try
    {
        if (boundaryConfigurationRejected)
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "A requested native accepted-boundary pause was not durably recorded; the solver was not started.");
        RLogger::info("Solver task (#%s) - Begin\n",this->taskID.toString().toUtf8().constData());
        RLogger::indent();
        loggerIndented = true;

        if (!this->inputSnapshot)
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver input snapshot was not captured before scheduling.");
        }
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver task was cancelled before execution.");
        }
        if (sha256File(this->inputSnapshotFileName) != this->inputSnapshotSha256)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The immutable solver input artifact changed after it was captured.");
        }
        const QString inputDirectory = QFileInfo(this->inputSnapshotFileName).absolutePath();
        if (sha256Directory(inputDirectory) != this->inputPackageSha256)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The immutable solver input package changed after it was captured.");
        }

        // The input model was serialized synchronously by SolverManager::submit().
        // Only its detached result copy is written to the path opened by the solver.
        try
        {
            const QString resultDirectory = QFileInfo(this->resultArtifactFileName).absolutePath();
            const QString logDirectory = QFileInfo(this->logFileName).absolutePath();
            const QString progressDirectory = QFileInfo(this->nativeProgressFileName).absolutePath();
            if (!QDir().mkpath(resultDirectory) || !QDir().mkpath(logDirectory))
            {
                throw RError(RError::Type::InvalidFileName, R_ERROR_REF,
                             "Failed to create solver run artifact directories under '%s'.",
                             resultDirectory.toUtf8().constData());
            }
            if (!QDir().mkpath(progressDirectory) || QFileInfo::exists(this->nativeProgressFileName) ||
                QFileInfo::exists(this->nativeResultJournalFileName))
            {
                throw RError(RError::Type::InvalidFileName, R_ERROR_REF,
                             "Failed to prepare the private native progress journal directory.");
            }
            QString historyError;
            if (!copyRestartHistoryToResult(this->inputSnapshotFileName,
                                            this->resultArtifactFileName,
                                            &reusedHistory,
                                            &historyError))
            {
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "Could not stage restart history: %s", historyError.toUtf8().constData());
            }
            // Numbered history writes publish a logical latest LINK. Restore
            // the prepared input MODEL last so the worker opens the requested
            // continuation settings, rather than the old checkpoint's setup.
            ModelIO::save(*this->resultSnapshot, this->resultArtifactFileName, false);
            if (!this->nativeCheckpointModelSha256.isEmpty() &&
                sha256File(this->resultArtifactFileName) != this->nativeCheckpointModelSha256)
            {
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "The solver's initial MODEL bytes differ from the checkpoint's frozen modelSha256.");
            }
            if (sha256Directory(inputDirectory) != this->inputPackageSha256)
            {
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "The immutable solver input package changed while preparing the result artifact.");
            }
        }
        catch (const RError &error)
        {
            RLogger::error("%s Result: '%s'\n",error.getMessage().toUtf8().constData(),this->resultArtifactFileName.toUtf8().constData());
            throw RError(RError::Type::Application,R_ERROR_REF,"Failed to prepare the detached solver result artifact.");
        }
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver task was cancelled before process start.");
        }
        if (this->nativeResultProvenanceFrozen &&
            sha256File(this->solverExecutable) != this->frozenSolverExecutableSha256)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The solver executable changed after native-result provenance was frozen.");
        }

        // Execute solver
        QString cmdLine = this->getCommandLine();

        RLogger::info("Executing \'%s\'\n",cmdLine.toUtf8().constData());

        this->solverProcess = new QProcess;
        QProcessEnvironment processEnvironment = QProcessEnvironment::systemEnvironment();
        processEnvironment.insert(QStringLiteral("RANGE_NATIVE_JOB_ID"), this->taskID.toString());
        processEnvironment.insert(QStringLiteral("RANGE_NATIVE_PROGRESS_PATH"),
                                  QFileInfo(this->nativeProgressFileName).absoluteFilePath());
        processEnvironment.insert(QStringLiteral("RANGE_NATIVE_RESULT_JOURNAL_PATH"),
                                  QFileInfo(this->nativeResultJournalFileName).absoluteFilePath());
        processEnvironment.insert(QStringLiteral("RANGE_NATIVE_RESULT_DIRECTORY"),
                                  QFileInfo(this->resultArtifactFileName).absolutePath());
        this->solverProcess->setProcessEnvironment(processEnvironment);

        QObject::connect(this->solverProcess,&QProcess::readyReadStandardOutput,this,&SolverTask::onProcessReadyReadStandardOutput,Qt::DirectConnection);
        QObject::connect(this->solverProcess,&QProcess::readyReadStandardError,this,&SolverTask::onProcessReadyReadStandardError,Qt::DirectConnection);

        this->solverProcess->start(this->solverExecutable,this->solverArguments);

        if (!this->solverProcess->waitForStarted())
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver process could not be started.");
        }
        processStarted = true;
        emit this->nativeProcessStarted(this->taskID.toString(), this->solverProcess->processId());
        for (;;)
        {
            const bool processFinished = this->solverProcess->waitForFinished(5);
            pollNativeProgress();
            pollNativeResults();
            if (processFinished) break;
            if (this->killRequested.load(std::memory_order_acquire))
            {
                if (this->solverProcess->state() != QProcess::NotRunning)
                {
                    this->solverProcess->kill();
                    if (!this->solverProcess->waitForFinished(5000))
                    {
                        RLogger::warning("Solver process did not exit promptly after cancellation.\n");
                    }
                }
                pollNativeProgress();
                throw RError(RError::Type::Application, R_ERROR_REF, "Solver execution was cancelled.");
            }
            if (this->solverProcess->state() == QProcess::NotRunning) break;
        }
        drainNativeProgress();
        drainNativeResults();
        if (!resultTail.isEmpty())
            resultUnavailable(QStringLiteral("The native result journal ended with an incomplete record."));
        if (!progressTail.isEmpty())
        {
            progressDiagnostic(QStringLiteral("Discarded incomplete final native progress line."));
            progressUnavailable(QStringLiteral("Native progress journal ended with an incomplete record."));
            progressTail.clear();
        }
        if (!progressFileObserved || QFileInfo(this->nativeProgressFileName).size() == 0)
            progressUnavailable(QStringLiteral("Native solver did not produce a readable progress journal."));
        if (!progressEventObserved && !progressUnavailableReported)
            progressUnavailable(QStringLiteral("Native solver produced no valid progress events."));
        processExited = this->solverProcess->state() == QProcess::NotRunning;
        if (processExited) processExitCode = this->solverProcess->exitCode();
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver execution was cancelled.");
        }
        if (!processExited || this->solverProcess->exitStatus() != QProcess::NormalExit || processExitCode != 0)
        {
            RLogger::warning("Command \'%s\' failed with exit code = %d.\n",cmdLine.toUtf8().constData(),processExitCode);

            throw RError(RError::Type::Application,R_ERROR_REF,"Solver execution failed.");
        }

        delete this->solverProcess;
        this->solverProcess = nullptr;

        RLogger::info("Command \'%s\' succesfully finished.\n",cmdLine.toUtf8().constData());

        // Read the saved result back into the detached task model first. The
        // complete artifact remains at this path even if the source changed.
        // Hash both sides of the readback so a replacement during parsing can
        // never be registered as a verified native result.
        QString sourceBeforeError;
        const auto sourceBefore = RProjectCatalog::resolveSourceBinding(
                    this->resultArtifactFileName, &sourceBeforeError);
        if (!sourceBefore)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The native result source chain could not be resolved before readback: %s",
                         sourceBeforeError.toUtf8().constData());
        }
        const QByteArray resultHashBeforeRead = sha256File(this->resultArtifactFileName);
        if (resultHashBeforeRead.isEmpty())
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The native result artifact could not be hashed before readback.");
        }
        RProjectCatalog::SourceBinding loadedSourceBinding;
        try
        {
            ModelIO::update(*this->resultSnapshot, this->resultArtifactFileName, &loadedSourceBinding);
        }
        catch (const RError &error)
        {
            RLogger::error("%s File: '%s'\n",error.getMessage().toUtf8().constData(),this->resultArtifactFileName.toUtf8().constData());
            throw RError(RError::Type::Application,R_ERROR_REF,"Failed to finish the solver because its result artifact could not be opened.");
        }
        QString sourceAfterError;
        const auto sourceAfter = RProjectCatalog::resolveSourceBinding(
                    this->resultArtifactFileName, &sourceAfterError);
        if (!sourceAfter || loadedSourceBinding != *sourceBefore || loadedSourceBinding != *sourceAfter)
        {
            const QString reason = !sourceAfter ? sourceAfterError
                    : QStringLiteral("The native result source chain changed while it was being read back.");
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The native result source chain was not stable during readback: %s",
                         reason.toUtf8().constData());
        }
        const QByteArray resultHashAfterRead = sha256File(this->resultArtifactFileName);
        if (resultHashAfterRead.isEmpty() || resultHashAfterRead != resultHashBeforeRead)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The native result artifact changed while it was being read back.");
        }
        const QString rawLogicalSha256 = QString::fromLatin1(resultHashAfterRead.toHex());
        if (loadedSourceBinding.logicalSourcePath.isEmpty() ||
            loadedSourceBinding.resolvedSourcePath.isEmpty() ||
            loadedSourceBinding.sourceArtifacts.isEmpty() ||
            loadedSourceBinding.sourceArtifacts.constFirst().path != loadedSourceBinding.logicalSourcePath ||
            loadedSourceBinding.sourceArtifacts.constFirst().sha256 != rawLogicalSha256 ||
            loadedSourceBinding.sourceArtifacts.constLast().path != loadedSourceBinding.resolvedSourcePath ||
            loadedSourceBinding.sourceArtifacts.constLast().sha256 != loadedSourceBinding.savedArtifactHash)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "The native result source binding does not match its raw logical artifact hash.");
        }
        resultArtifactPath = QFileInfo(this->resultArtifactFileName).absoluteFilePath();
        resultArtifactSha256 = rawLogicalSha256;
        resultSourceBinding = sourceBindingJson(loadedSourceBinding);
        resultArtifactVerified = true;
        resultReadbackComplete = true;
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver result publication was cancelled.");
        }
        emit this->resultArtifactCreated(resultArtifactPath, resultArtifactSha256, resultSourceBinding);
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "Solver result was retained as an artifact after cancellation and was not published.");
        }

        const QString resultDirectory = QFileInfo(this->resultArtifactFileName).absolutePath();
        const auto verifyReadbackBinding = [&]()
        {
            QString error;
            const auto current = RProjectCatalog::resolveSourceBinding(this->resultArtifactFileName, &error);
            if (!current || *current != loadedSourceBinding)
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "The native result source chain changed after readback and before publication.");
        };
        verifyReadbackBinding();
        QJsonObject publicationProvenance = this->nativeResultProvenance;
        QJsonArray retainedHistory;
        for (const QJsonValue &history : reusedHistory)
            if (!producedFramePaths.contains(history.toObject().value(QStringLiteral("relativePath")).toString()))
                retainedHistory.append(history);
        if (!retainedHistory.isEmpty()) publicationProvenance.insert(QStringLiteral("reusedHistory"), retainedHistory);
        const RNativeResultBroker::PublishResult manifestPublication =
                RNativeResultBroker::publishResult(resultDirectory,
                                                   this->resultArtifactFileName,
                                                   publicationProvenance);
        verifyReadbackBinding();
        if (!manifestPublication.valid)
        {
            if (!this->expectsNativeResultCatalog() &&
                manifestPublication.code == QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"))
            {
                emit this->nativeResultCatalogUnavailable(this->taskID.toString(),
                                                          manifestPublication.code,
                                                          manifestPublication.message);
            }
            else
            {
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "Native result manifest publication failed (%s): %s",
                             manifestPublication.code.toUtf8().constData(),
                             manifestPublication.message.toUtf8().constData());
            }
        }
        else
        {
            const QJsonObject terminal = manifestPublication.manifest.value(QStringLiteral("terminalModel")).toObject();
            if (QFileInfo(terminal.value(QStringLiteral("relativePath")).toString()).fileName() !=
                    terminal.value(QStringLiteral("relativePath")).toString() ||
                QFileInfo(QDir(resultDirectory).filePath(terminal.value(QStringLiteral("relativePath")).toString())).canonicalFilePath() !=
                    loadedSourceBinding.resolvedSourcePath ||
                terminal.value(QStringLiteral("sha256")).toString() != loadedSourceBinding.savedArtifactHash)
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "The native result manifest terminal MODEL does not match readback.");
            QFile publishedFile(manifestPublication.manifestPath);
            if (!publishedFile.open(QIODevice::ReadOnly) || publishedFile.size() > 16*1024*1024)
                throw RError(RError::Type::Application, R_ERROR_REF, "Published manifest readback is unavailable or oversized.");
            const QByteArray publishedBytes = publishedFile.readAll();
            QJsonParseError manifestParseError;
            const auto storedManifest = QJsonDocument::fromJson(publishedBytes, &manifestParseError);
            if (publishedFile.error() != QFileDevice::NoError || !storedManifest.isObject()
                || manifestParseError.error != QJsonParseError::NoError
                || QString::fromLatin1(QCryptographicHash::hash(publishedBytes, QCryptographicHash::Sha256).toHex()) != manifestPublication.manifestSha256)
                throw RError(RError::Type::Application, R_ERROR_REF, "Published manifest bytes changed before descriptor emission.");
            const QJsonObject descriptor{
                {QStringLiteral("jobId"), this->taskID.toString()},
                {QStringLiteral("resultArtifactId"), manifestPublication.artifactId},
                {QStringLiteral("resultManifestRef"), QFileInfo(manifestPublication.manifestPath).absoluteFilePath()},
                {QStringLiteral("resultManifestSha256"), manifestPublication.manifestSha256},
                {QStringLiteral("resultManifest"), storedManifest.object()},
                {QStringLiteral("resultProvenance"), this->nativeResultProvenance},
                {QStringLiteral("provenanceFrozen"), this->nativeResultProvenanceFrozen},
                {QStringLiteral("nativeResultCatalogAvailable"), true}};
            emit this->nativeResultManifestPublished(this->taskID.toString(), descriptor);
        }

        Session *session = Application::instance()->getSession();
        bool accepted = false;
        bool publicationCancelled = false;
        QString publicationError;
        const auto publishResult = [this, session, &accepted, &publicationCancelled, &publicationError]()
        {
            // Cancellation can arrive after the worker's pre-post check but
            // before this GUI-thread callback runs. Check again immediately
            // before touching the live document.
            if (this->killRequested.load(std::memory_order_acquire))
            {
                publicationCancelled = true;
                return;
            }
            // Recovery is an independent artifact even if an old source stamp
            // happens to match a live document in this host instance.
            if (this->isNativeRecoveryTask()) return;
            accepted = session->acceptSolverResult(this->sourceStamp,
                                                   *this->resultSnapshot,
                                                   this->resultArtifactFileName,
                                                   &publicationError);
        };
        if (QThread::currentThread() == session->thread())
        {
            publishResult();
        }
        else if (!QMetaObject::invokeMethod(session, publishResult, Qt::BlockingQueuedConnection))
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "Failed to publish solver result on the document thread.");
        }
        if (publicationCancelled)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "Solver result was retained as an artifact after cancellation and was not published.");
        }
        if (!publicationError.isEmpty())
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "%s", publicationError.toUtf8().constData());
        }
        if (!accepted && !this->isNativeRecoveryTask())
        {
            RLogger::warning("Solver result (#%s) completed for a stale or closed source; retained at '%s'.\n",
                             this->taskID.toString().toUtf8().constData(),
                             this->resultArtifactFileName.toUtf8().constData());
        }

        RLogger::unindent();
        loggerIndented = false;
        RLogger::info("Solver task (#%s) - End\n",this->taskID.toString().toUtf8().constData());
        if (!processExited || !resultReadbackComplete)
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "Solver completion evidence is incomplete; the result was not marked successful.");
        }
        reportTerminal(QStringLiteral("succeeded"), QString());
    }
    catch (const RError &error)
    {
        if (this->solverProcess)
        {
            if (this->solverProcess->state() != QProcess::NotRunning)
            {
                this->solverProcess->kill();
                if (!this->solverProcess->waitForFinished(5000))
                {
                    RLogger::warning("Solver process did not exit promptly during task cleanup.\n");
                    // A cancelled native process must be fully reaped before this task
                    // reports a terminal state or releases its QProcess object.
                    this->solverProcess->waitForFinished(-1);
                }
            }
            processExited = processStarted && this->solverProcess->state() == QProcess::NotRunning;
            if (processExited) processExitCode = this->solverProcess->exitCode();
            if (processStarted)
            {
                drainNativeProgress();
                drainNativeResults();
                if (!progressTail.isEmpty())
                {
                    progressDiagnostic(QStringLiteral("Discarded incomplete final native progress line."));
                    progressUnavailable(QStringLiteral("Native progress journal ended with an incomplete record."));
                    progressTail.clear();
                }
                if (!progressFileObserved || QFileInfo(this->nativeProgressFileName).size() == 0)
                    progressUnavailable(QStringLiteral("Native solver did not produce a readable progress journal."));
                if (!progressEventObserved && !progressUnavailableReported)
                    progressUnavailable(QStringLiteral("Native solver produced no valid progress events."));
            }
            delete this->solverProcess;
            this->solverProcess = nullptr;
        }
        if (loggerIndented) RLogger::unindent();
        RLogger::error("Solver task (#%s) failed: %s\n",this->taskID.toString().toUtf8().constData(),error.getMessage().toUtf8().constData());
        reportTerminal(this->killRequested.load(std::memory_order_acquire)
                           ? QStringLiteral("cancelled") : QStringLiteral("failed"),
                       error.getMessage());
        return 1;
    }
    return 0;
}

void SolverTask::onProcessReadyReadStandardOutput()
{
    emit this->readyReadStandardOutput(this->solverProcess->readAllStandardOutput());
}

void SolverTask::onProcessReadyReadStandardError()
{
    const QString message = QString::fromLocal8Bit(this->solverProcess->readAllStandardError());
    const QString marker = QStringLiteral("NativeProgressUnavailable:");
    const qsizetype markerPosition = message.indexOf(marker);
    if (markerPosition >= 0)
    {
        const QString reason = message.mid(markerPosition + marker.size()).trimmed();
        emit this->nativeProgressUnavailable(this->taskID.toString(),
                                             reason.isEmpty()
                                                 ? QStringLiteral("Native progress writer reported an unavailable journal.")
                                                 : reason.left(1024));
    }
    emit this->readyReadStandardError(message);
}
