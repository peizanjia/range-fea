#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QMetaObject>
#include <QTimer>
#include <QUuid>
#include <QMutexLocker>
#include <QThread>
#include <QScopeGuard>
#include <limits>

#include <rbl_job_manager.h>

#include <rml_file_io.h>
#include <rproject_catalog.h>

#include "session.h"
#include "application.h"
#include "model_io.h"
#include "model_command_host.h"

namespace
{
QString normalizedPath(const QString &path)
{
    if (path.isEmpty()) return {};
    QFileInfo info(path);
    QString normalized = info.canonicalFilePath();
    if (normalized.isEmpty()) normalized = info.absoluteFilePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(normalized));
}

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return left.toCaseFolded() == right.toCaseFolded();
#else
    return left == right;
#endif
}

QString nextSourceRevision(const QString &revision)
{
    if (!revision.startsWith(QLatin1Char('r'))) return {};
    bool ok = false;
    const qulonglong current = revision.mid(1).toULongLong(&ok);
    if (!ok || current >= 9007199254740991ULL) return {};
    return QStringLiteral("r%1").arg(current + 1);
}

QJsonArray sourceArtifactsJson(const QVector<RProjectCatalog::SourceArtifact> &artifacts)
{
    QJsonArray result;
    for (const auto &artifact : artifacts) result.append(artifact.toJson());
    return result;
}
}

Session::Session(QObject *parent)
    : QObject{parent}
{
    R_LOG_TRACE_IN;
    this->sessionID = QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmsszzz");
    this->drawEngine = new DrawEngine(this);
    this->commandHost = new ModelCommandHost(this);

    QString projectWorkspace = qEnvironmentVariable("RANGE_PROJECT_WORKSPACE");
    if (projectWorkspace.isEmpty())
    {
        QDir dataDir(Application::instance()->getApplicationSettings()->getDataDir());
        projectWorkspace = dataDir.filePath(QStringLiteral("projects/default"));
    }
    QString projectError;
    if (!this->openProject(projectWorkspace, &projectError))
    {
        this->projectOpenError = projectError;
        RLogger::error("Could not open the default project workspace: %s\n",
                       projectError.toUtf8().constData());
    }

    // Increment synchronously even when a legacy worker emits the notification.
    // A queued GUI slot must never leave a window in which an old request is valid.
    const auto invalidate = [this](uint id) { this->invalidateModelCommands(id); };
    connect(this, &Session::modelChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::problemChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::problemSelectionChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::resultsChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::modelRenamed, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::boundaryConditionChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::initialConditionChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::environmentConditionChanged, this, invalidate, Qt::DirectConnection);
    connect(this, &Session::materialChanged, this, invalidate, Qt::DirectConnection);
    connect(this, qOverload<uint, RVariableType>(&Session::variableDataChanged), this, invalidate, Qt::DirectConnection);
    connect(this, qOverload<const SessionEntityID &, RVariableType>(&Session::variableDataChanged), this,
            [this](const SessionEntityID &id, RVariableType) { this->invalidateModelCommands(id.getMid()); }, Qt::DirectConnection);

    QObject::connect(&this->pickList,&PickList::pickListChanged,this,&Session::onPickListChanged);
    QObject::connect(this,&Session::modelAdded,this,&Session::onModelAdded);
    QObject::connect(this,&Session::modelChanged,this,&Session::onModelChanged);
    QObject::connect(Application::instance()->getApplicationSettings(),
                     &ApplicationSettings::nHistoryRecordsChanged,
                     this,
                     &Session::onNHistoryRecordsChanged);
    R_LOG_TRACE_OUT;
}

Session::~Session() = default;

ModelCommandHost *Session::getModelCommandHost() const { return this->commandHost; }

QPair<QString, quint64> Session::modelCommandStamp(uint modelID) const
{
    QMutexLocker guard(&this->commandStampMutex);
    return modelID < uint(this->commandStamps.size()) ? this->commandStamps.at(int(modelID)) : QPair<QString, quint64>{};
}

uint Session::findModelByCommandIdentity(const QString &identity) const
{
    QMutexLocker guard(&this->commandStampMutex);
    for (qsizetype i = 0; i < this->commandStamps.size(); ++i)
        if (this->commandStamps.at(i).first == identity) return uint(i);
    return RConstants::eod;
}

bool Session::advanceModelCommandStamp(uint modelID, const QPair<QString, quint64> *expected,
                                       QPair<QString, quint64> *changed)
{
    QMutexLocker guard(&this->commandStampMutex);
    if (modelID >= uint(this->commandStamps.size())) return false;
    auto &stamp = this->commandStamps[int(modelID)];
    if (expected && stamp != *expected) return false;
    ++stamp.second;
    if (changed) *changed = stamp;
    return true;
}

void Session::queueProjectRevisionUpdate(const QPair<QString, quint64> &changed)
{
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates[changed.first] = qMax(this->pendingProjectUpdates.value(changed.first),
                                                          changed.second);
    }
    QMetaObject::invokeMethod(this, [this] { this->flushProjectCatalogUpdates(); }, Qt::QueuedConnection);
    // Never call observers while holding the stamp lock: GUI consumers may
    // query the new revision while handling the notification.
    emit documentRevisionChanged(changed.first, changed.second);
}

void Session::invalidateModelCommands(uint modelID)
{
    QPair<QString, quint64> changed;
    if (!this->advanceModelCommandStamp(modelID, nullptr, &changed)) return;
    this->queueProjectRevisionUpdate(changed);
}

bool Session::recordModelSaved(uint modelID, const QString &path, QString *error)
{
    if (QThread::currentThread() != thread() || modelID >= this->getNModels()
        || !this->getModel(modelID).getLoadedSourceBinding())
    {
        if (error) *error = tr("The expected native source binding is required to record a save.");
        return false;
    }
    return this->recordModelSaved(modelID, path, *this->getModel(modelID).getLoadedSourceBinding(), error);
}

bool Session::recordModelSaved(uint modelID, const QString &path,
                               const RProjectCatalog::SourceBinding &expectedWrittenBinding,
                               QString *error)
{
    if (!samePath(path, expectedWrittenBinding.logicalSourcePath))
    {
        if (error) *error = tr("The native save path does not match the worker's written source binding.");
        return false;
    }
    QStringList actualWritePaths;
    for (const auto &artifact : expectedWrittenBinding.sourceArtifacts)
        actualWritePaths.append(artifact.path);
    auto lease = this->reserveModelSave(modelID, expectedWrittenBinding.logicalSourcePath,
                                        actualWritePaths, error);
    if (!lease) return false;
    return this->recordModelSaved(modelID, expectedWrittenBinding.logicalSourcePath,
                                  expectedWrittenBinding, lease, error);
}

bool Session::recordModelSaved(uint modelID, const QString &path,
                               const ProjectPublicationLease &lease, QString *error)
{
    if (QThread::currentThread() != thread() || modelID >= this->getNModels()
        || !this->getModel(modelID).getLoadedSourceBinding())
    {
        if (error) *error = tr("The expected native source binding is required to record a save.");
        return false;
    }
    return this->recordModelSaved(modelID, path, *this->getModel(modelID).getLoadedSourceBinding(),
                                   lease, this->modelCommandStamp(modelID), error);
}

bool Session::recordModelSaved(uint modelID, const QString &path,
                               const RProjectCatalog::SourceBinding &expectedWrittenBinding,
                               const ProjectPublicationLease &lease, QString *error)
{
    return this->recordModelSaved(modelID, path, expectedWrittenBinding, lease,
                                  this->modelCommandStamp(modelID), error);
}

bool Session::recordModelSaved(uint modelID, const QString &path,
                               const ProjectPublicationLease &lease,
                               const QPair<QString, quint64> &expectedStamp, QString *error)
{
    if (QThread::currentThread() != thread() || modelID >= this->getNModels()
        || !this->getModel(modelID).getLoadedSourceBinding())
    {
        if (error) *error = tr("The expected native source binding is required to record a save.");
        this->abortModelSave(expectedStamp.first, lease, error ? *error : QString());
        return false;
    }
    return this->recordModelSaved(modelID, path, *this->getModel(modelID).getLoadedSourceBinding(),
                                  lease, expectedStamp, error);
}

bool Session::recordModelSaved(uint modelID, const QString &path,
                               const RProjectCatalog::SourceBinding &expectedWrittenBinding,
                               const ProjectPublicationLease &lease,
                               const QPair<QString, quint64> &expectedStamp, QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Project save metadata must be updated on the document thread.");
        return false;
    }
    if (expectedStamp.first.isEmpty() || this->modelCommandStamp(modelID) != expectedStamp)
    {
        const QString message = tr("The model changed or closed while saving; the saved snapshot was not attached.");
        if (error) *error = message;
        this->abortModelSave(expectedStamp.first, lease, message);
        return false;
    }
    auto binding = this->projectDocuments.find(expectedStamp.first);
    if (binding == this->projectDocuments.end() || binding->persistentDocumentId.isEmpty()
        || !this->projectCatalog || !lease)
    {
        const QString message = binding == this->projectDocuments.end()
                                    ? tr("The model has no project document binding.")
                                    : binding->persistenceMessage;
        if (error) *error = message;
        this->abortModelSave(expectedStamp.first, lease, message);
        return false;
    }
    if (!binding->publishing || binding->publicationLease != lease
        || binding->publicationSourceRevision != binding->acceptedSourceRevision
        || lease->persistentDocumentId() != binding->persistentDocumentId
        || lease->expectedSourceRevision() != binding->publicationSourceRevision)
    {
        const QString message = tr("The native save publication lease no longer matches this document.");
        if (error) *error = message;
        this->abortModelSave(expectedStamp.first, lease, message);
        return false;
    }
    if (!samePath(path, expectedWrittenBinding.logicalSourcePath))
    {
        const QString message = tr("The native save path does not match the worker's staged source binding.");
        if (error) *error = message;
        this->abortModelSave(expectedStamp.first, lease, message);
        return false;
    }
    if (binding->sourceConflict || binding->catalogRevisionConflict || binding->persistenceUnavailable
        || binding->draftMetadataPending || this->hasDocumentDraft(modelID))
    {
        const QString message = binding->sourceConflict ? binding->sourceConflictMessage
                               : binding->persistenceMessage.isEmpty()
                                     ? tr("The project document is not ready to publish this save.")
                                     : binding->persistenceMessage;
        if (error) *error = message;
        this->abortModelSave(expectedStamp.first, lease, message);
        return false;
    }

    auto abortOnFailure = qScopeGuard([this, &expectedStamp, &lease] {
        this->abortModelSave(expectedStamp.first, lease);
    });
    QPair<QString, quint64> savedStamp;
    if (!this->advanceModelCommandStamp(modelID, &expectedStamp, &savedStamp))
    {
        if (error) *error = tr("The model changed while saving; the saved snapshot was not attached.");
        return false;
    }

    QString projectError;
    QString errorCode;
    const bool projectSaved = this->projectCatalog->recordSave(binding->persistentDocumentId,
                                                                expectedWrittenBinding, lease,
                                                                binding->publicationSourceRevision,
                                                                &projectError, &errorCode,
                                                                binding->instanceLease);
    if (!projectSaved)
    {
        binding->lastCatalogErrorCode = errorCode;
        binding->persistenceMessage = projectError;
        if (errorCode == QStringLiteral("revision_conflict"))
            binding->catalogRevisionConflict = true;
        else if (errorCode == QStringLiteral("source_conflict"))
        {
            binding->sourceConflict = true;
            binding->sourceConflictPath = expectedWrittenBinding.logicalSourcePath;
            binding->sourceConflictMessage = projectError;
        }
        else if (errorCode != QStringLiteral("output_path_conflict")
                 && errorCode != QStringLiteral("invalid_argument"))
            binding->persistenceUnavailable = true;
        if (error) *error = projectError;
        return false;
    }

    this->getModel(modelID).setFileName(expectedWrittenBinding.logicalSourcePath);
    this->getModel(modelID).setLoadedSourceBinding(expectedWrittenBinding);
    this->getModel(modelID).clearNativeReadNormalization();
    binding->sourceAtLoad = expectedWrittenBinding;
    binding->sourcePath = expectedWrittenBinding.logicalSourcePath;
    binding->acceptedSavedArtifactHash = expectedWrittenBinding.savedArtifactHash;
    binding->acceptedSourceArtifacts = sourceArtifactsJson(expectedWrittenBinding.sourceArtifacts);
    binding->sourceUnverified = false;
    binding->unverifiedSourcePath.clear();
    binding->detachedFromChangedSource = false;
    binding->sourceLinkStatus.clear();
    binding->reopenedSavedSource = false;
    binding->sourceConflict = false;
    binding->sourceConflictPath.clear();
    binding->sourceConflictMessage.clear();

    const auto savedDocument = this->projectCatalog->document(binding->persistentDocumentId);
    if (savedDocument)
    {
        binding->acceptedSourceRevision = savedDocument->sourceRevision;
        binding->acceptedModelSha256 = savedDocument->modelSha256;
        binding->originDocumentId = savedDocument->originDocumentId;
        binding->orphanSnapshots = savedDocument->orphanSnapshots;
    }
    binding->acceptedDirty = false;
    binding->catalogAcceptedRuntimeRevision = savedStamp.second;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates.remove(savedStamp.first);
    }
    emit documentRevisionChanged(savedStamp.first, savedStamp.second);
    binding->drafts.clear();
    binding->knownDraftScopes.insert(QStringLiteral("analysis.modal"));
    binding->draftMetadataPending = false;
    binding->pendingDraftDirty = false;
    binding->persistenceUnavailable = false;
    binding->sourceVerificationUnavailable = false;
    binding->persistenceMessage.clear();
    binding->lastCatalogErrorCode.clear();

    binding->publishing = false;
    binding->publicationLease.reset();
    binding->publicationSourceRevision.clear();
    binding->publicationLogicalPath.clear();
    binding->publicationPaths.clear();

    QString hostError;
    const bool hostSaved = this->commandHost->recordArtifactPublished(modelID,
                                                                      expectedWrittenBinding.logicalSourcePath,
                                                                      &hostError);
    if (!hostSaved && error) *error = hostError;
    else if (binding->persistenceUnavailable && error) *error = binding->persistenceMessage;
    abortOnFailure.dismiss();
    return hostSaved && !binding->persistenceUnavailable && !binding->sourceConflict;
}

