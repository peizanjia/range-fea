#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdlib>

#include <rproject_catalog.h>

#include "application.h"
#include "model_command_host.h"
#include "session.h"

namespace
{
constexpr int abruptExitCode = 86;
constexpr int producerFailureCode = 87;
constexpr auto proofSchema = "range.model-commit-recovery-producer/v1";
constexpr auto preExecuteBoundary = "pre-execute-after-approval";
constexpr auto preObservationBoundary = "post-commit-pre-observation";
constexpr auto postObservationBoundary = "post-observation-return";

QString normalizedPath(const QString &path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
}

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QString::compare(normalizedPath(left), normalizedPath(right), Qt::CaseInsensitive) == 0;
#else
    return normalizedPath(left) == normalizedPath(right);
#endif
}

bool pathIsWithin(const QString &path, const QString &root)
{
    const QString candidatePath = QFileInfo(path).canonicalFilePath();
    const QString rootPath = QFileInfo(root).canonicalFilePath();
    if (candidatePath.isEmpty() || rootPath.isEmpty()) return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString candidate = QDir::fromNativeSeparators(QDir::cleanPath(candidatePath));
    QString base = QDir::fromNativeSeparators(QDir::cleanPath(rootPath));
    if (QString::compare(candidate, base, sensitivity) == 0) return true;
    if (!base.endsWith(QLatin1Char('/'))) base.append(QLatin1Char('/'));
    return candidate.startsWith(base, sensitivity);
}

QString failurePath(const QString &root, const QString &role, const QString &boundary)
{
    const QString safeRole = role == QStringLiteral("producer") || role == QStringLiteral("consumer")
        ? role : QStringLiteral("unknown-role");
    const QString safeBoundary = boundary == QLatin1String(preExecuteBoundary)
            || boundary == QLatin1String(preObservationBoundary)
            || boundary == QLatin1String(postObservationBoundary)
        ? boundary : QStringLiteral("invalid-boundary");
    return QDir(root).filePath(QStringLiteral("failure-%1-%2.json").arg(safeRole, safeBoundary));
}

QString sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool writeJsonAtomically(const QString &path, const QJsonObject &object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() || bytes.size() > 32768) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool writeFailure(const QString &root, const QString &role, const QString &boundary,
                  const QString &stage)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (canonicalRoot.isEmpty() || !pathIsWithin(canonicalRoot, QDir::tempPath())) return false;
    const QString safeRole = role == QStringLiteral("producer") || role == QStringLiteral("consumer")
        ? role : QStringLiteral("unknown-role");
    const QString safeBoundary = boundary == QLatin1String(preExecuteBoundary)
            || boundary == QLatin1String(preObservationBoundary)
            || boundary == QLatin1String(postObservationBoundary)
        ? boundary : QStringLiteral("invalid-boundary");
    const QString safeStage = stage.left(64);
    const QString path = failurePath(canonicalRoot, safeRole, safeBoundary);
    return writeJsonAtomically(path,
        {{QStringLiteral("schema"), QStringLiteral("range.model-commit-recovery-failure/v1")},
         {QStringLiteral("role"), safeRole}, {QStringLiteral("boundary"), safeBoundary},
         {QStringLiteral("stage"), safeStage}});
}

QJsonObject readJsonObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > 32768) return {};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject{};
}

Model modalFixture()
{
    Model model;
    model.setName(QStringLiteral("WF05A02 command commit recovery"));
    model.setNNodes(2);
    model.setNode(0, RNode(0.0, 0.0, 0.0));
    model.setNode(1, RNode(1.0, 0.0, 0.0));
    model.setNElements(1);
    RElement element(R_ELEMENT_TRUSS1);
    element.setNodeId(0, 0);
    element.setNodeId(1, 1);
    model.setElement(0, element);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    auto &modal = model.getProblemSetup().getModalSetup();
    modal.setMethod(R_MODAL_MULTIPLE_MODES);
    modal.setNIterations(12);
    modal.setNModesToExtract(2);
    modal.setConvergenceValue(0.001);
    return model;
}

QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters)
{
    return {{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), operation},
            {QStringLiteral("parameters"), parameters}};
}

