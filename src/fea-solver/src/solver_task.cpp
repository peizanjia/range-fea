#include <omp.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>

#include <rbl_error.h>
#include <rbl_logger.h>

#include <rml_file_utils.h>
#include <rml_file_io.h>

#include <rparallel_execution_context.h>
#include <rsolver.h>
#include <rstructural_thermal_fe_adapter.h>
#include <rstructural_control_conditioner.h>
#include <rstructural_truss_dynamics_restart_adapter.h>
#include <rstructural_truss_native_run.h>
#include "typed_control_task.h"

#include "execution_event_journal.h"
#include "control_ndjson_transport.h"
#include "native_progress_journal.h"
#include "native_result_journal.h"
#include "native_accepted_checkpoint.h"
#include "native_execution_gate.h"
#include "runtime_control_bridge.h"
#include "solver_task.h"

namespace
{
NativeAcceptedCheckpointStateView mixedCheckpointStateView(const RModel &model)
{
    const RStructuralTrussDynamicsModelAdapter adapter(model);
    auto record = model.getProblemSetup().getStructuralTrussDynamicsRestartData();
    auto control = model.getProblemSetup().getStructuralNativeControlState();
    if (record.available)
        RStructuralTrussDynamicsRestartAdapter::restore(adapter,record,control);
    else
    {
        RModel fresh(model);
        fresh.getProblemSetup().setRestart(false);
        const RStructuralTrussNativeRun initialized(fresh);
        control = initialized.acceptedControlState();
        record = RStructuralTrussDynamicsRestartAdapter::build(
            adapter,initialized.acceptedState(),0,0,control);
    }
    record.validate();
    const QByteArray fingerprint = QByteArray::fromHex(
        RStructuralTrussDynamicsRestartAdapter::stateFingerprint(record,control).toLatin1());
    if (fingerprint.size() != 32)
        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                     "Native mixed checkpoint state fingerprint is not SHA-256.");
    return {record.completedSteps,record.time,record.fixedTimeStep,fingerprint};
}

class ResultFrameObserverScope final
{
public:
    explicit ResultFrameObserverScope(RSolver &solver)
        : solver(solver)
    {
    }

    ~ResultFrameObserverScope()
    {
        this->solver.setResultFrameObserver({});
    }

    ResultFrameObserverScope(const ResultFrameObserverScope &) = delete;
    ResultFrameObserverScope &operator=(const ResultFrameObserverScope &) = delete;

private:
    RSolver &solver;
};

QString sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        throw RError(RError::Type::ReadFile,R_ERROR_REF,
                     "Cannot read '%s' for SHA-256: %s",
                     qPrintable(path),qPrintable(file.errorString()));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
    {
        throw RError(RError::Type::ReadFile,R_ERROR_REF,
                     "Cannot hash '%s': %s",qPrintable(path),qPrintable(file.errorString()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString canonicalTargetPath(const QString &fileName)
{
    QString path = QFileInfo(fileName).absoluteFilePath();
    QSet<QString> visited;
    for (uint depth=0;depth<16;depth++)
    {
        if (visited.contains(path))
            throw RError(RError::Type::InvalidFileName,R_ERROR_REF,
                         "Checkpoint target symlink cycle starts at '%s'.",qPrintable(path));
        visited.insert(path);
        const QFileInfo info(path);
        if (info.exists())
        {
            const QString canonical = info.canonicalFilePath();
            if (!canonical.isEmpty()) return QDir::cleanPath(canonical);
        }
        if (info.isSymLink())
        {
            const QString target = info.symLinkTarget();
            if (target.isEmpty())
                throw RError(RError::Type::InvalidFileName,R_ERROR_REF,
                             "Cannot resolve checkpoint target symlink '%s'.",qPrintable(path));
            path = QFileInfo(target).absoluteFilePath();
            continue;
        }
        const QFileInfo parentInfo(info.absolutePath());
        const QString canonicalParent = parentInfo.canonicalFilePath();
        const QString parent = canonicalParent.isEmpty()
            ? QDir::cleanPath(parentInfo.absoluteFilePath()) : QDir::cleanPath(canonicalParent);
        return QDir(parent).filePath(info.fileName());
    }
    throw RError(RError::Type::InvalidFileName,R_ERROR_REF,
                 "Checkpoint target symlink chain is too deep.");
}

QString nativeMixedWorkingOutputPath(const QString &checkpointDirectory,const QString &inputTargetPath)
{
    const QString absoluteRoot = QDir::cleanPath(QFileInfo(checkpointDirectory).absoluteFilePath());
    // The checkpoint store has already verified and locked this directory.
    // Recheck all components before creating the separate working output.
    QString component = absoluteRoot;
    for (;;)
    {
        const QFileInfo info(component);
        if (info.isSymLink())
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "Native mixed working output cannot traverse a symbolic link.");
        const QString parent = QDir::cleanPath(info.absolutePath());
        if (parent == component) break;
        component = parent;
    }
    const QString canonicalRoot = QFileInfo(absoluteRoot).canonicalFilePath();
    if (canonicalRoot.isEmpty() || !QFileInfo(canonicalRoot).isDir())
        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                     "Native mixed working output requires the verified checkpoint directory.");
    const QString outputDirectory = QDir(canonicalRoot).filePath(QStringLiteral("run-results"));
    const QString inputRelative = QDir(outputDirectory).relativeFilePath(
        QFileInfo(inputTargetPath).canonicalFilePath());
    if (!QDir::isAbsolutePath(inputRelative) && inputRelative != QStringLiteral("..") &&
        !inputRelative.startsWith(QStringLiteral("../")))
        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                     "Native mixed input MODEL cannot occupy the reserved run-results output directory; restart from the verified checkpoint MODEL.");
    QFileInfo outputInfo(outputDirectory);
    if (outputInfo.isSymLink() || (outputInfo.exists() && !outputInfo.isDir()))
        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                     "Native mixed run-results must be a regular directory.");
    if (!QDir().mkpath(outputDirectory))
        throw RError(RError::Type::WriteFile,R_ERROR_REF,
                     "Cannot create native mixed working output directory.");
    outputInfo.refresh();
    if (outputInfo.isSymLink() || !outputInfo.isDir() ||
        QDir::cleanPath(outputInfo.canonicalFilePath()) != QDir::cleanPath(outputDirectory))
        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                     "Native mixed working output escaped its checkpoint directory.");
    const QString outputPath = QDir(outputDirectory).filePath(
        QStringLiteral("native-mixed.") + RModel::getDefaultFileExtension(true));
    const QFileInfo modelInfo(outputPath);
    if (modelInfo.isSymLink() || (modelInfo.exists() && !modelInfo.isFile()))
        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                     "Native mixed working MODEL path must be a regular file or absent.");
    return outputPath;
}

QString resolveModelTargetPath(const QString &inputPath)
{
    QString path = QFileInfo(inputPath).absoluteFilePath();
    QSet<QString> visited;
    for (uint depth=0;depth<16;depth++)
    {
        if (visited.contains(path))
        {
            throw RError(RError::Type::InvalidFileFormat,R_ERROR_REF,
                         "Model link cycle starts at '%s'.",qPrintable(path));
        }
        visited.insert(path);
        const QString extension = RFileUtils::getExtension(path);
        const bool ascii = extension == RModel::getDefaultFileExtension(false);
        const bool binary = extension == RModel::getDefaultFileExtension(true);
        if (!ascii && !binary)
        {
            throw RError(RError::Type::InvalidFileName,R_ERROR_REF,
                         "Model link '%s' has an unsupported extension.",qPrintable(path));
        }
        RFile file(path,ascii ? RFile::ASCII : RFile::BINARY);
        QIODevice::OpenMode mode = QIODevice::ReadOnly;
        if (ascii)
        {
            mode |= QIODevice::Text;
        }
        if (!file.open(mode))
        {
            throw RError(RError::Type::OpenFile,R_ERROR_REF,
                         "Cannot read model header from '%s'.",qPrintable(path));
        }
        RFileHeader header;
        if (ascii)
        {
            RFileIO::readAscii(file,header);
        }
        else
        {
            RFileIO::readBinary(file,header);
        }
        if (header.getType() == R_FILE_TYPE_MODEL)
        {
            return path;
        }
        if (header.getType() != R_FILE_TYPE_LINK)
        {
            throw RError(RError::Type::InvalidFileFormat,R_ERROR_REF,
                         "File '%s' is neither a model nor a model link.",qPrintable(path));
        }
        path = QFileInfo(RFileUtils::findLinkTargetFileName(path,header.getInformation()))
                   .absoluteFilePath();
    }
    throw RError(RError::Type::InvalidFileFormat,R_ERROR_REF,
                 "Model link chain is too deep.");
}

