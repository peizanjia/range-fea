#include <QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdio>
#include <cstdlib>
#include <rbl_error.h>
#include <rmodel_command_session.h>
#include <rproject_catalog.h>
#include <rml_file_utils.h>

namespace
{
QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QString hashBytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString sourceIndexKey(const QString &path)
{
    const QString canonical = QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).canonicalFilePath()));
#ifdef Q_OS_WIN
    return canonical.toCaseFolded();
#else
    return canonical;
#endif
}

QString writeNativeModel(const QString &path, const RModel &model, bool writeLinkFile = false)
{
    try
    {
        return model.write(path, writeLinkFile);
    }
    catch (const RError &)
    {
        return {};
    }
}

bool writeNativeLink(const QString &path, const QString &target)
{
    try
    {
        RFileUtils::writeLink(path, target, RModel::version, true);
        return true;
    }
    catch (const RError &)
    {
        return false;
    }
}

QString cliPath()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("range-project.exe"));
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("range-project"));
#endif
}

struct CliResult
{
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QByteArray output;
    QByteArray error;
    bool finished = false;
};

CliResult runCli(const QStringList &arguments)
{
    QProcess process;
    process.setProgram(cliPath());
    process.setArguments(arguments);
    process.start();
    CliResult result;
    if (!process.waitForStarted(5000))
    {
        result.error = process.errorString().toUtf8();
        return result;
    }
    result.finished = process.waitForFinished(30000);
    if (!result.finished)
    {
        process.kill();
        process.waitForFinished(5000);
    }
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.output = process.readAllStandardOutput();
    result.error.append(process.readAllStandardError());
    return result;
}

bool parseObject(const QByteArray &bytes, QJsonObject &object)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    return true;
}

RModel sampleModel(double x)
{
    RModel model;
    model.setName(QStringLiteral("catalog fixture"));
    model.addNode(RNode(x, 2.0, 3.0));
    return model;
}

constexpr int ForcedCrashExitCode = 86;

struct HelperResult
{
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QByteArray output;
    QByteArray error;
};

HelperResult runSelfHelper(const QStringList &arguments)
{
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(QStringList{QStringLiteral("--wf05-helper")} + arguments);
    process.start();
    HelperResult result;
    result.started = process.waitForStarted(5000);
    if (!result.started)
    {
        result.error = process.errorString().toUtf8();
        return result;
    }
    result.finished = process.waitForFinished(30000);
    if (!result.finished)
    {
        process.kill();
        process.waitForFinished(5000);
    }
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.output = process.readAllStandardOutput();
    result.error = process.readAllStandardError();
    return result;
}

int helperFailure(const QString &message)
{
    const QByteArray bytes = message.toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    return 90;
}

bool appendSideEffect(const QString &path, const QString &effectId)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    const QByteArray line = effectId.toUtf8() + '\n';
    return file.write(line) == line.size() && file.flush();
}

int runCrashHelper(const QStringList &arguments)
{
    if (arguments.isEmpty()) return helperFailure(QStringLiteral("missing helper mode"));
    const QString mode = arguments.at(0);
    QString error, errorCode;

    if (mode == QStringLiteral("operation-boundary"))
    {
        if (arguments.size() != 4) return helperFailure(QStringLiteral("invalid operation-boundary arguments"));
        const QString phaseName = arguments.at(1);
        const QString workspace = arguments.at(2);
        const QString sideEffectPath = arguments.at(3);
        auto catalog = RProjectCatalog::open(workspace, &error);
        if (!catalog) return helperFailure(error);
        const QString key = QStringLiteral("crash-boundary-") + phaseName;
        const QJsonObject request{{QStringLiteral("commandId"), QStringLiteral("test.operation")},
                                  {QStringLiteral("phase"), phaseName}};
        RProjectCatalog::OperationAssociation candidate;
        candidate.candidateSha256 = hashBytes(QByteArrayLiteral("candidate bytes"));
        const auto begun = catalog->beginOperation(key, request, candidate, {}, &error, &errorCode);
        if (begun.disposition != RProjectCatalog::OperationDisposition::Created)
            return helperFailure(error.isEmpty() ? QStringLiteral("operation was not newly created") : error);

        if (phaseName == QStringLiteral("accepted")
            || phaseName == QStringLiteral("artifact_published")
            || phaseName == QStringLiteral("observed"))
        {
            RProjectCatalog::OperationAssociation accepted;
            accepted.acceptedRevision = QStringLiteral("r1");
            accepted.acceptedModelSha256 = hashBytes(QByteArrayLiteral("accepted model bytes"));
            if (!catalog->advanceOperation(begun.operation.operationId,
                    RProjectCatalog::OperationPhase::Accepted, accepted, {}, &error, &errorCode))
                return helperFailure(error);
        }
        if (phaseName == QStringLiteral("artifact_published")
            || phaseName == QStringLiteral("observed"))
        {
            const QByteArray effectBytes = QByteArrayLiteral("published test artifact");
            if (!writeBytes(sideEffectPath, effectBytes))
                return helperFailure(QStringLiteral("could not publish test artifact"));
            RProjectCatalog::OperationAssociation published;
            published.acceptedRevision = QStringLiteral("r1");
            published.acceptedModelSha256 = hashBytes(QByteArrayLiteral("accepted model bytes"));
            if (!catalog->advanceOperation(begun.operation.operationId,
                    RProjectCatalog::OperationPhase::ArtifactPublished, published, {}, &error, &errorCode))
                return helperFailure(error);
        }
        if (phaseName == QStringLiteral("observed"))
        {
            RProjectCatalog::OperationAssociation observed;
            observed.observation = {{QStringLiteral("status"), QStringLiteral("accepted")},
                {QStringLiteral("code"), QStringLiteral("ACCEPTED")},
                {QStringLiteral("result"), QJsonObject{
                    {QStringLiteral("acceptedRevision"), QStringLiteral("r1")},
                    {QStringLiteral("artifactSha256"), hashBytes(QByteArrayLiteral("published test artifact"))}}}};
            if (!catalog->advanceOperation(begun.operation.operationId,
                    RProjectCatalog::OperationPhase::Observed, observed, {}, &error, &errorCode))
                return helperFailure(error);
        }
        if (phaseName != QStringLiteral("intent") && phaseName != QStringLiteral("accepted")
            && phaseName != QStringLiteral("artifact_published") && phaseName != QStringLiteral("observed"))
            return helperFailure(QStringLiteral("unknown operation phase"));

        // _Exit bypasses C++ and Qt destructors after the durable boundary.
        std::_Exit(ForcedCrashExitCode);
    }

    if (mode == QStringLiteral("idempotent-operation"))
    {
        if (arguments.size() != 3) return helperFailure(QStringLiteral("invalid idempotent-operation arguments"));
        const QString workspace = arguments.at(1);
        const QString sideEffectPath = arguments.at(2);
        auto catalog = RProjectCatalog::open(workspace, &error);
        if (!catalog) return helperFailure(error);
        const QString key = QStringLiteral("cross-process-idempotency-key");
        const QJsonObject request{{QStringLiteral("commandId"), QStringLiteral("test.side-effect")},
                                  {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("value"), 17}}}};
        const auto begun = catalog->beginOperation(key, request, {}, {}, &error, &errorCode);
        if (begun.disposition == RProjectCatalog::OperationDisposition::Conflict)
            return helperFailure(error.isEmpty() ? QStringLiteral("idempotency conflict") : error);

        RProjectCatalog::DurableOperation result = begun.operation;
        if (begun.disposition == RProjectCatalog::OperationDisposition::Created)
        {
            const QString effectId = uuid();
            if (!appendSideEffect(sideEffectPath, effectId))
                return helperFailure(QStringLiteral("could not record test side effect"));
            RProjectCatalog::OperationAssociation accepted;
            accepted.acceptedRevision = QStringLiteral("r1");
            accepted.acceptedModelSha256 = hashBytes(effectId.toUtf8());
            if (!catalog->advanceOperation(result.operationId, RProjectCatalog::OperationPhase::Accepted,
                                           accepted, {}, &error, &errorCode))
                return helperFailure(error);
            RProjectCatalog::OperationAssociation observed;
            observed.observation = {{QStringLiteral("status"), QStringLiteral("accepted")},
                                    {QStringLiteral("code"), QStringLiteral("ACCEPTED")},
                                    {QStringLiteral("result"), QJsonObject{{QStringLiteral("effectId"), effectId}}}};
            if (!catalog->advanceOperation(result.operationId, RProjectCatalog::OperationPhase::Observed,
                                           observed, {}, &error, &errorCode))
                return helperFailure(error);
            const auto persisted = catalog->queryOperation(result.operationId, {}, &error, &errorCode);
            if (!persisted) return helperFailure(error);
            result = *persisted;
        }
        else if (begun.disposition == RProjectCatalog::OperationDisposition::Reused)
        {
            if (result.phase != RProjectCatalog::OperationPhase::Observed
                || result.association.observation.isEmpty())
                return helperFailure(QStringLiteral("reused operation has no terminal observation; refusing to retry"));
        }

        const QByteArray output = QJsonDocument(result.association.observation).toJson(QJsonDocument::Compact);
        std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return 0;
    }

    if (mode == QStringLiteral("save-partial"))
    {
        if (arguments.size() != 5) return helperFailure(QStringLiteral("invalid save-partial arguments"));
        const QString workspace = arguments.at(1);
        const QString documentId = arguments.at(2);
        const QString modelPath = arguments.at(3);
        const QString linkPath = arguments.at(4);
        const QByteArray oldModelBytes = QByteArrayLiteral("old model bytes");
        const QByteArray newModelBytes = QByteArrayLiteral("new accepted model bytes");
        const QByteArray newLinkBytes = QByteArrayLiteral("new link bytes");
        auto catalog = RProjectCatalog::open(workspace, &error);
        if (!catalog) return helperFailure(error);
        const auto document = catalog->document(documentId, &error);
        if (!document) return helperFailure(error);
        auto lease = catalog->reservePublication(documentId, {modelPath, linkPath},
                                                 document->sourceRevision, &error, &errorCode);
        if (!lease) return helperFailure(error);

        RProjectCatalog::OperationArtifactIntent modelArtifact;
        modelArtifact.path = modelPath;
        modelArtifact.expectedPriorExists = true;
        modelArtifact.expectedPriorSha256 = hashBytes(oldModelBytes);
        modelArtifact.intendedSha256 = hashBytes(newModelBytes);
        RProjectCatalog::OperationArtifactIntent linkArtifact;
        linkArtifact.path = linkPath;
        linkArtifact.intendedSha256 = hashBytes(newLinkBytes);
        const QJsonObject request{{QStringLiteral("commandId"), QStringLiteral("document.save")},
                                  {QStringLiteral("sourceRevision"), document->sourceRevision}};
        const auto begun = catalog->beginSavePublication(documentId, QStringLiteral("crash-save-key"),
            request, document->sourceRevision, {modelArtifact, linkArtifact}, lease, {}, &error, &errorCode);
        if (begun.disposition != RProjectCatalog::OperationDisposition::Created)
            return helperFailure(error.isEmpty() ? QStringLiteral("save operation was not created") : error);
        RProjectCatalog::OperationAssociation accepted;
        accepted.acceptedRevision = QStringLiteral("r1");
        accepted.acceptedModelSha256 = hashBytes(newModelBytes);
        if (!catalog->advanceOperation(begun.operation.operationId, RProjectCatalog::OperationPhase::Accepted,
                                       accepted, lease, &error, &errorCode))
            return helperFailure(error);
        if (!writeBytes(modelPath, newModelBytes)) return helperFailure(QStringLiteral("could not publish first save artifact"));

        // The model file is committed while the second native output is still absent.
        std::_Exit(ForcedCrashExitCode);
    }

    if (mode == QStringLiteral("close-retry"))
    {
        if (arguments.size() != 6) return helperFailure(QStringLiteral("invalid close-retry arguments"));
        const QString workspace = arguments.at(1);
        const QString journalPath = arguments.at(2);
        const QString retryId = arguments.at(3);
        const QString operationId = arguments.at(4);
        const QString crashWindow = arguments.at(5);
        RModelCommandSession::JournalOptions journalOptions;
        journalOptions.path = journalPath;
        RModelCommandSession session(sampleModel(8.0), journalOptions);
        const QJsonObject payload = session.prepareCloseRetry(retryId, operationId, &error);
        if (payload.isEmpty()) return helperFailure(error);
        auto catalog = RProjectCatalog::open(workspace, &error);
        if (!catalog) return helperFailure(error);
        if (!catalog->queueCloseRetry(retryId, operationId, QStringLiteral("journal_write_failed"),
                                      payload, {}, &error, &errorCode))
            return helperFailure(error);
        if (crashWindow == QStringLiteral("after-journal"))
        {
            if (!RModelCommandSession::replayCloseRetry(payload, &error, &errorCode))
                return helperFailure(error);
        }
        else if (crashWindow != QStringLiteral("after-queue"))
        {
            return helperFailure(QStringLiteral("unknown close-retry crash window"));
        }

        std::_Exit(ForcedCrashExitCode);
    }

    return helperFailure(QStringLiteral("unknown helper mode"));
}
}