void Session::abortModelSave(const QString &runtimeDocumentId,
                             const ProjectPublicationLease &lease,
                             const QString &reason)
{
    if (runtimeDocumentId.isEmpty()) return;
    if (QThread::currentThread() != thread())
    {
        QMetaObject::invokeMethod(this, [this, runtimeDocumentId, lease, reason] {
            this->abortModelSave(runtimeDocumentId, lease, reason);
        }, Qt::QueuedConnection);
        return;
    }
    auto binding = this->projectDocuments.find(runtimeDocumentId);
    if (binding == this->projectDocuments.end()) return;
    if (lease && binding->publicationLease && binding->publicationLease != lease) return;
    if (!binding->publishing && !binding->publicationLease) return;

    binding->publishing = false;
    binding->publicationLease.reset();
    binding->publicationSourceRevision.clear();
    binding->publicationLogicalPath.clear();
    binding->publicationPaths.clear();
    if (!reason.isEmpty() && !binding->sourceConflict && !binding->catalogRevisionConflict)
        binding->persistenceMessage = reason;

    this->refreshProjectSourceConflict(runtimeDocumentId);
    bool pendingMetadata = binding->draftMetadataPending || binding->persistenceUnavailable
                           || binding->sourceVerificationUnavailable;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        pendingMetadata = pendingMetadata || this->pendingProjectUpdates.contains(runtimeDocumentId);
    }
    if (pendingMetadata) this->scheduleProjectCatalogRetry();
}
bool Session::openProject(const QString &workspace, QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Project workspaces must be opened on the document thread.");
        return false;
    }
    const QString requestedPath = normalizedPath(workspace);
    if (requestedPath.isEmpty())
    {
        if (error) *error = tr("A project workspace directory is required.");
        return false;
    }
    if (!this->models.isEmpty())
    {
        if (this->projectCatalog && samePath(this->projectCatalog->workspace(), requestedPath))
            return true;
        if (error) *error = tr("Close all open models before changing the project workspace.");
        return false;
    }

    QString localError;
    auto catalog = RProjectCatalog::open(workspace, &localError);
    if (!catalog)
    {
        this->projectOpenError = localError;
        if (error) *error = localError;
        return false;
    }
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates.clear();
    }
    this->projectDocuments.clear();
    this->projectCatalog = std::move(catalog);
    this->projectOpenError.clear();
    return true;
}

QString Session::projectWorkspace() const
{
    return this->projectCatalog ? this->projectCatalog->workspace() : QString();
}

QString Session::projectDocumentLabel(const QString &baseName) const
{
    QString base = baseName.trimmed();
    if (base.isEmpty()) base = tr("Model");
    QString sanitized;
    sanitized.reserve(base.size());
    for (const QChar ch : base)
        if (ch.unicode() >= 0x20 && ch.unicode() != 0x7f) sanitized.append(ch);
    if (sanitized.trimmed().isEmpty()) sanitized = tr("Model");
    while (sanitized.toUtf8().size() > 256) sanitized.chop(1);
    return sanitized;
}

void Session::attachProjectDocument(uint modelID, const QString &runtimeIdentity,
                                    const std::optional<RProjectCatalog::SourceBinding> &sourceBinding,
                                    const QString &unverifiedSourcePath)
{
    ProjectDocumentBinding binding;
    binding.catalogAcceptedRuntimeRevision = this->modelCommandStamp(modelID).second;
    if (!this->projectCatalog)
    {
        binding.persistenceUnavailable = true;
        binding.persistenceMessage = this->projectOpenError.isEmpty()
                                         ? tr("The project catalog is unavailable.")
                                         : this->projectOpenError;
        this->projectDocuments.insert(runtimeIdentity, binding);
        return;
    }
    if (QThread::currentThread() != thread() || modelID >= this->getNModels())
    {
        binding.persistenceUnavailable = true;
        binding.persistenceMessage = tr("The model could not be registered on the document thread.");
        this->projectDocuments.insert(runtimeIdentity, binding);
        return;
    }

    Model &model = this->getModel(modelID);
    binding.label = this->projectDocumentLabel(model.getName());
    if (sourceBinding && !model.getFileName().isEmpty()
        && samePath(model.getFileName(), sourceBinding->logicalSourcePath))
    {
        binding.sourceAtLoad = *sourceBinding;
        binding.sourcePath = sourceBinding->logicalSourcePath;
        model.setLoadedSourceBinding(*sourceBinding);
    }
    else
    {
        binding.sourcePath.clear();
        binding.unverifiedSourcePath = sourceBinding ? sourceBinding->logicalSourcePath
                                         : unverifiedSourcePath.isEmpty() ? model.getFileName()
                                                                          : unverifiedSourcePath;
        if (!binding.unverifiedSourcePath.isEmpty())
        {
            binding.sourceUnverified = true;
            if (sourceBinding)
            {
                binding.sourceLinkStatus = QStringLiteral("unverified");
            }
            model.setFileName(QString());
            model.clearLoadedSourceBinding();
        }
    }
    this->projectDocuments.insert(runtimeIdentity, binding);
    this->retryProjectDocumentRegistration(runtimeIdentity);
}

bool Session::retryProjectDocumentRegistration(const QString &runtimeIdentity)
{
    auto binding = this->projectDocuments.find(runtimeIdentity);
    if (binding == this->projectDocuments.end() || !this->projectCatalog) return false;
    if (QThread::currentThread() != thread())
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = tr("The project catalog may only be used on the document thread.");
        return false;
    }
    QString registrationError;
    QString errorCode;
    QString persistentId = binding->persistentDocumentId;
    const uint modelID = this->findModelByCommandIdentity(runtimeIdentity);
    if (modelID == RConstants::eod) return false;

    auto detachAsCopy = [this, &binding, modelID, &registrationError](const QString &originId,
                                                                    bool unverified) {
        QString copyId;
        if (!originId.isEmpty())
            copyId = this->projectCatalog->createCopy(originId, binding->label, &registrationError);
        else
            copyId = this->projectCatalog->createDocument(binding->label, {}, &registrationError);
        if (copyId.isEmpty()) return QString();
        binding->originDocumentId = originId;
        binding->persistentDocumentId = copyId;
        binding->sourcePath.clear();
        binding->sourceAtLoad.reset();
        binding->sourceConflict = false;
        binding->sourceConflictPath.clear();
        binding->sourceConflictMessage.clear();
        binding->sourceUnverified = unverified;
        if (unverified && binding->unverifiedSourcePath.isEmpty())
            binding->unverifiedSourcePath = binding->sourcePath;
        this->getModel(modelID).setFileName(QString());
        this->getModel(modelID).clearLoadedSourceBinding();
        return copyId;
    };

    if (persistentId.isEmpty())
    {
        if (binding->sourceAtLoad)
        {
            persistentId = this->projectCatalog->registerDocument(*binding->sourceAtLoad, binding->label,
                                                                   {}, &registrationError, &errorCode);
            if (persistentId.isEmpty() && errorCode == QStringLiteral("source_conflict"))
            {
                QString queryError;
                const QJsonObject project = this->projectCatalog->query(&queryError);
                QString originId;
                const QString logicalPath = binding->sourceAtLoad->logicalSourcePath;
                for (const QJsonValue &value : project.value(QStringLiteral("documents")).toArray())
                {
                    const QJsonObject document = value.toObject();
                    if (document.value(QStringLiteral("tombstone")).toBool()) continue;
                    bool sameArtifact = samePath(document.value(QStringLiteral("sourcePath")).toString(), logicalPath);
                    for (const QJsonValue &artifact : document.value(QStringLiteral("sourceArtifacts")).toArray())
                        sameArtifact = sameArtifact || samePath(artifact.toObject().value(QStringLiteral("path")).toString(),
                                                                 logicalPath);
                    if (sameArtifact)
                    {
                        originId = document.value(QStringLiteral("persistentDocumentId")).toString();
                        break;
                    }
                }
                binding->unverifiedSourcePath = logicalPath;
                persistentId = detachAsCopy(originId, true);
                if (!persistentId.isEmpty())
                {
                    binding->sourceConflict = false;
                    binding->sourceConflictPath.clear();
                    binding->sourceConflictMessage.clear();
                    binding->detachedFromChangedSource = true;
                    binding->sourceLinkStatus = QStringLiteral("changedSinceRead");
                    registrationError.clear();
                }
            }
        }
        else
        {
            persistentId = this->projectCatalog->createDocument(binding->label, {}, &registrationError);
        }
        if (persistentId.isEmpty())
        {
            if (registrationError.contains(QStringLiteral("lock"), Qt::CaseInsensitive))
                errorCode = QStringLiteral("catalog_locked");
            binding->persistenceUnavailable = true;
            binding->lastCatalogErrorCode = errorCode;
            binding->persistenceMessage = registrationError;
            if (errorCode == QStringLiteral("catalog_locked") || errorCode == QStringLiteral("io_error"))
                this->scheduleProjectCatalogRetry();
            return false;
        }
    }

    binding->persistentDocumentId = persistentId;
    if (!binding->instanceLease)
    {
        const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
        RProjectCatalog::InstanceRef instanceRef;
        instanceRef.documentId = runtimeIdentity;
        instanceRef.sessionEpoch = this->commandHost->sessionEpoch();
        instanceRef.documentRevision = stamp.second;
        binding->instanceLease = this->projectCatalog->acquireDocumentInstance(persistentId, instanceRef,
                                                                                &registrationError, &errorCode);
        if (!binding->instanceLease && errorCode == QStringLiteral("document_in_use"))
        {
            const QString originId = persistentId;
            persistentId = detachAsCopy(originId, false);
            if (!persistentId.isEmpty())
            {
                instanceRef.documentRevision = this->modelCommandStamp(modelID).second;
                binding->instanceLease = this->projectCatalog->acquireDocumentInstance(
                    persistentId, instanceRef, &registrationError, &errorCode);
            }
        }
        if (!binding->instanceLease)
        {
            if (registrationError.contains(QStringLiteral("lock"), Qt::CaseInsensitive))
                errorCode = QStringLiteral("catalog_locked");
            binding->persistenceUnavailable = true;
            binding->lastCatalogErrorCode = errorCode;
            binding->persistenceMessage = registrationError;
            if (errorCode == QStringLiteral("catalog_locked") || errorCode == QStringLiteral("io_error"))
                this->scheduleProjectCatalogRetry();
            return false;
        }
    }

    binding->persistentDocumentId = persistentId;
    binding->persistenceUnavailable = false;
    binding->persistenceMessage.clear();
    binding->lastCatalogErrorCode.clear();
    QString documentError;
    auto document = this->projectCatalog->document(persistentId, &documentError);
    if (!document)
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = documentError;
        if (documentError.contains(QStringLiteral("locked"), Qt::CaseInsensitive))
            this->scheduleProjectCatalogRetry();
        return false;
    }

    auto hydrateDraftMetadata = [binding](const QJsonObject &draftMetadata) {
        const QJsonArray changedFields = draftMetadata.value(QStringLiteral("changedFields")).toArray();
        const QString draftStatus = draftMetadata.value(QStringLiteral("status")).toString();
        const int fieldCount = draftMetadata.value(QStringLiteral("fieldCount")).toInt();
        if (draftStatus == QStringLiteral("pending") && fieldCount > 0)
        {
            int hydratedFields = 0;
            for (const QJsonValue &value : changedFields)
            {
                const QString fieldPath = value.toString();
                if (fieldPath == QStringLiteral("analysis.modal")) continue;
                const qsizetype separator = fieldPath.lastIndexOf(QLatin1Char('.'));
                if (separator <= 0 || separator == fieldPath.size() - 1) continue;
                const QString scope = fieldPath.left(separator);
                const QString field = fieldPath.mid(separator + 1);
                if (!binding->drafts.value(scope).contains(field))
                    binding->drafts[scope].insert(field, QStringLiteral("pending"));
                binding->knownDraftScopes.insert(scope);
                ++hydratedFields;
            }
            if (hydratedFields == 0)
                binding->drafts[QStringLiteral("unknown")].insert(QStringLiteral("unknownField"),
                                                                    QStringLiteral("pending"));
        }
        else if (draftStatus == QStringLiteral("none")
                 && draftMetadata.value(QStringLiteral("kind")).toString() == QStringLiteral("analysis")
                 && fieldCount == 0 && changedFields.contains(QStringLiteral("analysis.modal")))
            binding->knownDraftScopes.insert(QStringLiteral("analysis.modal"));
    };
    hydrateDraftMetadata(document->draftMetadata);

    if (!document->sourcePath.isEmpty() && !binding->sourcePath.isEmpty()
        && !samePath(document->sourcePath, binding->sourcePath))
    {
        binding->sourceConflict = true;
        binding->sourceConflictPath = document->sourcePath;
        binding->sourceConflictMessage = tr("The loaded source does not match the project's registered source alias.");
    }
    binding->sourcePath = document->sourcePath.isEmpty() ? binding->sourcePath : document->sourcePath;
    binding->acceptedSourceRevision = document->sourceRevision;
    binding->acceptedSavedArtifactHash = document->savedArtifactHash;
    binding->acceptedModelSha256 = document->modelSha256;
    binding->acceptedSourceArtifacts = sourceArtifactsJson(document->sourceArtifacts);
    binding->originDocumentId = document->originDocumentId.isEmpty()
                                    ? binding->originDocumentId : document->originDocumentId;
    binding->orphanSnapshots = document->orphanSnapshots;
    binding->acceptedDirty = document->dirty;

    if (!binding->sourceConflict && !document->sourcePath.isEmpty() && document->dirty)
    {
        const bool loadedSameAlias = binding->sourceAtLoad
                                     && samePath(binding->sourceAtLoad->logicalSourcePath,
                                                 document->sourcePath);
        const bool reopenedSavedBytes = loadedSameAlias
                                        && binding->sourceAtLoad->savedArtifactHash
                                               == document->savedArtifactHash;
        if (!reopenedSavedBytes)
        {
            binding->sourceConflict = true;
            binding->sourceConflictPath = document->sourcePath;
            binding->sourceConflictMessage = tr("The saved source changed or lacks validated reload provenance.");
        }
        else
        {
            const QString revision = nextSourceRevision(document->sourceRevision);
            if (revision.isEmpty())
            {
                binding->persistenceUnavailable = true;
                binding->persistenceMessage = tr("The project source revision reached the v1 monotonic bound.");
                return false;
            }
            errorCode.clear();
            bool reopenedSavedSource = false;
            if (this->projectCatalog->acceptReload(*binding->sourceAtLoad, persistentId, revision,
                                                   &registrationError, document->sourceRevision, &errorCode,
                                                   &reopenedSavedSource, binding->instanceLease))
            {
                binding->reopenedSavedSource = reopenedSavedSource;
                binding->acceptedSourceRevision = revision;
                binding->acceptedDirty = false;
                // acceptReload deliberately drops old draft metadata. Restore only
                // scope and field descriptors; raw editor text remains in the GUI.
                const QJsonObject draftMetadata = this->projectDraftMetadata(*binding);
                if (draftMetadata.value(QStringLiteral("status")).toString() != QStringLiteral("unknown"))
                {
                    QString draftError;
                    QString draftErrorCode;
                    if (!this->projectCatalog->recordAccepted(persistentId, revision, false,
                                                              draftMetadata, &draftError, revision,
                                                              &draftErrorCode, binding->instanceLease))
                    {
                        binding->persistenceUnavailable = true;
                        binding->lastCatalogErrorCode = draftErrorCode;
                        binding->persistenceMessage = draftError;
                        if (draftErrorCode == QStringLiteral("catalog_locked"))
                            this->scheduleProjectCatalogRetry();
                        return false;
                    }
                }
                document = this->projectCatalog->document(persistentId, &documentError);
            }
            else
            {
                binding->lastCatalogErrorCode = errorCode;
                if (errorCode == QStringLiteral("source_conflict"))
                {
                    binding->sourceConflict = true;
                    binding->sourceConflictPath = binding->sourcePath;
                    binding->sourceConflictMessage = registrationError;
                }
                else if (errorCode == QStringLiteral("revision_conflict"))
                {
                    binding->catalogRevisionConflict = true;
                    binding->persistenceMessage = registrationError;
                }
                else
                {
                    binding->persistenceUnavailable = true;
                    binding->persistenceMessage = registrationError;
                    if (errorCode == QStringLiteral("catalog_locked")) this->scheduleProjectCatalogRetry();
                }
                return false;
            }
        }
    }
    if (!document)
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = documentError;
        if (documentError.contains(QStringLiteral("locked"), Qt::CaseInsensitive))
            this->scheduleProjectCatalogRetry();
        return false;
    }

    binding->acceptedSourceRevision = document->sourceRevision;
    binding->acceptedSavedArtifactHash = document->savedArtifactHash;
    binding->acceptedModelSha256 = document->modelSha256;
    binding->acceptedSourceArtifacts = sourceArtifactsJson(document->sourceArtifacts);
    binding->orphanSnapshots = document->orphanSnapshots;
    binding->acceptedDirty = document->dirty;
    binding->catalogRevisionConflict = false;
    if (!binding->sourceConflict)
    {
        binding->sourceConflictPath.clear();
        binding->sourceConflictMessage.clear();
    }
    binding->persistenceUnavailable = false;
    binding->sourceVerificationUnavailable = false;
    binding->persistenceMessage.clear();
    binding->lastCatalogErrorCode.clear();
    binding->catalogAcceptedRuntimeRevision = this->modelCommandStamp(modelID).second;
    return !binding->sourceConflict && !binding->catalogRevisionConflict;
}