QString reserveJournalPath(const QString &inputPath, QUuid &runId)
{
    const QFileInfo inputInfo(inputPath);
    const QDir directory(inputInfo.absolutePath());
    for (uint attempt=0;attempt<8;attempt++)
    {
        runId = ExecutionEventJournal::createRunId();
        const QString path = directory.filePath(inputInfo.fileName() + QLatin1Char('.') +
            runId.toString(QUuid::WithoutBraces) + QStringLiteral(".events.jsonl"));
        QFile reservation(path);
        if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            reservation.close();
            return path;
        }
        if (!QFileInfo::exists(path))
        {
            throw RError(RError::Type::WriteFile,R_ERROR_REF,
                         "AuditWriteFailed: cannot create journal '%s': %s",
                         qPrintable(path),qPrintable(reservation.errorString()));
        }
    }
    throw RError(RError::Type::WriteFile,R_ERROR_REF,
                 "AuditWriteFailed: could not reserve a unique event journal path.");
}

QString parallelModeName(RParallelExecutionMode mode)
{
    switch (mode)
    {
        case RParallelExecutionMode::Auto: return QStringLiteral("Auto");
        case RParallelExecutionMode::SerialDeterministic: return QStringLiteral("SerialDeterministic");
        case RParallelExecutionMode::FixedThreads: return QStringLiteral("FixedThreads");
    }
    return QStringLiteral("Unknown");
}

QJsonObject modelParallelConfig(const RParallelExecutionConfig &config)
{
    return {{QStringLiteral("mode"),parallelModeName(config.getMode())},
            {QStringLiteral("requestedThreads"),double(config.getRequestedThreads())},
            {QStringLiteral("deterministicReduction"),config.getDeterministicReduction()}};
}

QJsonObject parallelReportPayload(const RParallelExecutionReport &report)
{
    return {{QStringLiteral("mode"),parallelModeName(report.mode)},
            {QStringLiteral("requestedThreads"),double(report.requestedThreads)},
            {QStringLiteral("appliedThreads"),double(report.appliedThreads)},
            {QStringLiteral("actualThreads"),double(report.actualThreads)},
            {QStringLiteral("actualThreadsMeaning"),QStringLiteral("context-entry scope probe; not per-kernel")},
            {QStringLiteral("availableThreads"),double(report.availableThreads)},
            {QStringLiteral("backend"),report.backend},
            {QStringLiteral("deterministicReductionRequested"),report.deterministicReductionRequested},
            {QStringLiteral("deterministicReduction"),report.deterministicReduction},
            {QStringLiteral("fallback"),report.fallback},
            {QStringLiteral("diagnostic"),report.diagnostic}};
}