class TestProjectCatalog : public QObject
{
    Q_OBJECT

private slots:
    void createReopenAndAdoptWorkflowIdentity();
    void newUnsavedDocumentKeepsIdentityOnFirstSaveAs();
    void sourceAliasesCopyAndTombstoneNeverReuseIdentity();
    void nativeLinkAliasesResolveAndTargetTamperingConflicts();
    void nativeSourceBindingRejectsInvalidHeaderCycleAndDepth();
    void publicationLeaseReservesPathsAndBlocksMutations();
    void recordSaveRejectsChangedWrittenBinding();
    void publicationLeaseCannotCrossWorkspaceRoot();
    void separateCatalogInstancesUseLocalRevisionCas();
    void documentInstanceLeasePreventsCrossInstanceReloadAndSnapshot();
    void capabilityManifestExampleParsesAndScopesClaims();
    void corruptAndUnsupportedManifestsArePreserved();
    void lockRejectsConcurrentMutationWithoutManifestChange();
    void snapshotsAreCasBoundHashAddressedAndImmutable();
    void invalidRuntimeReferencesAreRejected();
    void durableOperationsSurviveReopenAndRejectIdempotencyConflicts();
    void durableOperationLedgerFailsClosedOnCorruptionAndLock();
    void runtimeLivenessDoesNotRetireAnotherLiveCatalogOperation();
    void operationCrashBoundariesRequireFreshRuntimeReconciliation();
    void crossProcessDuplicateOperationReturnsOriginalObservationWithoutRepeatingSideEffect();
    void savePublicationReconcilesPartialFilesAndExternalReplacement();
    void partialSavePublicationSurvivesAbruptProcessExit();
    void closeRetryQueueSurvivesReopenAndCompletes();
    void closeRetryQueueReplaysAcrossBothProcessCrashWindows();
    void cliQueryIsReadonlyJsonOnly();
};

void TestProjectCatalog::createReopenAndAdoptWorkflowIdentity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString expectedProjectId = uuid();
    QJsonObject workflowState{
        {QStringLiteral("storageVersion"), 1},
        {QStringLiteral("projectId"), expectedProjectId},
        {QStringLiteral("modelId"), uuid()},
        {QStringLiteral("sessionId"), uuid()},
        {QStringLiteral("revision"), 0},
        {QStringLiteral("auditRevision"), 0},
        {QStringLiteral("configuration"), QJsonObject{
             {QStringLiteral("dt"), 0.005}, {QStringLiteral("steps"), 400},
             {QStringLiteral("targetDamping"), 0.1}, {QStringLiteral("model"), QStringLiteral("four-rod")}}},
        {QStringLiteral("jobs"), QJsonObject{}},
        {QStringLiteral("previews"), QJsonObject{}},
        {QStringLiteral("audit"), QJsonArray{}},
        {QStringLiteral("extensions"), QJsonObject{}}};
    QVERIFY(QDir().mkpath(workspace));
    const QByteArray stateBefore = QJsonDocument(workflowState).toJson(QJsonDocument::Compact);
    QVERIFY(writeBytes(QDir(workspace).filePath(QStringLiteral("workflow-state.json")), stateBefore));

    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    QCOMPARE(catalog->projectId(), expectedProjectId);
    QCOMPARE(readBytes(QDir(workspace).filePath(QStringLiteral("workflow-state.json"))), stateBefore);
    const QByteArray manifestBefore = readBytes(QDir(workspace).filePath(QStringLiteral("project.json")));
    auto reopened = RProjectCatalog::open(workspace, &error);
    QVERIFY2(reopened, qPrintable(error));
    QCOMPARE(reopened->projectId(), catalog->projectId());
    QCOMPARE(readBytes(QDir(workspace).filePath(QStringLiteral("project.json"))), manifestBefore);

    const QJsonObject query = reopened->query(&error);
    QVERIFY2(!query.isEmpty(), qPrintable(error));
    QCOMPARE(query.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("range.project/v1"));
    QCOMPARE(query.value(QStringLiteral("storageVersion")).toInt(), 1);
    QCOMPARE(query.value(QStringLiteral("projectId")).toString(), expectedProjectId);
    QCOMPARE(query.value(QStringLiteral("documents")).toArray().size(), 0);

    const QString invalidWorkspace = temporary.filePath(QStringLiteral("invalid-state-workspace"));
    QVERIFY(QDir().mkpath(invalidWorkspace));
    const QString invalidStatePath = QDir(invalidWorkspace).filePath(QStringLiteral("workflow-state.json"));
    const QByteArray invalidState = QByteArrayLiteral("preserve invalid workflow state");
    QVERIFY(writeBytes(invalidStatePath, invalidState));
    auto independent = RProjectCatalog::open(invalidWorkspace, &error);
    QVERIFY2(independent, qPrintable(error));
    QVERIFY(independent->projectId() != expectedProjectId);
    QCOMPARE(readBytes(invalidStatePath), invalidState);
}

void TestProjectCatalog::sourceAliasesCopyAndTombstoneNeverReuseIdentity()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString sourceA = temporary.filePath(QStringLiteral("source-a.ram"));
    const QString sourceSameBytes = temporary.filePath(QStringLiteral("source-copy.ram"));
    const QString saveAs = temporary.filePath(QStringLiteral("save-as.rbm"));
    const RModel sameModel = sampleModel(3.0);
    QVERIFY(!writeNativeModel(sourceA, sameModel, true).isEmpty());
    QVERIFY(!writeNativeModel(sourceSameBytes, sameModel).isEmpty());
    const QString numberedTarget = sameModel.nativeWritePaths(sourceA, true).first();
    QVERIFY(QFileInfo::exists(numberedTarget));
    QCOMPARE(readBytes(sourceSameBytes), readBytes(numberedTarget));
    QVERIFY(!writeNativeModel(saveAs, sampleModel(7.0)).isEmpty());

    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString originalId = catalog->registerDocument(sourceA, QStringLiteral("Beam A"), {}, &error);
    QVERIFY2(!originalId.isEmpty(), qPrintable(error));
    const auto sourceBinding = RProjectCatalog::resolveSourceBinding(sourceA, &error);
    QVERIFY2(sourceBinding.has_value(), qPrintable(error));
    QCOMPARE(sourceBinding->logicalSourcePath, QFileInfo(sourceA).canonicalFilePath());
    QCOMPARE(sourceBinding->resolvedSourcePath, QFileInfo(numberedTarget).canonicalFilePath());
    QCOMPARE(sourceBinding->sourceArtifacts.size(), 2);
    QCOMPARE(sourceBinding->sourceArtifacts.first().path, sourceBinding->logicalSourcePath);
    QCOMPARE(sourceBinding->sourceArtifacts.last().path, sourceBinding->resolvedSourcePath);
    QCOMPARE(catalog->registerDocument(numberedTarget, QStringLiteral("Beam A"), {}, &error), originalId);
    const QJsonObject firstIndex = catalog->query(&error).value(QStringLiteral("sourcePathIndex")).toObject();
    QCOMPARE(firstIndex.value(sourceIndexKey(sourceA)).toString(), originalId);
    QCOMPARE(firstIndex.value(sourceIndexKey(numberedTarget)).toString(), originalId);
    QCOMPARE(catalog->registerDocument(sourceA, QStringLiteral("Beam A renamed"), {}, &error), originalId);
    QCOMPARE(catalog->document(originalId, &error)->label, QStringLiteral("Beam A"));
    const QString sameBytesId = catalog->registerDocument(sourceSameBytes, QStringLiteral("Beam A"), {}, &error);
    QVERIFY2(!sameBytesId.isEmpty(), qPrintable(error));
    QVERIFY(sameBytesId != originalId);
    QCOMPARE(catalog->registerDocument(sourceA, QStringLiteral("Beam B"), {}, &error), originalId);
    QCOMPARE(catalog->document(originalId, &error)->label, QStringLiteral("Beam A"));
    QVERIFY2(catalog->renameDocument(originalId, QStringLiteral("Beam B"), &error), qPrintable(error));
    QVERIFY2(catalog->renameDocument(sameBytesId, QStringLiteral("Beam B"), &error), qPrintable(error));
    const auto renamed = catalog->document(sameBytesId, &error);
    QVERIFY(renamed.has_value());
    QCOMPARE(renamed->persistentDocumentId, sameBytesId);
    QCOMPARE(renamed->label, QStringLiteral("Beam B"));

    const QString copyId = catalog->createCopy(originalId, QStringLiteral("Beam B"), &error);
    QVERIFY2(!copyId.isEmpty(), qPrintable(error));
    QVERIFY(copyId != originalId);
    const auto copy = catalog->document(copyId, &error);
    QVERIFY2(copy.has_value(), qPrintable(error));
    QCOMPARE(copy->originDocumentId, originalId);
    QVERIFY(copy->sourcePath.isEmpty());
    QVERIFY(copy->dirty);

    QVERIFY2(catalog->recordSave(originalId, saveAs, &error), qPrintable(error));
    const auto saved = catalog->document(originalId, &error);
    QVERIFY2(saved.has_value(), qPrintable(error));
    QCOMPARE(saved->sourcePath, QFileInfo(saveAs).canonicalFilePath());
    QCOMPARE(saved->savedArtifactHash, hashBytes(readBytes(saveAs)));
    QCOMPARE(saved->sourceRevision, QStringLiteral("r0"));
    QVERIFY(!writeNativeModel(saveAs, sampleModel(11.0)).isEmpty());
    QVERIFY(catalog->registerDocument(saveAs, QStringLiteral("Beam A renamed"), {}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY2(catalog->acceptReload(originalId, saveAs, QStringLiteral("r1"), &error), qPrintable(error));
    const auto reloaded = catalog->document(originalId, &error);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->sourceRevision, QStringLiteral("r1"));
    QCOMPARE(reloaded->savedArtifactHash, hashBytes(readBytes(saveAs)));
    QVERIFY(!catalog->recordAccepted(originalId, QStringLiteral("r0"), false, {}, &error));

    const QString reimportedOldPath = catalog->registerDocument(sourceA, QStringLiteral("Old source import"), {}, &error);
    QVERIFY2(!reimportedOldPath.isEmpty(), qPrintable(error));
    QVERIFY(reimportedOldPath != originalId);
    QVERIFY2(catalog->tombstone(reimportedOldPath, &error), qPrintable(error));
    const auto removed = catalog->document(reimportedOldPath, &error);
    QVERIFY(removed.has_value());
    QVERIFY(removed->tombstone);

    const QString importedAfterRemove = catalog->registerDocument(sourceA, QStringLiteral("Reimported source"), {}, &error);
    QVERIFY2(!importedAfterRemove.isEmpty(), qPrintable(error));
    QVERIFY(importedAfterRemove != reimportedOldPath);
    QVERIFY(importedAfterRemove != originalId);

    auto reopened = RProjectCatalog::open(workspace, &error);
    QVERIFY2(reopened, qPrintable(error));
    QVERIFY(reopened->document(originalId, &error).has_value());
    QVERIFY(reopened->document(copyId, &error).has_value());
}

