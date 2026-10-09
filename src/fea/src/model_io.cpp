#include <QFileInfo>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>

#include <rml_file_utils.h>
#include <rproject_catalog.h>

#include "model_io.h"
#include "application.h"
#include "model_command_host.h"

namespace
{
bool sameSourceBinding(const RProjectCatalog::SourceBinding &left,
                       const RProjectCatalog::SourceBinding &right)
{
    if (left.logicalSourcePath != right.logicalSourcePath
        || left.resolvedSourcePath != right.resolvedSourcePath
        || left.savedArtifactHash != right.savedArtifactHash
        || left.sourceArtifacts.size() != right.sourceArtifacts.size())
        return false;
    for (qsizetype i = 0; i < left.sourceArtifacts.size(); ++i)
    {
        if (left.sourceArtifacts.at(i).path != right.sourceArtifacts.at(i).path
            || left.sourceArtifacts.at(i).sha256 != right.sourceArtifacts.at(i).sha256)
            return false;
    }
    return true;
}

RProjectCatalog::SourceBinding resolveNativeSource(const QString &path)
{
    QString error;
    QString errorCode;
    const auto binding = RProjectCatalog::resolveSourceBinding(path, &error, &errorCode);
    if (!binding)
        throw RError(RError::Type::OpenFile, R_ERROR_REF,
                     "Failed to resolve native source '%s': %s",
                     path.toUtf8().constData(), error.toUtf8().constData());
    return *binding;
}

template <typename Function> void onSessionThread(Function function)
{
    auto *session = Application::instance()->getSession();
    if (QThread::currentThread() == session->thread()) function();
    else QMetaObject::invokeMethod(session, std::move(function), Qt::BlockingQueuedConnection);
}
}

bool ModelIOPrivate::copyFileAtomically(const QString &sourcePath, const QString &targetPath,
                                        const ModelIOPrivate::PublicationTargetValidator &validateBeforeCommit,
                                        QString &error)
{
    error.clear();
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("Could not read staged native file '%1'.").arg(sourcePath);
        return false;
    }
    QSaveFile target(targetPath);
    target.setDirectWriteFallback(false);
    if (!target.open(QIODevice::WriteOnly))
    {
        error = QStringLiteral("Could not open native publication target '%1'.").arg(targetPath);
        return false;
    }

    // Native saves also run in QThreadPool workers with the Windows default
    // stack reserve. Keep copying bounded without allocating a MiB on stack.
    char buffer[64 * 1024];
    while (true)
    {
        const qint64 bytesRead = source.read(buffer, qint64(sizeof(buffer)));
        if (bytesRead < 0)
        {
            target.cancelWriting();
            error = QStringLiteral("Could not read staged native file '%1'.").arg(sourcePath);
            return false;
        }
        if (bytesRead == 0) break;
        if (target.write(buffer, bytesRead) != bytesRead)
        {
            target.cancelWriting();
            error = QStringLiteral("Could not write native publication target '%1'.").arg(targetPath);
            return false;
        }
    }
    if (!source.atEnd() || source.error() != QFileDevice::NoError)
    {
        target.cancelWriting();
        error = QStringLiteral("Staged native file '%1' could not be read completely.").arg(sourcePath);
        return false;
    }
    QString validationError;
    if (!validateBeforeCommit || !validateBeforeCommit(validationError))
    {
        target.cancelWriting();
        error = validationError.isEmpty()
                    ? QStringLiteral("The native publication target could not be revalidated.")
                    : validationError;
        return false;
    }
    // This check immediately precedes the atomic rename. Without OS-level
    // exclusion, a non-cooperating writer can still race between this check and commit.
    if (!target.commit())
    {
        error = QStringLiteral("Could not commit native publication target '%1'.").arg(targetPath);
        return false;
    }
    return true;
}