QJsonObject Session::projectContext() const
{
    auto *self = const_cast<Session *>(this);
    if (QThread::currentThread() != thread())
        return {{QStringLiteral("availability"), QStringLiteral("unavailable")},
                {QStringLiteral("error"), tr("Project context must be queried on the document thread.")}};
    self->flushProjectCatalogUpdates();
    if (!this->projectCatalog)
        return {{QStringLiteral("availability"), QStringLiteral("unavailable")},
                {QStringLiteral("error"), this->projectOpenError}};
    QString error;
    QJsonObject context = this->projectCatalog->query(&error);
    if (context.isEmpty() && !error.isEmpty())
        return {{QStringLiteral("availability"), QStringLiteral("unavailable")},
                {QStringLiteral("workspace"), this->projectCatalog->workspace()},
                {QStringLiteral("error"), error}};
    context.insert(QStringLiteral("availability"), QStringLiteral("available"));
    return context;
}

QJsonObject Session::projectDraftMetadata(const ProjectDocumentBinding &binding) const
{
    QJsonArray changedFields;
    int fieldCount = 0;
    for (auto scope = binding.drafts.cbegin(); scope != binding.drafts.cend(); ++scope)
        for (auto field = scope->cbegin(); field != scope->cend(); ++field)
        {
            ++fieldCount;
            changedFields.append(scope.key() + QLatin1Char('.') + field.key());
        }
    const bool pending = fieldCount > 0;
    const bool modalKnown = binding.knownDraftScopes.contains(QStringLiteral("analysis.modal"));
    if (!pending && modalKnown) changedFields.append(QStringLiteral("analysis.modal"));
    return {{QStringLiteral("status"), pending ? QStringLiteral("pending") : modalKnown ? QStringLiteral("none") : QStringLiteral("unknown")},
            {QStringLiteral("kind"), pending || modalKnown ? QStringLiteral("analysis") : QStringLiteral("other")},
            {QStringLiteral("changedFields"), changedFields},
            {QStringLiteral("fieldCount"), fieldCount}};
}

bool Session::refreshProjectSourceConflict(const QString &runtimeIdentity)
{
    auto binding = this->projectDocuments.find(runtimeIdentity);
    if (binding == this->projectDocuments.end()) return false;
    if (binding->publishing) return binding->sourceConflict || binding->catalogRevisionConflict;
    if (!this->projectCatalog)
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = this->projectOpenError;
        return false;
    }
    if (binding->persistentDocumentId.isEmpty() || !binding->instanceLease)
    {
        this->retryProjectDocumentRegistration(runtimeIdentity);
        binding = this->projectDocuments.find(runtimeIdentity);
        if (binding == this->projectDocuments.end() || binding->persistentDocumentId.isEmpty()
            || !binding->instanceLease) return false;
    }
    QString error;
    std::optional<RProjectCatalog::Document> document;
    if (!binding->publishing)
        document = this->projectCatalog->document(binding->persistentDocumentId, &error);
    if (!document || document->tombstone)
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = error.isEmpty() ? tr("The persistent project document is unavailable.") : error;
        binding->lastCatalogErrorCode = error.contains(QStringLiteral("locked"), Qt::CaseInsensitive)
                                            ? QStringLiteral("catalog_locked") : QStringLiteral("io_error");
        if (error.contains(QStringLiteral("locked"), Qt::CaseInsensitive)) this->scheduleProjectCatalogRetry();
        return binding->sourceConflict || binding->catalogRevisionConflict;
    }
    binding->catalogRevisionConflict = document->sourceRevision != binding->acceptedSourceRevision;
    if (binding->catalogRevisionConflict)
    {
        binding->persistenceMessage = tr("The project advanced from locally accepted revision %1 to %2.")
                                          .arg(binding->acceptedSourceRevision, document->sourceRevision);
    }
    if (!document->sourcePath.isEmpty()
        && (!samePath(document->sourcePath, binding->sourcePath)
            || document->savedArtifactHash != binding->acceptedSavedArtifactHash
            || sourceArtifactsJson(document->sourceArtifacts) != binding->acceptedSourceArtifacts))
    {
        binding->sourceConflict = true;
        binding->sourceConflictPath = document->sourcePath;
        binding->sourceConflictMessage = tr("The registered source alias changed outside this runtime document.");
    }
    bool sourceCheckUnavailable = false;
    if (!document->sourcePath.isEmpty())
    {
        QString sourceError;
        QString sourceErrorCode;
        const auto source = RProjectCatalog::resolveSourceBinding(document->sourcePath, &sourceError,
                                                                  &sourceErrorCode);
        if (!source)
        {
            if (sourceErrorCode == QStringLiteral("source_conflict"))
            {
                binding->sourceConflict = true;
                binding->sourceConflictPath = document->sourcePath;
                binding->sourceConflictMessage = sourceError;
            }
            else
            {
                binding->persistenceUnavailable = true;
                binding->lastCatalogErrorCode = sourceErrorCode;
                binding->persistenceMessage = sourceError;
                sourceCheckUnavailable = true;
                binding->sourceVerificationUnavailable = true;
                if (sourceErrorCode == QStringLiteral("catalog_locked")
                    || sourceErrorCode == QStringLiteral("io_error"))
                    this->scheduleProjectCatalogRetry();
            }
        }
        else if (source->savedArtifactHash != document->savedArtifactHash
                 || sourceArtifactsJson(source->sourceArtifacts) != sourceArtifactsJson(document->sourceArtifacts))
        {
            binding->sourceConflict = true;
            binding->sourceConflictPath = document->sourcePath;
            binding->sourceConflictMessage = tr("The registered native source bytes changed outside this document.");
        }
        else
        {
            binding->sourceVerificationUnavailable = false;
        }
    }
    else
    {
        binding->sourceVerificationUnavailable = false;
    }
    const uint modelID = this->findModelByCommandIdentity(runtimeIdentity);
    const quint64 runtimeRevision = modelID == RConstants::eod ? 0
                                                               : this->modelCommandStamp(modelID).second;
    bool queuedRevision = false;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        queuedRevision = this->pendingProjectUpdates.contains(runtimeIdentity);
    }
    const bool unsynchronized = binding->draftMetadataPending || queuedRevision
                                || runtimeRevision > binding->catalogAcceptedRuntimeRevision;
    if (!sourceCheckUnavailable && !binding->sourceVerificationUnavailable
        && !binding->sourceConflict && !binding->catalogRevisionConflict
        && !unsynchronized)
    {
        binding->persistenceUnavailable = false;
        binding->persistenceMessage.clear();
        binding->lastCatalogErrorCode.clear();
    }
    else if (unsynchronized && !binding->sourceConflict && !binding->catalogRevisionConflict)
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = tr("The current runtime revision has not synchronized with the project catalog.");
    }
    return binding->sourceConflict || binding->catalogRevisionConflict;
}