QString controllerRevision(const QString &program, const QStringList &arguments)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    auto addField = [&hash](const QByteArray &bytes)
    {
        hash.addData(QByteArray::number(bytes.size()));
        hash.addData(":",1);
        hash.addData(bytes);
    };
    addField(QByteArrayLiteral("range-control-transport-v1"));
    addField(QFileInfo(program).canonicalFilePath().toUtf8());
    addField(sha256File(program).toUtf8());
    for (const QString &argument : arguments)
    {
        addField(argument.toUtf8());
        // Both '--policy path' and '--policy=path' are conventional argv forms.
        // Bind the latter's file contents too, so edited weights cannot retain
        // the same accepted checkpoint identity merely by using an '=' option.
        QString argumentPath = argument;
        if (argument.startsWith(QStringLiteral("--")))
        {
            const qsizetype separator = argument.indexOf(QLatin1Char('='));
            if (separator > 2) argumentPath = argument.mid(separator+1);
        }
        const QFileInfo file(argumentPath);
        // Script/config arguments affect controller behaviour and restart identity.
        if (file.isFile())
        {
            addField(file.canonicalFilePath().toUtf8());
            addField(sha256File(file.absoluteFilePath()).toUtf8());
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonArray controlSamplesJson(const RStructuralControlStepRecord &record)
{
    QJsonArray channels;
    for (const auto &sample : record.appliedChannels)
        channels.append(QJsonObject{{QStringLiteral("actuatorId"),sample.actuatorId},
                                    {QStringLiteral("appliedForceN"),sample.command},
                                    {QStringLiteral("endpointPowerW"),sample.endpointPower},
                                    {QStringLiteral("cumulativeWorkJ"),sample.cumulativeWork}});
    return channels;
}

bool isSupportedThermalTaskTree(const RProblemTaskItem &root)
{
    return root.getProblemType() == R_PROBLEM_NONE && root.getNIterations() == 1 &&
           root.getNChildren() == 2 &&
           root.getChild(0).getProblemType() == R_PROBLEM_HEAT &&
           root.getChild(0).getNChildren() == 0 &&
           root.getChild(1).getProblemType() == R_PROBLEM_STRESS &&
           root.getChild(1).getNChildren() == 0;
}
}

SolverTask::SolverTask(const SolverInput &solverInput, QCoreApplication *app, QObject *parent)
    : QObject(parent)
    , modelFileName(solverInput.modelFileName)
    , convergenceFileName(solverInput.convergenceFileName)
    , monitoringFileName(solverInput.monitoringFileName)
    , nThreads(solverInput.nThreads)
    , requestedNThreads(solverInput.nThreads)
    , controlProgram(solverInput.controlProgram)
    , controlArguments(solverInput.controlArguments)
    , controlTimeoutMs(solverInput.controlTimeoutMs)
    , thermalMacrosteps(solverInput.thermalMacrosteps)
    , thermalCheckpointFileName(solverInput.thermalCheckpointFileName)
    , typedControlSteps(solverInput.typedControlSteps)
    , typedCheckpointFileName(solverInput.typedCheckpointFileName)
    , typedControllerIdentity(solverInput.typedControllerIdentity)
    , typedConfigurationFileName(solverInput.typedConfigurationFileName)
    , nativeCheckpointDirectory(solverInput.nativeCheckpointDirectory)
    , nativeCheckpointContextJson(solverInput.nativeCheckpointContextJson)
    , nativeMixedStepBudget(solverInput.nativeMixedStepBudget)
    , nativeExecutionGate(solverInput.nativeExecutionGate)
    , restart(solverInput.restart)
    , app(app)
{
    if (this->nThreads)
    {
        *this->nThreads = std::max(*this->nThreads,uint(1));
        *this->nThreads = std::min(*this->nThreads,
                                  uint(std::max(1,omp_get_max_threads())));
    }
}

void SolverTask::run(void)
{
    if (this->typedControlSteps>0)
    {
        // Typed bundles have their own reader. A native source must be checked
        // before that mode can produce any candidate/checkpoint output.
        try
        {
            if (this->nativeMixedStepBudget)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native mixed step budgets cannot combine with typed FE mode.");
            if (!this->typedConfigurationFileName.isEmpty())
            {
                RModel source;
                source.read(this->modelFileName);
                if (source.getProblemSetup().getStructuralTrussDynamicsSetup().enabled)
                    throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                                 "Native mixed dynamics cannot combine with typed FE mode.");
            }
        }
        catch (const RError &error)
        {
            RLogger::error("%s\n",qPrintable(error.getMessage()));
            this->app->exit(1);
            emit this->finished();
            return;
        }
        TypedControlTaskInput input;
        input.inputFile=this->modelFileName;
        input.checkpointFile=this->typedCheckpointFileName;
        input.controllerIdentity=this->typedControllerIdentity;
        input.configurationFile=this->typedConfigurationFileName;
        input.program=this->controlProgram;
        input.arguments=this->controlArguments;
        input.timeoutMs=this->controlTimeoutMs;
        input.steps=this->typedControlSteps;
        input.nThreads=this->nThreads;
        QString error;
        const int exitCode=runTypedControlTask(input,error);
        if (exitCode) RLogger::error("%s\n",qPrintable(error));
        this->app->exit(exitCode);
        emit this->finished();
        return;
    }
    const QString progressJobId = qEnvironmentVariable(
        NativeProgressJournal::jobIdEnvironmentVariable);
    const QString progressPath = qEnvironmentVariable(
        NativeProgressJournal::pathEnvironmentVariable);
    const bool progressRequested = !progressJobId.isEmpty() || !progressPath.isEmpty();
    QString progressError;
    std::unique_ptr<NativeProgressJournal> progressJournal =
        NativeProgressJournal::createFromEnvironment(&progressError);
    NativeProgressHandlerScope progressHandlerScope(progressJournal.get());
    bool progressUnavailableReported = false;
    auto reportProgressUnavailable = [&]()
    {
        if (!progressRequested || progressUnavailableReported)
        {
            return;
        }
        QString reason = progressError;
        if (reason.isEmpty() && progressJournal)
        {
            reason = progressJournal->unavailableReason();
        }
        if (reason.isEmpty() && progressJournal && progressJournal->isAvailable())
        {
            return;
        }
        if (reason.isEmpty())
        {
            reason = QStringLiteral("Native progress sidechannel was not initialized");
        }
        RLogger::warning("NativeProgressUnavailable: %s\n",qPrintable(reason));
        progressUnavailableReported = true;
    };
    if (progressRequested && !progressJournal)
    {
        reportProgressUnavailable();
    }
    RModel model;
    QUuid runId;
    QString journalPath;
    std::unique_ptr<ExecutionEventJournal> journal;
    QString phase = QStringLiteral("audit-reservation");
    bool auditWriteFailed = false;
    bool loggerIndented = false;
    bool solverEntered = false;
    bool finalModelWritten = false;
    bool nativeMixedMode = false;
    bool nativeMixedSourceValidated = false;
    std::shared_ptr<NativeAcceptedCheckpointStore> acceptedCheckpointStore;
    std::unique_ptr<ControlNdjsonTransport> controlTransport;
    std::shared_ptr<RuntimeParameterGate> runtimeParameterGate;
    std::optional<QString> frozenRuntimeProposal;
    RuntimeParameterGate::State initialRuntimeState;
    QJsonObject runtimeIdentity;
    const QString inputPath = QFileInfo(this->modelFileName).absoluteFilePath();
    QString solverModelFileName = this->modelFileName;
    const bool thermalMode = this->thermalMacrosteps > 0;
    const QString requestedThermalCheckpointPath = thermalMode
        ? QFileInfo(this->thermalCheckpointFileName).absoluteFilePath() : QString();
    QString thermalCheckpointPath = requestedThermalCheckpointPath;
    std::unique_ptr<QLockFile> thermalCheckpointLock;
    bool thermalCheckpointSnapshotTaken = false;
    bool thermalCheckpointOriginalExists = false;
    QString thermalCheckpointOriginalHash;

    auto appendAudit = [&](const QString &type, const QString &status,
                           const QJsonObject &payload)
    {
        QString error;
        if (!journal || !journal->append(type,status,payload,QUuid::createUuid(),&error))
        {
            auditWriteFailed = true;
            throw RError(RError::Type::WriteFile,R_ERROR_REF,
                         "AuditWriteFailed: journal '%s': %s",
                         qPrintable(journalPath),qPrintable(error));
        }
    };
    auto finishWithFailure = [&](const QString &message)
    {
        if (runtimeParameterGate && frozenRuntimeProposal)
        {
            const auto rolledBack=runtimeParameterGate->onRollback(*frozenRuntimeProposal);
            frozenRuntimeProposal.reset();
            if (rolledBack.receipt && this->runtimeControlBridge)
                this->runtimeControlBridge->publish(*rolledBack.receipt);
        }
        if (this->runtimeControlBridge) this->runtimeControlBridge->stop();
        reportProgressUnavailable();
        if (loggerIndented)
        {
            RLogger::unindent();
            loggerIndented = false;
        }
        if (journal && !auditWriteFailed)
        {
            QString error;
            const bool thermalCheckpointExists = thermalMode &&
                QFileInfo::exists(thermalCheckpointPath);
            bool preservedThermalCheckpoint = false;
            if (thermalMode && !finalModelWritten && thermalCheckpointSnapshotTaken &&
                thermalCheckpointOriginalExists && thermalCheckpointExists)
            {
                try
                {
                    preservedThermalCheckpoint =
                        sha256File(thermalCheckpointPath) == thermalCheckpointOriginalHash;
                }
                catch (...) {}
            }
            QString failureMessage = message;
            if (thermalMode && finalModelWritten)
                failureMessage += QStringLiteral(
                    " Thermal checkpoint was already atomically published to '%1'; this process failed afterward, so the checkpoint may be committed despite the nonzero exit.")
                    .arg(thermalCheckpointPath);
            QJsonObject payload{
                {QStringLiteral("phase"),phase},
                {QStringLiteral("message"),failureMessage},
                {QStringLiteral("resultLinkPath"),thermalMode ? thermalCheckpointPath
                    : QFileInfo(solverModelFileName).absoluteFilePath()},
                {QStringLiteral("resultsMayAlreadyExist"),solverEntered || finalModelWritten ||
                    thermalCheckpointExists},
                {QStringLiteral("thermalCheckpointPreserved"),preservedThermalCheckpoint},
                {QStringLiteral("thermalCheckpointCommitted"),thermalMode && finalModelWritten}
            };
            if (nativeMixedMode)
            {
                const auto &accepted = model.getProblemSetup().getStructuralTrussDynamicsRestartData();
                const bool acceptedAvailable = nativeMixedSourceValidated && accepted.available;
                payload.insert(QStringLiteral("nativeMixedAcceptedStateAvailable"),acceptedAvailable);
                if (acceptedAvailable)
                {
                    payload.insert(QStringLiteral("lastAcceptedSequence"),double(accepted.completedSteps));
                    payload.insert(QStringLiteral("lastAcceptedTimeS"),accepted.time);
                }
                const auto *receipt = acceptedCheckpointStore
                    ? acceptedCheckpointStore->committedReceipt() : nullptr;
                payload.insert(QStringLiteral("nativeCheckpointCommitted"),receipt != nullptr);
                if (receipt)
                {
                    payload.insert(QStringLiteral("nativeCheckpointModelPath"),receipt->modelPath);
                    payload.insert(QStringLiteral("nativeCheckpointAcceptedSequence"),
                                   receipt->envelope.value(QStringLiteral("acceptedSeq")));
                }
                if (acceptedCheckpointStore)
                    payload.insert(QStringLiteral("nativeCheckpointStoreRetryable"),
                                   acceptedCheckpointStore->isRetryable());
            }
            if (!journal->append(QStringLiteral("RunFailed"),QStringLiteral("failed"),
                                 payload,QUuid::createUuid(),&error))
            {
                auditWriteFailed = true;
                RLogger::error("AuditWriteFailed: could not record RunFailed in '%s': %s\n",
                               qPrintable(journalPath),qPrintable(error));
            }
        }
        QString failureLogMessage = message;
        if (thermalMode && finalModelWritten)
            failureLogMessage += QStringLiteral(
                " Thermal checkpoint was already atomically published to '%1'; this process failed afterward, so the checkpoint may be committed despite the nonzero exit.")
                .arg(thermalCheckpointPath);
        RLogger::error("Failed during %s for model '%s': %s\n",
                       qPrintable(phase),qPrintable(inputPath),qPrintable(failureLogMessage));
        if (auditWriteFailed || solverEntered || finalModelWritten)
        {
            RLogger::warning("Previous model result files may already exist; no rollback is claimed. Audit journal: '%s'\n",
                             qPrintable(journalPath));
        }
        thermalCheckpointLock.reset();
        journal.reset();
        emit this->finished();
        this->app->exit(1);
    };

    try
    {
        journalPath = reserveJournalPath(inputPath,runId);
        journal.reset(new ExecutionEventJournal(journalPath,runId));
        RLogger::info("Audit journal: '%s' (run ID %s)\n",
                      qPrintable(journalPath),qPrintable(runId.toString(QUuid::WithoutBraces)));

        phase = QStringLiteral("input-hash");
        const bool thermalCheckpointInput = thermalMode &&
            QFileInfo(inputPath).suffix().compare(QStringLiteral("tfec"),Qt::CaseInsensitive) == 0;
        if (thermalMode)
        {
            if (QFileInfo(requestedThermalCheckpointPath).suffix().compare(
                    QStringLiteral("tfec"),Qt::CaseInsensitive) != 0)
                throw RError(RError::Type::InvalidFileName,R_ERROR_REF,
                             "--thermal-checkpoint must name a .tfec file.");
            if (!thermalCheckpointInput)
            {
                const QString extension = QFileInfo(inputPath).suffix();
                if (extension.compare(RModel::getDefaultFileExtension(false),Qt::CaseInsensitive) != 0 &&
                    extension.compare(RModel::getDefaultFileExtension(true),Qt::CaseInsensitive) != 0)
                    throw RError(RError::Type::InvalidFileName,R_ERROR_REF,
                                 "Thermal FE input must be a .ram/.rbm model or a .tfec checkpoint.");
            }
            thermalCheckpointPath = canonicalTargetPath(requestedThermalCheckpointPath);
            phase = QStringLiteral("thermal-checkpoint-lock");
            thermalCheckpointLock.reset(
                new QLockFile(thermalCheckpointPath+QStringLiteral(".lock")));
            thermalCheckpointLock->setStaleLockTime(0);
            if (!thermalCheckpointLock->tryLock(0))
            {
                qint64 ownerPid = 0;
                QString ownerHost,ownerApp;
                QString ownerInfo;
                if (thermalCheckpointLock->error() == QLockFile::LockFailedError &&
                    thermalCheckpointLock->getLockInfo(&ownerPid,&ownerHost,&ownerApp))
                    ownerInfo = QStringLiteral(" (owner pid %1, host '%2')")
                                    .arg(ownerPid).arg(ownerHost);
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "ThermalCheckpointLocked: cannot acquire the checkpoint writer lock for '%s'%s.",
                             qPrintable(thermalCheckpointPath),qPrintable(ownerInfo));
            }
            thermalCheckpointOriginalExists = QFileInfo::exists(thermalCheckpointPath);
            if (thermalCheckpointOriginalExists)
                thermalCheckpointOriginalHash = sha256File(thermalCheckpointPath);
            thermalCheckpointSnapshotTaken = true;
            phase = QStringLiteral("input-hash");
        }
        const QString inputArgumentHash = sha256File(inputPath);
        const QString inputTargetPath = thermalCheckpointInput ? inputPath
                                                               : resolveModelTargetPath(inputPath);
        const QString inputTargetHash = thermalCheckpointInput ? inputArgumentHash
                                                                : sha256File(inputTargetPath);
        std::optional<NativeAcceptedCheckpointContext> nativeCheckpointContext;
        if (this->nativeCheckpointDirectory.isEmpty())
        {
            if (!this->nativeCheckpointContextJson.isEmpty())
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native checkpoint context requires --native-checkpoint-dir.");
        }
        else
        {
            if (this->nativeCheckpointContextJson.isEmpty())
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native checkpoint directory requires a versioned context.");
            nativeCheckpointContext = NativeAcceptedCheckpointContext::parse(
                this->nativeCheckpointContextJson);
            if (!this->restart && nativeCheckpointContext->modelSha256 != inputTargetHash)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native checkpoint context modelSha256 does not match the input MODEL.");
        }
        if (this->nThreads)
        {
            RLogger::info("CLI thread override = %u (max=%d)\n",
                          *this->nThreads,omp_get_max_threads());
            omp_set_num_threads(int(*this->nThreads));
        }
        else
        {
            RLogger::info("No CLI thread override; using model/runtime parallel policy (max=%d)\n",
                          omp_get_max_threads());
        }

        phase = QStringLiteral("load-model");
        RStructuralThermalControlState acceptedThermalState;
        if (thermalCheckpointInput)
        {
            RStructuralThermalFEAdapter::readConfiguredCheckpoint(
                inputPath,model,acceptedThermalState);
        }
        else
        {
            model.read(this->modelFileName);
        }
        if (sha256File(inputPath) != inputArgumentHash ||
            (!thermalCheckpointInput && resolveModelTargetPath(inputPath) != inputTargetPath) ||
            (!thermalCheckpointInput && sha256File(inputTargetPath) != inputTargetHash))
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "Input model link or target changed while it was being loaded.");
        RLogger::info("Model has been successfully loaded from file '%s'.\n",
                      qPrintable(this->modelFileName));
        nativeMixedMode = model.getProblemSetup().getStructuralTrussDynamicsSetup().enabled;
        if (this->nativeMixedStepBudget && (!nativeMixedMode || thermalMode))
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "--native-mixed-steps requires native mixed structural dynamics and cannot combine with thermal FE mode.");
        if (nativeMixedMode)
        {
            phase = QStringLiteral("native-mixed-preflight");
            if (thermalMode || this->typedControlSteps > 0 || !this->controlProgram.isEmpty() ||
                this->runtimeControlBridge || RuntimeControlBridge::environmentConfig().has_value())
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native mixed dynamics rejects thermal, typed FE, v1/v2 force control and runtime parameter modes.");
            // CLI restart is authoritative. Fresh runs retain source initial
            // conditions and never infer state from a previous display/result.
            model.getProblemSetup().setRestart(this->restart);
            if (!this->restart)
            {
                model.getProblemSetup().setStructuralTrussDynamicsRestartData({});
                model.getProblemSetup().setStructuralNativeControlState({});
            }
            const RStructuralTrussDynamicsModelAdapter adapter(model);
            if (this->restart)
                RStructuralTrussDynamicsRestartAdapter::restore(adapter,
                    model.getProblemSetup().getStructuralTrussDynamicsRestartData(),
                    model.getProblemSetup().getStructuralNativeControlState());
            nativeMixedSourceValidated = true;
        }
        if (thermalMode && this->convergenceFileName.isEmpty())
        {
            model.getMatrixSolverConf(RMatrixSolverConf::CG).setOutputFileName(QString());
            model.getMatrixSolverConf(RMatrixSolverConf::GMRES).setOutputFileName(QString());
        }
        else
        {
            model.getMatrixSolverConf(RMatrixSolverConf::CG).setOutputFileName(
                RFileUtils::getFileNameWithSuffix(this->convergenceFileName,RMatrixSolverConf::getId(RMatrixSolverConf::CG)));
            model.getMatrixSolverConf(RMatrixSolverConf::GMRES).setOutputFileName(
                RFileUtils::getFileNameWithSuffix(this->convergenceFileName,RMatrixSolverConf::getId(RMatrixSolverConf::GMRES)));
        }
        model.getMonitoringPointManager().setOutputFileName(this->monitoringFileName);

        if (thermalMode)
        {
            phase = QStringLiteral("thermal-control-setup");
            if (!isSupportedThermalTaskTree(model.getProblemTaskTree()))
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Thermal FE CLI supports exactly one sequential Heat then Stress task pair.");
            if (model.getProblemSetup().getStructuralDynamicsSetup().getEnabled())
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Thermal FE CLI does not support structural dynamics.");
            if (model.getProblemSetup().getStructuralControlSetup().enabled)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Thermal FE mode cannot be combined with an enabled structural force controller.");

            const RStructuralThermalControlScheduler scheduler =
                RStructuralThermalFEAdapter::schedulerFromModel(model);
            if (!thermalCheckpointInput)
            {
                acceptedThermalState = scheduler.initialState(
                    RStructuralThermalFEAdapter::capture(model),
                    model.getTimeSolver().getComputedTime());
            }

            QJsonObject startedPayload{
                {QStringLiteral("inputArgumentPath"),inputPath},
                {QStringLiteral("inputArgumentSha256"),inputArgumentHash},
                {QStringLiteral("inputModelTargetPath"),inputTargetPath},
                {QStringLiteral("inputModelTargetSha256"),inputTargetHash},
                {QStringLiteral("thermalCheckpointInput"),thermalCheckpointInput},
                {QStringLiteral("thermalCheckpointPath"),thermalCheckpointPath},
                {QStringLiteral("thermalMacrostepsRequested"),double(this->thermalMacrosteps)},
                {QStringLiteral("completedMacrostepsBeforeRun"),
                 double(acceptedThermalState.completedMechanicalMacrosteps)},
                {QStringLiteral("acceptedTimeSeconds"),acceptedThermalState.acceptedTimeSeconds},
                {QStringLiteral("modelParallelConfig"),modelParallelConfig(
                    model.getProblemSetup().getParallelExecutionConfig())}
            };
            phase = QStringLiteral("thermal-fe-run");
            appendAudit(QStringLiteral("SolverRunStarted"),QStringLiteral("started"),startedPayload);
            RLogger::info("Running %u thermal FE macrosteps; checkpoint target '%s'.\n",
                          this->thermalMacrosteps,qPrintable(thermalCheckpointPath));

            QVector<QJsonObject> acceptedMacrostepPayloads;
            acceptedMacrostepPayloads.reserve(int(this->thermalMacrosteps));
            for (uint step=0;step<this->thermalMacrosteps;step++)
            {
                const auto result = RStructuralThermalFEAdapter::advance(
                    model,scheduler,acceptedThermalState);
                acceptedThermalState = result.candidate;
                QJsonArray channels;
                for (const auto &channel : result.channels)
                    channels.append(QJsonObject{
                        {QStringLiteral("actuatorId"),channel.actuatorId},
                        {QStringLiteral("requestedPowerW"),channel.requestedPowerWatts},
                        {QStringLiteral("conditionedPowerW"),channel.conditionedPowerWatts},
                        {QStringLiteral("appliedAveragePowerW"),channel.appliedAveragePowerWatts},
                        {QStringLiteral("appliedEnergyJ"),channel.appliedEnergyJoules}});
                acceptedMacrostepPayloads.append(
                    {{QStringLiteral("macrostep"),double(acceptedThermalState.completedMechanicalMacrosteps)},
                     {QStringLiteral("acceptedTimeSeconds"),acceptedThermalState.acceptedTimeSeconds},
                     {QStringLiteral("controllerSampled"),result.controllerSampled},
                     {QStringLiteral("channels"),channels},
                     {QStringLiteral("thermalCheckpointPath"),thermalCheckpointPath}});
            }

            phase = QStringLiteral("thermal-checkpoint-publish");
            const bool checkpointExistsNow = QFileInfo::exists(thermalCheckpointPath);
            bool checkpointUnchanged = checkpointExistsNow == thermalCheckpointOriginalExists;
            if (checkpointUnchanged && checkpointExistsNow)
            {
                try
                {
                    checkpointUnchanged =
                        sha256File(thermalCheckpointPath) == thermalCheckpointOriginalHash;
                }
                catch (...)
                {
                    checkpointUnchanged = false;
                }
            }
            if (!checkpointUnchanged)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "ThermalCheckpointConflict: target '%s' changed after this run began; the candidate was not published.",
                             qPrintable(thermalCheckpointPath));
            RStructuralThermalFEAdapter::writeCheckpoint(
                thermalCheckpointPath,model,scheduler,acceptedThermalState);
            finalModelWritten = true;

            for (const QJsonObject &payload : acceptedMacrostepPayloads)
                appendAudit(QStringLiteral("ThermalMacrostepAccepted"),QStringLiteral("accepted"),payload);

            phase = QStringLiteral("run-computed-audit");
            appendAudit(QStringLiteral("RunComputed"),QStringLiteral("computed"),
                {{QStringLiteral("computedTime"),model.getTimeSolver().getComputedTime()},
                 {QStringLiteral("acceptedTimeSeconds"),acceptedThermalState.acceptedTimeSeconds},
                 {QStringLiteral("completedMacrosteps"),
                  double(acceptedThermalState.completedMechanicalMacrosteps)},
                 {QStringLiteral("thermalCheckpointPath"),thermalCheckpointPath}});

            phase = QStringLiteral("thermal-checkpoint-hash");
            const QString checkpointHash = sha256File(thermalCheckpointPath);
            phase = QStringLiteral("run-finished-audit");
            appendAudit(QStringLiteral("RunFinished"),QStringLiteral("finished"),
                {{QStringLiteral("thermalCheckpointPath"),thermalCheckpointPath},
                 {QStringLiteral("thermalCheckpointSha256"),checkpointHash},
                 {QStringLiteral("completedMacrosteps"),
                  double(acceptedThermalState.completedMechanicalMacrosteps)},
                 {QStringLiteral("acceptedTimeSeconds"),acceptedThermalState.acceptedTimeSeconds}});
            thermalCheckpointLock.reset();
            journal.reset();
            emit this->finished();
            return;
        }

        if (model.getProblemSetup().getStructuralThermalControlSetup().enabled)
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "Enabled thermal control setup requires --thermal-steps and --thermal-checkpoint.");
        if (model.getProblemTaskTree().getProblemTypeMask() == R_PROBLEM_NONE)
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "No solver task is configured. A remeshed candidate requires explicit physics rebinding before analysis.");
        if (this->restart)
        {
            model.getProblemSetup().setRestart(true);
        }

        phase = QStringLiteral("control-setup");
        const RStructuralControlSetup &controlSetup =
            model.getProblemSetup().getStructuralControlSetup();
        if (controlSetup.enabled != !this->controlProgram.isEmpty())
            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                         "Enabled structural control requires --control-program; disabled control rejects one.");
        QString controlRevision;
        quint64 nextControlStepId = 1;
        if (controlSetup.enabled)
        {
            controlRevision = controllerRevision(this->controlProgram,this->controlArguments);
            const auto &restartData = model.getProblemSetup().getStructuralDynamicsRestartData();
            if (this->restart && restartData.available)
                nextControlStepId = quint64(restartData.completedSteps)+1;
        }

        if (nativeCheckpointContext)
        {
            const QString activeControlSha256 = controlSetup.enabled
                ? controlRevision : NativeAcceptedCheckpointContext::noControlSha256();
            const QString runtimeSha256 = sha256File(QCoreApplication::applicationFilePath());
            acceptedCheckpointStore.reset(new NativeAcceptedCheckpointStore(
                this->nativeCheckpointDirectory,this->nativeCheckpointContextJson,
                inputTargetHash,activeControlSha256,runtimeSha256,this->restart,model,
                nativeMixedMode ? NativeAcceptedCheckpointStore::StateViewExtractor(mixedCheckpointStateView)
                                : NativeAcceptedCheckpointStore::StateViewExtractor{}));
            if (nativeMixedMode)
            {
                phase = QStringLiteral("native-mixed-working-output");
                solverModelFileName = nativeMixedWorkingOutputPath(this->nativeCheckpointDirectory,inputTargetPath);
                RLogger::info("Native mixed working result link: '%s'.\n",
                              qPrintable(solverModelFileName));
            }
        }

        quint64 acceptedBaselineSequence = 0;
        double acceptedBaselineTimeS = 0.0;
        quint64 targetAcceptedSequence = 0;
        const RStructuralDynamicsRestartData &initialRestartData =
            model.getProblemSetup().getStructuralDynamicsRestartData();
        if (nativeMixedMode)
        {
            const auto initialView = mixedCheckpointStateView(model);
            acceptedBaselineSequence = initialView.sequence;
            acceptedBaselineTimeS = initialView.timeS;
        }
        else if (initialRestartData.available)
        {
            initialRestartData.validate();
            acceptedBaselineSequence = initialRestartData.completedSteps;
            acceptedBaselineTimeS = initialRestartData.time;
        }
        if (this->nativeExecutionGate)
        {
            if (!nativeCheckpointContext || !acceptedCheckpointStore)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native execution pause requires the periodic native structural checkpoint context.");
            const RStructuralDynamicsSetup &dynamicsSetup =
                model.getProblemSetup().getStructuralDynamicsSetup();
            if (!dynamicsSetup.getEnabled() ||
                dynamicsSetup.getMethod() != RStructuralTimeIntegrationMethod::Newmark ||
                model.getProblemTaskTree().getProblemTypeMask() != R_PROBLEM_STRESS)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native execution pause supports only stress-only Newmark structural dynamics.");
            const quint64 requestedSteps = dynamicsSetup.getNSteps();
            if (requestedSteps == 0 ||
                acceptedBaselineSequence > std::numeric_limits<quint64>::max()-requestedSteps)
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Native execution target accepted sequence overflows its supported range.");
            targetAcceptedSequence = nativeMixedMode ? requestedSteps
                                                    : acceptedBaselineSequence + requestedSteps;
            QString gateError;
            if (!this->nativeExecutionGate->configureAcceptedRange(
                    acceptedBaselineSequence,targetAcceptedSequence,
                    acceptedBaselineTimeS,&gateError))
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "Invalid native execution pause range: %s",qPrintable(gateError));
        }

        if (controlSetup.enabled)
        {
            ControlTransportConfig config;
            config.program = this->controlProgram;
            config.arguments = this->controlArguments;
            config.controllerId = controlSetup.controllerId;
            config.controllerRevision = controlRevision;
            config.modelSnapshotSha256 = nativeCheckpointContext
                ? nativeCheckpointContext->modelSha256 : inputTargetHash;
            config.deterministicSeed = controlSetup.deterministicSeed;
            config.timeoutMs = int(this->controlTimeoutMs);
            const auto &interaction = model.getProblemSetup().getStructuralInteractionSetup();
            for (const auto &sensor : interaction.getSensors())
                config.sensorUnits.insert(sensor.id,sensor.unit.isEmpty()
                    ? RStructuralInteractionSetup::getSensorUnit(sensor.quantity) : sensor.unit);
            for (const auto &actuator : interaction.getActuators())
                config.actuatorIds.append(actuator.id);
            if (this->runtimeControlBridge)
            {
                namespace Runtime=RStructuralControlRuntimeParameters;
                if (!nativeCheckpointContext || !this->nativeExecutionGate ||
                    interaction.getSensors().empty() || interaction.getActuators().size()!=1 ||
                    model.getProblemSetup().getStructuralCableSetup().enabled ||
                    model.getProblemSetup().getStructuralPiezoSetup().enabled)
                    throw std::invalid_argument("Runtime control requires scalar linear Newmark source and checkpoint identity");
                bool hasPhysicalVelocity = false;
                for (const auto &sensor : interaction.getSensors())
                    if (sensor.quantity==RStructuralSensorQuantity::Velocity &&
                        config.sensorUnits.value(sensor.id)==QStringLiteral("m/s"))
                        hasPhysicalVelocity = true;
                if (!hasPhysicalVelocity)
                    throw std::invalid_argument("Runtime control source requires a physical scalar velocity sensor in m/s");
                std::vector<Runtime::ActuatorLimit> limits;
                for (const auto &actuator : interaction.getActuators())
                {
                    if (actuator.type!=RStructuralActuatorType::NodalForce || actuator.unit!=QStringLiteral("N") ||
                        !std::isfinite(actuator.minimum) || !std::isfinite(actuator.maximum) ||
                        std::isnan(actuator.rateLimit) || actuator.rateLimit<=0.0)
                        throw std::invalid_argument("Runtime control source requires NodalForce in N, finite amplitude and positive or unlimited model rate");
                    // Positive infinity is the model's unlimited rate. The
                    // runtime codec expresses that same policy as zero; a
                    // model rate of zero (held force) remains unsupported.
                    limits.push_back({actuator.id,actuator.minimum,actuator.maximum,
                                      std::isinf(actuator.rateLimit) ? 0.0 : actuator.rateLimit});
                }
                initialRuntimeState=Runtime::fresh(controlRevision,this->runtimeControlBridge->bootstrapGain(),limits);
                if (this->restart && initialRestartData.available && initialRestartData.controlAvailable)
                {
                    const auto envelope=RStructuralControlStateEnvelope::deserialize(initialRestartData.controlControllerState);
                    if (!envelope.runtimeParametersState.isEmpty())
                        initialRuntimeState=Runtime::deserialize(envelope.runtimeParametersState);
                }
                if (initialRuntimeState.parameters.fixedBaseControllerRevision!=controlRevision ||
                    (!initialRuntimeState.history.empty() &&
                     (initialRuntimeState.history.back().appliedSeq>acceptedBaselineSequence ||
                      initialRuntimeState.history.back().acceptedTimeS>acceptedBaselineTimeS)))
                    throw std::invalid_argument("Runtime checkpoint parameters do not match the immutable controller/accepted boundary");
                const QString jobId=this->runtimeControlBridge->jobId();
                const QString taskId=this->runtimeControlBridge->taskId();
                RuntimeParameterGate::Identity identity{jobId,this->runtimeControlBridge->runId(),taskId,
                    nativeCheckpointContext->modelSha256,controlRevision};
                runtimeIdentity=RuntimeControlBridge::identityJson(identity);
                config.runtimeIdentity=runtimeIdentity;
                QString bridgeError;
                if (!this->runtimeControlBridge->initialize(identity,initialRuntimeState,limits,&bridgeError))
                    throw std::invalid_argument(bridgeError.toStdString());
                runtimeParameterGate=this->runtimeControlBridge->parameterGate();
                config.maxLineBytes=65536;
            }
            controlTransport.reset(new ControlNdjsonTransport(std::move(config)));
            QString transportError;
            if (!controlTransport->start(&transportError))
                throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                             "ControlHandshakeFailed: %s",qPrintable(transportError));
        }

        QJsonObject cliOverride{{QStringLiteral("provided"),bool(this->requestedNThreads)}};
        if (this->requestedNThreads)
        {
            cliOverride.insert(QStringLiteral("requestedThreads"),double(*this->requestedNThreads));
            cliOverride.insert(QStringLiteral("clampedThreads"),double(*this->nThreads));
        }
        const QJsonObject startedPayload{
            {QStringLiteral("inputArgumentPath"),inputPath},
            {QStringLiteral("inputArgumentSha256"),inputArgumentHash},
            {QStringLiteral("inputModelTargetPath"),inputTargetPath},
            {QStringLiteral("inputModelTargetSha256"),inputTargetHash},
            {QStringLiteral("resultLinkPath"),QFileInfo(solverModelFileName).absoluteFilePath()},
            {QStringLiteral("restartRequested"),this->restart},
            {QStringLiteral("modelParallelConfig"),modelParallelConfig(
                model.getProblemSetup().getParallelExecutionConfig())},
            {QStringLiteral("cliOverride"),cliOverride}
        };

        phase = QStringLiteral("solver-run");
        RLogger::info("\n********* SOLVER - BEGIN *********\n\n");
        RLogger::indent();
        loggerIndented = true;
        RSolver solver(model,solverModelFileName,this->convergenceFileName);
        if (nativeMixedMode)
        {
            solver.setNativeMixedStepBudget(this->nativeMixedStepBudget.value_or(0));
            if (acceptedCheckpointStore)
                solver.setAcceptedStepParticipant(acceptedCheckpointStore);
        }
        quint64 lastCheckpointSequence = acceptedBaselineSequence;
        double lastCheckpointTimeS = acceptedBaselineTimeS;
        NativeAcceptedCheckpointStore::VerifiedCheckpoint latestCheckpoint;
        bool hasLatestCheckpoint = false;
        if (acceptedCheckpointStore)
        {
            if (nativeMixedMode)
            {
                solver.setAcceptedStructuralStepObserver([&](const RModel &acceptedModel)
                {
                    // Publication and participant promotion already succeeded.
                    // Receipts are in-memory; this observer performs no store I/O.
                    const auto &state = acceptedModel.getProblemSetup()
                        .getStructuralTrussDynamicsRestartData();
                    if (this->nativeExecutionGate)
                    {
                        NativeExecutionGate::CheckpointReceipt receipt;
                        const auto *committed = acceptedCheckpointStore->committedReceipt();
                        if (committed)
                        {
                            receipt.available = true;
                            receipt.acceptedSequence = quint64(committed->envelope
                                .value(QStringLiteral("acceptedSeq")).toDouble());
                            receipt.timeS = committed->envelope.value(QStringLiteral("timeS")).toDouble();
                            receipt.modelSha256 = committed->envelope
                                .value(QStringLiteral("checkpointModelSha256")).toString();
                            receipt.envelopeSha256 = QString::fromLatin1(QCryptographicHash::hash(
                                QJsonDocument(committed->envelope).toJson(QJsonDocument::Compact),
                                QCryptographicHash::Sha256).toHex());
                        }
                        QString gateError;
                        this->nativeExecutionGate->acceptedBoundary(
                            state.completedSteps,state.time,receipt,&gateError);
                        if (!gateError.isEmpty())
                            throw RError(RError::Type::Application,R_ERROR_REF,
                                         "Native execution state gate failed after commit: %s",
                                         qPrintable(gateError));
                    }
                });
            }
            else
            {
            const QString latestPointerPath = QDir(this->nativeCheckpointDirectory)
                .filePath(QStringLiteral("latest.json"));
            if (QFileInfo::exists(latestPointerPath))
            {
                latestCheckpoint = acceptedCheckpointStore->readLatest();
                hasLatestCheckpoint = true;
                lastCheckpointSequence = quint64(latestCheckpoint.envelope
                    .value(QStringLiteral("acceptedSeq")).toDouble());
                lastCheckpointTimeS = latestCheckpoint.envelope
                    .value(QStringLiteral("timeS")).toDouble();
            }
            solver.setAcceptedStructuralStepObserver(
                [&,lastCheckpointSequence,lastCheckpointTimeS](const RModel &acceptedModel) mutable
                {
                    const RStructuralDynamicsRestartData &restartData =
                        acceptedModel.getProblemSetup().getStructuralDynamicsRestartData();
                    const RTimeSolver &timeSolver = acceptedModel.getTimeSolver();
                    const quint64 acceptedSequence = restartData.completedSteps;
                    const double acceptedTimeS = restartData.time;
                    const double elapsedSinceCheckpoint = acceptedTimeS-lastCheckpointTimeS;
                    if (acceptedSequence < lastCheckpointSequence ||
                        !std::isfinite(elapsedSinceCheckpoint) || elapsedSinceCheckpoint < 0.0)
                        throw RError(RError::Type::Application,R_ERROR_REF,
                                     "Native checkpoint schedule moved backwards at an accepted step.");
                    const bool checkpointDue =
                        acceptedSequence-lastCheckpointSequence >=
                            nativeCheckpointContext->maxAcceptedSteps ||
                        elapsedSinceCheckpoint >= nativeCheckpointContext->maxSimulationSeconds ||
                        elapsedSinceCheckpoint+restartData.fixedTimeStep >
                            nativeCheckpointContext->maxSimulationSeconds ||
                        timeSolver.getCurrentTimeStep()+1 >= timeSolver.getNTimeSteps();
                    acceptedCheckpointStore->accept(acceptedModel);
                    if (checkpointDue)
                    {
                        latestCheckpoint = acceptedCheckpointStore->readLatest();
                        hasLatestCheckpoint = true;
                        lastCheckpointSequence = quint64(latestCheckpoint.envelope
                            .value(QStringLiteral("acceptedSeq")).toDouble());
                        lastCheckpointTimeS = latestCheckpoint.envelope
                            .value(QStringLiteral("timeS")).toDouble();
                    }
                    if (this->nativeExecutionGate)
                    {
                        NativeExecutionGate::CheckpointReceipt receipt;
                        if (hasLatestCheckpoint)
                        {
                            receipt.available = true;
                            receipt.acceptedSequence = quint64(latestCheckpoint.envelope
                                .value(QStringLiteral("acceptedSeq")).toDouble());
                            receipt.timeS = latestCheckpoint.envelope
                                .value(QStringLiteral("timeS")).toDouble();
                            receipt.modelSha256 = latestCheckpoint.envelope
                                .value(QStringLiteral("checkpointModelSha256")).toString();
                            receipt.envelopeSha256 = QString::fromLatin1(
                                QCryptographicHash::hash(
                                    QJsonDocument(latestCheckpoint.envelope)
                                        .toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256).toHex());
                        }
                        QString gateError;
                        this->nativeExecutionGate->acceptedBoundary(
                            acceptedSequence,acceptedTimeS,receipt,&gateError);
                        if (!gateError.isEmpty())
                            throw RError(RError::Type::Application,R_ERROR_REF,
                                         "Native execution state gate failed: %s",
                                         qPrintable(gateError));
                    }
                });
            }
        }
        const bool resultJournalRequested =
            !qEnvironmentVariable(NativeResultJournal::jobIdEnvironmentVariable).isEmpty() ||
            !qEnvironmentVariable(NativeResultJournal::pathEnvironmentVariable).isEmpty() ||
            !qEnvironmentVariable(NativeResultJournal::resultDirectoryEnvironmentVariable).isEmpty();
        QString resultJournalError;
        std::unique_ptr<NativeResultJournal> resultJournal =
            NativeResultJournal::createFromEnvironment(&resultJournalError);
        ResultFrameObserverScope resultFrameObserverScope(solver);
        bool resultJournalUnavailableReported = false;
        auto reportResultJournalUnavailable = [&]()
        {
            if (!resultJournalRequested || resultJournalUnavailableReported)
            {
                return;
            }
            QString reason = resultJournalError;
            if (reason.isEmpty() && resultJournal)
            {
                reason = resultJournal->unavailableReason();
            }
            if (reason.isEmpty() && resultJournal && resultJournal->isAvailable())
            {
                return;
            }
            if (reason.isEmpty())
            {
                reason = QStringLiteral("Native result-frame journal was not initialized");
            }
            RLogger::warning("NativeResultFrameUnavailable: %s\n",qPrintable(reason));
            resultJournalUnavailableReported = true;
        };
        if (resultJournalRequested && !resultJournal)
        {
            reportResultJournalUnavailable();
        }
        if (resultJournal && resultJournal->isAvailable())
        {
            solver.setResultFrameObserver([&](const RSolverResultFrame &frame)
            {
                if (!resultJournal->appendFrame(frame))
                {
                    reportResultJournalUnavailable();
                }
            });
        }
        if (controlTransport)
        {
            solver.setStructuralControlCallback(controlSetup.controllerId,controlRevision,
                [&](const RStructuralControlObservation &observation)
                {
                    QJsonObject runtimeParameters;
                    RStructuralControlDecision decision;
                    if (runtimeParameterGate)
                    {
                        namespace Runtime=RStructuralControlRuntimeParameters;
                        if (this->nativeExecutionGate->isCancelled())
                            throw std::invalid_argument("Runtime control run was cancelled before compute");
                        const auto actual=observation.priorRuntimeParametersState.isEmpty()
                            ? initialRuntimeState : Runtime::deserialize(observation.priorRuntimeParametersState);
                        if ((observation.priorRuntimeParametersState.isEmpty() && actual.parameters.revision!=0) ||
                            Runtime::serialize(actual)!=Runtime::serialize(runtimeParameterGate->acceptedState()))
                            throw std::invalid_argument("Runtime observation does not match the accepted gate state");
                        if (frozenRuntimeProposal)
                            throw std::invalid_argument("Runtime callback replay before the prior transaction committed or rolled back");
                        auto effective=actual.parameters;
                        if (const auto frozen=runtimeParameterGate->freezeCandidate(actual))
                        {
                            frozenRuntimeProposal=frozen->proposalId;
                            effective=frozen->parameters;
                            decision.runtimeParameterCandidate=effective;
                            decision.runtimeParameterProposalId=frozen->proposalId;
                        }
                        if (observation.priorRuntimeParametersState.isEmpty())
                            decision.initialRuntimeParametersState=Runtime::serialize(initialRuntimeState);
                        runtimeParameters={{QStringLiteral("schema"),QStringLiteral("range.runtime-parameters/v1")},
                            {QStringLiteral("identity"),runtimeIdentity},
                            {QStringLiteral("parameters"),RuntimeControlBridge::parametersJson(effective)}};
                    }
                    QJsonObject acceptedState;
                    if (!observation.priorControllerState.isEmpty())
                    {
                        QJsonParseError parseError;
                        const QJsonDocument document = QJsonDocument::fromJson(
                            observation.priorControllerState,&parseError);
                        if (parseError.error != QJsonParseError::NoError || !document.isObject())
                            throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                                         "Checkpoint controller state is not a JSON object.");
                        acceptedState = document.object();
                    }
                    QVector<ControlObservation> values;
                    values.reserve(int(observation.sensors.size()));
                    for (const auto &sample : observation.sensors)
                        values.append({sample.sensorId,sample.value,sample.unit});
                    ControlStepReply reply;
                    QString error;
                    if (!controlTransport->compute(nextControlStepId,observation.acceptedTime,
                                                   observation.nextTime-observation.acceptedTime,
                                                   values,acceptedState,reply,&error,runtimeParameters))
                        throw RError(RError::Type::InvalidInput,R_ERROR_REF,
                                     "ControlComputeFailed: %s",qPrintable(error));
                    if (runtimeParameterGate && this->nativeExecutionGate->isCancelled())
                        throw std::invalid_argument("Runtime control run was cancelled during compute");
                    QMap<QString,double> commandsById;
                    for (const auto &command : reply.commands)
                        commandsById.insert(command.actuatorId,command.force);
                    for (const auto &actuator : model.getProblemSetup()
                                                     .getStructuralInteractionSetup().getActuators())
                        decision.requestedCommands.push_back(
                            {actuator.id,commandsById.value(actuator.id)});
                    decision.nextControllerState = QJsonDocument(reply.nextControllerState)
                                                       .toJson(QJsonDocument::Compact);
                    return decision;
                });
            solver.setStructuralControlStepObserver(
                [&](const RStructuralControlStepRecord &record)
                {
                    if (runtimeParameterGate)
                    {
                        namespace Runtime=RStructuralControlRuntimeParameters;
                        // Entry into this observer proves the solver transaction
                        // is public. Even decoding failure must not reject it as
                        // though a trial had rolled back.
                        const bool appliedRuntimeProposal=bool(frozenRuntimeProposal);
                        frozenRuntimeProposal.reset();
                        const auto committed=Runtime::deserialize(record.runtimeParametersState);
                        if (appliedRuntimeProposal)
                        {
                            // This observer runs after the public step commit. Never
                            // turn a notification failure into rollback of that step.
                            if (committed.history.empty() || committed.history.back().appliedSeq!=record.appliedSeq ||
                                committed.history.back().acceptedTimeS!=record.nextTime)
                                throw std::invalid_argument("Runtime receipt does not match the actual committed record");
                            const auto result=runtimeParameterGate->onCommitted(committed);
                            if (result.error!=RuntimeParameterGate::Error::None || !result.receipt)
                                throw std::invalid_argument("Runtime gate rejected the actual committed transaction state");
                            QString publishError;
                            if (!this->runtimeControlBridge->publish(*result.receipt,&publishError))
                                throw std::invalid_argument(publishError.toStdString());
                        }
                        else if (Runtime::serialize(committed)!=Runtime::serialize(runtimeParameterGate->acceptedState()))
                            throw std::invalid_argument("Committed runtime state changed without a frozen proposal");
                    }
                    QJsonArray requested;
                    for (const auto &command : record.requestedCommands)
                        requested.append(QJsonObject{{QStringLiteral("actuatorId"),command.actuatorId},
                                                     {QStringLiteral("requestedForceN"),command.value}});
                    QJsonArray conditioned;
                    for (const auto &command : record.conditionedCommands)
                        conditioned.append(QJsonObject{{QStringLiteral("actuatorId"),command.actuatorId},
                                                       {QStringLiteral("conditionedForceN"),command.value}});
                    QJsonArray sensors;
                    for (const auto &sample : record.sensors)
                        sensors.append(QJsonObject{{QStringLiteral("sensorId"),sample.sensorId},
                                                   {QStringLiteral("value"),sample.value},
                                                   {QStringLiteral("unit"),sample.unit}});
                    const RTimeSolver &clock = model.getTimeSolver();
                    const uint stepNumber = clock.getCurrentTimeStep()+1;
                    const uint outputFrequency = clock.getOutputFrequency();
                    const bool persisted = stepNumber == clock.getNTimeSteps() ||
                        (outputFrequency > 0 && stepNumber % outputFrequency == 0);
                    QJsonObject payload{
                        {QStringLiteral("stepId"),QString::number(nextControlStepId)},
                        {QStringLiteral("controllerId"),controlSetup.controllerId},
                        {QStringLiteral("controllerRevision"),controlRevision},
                        {QStringLiteral("deterministicSeed"),
                         QString::number(controlSetup.deterministicSeed)},
                        {QStringLiteral("acceptedTimeS"),record.acceptedTime},
                        {QStringLiteral("nextTimeS"),record.nextTime},
                        {QStringLiteral("sensors"),sensors},
                        {QStringLiteral("requestedCommands"),requested},
                        {QStringLiteral("conditionedCommands"),conditioned},
                        {QStringLiteral("appliedChannels"),controlSamplesJson(record)},
                        {QStringLiteral("controllerStateSha256"),
                         QString::fromLatin1(record.controllerStateSha256)},
                        {QStringLiteral("resultArtifactPersisted"),persisted}};
                    if (persisted)
                    {
                        payload.insert(QStringLiteral("resultLinkPath"),
                                       QFileInfo(solverModelFileName).absoluteFilePath());
                        payload.insert(QStringLiteral("resultModelTargetPath"),
                                       resolveModelTargetPath(solverModelFileName));
                    }
                    appendAudit(QStringLiteral("ControlStepAccepted"),
                                QStringLiteral("accepted"),payload);
                    ++nextControlStepId;
                });
        }
        solver.setParallelExecutionReportObserver(
            [&](const RParallelExecutionReport &report)
            {
                QJsonObject payload(startedPayload);
                payload.insert(QStringLiteral("parallelReport"),parallelReportPayload(report));
                appendAudit(QStringLiteral("SolverRunStarted"),QStringLiteral("started"),payload);
            });
        solverEntered = true;
        solver.run();
        if (this->runtimeControlBridge) this->runtimeControlBridge->stop();
        if (this->nativeExecutionGate)
        {
            QString completionGateError;
            if (!this->nativeExecutionGate->markCompleted(&completionGateError))
                throw RError(RError::Type::Application,R_ERROR_REF,
                             "Native execution completion state failed: %s",
                             qPrintable(completionGateError));
        }
        reportResultJournalUnavailable();
        reportProgressUnavailable();
        RLogger::unindent();
        loggerIndented = false;
        RLogger::info("\n********** SOLVER - END **********\n\n");

        phase = QStringLiteral("run-computed-audit");
        appendAudit(QStringLiteral("RunComputed"),QStringLiteral("computed"),
                    {{QStringLiteral("computedTime"),model.getTimeSolver().getComputedTime()},
                     {QStringLiteral("resultLinkPath"),QFileInfo(solverModelFileName).absoluteFilePath()}});

        phase = QStringLiteral("final-model-write");
        solverModelFileName = model.write(solverModelFileName);
        finalModelWritten = true;
        RLogger::info("Model has been successfully saved to file '%s'.\n",
                      qPrintable(solverModelFileName));

        phase = QStringLiteral("result-link-hash");
        const QString resultLinkPath = QFileInfo(solverModelFileName).absoluteFilePath();
        const QString resultLinkHash = sha256File(resultLinkPath);
        const QString resultTargetPath = resolveModelTargetPath(resultLinkPath);
        const QString resultTargetHash = sha256File(resultTargetPath);
        phase = QStringLiteral("run-finished-audit");
        appendAudit(QStringLiteral("RunFinished"),QStringLiteral("finished"),
                    {{QStringLiteral("resultLinkPath"),resultLinkPath},
                     {QStringLiteral("resultLinkSha256"),resultLinkHash},
                     {QStringLiteral("resultModelTargetPath"),resultTargetPath},
                     {QStringLiteral("resultModelTargetSha256"),resultTargetHash}});
    }
    catch (const RError &error)
    {
        finishWithFailure(error.getMessage());
        return;
    }
    catch (const std::exception &error)
    {
        finishWithFailure(QString::fromUtf8(error.what()));
        return;
    }
    catch (...)
    {
        finishWithFailure(QStringLiteral("Unknown exception"));
        return;
    }
    journal.reset();
    emit this->finished();
}