ModelIO::ModelIO(ModelIOType type, const QString &fileName, Model *pModel)
{
    R_LOG_TRACE_IN;
    this->type = type;
    this->fileName = fileName;
    if (pModel)
    {
        auto *session = Application::instance()->getSession();
        const uint id = session->findModelByPtr(pModel);
        if (id != RConstants::eod) this->modelStamp = session->modelCommandStamp(id);
        if (id != RConstants::eod && type == MODEL_IO_UPDATE && !ModelCommandHost::isBusy())
            session->getModelCommandHost()->query(id);
        if (type == MODEL_IO_ADD) this->modelSnapshot.reset(pModel);
        else this->modelSnapshot = std::make_unique<Model>(*pModel);
    }
    R_LOG_TRACE_OUT;
}

QString ModelIO::findNewModelName(const QString &fileName)
{
    R_LOG_TRACE_IN;
    QString modelBaseName = QFileInfo(fileName).baseName();
    QString modelName = modelBaseName;
    uint nTries = 2;
    while (Application::instance()->getSession()->findModelByName(modelName) != RConstants::eod)
    {
        modelName = modelBaseName + " [" + QString::number(nTries) + "]";
        nTries++;
    }
    R_LOG_TRACE_OUT;
    return modelName;
}

QString ModelIO::getModelSaveName(const ApplicationSettings *mainSettings, QWidget *parent, const Model &rModel, bool saveAs)
{
    R_LOG_TRACE_IN;
    QDir dataDir(mainSettings->getDataDir());
    QString binaryExtension = RModel::getDefaultFileExtension(true);
    QString asciiExtension = RModel::getDefaultFileExtension(false);
    QString dialogDesc = "Range model files (*." + binaryExtension + " *." + asciiExtension + ");;Any files (*)";

    QString fileName = RFileUtils::getFileNameWithOutTimeStep(rModel.getFileName());
    if (saveAs)
    {
        fileName = QFileDialog::getSaveFileName(parent,
                                                "Save model as",
                                                fileName,
                                                dialogDesc);
    }
    else
    {
        if (fileName.isEmpty())
        {
            fileName = dataDir.filePath(rModel.getName() + "." + binaryExtension);
            fileName = QFileDialog::getSaveFileName(parent,
                                                    "Save model",
                                                    fileName,
                                                    dialogDesc);
        }
    }

    R_LOG_TRACE_OUT;
    return fileName;
}