void TestProjectCatalog::nativeLinkAliasesResolveAndTargetTamperingConflicts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString logicalLink = temporary.filePath(QStringLiteral("linked-model.rbm"));
    const QString replacementPath = temporary.filePath(QStringLiteral("replacement.rbm"));
    const RModel originalModel = sampleModel(2.25);
    QVERIFY(!writeNativeModel(logicalLink, originalModel, true).isEmpty());
    const QStringList writePaths = originalModel.nativeWritePaths(logicalLink, true);
    QCOMPARE(writePaths.size(), 2);
    const QString numberedTarget = writePaths.first();
    QCOMPARE(writePaths.last(), logicalLink);
    QVERIFY(QFileInfo::exists(numberedTarget));

    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto binding = RProjectCatalog::resolveSourceBinding(logicalLink, &error, &errorCode);
    QVERIFY2(binding.has_value(), qPrintable(error));
    QCOMPARE(binding->sourceArtifacts.size(), 2);
    QCOMPARE(binding->savedArtifactHash, binding->sourceArtifacts.last().sha256);
    const QString id = catalog->registerDocument(*binding, QStringLiteral("Linked beam"), {}, &error, &errorCode);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QCOMPARE(catalog->registerDocument(numberedTarget, QStringLiteral("Linked beam"), {}, &error, &errorCode), id);
    const QJsonObject pathIndex = catalog->query(&error).value(QStringLiteral("sourcePathIndex")).toObject();
    QCOMPARE(pathIndex.value(sourceIndexKey(logicalLink)).toString(), id);
    QCOMPARE(pathIndex.value(sourceIndexKey(numberedTarget)).toString(), id);

    QVERIFY2(catalog->recordAccepted(id, QStringLiteral("r1"), true, {}, &error,
                                     QStringLiteral("r0"), &errorCode), qPrintable(error));
    RProjectCatalog::InstanceRef ref{uuid(), uuid(), 1};
    RProjectCatalog::SnapshotArtifact snapshot;
    QVERIFY2(catalog->captureSnapshot(id, originalModel, ref, &snapshot, &error,
                                     QStringLiteral("r1"), &errorCode), qPrintable(error));
    const QString previousSnapshot = QDir(workspace).filePath(snapshot.relativePath);
    QVERIFY(QFileInfo::exists(previousSnapshot));

    bool reopenedSavedSource = false;
    QVERIFY2(catalog->acceptReload(id, logicalLink, QStringLiteral("r2"), &error,
                                   QStringLiteral("r1"), &errorCode, &reopenedSavedSource), qPrintable(error));
    QVERIFY(reopenedSavedSource);
    const auto savedReload = catalog->document(id, &error);
    QVERIFY(savedReload.has_value());
    QVERIFY(!savedReload->dirty);
    QCOMPARE(savedReload->orphanSnapshots.size(), 1);
    QVERIFY(QFileInfo::exists(previousSnapshot));

    QVERIFY(!writeNativeModel(replacementPath, sampleModel(19.0)).isEmpty());
    QVERIFY(writeBytes(numberedTarget, readBytes(replacementPath)));
    QVERIFY(catalog->registerDocument(*binding, QStringLiteral("Linked beam"), {}, &error, &errorCode).isEmpty());
    QCOMPARE(errorCode, QStringLiteral("source_conflict"));
    error.clear();
    errorCode.clear();
    QVERIFY(catalog->registerDocument(numberedTarget, QStringLiteral("Linked beam"), {}, &error, &errorCode).isEmpty());
    QCOMPARE(errorCode, QStringLiteral("source_conflict"));

    QVERIFY2(catalog->acceptReload(id, logicalLink, QStringLiteral("r3"), &error,
                                   QStringLiteral("r2"), &errorCode, &reopenedSavedSource), qPrintable(error));
    QVERIFY(!reopenedSavedSource);
    const auto reloaded = catalog->document(id, &error);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->sourceRevision, QStringLiteral("r3"));
    QCOMPARE(reloaded->sourcePath, QFileInfo(logicalLink).canonicalFilePath());
    QCOMPARE(reloaded->resolvedSourcePath, QFileInfo(numberedTarget).canonicalFilePath());
    QCOMPARE(reloaded->savedArtifactHash, hashBytes(readBytes(numberedTarget)));
    QCOMPARE(reloaded->orphanSnapshots.size(), 1);
    QCOMPARE(reloaded->orphanSnapshots.first().toObject().value(QStringLiteral("relativePath")).toString(),
             snapshot.relativePath);
    QVERIFY(QFileInfo::exists(previousSnapshot));
    QCOMPARE(catalog->registerDocument(numberedTarget, QStringLiteral("Terminal alias"), {}, &error, &errorCode), id);
}

void TestProjectCatalog::nativeSourceBindingRejectsInvalidHeaderCycleAndDepth()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString invalidHeader = temporary.filePath(QStringLiteral("invalid.rbm"));
    QVERIFY(writeBytes(invalidHeader, QByteArrayLiteral("not a Range model")));
    QString error, errorCode;
    QVERIFY(!RProjectCatalog::resolveSourceBinding(invalidHeader, &error, &errorCode).has_value());
    QCOMPARE(errorCode, QStringLiteral("invalid_argument"));

    const QString cycleA = temporary.filePath(QStringLiteral("cycle-a.rbm"));
    const QString cycleB = temporary.filePath(QStringLiteral("cycle-b.rbm"));
    QVERIFY(writeNativeLink(cycleA, cycleB));
    QVERIFY(writeNativeLink(cycleB, cycleA));
    error.clear();
    errorCode.clear();
    QVERIFY(!RProjectCatalog::resolveSourceBinding(cycleA, &error, &errorCode).has_value());
    QCOMPARE(errorCode, QStringLiteral("invalid_argument"));

    const QString terminal = temporary.filePath(QStringLiteral("terminal.rbm"));
    QVERIFY(!writeNativeModel(terminal, sampleModel(1.75)).isEmpty());
    QString next = terminal;
    QString firstLink;
    for (int index = 16; index >= 0; --index)
    {
        const QString current = temporary.filePath(QStringLiteral("deep-%1.rbm").arg(index));
        QVERIFY(writeNativeLink(current, next));
        next = current;
        firstLink = current;
    }
    error.clear();
    errorCode.clear();
    QVERIFY(!RProjectCatalog::resolveSourceBinding(firstLink, &error, &errorCode).has_value());
    QCOMPARE(errorCode, QStringLiteral("invalid_argument"));

    const QString oversizedLink = temporary.filePath(QStringLiteral("oversized-link.rbm"));
    QVERIFY(writeNativeLink(oversizedLink, terminal));
    QFile append(oversizedLink);
    QVERIFY(append.open(QIODevice::WriteOnly | QIODevice::Append));
    const QByteArray padding(70 * 1024, 'x');
    QCOMPARE(append.write(padding), qint64(padding.size()));
    append.close();
    error.clear();
    errorCode.clear();
    QVERIFY(!RProjectCatalog::resolveSourceBinding(oversizedLink, &error, &errorCode).has_value());
    QCOMPARE(errorCode, QStringLiteral("invalid_argument"));
}

void TestProjectCatalog::publicationLeaseReservesPathsAndBlocksMutations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.rbm"));
    const QString output = temporary.filePath(QStringLiteral("published.rbm"));
    const QString managedDirectory = QDir(workspace).filePath(QStringLiteral("models"));
    QVERIFY(!writeNativeModel(source, sampleModel(2.0)).isEmpty());
    QVERIFY(QDir().mkpath(managedDirectory));
    QVERIFY(QDir().mkpath(QDir(managedDirectory).filePath(QStringLiteral("reserved"))));

    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString id = catalog->registerDocument(source, QStringLiteral("Lease source"), {}, &error, &errorCode);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    const QString otherId = catalog->createDocument(QStringLiteral("Other document"), {}, &error);
    QVERIFY2(!otherId.isEmpty(), qPrintable(error));
    RModel outputModel = sampleModel(9.0);

    auto collided = catalog->reservePublication(otherId, outputModel.nativeWritePaths(source, true),
                                                 QStringLiteral("r0"), &error, &errorCode);
    QVERIFY(!collided);
    QCOMPARE(errorCode, QStringLiteral("output_path_conflict"));
    const QString managedOutput = QDir(managedDirectory).filePath(QStringLiteral("reserved/model.rbm"));
    collided = catalog->reservePublication(otherId, outputModel.nativeWritePaths(managedOutput, true),
                                           QStringLiteral("r0"), &error, &errorCode);
    QVERIFY(!collided);
    QCOMPARE(errorCode, QStringLiteral("output_path_conflict"));

    const QStringList planned = outputModel.nativeWritePaths(output, true);
    QCOMPARE(planned.size(), 2);
    auto lease = catalog->reservePublication(id, planned, QStringLiteral("r0"), &error, &errorCode);
    QVERIFY2(lease, qPrintable(error));
    QCOMPARE(lease->persistentDocumentId(), id);
    QCOMPARE(lease->expectedSourceRevision(), QStringLiteral("r0"));
    QCOMPARE(lease->reservedPaths().size(), 2);

    auto secondCatalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(secondCatalog, qPrintable(error));
    QVERIFY(!secondCatalog->recordAccepted(id, QStringLiteral("r1"), true, {}, &error,
                                           QStringLiteral("r0"), &errorCode));
    QCOMPARE(errorCode, QStringLiteral("catalog_locked"));
    const QString competingOutput = temporary.filePath(QStringLiteral("competing.rbm"));
    const auto competingLease = secondCatalog->reservePublication(
        otherId, outputModel.nativeWritePaths(competingOutput, true), QStringLiteral("r0"),
        &error, &errorCode);
    QVERIFY(!competingLease);
    QCOMPARE(errorCode, QStringLiteral("catalog_locked"));

    QVERIFY(!writeNativeModel(output, outputModel, true).isEmpty());
    const auto expectedWrittenBinding = RProjectCatalog::resolveSourceBinding(output, &error, &errorCode);
    QVERIFY2(expectedWrittenBinding.has_value(), qPrintable(error));
    QVERIFY2(catalog->recordSave(id, *expectedWrittenBinding, lease, QStringLiteral("r0"),
                                 &error, &errorCode), qPrintable(error));
    const auto saved = catalog->document(id, &error);
    QVERIFY(saved.has_value());
    QCOMPARE(saved->sourcePath, QFileInfo(output).canonicalFilePath());
    QCOMPARE(saved->resolvedSourcePath, QFileInfo(planned.first()).canonicalFilePath());
    QVERIFY(!saved->dirty);
    lease.reset();

    QVERIFY2(secondCatalog->recordAccepted(id, QStringLiteral("r1"), true, {}, &error,
                                           QStringLiteral("r0"), &errorCode), qPrintable(error));
}

void TestProjectCatalog::recordSaveRejectsChangedWrittenBinding()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("original.rbm"));
    const QString outputDirectory = temporary.filePath(QStringLiteral("output"));
    const QString stagingDirectory = temporary.filePath(QStringLiteral("staging"));
    QVERIFY(QDir().mkpath(outputDirectory));
    QVERIFY(QDir().mkpath(stagingDirectory));
    const QString output = QDir(outputDirectory).filePath(QStringLiteral("published.rbm"));
    const QString stagedOutput = QDir(stagingDirectory).filePath(QStringLiteral("published.rbm"));
    const RModel originalModel = sampleModel(4.0);
    const RModel writtenModel = sampleModel(12.0);
    QVERIFY(!writeNativeModel(source, originalModel).isEmpty());

    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString id = catalog->registerDocument(source, QStringLiteral("Save provenance"), {},
                                                  &error, &errorCode);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    const QStringList outputPaths = writtenModel.nativeWritePaths(output, true);
    const QStringList stagingPaths = writtenModel.nativeWritePaths(stagedOutput, true);
    QCOMPARE(outputPaths.size(), stagingPaths.size());
    const auto lease = catalog->reservePublication(id, outputPaths, QStringLiteral("r0"),
                                                    &error, &errorCode);
    QVERIFY2(lease, qPrintable(error));

    // The worker's staging directory uses the same basename, so its LINK bytes
    // and model hashes are exactly those expected at the final paths.
    QVERIFY(!writeNativeModel(stagedOutput, writtenModel, true).isEmpty());
    const auto stagedBinding = RProjectCatalog::resolveSourceBinding(stagedOutput, &error, &errorCode);
    QVERIFY2(stagedBinding.has_value(), qPrintable(error));
    RProjectCatalog::SourceBinding expectedWrittenBinding = *stagedBinding;
    const QStringList canonicalOutputPaths = lease->reservedPaths();
    QCOMPARE(canonicalOutputPaths.size(), outputPaths.size());
    expectedWrittenBinding.logicalSourcePath = canonicalOutputPaths.last();
    expectedWrittenBinding.resolvedSourcePath = canonicalOutputPaths.first();
    for (auto &artifact : expectedWrittenBinding.sourceArtifacts)
    {
        if (artifact.path == stagedBinding->logicalSourcePath)
            artifact.path = canonicalOutputPaths.last();
        else if (artifact.path == stagedBinding->resolvedSourcePath)
            artifact.path = canonicalOutputPaths.first();
        else
            QFAIL("The staged output source chain contains an unexpected artifact path.");
    }

    QVERIFY(!writeNativeModel(output, writtenModel, true).isEmpty());
    const auto finalBindingBeforeOverwrite = RProjectCatalog::resolveSourceBinding(output, &error, &errorCode);
    QVERIFY2(finalBindingBeforeOverwrite.has_value(), qPrintable(error));
    QVERIFY(*finalBindingBeforeOverwrite == expectedWrittenBinding);
    const QString replacement = temporary.filePath(QStringLiteral("replacement.rbm"));
    QVERIFY(!writeNativeModel(replacement, sampleModel(99.0)).isEmpty());
    QVERIFY(writeBytes(outputPaths.first(), readBytes(replacement)));

    QVERIFY(!catalog->recordSave(id, expectedWrittenBinding, lease, QStringLiteral("r0"),
                                 &error, &errorCode));
    QCOMPARE(errorCode, QStringLiteral("source_conflict"));
    const auto unchanged = catalog->document(id, &error);
    QVERIFY(unchanged.has_value());
    QCOMPARE(unchanged->sourcePath, QFileInfo(source).canonicalFilePath());
    QCOMPARE(unchanged->resolvedSourcePath, QFileInfo(source).canonicalFilePath());
    QCOMPARE(unchanged->savedArtifactHash, hashBytes(readBytes(source)));
    QCOMPARE(unchanged->sourceRevision, QStringLiteral("r0"));
}