QJsonObject Session::documentProjectContext(uint modelID) const
{
    auto *self = const_cast<Session *>(this);
    if (QThread::currentThread() != thread())
        return {{QStringLiteral("availability"), QStringLiteral("unavailable")},
                {QStringLiteral("persistence"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unavailable")}}},
                {QStringLiteral("draft"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")}}},
                {QStringLiteral("unsaved"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")}}}};
    self->flushProjectCatalogUpdates();
    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    if (stamp.first.isEmpty() || !this->projectCatalog)
        return {{QStringLiteral("availability"), QStringLiteral("unavailable")},
                {QStringLiteral("persistenceAvailability"), QStringLiteral("unavailable")},
                {QStringLiteral("persistence"),
                 QJsonObject{{QStringLiteral("status"), QStringLiteral("unavailable")},
                             {QStringLiteral("reason"), this->projectOpenError}}},
                {QStringLiteral("draft"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")}}},
                {QStringLiteral("unsaved"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")}}}};
    self->refreshProjectSourceConflict(stamp.first);
    const auto binding = this->projectDocuments.constFind(stamp.first);
    if (binding == this->projectDocuments.cend() || binding->persistentDocumentId.isEmpty())
        return {{QStringLiteral("availability"), QStringLiteral("unavailable")},
                {QStringLiteral("persistenceAvailability"), QStringLiteral("unavailable")},
                {QStringLiteral("sourceConflict"), binding != this->projectDocuments.cend() && binding->sourceConflict},
                {QStringLiteral("persistence"),
                 QJsonObject{{QStringLiteral("status"), QStringLiteral("unavailable")},
                             {QStringLiteral("reason"), binding == this->projectDocuments.cend()
                                                              ? tr("No project document is mapped to this runtime document.")
                                                              : binding->persistenceMessage}}},
                {QStringLiteral("draft"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")}}},
                {QStringLiteral("unsaved"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")}}}};

    QString error;
    std::optional<RProjectCatalog::Document> document;
    if (!binding->publishing)
        document = this->projectCatalog->document(binding->persistentDocumentId, &error);
    QString originDocumentId = binding->originDocumentId;
    QJsonArray orphanSnapshots = binding->orphanSnapshots;
    if (document)
    {
        originDocumentId = document->originDocumentId;
        orphanSnapshots = document->orphanSnapshots;
    }

    QJsonArray changedFields;
    const auto modalDraft = binding->drafts.constFind(QStringLiteral("analysis.modal"));
    const bool modalPending = modalDraft != binding->drafts.cend() && !modalDraft->isEmpty();
    const bool anyDraftPending = this->hasDocumentDraft(modelID);
    if (modalPending)
        for (auto field = modalDraft->cbegin(); field != modalDraft->cend(); ++field)
            changedFields.append(field.key());
    const QString draftStatus = anyDraftPending ? QStringLiteral("pending")
                                              : binding->knownDraftScopes.contains(QStringLiteral("analysis.modal"))
                                                    ? QStringLiteral("none")
                                                    : QStringLiteral("unknown");
    const QString draftScope = modalPending ? QStringLiteral("analysis.modal")
                                            : anyDraftPending ? QStringLiteral("other")
                                                              : QStringLiteral("analysis.modal");
    bool queuedUpdate = false;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        queuedUpdate = this->pendingProjectUpdates.contains(stamp.first);
    }
    const bool metadataSynchronized = !binding->publishing && !binding->draftMetadataPending && !queuedUpdate
                                      && binding->catalogAcceptedRuntimeRevision >= stamp.second;
    const QString persistenceStatus = binding->publishing
                                          ? QStringLiteral("publishing")
                                          : binding->sourceConflict || binding->catalogRevisionConflict
                                                ? QStringLiteral("conflict")
                                                : binding->persistenceUnavailable || !metadataSynchronized
                                                      ? QStringLiteral("unavailable")
                                                      : QStringLiteral("available");
    QJsonObject persistence{{QStringLiteral("status"),
                             persistenceStatus},
                            {QStringLiteral("workspace"), this->projectCatalog->workspace()},
                            {QStringLiteral("sourceConflict"), binding->sourceConflict},
                            {QStringLiteral("sourceConflictPath"), binding->sourceConflictPath},
                            {QStringLiteral("sourceArtifacts"), binding->acceptedSourceArtifacts},
                            {QStringLiteral("sourceUnverified"), binding->sourceUnverified},
                            {QStringLiteral("unverifiedSourcePath"), binding->unverifiedSourcePath},
                            {QStringLiteral("detachedFromChangedSource"), binding->detachedFromChangedSource},
                            {QStringLiteral("sourceLinkStatus"), binding->sourceLinkStatus},
                            {QStringLiteral("publishing"), binding->publishing},
                            {QStringLiteral("reason"), binding->sourceConflict ? binding->sourceConflictMessage
                                                                                : binding->persistenceMessage},
                            {QStringLiteral("catalogRevisionConflict"), binding->catalogRevisionConflict},
                            {QStringLiteral("metadataSynchronized"), metadataSynchronized}};
    return {{QStringLiteral("projectId"), this->projectCatalog->projectId()},
            {QStringLiteral("availability"), persistenceStatus == QStringLiteral("available")
                                                       || persistenceStatus == QStringLiteral("publishing")
                                                       ? QStringLiteral("available")
                                                       : QStringLiteral("unavailable")},
            {QStringLiteral("persistentDocumentId"), binding->persistentDocumentId},
            {QStringLiteral("label"), binding->label},
            {QStringLiteral("sourcePath"), binding->sourcePath},
            {QStringLiteral("sourceRevision"), binding->acceptedSourceRevision},
            {QStringLiteral("modelSha256"), binding->acceptedModelSha256},
            {QStringLiteral("savedArtifactHash"), binding->acceptedSavedArtifactHash},
            {QStringLiteral("originDocumentId"), originDocumentId},
            {QStringLiteral("persistence"), persistence},
            {QStringLiteral("persistenceAvailability"), persistence.value(QStringLiteral("status"))},
            {QStringLiteral("sourceConflict"), binding->sourceConflict},
            {QStringLiteral("sourceUnverified"), binding->sourceUnverified},
            {QStringLiteral("detachedFromChangedSource"), binding->detachedFromChangedSource},
            {QStringLiteral("sourceLinkStatus"), binding->sourceLinkStatus},
            {QStringLiteral("publishing"), binding->publishing},
            {QStringLiteral("catalogRevisionConflict"), binding->catalogRevisionConflict},
            {QStringLiteral("reopenedSavedSource"), binding->reopenedSavedSource},
            {QStringLiteral("orphanSnapshots"), orphanSnapshots},
            {QStringLiteral("draft"),
             QJsonObject{{QStringLiteral("status"), draftStatus},
                         {QStringLiteral("scope"), draftScope},
                         {QStringLiteral("changedFields"), changedFields},
                         {QStringLiteral("otherScopesStatus"), anyDraftPending && !modalPending
                                                                       ? QStringLiteral("pending")
                                                                       : QStringLiteral("unknown")}}},
            {QStringLiteral("unsaved"),
             QJsonObject{{QStringLiteral("status"), binding->acceptedDirty ? QStringLiteral("unsaved")
                                                                           : QStringLiteral("saved")},
                         {QStringLiteral("dirty"), binding->acceptedDirty}}}};
}

bool Session::hasDocumentDraft(uint modelID) const
{
    if (QThread::currentThread() != thread()) return true;
    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    const auto binding = this->projectDocuments.constFind(stamp.first);
    if (stamp.first.isEmpty() || binding == this->projectDocuments.cend()) return false;
    for (auto scope = binding->drafts.cbegin(); scope != binding->drafts.cend(); ++scope)
        if (!scope->isEmpty()) return true;
    return false;
}

bool Session::canMutateDocument(uint modelID, QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Document mutations must be validated on the document thread.");
        return false;
    }
    this->flushProjectCatalogUpdates();
    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    auto binding = this->projectDocuments.find(stamp.first);
    if (stamp.first.isEmpty() || binding == this->projectDocuments.end() || !this->projectCatalog)
    {
        if (error) *error = tr("The model has no persistent project context.");
        return false;
    }
    if (binding->persistentDocumentId.isEmpty() || !binding->instanceLease)
        this->retryProjectDocumentRegistration(stamp.first);
    binding = this->projectDocuments.find(stamp.first);
    if (binding == this->projectDocuments.end() || binding->persistentDocumentId.isEmpty()
        || !binding->instanceLease)
    {
        if (error) *error = binding == this->projectDocuments.end()
                                ? tr("The model has no persistent project identity.")
                                : binding->persistenceMessage;
        return false;
    }
    this->refreshProjectSourceConflict(stamp.first);
    if (binding->publishing || binding->sourceConflict || binding->catalogRevisionConflict
        || binding->persistenceUnavailable)
    {
        if (error) *error = binding->publishing ? tr("A native save is being published for this document.")
                         : binding->sourceConflict ? binding->sourceConflictMessage
                                : binding->persistenceMessage.isEmpty()
                                      ? tr("The project document is not synchronized with this runtime state.")
                                      : binding->persistenceMessage;
        return false;
    }
    bool pendingUpdate = false;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        pendingUpdate = this->pendingProjectUpdates.contains(stamp.first);
    }
    if (this->hasDocumentDraft(modelID))
    {
        if (error) *error = tr("Resolve the pending numeric draft before changing the accepted model.");
        return false;
    }
    if (binding->draftMetadataPending || pendingUpdate
        || binding->catalogAcceptedRuntimeRevision < stamp.second)
    {
        if (error) *error = tr("Wait for project metadata to synchronize before changing the accepted model.");
        return false;
    }
    return true;
}

void Session::setNumericDraft(uint modelID, const QString &scope, const QString &field,
                              const QString &state, const QString &text)
{
    Q_UNUSED(text);
    if (QThread::currentThread() != thread() || scope.isEmpty() || field.isEmpty()) return;
    this->flushProjectCatalogUpdates();
    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    auto binding = this->projectDocuments.find(stamp.first);
    if (stamp.first.isEmpty() || binding == this->projectDocuments.end() || !this->projectCatalog) return;
    if (state == QStringLiteral("unknown"))
    {
        binding->drafts.remove(scope);
        binding->knownDraftScopes.remove(scope);
    }
    else if (state == QStringLiteral("none"))
    {
        auto fields = binding->drafts.find(scope);
        if (fields != binding->drafts.end())
        {
            fields->remove(field);
            if (fields->isEmpty()) binding->drafts.erase(fields);
        }
        binding->knownDraftScopes.insert(scope);
    }
    else
    {
        binding->drafts[scope].insert(field, state);
    }
    binding->draftMetadataPending = true;

    if (binding->persistentDocumentId.isEmpty())
    {
        this->retryProjectDocumentRegistration(stamp.first);
        this->scheduleProjectCatalogRetry();
        return;
    }
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates[stamp.first] = qMax(this->pendingProjectUpdates.value(stamp.first), stamp.second);
    }
    this->flushProjectCatalogUpdates();
}

void Session::clearNumericDraft(uint modelID, const QString &scope, const QString &field)
{
    this->setNumericDraft(modelID, scope, field, QStringLiteral("none"), {});
}

void Session::scheduleProjectCatalogRetry()
{
    if (QThread::currentThread() != thread() || this->projectRetryScheduled) return;
    this->projectRetryScheduled = true;
    QTimer::singleShot(100, this, [this] {
        this->projectRetryScheduled = false;
        this->flushProjectCatalogUpdates();
    });
}

void Session::flushProjectCatalogUpdates()
{
    if (QThread::currentThread() != thread() || !this->projectCatalog) return;
    QHash<QString, quint64> pending;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        pending.swap(this->pendingProjectUpdates);
    }
    QSet<QString> work;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) work.insert(it.key());
    for (auto it = this->projectDocuments.cbegin(); it != this->projectDocuments.cend(); ++it)
        if (it->draftMetadataPending || it->persistenceUnavailable || it->sourceVerificationUnavailable)
            work.insert(it.key());
    bool retryNeeded = false;
    auto retainPending = [this, &retryNeeded](const QString &identity, quint64 revision, bool retry) {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates[identity] = qMax(this->pendingProjectUpdates.value(identity), revision);
        retryNeeded = retryNeeded || retry;
    };

    for (const QString &identity : std::as_const(work))
    {
        const uint modelID = this->findModelByCommandIdentity(identity);
        if (modelID == RConstants::eod) continue;
        const QPair<QString, quint64> currentStamp = this->modelCommandStamp(modelID);
        auto binding = this->projectDocuments.find(identity);
        if (binding == this->projectDocuments.end() || currentStamp.first != identity) continue;
        if (binding->publishing)
        {
            retainPending(identity, currentStamp.second, false);
            continue;
        }
        const quint64 requestedRevision = pending.value(identity, currentStamp.second);
        if (requestedRevision > currentStamp.second) continue;
        const bool runtimeChanged = currentStamp.second > binding->catalogAcceptedRuntimeRevision;
        const bool draftPending = binding->draftMetadataPending;
        const bool availabilityCheck = binding->persistenceUnavailable || binding->sourceVerificationUnavailable;
        if (!runtimeChanged && !draftPending && !availabilityCheck) continue;

        if (binding->persistentDocumentId.isEmpty() || !binding->instanceLease)
        {
            this->retryProjectDocumentRegistration(identity);
            if (binding->persistentDocumentId.isEmpty() || !binding->instanceLease)
            {
                retainPending(identity, currentStamp.second, true);
                continue;
            }
        }
        const bool sourceConflict = this->refreshProjectSourceConflict(identity);
        if (sourceConflict || binding->catalogRevisionConflict)
        {
            if (runtimeChanged || draftPending) retainPending(identity, currentStamp.second, false);
            continue;
        }
        if (binding->sourceVerificationUnavailable)
        {
            if (runtimeChanged || draftPending) retainPending(identity, currentStamp.second, true);
            else retryNeeded = true;
            continue;
        }
        if (!runtimeChanged && !draftPending) continue;
        QString documentError;
        const auto document = this->projectCatalog->document(binding->persistentDocumentId, &documentError);
        if (!document)
        {
            binding->persistenceUnavailable = true;
            binding->persistenceMessage = documentError;
            retainPending(identity, currentStamp.second, true);
            continue;
        }
        if (document->sourceRevision != binding->acceptedSourceRevision)
        {
            binding->catalogRevisionConflict = true;
            binding->persistenceMessage = tr("The project advanced from locally accepted revision %1 to %2.")
                                              .arg(binding->acceptedSourceRevision, document->sourceRevision);
            retainPending(identity, currentStamp.second, false);
            continue;
        }
        const QString revision = runtimeChanged ? nextSourceRevision(binding->acceptedSourceRevision)
                                                : binding->acceptedSourceRevision;
        if (revision.isEmpty())
        {
            binding->persistenceUnavailable = true;
            binding->persistenceMessage = tr("The project source revision reached the v1 monotonic bound.");
            retainPending(identity, currentStamp.second, false);
            continue;
        }
        const bool dirty = runtimeChanged ? true : binding->acceptedDirty;
        QString error;
        QString errorCode;
        if (this->projectCatalog->recordAccepted(binding->persistentDocumentId, revision, dirty,
                                                 this->projectDraftMetadata(*binding), &error,
                                                 binding->acceptedSourceRevision, &errorCode,
                                                 binding->instanceLease))
        {
            binding->catalogAcceptedRuntimeRevision = currentStamp.second;
            binding->acceptedSourceRevision = revision;
            binding->acceptedDirty = dirty;
            if (runtimeChanged) binding->acceptedModelSha256.clear();
            binding->draftMetadataPending = false;
            binding->persistenceUnavailable = false;
            binding->sourceVerificationUnavailable = false;
            binding->persistenceMessage.clear();
            binding->lastCatalogErrorCode.clear();
        }
        else
        {
            binding->lastCatalogErrorCode = errorCode;
            binding->persistenceMessage = error;
            if (errorCode == QStringLiteral("revision_conflict"))
                binding->catalogRevisionConflict = true;
            else if (errorCode == QStringLiteral("source_conflict"))
            {
                binding->sourceConflict = true;
                binding->sourceConflictMessage = error;
            }
            else
                binding->persistenceUnavailable = true;
            const bool retry = errorCode != QStringLiteral("revision_conflict")
                               && errorCode != QStringLiteral("source_conflict");
            retainPending(identity, currentStamp.second, retry);
        }
    }
    if (retryNeeded) this->scheduleProjectCatalogRetry();
}

bool Session::canSaveModel(uint modelID, const QString &path, QString *error)
{
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Save targets must be checked on the document thread.");
        return false;
    }
    if (modelID >= this->getNModels())
    {
        if (error) *error = tr("The model is no longer part of this session.");
        return false;
    }
    const QStringList actualWritePaths = this->getModel(modelID).nativeWritePaths(path, true);
    return this->canSaveModel(modelID, path, actualWritePaths, error);
}

bool Session::canSaveModel(uint modelID, const QString &path, const QStringList &actualWritePaths,
                           QString *error)
{
    auto lease = this->reserveModelSave(modelID, path, actualWritePaths, error);
    if (!lease) return false;
    const QString identity = this->modelCommandStamp(modelID).first;
    this->abortModelSave(identity, lease);
    return true;
}

Session::ProjectPublicationLease Session::reserveModelSave(uint modelID, const QString &logicalPath,
                                                            const QStringList &actualWritePaths,
                                                            QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Save targets must be checked on the document thread.");
        return {};
    }
    this->flushProjectCatalogUpdates();
    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    auto binding = this->projectDocuments.find(stamp.first);
    if (stamp.first.isEmpty() || binding == this->projectDocuments.end() || !this->projectCatalog
        || binding->persistentDocumentId.isEmpty())
    {
        if (error) *error = binding == this->projectDocuments.end()
                                ? tr("The model has no persistent project identity.")
                                : binding->persistenceMessage;
        return {};
    }
    if (binding->publishing)
    {
        if (error) *error = tr("A native save is already being published for this document.");
        return {};
    }
    this->refreshProjectSourceConflict(stamp.first);
    if (binding->sourceConflict || binding->catalogRevisionConflict || binding->persistenceUnavailable)
    {
        if (error) *error = binding->sourceConflict ? binding->sourceConflictMessage
                                : binding->persistenceMessage.isEmpty()
                                      ? tr("The project document is not synchronized with this runtime state.")
                                      : binding->persistenceMessage;
        return {};
    }
    bool pendingMetadata = false;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        pendingMetadata = this->pendingProjectUpdates.contains(stamp.first);
    }
    if (this->hasDocumentDraft(modelID) || binding->draftMetadataPending
        || pendingMetadata || binding->catalogAcceptedRuntimeRevision < stamp.second)
    {
        if (error) *error = this->hasDocumentDraft(modelID)
                                ? tr("Resolve the pending numeric draft before saving this project document.")
                                : tr("Wait for the current document revision to synchronize before saving.");
        return {};
    }
    if (actualWritePaths.isEmpty() || logicalPath.trimmed().isEmpty())
    {
        if (error) *error = tr("The native save target paths are unavailable.");
        return {};
    }
    QStringList reservedPaths = actualWritePaths;
    if (!reservedPaths.contains(logicalPath)) reservedPaths.append(logicalPath);
    QString errorCode;
    QString localError;
    auto lease = this->projectCatalog->reservePublication(binding->persistentDocumentId,
                                                           reservedPaths,
                                                           binding->acceptedSourceRevision,
                                                           &localError, &errorCode,
                                                           binding->instanceLease);
    if (!lease)
    {
        binding->lastCatalogErrorCode = errorCode;
        binding->persistenceMessage = localError;
        if (errorCode == QStringLiteral("revision_conflict"))
            binding->catalogRevisionConflict = true;
        else if (errorCode == QStringLiteral("source_conflict"))
        {
            binding->sourceConflict = true;
            binding->sourceConflictMessage = localError;
        }
        else if (errorCode == QStringLiteral("output_path_conflict")
                 || errorCode == QStringLiteral("invalid_argument"))
        {
            // A rejected target does not make the currently open document unavailable.
        }
        else
        {
            binding->persistenceUnavailable = true;
            if (errorCode == QStringLiteral("catalog_locked") || errorCode == QStringLiteral("io_error"))
                this->scheduleProjectCatalogRetry();
        }
        if (error) *error = localError;
        return {};
    }
    binding->persistenceUnavailable = false;
    binding->persistenceMessage.clear();
    binding->lastCatalogErrorCode.clear();
    binding->publicationLease = lease;
    binding->publicationSourceRevision = binding->acceptedSourceRevision;
    binding->publicationLogicalPath = logicalPath;
    binding->publicationPaths = reservedPaths;
    binding->publishing = true;
    return lease;
}

bool Session::recordModelReloaded(uint modelID, const QString &path, QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Reload acceptance must be recorded on the document thread.");
        return false;
    }
    QString sourceError;
    QString sourceErrorCode;
    const auto sourceBinding = RProjectCatalog::resolveSourceBinding(path, &sourceError, &sourceErrorCode);
    if (!sourceBinding)
    {
        if (error) *error = sourceError;
        return false;
    }
    return this->recordModelReloaded(modelID, *sourceBinding, error);
}

bool Session::validateReloadSource(uint modelID,
                                   const RProjectCatalog::SourceBinding &sourceBinding,
                                   QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Reload sources must be validated on the document thread.");
        return false;
    }
    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    auto binding = this->projectDocuments.find(stamp.first);
    if (stamp.first.isEmpty() || binding == this->projectDocuments.end() || !this->projectCatalog
        || binding->persistentDocumentId.isEmpty() || !binding->instanceLease)
    {
        if (error) *error = tr("The model has no active persistent project instance.");
        return false;
    }
    if (binding->publishing)
    {
        if (error) *error = tr("A native save is being published for this document.");
        return false;
    }

    QString sourceError;
    QString sourceErrorCode;
    const auto currentBinding = RProjectCatalog::resolveSourceBinding(sourceBinding.logicalSourcePath,
                                                                        &sourceError, &sourceErrorCode);
    if (!currentBinding || *currentBinding != sourceBinding)
    {
        const bool changed = currentBinding && *currentBinding != sourceBinding;
        if (sourceErrorCode == QStringLiteral("source_conflict") || changed)
        {
            binding->sourceConflict = true;
            binding->sourceConflictPath = sourceBinding.logicalSourcePath;
            binding->sourceConflictMessage = sourceError.isEmpty()
                                                 ? tr("The native source changed after it was read.")
                                                 : sourceError;
        }
        else
        {
            binding->sourceVerificationUnavailable = true;
            binding->persistenceUnavailable = true;
            binding->lastCatalogErrorCode = sourceErrorCode;
            binding->persistenceMessage = sourceError;
        }
        if (error) *error = binding->sourceConflict ? binding->sourceConflictMessage : sourceError;
        return false;
    }

    QString documentError;
    const auto document = this->projectCatalog->document(binding->persistentDocumentId, &documentError);
    if (!document)
    {
        binding->persistenceUnavailable = true;
        binding->lastCatalogErrorCode = documentError.contains(QStringLiteral("locked"), Qt::CaseInsensitive)
                                            ? QStringLiteral("catalog_locked") : QStringLiteral("io_error");
        binding->persistenceMessage = documentError;
        if (binding->lastCatalogErrorCode == QStringLiteral("catalog_locked")) this->scheduleProjectCatalogRetry();
        if (error) *error = documentError;
        return false;
    }
    if (document->sourceRevision != binding->acceptedSourceRevision)
    {
        binding->catalogRevisionConflict = true;
        binding->persistenceMessage = tr("The project advanced from locally accepted revision %1 to %2.")
                                          .arg(binding->acceptedSourceRevision, document->sourceRevision);
        if (error) *error = binding->persistenceMessage;
        return false;
    }
    return true;
}

bool Session::recordModelReloaded(uint modelID,
                                  const RProjectCatalog::SourceBinding &sourceBinding,
                                  QString *error)
{
    if (error) error->clear();
    if (!this->validateReloadSource(modelID, sourceBinding, error)) return false;

    const QPair<QString, quint64> stamp = this->modelCommandStamp(modelID);
    auto binding = this->projectDocuments.find(stamp.first);
    if (stamp.first.isEmpty() || binding == this->projectDocuments.end())
    {
        if (error) *error = tr("The reloaded model is no longer part of this session.");
        return false;
    }
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates.remove(stamp.first);
    }

    const QString revision = nextSourceRevision(binding->acceptedSourceRevision);
    if (revision.isEmpty())
    {
        if (error) *error = tr("The project source revision reached the v1 monotonic bound.");
        return false;
    }
    QString localError;
    QString errorCode;
    bool reopenedSavedSource = false;
    if (!this->projectCatalog->acceptReload(sourceBinding, binding->persistentDocumentId,
                                            revision, &localError, binding->acceptedSourceRevision,
                                            &errorCode, &reopenedSavedSource, binding->instanceLease))
    {
        binding->lastCatalogErrorCode = errorCode;
        binding->persistenceMessage = localError;
        if (errorCode == QStringLiteral("revision_conflict")) binding->catalogRevisionConflict = true;
        else if (errorCode == QStringLiteral("source_conflict"))
        {
            binding->sourceConflict = true;
            binding->sourceConflictPath = sourceBinding.logicalSourcePath;
            binding->sourceConflictMessage = localError;
        }
        else
        {
            binding->persistenceUnavailable = true;
            if (errorCode == QStringLiteral("catalog_locked")) this->scheduleProjectCatalogRetry();
        }
        if (error) *error = localError;
        return false;
    }

    binding->acceptedSourceRevision = revision;
    binding->sourceAtLoad = sourceBinding;
    binding->sourcePath = sourceBinding.logicalSourcePath;
    binding->acceptedSavedArtifactHash = sourceBinding.savedArtifactHash;
    binding->acceptedSourceArtifacts = sourceArtifactsJson(sourceBinding.sourceArtifacts);
    binding->sourceUnverified = false;
    binding->unverifiedSourcePath.clear();
    binding->detachedFromChangedSource = false;
    binding->sourceLinkStatus.clear();
    binding->acceptedDirty = this->getModel(modelID).hasNativeReadNormalization();
    binding->reopenedSavedSource = reopenedSavedSource;
    binding->sourceConflict = false;
    binding->sourceConflictPath.clear();
    binding->sourceConflictMessage.clear();
    binding->catalogRevisionConflict = false;
    binding->persistenceUnavailable = false;
    binding->sourceVerificationUnavailable = false;
    binding->persistenceMessage.clear();
    binding->lastCatalogErrorCode.clear();
    this->getModel(modelID).setLoadedSourceBinding(sourceBinding);

    // Reload accepts source bytes, but unresolved editor drafts remain pending.
    // Persist only their scope and field names; raw input stays in the GUI.
    const QJsonObject draftMetadata = this->projectDraftMetadata(*binding);
    binding->catalogAcceptedRuntimeRevision = stamp.second;
    binding->draftMetadataPending = false;
    if (draftMetadata.value(QStringLiteral("status")).toString() != QStringLiteral("unknown")
        || binding->acceptedDirty)
    {
        QString draftError;
        QString draftErrorCode;
        if (!this->projectCatalog->recordAccepted(binding->persistentDocumentId, revision, binding->acceptedDirty,
                                                  draftMetadata, &draftError, revision,
                                                  &draftErrorCode, binding->instanceLease))
        {
            binding->draftMetadataPending = true;
            binding->persistenceUnavailable = true;
            binding->lastCatalogErrorCode = draftErrorCode;
            binding->persistenceMessage = draftError;
            {
                QMutexLocker guard(&this->pendingProjectUpdatesMutex);
                this->pendingProjectUpdates[stamp.first] = qMax(this->pendingProjectUpdates.value(stamp.first),
                                                                 stamp.second);
            }
            if (draftErrorCode != QStringLiteral("revision_conflict")
                && draftErrorCode != QStringLiteral("source_conflict"))
                this->scheduleProjectCatalogRetry();
        }
    }
    const auto updatedDocument = this->projectCatalog->document(binding->persistentDocumentId);
    if (updatedDocument && updatedDocument->sourceRevision == binding->acceptedSourceRevision)
    {
        binding->acceptedModelSha256 = updatedDocument->modelSha256;
        binding->originDocumentId = updatedDocument->originDocumentId;
        binding->orphanSnapshots = updatedDocument->orphanSnapshots;
    }
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        if (!binding->draftMetadataPending) this->pendingProjectUpdates.remove(stamp.first);
    }
    return true;
}
QJsonObject Session::createDocumentSnapshot(uint modelID, QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Document snapshots must be captured on the document thread.");
        return {};
    }
    this->flushProjectCatalogUpdates();
    const QPair<QString, quint64> before = this->modelCommandStamp(modelID);
    auto binding = this->projectDocuments.find(before.first);
    if (before.first.isEmpty() || binding == this->projectDocuments.end() || !this->projectCatalog)
    {
        if (error) *error = tr("The model has no persistent project identity.");
        return {};
    }
    this->refreshProjectSourceConflict(before.first);
    if (binding->persistentDocumentId.isEmpty())
    {
        if (error) *error = binding->persistenceMessage.isEmpty()
                                ? tr("The model has no persistent project identity.")
                                : binding->persistenceMessage;
        return {};
    }
    if (binding->publishing || binding->sourceConflict || binding->catalogRevisionConflict
        || binding->persistenceUnavailable)
    {
        if (error) *error = binding->publishing ? tr("A native save is being published for this document.")
                         : binding->sourceConflict ? binding->sourceConflictMessage
                                : binding->persistenceMessage.isEmpty()
                                      ? tr("The project document is not synchronized with this runtime state.")
                                      : binding->persistenceMessage;
        return {};
    }
    bool pendingMetadata = false;
    {
        QMutexLocker guard(&this->pendingProjectUpdatesMutex);
        pendingMetadata = this->pendingProjectUpdates.contains(before.first);
    }
    if (this->hasDocumentDraft(modelID) || binding->draftMetadataPending || pendingMetadata
        || binding->catalogAcceptedRuntimeRevision < before.second)
    {
        if (error) *error = this->hasDocumentDraft(modelID)
                                ? tr("Resolve the pending numeric draft before snapshotting this project document.")
                                : tr("Wait for the current document revision to synchronize before snapshotting.");
        return {};
    }
    QString documentError;
    const auto document = this->projectCatalog->document(binding->persistentDocumentId, &documentError);
    if (!document || document->tombstone)
    {
        binding->persistenceUnavailable = true;
        binding->persistenceMessage = documentError;
        if (error) *error = documentError.isEmpty() ? tr("The persistent project document is unavailable.")
                                                    : documentError;
        return {};
    }
    if (document->sourceRevision != binding->acceptedSourceRevision)
    {
        binding->catalogRevisionConflict = true;
        binding->persistenceMessage = tr("The project advanced from locally accepted revision %1 to %2.")
                                          .arg(binding->acceptedSourceRevision, document->sourceRevision);
        if (error) *error = binding->persistenceMessage;
        return {};
    }

    if (before.second > 9007199254740991ULL)
    {
        if (error) *error = tr("The runtime document revision exceeds the project reference bound.");
        return {};
    }
    if (!this->mutex.tryLock())
    {
        if (error) *error = tr("The model is busy and cannot be snapshotted now.");
        return {};
    }
    auto unlock = qScopeGuard([this] { this->unlock(); });
    if (this->modelCommandStamp(modelID) != before)
    {
        if (error) *error = tr("The model changed before snapshot capture.");
        return {};
    }
    RModel detachedModel(*this->getModelPtr(modelID));
    const QPair<QString, quint64> after = this->modelCommandStamp(modelID);
    if (after != before)
    {
        if (error) *error = tr("The model changed during snapshot capture.");
        return {};
    }
    this->unlock();
    unlock.dismiss();

    RProjectCatalog::InstanceRef instanceRef;
    instanceRef.documentId = after.first;
    instanceRef.sessionEpoch = this->commandHost->sessionEpoch();
    instanceRef.documentRevision = after.second;
    RProjectCatalog::SnapshotArtifact artifact;
    QString localError;
    QString errorCode;
    if (!this->projectCatalog->captureSnapshot(binding->persistentDocumentId, detachedModel,
                                                instanceRef, &artifact, &localError,
                                                binding->acceptedSourceRevision, &errorCode,
                                                binding->instanceLease))
    {
        binding->lastCatalogErrorCode = errorCode;
        binding->persistenceMessage = localError;
        if (errorCode == QStringLiteral("revision_conflict"))
            binding->catalogRevisionConflict = true;
        else if (errorCode == QStringLiteral("source_conflict"))
        {
            binding->sourceConflict = true;
            binding->sourceConflictPath = binding->sourcePath;
            binding->sourceConflictMessage = localError;
        }
        else
        {
            binding->persistenceUnavailable = true;
            if (errorCode == QStringLiteral("catalog_locked")) this->scheduleProjectCatalogRetry();
        }
        if (error) *error = localError;
        return {};
    }
    binding->acceptedModelSha256 = artifact.modelSha256;
    if (this->modelCommandStamp(modelID) != after)
    {
        this->flushProjectCatalogUpdates();
        if (error) *error = tr("The model changed while its snapshot was being published; retry the snapshot.");
        return {};
    }
    return artifact.toJson();
}

bool Session::acceptSolverResult(const QPair<QString, quint64> &sourceStamp,
                                const Model &result, const QString &artifactPath,
                                QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("Solver results must be attached on the document thread.");
        return false;
    }
    const uint id = findModelByCommandIdentity(sourceStamp.first);
    if (id == RConstants::eod || modelCommandStamp(id) != sourceStamp || ModelCommandHost::isMutationBlocked())
    {
        emit solverResultAvailable(sourceStamp.first, sourceStamp.second, artifactPath, false);
        return false;
    }
    // Establish the pre-result audit snapshot even for documents that have not
    // previously been inspected through the command interface.
    if (commandHost->query(id).value("modelId").toString().isEmpty())
    {
        if (error) *error = tr("Could not prepare the document audit; the solver artifact has been retained.");
        emit solverResultAvailable(sourceStamp.first, sourceStamp.second, artifactPath, false);
        return false;
    }
    if (!mutex.tryLock())
    {
        emit solverResultAvailable(sourceStamp.first, sourceStamp.second, artifactPath, false);
        return false;
    }
    if (findModelByCommandIdentity(sourceStamp.first) != id || modelCommandStamp(id) != sourceStamp)
    {
        mutex.unlock();
        emit solverResultAvailable(sourceStamp.first, sourceStamp.second, artifactPath, false);
        return false;
    }
    // Prepare before changing the live model/history. update() retains display
    // properties; the task's private path must not replace the user's save path.
    try
    {
        QList<Model> nextModels(models);
        nextModels.detach();
        for (qsizetype i = 0; i < nextModels.size(); ++i)
            nextModels[i].copyHistoryFrom(models.at(i));
        Model &candidate = nextModels[int(id)];
        const QString sourcePath = candidate.getFileName();
        candidate.storeCurentVersion(Application::instance()->getApplicationSettings()->getNHistoryRecords(),
                                    tr("Accept solver result"));
        candidate.update(result);
        candidate.setFileName(sourcePath);
        models.swap(nextModels);
    }
    catch (...)
    {
        mutex.unlock();
        if (error) *error = tr("Could not attach the solver result; its artifact has been retained.");
        emit solverResultAvailable(sourceStamp.first, sourceStamp.second, artifactPath, false);
        return false;
    }
    mutex.unlock();
    QString auditError;
    const bool recorded = commandHost->recordExternalChange(id, "external_edit", &auditError);
    setModelChanged(id);
    emit solverResultAvailable(sourceStamp.first, sourceStamp.second, artifactPath, true);
    if (!recorded && error)
        *error = tr("The solver result was attached, but its audit record failed: %1").arg(auditError);
    return true;
}

const QString &Session::getID() const
{
    return this->sessionID;
}

void Session::lock()
{
    this->mutex.lock();
}

bool Session::trylock(int timeout)
{
    return this->mutex.tryLock(timeout);
}

void Session::unlock()
{
    this->mutex.unlock();
}

DrawEngine *Session::getDrawEngine()
{
    return this->drawEngine;
}

QMutex *Session::getMutex()
{
    return &this->mutex;
}

const QString &Session::getFileName() const
{
    return this->fileName;
}

uint Session::getNModels() const
{
    return uint(this->models.size());
}

const Model & Session::getModel(uint position) const
{
    return this->models[int(position)];
}

Model & Session::getModel(uint position)
{
    return this->models[int(position)];
}

const Model * Session::getModelPtr(uint position) const
{
    return &this->models[int(position)];
}

Model * Session::getModelPtr(uint position)
{
    return &this->models[int(position)];
}

void Session::addModel(const Model &model)
{
    this->addModelInternal(model, model.getLoadedSourceBinding());
}

void Session::addModel(const Model &model,
                       const RProjectCatalog::SourceBinding &sourceBinding)
{
    this->addModelInternal(model, sourceBinding);
}

void Session::addModelInternal(const Model &model,
                               const std::optional<RProjectCatalog::SourceBinding> &sourceBinding)
{
    R_LOG_TRACE_IN;
    auto detachedCopy = [&model] {
        auto copy = std::make_shared<Model>(model);
        copy->copyHistoryFrom(model);
        return copy;
    };
    if (QThread::currentThread() != thread())
    {
        const auto copy = detachedCopy();
        QMetaObject::invokeMethod(this, [this, copy, sourceBinding] {
            this->addModelInternal(*copy, sourceBinding);
        }, Qt::QueuedConnection);
        R_LOG_TRACE_OUT;
        return;
    }
    if (!this->trylock())
    {
        const auto copy = detachedCopy();
        QTimer::singleShot(10, this, [this, copy, sourceBinding] {
            this->addModelInternal(*copy, sourceBinding);
        });
        R_LOG_TRACE_OUT;
        return;
    }
    auto unlockOnFailure = qScopeGuard([this] { this->unlock(); });
    // Model snapshots omit history. QList growth may invoke those copy
    // constructors, so explicitly preserve each document's history.
    QList<Model> nextModels;
    nextModels.reserve(this->models.size() + 1);
    for (const Model &existing : std::as_const(this->models))
    {
        nextModels.append(existing);
        nextModels.last().copyHistoryFrom(existing);
    }
    nextModels.append(model);
    nextModels.last().copyHistoryFrom(model);
    const auto &meshBefore = model.getProblemSetup().getMeshSetup();
    const double minimumEdgeBefore = meshBefore.getMinEdgeLength();
    const double maximumEdgeBefore = meshBefore.getMaxEdgeLength();
    nextModels.last().initializeMeshInput();
    const auto &meshAfter = nextModels.last().getProblemSetup().getMeshSetup();
    const bool nativeMeshDefaultsChanged = meshAfter.getMinEdgeLength() != minimumEdgeBefore
                                          || meshAfter.getMaxEdgeLength() != maximumEdgeBefore;
    QString runtimeIdentity;
    {
        QMutexLocker stampGuard(&this->commandStampMutex);
        auto nextStamps = this->commandStamps;
        runtimeIdentity = QUuid::createUuid().toString(QUuid::WithoutBraces);
        nextStamps.append({runtimeIdentity, 0});
        this->models.swap(nextModels);
        this->commandStamps.swap(nextStamps);
    }
    this->unlock();
    unlockOnFailure.dismiss();
    const uint modelID = this->findModelByCommandIdentity(runtimeIdentity);
    if (modelID == RConstants::eod)
    {
        R_LOG_TRACE_OUT;
        return;
    }
    const QString unverifiedSourcePath = sourceBinding ? QString() : model.getFileName();
    this->attachProjectDocument(modelID, runtimeIdentity, sourceBinding, unverifiedSourcePath);
    // Mesh input caches may also fill native edge-length defaults. Preserve
    // that legacy normalization, but do not call its changed accepted values
    // a clean copy of the saved source.
    if (sourceBinding && (nativeMeshDefaultsChanged || model.hasNativeReadNormalization()))
        this->invalidateModelCommands(modelID);
    emit this->modelAdded(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setModel(uint position, const Model &model)
{
    R_LOG_TRACE_IN;
    bool locked = this->trylock();
    this->models[int(position)] = model;
    if (locked)
    {
        this->unlock();
    }
    this->setModelChanged(position);
    R_LOG_TRACE_OUT;
}

void Session::removeModel(uint position)
{
    R_LOG_TRACE_IN;
    const QPair<QString, quint64> removedStamp = this->modelCommandStamp(position);
    bool locked = this->trylock();
    auto unlockOnFailure = qScopeGuard([this, locked] { if (locked) this->unlock(); });
    // Compaction by Model assignment would keep the removed slot's history.
    // Prepare survivors with their own histories before publishing the list.
    QList<Model> nextModels;
    nextModels.reserve(this->models.size() - 1);
    for (qsizetype i = 0; i < this->models.size(); ++i)
    {
        if (i == qsizetype(position)) continue;
        nextModels.append(this->models.at(i));
        nextModels.last().copyHistoryFrom(this->models.at(i));
    }
    {
        QMutexLocker stampGuard(&this->commandStampMutex);
        auto nextStamps = this->commandStamps;
        nextStamps.removeAt(int(position));
        this->models.swap(nextModels);
        this->commandStamps.swap(nextStamps);
    }
    this->pickList.removeModelItemsAndReindex(position);
    this->projectDocuments.remove(removedStamp.first);
    {
        QMutexLocker updateGuard(&this->pendingProjectUpdatesMutex);
        this->pendingProjectUpdates.remove(removedStamp.first);
    }
    emit this->runtimeDocumentRemoved(removedStamp.first);
    if (locked)
    {
        this->unlock();
    }
    unlockOnFailure.dismiss();
    emit this->modelRemoved(position);
    R_LOG_TRACE_OUT;
}

void Session::setModelSelected(uint modelID, bool selected)
{
    R_LOG_TRACE_IN;
    for (uint i=0;i<this->getModel(modelID).getNPoints();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_POINT,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNLines();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_LINE,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNSurfaces();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_SURFACE,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNVolumes();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_VOLUME,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNVectorFields();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_VECTOR_FIELD,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNScalarFields();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_SCALAR_FIELD,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNStreamLines();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_STREAM_LINE,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNCuts();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_CUT,i,selected,false);
    }
    for (uint i=0;i<this->getModel(modelID).getNIsos();i++)
    {
        this->setEntitySelected(modelID,R_ENTITY_GROUP_ISO,i,selected,false);
    }
    this->getModel(modelID).setSelected(selected);
    this->setModelSelectionChanged(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setEntitySelected(uint modelID, REntityGroupType elementGrpType, uint entityID, bool selected, bool notifyModel)
{
    R_LOG_TRACE_IN;
    if (this->getModel(modelID).getSelected(elementGrpType,entityID) != selected)
    {
        this->getModel(modelID).setSelected(elementGrpType,entityID,selected);
        emit this->entitySelectionChanged(modelID,elementGrpType,entityID,selected);
        if (notifyModel)
        {
            this->setModelSelectionChanged(modelID);
        }
    }
    R_LOG_TRACE_OUT;
}

void Session::setEntityVisible(uint modelID, REntityGroupType elementGrpType, uint entityID, bool visible)
{
    R_LOG_TRACE_IN;
    if (this->getModel(modelID).getVisible(elementGrpType,entityID) != visible)
    {
        this->getModel(modelID).setVisible(elementGrpType,entityID,visible);
        emit this->entityVisibilityChanged(modelID,elementGrpType,entityID,visible);
        emit this->modelVisibilityChanged(modelID);
    }
    R_LOG_TRACE_OUT;
}

void Session::setModelVisible(uint modelID, bool visible)
{
    R_LOG_TRACE_IN;
    for (uint i=0;i<this->getModel(modelID).getNPoints();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_POINT,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNLines();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_LINE,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNSurfaces();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_SURFACE,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNVolumes();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_VOLUME,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNVectorFields();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_VECTOR_FIELD,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNScalarFields();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_SCALAR_FIELD,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNStreamLines();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_STREAM_LINE,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNCuts();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_CUT,i,visible);
    }
    for (uint i=0;i<this->getModel(modelID).getNIsos();i++)
    {
        this->setEntityVisible(modelID,R_ENTITY_GROUP_ISO,i,visible);
    }
    R_LOG_TRACE_OUT;
}

QList<uint> Session::getSelectedModelIDs() const
{
//    R_LOG_TRACE_IN;
    QList<uint> selectedModelIDs;

    for (uint i=0;i<this->getNModels();i++)
    {
        // Check if model is partialy selected (NOT completely unselected)
        if (!this->getModel(i).isSelected(false))
        {
            selectedModelIDs.push_back(i);
        }
    }

//    R_LOG_TRACE_OUT;
    return selectedModelIDs;
}

QList<uint> Session::getVisibleModelIDs() const
{
    R_LOG_TRACE_IN;
    QList<uint> visibleModelIDs;

    for (uint i=0;i<this->getNModels();i++)
    {
        // Check if model is partialy visible (NOT completely unselected)
        if (!this->getModel(i).isVisible(false))
        {
            visibleModelIDs.push_back(i);
        }
    }

    R_LOG_TRACE_OUT;
    return visibleModelIDs;
}

bool Session::isModelSelected(uint modelID)
{
//    R_LOG_TRACE_IN;
    return (!this->getModel(modelID).isSelected(false));
}

QList<SessionEntityID> Session::getAllEntityIDs() const
{
    R_LOG_TRACE_IN;
    QList<SessionEntityID> allEntityIDs;

    for (uint i=0;i<this->getNModels();i++)
    {
        allEntityIDs += this->getAllEntityIDs(i);
    }
    R_LOG_TRACE_OUT;
    return allEntityIDs;
}

QList<SessionEntityID> Session::getAllEntityIDs(uint modelID) const
{
    R_LOG_TRACE_IN;
    QList<SessionEntityID> allEntityIDs;
    SessionEntityID entityID;

    entityID.setMid(modelID);

    entityID.setType(R_ENTITY_GROUP_POINT);
    for (uint j=0;j<this->getModel(modelID).getNPoints();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_LINE);
    for (uint j=0;j<this->getModel(modelID).getNLines();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_SURFACE);
    for (uint j=0;j<this->getModel(modelID).getNSurfaces();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_VOLUME);
    for (uint j=0;j<this->getModel(modelID).getNVolumes();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_VECTOR_FIELD);
    for (uint j=0;j<this->getModel(modelID).getNVectorFields();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_SCALAR_FIELD);
    for (uint j=0;j<this->getModel(modelID).getNScalarFields();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_STREAM_LINE);
    for (uint j=0;j<this->getModel(modelID).getNStreamLines();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_CUT);
    for (uint j=0;j<this->getModel(modelID).getNCuts();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    entityID.setType(R_ENTITY_GROUP_ISO);
    for (uint j=0;j<this->getModel(modelID).getNIsos();j++)
    {
        entityID.setEid(j);
        allEntityIDs.push_back(entityID);
    }

    R_LOG_TRACE_OUT;
    return allEntityIDs;
}

QList<SessionEntityID> Session::getSelectedEntityIDs() const
{
    R_LOG_TRACE_IN;
    QList<SessionEntityID> selectedEntityIDs;

    for (uint i=0;i<this->getNModels();i++)
    {
        selectedEntityIDs += this->getModel(i).getSelectedEntityIDs(i);
    }
    R_LOG_TRACE_OUT;
    return selectedEntityIDs;
}

QList<SessionEntityID> Session::getVisibleEntityIDs() const
{
    R_LOG_TRACE_IN;
    QList<SessionEntityID> visibleEntityIDs;

    for (uint i=0;i<this->getNModels();i++)
    {
        visibleEntityIDs += this->getModel(i).getVisibleEntityIDs(i);
    }
    R_LOG_TRACE_OUT;
    return visibleEntityIDs;
}

QList<SessionEntityID> Session::filterSelectedEntityIDs(const QList<SessionEntityID> selectedEntityIDs, uint modelID, REntityGroupTypeMask entityTypeMask)
{
    R_LOG_TRACE_IN;
    QList<SessionEntityID> selectedEntities(selectedEntityIDs);

    for (int i=selectedEntities.size()-1;i>=0;i--)
    {
        if (selectedEntities[i].getMid() == modelID
            &&
            selectedEntities[i].getType() & entityTypeMask
            &&
            R_ENTITY_GROUP_TYPE_IS_ELEMENT(selectedEntities[i].getType()))
        {
            continue;
        }
        selectedEntities.erase(selectedEntities.begin()+i);
    }

    R_LOG_TRACE_OUT;
    return selectedEntities;
}

uint Session::findModelByName(const QString &modelName)
{
    R_LOG_TRACE_IN;
    for (uint i=0;i<this->getNModels();i++)
    {
        if (this->getModel(i).getName() == modelName)
        {
            R_LOG_TRACE_OUT;
            return i;
        }
    }
    R_LOG_TRACE_OUT;
    return RConstants::eod;
}

uint Session::findModelByPtr(const Model *pModel)
{
    R_LOG_TRACE_IN;
    for (uint i=0;i<this->getNModels();i++)
    {
        if (this->getModelPtr(i) == pModel)
        {
            R_LOG_TRACE_OUT;
            return i;
        }
    }
    R_LOG_TRACE_OUT;
    return RConstants::eod;
}

void Session::setModelSelectionChanged(uint modelID)
{
    R_LOG_TRACE_IN;
    emit this->modelSelectionChanged(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setDisplayPropertiesChanged(uint modelID, REntityGroupType elementGrpType, uint entityID)
{
    R_LOG_TRACE_IN;
    emit this->displayPropertiesChanged(modelID,elementGrpType,entityID);
    R_LOG_TRACE_OUT;
}

void Session::setModelChanged(uint modelID)
{
    R_LOG_TRACE_IN;
    emit this->modelChanged(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setModelRenamed(uint modelID)
{
    R_LOG_TRACE_IN;
    emit this->modelRenamed(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setProblemChanged(uint modelID)
{
    R_LOG_TRACE_IN;
    emit this->problemChanged(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setProblemSelectionChanged(uint modelID)
{
    R_LOG_TRACE_IN;
    emit this->problemSelectionChanged(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setResultsChanged(uint modelID)
{
    R_LOG_TRACE_IN;
    emit this->resultsChanged(modelID);
    R_LOG_TRACE_OUT;
}

void Session::setBoundaryConditionChanged(uint modelID, REntityGroupType elementGrpType, uint entityID)
{
    R_LOG_TRACE_IN;
    emit this->boundaryConditionChanged(modelID,elementGrpType,entityID);
    R_LOG_TRACE_OUT;
}

void Session::setInitialConditionChanged(uint modelID, REntityGroupType elementGrpType, uint entityID)
{
    R_LOG_TRACE_IN;
    emit this->initialConditionChanged(modelID,elementGrpType,entityID);
    R_LOG_TRACE_OUT;
}

void Session::setEnvironmentConditionChanged(uint modelID, REntityGroupType elementGrpType, uint entityID)
{
    R_LOG_TRACE_IN;
    emit this->environmentConditionChanged(modelID,elementGrpType,entityID);
    R_LOG_TRACE_OUT;
}

void Session::setMaterialChanged(uint modelID, REntityGroupType elementGrpType, uint entityID)
{
    R_LOG_TRACE_IN;
    emit this->materialChanged(modelID,elementGrpType,entityID);
    R_LOG_TRACE_OUT;
}

void Session::setVariableDataChanged(uint modelID, RVariableType variableType)
{
    R_LOG_TRACE_IN;
    emit this->variableDataChanged(modelID,variableType);
    R_LOG_TRACE_OUT;
}

void Session::setVariableDataChanged(const SessionEntityID &entityID, RVariableType variableType)
{
    R_LOG_TRACE_IN;
    emit this->variableDataChanged(entityID,variableType);
    R_LOG_TRACE_OUT;
}

const PickList &Session::getPickList() const
{
    R_LOG_TRACE;
    return this->pickList;
}

PickList &Session::getPickList()
{
    R_LOG_TRACE;
    return this->pickList;
}

void Session::setBeginDrawStreamLinePosition(const RR3Vector &position)
{
    R_LOG_TRACE_IN;
    emit this->beginDrawStreamLinePosition(position);
    R_LOG_TRACE_OUT;
}

void Session::setEndDrawStreamLinePosition()
{
    R_LOG_TRACE_IN;
    emit this->endDrawStreamLinePosition();
    R_LOG_TRACE_OUT;
}

void Session::setBeginDrawScaleOrigin(const RR3Vector &position)
{
    R_LOG_TRACE_IN;
    emit this->beginDrawScaleOrigin(position);
    R_LOG_TRACE_OUT;
}

void Session::setEndDrawScaleOrigin()
{
    R_LOG_TRACE_IN;
    emit this->endDrawScaleOrigin();
    R_LOG_TRACE_OUT;
}

void Session::setBeginDrawRotationOrigin(const RR3Vector &position)
{
    R_LOG_TRACE_IN;
    emit this->beginDrawRotationOrigin(position);
    R_LOG_TRACE_OUT;
}

void Session::setEndDrawRotationOrigin()
{
    R_LOG_TRACE_IN;
    emit this->endDrawRotationOrigin();
    R_LOG_TRACE_OUT;
}

void Session::setBeginDrawLocalDirections(const QList<RLocalDirection> &localDirections)
{
    R_LOG_TRACE_IN;
    emit this->beginDrawLocalDirections(localDirections);
    R_LOG_TRACE_OUT;
}

void Session::setEndDrawLocalDirections()
{
    R_LOG_TRACE_IN;
    emit this->endDrawLocalDirections();
    R_LOG_TRACE_OUT;
}

void Session::setBeginDrawCutPlane(const RPlane &plane)
{
    R_LOG_TRACE_IN;
    emit this->beginDrawCutPlane(plane);
    R_LOG_TRACE_OUT;
}

void Session::setEndDrawCutPlane()
{
    R_LOG_TRACE_IN;
    emit this->endDrawCutPlane();
    R_LOG_TRACE_OUT;
}

void Session::setBeginDrawMoveNodes(const QMap<SessionNodeID, RR3Vector> &nodesToMove)
{
    R_LOG_TRACE_IN;
    emit this->beginDrawMoveNodes(nodesToMove);
    R_LOG_TRACE_OUT;
}

void Session::setEndDrawMoveNodes()
{
    R_LOG_TRACE_IN;
    emit this->endDrawMoveNodes();
    R_LOG_TRACE_OUT;
}

void Session::setTakeScreenShot(uint modelID, const QString &screenShotFileName, bool blocking)
{
    R_LOG_TRACE_IN;
    if (blocking)
    {
        emit this->takeScreenShotBlocking(modelID, screenShotFileName);
    }
    else
    {
        emit this->takeScreenShot(modelID, screenShotFileName);
    }
    R_LOG_TRACE_OUT;
}

void Session::readModels(const QStringList &fileNames)
{
    R_LOG_TRACE_IN;
    RLogger::info("Loading %u files\n",fileNames.size());
    RLogger::indent();
    for (int i=0;i<fileNames.size();i++)
    {
        RLogger::info("Loading %u: \'%s\'\n",i+1,fileNames.at(i).toUtf8().constData());

        if (!fileNames.at(i).isEmpty())
        {
            // Read model.
            ModelIO *modelIO = new ModelIO(MODEL_IO_OPEN, fileNames.at(i));
            modelIO->setAutoDelete(true);

            RJobManager::getInstance().submit(modelIO);
        }
    }
    RLogger::unindent();
    R_LOG_TRACE_OUT;
}

void Session::read(const QString &fileName)
{
    R_LOG_TRACE_IN;
    if (fileName.isEmpty())
    {
        R_LOG_TRACE_OUT;
        throw RError(RError::Type::InvalidFileName,R_ERROR_REF,"No file name was provided.");
    }

    RLogger::info("Reading session file \'%s\'.\n",fileName.toUtf8().constData());
    RLogger::indent();

    this->fileName = fileName;

    RFile sessionFile(this->fileName,RFile::ASCII);

    if (!sessionFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        RLogger::unindent();
        R_LOG_TRACE_OUT;
        throw RError(RError::Type::OpenFile,R_ERROR_REF,"Failed to open the file \'%s\'.",this->fileName.toUtf8().constData());
    }

    RLogger::info("Reading session file records.\n");
    RLogger::indent();

    uint nModels = 0;
    RFileIO::readAscii(sessionFile,nModels);
    RLogger::info("Detected %u file records.\n",nModels);
    QStringList modelFileNames;
    for (uint i=0;i<nModels;i++)
    {
        QString modelFileName;
        RFileIO::readAscii(sessionFile,modelFileName);

        if (modelFileName.isEmpty())
        {
            RLogger::info("File record %u is empty.\n",i+1);
        }
        else
        {
            RLogger::info("File record %u \'%s\'\n",i+1,modelFileName.toUtf8().constData());
            modelFileNames.append(modelFileName);
        }
    }
    RLogger::unindent();

    sessionFile.close();

    this->readModels(modelFileNames);

    RLogger::unindent();
    R_LOG_TRACE_OUT;
}

void Session::write(const QString &fileName, bool writeModels)
{
    R_LOG_TRACE_IN;
    RLogger::info("Writing session file \'%s\'.\n",fileName.toUtf8().constData());

    if (fileName.isEmpty())
    {
        R_LOG_TRACE_OUT;
        throw RError(RError::Type::InvalidFileName,R_ERROR_REF,"No file name was provided.");
    }

    this->fileName = fileName;

    RSaveFile sessionFile(this->fileName,RSaveFile::ASCII);

    if (!sessionFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        R_LOG_TRACE_OUT;
        throw RError(RError::Type::OpenFile,R_ERROR_REF,"Failed to open the file \'%s\'.",this->fileName.toUtf8().constData());
    }

    RFileIO::writeAscii(sessionFile,this->getNModels(),true);
    for (uint i=0;i<this->getNModels();i++)
    {
        RFileIO::writeAscii(sessionFile,"\"" + this->getModel(i).getFileName() + "\"",true);

        if (writeModels)
        {
            if (this->getModel(i).getFileName().isEmpty())
            {
                QDir dataDir(Application::instance()->getApplicationSettings()->getDataDir());
                QString binaryExtension = RModel::getDefaultFileExtension(true);
                QString modelName = this->getModel(i).getName();
                if (modelName.length() == 0)
                {
                    modelName = "Model_" + QString::number(i+1);
                }
                QString modelFileName = dataDir.filePath(modelName + "." + binaryExtension);
                this->getModel(i).setFileName(modelFileName);
            }

            // Write model.
            ModelIO *modelIO = new ModelIO(MODEL_IO_SAVE, this->getModel(i).getFileName(), this->getModelPtr(i));
            modelIO->setAutoDelete(true);

            RJobManager::getInstance().submit(modelIO);
        }
    }

    sessionFile.commit();
    R_LOG_TRACE_OUT;
}

void Session::clear()
{
    R_LOG_TRACE_IN;
    RLogger::info("Closing session\n");
    this->fileName.clear();

    while (this->getNModels() > 0)
    {
        RLogger::indent();
        RLogger::info("Closing model \'%s\'\n",this->getModel(this->getNModels()-1).getName().toUtf8().constData());

        this->removeModel(this->getNModels()-1);

        RLogger::unindent();
    }
    R_LOG_TRACE_OUT;
}

void Session::storeCurentModelVersion(uint modelID, const QString &message)
{
    R_LOG_TRACE_IN;
    this->getModel(modelID).storeCurentVersion(Application::instance()->getApplicationSettings()->getNHistoryRecords(),message);
    R_LOG_TRACE_OUT;
}

bool Session::isUndoAvailable() const
{
    R_LOG_TRACE_IN;
    QList<uint> selectedModelIDs = this->getSelectedModelIDs();
    for (int i=0;i<selectedModelIDs.size();i++)
    {
        if (this->getModel(selectedModelIDs[i]).getUndoStackSize() > 0)
        {
            R_LOG_TRACE_OUT;
            return true;
        }
    }
    R_LOG_TRACE_OUT;
    return false;
}

bool Session::isRedoAvailable() const
{
    R_LOG_TRACE_IN;
    QList<uint> selectedModelIDs = this->getSelectedModelIDs();
    for (int i=0;i<selectedModelIDs.size();i++)
    {
        if (this->getModel(selectedModelIDs[i]).getRedoStackSize() > 0)
        {
            R_LOG_TRACE_OUT;
            return true;
        }
    }
    R_LOG_TRACE_OUT;
    return false;
}

QString Session::getUndoTooltip() const
{
    R_LOG_TRACE_IN;
    QString undoMessage;

    QList<uint> selectedModelIDs = this->getSelectedModelIDs();
    for (int i=0;i<selectedModelIDs.size();i++)
    {
        if (!undoMessage.isEmpty())
        {
            undoMessage += "; ";
        }
        undoMessage += this->getModel(selectedModelIDs[i]).getUndoActionMessage();
    }
    undoMessage = tr("Undo") + (undoMessage.isEmpty() ? QString() : ": " + undoMessage);
    R_LOG_TRACE_OUT;
    return undoMessage;
}

QString Session::getRedoTooltip() const
{
    R_LOG_TRACE_IN;
    QString redoMessage;

    QList<uint> selectedModelIDs = this->getSelectedModelIDs();
    for (int i=0;i<selectedModelIDs.size();i++)
    {
        if (!redoMessage.isEmpty())
        {
            redoMessage += "; ";
        }
        redoMessage += this->getModel(selectedModelIDs[i]).getRedoActionMessage();
    }
    redoMessage = tr("Redo") + (redoMessage.isEmpty() ? QString() : ": " + redoMessage);
    R_LOG_TRACE_OUT;
    return redoMessage;
}

QString Session::getDefaultFileExtension()
{
    R_LOG_TRACE;
    return "ras";
}

QString Session::getDefaultFileName()
{
    R_LOG_TRACE_IN;
    QDir sesionDir = Application::instance()->getApplicationSettings()->getSessionDir();
    R_LOG_TRACE_OUT;
    return sesionDir.filePath(QString("default.") + Session::getDefaultFileExtension());
}

bool Session::selectedModelsHasEntities(REntityGroupTypeMask entityTypeMask)
{
//    R_LOG_TRACE_IN;
    foreach (uint modelID, Application::instance()->getSession()->getSelectedModelIDs())
    {
        if (((entityTypeMask & R_ENTITY_GROUP_POINT) && Application::instance()->getSession()->getModel(modelID).getNPoints())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_LINE) && Application::instance()->getSession()->getModel(modelID).getNLines())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_SURFACE) && Application::instance()->getSession()->getModel(modelID).getNSurfaces())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_VOLUME) && Application::instance()->getSession()->getModel(modelID).getNVolumes())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_CUT) && Application::instance()->getSession()->getModel(modelID).getNCuts())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_ISO) && Application::instance()->getSession()->getModel(modelID).getNIsos())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_STREAM_LINE) && Application::instance()->getSession()->getModel(modelID).getNStreamLines())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_SCALAR_FIELD) && Application::instance()->getSession()->getModel(modelID).getNScalarFields())
            ||
            ((entityTypeMask & R_ENTITY_GROUP_VECTOR_FIELD) && Application::instance()->getSession()->getModel(modelID).getNVectorFields()))
        {
//            R_LOG_TRACE_OUT;
            return true;
        }
    }
//    R_LOG_TRACE_OUT;
    return false;
}

bool Session::selectedModelsHasEntitySelected(REntityGroupType entityGroupType)
{
//    R_LOG_TRACE_IN;
    foreach (uint modelID, Application::instance()->getSession()->getSelectedModelIDs())
    {
        foreach (SessionEntityID sessionID, Application::instance()->getSession()->getModel(modelID).getSelectedEntityIDs(modelID))
        {
            if (sessionID.getType() == entityGroupType)
            {
//                R_LOG_TRACE_OUT;
                return true;
            }
        }
    }
//    R_LOG_TRACE_OUT;
    return false;
}

void Session::onPickListChanged()
{
    R_LOG_TRACE_IN;
    emit this->pickListChanged();
    R_LOG_TRACE_OUT;
}

void Session::onModelAdded(uint modelID)
{
    R_LOG_TRACE_IN;
    this->getModel(modelID).createDependentEntities();
    R_LOG_TRACE_OUT;
}

void Session::onModelChanged(uint modelID)
{
    R_LOG_TRACE_IN;
    this->getModel(modelID).createDependentEntities();
    R_LOG_TRACE_OUT;
}

void Session::onNHistoryRecordsChanged(uint nHistoryRecords)
{
    R_LOG_TRACE_IN;
    for (uint i=0;i<this->getNModels();i++)
    {
        this->getModel(i).updateHistoryStackSize(nHistoryRecords);
    }
    emit this->nHistoryRecordsChanged(nHistoryRecords);
    R_LOG_TRACE_OUT;
}

void Session::onSoftwareUpdateFileSaved(uint downloadID, const QString &fileName)
{
    R_LOG_TRACE_IN;
    RLogger::info("Opening downloaded file \'%s\' (id=\'%u\')\n",fileName.toUtf8().constData(),downloadID);

    if (!QDesktopServices::openUrl(QUrl("file:///" + fileName)))
    {
        RLogger::error("Failed to open the file \'%s\'\n",fileName.toUtf8().constData());
    }
    R_LOG_TRACE_OUT;
}