void ModelIO::save(Model &rModel, const QString &fileName, bool writeLinkFile)
{
    R_LOG_TRACE_IN;
    RLogger::info("Saving model to file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        rModel.clearLoadedSourceBinding();
        const QString logicalBase = static_cast<const RModel &>(rModel).write(fileName, writeLinkFile);
        rModel.setFileName(logicalBase);
        const auto source = resolveNativeSource(logicalBase);
        rModel.setLoadedSourceBinding(source);
        rModel.clearNativeReadNormalization();

        RLogger::unindent();
        RLogger::info("Model has been successfully saved to file \'%s\'.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to save model to file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::open(Model &rModel, const QString &fileName,
                   RProjectCatalog::SourceBinding *validatedSource)
{
    R_LOG_TRACE_IN;
    RLogger::info("Loading model file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        const auto sourceBefore = resolveNativeSource(fileName);
        rModel.read(sourceBefore.resolvedSourcePath);
        const auto sourceAfter = resolveNativeSource(fileName);
        if (!sameSourceBinding(sourceBefore, sourceAfter))
            throw RError(RError::Type::OpenFile, R_ERROR_REF,
                         "Native source changed while it was being read: '%s'.",
                         fileName.toUtf8().constData());
        rModel.setFileName(fileName);
        rModel.setLoadedSourceBinding(sourceAfter);
        if (validatedSource) *validatedSource = sourceAfter;

        RLogger::unindent();
        RLogger::info("Model has been successfully loaded from file \'%s\'.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to load model from file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::update(Model &rModel, const QString &fileName,
                     RProjectCatalog::SourceBinding *validatedSource)
{
    R_LOG_TRACE_IN;
    RLogger::info("Updating model from file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        const auto sourceBefore = resolveNativeSource(fileName);
        RModel tmpModel;
        tmpModel.read(sourceBefore.resolvedSourcePath);
        const auto sourceAfter = resolveNativeSource(fileName);
        if (!sameSourceBinding(sourceBefore, sourceAfter))
            throw RError(RError::Type::OpenFile, R_ERROR_REF,
                         "Native source changed while it was being read: '%s'.",
                         fileName.toUtf8().constData());

        rModel.update(tmpModel);
        rModel.setFileName(fileName);
        rModel.setLoadedSourceBinding(sourceAfter);
        if (validatedSource) *validatedSource = sourceAfter;

        RLogger::unindent();
        RLogger::info("Model has been successfully updated from file \'%s\'.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to update from file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::exportMsh(Model &rModel, const QString &fileName)
{
    R_LOG_TRACE_IN;
    RModelMsh modelMsh;
    rModel.exportTo(modelMsh);

    RLogger::info("Exporting MSH file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        modelMsh.write(fileName);

        RLogger::unindent();
        RLogger::info("MSH file \'%s\' has been successfully exported.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to export MSH file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::importMsh(Model &rModel, const QString &fileName)
{
    R_LOG_TRACE_IN;
    RModelMsh modelMsh;

    RLogger::info("Importing MSH file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        modelMsh.read(fileName);

        rModel = Model(modelMsh);

        RLogger::unindent();
        RLogger::info("MSH file \'%s\' has been successfully imported.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to import MSH file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::exportRaw(Model &rModel, const QString &fileName)
{
    R_LOG_TRACE_IN;
    RModelRaw modelRaw;
    rModel.exportTo(modelRaw);

    RLogger::info("Exporting RAW file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        modelRaw.write(fileName);

        RLogger::unindent();
        RLogger::info("RAW file \'%s\' has been successfully exported.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to export RAW file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::importRaw(Model &rModel, const QString &fileName)
{
    R_LOG_TRACE_IN;
    RModelRaw modelRaw;
    double tolerance = 0.0;

    RLogger::info("Importing RAW file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        modelRaw.read(fileName, tolerance);

        rModel = Model(modelRaw,ModelIO::findNewModelName(fileName).toUtf8().constData());

        RLogger::unindent();
        RLogger::info("RAW file \'%s\' has been successfully imported.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to import RAW file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::exportStl(Model &rModel, const QString &fileName, bool binary)
{
    R_LOG_TRACE_IN;
    RModelStl modelStl;
    rModel.exportTo(modelStl);

    RLogger::info("Exporting STL (%s) file \'%s\'.\n", binary?"binary":"ascii", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        modelStl.write(fileName,binary);

        RLogger::unindent();
        RLogger::info("STL (%s) file \'%s\' has been successfully exported.\n", binary?"binary":"ascii", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to export STL file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

void ModelIO::importStl(Model &rModel, const QString &fileName)
{
    R_LOG_TRACE_IN;
    RModelStl modelStl;
    double tolerance = 0.0;

    RLogger::info("Importing STL file \'%s\'.\n", fileName.toUtf8().constData());
    RLogger::indent();

    try
    {
        modelStl.read(fileName, tolerance);

        rModel = Model(modelStl);

        RLogger::unindent();
        RLogger::info("STL file \'%s\' has been successfully imported.\n", fileName.toUtf8().constData());
    }
    catch (const RError &error)
    {
        RLogger::unindent();
        RLogger::error("Failed to import STL file \'%s\'.\n", fileName.toUtf8().constData());
        throw error;
    }
    R_LOG_TRACE_OUT;
}

int ModelIO::perform()
{
    R_LOG_TRACE_IN;
    try
    {
        if (this->type == MODEL_IO_ADD)
        {
            onSessionThread([this] {
                auto *session = Application::instance()->getSession();
                session->addModel(*this->modelSnapshot);
                session->setModelSelected(session->getNModels()-1,true);
            });
        }
        else if (this->type == MODEL_IO_SAVE)
        {
            const QStringList actualWritePaths = this->modelSnapshot->nativeWritePaths(this->fileName, true);
            Session::ProjectPublicationLease publicationLease;
            QString publicationError;
            onSessionThread([this, &actualWritePaths, &publicationLease, &publicationError] {
                auto *session = Application::instance()->getSession();
                const uint id = session->findModelByCommandIdentity(this->modelStamp.first);
                if (id == RConstants::eod || session->modelCommandStamp(id) != this->modelStamp)
                {
                    publicationError = tr("The model changed or closed before the save started.");
                    return;
                }
                publicationLease = session->reserveModelSave(id, this->fileName, actualWritePaths,
                                                             &publicationError);
            });
            if (!publicationLease)
            {
                RLogger::error("Model save was refused: %s\n", publicationError.toUtf8().constData());
                R_LOG_TRACE_RETURN(1);
            }
            auto abortPublication = qScopeGuard([this, &publicationLease] {
                if (!publicationLease) return;
                onSessionThread([this, &publicationLease] {
                    Application::instance()->getSession()->abortModelSave(this->modelStamp.first,
                                                                           publicationLease);
                });
            });
            QTemporaryDir stagingDirectory(QDir(QDir::tempPath()).filePath(
                QStringLiteral("range-fea-model-save-XXXXXX")));
            if (!stagingDirectory.isValid())
                throw RError(RError::Type::WriteFile, R_ERROR_REF,
                             "Could not create a private staging directory for the native save.");

            const QFileInfo requestedInfo(this->fileName);
            const QString stagedRequestedPath = QDir(stagingDirectory.path()).filePath(requestedInfo.fileName());
            const QString stagedLogicalPath = static_cast<const RModel &>(*this->modelSnapshot)
                                                  .write(stagedRequestedPath, true);
            const RProjectCatalog::SourceBinding stagedBinding = resolveNativeSource(stagedLogicalPath);
            const QStringList stagedWritePaths = this->modelSnapshot->nativeWritePaths(stagedRequestedPath, true);
            if (stagedWritePaths.size() != actualWritePaths.size())
                throw RError(RError::Type::WriteFile, R_ERROR_REF,
                             "The staged native output list differs from the reserved output list.");

            const QString publicParent = requestedInfo.absoluteDir().canonicalPath();
            if (publicParent.isEmpty())
                throw RError(RError::Type::WriteFile, R_ERROR_REF,
                             "The native save destination directory could not be resolved.");
            const QString stageRoot = QDir::cleanPath(QDir::fromNativeSeparators(stagingDirectory.path()));
            const auto mapStagedPath = [&stageRoot, &publicParent](const QString &stagedPath) {
                const QString relative = QDir::fromNativeSeparators(
                    QDir(stageRoot).relativeFilePath(QDir::fromNativeSeparators(stagedPath)));
                if (relative.isEmpty() || relative == QStringLiteral("..")
                    || relative.startsWith(QStringLiteral("../")) || QFileInfo(relative).isAbsolute())
                    throw RError(RError::Type::WriteFile, R_ERROR_REF,
                                 "A staged native output escaped its private directory.");
                return QDir::cleanPath(QDir::fromNativeSeparators(QDir(publicParent).filePath(relative)));
            };

            RProjectCatalog::SourceBinding expectedWrittenBinding = stagedBinding;
            expectedWrittenBinding.logicalSourcePath = mapStagedPath(stagedBinding.logicalSourcePath);
            expectedWrittenBinding.resolvedSourcePath = mapStagedPath(stagedBinding.resolvedSourcePath);
            for (auto &artifact : expectedWrittenBinding.sourceArtifacts)
                artifact.path = mapStagedPath(artifact.path);
            const QString publicLogicalPath = mapStagedPath(stagedLogicalPath);

            bool intentBound = false;
            onSessionThread([this, &expectedWrittenBinding, &publicationLease, &publicationError, &intentBound] {
                intentBound = Application::instance()->getSession()->bindModelSaveArtifacts(
                    this->modelStamp.first, expectedWrittenBinding, publicationLease, &publicationError);
            });
            if (!intentBound)
                throw RError(RError::Type::WriteFile, R_ERROR_REF,
                             "Native save intent could not bind its staged bytes before publication: %s",
                             publicationError.toUtf8().constData());

            qsizetype publishedFiles = 0;
            for (qsizetype i = 0; i < stagedWritePaths.size(); ++i)
            {
                QString copyError;
                const QString publicTargetPath = actualWritePaths.at(i);
                const auto validateTarget = [this, &publicationLease, &publicTargetPath](QString &error) {
                    bool valid = false;
                    onSessionThread([this, &publicationLease, &publicTargetPath, &error, &valid] {
                        valid = Application::instance()->getSession()->validateModelSaveArtifactTarget(
                            this->modelStamp.first, publicTargetPath, publicationLease, &error);
                    });
                    return valid;
                };
                if (!ModelIOPrivate::copyFileAtomically(stagedWritePaths.at(i), publicTargetPath,
                                                        validateTarget, copyError))
                    throw RError(RError::Type::WriteFile, R_ERROR_REF,
                                 "Native save publication stopped after %u of %u files at '%s': %s. Earlier files may already be published.",
                                 uint(publishedFiles), uint(stagedWritePaths.size()),
                                 publicTargetPath.toUtf8().constData(), copyError.toUtf8().constData());
                ++publishedFiles;
            }
            const RProjectCatalog::SourceBinding publishedBinding = resolveNativeSource(publicLogicalPath);
            if (!sameSourceBinding(publishedBinding, expectedWrittenBinding))
                throw RError(RError::Type::WriteFile, R_ERROR_REF,
                             "The public native files no longer match the staged save at '%s'. Files may already have been published.",
                             publicLogicalPath.toUtf8().constData());
            this->modelSnapshot->setFileName(publicLogicalPath);
            this->modelSnapshot->setLoadedSourceBinding(expectedWrittenBinding);
            bool metadataRecorded = false;
            onSessionThread([this, &publicationError, &publicationLease, &metadataRecorded,
                             &expectedWrittenBinding] {
                auto *session = Application::instance()->getSession();
                const uint id = session->findModelByCommandIdentity(this->modelStamp.first);
                if (id == RConstants::eod || session->modelCommandStamp(id) != this->modelStamp)
                {
                    publicationError = tr("The model changed or closed while saving; the saved snapshot was not attached to the current model.");
                    session->abortModelSave(this->modelStamp.first, publicationLease, publicationError);
                    return;
                }
                metadataRecorded = session->recordModelSaved(id, expectedWrittenBinding.logicalSourcePath,
                                                             expectedWrittenBinding, publicationLease,
                                                             this->modelStamp, &publicationError);
                if (!metadataRecorded)
                    session->abortModelSave(this->modelStamp.first, publicationLease, publicationError);
            });
            publicationLease.reset();
            abortPublication.dismiss();
            if (!publicationError.isEmpty())
            {
                RLogger::error("Model file was saved but publication did not complete: %s\n", publicationError.toUtf8().constData());
                R_LOG_TRACE_RETURN(1);
            }
        }
        else if (this->type == MODEL_IO_OPEN)
        {
            Model model;
            RProjectCatalog::SourceBinding sourceBinding;
            ModelIO::open(model,this->fileName,&sourceBinding);
            onSessionThread([&model, &sourceBinding] {
                Application::instance()->getSession()->addModel(model, sourceBinding);
            });
        }
        else if (this->type == MODEL_IO_UPDATE)
        {
            RProjectCatalog::SourceBinding sourceBinding;
            ModelIO::update(*this->modelSnapshot,this->fileName,&sourceBinding);
            this->modelSnapshot->setFileName(this->fileName);
            this->modelSnapshot->setLoadedSourceBinding(sourceBinding);
            bool applied = false;
            QString auditError;
            onSessionThread([this, &applied, &auditError, &sourceBinding] {
                auto *session = Application::instance()->getSession();
                const uint id = session->findModelByCommandIdentity(this->modelStamp.first);
                if (id == RConstants::eod || session->modelCommandStamp(id) != this->modelStamp) return;
                QString sourceError;
                if (!session->validateReloadSource(id, sourceBinding, &sourceError))
                {
                    auditError = sourceError;
                    return;
                }
                Model previous = session->getModel(id);
                previous.copyHistoryFrom(session->getModel(id));
                session->storeCurentModelVersion(id, tr("Update model from file"));
                session->setModel(id, *this->modelSnapshot);
                QString hostError;
                session->getModelCommandHost()->recordExternalChange(id, "reload", &hostError);
                QString projectError;
                if (!session->recordModelReloaded(id, sourceBinding, &projectError))
                {
                    session->setModel(id, previous);
                    QString rollbackError;
                    session->getModelCommandHost()->recordExternalChange(id, "reload_rejected", &rollbackError);
                    if (!auditError.isEmpty()) auditError += QStringLiteral("; ");
                    auditError += projectError;
                    if (!rollbackError.isEmpty()) auditError += QStringLiteral("; ") + rollbackError;
                }
                if (!hostError.isEmpty())
                {
                    if (!auditError.isEmpty()) auditError += QStringLiteral("; ");
                    auditError += hostError;
                }
                applied = true;
            });
            if (!applied)
            {
                RLogger::error("The model changed or closed before the reload completed.\n");
                R_LOG_TRACE_RETURN(1);
            }
            if (!auditError.isEmpty())
            {
                RLogger::error("Model was reloaded but its audit could not be saved: %s\n", auditError.toUtf8().constData());
                R_LOG_TRACE_RETURN(1);
            }
        }
        else if (this->type == MODEL_IO_MSH_EXPORT)
        {
            ModelIO::exportMsh(*this->modelSnapshot,this->fileName);
        }
        else if (this->type == MODEL_IO_MSH_IMPORT)
        {
            Model model;
            ModelIO::importMsh(model,this->fileName);
            onSessionThread([&model] { Application::instance()->getSession()->addModel(model); });
        }
        else if (this->type == MODEL_IO_RAW_EXPORT)
        {
            ModelIO::exportRaw(*this->modelSnapshot,this->fileName);
        }
        else if (this->type == MODEL_IO_RAW_IMPORT)
        {
            Model model;
            ModelIO::importRaw(model,this->fileName);
            onSessionThread([&model] { Application::instance()->getSession()->addModel(model); });
        }
        else if (this->type == MODEL_IO_STL_ASCII_EXPORT)
        {
            ModelIO::exportStl(*this->modelSnapshot,this->fileName,false);
        }
        else if (this->type == MODEL_IO_STL_BINARY_EXPORT)
        {
            ModelIO::exportStl(*this->modelSnapshot,this->fileName);
        }
        else if (this->type == MODEL_IO_STL_IMPORT)
        {
            Model model;
            ModelIO::importStl(model,this->fileName);
            onSessionThread([&model] { Application::instance()->getSession()->addModel(model); });
        }
    }
    catch (const RError &error)
    {
        RLogger::error("%s File: '%s'\n",
                       error.getMessage().toUtf8().constData(),
                       fileName.toUtf8().constData());
        R_LOG_TRACE_RETURN(1);
    }
    R_LOG_TRACE_RETURN(0);
}