void TestProjectCatalog::publicationLeaseCannotCrossWorkspaceRoot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspaceA = temporary.filePath(QStringLiteral("workspace-a"));
    const QString workspaceB = temporary.filePath(QStringLiteral("workspace-b"));
    const QString source = temporary.filePath(QStringLiteral("source.rbm"));
    const QString output = temporary.filePath(QStringLiteral("published.rbm"));
    const RModel model = sampleModel(3.5);
    QVERIFY(!writeNativeModel(source, model).isEmpty());
    QVERIFY(QDir().mkpath(workspaceB));
    QString error, errorCode;
    auto catalogA = RProjectCatalog::open(workspaceA, &error);
    QVERIFY2(catalogA, qPrintable(error));
    const QString id = catalogA->registerDocument(source, QStringLiteral("Root bound"), {}, &error, &errorCode);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QVERIFY(writeBytes(QDir(workspaceB).filePath(QStringLiteral("project.json")),
                       readBytes(QDir(workspaceA).filePath(QStringLiteral("project.json")))));
    auto catalogB = RProjectCatalog::open(workspaceB, &error);
    QVERIFY2(catalogB, qPrintable(error));
    QCOMPARE(catalogB->projectId(), catalogA->projectId());

    const auto lease = catalogA->reservePublication(id, model.nativeWritePaths(output, true),
                                                     QStringLiteral("r0"), &error, &errorCode);
    QVERIFY2(lease, qPrintable(error));
    QVERIFY(!writeNativeModel(output, model, true).isEmpty());
    QVERIFY(!catalogB->recordSave(id, output, lease, QStringLiteral("r0"), &error, &errorCode));
    QCOMPARE(errorCode, QStringLiteral("lease_mismatch"));
}

void TestProjectCatalog::separateCatalogInstancesUseLocalRevisionCas()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.rbm"));
    QVERIFY(!writeNativeModel(source, sampleModel(3.0)).isEmpty());
    QString error, errorCode;
    auto firstCatalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(firstCatalog, qPrintable(error));
    const QString id = firstCatalog->registerDocument(source, QStringLiteral("CAS model"), {}, &error, &errorCode);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    auto secondCatalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(secondCatalog, qPrintable(error));

    QVERIFY2(firstCatalog->recordAccepted(id, QStringLiteral("r1"), true, {}, &error,
                                          QStringLiteral("r0"), &errorCode), qPrintable(error));
    RProjectCatalog::InstanceRef ref{uuid(), uuid(), 7};
    RProjectCatalog::SnapshotArtifact firstArtifact;
    QVERIFY2(firstCatalog->captureSnapshot(id, sampleModel(4.0), ref, &firstArtifact, &error,
                                          QStringLiteral("r1"), &errorCode), qPrintable(error));
    const QString firstPath = QDir(workspace).filePath(firstArtifact.relativePath);
    const QByteArray firstBytes = readBytes(firstPath);

    QVERIFY(!secondCatalog->recordAccepted(id, QStringLiteral("r2"), true, {}, &error,
                                           QStringLiteral("r0"), &errorCode));
    QCOMPARE(errorCode, QStringLiteral("revision_conflict"));
    RProjectCatalog::SnapshotArtifact conflictingArtifact;
    QVERIFY(!secondCatalog->captureSnapshot(id, sampleModel(99.0), ref, &conflictingArtifact,
                                            &error, QStringLiteral("r0"), &errorCode));
    QCOMPARE(errorCode, QStringLiteral("revision_conflict"));
    QCOMPARE(readBytes(firstPath), firstBytes);
    const auto current = firstCatalog->document(id, &error);
    QVERIFY(current.has_value());
    QCOMPARE(current->sourceRevision, QStringLiteral("r1"));
    QCOMPARE(current->modelSha256, firstArtifact.modelSha256);
}

void TestProjectCatalog::documentInstanceLeasePreventsCrossInstanceReloadAndSnapshot()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("saved-model.rbm"));
    QVERIFY(!writeNativeModel(source, sampleModel(6.0)).isEmpty());
    QString error, errorCode;
    auto catalogA = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalogA, qPrintable(error));
    const QString id = catalogA->registerDocument(source, QStringLiteral("Active document"), {},
                                                   &error, &errorCode);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QVERIFY2(catalogA->recordAccepted(id, QStringLiteral("r1"), true, {}, &error,
                                      QStringLiteral("r0"), &errorCode), qPrintable(error));

    RProjectCatalog::InstanceRef refA{uuid(), uuid(), 3};
    auto leaseA = catalogA->acquireDocumentInstance(id, refA, &error, &errorCode);
    QVERIFY2(leaseA, qPrintable(error));
    RProjectCatalog::SnapshotArtifact artifactA;
    QVERIFY2(catalogA->captureSnapshot(id, sampleModel(8.0), refA, &artifactA, &error,
                                      QStringLiteral("r1"), &errorCode, leaseA), qPrintable(error));
    const QString oldSnapshotPath = QDir(workspace).filePath(artifactA.relativePath);
    QVERIFY(QFileInfo::exists(oldSnapshotPath));

    auto catalogB = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalogB, qPrintable(error));
    RProjectCatalog::InstanceRef refB{uuid(), uuid(), 0};
    const auto leaseBWhileAActive = catalogB->acquireDocumentInstance(id, refB, &error, &errorCode);
    QVERIFY(!leaseBWhileAActive);
    QCOMPARE(errorCode, QStringLiteral("document_in_use"));
    RProjectCatalog::SnapshotArtifact forbiddenArtifact;
    QVERIFY(!catalogB->captureSnapshot(id, sampleModel(99.0), refB, &forbiddenArtifact, &error,
                                       QStringLiteral("r1"), &errorCode));
    QCOMPARE(errorCode, QStringLiteral("document_in_use"));
    QVERIFY(!catalogB->captureSnapshot(id, sampleModel(99.0), refB, &forbiddenArtifact, &error,
                                       QStringLiteral("r1"), &errorCode, leaseA));
    QCOMPARE(errorCode, QStringLiteral("instance_mismatch"));

    const QString inFlightOutput = temporary.filePath(QStringLiteral("in-flight-save.rbm"));
    auto publicationLease = catalogA->reservePublication(id, sampleModel(10.0).nativeWritePaths(inFlightOutput, true),
                                                          QStringLiteral("r1"), &error, &errorCode, leaseA);
    QVERIFY2(publicationLease, qPrintable(error));
    leaseA.reset();
    QLockFile instanceProbe(QDir(workspace).filePath(
        QStringLiteral("models/%1/.instance.lock").arg(id)));
    instanceProbe.setStaleLockTime(0);
    QVERIFY(!instanceProbe.tryLock(0));
    const auto whilePublicationHeld = catalogB->acquireDocumentInstance(id, refB, &error, &errorCode);
    QVERIFY(!whilePublicationHeld);
    QCOMPARE(errorCode, QStringLiteral("catalog_locked"));
    publicationLease.reset();
    QVERIFY(instanceProbe.tryLock(0));
    instanceProbe.unlock();
    auto leaseB = catalogB->acquireDocumentInstance(id, refB, &error, &errorCode);
    QVERIFY2(leaseB, qPrintable(error));
    const auto savedBinding = RProjectCatalog::resolveSourceBinding(source, &error, &errorCode);
    QVERIFY2(savedBinding.has_value(), qPrintable(error));
    bool reopenedSavedSource = false;
    QVERIFY2(catalogB->acceptReload(*savedBinding, id, QStringLiteral("r2"), &error,
                                    QStringLiteral("r1"), &errorCode,
                                    &reopenedSavedSource, leaseB), qPrintable(error));
    QVERIFY(reopenedSavedSource);
    const auto reopened = catalogB->document(id, &error);
    QVERIFY(reopened.has_value());
    QVERIFY(!reopened->dirty);
    QCOMPARE(reopened->sourceRevision, QStringLiteral("r2"));
    QCOMPARE(reopened->orphanSnapshots.size(), 1);
    QCOMPARE(reopened->orphanSnapshots.first().toObject()
                 .value(QStringLiteral("relativePath")).toString(), artifactA.relativePath);
    QVERIFY(QFileInfo::exists(oldSnapshotPath));
}

void TestProjectCatalog::newUnsavedDocumentKeepsIdentityOnFirstSaveAs()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString firstSavePath = temporary.filePath(QStringLiteral("untitled.ram"));
    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString id = catalog->createDocument(QStringLiteral("Untitled"), {}, &error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    const QString secondId = catalog->createDocument(QStringLiteral("Untitled"), {}, &error);
    QVERIFY2(!secondId.isEmpty(), qPrintable(error));
    QVERIFY(secondId != id);
    const QString thirdId = catalog->createDocument(QStringLiteral("Draft C"), {}, &error);
    QVERIFY2(!thirdId.isEmpty(), qPrintable(error));
    QVERIFY(thirdId != id && thirdId != secondId);
    const QString copyId = catalog->createCopy(id, QStringLiteral("Untitled"), &error);
    QVERIFY2(!copyId.isEmpty(), qPrintable(error));
    QVERIFY(copyId != id && copyId != secondId && copyId != thirdId);
    QVERIFY2(catalog->renameDocument(thirdId, QStringLiteral("Untitled"), &error), qPrintable(error));
    const auto duplicateLabel = catalog->document(thirdId, &error);
    QVERIFY(duplicateLabel.has_value());
    QCOMPARE(duplicateLabel->label, QStringLiteral("Untitled"));
    QCOMPARE(catalog->document(copyId, &error)->label, QStringLiteral("Untitled"));
    auto unsaved = catalog->document(id, &error);
    QVERIFY(unsaved.has_value());
    QVERIFY(unsaved->sourcePath.isEmpty());
    QVERIFY(unsaved->savedArtifactHash.isEmpty());
    QCOMPARE(unsaved->sourceRevision, QStringLiteral("r0"));
    QVERIFY(unsaved->dirty);
    const RModel firstSavedModel = sampleModel(1.5);
    auto firstSaveLease = catalog->reservePublication(
        id, firstSavedModel.nativeWritePaths(firstSavePath, false), QStringLiteral("r0"), &error);
    QVERIFY2(firstSaveLease, qPrintable(error));
    QVERIFY(!writeNativeModel(firstSavePath, firstSavedModel).isEmpty());
    QVERIFY2(catalog->recordSave(id, firstSavePath, firstSaveLease, QStringLiteral("r0"), &error), qPrintable(error));
    firstSaveLease.reset();
    const auto saved = catalog->document(id, &error);
    QVERIFY(saved.has_value());
    QCOMPARE(saved->persistentDocumentId, id);
    QCOMPARE(saved->sourcePath, QFileInfo(firstSavePath).canonicalFilePath());
    QCOMPARE(saved->savedArtifactHash, hashBytes(readBytes(firstSavePath)));
    QVERIFY(!saved->dirty);
}

