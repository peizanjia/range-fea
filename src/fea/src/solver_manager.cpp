#include "application.h"
#include "solver_manager.h"
#include "model_command_host.h"
#include "session.h"
#include "solver_setup_checker.h"
#include <rnative_result_broker.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QMetaObject>
#include <QQueue>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>

#include <cmath>
#include <algorithm>
#include <exception>
#include <memory>
#include <utility>

namespace
{
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

bool samePathText(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QDir::cleanPath(left).compare(QDir::cleanPath(right), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(left) == QDir::cleanPath(right);
#endif
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
}

SolverManager::SolverManager(QObject *parent)
    : RJobManager(parent)
    , taskServer(0)
{
    this->taskServerName = "RANGE_SOLVER_TASK_SERVER_" + Application::instance()->getSession()->getID();
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
    this->registerNativeTask(solverTask, jobId, projectSnapshotRef, projectLink,
                             frozenSnapshotVerified, frozenSnapshotPath, frozenSnapshotSha256);
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

void SolverManager::registerNativeTask(SolverTask *solverTask, const QString &jobId,
                                       const QJsonObject &projectSnapshotRef, const QString &projectLink,
                                       bool frozenSnapshotVerified, const QString &frozenSnapshotPath,
                                       const QString &frozenSnapshotSha256)
{
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
    const QJsonObject sourceSnapshotRef = projectSnapshotRef.isEmpty()
            ? QJsonObject{{QStringLiteral("kind"), QStringLiteral("gui-native-input-snapshot/v1")},
                          {QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp},
                          {QStringLiteral("inputDerivedFrom"), solverTask->getInputDerivedFrom()},
                          {QStringLiteral("inputSnapshotSha256"), QString::fromLatin1(solverTask->getInputSnapshotSha256().toHex())},
                          {QStringLiteral("inputPackageSha256"), QString::fromLatin1(solverTask->getInputPackageSha256().toHex())}}
            : projectSnapshotRef;
    const bool provenanceFrozen = solverTask->freezeNativeResultProvenance(sourceRuntimeStamp, sourceSnapshotRef);
    const bool nativeResultCatalogExpected = solverTask->expectsNativeResultCatalog();
    const bool linkedSnapshot = projectLink == QStringLiteral("linked") && frozenSnapshotVerified;
    QJsonObject record{{QStringLiteral("jobId"), jobId},
                       {QStringLiteral("jobManagerId"), int(solverTask->getID())},
                       {QStringLiteral("modelIdAtSubmit"), int(solverTask->getModelID())},
                       {QStringLiteral("state"), QStringLiteral("queued")},
                       {QStringLiteral("createdAt"), now},
                       {QStringLiteral("updatedAt"), now},
                       {QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp},
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
    this->nativeJobs.insert(jobId, record);
    this->nativeJobOrder.append(jobId);
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
                     [this, jobId](const QString &reportedJobId, const QString &reportedState,
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
        this->updateNativeJob(jobId, fields);
        if (state == QStringLiteral("succeeded") && verifiedArtifact)
            this->lastResultArtifactFileName = actualResultPath;
        this->nativeTasks.remove(jobId);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &RJob::failed, this, [this, jobId]()
    {
        const QString state = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("queued") && state != QStringLiteral("running") && state != QStringLiteral("cancelling")) return;
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        this->updateNativeJob(jobId, {{QStringLiteral("state"), state == QStringLiteral("cancelling")
                                                              ? QStringLiteral("cancelled") : QStringLiteral("failed")},
                                      {QStringLiteral("finishedAt"), now},
                                      {QStringLiteral("updatedAt"), now},
                                      {QStringLiteral("message"), QStringLiteral("Native solver ended without complete terminal evidence.")}});
        this->nativeTasks.remove(jobId);
    }, Qt::QueuedConnection);
    QObject::connect(solverTask, &RJob::finished, this, [this, jobId]()
    {
        const QString state = this->nativeJobs.value(jobId).value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("queued") && state != QStringLiteral("running") && state != QStringLiteral("cancelling")) return;
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        this->updateNativeJob(jobId, {{QStringLiteral("state"), QStringLiteral("failed")},
                                      {QStringLiteral("finishedAt"), now},
                                      {QStringLiteral("updatedAt"), now},
                                      {QStringLiteral("message"), QStringLiteral("Native solver returned without complete terminal evidence.")}});
        this->nativeTasks.remove(jobId);
    }, Qt::QueuedConnection);
}

void SolverManager::updateNativeJob(const QString &jobId, const QJsonObject &fields)
{
    auto it = this->nativeJobs.find(jobId);
    if (it == this->nativeJobs.end()) return;
    QJsonObject record = it.value();
    for (auto field = fields.begin(); field != fields.end(); ++field)
        record.insert(field.key(), field.value());
    it.value() = record;
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
        if (!this->nativeJobs.contains(jobId))
            result = {{QStringLiteral("found"), false}, {QStringLiteral("jobId"), jobId}};
        else
        {
            result = this->nativeJobs.value(jobId);
            result.insert(QStringLiteral("found"), true);
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
        const int total = this->nativeJobOrder.size();
        const int end = offset >= total ? total : qMin(total, offset + limit);
        QJsonArray jobs;
        for (int index = offset; index < end; ++index)
        {
            QJsonObject record = this->nativeJobs.value(this->nativeJobOrder.at(index));
            record.insert(QStringLiteral("found"), true);
            jobs.append(record);
        }
        result = {{QStringLiteral("jobs"), jobs},
                  {QStringLiteral("offset"), offset},
                  {QStringLiteral("limit"), limit},
                  {QStringLiteral("total"), total},
                  {QStringLiteral("hasMore"), end < total}};
    }))
        return {{QStringLiteral("error"), QStringLiteral("wrong_thread")}};
    return result;
}