QString projectPath(const QString &root)
{
    return QDir(root).filePath(QStringLiteral("project"));
}

QString proofPath(const QString &root, const QString &boundary)
{
    return QDir(root).filePath(QStringLiteral("producer-%1.json").arg(boundary));
}

bool prepareProject(Session *session, const QString &workspace, QString *error)
{
    if (!session || !QDir().mkpath(workspace))
    {
        if (error) *error = QStringLiteral("Could not prepare the bounded project workspace.");
        return false;
    }
    session->clear();
    return session->openProject(workspace, error);
}

QJsonObject createInitialSnapshot(Session *session, uint modelId, QString *error)
{
    QElapsedTimer deadline;
    deadline.start();
    QString localError;
    while (deadline.elapsed() < 10000)
    {
        const QJsonObject snapshot = session->createDocumentSnapshot(modelId, &localError);
        if (!snapshot.isEmpty())
        {
            if (error) error->clear();
            return snapshot;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(10);
    }
    if (error) *error = localError;
    return {};
}

QJsonObject modalPreview(Session *session, const QString &documentId, uint iterations)
{
    const uint modelId = session->findModelByCommandIdentity(documentId);
    if (modelId == RConstants::eod) return {};
    const auto stamp = session->modelCommandStamp(modelId);
    return session->getModelCommandHost()->handle(hostRequest(QStringLiteral("command.preview"),
        {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                                     {QStringLiteral("sessionEpoch"), session->getModelCommandHost()->sessionEpoch()}}},
         {QStringLiteral("expectedDocumentRevision"), double(stamp.second)},
         {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
         {QStringLiteral("commandVersion"), 1},
         {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("method"), QStringLiteral("multiple")},
             {QStringLiteral("iterations"), int(iterations)}, {QStringLiteral("modes"), 2},
             {QStringLiteral("tolerance"), 1e-6}, {QStringLiteral("unit"), QStringLiteral("1")}}}}));
}

QString durablePhaseName(RProjectCatalog::OperationPhase phase)
{
    switch (phase)
    {
        case RProjectCatalog::OperationPhase::Intent: return QStringLiteral("intent");
        case RProjectCatalog::OperationPhase::Accepted: return QStringLiteral("accepted");
        case RProjectCatalog::OperationPhase::ArtifactPublished: return QStringLiteral("artifact_published");
        case RProjectCatalog::OperationPhase::Observed: return QStringLiteral("observed");
        case RProjectCatalog::OperationPhase::Unknown: return QStringLiteral("unknown");
        case RProjectCatalog::OperationPhase::Conflict: return QStringLiteral("conflict");
    }
    return {};
}

bool verifySnapshot(const QString &workspace, const QJsonObject &snapshotRef,
                    const QString &expectedHash, uint expectedIterations,
                    QString *error)
{
    const QString relativePath = snapshotRef.value(QStringLiteral("relativePath")).toString();
    const QString path = QDir(workspace).filePath(relativePath);
    if (relativePath.isEmpty() || !pathIsWithin(path, workspace) || sha256File(path) != expectedHash
        || snapshotRef.value(QStringLiteral("modelSha256")).toString() != expectedHash)
    {
        if (error) *error = QStringLiteral("The persisted model snapshot did not match its accepted hash and workspace.");
        return false;
    }
    try
    {
        Model model;
        model.read(path);
        if (model.getProblemSetup().getModalSetup().getNIterations() != expectedIterations)
        {
            if (error) *error = QStringLiteral("The persisted modal iteration count did not match the expected boundary state.");
            return false;
        }
    }
    catch (...)
    {
        if (error) *error = QStringLiteral("The persisted native model snapshot could not be reopened.");
        return false;
    }
    if (error) error->clear();
    return true;
}