void TestProjectCatalog::capabilityManifestExampleParsesAndScopesClaims()
{
    const QJsonObject advertised = RProjectCatalog::capabilityDescription();
    const QJsonObject manifestExample = advertised.value(QStringLiteral("manifestExample")).toObject();
    QVERIFY(!manifestExample.isEmpty());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    QVERIFY(QDir().mkpath(workspace));
    const QString manifestPath = QDir(workspace).filePath(QStringLiteral("project.json"));
    const QByteArray manifestBytes = QJsonDocument(manifestExample).toJson(QJsonDocument::Indented);
    QVERIFY(writeBytes(manifestPath, manifestBytes));

    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    QCOMPARE(catalog->projectId(), manifestExample.value(QStringLiteral("projectId")).toString());
    const QJsonObject query = catalog->query(&error);
    QVERIFY2(!query.isEmpty(), qPrintable(error));
    const QJsonObject capability = query.value(QStringLiteral("capability")).toObject();
    QVERIFY(!capability.isEmpty());
    const QJsonArray documentRows = query.value(QStringLiteral("documents")).toArray();
    const QJsonObject exampleDocuments = manifestExample.value(QStringLiteral("documents")).toObject();
    QCOMPARE(documentRows.size(), exampleDocuments.size());
    QVERIFY(!documentRows.isEmpty());
    QVERIFY(documentRows.first().toObject() == exampleDocuments.begin().value().toObject());
    QCOMPARE(readBytes(manifestPath), manifestBytes);

    const QJsonObject modelScope = capability.value(QStringLiteral("supportedModels")).toObject();
    const QJsonArray artifactTypes = modelScope.value(QStringLiteral("artifactTypes")).toArray();
    QCOMPARE(artifactTypes.size(), 2);
    QCOMPARE(artifactTypes.at(0).toString(), QStringLiteral("MODEL"));
    QCOMPARE(artifactTypes.at(1).toString(), QStringLiteral("LINK"));
    QCOMPARE(modelScope.value(QStringLiteral("solverCompatibility")).toString(),
             QStringLiteral("not evaluated for arbitrary model, element, and analysis combinations"));
    const QJsonObject elementScope = capability.value(QStringLiteral("supportedElements")).toObject();
    QVERIFY(elementScope.value(QStringLiteral("scope")).toString().contains(QStringLiteral("Opaque preservation")));
    QCOMPARE(elementScope.value(QStringLiteral("solverCompatibility")).toString(),
             QStringLiteral("not evaluated; no element formulation allowlist is declared"));
    const QJsonObject analysisScope = capability.value(QStringLiteral("supportedAnalyses")).toObject();
    QCOMPARE(analysisScope.value(QStringLiteral("execution")).toString(),
             QStringLiteral("not provided by the project catalog"));
    QCOMPARE(analysisScope.value(QStringLiteral("numericalAcceptance")).toString(), QStringLiteral("not inferred"));
    const QJsonObject unitConvention = capability.value(QStringLiteral("unitConvention")).toObject();
    QCOMPARE(unitConvention.value(QStringLiteral("system")).toString(), QStringLiteral("SI"));
    QCOMPARE(unitConvention.value(QStringLiteral("length")).toString(), QStringLiteral("m"));
    QCOMPARE(unitConvention.value(QStringLiteral("mass")).toString(), QStringLiteral("kg"));
    QCOMPARE(unitConvention.value(QStringLiteral("time")).toString(), QStringLiteral("s"));
    QCOMPARE(unitConvention.value(QStringLiteral("temperature")).toString(), QStringLiteral("K"));
    QCOMPARE(unitConvention.value(QStringLiteral("angle")).toString(), QStringLiteral("rad"));
    QVERIFY(!unitConvention.value(QStringLiteral("conversion")).toBool());
    QVERIFY(!unitConvention.value(QStringLiteral("numericalAcceptance")).toBool());
    QVERIFY(capability.value(QStringLiteral("unitConventionScope")).toString()
                .contains(QStringLiteral("does not validate physical units")));
    const QJsonObject snapshotExecution = capability.value(QStringLiteral("snapshotExecution")).toObject();
    QCOMPARE(snapshotExecution.value(QStringLiteral("execution")).toString(),
             QStringLiteral("synchronous on the catalog owner thread"));
    QVERIFY(!snapshotExecution.value(QStringLiteral("independentCancellationApi")).toBool());

    catalog.reset();
    QJsonObject unknownFieldManifest = manifestExample;
    unknownFieldManifest.insert(QStringLiteral("futureField"), true);
    const QByteArray unknownFieldBytes = QJsonDocument(unknownFieldManifest).toJson(QJsonDocument::Indented);
    QVERIFY(writeBytes(manifestPath, unknownFieldBytes));
    auto rejected = RProjectCatalog::open(workspace, &error);
    QVERIFY(!rejected);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(manifestPath), unknownFieldBytes);

    QJsonObject unsupportedManifest = manifestExample;
    unsupportedManifest.insert(QStringLiteral("storageVersion"), RProjectCatalog::StorageVersion + 1);
    const QByteArray unsupportedBytes = QJsonDocument(unsupportedManifest).toJson(QJsonDocument::Indented);
    QVERIFY(writeBytes(manifestPath, unsupportedBytes));
    error.clear();
    rejected = RProjectCatalog::open(workspace, &error);
    QVERIFY(!rejected);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(manifestPath), unsupportedBytes);
}

void TestProjectCatalog::corruptAndUnsupportedManifestsArePreserved()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString projectId = catalog->projectId();
    const QString manifestPath = QDir(workspace).filePath(QStringLiteral("project.json"));
    const QJsonObject original = QJsonDocument::fromJson(readBytes(manifestPath)).object();
    QJsonObject unsupported = original;
    unsupported.insert(QStringLiteral("storageVersion"), 2);
    const QByteArray unsupportedBytes = QJsonDocument(unsupported).toJson(QJsonDocument::Indented);
    QVERIFY(writeBytes(manifestPath, unsupportedBytes));
    catalog.reset();
    auto rejected = RProjectCatalog::open(workspace, &error);
    QVERIFY(!rejected);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(manifestPath), unsupportedBytes);

    const QByteArray corruptBytes = QByteArrayLiteral("{not-json");
    QVERIFY(writeBytes(manifestPath, corruptBytes));
    error.clear();
    rejected = RProjectCatalog::open(workspace, &error);
    QVERIFY(!rejected);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(manifestPath), corruptBytes);

    const QByteArray duplicateKeyBytes =
        QByteArrayLiteral("{\"storageVersion\":1,\"storageVersion\":1,\"projectId\":\"")
        + projectId.toUtf8()
        + QByteArrayLiteral("\",\"documents\":{},\"sourcePathIndex\":{}}");
    QVERIFY(writeBytes(manifestPath, duplicateKeyBytes));
    error.clear();
    rejected = RProjectCatalog::open(workspace, &error);
    QVERIFY(!rejected);
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(manifestPath), duplicateKeyBytes);
}

void TestProjectCatalog::lockRejectsConcurrentMutationWithoutManifestChange()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.ram"));
    QVERIFY(!writeNativeModel(source, sampleModel(2.5)).isEmpty());
    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto binding = RProjectCatalog::resolveSourceBinding(source, &error);
    QVERIFY2(binding.has_value(), qPrintable(error));
    const QByteArray before = readBytes(QDir(workspace).filePath(QStringLiteral("project.json")));
    QLockFile externalLock(QDir(workspace).filePath(QStringLiteral(".project.lock")));
    externalLock.setStaleLockTime(0);
    QVERIFY(externalLock.tryLock(0));
    QVERIFY(catalog->registerDocument(*binding, QStringLiteral("Locked"), {}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(QDir(workspace).filePath(QStringLiteral("project.json"))), before);
    externalLock.unlock();
    error.clear();
    QVERIFY(!catalog->registerDocument(*binding, QStringLiteral("Unlocked"), {}, &error).isEmpty());
}

void TestProjectCatalog::snapshotsAreCasBoundHashAddressedAndImmutable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.ram"));
    QVERIFY(!writeNativeModel(source, sampleModel(4.5)).isEmpty());
    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString id = catalog->registerDocument(source, QStringLiteral("Snapshot model"), {}, &error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    const QJsonObject draft{{QStringLiteral("draftId"), uuid()},
                            {QStringLiteral("status"), QStringLiteral("pending")},
                            {QStringLiteral("kind"), QStringLiteral("analysis")},
                            {QStringLiteral("changedFields"), QJsonArray{QStringLiteral("modal.tolerance")}},
                            {QStringLiteral("fieldCount"), 1}};
    QVERIFY2(catalog->recordAccepted(id, QStringLiteral("r1"), true, draft, &error), qPrintable(error));
    RProjectCatalog::InstanceRef ref;
    ref.documentId = uuid();
    ref.sessionEpoch = uuid();
    ref.documentRevision = 23;
    RModel firstModel = sampleModel(1.0);
    RProjectCatalog::SnapshotArtifact first;
    QVERIFY2(catalog->captureSnapshot(id, firstModel, ref, &first, &error, QStringLiteral("r1")), qPrintable(error));
    QCOMPARE(first.projectId, catalog->projectId());
    QCOMPARE(first.persistentDocumentId, id);
    QCOMPARE(first.sourceRevision, QStringLiteral("r1"));
    QCOMPARE(first.instanceRef.documentId, ref.documentId);
    QVERIFY(first.sourceRevision != QString::number(ref.documentRevision));
    QCOMPARE(first.modelSha256.size(), 64);
    QVERIFY(first.relativePath.startsWith(QStringLiteral("models/") + id + QStringLiteral("/snapshots/")));
    const QString firstPath = QDir(workspace).filePath(first.relativePath);
    const auto firstIndexed = catalog->document(id, &error);
    QVERIFY(firstIndexed.has_value());
    QCOMPARE(firstIndexed->snapshotSourceRevision, QStringLiteral("r1"));
    QCOMPARE(firstIndexed->snapshotInstanceRef.value(QStringLiteral("documentRevision")).toInt(), 23);
    const QByteArray firstBytes = readBytes(firstPath);
    QVERIFY(!firstBytes.isEmpty());
    QCOMPARE(hashBytes(firstBytes), first.modelSha256);
    RModel binarySnapshot;
    binarySnapshot.read(firstPath);
    QCOMPARE(binarySnapshot.getNNodes(), 1U);

    RProjectCatalog::SnapshotArtifact repeated;
    QVERIFY2(catalog->captureSnapshot(id, firstModel, ref, &repeated, &error, QStringLiteral("r1")), qPrintable(error));
    QCOMPARE(repeated.modelSha256, first.modelSha256);
    QCOMPARE(readBytes(firstPath), firstBytes);

    QVERIFY2(catalog->recordAccepted(id, QStringLiteral("r2"), true, draft, &error), qPrintable(error));
    const auto afterAccept = catalog->document(id, &error);
    QVERIFY(afterAccept.has_value());
    QVERIFY(afterAccept->modelSha256.isEmpty());
    QVERIFY(afterAccept->snapshotRelativePath.isEmpty());
    QCOMPARE(afterAccept->orphanSnapshots.size(), 1);
    QCOMPARE(afterAccept->orphanSnapshots.first().toObject()
                 .value(QStringLiteral("relativePath")).toString(), first.relativePath);
    QCOMPARE(readBytes(firstPath), firstBytes);

    RModel secondModel = sampleModel(8.0);
    RProjectCatalog::SnapshotArtifact second;
    QVERIFY2(catalog->captureSnapshot(id, secondModel, ref, &second, &error, QStringLiteral("r2")), qPrintable(error));
    QVERIFY(second.modelSha256 != first.modelSha256);
    QVERIFY(second.relativePath != first.relativePath);
    QCOMPARE(readBytes(firstPath), firstBytes);
    const QByteArray secondBytes = readBytes(QDir(workspace).filePath(second.relativePath));
    QCOMPARE(hashBytes(secondBytes), second.modelSha256);
    QVERIFY(!catalog->captureSnapshot(id, firstModel, ref, &repeated, &error, QStringLiteral("r1")));
    QCOMPARE(readBytes(firstPath), firstBytes);
}

