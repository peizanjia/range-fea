#include <omp.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <exception>
#include <memory>

#include <rbl_error.h>
#include <rbl_logger.h>

#include <rml_file_utils.h>
#include <rml_file_io.h>

#include <rparallel_execution_context.h>
#include <rsolver.h>

#include "execution_event_journal.h"
#include "solver_task.h"

namespace
{
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
}

SolverTask::SolverTask(const SolverInput &solverInput, QCoreApplication *app, QObject *parent)
    : QObject(parent)
    , modelFileName(solverInput.modelFileName)
    , convergenceFileName(solverInput.convergenceFileName)
    , monitoringFileName(solverInput.monitoringFileName)
    , nThreads(solverInput.nThreads)
    , requestedNThreads(solverInput.nThreads)
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
    RModel model;
    QUuid runId;
    QString journalPath;
    std::unique_ptr<ExecutionEventJournal> journal;
    QString phase = QStringLiteral("audit-reservation");
    bool auditWriteFailed = false;
    bool loggerIndented = false;
    bool solverEntered = false;
    bool finalModelWritten = false;
    const QString inputPath = QFileInfo(this->modelFileName).absoluteFilePath();

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
        if (loggerIndented)
        {
            RLogger::unindent();
            loggerIndented = false;
        }
        if (journal && !auditWriteFailed)
        {
            QString error;
            const QJsonObject payload{
                {QStringLiteral("phase"),phase},
                {QStringLiteral("message"),message},
                {QStringLiteral("resultLinkPath"),QFileInfo(this->modelFileName).absoluteFilePath()},
                {QStringLiteral("resultsMayAlreadyExist"),solverEntered || finalModelWritten}
            };
            if (!journal->append(QStringLiteral("RunFailed"),QStringLiteral("failed"),
                                 payload,QUuid::createUuid(),&error))
            {
                auditWriteFailed = true;
                RLogger::error("AuditWriteFailed: could not record RunFailed in '%s': %s\n",
                               qPrintable(journalPath),qPrintable(error));
            }
        }
        RLogger::error("Failed during %s for model '%s': %s\n",
                       qPrintable(phase),qPrintable(inputPath),qPrintable(message));
        if (auditWriteFailed || solverEntered || finalModelWritten)
        {
            RLogger::warning("Previous model result files may already exist; no rollback is claimed. Audit journal: '%s'\n",
                             qPrintable(journalPath));
        }
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
        const QString inputArgumentHash = sha256File(inputPath);
        const QString inputTargetPath = resolveModelTargetPath(inputPath);
        const QString inputTargetHash = sha256File(inputTargetPath);
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
        model.read(this->modelFileName);
        RLogger::info("Model has been successfully loaded from file '%s'.\n",
                      qPrintable(this->modelFileName));
        model.getMatrixSolverConf(RMatrixSolverConf::CG).setOutputFileName(
            RFileUtils::getFileNameWithSuffix(this->convergenceFileName,RMatrixSolverConf::getId(RMatrixSolverConf::CG)));
        model.getMatrixSolverConf(RMatrixSolverConf::GMRES).setOutputFileName(
            RFileUtils::getFileNameWithSuffix(this->convergenceFileName,RMatrixSolverConf::getId(RMatrixSolverConf::GMRES)));
        model.getMonitoringPointManager().setOutputFileName(this->monitoringFileName);
        if (this->restart)
        {
            model.getProblemSetup().setRestart(true);
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
            {QStringLiteral("restartRequested"),this->restart},
            {QStringLiteral("modelParallelConfig"),modelParallelConfig(
                model.getProblemSetup().getParallelExecutionConfig())},
            {QStringLiteral("cliOverride"),cliOverride}
        };

        phase = QStringLiteral("solver-run");
        RLogger::info("\n********* SOLVER - BEGIN *********\n\n");
        RLogger::indent();
        loggerIndented = true;
        RSolver solver(model,this->modelFileName,this->convergenceFileName);
        solver.setParallelExecutionReportObserver(
            [&](const RParallelExecutionReport &report)
            {
                QJsonObject payload(startedPayload);
                payload.insert(QStringLiteral("parallelReport"),parallelReportPayload(report));
                appendAudit(QStringLiteral("SolverRunStarted"),QStringLiteral("started"),payload);
            });
        solverEntered = true;
        solver.run();
        RLogger::unindent();
        loggerIndented = false;
        RLogger::info("\n********** SOLVER - END **********\n\n");

        phase = QStringLiteral("run-computed-audit");
        appendAudit(QStringLiteral("RunComputed"),QStringLiteral("computed"),
                    {{QStringLiteral("computedTime"),model.getTimeSolver().getComputedTime()},
                     {QStringLiteral("resultLinkPath"),QFileInfo(this->modelFileName).absoluteFilePath()}});

        phase = QStringLiteral("final-model-write");
        this->modelFileName = model.write(this->modelFileName);
        finalModelWritten = true;
        RLogger::info("Model has been successfully saved to file '%s'.\n",
                      qPrintable(this->modelFileName));

        phase = QStringLiteral("result-link-hash");
        const QString resultLinkPath = QFileInfo(this->modelFileName).absoluteFilePath();
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
    emit this->finished();
}