int runProducer(const QString &root, const QString &boundary)
{
    const QString normalizedRoot = QFileInfo(root).canonicalFilePath();
    if (normalizedRoot.isEmpty() || !pathIsWithin(normalizedRoot, QDir::tempPath()))
        return producerFailureCode;
    const auto fail = [&](const QString &stage) {
        writeFailure(normalizedRoot, QStringLiteral("producer"), boundary, stage);
        return producerFailureCode;
    };
    if (boundary != QLatin1String(preExecuteBoundary)
        && boundary != QLatin1String(preObservationBoundary)
        && boundary != QLatin1String(postObservationBoundary)) return fail(QStringLiteral("boundary"));
    if (!samePath(qEnvironmentVariable("RANGE_MODEL_COMMIT_RECOVERY_ROOT"), normalizedRoot))
        return fail(QStringLiteral("root-environment"));

    QString error;
    Session *session = Application::instance()->getSession();
    if (!prepareProject(session, projectPath(normalizedRoot), &error)) return fail(QStringLiteral("project-open"));
    session->addModel(modalFixture());
    const QString documentId = session->modelCommandStamp(0).first;
    const QString persistentId = session->documentProjectContext(0)
        .value(QStringLiteral("persistentDocumentId")).toString();
    if (documentId.isEmpty() || persistentId.isEmpty()) return fail(QStringLiteral("model-registration"));
    const QJsonObject baselineSnapshot = createInitialSnapshot(session, 0, &error);
    if (baselineSnapshot.isEmpty()) return fail(QStringLiteral("baseline-snapshot"));

    const QJsonObject preview = modalPreview(session, documentId, 22);
    if (preview.value(QStringLiteral("status")).toString() != QStringLiteral("preview"))
        return fail(QStringLiteral("preview"));
    const QString previewId = preview.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("previewId")).toString();
    const QString idempotencyKey = QStringLiteral("wf05a02-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    ModelCommandHost *host = session->getModelCommandHost();
    if (previewId.isEmpty()) return fail(QStringLiteral("preview-id"));

    QJsonObject proof{{QStringLiteral("schema"), QString::fromLatin1(proofSchema)},
        {QStringLiteral("boundary"), boundary}, {QStringLiteral("projectPath"), normalizedRoot},
        {QStringLiteral("documentId"), documentId}, {QStringLiteral("persistentDocumentId"), persistentId},
        {QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey},
        {QStringLiteral("baselineSnapshotRef"), baselineSnapshot}};
    if (boundary == QLatin1String(preExecuteBoundary))
    {
        // Persist the proposal identity before the last trusted action. The
        // expected exit code proves approval succeeded; no execute call or
        // operation intent exists in this phase.
        if (!writeJsonAtomically(proofPath(normalizedRoot, boundary), proof))
            return fail(QStringLiteral("pre-execute-proof-write"));
        if (!host->approvePreview(previewId)) return fail(QStringLiteral("preview-approval"));
        std::_Exit(abruptExitCode);
    }
    if (!host->approvePreview(previewId)) return fail(QStringLiteral("preview-approval"));

    if (boundary == QLatin1String(preObservationBoundary))
    {
        // This is an actual synchronous host event emitted by Session's
        // documentRevisionChanged signal after the live candidate commit and
        // before command.execute reaches accepted-snapshot capture/observation.
        QObject::connect(host, &ModelCommandHost::eventPublished, host,
            [session, documentId, proof, normalizedRoot, boundary](const QJsonObject &event) mutable {
                if (event.value(QStringLiteral("type")).toString() != QStringLiteral("document.changed")
                    || event.value(QStringLiteral("documentId")).toString() != documentId) return;
                proof.insert(QStringLiteral("operationId"), event.value(QStringLiteral("operationId")));
                proof.insert(QStringLiteral("revisionEvent"), event);
                proof.insert(QStringLiteral("liveIterationsAtExit"), int(session->getModel(
                    session->findModelByCommandIdentity(documentId)).getProblemSetup()
                        .getModalSetup().getNIterations()));
                if (proof.value(QStringLiteral("operationId")).toString().isEmpty()
                    || !writeJsonAtomically(proofPath(normalizedRoot, boundary), proof))
                {
                    writeFailure(normalizedRoot, QStringLiteral("producer"), boundary,
                                 QStringLiteral("commit-boundary-proof"));
                    std::_Exit(producerFailureCode);
                }
                std::_Exit(abruptExitCode);
            }, Qt::DirectConnection);
    }

    const QJsonObject response = host->handle(hostRequest(QStringLiteral("command.execute"),
        {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey}}));
    if (boundary == QLatin1String(preObservationBoundary))
    {
        Q_UNUSED(response);
        // The synchronous event above must terminate the process before this point.
        return fail(QStringLiteral("commit-boundary-signal-not-reached"));
    }

    const QJsonObject result = response.value(QStringLiteral("result")).toObject();
    if (response.value(QStringLiteral("status")).toString() != QStringLiteral("accepted")
        || response.value(QStringLiteral("code")).toString() != QStringLiteral("OK")
        || result.value(QStringLiteral("persistence")).toString() != QStringLiteral("accepted_snapshot")
        || result.value(QStringLiteral("acceptedSnapshotRef")).toObject().isEmpty())
        return fail(QStringLiteral("accepted-response"));
    QString ledgerError;
    const auto operation = session->getProjectCatalog()->queryOperation(
        result.value(QStringLiteral("operationId")).toString(), {}, &ledgerError);
    if (!operation || operation->phase != RProjectCatalog::OperationPhase::Observed)
        return fail(QStringLiteral("observed-ledger"));
    proof.insert(QStringLiteral("operationId"), result.value(QStringLiteral("operationId")));
    proof.insert(QStringLiteral("response"), response);
    if (!writeJsonAtomically(proofPath(normalizedRoot, boundary), proof))
        return fail(QStringLiteral("producer-proof-write"));

    // command.execute has returned its original accepted observation. The
    // next instruction ends this owner without running destructors.
    std::_Exit(abruptExitCode);
}