void TestProjectCatalog::invalidRuntimeReferencesAreRejected()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.ram"));
    QVERIFY(!writeNativeModel(source, sampleModel(5.5)).isEmpty());
    QString error;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString id = catalog->registerDocument(source, QStringLiteral("Invalid reference"), {}, &error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    const RModel model = sampleModel(1.0);
    RProjectCatalog::SnapshotArtifact artifact;
    RProjectCatalog::InstanceRef ref{QStringLiteral("persistent-id-is-not-runtime-uuid"), uuid(), 0};
    QVERIFY(!catalog->captureSnapshot(id, model, ref, &artifact, &error, QStringLiteral("r0")));
    QVERIFY(!error.isEmpty());
    ref.documentId = uuid();
    ref.sessionEpoch = QStringLiteral("bad-epoch");
    error.clear();
    QVERIFY(!catalog->captureSnapshot(id, model, ref, &artifact, &error, QStringLiteral("r0")));
    QVERIFY(!error.isEmpty());
    ref.sessionEpoch = uuid();
    ref.documentRevision = 9007199254740992ULL;
    error.clear();
    QVERIFY(!catalog->captureSnapshot(id, model, ref, &artifact, &error, QStringLiteral("r0")));
    QVERIFY(!error.isEmpty());
    ref.documentRevision = 4;
    error.clear();
    QVERIFY(!catalog->captureSnapshot(id, model, ref, &artifact, &error, QStringLiteral("r00")));
    QVERIFY(!error.isEmpty());
}

void TestProjectCatalog::durableOperationsSurviveReopenAndRejectIdempotencyConflicts()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    RProjectCatalog::OperationAssociation association;
    association.candidateSha256 = hashBytes(QByteArrayLiteral("candidate"));
    association.approvalSummarySha256 = hashBytes(QByteArrayLiteral("approval summary"));
    const QJsonObject request{{QStringLiteral("commandId"), QStringLiteral("analysis.modal.configure")},
                              {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("modes"), 3}}}};
    const auto first = catalog->beginOperation(QStringLiteral("operation-key-1"), request,
                                               association, {}, &error, &errorCode);
    QCOMPARE(int(first.disposition), int(RProjectCatalog::OperationDisposition::Created));
    QVERIFY2(!first.operation.operationId.isEmpty(), qPrintable(error));
    const QString operationId = first.operation.operationId;
    const QByteArray ledgerBytes = readBytes(QDir(workspace).filePath(QStringLiteral("operations/ledger.json")));
    QVERIFY(!ledgerBytes.isEmpty());
    QVERIFY(!ledgerBytes.contains("approval summary"));
    QVERIFY(!ledgerBytes.contains("approvalToken"));

    catalog.reset();
    catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto repeated = catalog->beginOperation(QStringLiteral("operation-key-1"), request,
                                                 association, {}, &error, &errorCode);
    QCOMPARE(int(repeated.disposition), int(RProjectCatalog::OperationDisposition::Reused));
    QCOMPARE(repeated.operation.operationId, operationId);
    RProjectCatalog::OperationAssociation accepted;
    accepted.acceptedRevision = QStringLiteral("r1");
    accepted.acceptedModelSha256 = hashBytes(QByteArrayLiteral("accepted model"));
    QVERIFY2(catalog->advanceOperation(operationId, RProjectCatalog::OperationPhase::Accepted,
                                       accepted, {}, &error, &errorCode), qPrintable(error));
    catalog.reset();
    catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    QVERIFY2(catalog->markUnfinishedOperationsUnknown({}, &error, &errorCode), qPrintable(error));
    const auto persisted = catalog->queryOperation(operationId, {}, &error, &errorCode);
    QVERIFY2(persisted.has_value(), qPrintable(error));
    QCOMPARE(int(persisted->phase), int(RProjectCatalog::OperationPhase::Unknown));
    QCOMPARE(persisted->association.diagnosticCode, QStringLiteral("interrupted_unobserved"));
    QCOMPARE(persisted->association.acceptedRevision, QStringLiteral("r1"));
    const auto byKey = catalog->queryOperationByIdempotencyKey(QStringLiteral("operation-key-1"),
        {}, &error, &errorCode);
    QVERIFY2(byKey.has_value(), qPrintable(error));
    QCOMPARE(byKey->operationId, operationId);
    const auto page = catalog->queryOperations(1, {}, {}, &error, &errorCode);
    QCOMPARE(page.operations.size(), 1);
    QVERIFY(page.nextCursor.isEmpty());
    const auto recoveredRetry = catalog->beginOperation(QStringLiteral("operation-key-1"), request,
        association, {}, &error, &errorCode);
    QCOMPARE(int(recoveredRetry.disposition), int(RProjectCatalog::OperationDisposition::Reused));
    QCOMPARE(int(recoveredRetry.operation.phase), int(RProjectCatalog::OperationPhase::Unknown));
    RProjectCatalog::OperationAssociation observed;
    observed.observation = {{QStringLiteral("status"), QStringLiteral("accepted")},
        {QStringLiteral("code"), QStringLiteral("ACCEPTED")},
        {QStringLiteral("result"), QJsonObject{
            {QStringLiteral("documentId"), uuid()},
            {QStringLiteral("beforeRevision"), QStringLiteral("r0")},
            {QStringLiteral("afterRevision"), QStringLiteral("r1")},
            {QStringLiteral("changedPaths"), QJsonArray{QStringLiteral("analysis.modal")}}}}};
    QVERIFY2(catalog->advanceOperation(operationId, RProjectCatalog::OperationPhase::Observed,
        observed, {}, &error, &errorCode), qPrintable(error));
    catalog.reset();
    catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto observedOperation = catalog->queryOperation(operationId, {}, &error, &errorCode);
    QVERIFY2(observedOperation.has_value(), qPrintable(error));
    QCOMPARE(observedOperation->association.observation, observed.observation);

    const QJsonObject changedRequest{{QStringLiteral("commandId"), QStringLiteral("analysis.modal.configure")},
        {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("modes"), 4}}}};
    const auto conflict = catalog->beginOperation(QStringLiteral("operation-key-1"), changedRequest,
                                                  association, {}, &error, &errorCode);
    QCOMPARE(int(conflict.disposition), int(RProjectCatalog::OperationDisposition::Conflict));
    QCOMPARE(errorCode, QStringLiteral("idempotency_conflict"));
    QCOMPARE(conflict.operation.operationId, operationId);

    RProjectCatalog::OperationAssociation unsafeObservation;
    unsafeObservation.observation = {{QStringLiteral("status"), QStringLiteral("accepted")},
        {QStringLiteral("result"), QJsonObject{{QStringLiteral("approvalToken"), QStringLiteral("must-not-persist")}}}};
    const auto rejectedSecret = catalog->beginOperation(QStringLiteral("unsafe-observation-key"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("edit")}},
        unsafeObservation, {}, &error, &errorCode);
    QCOMPARE(int(rejectedSecret.disposition), int(RProjectCatalog::OperationDisposition::Conflict));
    QCOMPARE(errorCode, QStringLiteral("invalid_argument"));
    RProjectCatalog::OperationAssociation oversizedObservation;
    oversizedObservation.observation = {{QStringLiteral("status"), QStringLiteral("accepted")},
        {QStringLiteral("result"), QJsonObject{{QStringLiteral("detail"), QString(16 * 1024 + 1, QLatin1Char('x'))}}}};
    const auto rejectedSize = catalog->beginOperation(QStringLiteral("oversized-observation-key"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("edit")}},
        oversizedObservation, {}, &error, &errorCode);
    QCOMPARE(int(rejectedSize.disposition), int(RProjectCatalog::OperationDisposition::Conflict));
    QCOMPARE(errorCode, QStringLiteral("invalid_argument"));
}

void TestProjectCatalog::durableOperationLedgerFailsClosedOnCorruptionAndLock()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto created = catalog->beginOperation(QStringLiteral("ledger-key"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("edit")}}, {}, {}, &error, &errorCode);
    QCOMPARE(int(created.disposition), int(RProjectCatalog::OperationDisposition::Created));
    const QString ledgerPath = QDir(workspace).filePath(QStringLiteral("operations/ledger.json"));
    const QByteArray malformed = QByteArrayLiteral("{\"ledgerVersion\":1");
    QVERIFY(writeBytes(ledgerPath, malformed));
    const auto rejected = catalog->queryOperation(created.operation.operationId, {}, &error, &errorCode);
    QVERIFY(!rejected.has_value());
    QCOMPARE(errorCode, QStringLiteral("corrupt_operation_ledger"));
    QCOMPARE(readBytes(ledgerPath), malformed);

    const QString lockWorkspace = temporary.filePath(QStringLiteral("locked-workspace"));
    auto lockedCatalog = RProjectCatalog::open(lockWorkspace, &error);
    QVERIFY2(lockedCatalog, qPrintable(error));
    QLockFile externalLock(QDir(lockWorkspace).filePath(QStringLiteral(".project.lock")));
    externalLock.setStaleLockTime(0);
    QVERIFY(externalLock.tryLock(0));
    const auto busy = lockedCatalog->beginOperation(QStringLiteral("locked-key"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("edit")}}, {}, {}, &error, &errorCode);
    QCOMPARE(int(busy.disposition), int(RProjectCatalog::OperationDisposition::Conflict));
    QCOMPARE(errorCode, QStringLiteral("catalog_locked"));
    QVERIFY(!QFileInfo::exists(QDir(lockWorkspace).filePath(QStringLiteral("operations/ledger.json"))));
}

void TestProjectCatalog::runtimeLivenessDoesNotRetireAnotherLiveCatalogOperation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    QString error, errorCode;
    auto ownerCatalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(ownerCatalog, qPrintable(error));
    auto recoveryCatalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(recoveryCatalog, qPrintable(error));
    const auto created = ownerCatalog->beginOperation(QStringLiteral("live-runtime-key"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("document.edit")}}, {}, {},
        &error, &errorCode);
    QCOMPARE(int(created.disposition), int(RProjectCatalog::OperationDisposition::Created));
    QCOMPARE(created.operation.ownerRuntimeId, ownerCatalog->runtimeId());

    QVERIFY2(recoveryCatalog->markUnfinishedOperationsUnknown({}, &error, &errorCode), qPrintable(error));
    auto whileLive = recoveryCatalog->queryOperation(created.operation.operationId, {}, &error, &errorCode);
    QVERIFY2(whileLive.has_value(), qPrintable(error));
    QCOMPARE(int(whileLive->phase), int(RProjectCatalog::OperationPhase::Intent));

    ownerCatalog.reset();
    QVERIFY2(recoveryCatalog->markUnfinishedOperationsUnknown({}, &error, &errorCode), qPrintable(error));
    const auto afterOwnerExit = recoveryCatalog->queryOperation(created.operation.operationId,
        {}, &error, &errorCode);
    QVERIFY2(afterOwnerExit.has_value(), qPrintable(error));
    QCOMPARE(int(afterOwnerExit->phase), int(RProjectCatalog::OperationPhase::Unknown));
    QCOMPARE(afterOwnerExit->association.diagnosticCode, QStringLiteral("interrupted_unobserved"));
}