QJsonObject SolverManager::nativeResultManifestForSource(const QString &sourcePath) const
{
    QJsonObject result;
    this->invokeOnOwner([this, &result, &sourcePath]()
    {
        for (const QString &jobId : this->nativeJobOrder)
        {
            const QJsonObject record = this->nativeJobs.value(jobId);
            if (samePathText(record.value(QStringLiteral("resultArtifactRef")).toString(), sourcePath) ||
                samePathText(record.value(QStringLiteral("resultSourceBinding")).toObject()
                             .value(QStringLiteral("resolvedSourcePath")).toString(), sourcePath))
            {
                const QJsonObject descriptor = this->nativeResultDescriptors.value(jobId);
                if (!descriptor.isEmpty())
                    result = {{QStringLiteral("resultManifestRef"), descriptor.value(QStringLiteral("resultManifestRef"))},
                              {QStringLiteral("resultManifestSha256"), descriptor.value(QStringLiteral("resultManifestSha256"))}};
            }
        }
    });
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
        if (!this->nativeJobs.contains(jobId))
        {
            result = failure(QStringLiteral("UNKNOWN_NATIVE_JOB"), QStringLiteral("Unknown native solver job ID."));
            return;
        }
        const QJsonObject record = this->nativeJobs.value(jobId);
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
        this->updateNativeJob(jobId, {{QStringLiteral("state"), QStringLiteral("cancelled")},
                                      {QStringLiteral("cancellationRequested"), true},
                                      {QStringLiteral("cancelRequestedAt"), now},
                                      {QStringLiteral("finishedAt"), now},
                                      {QStringLiteral("updatedAt"), now},
                                      {QStringLiteral("processStarted"), false},
                                      {QStringLiteral("processExited"), false},
                                      {QStringLiteral("resultReadbackComplete"), false},
                                      {QStringLiteral("message"), QStringLiteral("Cancelled before the native solver process started.")}});
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