bool verifyRawPhase(const QString &workspace, const QString &operationId,
                    const QString &expectedPhase)
{
    const QJsonObject ledger = readJsonObject(QDir(workspace).filePath(
        QStringLiteral("operations/ledger.json")));
    const QJsonArray operations = ledger.value(QStringLiteral("operations")).toArray();
    for (const QJsonValue &value : operations)
    {
        const QJsonObject operation = value.toObject();
        if (operation.value(QStringLiteral("operationId")).toString() == operationId)
            return operation.value(QStringLiteral("phase")).toString() == expectedPhase;
    }
    return false;
}

int runConsumer(const QString &root, const QString &boundary)
{
    const QString normalizedRoot = QFileInfo(root).canonicalFilePath();
    if (normalizedRoot.isEmpty() || !pathIsWithin(normalizedRoot, QDir::tempPath()))
        return producerFailureCode;
    const auto fail = [&](const QString &stage) {
        writeFailure(normalizedRoot, QStringLiteral("consumer"), boundary, stage);
        return producerFailureCode;
    };
    if (boundary != QLatin1String(preExecuteBoundary)
        && boundary != QLatin1String(preObservationBoundary)
        && boundary != QLatin1String(postObservationBoundary)) return fail(QStringLiteral("boundary"));
    if (!samePath(qEnvironmentVariable("RANGE_MODEL_COMMIT_RECOVERY_ROOT"), normalizedRoot))
        return fail(QStringLiteral("root-environment"));
    const QJsonObject proof = readJsonObject(proofPath(normalizedRoot, boundary));
    if (proof.value(QStringLiteral("schema")).toString() != QLatin1String(proofSchema)
        || proof.value(QStringLiteral("boundary")).toString() != boundary
        || !samePath(proof.value(QStringLiteral("projectPath")).toString(), normalizedRoot))
        return fail(QStringLiteral("producer-proof"));
    const QString workspace = projectPath(normalizedRoot);
    const QString operationId = proof.value(QStringLiteral("operationId")).toString();
    const QString idempotencyKey = proof.value(QStringLiteral("idempotencyKey")).toString();
    const QString previewId = proof.value(QStringLiteral("previewId")).toString();
    const QString persistentId = proof.value(QStringLiteral("persistentDocumentId")).toString();
    const bool beforeExecute = boundary == QLatin1String(preExecuteBoundary);
    if (idempotencyKey.isEmpty() || previewId.isEmpty() || persistentId.isEmpty()
        || (beforeExecute ? !operationId.isEmpty() : operationId.isEmpty()))
        return fail(QStringLiteral("producer-proof-identities"));

    const QString expectedRawPhase = boundary == QLatin1String(preObservationBoundary)
        ? QStringLiteral("intent") : QStringLiteral("observed");
    if (!beforeExecute && !verifyRawPhase(workspace, operationId, expectedRawPhase))
        return fail(QStringLiteral("ledger-phase-before-reopen"));

    QString error;
    Session *session = Application::instance()->getSession();
    if (!prepareProject(session, workspace, &error)) return fail(QStringLiteral("project-open"));
    ModelCommandHost *host = session->getModelCommandHost();
    RProjectCatalog *catalog = session->getProjectCatalog();
    if (!catalog) return fail(QStringLiteral("catalog-unavailable"));

    const auto byKey = catalog->queryOperationByIdempotencyKey(idempotencyKey, {}, &error);
    if (beforeExecute)
    {
        if (byKey || !error.isEmpty()) return fail(QStringLiteral("pre-execute-key-record-absent"));
        const auto page = catalog->queryOperations(100, {}, {}, &error);
        if (!error.isEmpty() || !page.operations.isEmpty())
            return fail(QStringLiteral("pre-execute-operation-ledger-empty"));
    }
    else if (!byKey || byKey->operationId != operationId)
        return fail(QStringLiteral("operation-query-by-key"));
    const QString expectedRestartPhase = boundary == QLatin1String(preObservationBoundary)
        ? QStringLiteral("unknown") : QStringLiteral("observed");
    if (!beforeExecute && durablePhaseName(byKey->phase) != expectedRestartPhase)
        return fail(QStringLiteral("restart-phase"));

    QJsonObject operation;
    if (!beforeExecute)
    {
        const QJsonObject operationQuery = host->handle(hostRequest(QStringLiteral("operation.query"),
            {{QStringLiteral("operationId"), operationId}}));
        if (operationQuery.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
            return fail(QStringLiteral("host-operation-query"));
        operation = operationQuery.value(QStringLiteral("result")).toObject();
        if (operation.value(QStringLiteral("phase")).toString() != expectedRestartPhase
            || operation.value(QStringLiteral("operationId")).toString() != operationId)
            return fail(QStringLiteral("host-operation-query-result"));
    }

    const auto document = catalog->document(persistentId, &error);
    if (!document || document->tombstone) return fail(QStringLiteral("project-document-query"));
    const QJsonObject expectedSnapshot = boundary != QLatin1String(postObservationBoundary)
        ? proof.value(QStringLiteral("baselineSnapshotRef")).toObject()
        : proof.value(QStringLiteral("response")).toObject().value(QStringLiteral("result")).toObject()
              .value(QStringLiteral("acceptedSnapshotRef")).toObject();
    const uint expectedIterations = boundary == QLatin1String(postObservationBoundary) ? 22u : 12u;
    const QString expectedHash = expectedSnapshot.value(QStringLiteral("modelSha256")).toString();
    if (expectedSnapshot.isEmpty() || document->modelSha256 != expectedHash
        || document->snapshotRelativePath != expectedSnapshot.value(QStringLiteral("relativePath")).toString()
        || !verifySnapshot(workspace, expectedSnapshot, expectedHash, expectedIterations, &error))
        return fail(QStringLiteral("accepted-snapshot-verification"));

    if (beforeExecute)
    {
        if (host->approvePreview(previewId)) return fail(QStringLiteral("pre-execute-old-approval-reused"));
        const QJsonObject rejectedReplay = host->handle(hostRequest(QStringLiteral("command.execute"),
            {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey}}));
        if (rejectedReplay.value(QStringLiteral("status")).toString() != QStringLiteral("rejected")
            || rejectedReplay.value(QStringLiteral("code")).toString() != QStringLiteral("PREVIEW_EXPIRED"))
            return fail(QStringLiteral("pre-execute-old-preview-reused"));
        const auto afterAttempt = catalog->queryOperationByIdempotencyKey(idempotencyKey, {}, &error);
        const auto afterAttemptPage = catalog->queryOperations(100, {}, {}, &error);
        if (afterAttempt || !error.isEmpty() || !afterAttemptPage.operations.isEmpty())
            return fail(QStringLiteral("pre-execute-replay-created-operation"));
    }
    else if (boundary == QLatin1String(preObservationBoundary))
    {
        if (proof.value(QStringLiteral("liveIterationsAtExit")).toInt() != 22
            || operation.value(QStringLiteral("acceptedRevision")).toString().size() != 0
            || operation.value(QStringLiteral("acceptedModelSha256")).toString().size() != 0
            || !operation.value(QStringLiteral("observation")).toObject().isEmpty())
            return fail(QStringLiteral("unknown-outcome-assertions"));
        if (host->approvePreview(previewId)) return fail(QStringLiteral("old-approval-reused"));
        const QJsonObject replay = host->handle(hostRequest(QStringLiteral("command.execute"),
            {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey}}));
        if (replay.value(QStringLiteral("status")).toString() != QStringLiteral("rejected")
            || replay.value(QStringLiteral("code")).toString() != QStringLiteral("OPERATION_RECONCILE_REQUIRED"))
            return fail(QStringLiteral("unknown-operation-replay"));
    }
    else
    {
        const QJsonObject originalResponse = proof.value(QStringLiteral("response")).toObject();
        const QJsonObject originalResult = originalResponse.value(QStringLiteral("result")).toObject();
        const QJsonObject originalObservation = byKey->association.observation;
        if (operation.value(QStringLiteral("acceptedModelSha256")).toString() != expectedHash
            || operation.value(QStringLiteral("acceptedRevision")).toString()
                != expectedSnapshot.value(QStringLiteral("sourceRevision")).toString()
            || originalObservation.value(QStringLiteral("status")).toString() != QStringLiteral("accepted")
            || originalObservation.value(QStringLiteral("code")).toString() != QStringLiteral("OK")
            || originalObservation.value(QStringLiteral("result")).toObject() != originalResult
            || host->approvePreview(previewId))
            return fail(QStringLiteral("observed-outcome-assertions"));
        const QJsonObject replay = host->handle(hostRequest(QStringLiteral("command.execute"),
            {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey}}));
        if (replay.value(QStringLiteral("status")) != originalResponse.value(QStringLiteral("status"))
            || replay.value(QStringLiteral("code")) != originalResponse.value(QStringLiteral("code"))
            || replay.value(QStringLiteral("result")).toObject() != originalResult)
            return fail(QStringLiteral("observed-operation-replay"));
    }

    if (!beforeExecute)
    {
        QString afterReplayError;
        const auto afterReplay = catalog->queryOperation(operationId, {}, &afterReplayError);
        if (!afterReplay || durablePhaseName(afterReplay->phase) != expectedRestartPhase
            || afterReplay->association.observation != byKey->association.observation
            || afterReplay->association.acceptedModelSha256 != byKey->association.acceptedModelSha256)
            return fail(QStringLiteral("idempotent-replay-stability"));
    }
    const auto finalDocument = catalog->document(persistentId, &error);
    if (!finalDocument || finalDocument->tombstone
        || finalDocument->sourceRevision != expectedSnapshot.value(QStringLiteral("sourceRevision")).toString()
        || finalDocument->snapshotSourceRevision != expectedSnapshot.value(QStringLiteral("sourceRevision")).toString()
        || finalDocument->modelSha256 != expectedHash
        || finalDocument->snapshotRelativePath != expectedSnapshot.value(QStringLiteral("relativePath")).toString()
        || !verifySnapshot(workspace, expectedSnapshot, expectedHash, expectedIterations, &error))
        return fail(QStringLiteral("post-replay-protected-snapshot-stability"));
    return 0;
}