void TestProjectCatalog::operationCrashBoundariesRequireFreshRuntimeReconciliation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const struct Boundary
    {
        QString name;
        RProjectCatalog::OperationPhase beforeRecovery;
        bool hasArtifact;
    } boundaries[] = {
        {QStringLiteral("intent"), RProjectCatalog::OperationPhase::Intent, false},
        {QStringLiteral("accepted"), RProjectCatalog::OperationPhase::Accepted, false},
        {QStringLiteral("artifact_published"), RProjectCatalog::OperationPhase::ArtifactPublished, true},
        {QStringLiteral("observed"), RProjectCatalog::OperationPhase::Observed, true}};

    for (const Boundary &boundary : boundaries)
    {
        const QString workspace = temporary.filePath(QStringLiteral("workspace-%1").arg(boundary.name));
        const QString sideEffectPath = temporary.filePath(QStringLiteral("effect-%1.bin").arg(boundary.name));
        const HelperResult child = runSelfHelper({QStringLiteral("operation-boundary"), boundary.name,
                                                  workspace, sideEffectPath});
        QVERIFY2(child.started && child.finished, qPrintable(QString::fromUtf8(child.error)));
        QCOMPARE(child.exitStatus, QProcess::NormalExit);
        QCOMPARE(child.exitCode, ForcedCrashExitCode);

        QString error, errorCode;
        auto catalog = RProjectCatalog::open(workspace, &error);
        QVERIFY2(catalog, qPrintable(error));
        const QString key = QStringLiteral("crash-boundary-") + boundary.name;
        const auto beforeRecovery = catalog->queryOperationByIdempotencyKey(key, {}, &error, &errorCode);
        QVERIFY2(beforeRecovery.has_value(), qPrintable(error));
        QCOMPARE(int(beforeRecovery->phase), int(boundary.beforeRecovery));
        QVERIFY(beforeRecovery->ownerRuntimeId != catalog->runtimeId());
        if (boundary.hasArtifact)
            QCOMPARE(readBytes(sideEffectPath), QByteArrayLiteral("published test artifact"));
        else
            QVERIFY(!QFileInfo::exists(sideEffectPath));

        const QJsonObject sameRequest{{QStringLiteral("commandId"), QStringLiteral("test.operation")},
                                      {QStringLiteral("phase"), boundary.name}};
        const auto repeated = catalog->beginOperation(key, sameRequest, {}, {}, &error, &errorCode);
        QCOMPARE(int(repeated.disposition), int(RProjectCatalog::OperationDisposition::Reused));
        QCOMPARE(repeated.operation.operationId, beforeRecovery->operationId);
        QCOMPARE(readBytes(sideEffectPath), boundary.hasArtifact
                 ? QByteArrayLiteral("published test artifact") : QByteArray{});

        QVERIFY2(catalog->markUnfinishedOperationsUnknown({}, &error, &errorCode), qPrintable(error));
        const auto recovered = catalog->queryOperation(beforeRecovery->operationId, {}, &error, &errorCode);
        QVERIFY2(recovered.has_value(), qPrintable(error));
        if (boundary.beforeRecovery == RProjectCatalog::OperationPhase::Observed)
        {
            QCOMPARE(int(recovered->phase), int(RProjectCatalog::OperationPhase::Observed));
            QCOMPARE(recovered->association.observation, beforeRecovery->association.observation);
        }
        else
        {
            QCOMPARE(int(recovered->phase), int(RProjectCatalog::OperationPhase::Unknown));
            QCOMPARE(recovered->association.diagnosticCode, QStringLiteral("interrupted_unobserved"));
            QCOMPARE(recovered->association.acceptedRevision,
                     beforeRecovery->association.acceptedRevision);
        }
    }
}

void TestProjectCatalog::crossProcessDuplicateOperationReturnsOriginalObservationWithoutRepeatingSideEffect()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString sideEffectPath = temporary.filePath(QStringLiteral("side-effects.txt"));

    const HelperResult first = runSelfHelper({QStringLiteral("idempotent-operation"), workspace, sideEffectPath});
    QVERIFY2(first.started && first.finished, qPrintable(QString::fromUtf8(first.error)));
    QCOMPARE(first.exitStatus, QProcess::NormalExit);
    QCOMPARE(first.exitCode, 0);
    QJsonObject firstObservation;
    QVERIFY2(parseObject(first.output, firstObservation), qPrintable(QString::fromUtf8(first.output)));

    const QByteArray sideEffectsAfterFirst = readBytes(sideEffectPath);
    QCOMPARE(sideEffectsAfterFirst.count('\n'), 1);
    QVERIFY(!sideEffectsAfterFirst.trimmed().isEmpty());

    const HelperResult second = runSelfHelper({QStringLiteral("idempotent-operation"), workspace, sideEffectPath});
    QVERIFY2(second.started && second.finished, qPrintable(QString::fromUtf8(second.error)));
    QCOMPARE(second.exitStatus, QProcess::NormalExit);
    QCOMPARE(second.exitCode, 0);
    QJsonObject secondObservation;
    QVERIFY2(parseObject(second.output, secondObservation), qPrintable(QString::fromUtf8(second.output)));
    QCOMPARE(secondObservation, firstObservation);
    QCOMPARE(readBytes(sideEffectPath), sideEffectsAfterFirst);

    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto persisted = catalog->queryOperationByIdempotencyKey(
        QStringLiteral("cross-process-idempotency-key"), {}, &error, &errorCode);
    QVERIFY2(persisted.has_value(), qPrintable(error));
    QCOMPARE(int(persisted->phase), int(RProjectCatalog::OperationPhase::Observed));
    QCOMPARE(persisted->association.observation, firstObservation);
}

void TestProjectCatalog::savePublicationReconcilesPartialFilesAndExternalReplacement()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.ram"));
    QVERIFY(!writeNativeModel(source, sampleModel(5.5)).isEmpty());
    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString documentId = catalog->registerDocument(source, QStringLiteral("Save source"), {}, &error);
    QVERIFY2(!documentId.isEmpty(), qPrintable(error));
    const auto document = catalog->document(documentId, &error);
    QVERIFY2(document.has_value(), qPrintable(error));

    const QString modelOutput = temporary.filePath(QStringLiteral("published.rbm"));
    const QString linkOutput = temporary.filePath(QStringLiteral("published.ram"));
    const QByteArray oldModelBytes = QByteArrayLiteral("old model bytes");
    const QByteArray newModelBytes = QByteArrayLiteral("new accepted model bytes");
    const QByteArray newLinkBytes = QByteArrayLiteral("new link bytes");
    QVERIFY(writeBytes(modelOutput, oldModelBytes));
    const QString oldModelHash = hashBytes(oldModelBytes);
    auto lease = catalog->reservePublication(documentId, {modelOutput, linkOutput},
                                             document->sourceRevision, &error, &errorCode);
    QVERIFY2(lease, qPrintable(error));
    RProjectCatalog::OperationArtifactIntent modelIntent;
    modelIntent.path = modelOutput;
    modelIntent.expectedPriorExists = true;
    modelIntent.expectedPriorSha256 = oldModelHash;
    modelIntent.intendedSha256 = hashBytes(newModelBytes);
    RProjectCatalog::OperationArtifactIntent linkIntent;
    linkIntent.path = linkOutput;
    RProjectCatalog::OperationAssociation initialAssociation;
    initialAssociation.candidateSha256 = hashBytes(newModelBytes);
    const QJsonObject normalizedRequest{{QStringLiteral("commandId"), QStringLiteral("document.save")},
        {QStringLiteral("sourceRevision"), document->sourceRevision}};
    const auto intent = catalog->beginSavePublication(documentId, QStringLiteral("save-key-1"),
        normalizedRequest, document->sourceRevision, {modelIntent, linkIntent}, lease,
        initialAssociation, &error, &errorCode);
    QCOMPARE(int(intent.disposition), int(RProjectCatalog::OperationDisposition::Created));
    QVERIFY2(!intent.operation.operationId.isEmpty(), qPrintable(error));
    QVERIFY2(catalog->bindSaveArtifactHash(intent.operation.operationId, linkOutput,
        hashBytes(newLinkBytes), lease, &error, &errorCode), qPrintable(error));
    RProjectCatalog::OperationAssociation accepted;
    accepted.acceptedRevision = QStringLiteral("r1");
    accepted.acceptedModelSha256 = hashBytes(newModelBytes);
    QVERIFY2(catalog->advanceOperation(intent.operation.operationId,
        RProjectCatalog::OperationPhase::Accepted, accepted, lease, &error, &errorCode), qPrintable(error));

    QVERIFY(writeBytes(modelOutput, newModelBytes));
    QVERIFY2(catalog->reconcileSavePublication(intent.operation.operationId, lease, &error, &errorCode), qPrintable(error));
    auto partial = catalog->queryOperation(intent.operation.operationId, lease, &error, &errorCode);
    QVERIFY2(partial.has_value(), qPrintable(error));
    QCOMPARE(int(partial->phase), int(RProjectCatalog::OperationPhase::Unknown));
    QCOMPARE(partial->artifacts.at(0).observedStatus, QStringLiteral("published"));
    QCOMPARE(partial->artifacts.at(1).observedStatus, QStringLiteral("not_published"));

    QVERIFY(writeBytes(linkOutput, newLinkBytes));
    QVERIFY2(catalog->reconcileSavePublication(intent.operation.operationId, lease, &error, &errorCode), qPrintable(error));
    auto complete = catalog->queryOperation(intent.operation.operationId, lease, &error, &errorCode);
    QVERIFY2(complete.has_value(), qPrintable(error));
    QCOMPARE(int(complete->phase), int(RProjectCatalog::OperationPhase::Observed));
    QCOMPARE(complete->artifacts.at(1).observedStatus, QStringLiteral("published"));

    const QByteArray externalBytes = QByteArrayLiteral("external replacement");
    QVERIFY(writeBytes(modelOutput, externalBytes));
    QVERIFY2(catalog->reconcileSavePublication(intent.operation.operationId, lease, &error, &errorCode), qPrintable(error));
    const auto conflicted = catalog->queryOperation(intent.operation.operationId, lease, &error, &errorCode);
    QVERIFY2(conflicted.has_value(), qPrintable(error));
    QCOMPARE(int(conflicted->phase), int(RProjectCatalog::OperationPhase::Conflict));
    QCOMPARE(conflicted->artifacts.at(0).observedStatus, QStringLiteral("conflict"));
    QCOMPARE(readBytes(modelOutput), externalBytes);

    lease.reset();
    const QString unboundOutput = temporary.filePath(QStringLiteral("unbound.rbm"));
    auto unboundLease = catalog->reservePublication(documentId, {unboundOutput},
        document->sourceRevision, &error, &errorCode);
    QVERIFY2(unboundLease, qPrintable(error));
    RProjectCatalog::OperationArtifactIntent unboundIntent;
    unboundIntent.path = unboundOutput;
    const auto unbound = catalog->beginSavePublication(documentId, QStringLiteral("save-key-unbound"),
        QJsonObject{{QStringLiteral("commandId"), QStringLiteral("document.save")},
                    {QStringLiteral("variant"), QStringLiteral("unbound")}},
        document->sourceRevision, {unboundIntent}, unboundLease, {}, &error, &errorCode);
    QCOMPARE(int(unbound.disposition), int(RProjectCatalog::OperationDisposition::Created));
    accepted.acceptedRevision = QStringLiteral("r2");
    QVERIFY2(catalog->advanceOperation(unbound.operation.operationId,
        RProjectCatalog::OperationPhase::Accepted, accepted, unboundLease, &error, &errorCode), qPrintable(error));
    const QByteArray unknownBytes = QByteArrayLiteral("unattributed new file");
    QVERIFY(writeBytes(unboundOutput, unknownBytes));
    QVERIFY2(catalog->reconcileSavePublication(unbound.operation.operationId, unboundLease,
        &error, &errorCode), qPrintable(error));
    const auto unknown = catalog->queryOperation(unbound.operation.operationId,
        unboundLease, &error, &errorCode);
    QVERIFY2(unknown.has_value(), qPrintable(error));
    QCOMPARE(int(unknown->phase), int(RProjectCatalog::OperationPhase::Unknown));
    QCOMPARE(unknown->artifacts.first().observedStatus, QStringLiteral("unknown"));
    QCOMPARE(unknown->artifacts.first().observedSha256, hashBytes(unknownBytes));
    QCOMPARE(readBytes(unboundOutput), unknownBytes);
}

void TestProjectCatalog::partialSavePublicationSurvivesAbruptProcessExit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    const QString source = temporary.filePath(QStringLiteral("source.ram"));
    QVERIFY(!writeNativeModel(source, sampleModel(5.5)).isEmpty());
    QString error, errorCode;
    auto seedCatalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(seedCatalog, qPrintable(error));
    const QString documentId = seedCatalog->registerDocument(source, QStringLiteral("Crash save source"), {}, &error);
    QVERIFY2(!documentId.isEmpty(), qPrintable(error));
    seedCatalog.reset();

    const QString modelPath = temporary.filePath(QStringLiteral("published-model.rbm"));
    const QString linkPath = temporary.filePath(QStringLiteral("published-link.ram"));
    QVERIFY(writeBytes(modelPath, QByteArrayLiteral("old model bytes")));
    const HelperResult child = runSelfHelper({QStringLiteral("save-partial"), workspace, documentId,
                                              modelPath, linkPath});
    QVERIFY2(child.started && child.finished, qPrintable(QString::fromUtf8(child.error)));
    QCOMPARE(child.exitStatus, QProcess::NormalExit);
    QCOMPARE(child.exitCode, ForcedCrashExitCode);
    QCOMPARE(readBytes(modelPath), QByteArrayLiteral("new accepted model bytes"));
    QVERIFY(!QFileInfo::exists(linkPath));

    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto pending = catalog->queryOperationByIdempotencyKey(
        QStringLiteral("crash-save-key"), {}, &error, &errorCode);
    QVERIFY2(pending.has_value(), qPrintable(error));
    QVERIFY(pending->ownerRuntimeId != catalog->runtimeId());
    QCOMPARE(int(pending->phase), int(RProjectCatalog::OperationPhase::Accepted));
    QVERIFY2(catalog->reconcileSavePublication(pending->operationId, {}, &error, &errorCode), qPrintable(error));

    const auto reconciled = catalog->queryOperation(pending->operationId, {}, &error, &errorCode);
    QVERIFY2(reconciled.has_value(), qPrintable(error));
    QCOMPARE(int(reconciled->phase), int(RProjectCatalog::OperationPhase::Unknown));
    QCOMPARE(reconciled->association.diagnosticCode, QStringLiteral("partial_publication"));
    QCOMPARE(reconciled->artifacts.size(), 2);
    QCOMPARE(reconciled->artifacts.at(0).observedStatus, QStringLiteral("published"));
    QCOMPARE(reconciled->artifacts.at(1).observedStatus, QStringLiteral("not_published"));
    QCOMPARE(reconciled->artifacts.at(0).observedSha256,
             hashBytes(QByteArrayLiteral("new accepted model bytes")));
    QVERIFY(!reconciled->artifacts.at(1).observedExists);
    QCOMPARE(readBytes(modelPath), QByteArrayLiteral("new accepted model bytes"));
    QVERIFY(!QFileInfo::exists(linkPath));
}

