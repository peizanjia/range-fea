#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QUuid>

#include <algorithm>

#include <rml_file_utils.h>
#include <rml_view_factor_matrix.h>

#include "model_io.h"
#include "solver_manager.h"
#include "solver_task.h"
#include "application.h"

namespace
{
QString makeRunArtifactFileName(const Model &model,
                                const ApplicationSettings *applicationSettings,
                                const SolverTaskID &taskID)
{
    const QString sourceFileName = RFileUtils::getFileNameWithOutTimeStep(model.getFileName());
    const QFileInfo sourceInfo(sourceFileName);

    QString sourceDirectory = sourceInfo.absolutePath();
    if (sourceFileName.isEmpty())
    {
        sourceDirectory = applicationSettings->getDataDir();
        if (sourceDirectory.isEmpty()) sourceDirectory = QDir::tempPath();
    }

    QString baseName = sourceInfo.completeBaseName();
    if (baseName.isEmpty()) baseName = QFileInfo(model.getName()).fileName();
    if (baseName.isEmpty()) baseName = QStringLiteral("model");

    const QString sourceExtension = sourceInfo.suffix();
    QString extension;
    if (sourceExtension.compare(RModel::getDefaultFileExtension(false), Qt::CaseInsensitive) == 0)
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

QString absoluteModelPath(const QString &fileName)
{
    if (fileName.isEmpty()) return {};
    return QFileInfo(RFileUtils::getFileNameWithOutTimeStep(fileName)).absoluteFilePath();
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
                                           QStringLiteral("-(\\d{5})\\.") +
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

    try
    {
        for (auto it = sourceRecords.cbegin(); it != sourceRecords.cend(); ++it)
        {
            RModel record;
            record.read(it.value());
            setRecordNumber(record, it.key());
            record.write(RFileUtils::getFileNameWithTimeStep(inputHistoryBase, it.key()), true);
        }
    }
    catch (const RError &readError)
    {
        if (error) *error = QStringLiteral("Could not snapshot restart record: %1").arg(readError.getMessage());
        return false;
    }
    return true;
}

bool copyRestartHistoryToResult(const QString &inputFileName,
                                const QString &resultFileName,
                                QString *error)
{
    const QFileInfo inputInfo(inputFileName);
    const QString inputBaseName = inputInfo.completeBaseName();
    const QString inputExtension = inputInfo.suffix();
    const QString inputHistoryBase = QDir(inputInfo.absolutePath())
            .filePath(QStringLiteral("history/") + inputInfo.fileName());
    const QFileInfo historyBaseInfo(inputHistoryBase);
    const QRegularExpression recordPattern(QStringLiteral("^") + QRegularExpression::escape(inputBaseName) +
                                           QStringLiteral("-(\\d{5})\\.") +
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

    try
    {
        for (auto it = inputRecords.cbegin(); it != inputRecords.cend(); ++it)
        {
            RModel record;
            record.read(it.value());
            setRecordNumber(record, it.key());
            record.write(RFileUtils::getFileNameWithTimeStep(resultFileName, it.key()), true);
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
    this->solverExecutable = applicationSettings->getSolverPath();

    Session *session = Application::instance()->getSession();
    Model &rModel = session->getModel(this->modelID);
    this->sourceDocumentId = session->modelCommandStamp(this->modelID).first;

    this->modelFileName = makeRunArtifactFileName(rModel, applicationSettings, this->taskID);
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

    this->solverArguments.append("--file=" + this->modelFileName);
    this->solverArguments.append("--log-file=" + this->logFileName);
    this->solverArguments.append("--convergence-file=" + this->convergenceFileName);
    this->solverArguments.append("--monitoring-file=" + this->monitoringFileName);
    this->solverArguments.append("--nthreads=" + QString::number(this->applicationSettings->getNThreads()));
    this->solverArguments.append("--task-id=" + this->taskID.toString());
    this->solverArguments.append("--task-server=" + SolverManager::getInstance().getTaskServerName());
}

bool SolverTask::captureInputSnapshot(const Model &model,
                                      const QPair<QString, quint64> &sourceStamp,
                                      uint currentModelID)
{
    return this->captureInputSnapshot(std::make_unique<Model>(model), sourceStamp, currentModelID);
}

bool SolverTask::captureInputSnapshot(std::unique_ptr<Model> modelSnapshot,
                                      const QPair<QString, quint64> &sourceStamp,
                                      uint currentModelID)
{
    if (QThread::currentThread() != this->thread() ||
        sourceStamp.first.isEmpty() || sourceStamp.first != this->sourceDocumentId ||
        !this->sourceStamp.first.isEmpty() || !modelSnapshot)
    {
        return false;
    }

    this->sourceModelFileName = absoluteModelPath(modelSnapshot->getFileName());
    this->inputSnapshot = std::move(modelSnapshot);
    this->resultSnapshot = std::make_unique<Model>(*this->inputSnapshot);

    const QString runRoot = getRunRoot(this->resultArtifactFileName);
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
    if (!stageViewFactorReferences(*this->inputSnapshot,
                                   *this->resultSnapshot,
                                   this->sourceModelFileName,
                                   this->inputSnapshotFileName,
                                   this->resultArtifactFileName,
                                   &dependencyError) ||
        !stageRestartHistory(*this->inputSnapshot,
                             this->sourceModelFileName,
                             this->inputSnapshotFileName,
                             &dependencyError))
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
    return true;
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

const QString &SolverTask::getResultArtifactFileName() const
{
    return this->resultArtifactFileName;
}

uint SolverTask::getModelID() const
{
    return this->modelID;
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
    bool loggerIndented = false;
    try
    {
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
            if (!QDir().mkpath(resultDirectory) || !QDir().mkpath(logDirectory))
            {
                throw RError(RError::Type::InvalidFileName, R_ERROR_REF,
                             "Failed to create solver run artifact directories under '%s'.",
                             resultDirectory.toUtf8().constData());
            }
            ModelIO::save(*this->resultSnapshot, this->resultArtifactFileName, false);
            QString historyError;
            if (!copyRestartHistoryToResult(this->inputSnapshotFileName,
                                            this->resultArtifactFileName,
                                            &historyError))
            {
                throw RError(RError::Type::Application, R_ERROR_REF,
                             "Could not stage restart history: %s", historyError.toUtf8().constData());
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

        // Execute solver
        QString cmdLine = this->getCommandLine();

        RLogger::info("Executing \'%s\'\n",cmdLine.toUtf8().constData());

        this->solverProcess = new QProcess;

        QObject::connect(this->solverProcess,&QProcess::readyReadStandardOutput,this,&SolverTask::onProcessReadyReadStandardOutput,Qt::DirectConnection);
        QObject::connect(this->solverProcess,&QProcess::readyReadStandardError,this,&SolverTask::onProcessReadyReadStandardError,Qt::DirectConnection);

        this->solverProcess->start(this->solverExecutable,this->solverArguments);

        if (!this->solverProcess->waitForStarted())
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver process could not be started.");
        }
        while (!this->solverProcess->waitForFinished(100))
        {
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
                throw RError(RError::Type::Application, R_ERROR_REF, "Solver execution was cancelled.");
            }
            if (this->solverProcess->state() == QProcess::NotRunning) break;
        }
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver execution was cancelled.");
        }
        if (this->solverProcess->exitCode() != 0)
        {
            RLogger::warning("Command \'%s\' failed with exit code = %d.\n",cmdLine.toUtf8().constData(),this->solverProcess->exitCode());

            throw RError(RError::Type::Application,R_ERROR_REF,"Solver execution failed.");
        }

        delete this->solverProcess;
        this->solverProcess = nullptr;

        RLogger::info("Command \'%s\' succesfully finished.\n",cmdLine.toUtf8().constData());

        // Read the saved result back into the detached task model first. The
        // complete artifact remains at this path even if the source changed.
        try
        {
            ModelIO::update(*this->resultSnapshot, this->resultArtifactFileName);
        }
        catch (const RError &error)
        {
            RLogger::error("%s File: '%s'\n",error.getMessage().toUtf8().constData(),this->resultArtifactFileName.toUtf8().constData());
            throw RError(RError::Type::Application,R_ERROR_REF,"Failed to finish the solver because its result artifact could not be opened.");
        }
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF, "Solver result publication was cancelled.");
        }
        emit this->resultArtifactCreated(this->resultArtifactFileName);
        if (this->killRequested.load(std::memory_order_acquire))
        {
            throw RError(RError::Type::Application, R_ERROR_REF,
                         "Solver result was retained as an artifact after cancellation and was not published.");
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
        if (!accepted)
        {
            RLogger::warning("Solver result (#%s) completed for a stale or closed source; retained at '%s'.\n",
                             this->taskID.toString().toUtf8().constData(),
                             this->resultArtifactFileName.toUtf8().constData());
        }

        RLogger::unindent();
        loggerIndented = false;
        RLogger::info("Solver task (#%s) - End\n",this->taskID.toString().toUtf8().constData());
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
                }
            }
            delete this->solverProcess;
            this->solverProcess = nullptr;
        }
        if (loggerIndented) RLogger::unindent();
        RLogger::error("Solver task (#%s) failed: %s\n",this->taskID.toString().toUtf8().constData(),error.getMessage().toUtf8().constData());
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
    emit this->readyReadStandardError(this->solverProcess->readAllStandardError());
}