QProcessEnvironment childEnvironment(const QString &root)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &name : environment.keys())
    {
        const QString upper = name.toUpper();
        if (upper.contains(QStringLiteral("KEY")) || upper.contains(QStringLiteral("TOKEN"))
            || upper.contains(QStringLiteral("SECRET")) || upper.contains(QStringLiteral("PASSWORD"))
            || upper.contains(QStringLiteral("CREDENTIAL")) || upper.contains(QStringLiteral("AUTH")))
            environment.remove(name);
    }
    environment.insert(QStringLiteral("RANGE_MODEL_COMMIT_RECOVERY_ROOT"), root);
    return environment;
}

class RetainRootOnFailure
{
public:
    explicit RetainRootOnFailure(QTemporaryDir *directory) : directory(directory) {}
    ~RetainRootOnFailure()
    {
        if (directory && !succeeded)
        {
            directory->setAutoRemove(false);
            qInfo().noquote() << "WF05A02 failure artifacts retained at:"
                              << directory->path();
        }
    }
    void markSucceeded() { succeeded = true; }

private:
    QTemporaryDir *directory = nullptr;
    bool succeeded = false;
};
}

class TestModelCommitRecovery : public QObject
{
    Q_OBJECT
private slots:
    void restartClassifiesPreExecutePostCommitAndObservedBoundaries()
    {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/range-wf05a02-model-commit-XXXXXX"));
        QVERIFY(directory.isValid());
        RetainRootOnFailure retainRoot(&directory);
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        QVERIFY(!root.isEmpty());
        QVERIFY(pathIsWithin(root, QDir::tempPath()));

        auto runChild = [&](const QString &scenarioRoot, const QString &role,
                            const QString &boundary, int expectedCode) {
            QProcess child;
            child.setProcessEnvironment(childEnvironment(scenarioRoot));
            child.setProgram(QCoreApplication::applicationFilePath());
            child.setArguments({QStringLiteral("--model-commit-recovery"), role, scenarioRoot, boundary});
            child.setStandardOutputFile(QProcess::nullDevice());
            child.setStandardErrorFile(QProcess::nullDevice());
            child.start();
            if (!child.waitForStarted(10000))
            {
                writeFailure(scenarioRoot, role == QStringLiteral("--producer")
                    ? QStringLiteral("producer") : QStringLiteral("consumer"), boundary,
                    QStringLiteral("child-start"));
                qWarning().noquote() << "WF05A02 child start failed at" << role << boundary;
                return false;
            }
            QElapsedTimer deadline;
            deadline.start();
            while (child.state() != QProcess::NotRunning && deadline.elapsed() < 60000)
            {
                child.waitForFinished(20);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            if (child.state() != QProcess::NotRunning)
            {
                child.kill();
                child.waitForFinished(10000);
                const QString roleName = role == QStringLiteral("--producer")
                    ? QStringLiteral("producer") : QStringLiteral("consumer");
                writeFailure(scenarioRoot, roleName, boundary, QStringLiteral("child-timeout"));
                qWarning().noquote() << "WF05A02 child timed out at" << roleName << boundary;
                return false;
            }
            if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != expectedCode)
            {
                const QString roleName = role == QStringLiteral("--producer")
                    ? QStringLiteral("producer") : QStringLiteral("consumer");
                const QJsonObject facts = readJsonObject(failurePath(scenarioRoot, roleName, boundary));
                const QString stage = facts.value(QStringLiteral("stage")).toString();
                qWarning().noquote() << "WF05A02 child failed at" << roleName << boundary
                    << (stage.isEmpty() ? QStringLiteral("unspecified-stage") : stage)
                    << "exit code" << child.exitCode();
                return false;
            }
            return true;
        };

        auto runBoundary = [&](const QString &boundary) {
            const QString scenarioRoot = QDir(root).filePath(boundary);
            if (!QDir().mkpath(scenarioRoot) || !pathIsWithin(scenarioRoot, root))
            {
                qWarning().noquote() << "WF05A02 could not prepare bounded subroot for" << boundary;
                return false;
            }
            return runChild(scenarioRoot, QStringLiteral("--producer"), boundary, abruptExitCode)
                && runChild(scenarioRoot, QStringLiteral("--consumer"), boundary, 0);
        };

        QVERIFY(runBoundary(QString::fromLatin1(preExecuteBoundary)));
        QVERIFY(runBoundary(QString::fromLatin1(preObservationBoundary)));
        QVERIFY(runBoundary(QString::fromLatin1(postObservationBoundary)));
        retainRoot.markSucceeded();
    }
};

int runModelCommitRecoveryTests(int argc, char **argv)
{
    if (argc >= 4 && QByteArray(argv[1]) == QByteArrayLiteral("--producer"))
        return runProducer(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    if (argc >= 4 && QByteArray(argv[1]) == QByteArrayLiteral("--consumer"))
        return runConsumer(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    TestModelCommitRecovery test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_model_commit_recovery.moc"