void TestProjectCatalog::closeRetryQueueSurvivesReopenAndCompletes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    QString error, errorCode;
    auto catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const QString retryId = uuid();
    const QString operationId = uuid();
    const QString journalPath = temporary.filePath(QStringLiteral("close.journal"));
    RModelCommandSession::JournalOptions journalOptions;
    journalOptions.path = journalPath;
    auto session = std::make_unique<RModelCommandSession>(sampleModel(1.0), journalOptions);
    const QJsonObject payload = session->prepareCloseRetry(retryId, operationId, &error);
    QVERIFY2(!payload.isEmpty(), qPrintable(error));
    QVERIFY2(catalog->queueCloseRetry(retryId, operationId, QStringLiteral("journal_write_failed"),
                                      payload, {}, &error, &errorCode), qPrintable(error));
    QCOMPARE(catalog->pendingCloseRetries({}, &error, &errorCode).size(), 1);
    QVERIFY2(session->closeWithRetryPayload(payload, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(journalPath));
    QVERIFY2(catalog->completeCloseRetry(retryId, {}, &error, &errorCode), qPrintable(error));
    QCOMPARE(catalog->pendingCloseRetries({}, &error, &errorCode).size(), 0);

    const QString replayRetryId = uuid();
    const QString replayOperationId = uuid();
    const QString replayJournalPath = temporary.filePath(QStringLiteral("replay-close.journal"));
    RModelCommandSession::JournalOptions replayOptions;
    replayOptions.path = replayJournalPath;
    auto interruptedSession = std::make_unique<RModelCommandSession>(sampleModel(2.0), replayOptions);
    const QJsonObject replayPayload = interruptedSession->prepareCloseRetry(
        replayRetryId, replayOperationId, &error);
    QVERIFY2(!replayPayload.isEmpty(), qPrintable(error));
    QVERIFY2(catalog->queueCloseRetry(replayRetryId, replayOperationId,
        QStringLiteral("journal_write_failed"), replayPayload, {}, &error, &errorCode), qPrintable(error));
    interruptedSession.reset();

    catalog.reset();
    catalog = RProjectCatalog::open(workspace, &error);
    QVERIFY2(catalog, qPrintable(error));
    const auto pending = catalog->pendingCloseRetries({}, &error, &errorCode);
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().retryId, replayRetryId);
    QCOMPARE(pending.first().operationId, replayOperationId);
    QCOMPARE(pending.first().payload, replayPayload);
    QVERIFY2(RModelCommandSession::replayCloseRetry(pending.first().payload, &error, &errorCode), qPrintable(error));
    const QByteArray replayedBytes = readBytes(replayJournalPath);
    QVERIFY(!replayedBytes.isEmpty());
    QVERIFY2(RModelCommandSession::replayCloseRetry(pending.first().payload, &error, &errorCode), qPrintable(error));
    QCOMPARE(readBytes(replayJournalPath), replayedBytes);
    QVERIFY2(catalog->completeCloseRetry(replayRetryId, {}, &error, &errorCode), qPrintable(error));
    QCOMPARE(catalog->pendingCloseRetries({}, &error, &errorCode).size(), 0);
    QVERIFY2(catalog->completeCloseRetry(replayRetryId, {}, &error, &errorCode), qPrintable(error));

    const QString externalRetryId = uuid();
    const QString externalOperationId = uuid();
    const QString externalJournalPath = temporary.filePath(QStringLiteral("external-close.journal"));
    RModelCommandSession::JournalOptions externalOptions;
    externalOptions.path = externalJournalPath;
    auto queuedSession = std::make_unique<RModelCommandSession>(sampleModel(3.0), externalOptions);
    const QJsonObject externalPayload = queuedSession->prepareCloseRetry(
        externalRetryId, externalOperationId, &error);
    QVERIFY2(!externalPayload.isEmpty(), qPrintable(error));
    QVERIFY2(catalog->queueCloseRetry(externalRetryId, externalOperationId,
        QStringLiteral("journal_write_failed"), externalPayload, {}, &error, &errorCode), qPrintable(error));
    queuedSession.reset();
    RModelCommandSession replacementSession(sampleModel(4.0), externalOptions);
    QVERIFY2(replacementSession.close(), qPrintable(error));
    const QByteArray externalBytes = readBytes(externalJournalPath);
    QVERIFY(!externalBytes.isEmpty());
    QVERIFY(!RModelCommandSession::replayCloseRetry(externalPayload, &error, &errorCode));
    QCOMPARE(errorCode, QStringLiteral("journal_conflict"));
    QCOMPARE(readBytes(externalJournalPath), externalBytes);
}

void TestProjectCatalog::closeRetryQueueReplaysAcrossBothProcessCrashWindows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QStringList crashWindows{QStringLiteral("after-queue"), QStringLiteral("after-journal")};
    for (const QString &crashWindow : crashWindows)
    {
        const QString workspace = temporary.filePath(QStringLiteral("workspace-%1").arg(crashWindow));
        const QString journalPath = temporary.filePath(QStringLiteral("journal-%1.json").arg(crashWindow));
        const QString retryId = uuid();
        const QString operationId = uuid();
        const HelperResult child = runSelfHelper({QStringLiteral("close-retry"), workspace, journalPath,
                                                  retryId, operationId, crashWindow});
        QVERIFY2(child.started && child.finished, qPrintable(QString::fromUtf8(child.error)));
        QCOMPARE(child.exitStatus, QProcess::NormalExit);
        QCOMPARE(child.exitCode, ForcedCrashExitCode);

        const QByteArray journalBeforeReplay = readBytes(journalPath);
        if (crashWindow == QStringLiteral("after-queue"))
            QVERIFY(!QFileInfo::exists(journalPath));
        else
        {
            QVERIFY(!journalBeforeReplay.isEmpty());
            QVERIFY(journalBeforeReplay.contains("model.session.close"));
        }

        QString error, errorCode;
        auto catalog = RProjectCatalog::open(workspace, &error);
        QVERIFY2(catalog, qPrintable(error));
        const auto pending = catalog->pendingCloseRetries({}, &error, &errorCode);
        QCOMPARE(pending.size(), 1);
        QCOMPARE(pending.first().retryId, retryId);
        QCOMPARE(pending.first().operationId, operationId);
        QVERIFY2(RModelCommandSession::replayCloseRetry(pending.first().payload, &error, &errorCode),
                 qPrintable(error));
        const QByteArray journalAfterReplay = readBytes(journalPath);
        QVERIFY(!journalAfterReplay.isEmpty());
        QVERIFY2(RModelCommandSession::replayCloseRetry(pending.first().payload, &error, &errorCode),
                 qPrintable(error));
        QCOMPARE(readBytes(journalPath), journalAfterReplay);
        QVERIFY2(catalog->completeCloseRetry(retryId, {}, &error, &errorCode), qPrintable(error));
        QCOMPARE(catalog->pendingCloseRetries({}, &error, &errorCode).size(), 0);
        QVERIFY2(catalog->completeCloseRetry(retryId, {}, &error, &errorCode), qPrintable(error));
        QCOMPARE(readBytes(journalPath), journalAfterReplay);
    }
}

void TestProjectCatalog::cliQueryIsReadonlyJsonOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString workspace = temporary.filePath(QStringLiteral("workspace"));
    CliResult created = runCli({QStringLiteral("--workspace"), workspace, QStringLiteral("--create")});
    QVERIFY2(created.finished, qPrintable(QString::fromUtf8(created.error)));
    QCOMPARE(created.exitStatus, QProcess::NormalExit);
    QCOMPARE(created.exitCode, 0);
    QJsonObject createObject;
    QVERIFY(parseObject(created.output, createObject));
    QCOMPARE(createObject.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("range.project/v1"));
    QCOMPARE(createObject.value(QStringLiteral("created")).toBool(), true);

    const QString manifestPath = QDir(workspace).filePath(QStringLiteral("project.json"));
    const QByteArray before = readBytes(manifestPath);
    CliResult queried = runCli({QStringLiteral("--workspace"), workspace, QStringLiteral("--query")});
    QVERIFY2(queried.finished, qPrintable(QString::fromUtf8(queried.error)));
    QCOMPARE(queried.exitStatus, QProcess::NormalExit);
    QCOMPARE(queried.exitCode, 0);
    QJsonObject queryObject;
    QVERIFY2(parseObject(queried.output, queryObject), qPrintable(QString::fromUtf8(queried.output)));
    QCOMPARE(queryObject.value(QStringLiteral("protocolVersion")).toString(), QStringLiteral("range.project/v1"));
    QCOMPARE(queryObject.value(QStringLiteral("storageVersion")).toInt(), 1);
    QVERIFY(queryObject.value(QStringLiteral("capability")).isObject());
    QCOMPARE(readBytes(manifestPath), before);

    const QString source = temporary.filePath(QStringLiteral("cli-source.ram"));
    QVERIFY(!writeNativeModel(source, sampleModel(6.5)).isEmpty());
    CliResult registered = runCli({QStringLiteral("--workspace"), workspace,
                                   QStringLiteral("--register"), source, QStringLiteral("CLI Beam")});
    QVERIFY2(registered.finished, qPrintable(QString::fromUtf8(registered.error)));
    QCOMPARE(registered.exitCode, 0);
    QJsonObject registerObject;
    QVERIFY(parseObject(registered.output, registerObject));
    const QString persistentId = registerObject.value(QStringLiteral("persistentDocumentId")).toString();
    QVERIFY(!persistentId.isEmpty());
    const QByteArray beforeDocumentQuery = readBytes(manifestPath);
    CliResult documentQuery = runCli({QStringLiteral("--workspace"), workspace,
                                      QStringLiteral("--document"), persistentId});
    QVERIFY2(documentQuery.finished, qPrintable(QString::fromUtf8(documentQuery.error)));
    QCOMPARE(documentQuery.exitCode, 0);
    QJsonObject documentObject;
    QVERIFY(parseObject(documentQuery.output, documentObject));
    QCOMPARE(documentObject.value(QStringLiteral("document")).toObject()
                 .value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
    QCOMPARE(readBytes(manifestPath), beforeDocumentQuery);

    CliResult snapshot = runCli({QStringLiteral("--workspace"), workspace,
                                 QStringLiteral("--snapshot"), source,
                                 QStringLiteral("--document"), persistentId});
    QVERIFY2(snapshot.finished, qPrintable(QString::fromUtf8(snapshot.error)));
    QCOMPARE(snapshot.exitCode, 0);
    QJsonObject snapshotObject;
    QVERIFY(parseObject(snapshot.output, snapshotObject));
    const QJsonObject artifact = snapshotObject.value(QStringLiteral("artifact")).toObject();
    QCOMPARE(artifact.value(QStringLiteral("persistentDocumentId")).toString(), persistentId);
    QCOMPARE(artifact.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r0"));
    QCOMPARE(artifact.value(QStringLiteral("relativePath")).toString().endsWith(QStringLiteral(".rbm")), true);
    QVERIFY(QFileInfo::exists(QDir(workspace).filePath(artifact.value(QStringLiteral("relativePath")).toString())));
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() >= 2 && arguments.at(1) == QStringLiteral("--wf05-helper"))
        return runCrashHelper(arguments.mid(2));
    TestProjectCatalog test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_project_catalog.moc"
