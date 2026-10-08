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
#include <rbl_error.h>
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

QTEST_GUILESS_MAIN(TestProjectCatalog)
#include "tst_project_catalog.moc"
