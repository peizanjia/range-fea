#include "model_command_host.h"
#include "mesh_job_controller.h"

#include "application.h"
#include "model_io.h"
#include "session.h"
#include "solver_manager.h"
#include "trusted_artifact_paths.h"
#include <rmodel_command_service.h>
#include <rmodel_command_session.h>
#include <rproject_catalog.h>
#include <rtrusted_experiment_service.h>
#include <rtrusted_convergence_service.h>
#include <rtrusted_mechanics_service.h>
#include <rtrusted_verification_service.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <rml_condition.h>
#include <rml_element_group.h>
#include <rml_material.h>
#include <rml_material_property.h>
#include <QCryptographicHash>
#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QScopeGuard>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <limits>
#include <map>

namespace
{
// The controller retains file paths and worker requests as trusted host data.
// Model-facing previews expose the same scientific receipt without transport paths.
QJsonObject meshReceipt(QJsonObject receipt)
{
    receipt.remove("request"); receipt.remove("frozenRequest");
    const auto capture = receipt.take("capture").toObject();
    if (!capture.isEmpty())
        for (const QString &field : {QStringLiteral("sourceRuntimeStamp"), QStringLiteral("sourceModelSha256"),
                                    QStringLiteral("supportedSourceStateSha256"), QStringLiteral("physicalIds")})
            receipt.insert(field, capture.value(field));
    auto descriptor = receipt.value("descriptor").toObject();
    descriptor.remove("frozenRequest");
    for (auto it = descriptor.begin(); it != descriptor.end();)
        if (it.key().endsWith("Path") || it.key().endsWith("Directory")) it = descriptor.erase(it);
        else ++it;
    if (receipt.contains("descriptor")) receipt.insert("descriptor", descriptor);
    return receipt;
}

QString durableOperationPhase(RProjectCatalog::OperationPhase phase)
{
    switch (phase)
    {
        case RProjectCatalog::OperationPhase::Intent: return QStringLiteral("intent");
        case RProjectCatalog::OperationPhase::Accepted: return QStringLiteral("accepted");
        case RProjectCatalog::OperationPhase::ArtifactPublished: return QStringLiteral("artifact_published");
        case RProjectCatalog::OperationPhase::Observed: return QStringLiteral("observed");
        case RProjectCatalog::OperationPhase::Conflict: return QStringLiteral("conflict");
        case RProjectCatalog::OperationPhase::Unknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QJsonObject durableOperationJson(const RProjectCatalog::DurableOperation &operation)
{
    return {{"projectId", operation.projectId}, {"operationId", operation.operationId},
            {"phase", durableOperationPhase(operation.phase)}, {"persistence", "durable"},
            {"normalizedRequestSha256", operation.normalizedRequestSha256},
            {"acceptedRevision", operation.association.acceptedRevision},
            {"acceptedModelSha256", operation.association.acceptedModelSha256},
            {"diagnosticCode", operation.association.diagnosticCode},
            {"observation", operation.association.observation}};
}

bool sensitiveObservationKey(const QString &key)
{
    QString normalized = key.toCaseFolded();
    normalized.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return normalized.contains(QStringLiteral("token"))
        || normalized.contains(QStringLiteral("secret"))
        || normalized.contains(QStringLiteral("credential"))
        || normalized.contains(QStringLiteral("password"))
        || normalized.contains(QStringLiteral("apikey"))
        || normalized == QStringLiteral("auth")
        || normalized.contains(QStringLiteral("authorization"))
        || normalized.contains(QStringLiteral("authheader"))
        || normalized.contains(QStringLiteral("cookie"));
}

QJsonValue durableObservationValue(const QJsonValue &value)
{
    if (value.isArray()) {
        QJsonArray sanitized;
        for (const QJsonValue &entry : value.toArray())
            sanitized.append(durableObservationValue(entry));
        return sanitized;
    }
    if (value.isObject()) {
        QJsonObject sanitized;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            if (!sensitiveObservationKey(it.key()))
                sanitized.insert(it.key(), durableObservationValue(it.value()));
        return sanitized;
    }
    return value;
}

QString modelArtifact(QString path, QString *error = nullptr)
{
    QSet<QString> visited;
    try
    {
        for (int depth = 0; depth < 16; ++depth)
        {
            path = QFileInfo(path).canonicalFilePath();
            if (path.isEmpty() || visited.contains(path)) break;
            visited.insert(path);
            const QString extension = QFileInfo(path).suffix();
            const bool ascii = extension == RModel::getDefaultFileExtension(false);
            if (!ascii && extension != RModel::getDefaultFileExtension(true)) break;
            RFile file(path, ascii ? RFile::ASCII : RFile::BINARY);
            if (!file.open(ascii ? QIODevice::ReadOnly | QIODevice::Text : QIODevice::ReadOnly)) break;
            RFileHeader header;
            if (ascii) RFileIO::readAscii(file, header); else RFileIO::readBinary(file, header);
            if (header.getType() == R_FILE_TYPE_MODEL) return path;
            if (header.getType() != R_FILE_TYPE_LINK) break;
            path = RFileUtils::findLinkTargetFileName(path, header.getInformation());
        }
    }
    catch (const RError &failure)
    {
        if (error) *error = failure.getMessage();
        return {};
    }
    if (error) *error = QStringLiteral("Cannot resolve the complete native model artifact.");
    return {};
}

bool isDirectModelArtifact(const QString &path, QString *error = nullptr)
{
    if (error) error->clear();
    const QString extension = QFileInfo(path).suffix();
    const bool ascii = extension == RModel::getDefaultFileExtension(false);
    if (!ascii && extension != RModel::getDefaultFileExtension(true))
    {
        if (error) *error = QStringLiteral("The frozen candidate snapshot has an unsupported model extension.");
        return false;
    }
    RFile file(path, ascii ? RFile::ASCII : RFile::BINARY);
    if (!file.open(ascii ? QIODevice::ReadOnly | QIODevice::Text : QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("The frozen candidate snapshot could not be opened as a model artifact.");
        return false;
    }
    RFileHeader header;
    if (ascii) RFileIO::readAscii(file, header); else RFileIO::readBinary(file, header);
    if (header.getType() != R_FILE_TYPE_MODEL)
    {
        if (error) *error = QStringLiteral("The frozen candidate snapshot is not a direct model artifact.");
        return false;
    }
    return true;
}

QJsonObject rejected(const QString &code, const QString &message)
{
    return {{"apiVersion", "range.application/v1"}, {"status", "rejected"}, {"code", code}, {"message", message}};
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) return false;
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.contains(it.key())) return false;
    return true;
}

bool validToken(const QJsonValue &value, int maximum = 128)
{
    if (!value.isString()) return false;
    const QString token = value.toString();
    if (token.isEmpty() || token.size() > maximum) return false;
    static const QRegularExpression grammar(QStringLiteral("^[A-Za-z0-9._:-]+$"));
    return grammar.match(token).hasMatch();
}

bool unsignedInteger(const QJsonValue &value, quint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    // JSON numbers cannot exactly represent integers beyond this boundary.
    constexpr double maxExactJsonInteger = 9007199254740991.0;
    if (!std::isfinite(number) || number < 0.0 || number > maxExactJsonInteger || std::floor(number) != number)
        return false;
    if (result) *result = quint64(number);
    return true;
}

QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isCoreEditCommand(const QString &command)
{
    return command == QStringLiteral("analysis.modal.configure")
        || command == QStringLiteral("analysis.structural_dynamics.configure")
        || command == QStringLiteral("entity.material.set")
        || command == QStringLiteral("entity.boundary.set")
        || command == QStringLiteral("geometry.node_coordinates.set")
        || command == QStringLiteral("geometry.rigid_transform")
        || command == QStringLiteral("mesh.rebind.static")
        || command == QStringLiteral("piezo.configure-experiment");
}

RElementGroup *mutableElementGroup(RModel &model, uint physicalId)
{
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    if (!model.findElementGroupByPhysicalId(physicalId, &type, &index)) return nullptr;
    switch (type)
    {
        case R_ENTITY_GROUP_POINT: return &model.getPoint(index);
        case R_ENTITY_GROUP_LINE: return &model.getLine(index);
        case R_ENTITY_GROUP_SURFACE: return &model.getSurface(index);
        case R_ENTITY_GROUP_VOLUME: return &model.getVolume(index);
        default: return nullptr;
    }
}

const RElementGroup *constElementGroup(const RModel &model, uint physicalId)
{
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    if (!model.findElementGroupByPhysicalId(physicalId, &type, &index)) return nullptr;
    switch (type)
    {
        case R_ENTITY_GROUP_POINT: return &model.getPoint(index);
        case R_ENTITY_GROUP_LINE: return &model.getLine(index);
        case R_ENTITY_GROUP_SURFACE: return &model.getSurface(index);
        case R_ENTITY_GROUP_VOLUME: return &model.getVolume(index);
        default: return nullptr;
    }
}

bool commitSupportedCandidate(RModel &liveModel, const RModel &candidate,
                             const RModelCommandService::Delta &delta,
                             QString *error = nullptr)
{
    switch (delta.kind)
    {
        case RModelCommandService::Delta::Kind::MeshRebind:
        {
            const QString acceptedHash = delta.meshRebind.afterStateSha256;
            if (acceptedHash.isEmpty()
                || RModelCommandService::stateFingerprint(liveModel) != delta.meshRebind.beforeStateSha256
                || RModelCommandService::stateFingerprint(candidate) != acceptedHash)
            {
                if (error) *error = QObject::tr("Static binding publication no longer matches the approved source and candidate.");
                return false;
            }
            return liveModel.applyStaticMeshBindingCandidate(candidate,
                [&](const RModel &prepared) {
                    return RModelCommandService::stateFingerprint(prepared) == acceptedHash;
                }, error);
        }
        case RModelCommandService::Delta::Kind::Modal:
            liveModel.getProblemSetup().setModalSetup(
                candidate.getProblemSetup().getModalSetup());
            return true;
        case RModelCommandService::Delta::Kind::StructuralDynamics:
            liveModel.getProblemSetup().setStructuralDynamicsSetup(
                candidate.getProblemSetup().getStructuralDynamicsSetup());
            liveModel.setTimeSolver(candidate.getTimeSolver());
            return true;
        case RModelCommandService::Delta::Kind::Material:
        case RModelCommandService::Delta::Kind::Boundary:
        {
            RElementGroup *liveGroup = mutableElementGroup(liveModel, delta.physicalId);
            const RElementGroup *candidateGroup = constElementGroup(candidate, delta.physicalId);
            if (!liveGroup || !candidateGroup)
            {
                if (error) *error = QObject::tr("The approved physical target no longer exists.");
                return false;
            }
            if (delta.kind == RModelCommandService::Delta::Kind::Material)
            {
                liveGroup->setMaterial(candidateGroup->getMaterial());
                return true;
            }
            const bool candidateHas = candidateGroup->hasBoundaryCondition(delta.boundaryType);
            if (candidateHas)
                liveGroup->addBoundaryCondition(candidateGroup->getBoundaryCondition(delta.boundaryType));
            else if (liveGroup->hasBoundaryCondition(delta.boundaryType))
                liveGroup->removeBoundaryCondition(delta.boundaryType);
            return true;
        }
        case RModelCommandService::Delta::Kind::Geometry:
        {
            // Resolve all stable IDs before publishing; preserve the GUI Model subclass.
            QList<QPair<uint, uint>> positions;
            for (const auto &node : delta.geometry.nodes)
            {
                const uint live = liveModel.findNodePositionByPhysicalId(node.physicalNodeId);
                const uint proposed = candidate.findNodePositionByPhysicalId(node.physicalNodeId);
                if (live >= liveModel.getNNodes() || proposed >= candidate.getNNodes())
                {
                    if (error) *error = QObject::tr("The approved physical node no longer exists.");
                    return false;
                }
                positions.append(qMakePair(live, proposed));
            }
            for (const auto &position : positions)
            {
                const auto &node = candidate.getNode(position.second);
                liveModel.getNode(position.first).set(node.getX(), node.getY(), node.getZ());
            }
            return true;
        }
        case RModelCommandService::Delta::Kind::None:
            break;
    }
    if (error) *error = QObject::tr("The approved command did not contain a supported model change.");
    return false;
}

bool commitSupportedDelta(RModel &liveModel, const RModelCommandService::Delta &delta,
                          QString *error = nullptr)
{
    const auto evaluated = RModelCommandService::applyDelta(liveModel, delta, true);
    if (!evaluated.valid)
    {
        if (error) *error = evaluated.message;
        return false;
    }
    return commitSupportedCandidate(liveModel, evaluated.candidate, delta, error);
}

bool activeModalInteraction()
{
    return QApplication::activeModalWidget() != nullptr;
}

QJsonObject sourceStampJson(const QPair<QString, quint64> &stamp, const QString &sessionEpoch)
{
    return {{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("revision"), QString::number(stamp.second)},
            {QStringLiteral("sessionEpoch"), sessionEpoch}};
}

QJsonObject nativeSourceBindingJson(const RProjectCatalog::SourceBinding &binding)
{
    QJsonArray artifacts;
    for (const auto &artifact : binding.sourceArtifacts) artifacts.append(artifact.toJson());
    return {{QStringLiteral("logicalSourcePath"), binding.logicalSourcePath},
            {QStringLiteral("resolvedSourcePath"), binding.resolvedSourcePath},
            {QStringLiteral("savedArtifactHash"), binding.savedArtifactHash},
            {QStringLiteral("sourceArtifacts"), artifacts}};
}

bool nativeSourceFilesMatch(const RProjectCatalog::SourceBinding &binding)
{
    if (binding.sourceArtifacts.isEmpty()) return false;
    for (const auto &artifact : binding.sourceArtifacts)
    {
        QFile file(artifact.path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file) || QString::fromLatin1(hash.result().toHex())!=artifact.sha256) return false;
    }
    return true;
}

QJsonObject hostResponse(const QJsonValue &requestId, const QString &status, const QString &code,
                         const QJsonObject &result = {})
{
    QJsonObject response{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                         {QStringLiteral("requestId"), requestId},
                         {QStringLiteral("status"), status},
                         {QStringLiteral("code"), code}};
    if (!result.isEmpty()) response.insert(QStringLiteral("result"), result);
    return response;
}

QJsonObject hostRejected(const QJsonValue &requestId, const QString &code, const QString &message)
{
    QJsonObject response = hostResponse(requestId, QStringLiteral("rejected"), code);
    response.insert(QStringLiteral("message"), message);
    return response;
}

class TrySessionLock
{
public:
    explicit TrySessionLock(Session *session) : session(session), locked(session && session->trylock(0)) {}
    ~TrySessionLock() { if (locked) session->unlock(); }
    TrySessionLock(const TrySessionLock &) = delete;
    TrySessionLock &operator=(const TrySessionLock &) = delete;
    bool ownsLock() const { return locked; }
    void unlock() { if (locked) { session->unlock(); locked = false; } }
private:
    Session *session;
    bool locked;
};

RTrustedExperimentService::SourceIdentity experimentSource(Session *session, const QString &epoch,
                                                         const QString &documentId)
{
    TrySessionLock guard(session);
    if (!guard.ownsLock() || ModelCommandHost::isMutationBlocked()) return {};
    const uint id = session->findModelByCommandIdentity(documentId);
    if (id == RConstants::eod) return {};
    const auto stamp = session->modelCommandStamp(id);
    RTrustedExperimentService::SourceIdentity source;
    source.workspaceId = session->projectWorkspace();
    source.sessionId = session->getID();
    source.sessionEpoch = epoch;
    source.documentId = stamp.first;
    source.documentRevision = QString::number(stamp.second);
    source.snapshotSha256 = RModelCommandService::stateFingerprint(session->getModel(id));
    if (session->modelCommandStamp(id) != stamp) return {};
    return source;
}

bool ownsExperiment(const QJsonObject &record, const QString &workspace,
                    const QString &sessionId, const QString &epoch)
{
    const auto source = record.value("authorization").toObject().value("sourceBinding").toObject();
    return source.value("workspaceId") == QJsonValue(workspace)
        && source.value("sessionId") == QJsonValue(sessionId)
        && source.value("sessionEpoch") == QJsonValue(epoch);
}

// Deliberately allowlist scalar status fields: durable records and job provenance
// contain trusted filesystem references that must never enter model responses.
QJsonObject experimentSummary(const QJsonObject &record, const QJsonArray &jobs)
{
    QJsonObject result;
    for (const QString &key : {QStringLiteral("experimentId"), QStringLiteral("state"),
                              QStringLiteral("cancelRequested"), QStringLiteral("terminalCode"),
                              QStringLiteral("verificationPassed"), QStringLiteral("createdAtUtc")})
        if (record.contains(key)) result.insert(key, record.value(key));
    result.insert("counters", record.value("counters"));
    const auto authorization = record.value("authorization").toObject();
    result.insert("authorizationDigest", authorization.value("authorizationDigest"));
    const QJsonObject budgets = authorization.value("experimentPolicy").toObject()
                                    .value(QStringLiteral("budgets")).toObject();
    quint64 solveBudget = 0, repairBudget = 0, modelTurnBudget = 0;
    if (exactKeys(budgets, {QStringLiteral("solves"), QStringLiteral("repairs"), QStringLiteral("modelTurns")})
        && unsignedInteger(budgets.value(QStringLiteral("solves")), &solveBudget) && solveBudget >= 1 && solveBudget <= 8
        && unsignedInteger(budgets.value(QStringLiteral("repairs")), &repairBudget) && repairBudget <= 3
        && unsignedInteger(budgets.value(QStringLiteral("modelTurns")), &modelTurnBudget)
        && modelTurnBudget >= 1 && modelTurnBudget <= 12)
        result.insert(QStringLiteral("budgets"), budgets);
    const QJsonObject sourceBinding = authorization.value("sourceBinding").toObject();
    const QString sourceDocumentId = sourceBinding.value(QStringLiteral("documentId")).toString();
    const QString sourceSessionEpoch = sourceBinding.value(QStringLiteral("sessionEpoch")).toString();
    const QString sourceRevision = sourceBinding.value(QStringLiteral("documentRevision")).toString();
    static const QRegularExpression decimalRevision(QStringLiteral("^[0-9]{1,32}$"));
    if (!sourceDocumentId.isEmpty() && sourceDocumentId.size() <= 128
        && !sourceSessionEpoch.isEmpty() && sourceSessionEpoch.size() <= 128
        && decimalRevision.match(sourceRevision).hasMatch())
        result.insert(QStringLiteral("sourceRuntimeStamp"), QJsonObject{
            {QStringLiteral("documentId"), sourceDocumentId},
            {QStringLiteral("sessionEpoch"), sourceSessionEpoch},
            {QStringLiteral("revision"), sourceRevision}});

    // Keep report evidence useful while excluding manifest paths, solver logs,
    // source snapshots, and arbitrary event payloads from model-facing reads.
    const QJsonObject verification = record.value(QStringLiteral("verificationReport")).toObject();
    if (!verification.isEmpty()) {
        QJsonObject publicVerification{{QStringLiteral("schema"), verification.value(QStringLiteral("schema"))},
            {QStringLiteral("status"), verification.value(QStringLiteral("status"))},
            {QStringLiteral("passed"), verification.value(QStringLiteral("passed"))}};
        const QJsonObject identity = verification.value(QStringLiteral("identity")).toObject();
        QJsonObject publicIdentity;
        for (const QString &key : {QStringLiteral("artifactId"), QStringLiteral("jobId")})
            if (identity.value(key).isString() && identity.value(key).toString().size() <= 128)
                publicIdentity.insert(key, identity.value(key));
        static const QRegularExpression sha256Grammar(QStringLiteral("^[0-9a-f]{64}$"));
        for (const QString &key : {QStringLiteral("manifestSha256"), QStringLiteral("physicalSummarySha256"),
                                   QStringLiteral("normalizedProblemSpecSha256"), QStringLiteral("inputModelSha256"),
                                   QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256")})
            if (identity.value(key).isString() && sha256Grammar.match(identity.value(key).toString()).hasMatch())
                publicIdentity.insert(key, identity.value(key));
        if (!publicIdentity.isEmpty()) publicVerification.insert(QStringLiteral("identity"), publicIdentity);
        QJsonObject publicMetrics;
        const QJsonObject metrics = verification.value(QStringLiteral("metrics")).toObject();
        if (metrics.size() > 24) return {};
        const QStringList metricNames{QStringLiteral("identity"), QStringLiteral("specification"),
            QStringLiteral("modelSupport"), QStringLiteral("nodalFields"), QStringLiteral("supportValues"),
            QStringLiteral("timeGrid"), QStringLiteral("displacement"), QStringLiteral("frequency"),
            QStringLiteral("dynamicEquilibrium"), QStringLiteral("referenceConvergence"),
            QStringLiteral("staticResidual"), QStringLiteral("reactionBalance"), QStringLiteral("strainEnergy"),
            QStringLiteral("modalResidual"), QStringLiteral("modeShape"), QStringLiteral("energyBalance"),
            QStringLiteral("kinematicConsistency"), QStringLiteral("sdofAnalyticDisplacement"),
            QStringLiteral("frozenForcedOscillator"), QStringLiteral("rodHistoryReference")};
        for (const QString &metricName : metricNames) {
            if (!metrics.contains(metricName)) continue;
            const QJsonObject metric = metrics.value(metricName).toObject();
            QJsonObject summary{{QStringLiteral("status"), metric.value(QStringLiteral("status"))},
                                {QStringLiteral("required"), metric.value(QStringLiteral("required"))}};
            for (const QString &key : {QStringLiteral("value"), QStringLiteral("limit"),
                                       QStringLiteral("tolerance"), QStringLiteral("referenceError")}) {
                const QJsonValue value = metric.value(key);
                if (value.isDouble() && std::isfinite(value.toDouble())) summary.insert(key, value);
                else if (value.isBool()) summary.insert(key, value);
                else if (value.isString() && value.toString().size() <= 128)
                    summary.insert(key, value);
            }
            const QJsonObject evidence = metric.value(QStringLiteral("evidence")).toObject();
            QJsonObject publicEvidence;
            for (const QString &key : {QStringLiteral("relativeError"), QStringLiteral("maximumRelativeError"),
                                       QStringLiteral("relativeDifference"), QStringLiteral("maximumAbsoluteErrorM"),
                                       QStringLiteral("fixedAbsoluteLimitM"), QStringLiteral("fixedLimit"),
                                       QStringLiteral("scaledMaximumAbsoluteError"), QStringLiteral("residualNorm"),
                                       QStringLiteral("normalizedResidual"), QStringLiteral("maximumRelativeResidual"),
                                       QStringLiteral("maximumNormalizedResidual"), QStringLiteral("relativeResidual"),
                                       QStringLiteral("relativeBalanceError"), QStringLiteral("relativeEnergyError"),
                                       QStringLiteral("maximumNormalizedImbalance"), QStringLiteral("effectiveRelativeLimit"),
                                       QStringLiteral("recordCount"), QStringLiteral("records"),
                                       QStringLiteral("expectedRecordCount"), QStringLiteral("expectedSteps"),
                                       QStringLiteral("timeStepS"), QStringLiteral("durationS"),
                                       QStringLiteral("actualHz"), QStringLiteral("referenceHz"),
                                       QStringLiteral("strainEnergyJ"), QStringLiteral("externalWorkJ"),
                                       QStringLiteral("estimatedScalarWork"), QStringLiteral("scalarWorkBudget"),
                                       QStringLiteral("massKg"), QStringLiteral("stiffnessNPerM"),
                                       QStringLiteral("omegaRadPerS")})
                if (evidence.value(key).isDouble() && std::isfinite(evidence.value(key).toDouble()))
                    publicEvidence.insert(key, evidence.value(key));
            if (evidence.value(QStringLiteral("reference")).isString()
                && evidence.value(QStringLiteral("reference")).toString().size() <= 128)
                publicEvidence.insert(QStringLiteral("reference"), evidence.value(QStringLiteral("reference")));
            if (evidence.value(QStringLiteral("referenceKind")).isString()
                && evidence.value(QStringLiteral("referenceKind")).toString().size() <= 128)
                publicEvidence.insert(QStringLiteral("referenceKind"), evidence.value(QStringLiteral("referenceKind")));
            if (!publicEvidence.isEmpty()) summary.insert(QStringLiteral("evidence"), publicEvidence);
            publicMetrics.insert(metricName, summary);
        }
        publicVerification.insert(QStringLiteral("metrics"), publicMetrics);
        const QJsonObject thresholds = verification.value(QStringLiteral("thresholds")).toObject();
        QJsonObject publicThresholds;
        for (const QString &key : {QStringLiteral("displacementRelative"), QStringLiteral("frequencyRelative"),
                                   QStringLiteral("effectiveDisplacementRelative"),
                                   QStringLiteral("effectiveFrequencyRelative"),
                                   QStringLiteral("sdofScaledAbsoluteDisplacement"),
                                   QStringLiteral("rodHistoryReferenceAbsoluteM")})
            if (thresholds.value(key).isDouble() && std::isfinite(thresholds.value(key).toDouble()))
                publicThresholds.insert(key, thresholds.value(key));
        if (!publicThresholds.isEmpty()) publicVerification.insert(QStringLiteral("thresholds"), publicThresholds);
        result.insert(QStringLiteral("verification"), publicVerification);
    }

    QJsonArray repairs;
    for (const QJsonValue &entryValue : record.value(QStringLiteral("history")).toArray()) {
        if (repairs.size() >= 2 * RTrustedExperimentService::HardRepairLimit) break;
        const QJsonObject entry = entryValue.toObject();
        const QString event = entry.value(QStringLiteral("event")).toString();
        if (event != QLatin1String("repair-started") && event != QLatin1String("repair-verified")
            && event != QLatin1String("repair-rejected")) continue;
        QJsonObject summary{{QStringLiteral("event"), event}};
        const QString at = entry.value(QStringLiteral("atUtc")).toString();
        if (at.size() <= 40) summary.insert(QStringLiteral("atUtc"), at);
        const QJsonObject details = entry.value(QStringLiteral("details")).toObject();
        for (const QString &key : {QStringLiteral("repair"), QStringLiteral("numericalConfigDigest"),
                                   QStringLiteral("observedPhysicalSummaryDigest")})
            if (details.contains(key)) summary.insert(key, details.value(key));
        const QJsonArray changedPaths = details.value(QStringLiteral("changedPaths")).toArray();
        if (!changedPaths.isEmpty()) {
            if (changedPaths.size() > 32) return {};
            bool validFields = true;
            for (const QJsonValue &field : changedPaths)
                validFields = validFields && field.isString() && field.toString().size() <= 128
                    && !field.toString().contains(QLatin1Char(':'));
            if (!validFields) return {};
            summary.insert(QStringLiteral("changedProblemFields"), changedPaths);
        }
        repairs.append(summary);
    }
    result.insert(QStringLiteral("repairs"), repairs);

    const QJsonObject convergence = record.value(QStringLiteral("convergence")).toObject();
    if (!convergence.isEmpty()) {
        QJsonObject publicConvergence;
        for (const QString &key : {QStringLiteral("kind"), QStringLiteral("status"),
                                   QStringLiteral("toleranceRelative"),
                                   QStringLiteral("requiredConsecutiveChanges"),
                                   QStringLiteral("maximumLevels"), QStringLiteral("levelsCompleted"),
                                   QStringLiteral("stopReason"), QStringLiteral("physicalSummaryDigest"),
                                   QStringLiteral("adjacentChanges"), QStringLiteral("cost")})
            if (convergence.contains(key)) publicConvergence.insert(key, convergence.value(key));
        QJsonArray levels;
        for (const QJsonValue &value : record.value(QStringLiteral("convergenceLevels")).toArray()) {
            const QJsonObject level = value.toObject();
            QJsonObject summary;
            for (const QString &key : {QStringLiteral("level"), QStringLiteral("numericalConfiguration"),
                                       QStringLiteral("physicalSummaryDigest"),
                                       QStringLiteral("verificationPassed"), QStringLiteral("artifactId"),
                                       QStringLiteral("jobId"), QStringLiteral("manifestSha256"),
                                       QStringLiteral("measurementSha256"), QStringLiteral("changeFromPrevious"),
                                       QStringLiteral("comparisonError"), QStringLiteral("cost")})
                if (level.contains(key)) summary.insert(key, level.value(key));
            const QJsonObject measurements = level.value(QStringLiteral("measurementSummary")).toObject();
            for (const QString &key : {QStringLiteral("analysisKind"), QStringLiteral("frequencyHz"),
                                       QStringLiteral("mode"), QStringLiteral("strainEnergyJ")})
                if (measurements.contains(key)) summary.insert(key, measurements.value(key));
            const QJsonObject cost = level.value(QStringLiteral("cost")).toObject();
            if (cost.contains(QStringLiteral("timeStepS"))) {
                const double timeStepS = cost.value(QStringLiteral("timeStepS")).toDouble();
                const double steps = cost.value(QStringLiteral("steps")).toDouble();
                summary.insert(QStringLiteral("timeStepS"), timeStepS);
                if (std::isfinite(timeStepS) && timeStepS > 0.0 && std::isfinite(steps) && steps > 0.0)
                    summary.insert(QStringLiteral("totalDurationS"), timeStepS * steps);
            }
            QJsonArray qois;
            for (const QJsonValue &observableValue : measurements.value(QStringLiteral("displacements")).toArray()) {
                const QJsonObject observable = observableValue.toObject();
                const double peak = observable.value(QStringLiteral("maximumAbsoluteValue")).toDouble();
                QJsonObject qoi{{QStringLiteral("quantity"), QStringLiteral("displacement")},
                    {QStringLiteral("selector"), observable.value(QStringLiteral("selector"))},
                    {QStringLiteral("component"), observable.value(QStringLiteral("component"))},
                    {QStringLiteral("coordinateM"), observable.value(QStringLiteral("coordinateM"))},
                    {QStringLiteral("sampleCount"), observable.value(QStringLiteral("sampleCount"))},
                    {QStringLiteral("peakAbsoluteM"), peak},
                    {QStringLiteral("rmsM"), observable.value(QStringLiteral("rmsValue"))},
                    {QStringLiteral("firstValueM"), observable.value(QStringLiteral("firstValue"))},
                    {QStringLiteral("lastValueM"), observable.value(QStringLiteral("lastValue"))}};
                if (observable.contains(QStringLiteral("startTimeS")))
                    qoi.insert(QStringLiteral("firstTimeS"), observable.value(QStringLiteral("startTimeS")));
                if (observable.contains(QStringLiteral("endTimeS")))
                    qoi.insert(QStringLiteral("lastTimeS"), observable.value(QStringLiteral("endTimeS")));
                qois.append(qoi);
            }
            summary.insert(QStringLiteral("quantities"), qois);
            const QJsonObject verifierMetrics = level.value(QStringLiteral("verificationReport")).toObject()
                .value(QStringLiteral("metrics")).toObject();
            QJsonObject referenceErrors;
            static const QSet<QString> referenceValues{
                QStringLiteral("relativeError"), QStringLiteral("maximumRelativeError"),
                QStringLiteral("relativeDifference"), QStringLiteral("maximumAbsoluteErrorM"),
                QStringLiteral("scaledMaximumAbsoluteError")};
            for (auto metric = verifierMetrics.constBegin(); metric != verifierMetrics.constEnd(); ++metric) {
                const QJsonObject metricObject = metric.value().toObject();
                const QJsonObject evidence = metricObject.value(QStringLiteral("evidence")).toObject();
                QJsonObject safeEvidence;
                for (const QString &key : referenceValues)
                    if (evidence.contains(key) && evidence.value(key).isDouble())
                        safeEvidence.insert(key, evidence.value(key));
                if (!safeEvidence.isEmpty())
                    referenceErrors.insert(metric.key(), QJsonObject{
                        {QStringLiteral("status"), metricObject.value(QStringLiteral("status"))},
                        {QStringLiteral("evidence"), safeEvidence}});
            }
            if (!referenceErrors.isEmpty()) summary.insert(QStringLiteral("referenceErrors"), referenceErrors);
            levels.append(summary);
        }
        publicConvergence.insert(QStringLiteral("levels"), levels);
        result.insert(QStringLiteral("convergence"), publicConvergence);
    }
    QJsonArray children;
    for (const auto &value : jobs)
    {
        const auto job = value.toObject();
        QJsonObject child;
        for (const QString &key : {QStringLiteral("jobId"), QStringLiteral("state"),
                                  QStringLiteral("trustedExperimentAttemptId"), QStringLiteral("resultArtifactId"),
                                  QStringLiteral("resultManifestVerified"), QStringLiteral("processExitCode")})
            if (job.contains(key)) child.insert(key, job.value(key));
        children.append(child);
    }
    result.insert("childJobs", children);
    result.insert("childJobsTruncated", jobs.size() >= RTrustedExperimentService::HardSolveLimit);
    return result;
}

constexpr qsizetype trustedHostReadbackLimit = 64 * 1024;
constexpr qsizetype maxTrustedFailurePackageBytes = 24 * 1024 * 1024;
constexpr uint trustedInspectionEntityLimit = 25000;
constexpr quint64 trustedInspectionNestedRecordLimit = 4096;
constexpr quint64 trustedInspectionMetadataByteLimit = 256 * 1024;

bool trustedInspectionWithinEntityLimit(const RModel &model)
{
    const quint64 count = quint64(model.getNNodes()) + quint64(model.getNElements())
        + quint64(model.getNLines()) + quint64(model.getNPoints())
        + quint64(model.getNSurfaces()) + quint64(model.getNVolumes());
    return count <= trustedInspectionEntityLimit;
}

bool trustedInspectionWithinNestedDataLimits(const RModel &model)
{
    quint64 nestedRecords = 0;
    quint64 metadataBytes = 0;
    const auto addRecords = [&nestedRecords](quint64 count) {
        if (nestedRecords > trustedInspectionNestedRecordLimit ||
            count > trustedInspectionNestedRecordLimit - nestedRecords)
            return false;
        nestedRecords += count;
        return true;
    };
    const auto addText = [&metadataBytes](const QString &value) {
        // Three bytes per UTF-16 code unit conservatively bounds UTF-8 storage
        // without first allocating a converted copy of untrusted model text.
        const quint64 codeUnits = quint64(value.size());
        if (metadataBytes > trustedInspectionMetadataByteLimit ||
            codeUnits > (trustedInspectionMetadataByteLimit - metadataBytes) / 3)
            return false;
        metadataBytes += codeUnits * 3;
        return true;
    };
    const auto &modalDampingRatios = model.getProblemSetup().getStructuralDynamicsSetup()
                                         .getModalDampingRatios();
    if (!addRecords(quint64(modalDampingRatios.size()))) return false;
    const auto withinCondition = [&addRecords, &addText](const RCondition &condition) {
        if (!addRecords(1) || !addText(condition.getName())) return false;
        for (uint componentIndex = 0; componentIndex < condition.size(); ++componentIndex) {
            const RConditionComponent &component = condition.getComponent(componentIndex);
            if (!addRecords(quint64(component.size()) + 1) || !addText(component.getName()) ||
                !addText(component.getUnits()))
                return false;
        }
        return true;
    };
    const auto withinGroup = [&addRecords, &addText, &withinCondition](const RElementGroup &group) {
        if (!addRecords(group.size()) || !addText(group.getName())) return false;
        const RMaterial &material = group.getMaterial();
        if (!addText(material.getName())) return false;
        for (uint propertyIndex = 0; propertyIndex < material.size(); ++propertyIndex) {
            const RMaterialProperty &property = material.get(propertyIndex);
            if (!addRecords(quint64(property.size()) + 1) || !addText(property.getName()) ||
                !addText(property.getKeyName()) || !addText(property.getUnits()) ||
                !addText(property.getKeyUnits()))
                return false;
        }
        for (uint conditionIndex = 0; conditionIndex < group.getNBoundaryConditions(); ++conditionIndex)
            if (!withinCondition(group.getBoundaryCondition(conditionIndex))) return false;
        for (uint conditionIndex = 0; conditionIndex < group.getNEnvironmentConditions(); ++conditionIndex)
            if (!withinCondition(group.getEnvironmentCondition(conditionIndex))) return false;
        for (uint conditionIndex = 0; conditionIndex < group.getNInitialConditions(); ++conditionIndex)
            if (!withinCondition(group.getInitialCondition(conditionIndex))) return false;
        return true;
    };

    if (!addText(model.getName()) || !addText(model.getDescription())) return false;
    for (uint index = 0; index < model.getNLines(); ++index)
        if (!withinGroup(model.getLine(index))) return false;
    for (uint index = 0; index < model.getNPoints(); ++index)
        if (!withinGroup(model.getPoint(index))) return false;
    for (uint index = 0; index < model.getNSurfaces(); ++index)
        if (!withinGroup(model.getSurface(index))) return false;
    for (uint index = 0; index < model.getNVolumes(); ++index)
        if (!withinGroup(model.getVolume(index))) return false;
    return true;
}

QJsonObject trustedObservedPhysicalStateProjection(const QJsonObject &observed)
{
    QJsonObject safe;
    QJsonArray nodes;
    for (const QJsonValue &value : observed.value(QStringLiteral("nodes")).toArray())
    {
        const QJsonObject source = value.toObject();
        QJsonObject node;
        for (const QString &key : {QStringLiteral("position"), QStringLiteral("physicalId"),
                                   QStringLiteral("xyz_m")})
            if (source.contains(key)) node.insert(key, source.value(key));
        nodes.append(node);
    }
    safe.insert(QStringLiteral("nodes"), nodes);

    QJsonArray elements;
    for (const QJsonValue &value : observed.value(QStringLiteral("elements")).toArray())
    {
        const QJsonObject source = value.toObject();
        QJsonObject element;
        for (const QString &key : {QStringLiteral("position"), QStringLiteral("physicalId"),
                                   QStringLiteral("type"), QStringLiteral("connectivity")})
            if (source.contains(key)) element.insert(key, source.value(key));
        elements.append(element);
    }
    safe.insert(QStringLiteral("elements"), elements);

    auto projectConditions = [](const QJsonValue &value) {
        QJsonArray projected;
        for (const QJsonValue &conditionValue : value.toArray())
        {
            const QJsonObject source = conditionValue.toObject();
            QJsonObject condition;
            for (const QString &key : {QStringLiteral("type"), QStringLiteral("typeName"),
                                       QStringLiteral("hasLocalDirection"),
                                       QStringLiteral("explicitLocalDirection"),
                                       QStringLiteral("localDirection")})
                if (source.contains(key)) condition.insert(key, source.value(key));
            QJsonArray components;
            for (const QJsonValue &componentValue : source.value(QStringLiteral("components")).toArray())
            {
                const QJsonObject sourceComponent = componentValue.toObject();
                QJsonObject component;
                for (const QString &key : {QStringLiteral("variable"), QStringLiteral("enabled"),
                                           QStringLiteral("unit")})
                    if (sourceComponent.contains(key)) component.insert(key, sourceComponent.value(key));
                QJsonArray samples;
                for (const QJsonValue &sampleValue : sourceComponent.value(QStringLiteral("samples")).toArray())
                {
                    const QJsonObject sourceSample = sampleValue.toObject();
                    QJsonObject sample;
                    for (const QString &key : {QStringLiteral("time_s"), QStringLiteral("timeKeyFinite"),
                                               QStringLiteral("value"), QStringLiteral("valueFinite")})
                        if (sourceSample.contains(key)) sample.insert(key, sourceSample.value(key));
                    samples.append(sample);
                }
                component.insert(QStringLiteral("samples"), samples);
                components.append(component);
            }
            condition.insert(QStringLiteral("components"), components);
            projected.append(condition);
        }
        return projected;
    };

    auto projectMaterial = [](const QJsonObject &source) {
        QJsonArray properties;
        for (const QJsonValue &propertyValue : source.value(QStringLiteral("properties")).toArray())
        {
            const QJsonObject sourceProperty = propertyValue.toObject();
            QJsonObject property;
            for (const QString &key : {QStringLiteral("type"), QStringLiteral("keyType")})
                if (sourceProperty.contains(key)) property.insert(key, sourceProperty.value(key));
            QJsonArray table;
            for (const QJsonValue &sampleValue : sourceProperty.value(QStringLiteral("table")).toArray())
            {
                const QJsonObject sourceSample = sampleValue.toObject();
                QJsonObject sample;
                for (const QString &key : {QStringLiteral("key"), QStringLiteral("value")})
                    if (sourceSample.contains(key)) sample.insert(key, sourceSample.value(key));
                table.append(sample);
            }
            property.insert(QStringLiteral("table"), table);
            properties.append(property);
        }
        return QJsonObject{{QStringLiteral("properties"), properties}};
    };

    for (const QString &groupType : {QStringLiteral("lines"), QStringLiteral("points"),
                                     QStringLiteral("surfaces"), QStringLiteral("volumes")})
    {
        QJsonArray groups;
        for (const QJsonValue &groupValue : observed.value(groupType).toArray())
        {
            const QJsonObject source = groupValue.toObject();
            QJsonObject group;
            for (const QString &key : {QStringLiteral("physicalId"), QStringLiteral("elements"),
                                       QStringLiteral("volume_m3"), QStringLiteral("thickness_m"),
                                       QStringLiteral("structuralFormulation"),
                                       QStringLiteral("structuralFormulationId")})
                if (source.contains(key)) group.insert(key, source.value(key));
            if (source.contains(QStringLiteral("section")))
            {
                const QJsonObject sourceSection = source.value(QStringLiteral("section")).toObject();
                QJsonObject section;
                for (auto it = sourceSection.constBegin(); it != sourceSection.constEnd(); ++it)
                    if (it.key() != QStringLiteral("name")) section.insert(it.key(), it.value());
                group.insert(QStringLiteral("section"), section);
            }
            if (source.contains(QStringLiteral("material")))
                group.insert(QStringLiteral("material"), projectMaterial(source.value(QStringLiteral("material")).toObject()));
            for (const QString &key : {QStringLiteral("boundaryConditions"),
                                       QStringLiteral("environmentConditions"),
                                       QStringLiteral("initialConditions")})
                if (source.contains(key)) group.insert(key, projectConditions(source.value(key)));
            groups.append(group);
        }
        safe.insert(groupType, groups);
    }

    const QJsonObject sourceAnalysis = observed.value(QStringLiteral("analysis")).toObject();
    QJsonObject analysis;
    if (sourceAnalysis.contains(QStringLiteral("problemType")))
        analysis.insert(QStringLiteral("problemType"), sourceAnalysis.value(QStringLiteral("problemType")));
    for (const QString &groupKey : {QStringLiteral("modal"), QStringLiteral("structuralDynamics")})
    {
        const QJsonObject sourceGroup = sourceAnalysis.value(groupKey).toObject();
        if (sourceGroup.isEmpty()) continue;
        QJsonObject group;
        for (const QString &key : {QStringLiteral("enabled"), QStringLiteral("method"),
                                   QStringLiteral("modes"), QStringLiteral("timeStep_s"),
                                   QStringLiteral("rayleighAlpha_1_s"), QStringLiteral("rayleighBeta_s"),
                                   QStringLiteral("rhoInfinity"), QStringLiteral("newmarkGamma"),
                                   QStringLiteral("newmarkBeta"), QStringLiteral("modalDampingRatios"),
                                   QStringLiteral("nIterations"), QStringLiteral("convergenceValue"),
                                   QStringLiteral("steps"), QStringLiteral("outputFrequency")})
            if (sourceGroup.contains(key)) group.insert(key, sourceGroup.value(key));
        analysis.insert(groupKey, group);
    }
    safe.insert(QStringLiteral("analysis"), analysis);
    return safe;
}

QJsonObject trustedInspectionProjection(const QJsonObject &inspection, const QString &basis,
                                        const QJsonObject &runtimeStamp,
                                        const QString &modelFingerprint)
{
    const QJsonObject source = inspection.value(QStringLiteral("source")).toObject();
    const QJsonObject mesh = inspection.value(QStringLiteral("mesh")).toObject();
    QJsonObject result{{QStringLiteral("operation"), QStringLiteral("analysis.inspect")},
        {QStringLiteral("schemaVersion"), RTrustedMechanicsService::SchemaVersion},
        {QStringLiteral("basis"), basis}, {QStringLiteral("sourceRuntimeStamp"), runtimeStamp},
        {QStringLiteral("source"), QJsonObject{{QStringLiteral("documentId"), source.value(QStringLiteral("sourceId"))},
            {QStringLiteral("documentRevision"), source.value(QStringLiteral("modelRevision"))},
            {QStringLiteral("modelFingerprint"), source.value(QStringLiteral("modelFingerprint"))}}},
        {QStringLiteral("mesh"), mesh},
        {QStringLiteral("model"), QJsonObject{{QStringLiteral("fingerprint"), modelFingerprint},
                                              {QStringLiteral("topology"), mesh}}}};
    const QJsonObject physical = inspection.value(QStringLiteral("physicalSummary")).toObject();
    if (!physical.isEmpty()) {
        QJsonObject safePhysical{{QStringLiteral("schemaVersion"), physical.value(QStringLiteral("schemaVersion"))},
                                 {QStringLiteral("modelType"), physical.value(QStringLiteral("modelType"))}};
        const QJsonObject geometry = physical.value(QStringLiteral("geometry")).toObject();
        const QJsonObject length = geometry.value(QStringLiteral("length")).toObject();
        if (length.value(QStringLiteral("value")).isDouble())
            safePhysical.insert(QStringLiteral("geometry"), QJsonObject{{QStringLiteral("lengthM"), length.value(QStringLiteral("value"))}});
        else if (geometry.value(QStringLiteral("length")).isDouble())
            safePhysical.insert(QStringLiteral("geometry"), QJsonObject{{QStringLiteral("lengthM"), geometry.value(QStringLiteral("length"))}});
        QJsonObject material;
        const QJsonObject sourceMaterial = physical.value(QStringLiteral("material")).toObject();
        for (const QString &key : {QStringLiteral("youngModulus"), QStringLiteral("poissonRatio"), QStringLiteral("density")})
            if (sourceMaterial.value(key).isDouble()) material.insert(key, sourceMaterial.value(key));
        safePhysical.insert(QStringLiteral("material"), material);
        QJsonObject section;
        const QJsonObject sourceSection = physical.value(QStringLiteral("section")).toObject();
        for (auto it = sourceSection.constBegin(); it != sourceSection.constEnd(); ++it) {
            if (it.value().isDouble()) section.insert(it.key(), it.value());
            else if (it.key() == QLatin1String("localY") && it.value().isArray()) section.insert(it.key(), it.value());
        }
        safePhysical.insert(QStringLiteral("section"), section);
        const QJsonObject analysis = physical.value(QStringLiteral("analysis")).toObject();
        QJsonObject safeAnalysis{{QStringLiteral("kind"), analysis.value(QStringLiteral("kind"))}};
        for (const QString &key : {QStringLiteral("duration"), QStringLiteral("rayleighAlpha"), QStringLiteral("rayleighBeta")})
            if (analysis.value(key).isDouble()) safeAnalysis.insert(key, analysis.value(key));
        safePhysical.insert(QStringLiteral("analysis"), safeAnalysis);
        auto safeSelectors = [](const QJsonValue &value) {
            QJsonArray output;
            for (const QJsonValue &entry : value.toArray()) {
                const QJsonObject object = entry.toObject();
                const QJsonObject selector = object.value(QStringLiteral("selector")).toObject();
                QJsonObject safeSelector{{QStringLiteral("kind"), selector.value(QStringLiteral("kind"))}};
                for (const QString &key : {QStringLiteral("x"), QStringLiteral("tolerance")})
                    if (selector.value(key).isDouble()) safeSelector.insert(key, selector.value(key));
                QJsonObject safe{{QStringLiteral("selector"), safeSelector}};
                if (object.value(QStringLiteral("dofs")).isArray()) safe.insert(QStringLiteral("dofs"), object.value(QStringLiteral("dofs")));
                for (const QString &key : {QStringLiteral("kind"), QStringLiteral("quantity"), QStringLiteral("component"),
                                           QStringLiteral("type")})
                    if (object.value(key).isString()) safe.insert(key, object.value(key));
                if (object.value(QStringLiteral("components")).isArray()) safe.insert(QStringLiteral("components"), object.value(QStringLiteral("components")));
                if (object.value(QStringLiteral("history")).isArray()) {
                    QJsonArray history;
                    for (const QJsonValue &sampleValue : object.value(QStringLiteral("history")).toArray()) {
                        const QJsonObject sample = sampleValue.toObject();
                        if (sample.value(QStringLiteral("time")).isDouble()
                            && sample.value(QStringLiteral("components")).isArray())
                            history.append(QJsonObject{{QStringLiteral("timeS"), sample.value(QStringLiteral("time"))},
                                                       {QStringLiteral("componentsN"), sample.value(QStringLiteral("components"))}});
                    }
                    safe.insert(QStringLiteral("history"), history);
                }
                output.append(safe);
            }
            return output;
        };
        safePhysical.insert(QStringLiteral("supports"), safeSelectors(physical.value(QStringLiteral("supports"))));
        safePhysical.insert(QStringLiteral("loads"), safeSelectors(physical.value(QStringLiteral("loads"))));
        safePhysical.insert(QStringLiteral("initialConditions"), safeSelectors(physical.value(QStringLiteral("initialConditions"))));
        safePhysical.insert(QStringLiteral("observables"), safeSelectors(physical.value(QStringLiteral("observables"))));
        result.insert(QStringLiteral("physicalSummary"), safePhysical);
    }
    for (const QString &key : {QStringLiteral("physicalSummaryDigest"), QStringLiteral("physicalSummaryBasis"),
                               QStringLiteral("numericalSummaryDigest")})
        if (inspection.contains(key)) result.insert(key, inspection.value(key));
    const QJsonObject numerical = inspection.value(QStringLiteral("numericalSummary")).toObject();
    if (!numerical.isEmpty()) result.insert(QStringLiteral("numericalSummary"), numerical);
    const QJsonObject observedPhysicalState = inspection.value(QStringLiteral("observedPhysicalState")).toObject();
    if (!observedPhysicalState.isEmpty())
    {
        const QJsonObject projectedObserved = trustedObservedPhysicalStateProjection(observedPhysicalState);
        result.insert(QStringLiteral("observedPhysicalState"), projectedObserved);
        result.insert(QStringLiteral("observedPhysicalStateDigest"),
                      RTrustedMechanicsService::summaryDigest(projectedObserved));
        result.insert(QStringLiteral("observedPhysicalStateDigestBasis"),
                      QStringLiteral("host-projected-observed-physical-state/v1"));
    }

    QJsonArray bindings;
    for (const QJsonValue &bindingValue : inspection.value(QStringLiteral("selectorBindings")).toArray()) {
        const QJsonObject binding = bindingValue.toObject();
        QJsonObject safe;
        for (const QString &key : {QStringLiteral("field"), QStringLiteral("index"), QStringLiteral("valid"),
                                   QStringLiteral("code"), QStringLiteral("modelRevision"),
                                   QStringLiteral("physicalIds"), QStringLiteral("nodePositions"),
                                   QStringLiteral("errorCode")})
            if (binding.contains(key)) safe.insert(key, binding.value(key));
        const QJsonObject selector = binding.value(QStringLiteral("selector")).toObject();
        if (!selector.isEmpty()) {
            QJsonObject safeSelector{{QStringLiteral("kind"), selector.value(QStringLiteral("kind"))}};
            for (const QString &key : {QStringLiteral("x"), QStringLiteral("tolerance")})
                if (selector.value(key).isDouble()) safeSelector.insert(key, selector.value(key));
            safe.insert(QStringLiteral("selector"), safeSelector);
        }
        bindings.append(safe);
    }
    if (!bindings.isEmpty()) result.insert(QStringLiteral("selectorBindings"), bindings);
    return result;
}

QJsonObject trustedInspectionJobIdentity(const QString &jobId, const QJsonObject &runtimeStamp,
                                         const QString &inspectedFingerprint)
{
    if (jobId.isEmpty())
        return {{QStringLiteral("status"), QStringLiteral("no_job")},
                {QStringLiteral("state"), QStringLiteral("no_job")},
                {QStringLiteral("resultSourceStatus"), QStringLiteral("no_job")}};

    QJsonObject result{{QStringLiteral("jobId"), jobId}};
    const QJsonObject job = SolverManager::getInstance().jobQuery(jobId);
    if (!job.value(QStringLiteral("found")).toBool())
    {
        result.insert(QStringLiteral("status"), QStringLiteral("not_found"));
        result.insert(QStringLiteral("state"), QStringLiteral("not_found"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("not_found"));
        return result;
    }
    if (job.value(QStringLiteral("jobId")).toString() != jobId)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("state"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }

    const QString jobState = job.value(QStringLiteral("state")).toString();
    if (!jobState.isEmpty()) result.insert(QStringLiteral("state"), jobState);
    if (job.value(QStringLiteral("processStarted")).isBool())
        result.insert(QStringLiteral("processStarted"), job.value(QStringLiteral("processStarted")));
    if (job.value(QStringLiteral("processExitCode")).isDouble())
        result.insert(QStringLiteral("processExitCode"), job.value(QStringLiteral("processExitCode")));

    const QJsonObject jobStamp = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
    const QJsonObject provenance = job.value(QStringLiteral("nativeResultProvenance")).toObject();
    const QJsonObject provenanceStamp = provenance.value(QStringLiteral("sourceRuntimeStamp")).toObject();
    const auto validStamp = [](const QJsonObject &stamp)
    {
        static const QRegularExpression revisionGrammar(QStringLiteral("^[0-9]{1,32}$"));
        const QString revision = stamp.value(QStringLiteral("revision")).toString();
        return validToken(stamp.value(QStringLiteral("documentId"))) &&
                validToken(stamp.value(QStringLiteral("sessionEpoch"))) &&
                revisionGrammar.match(revision).hasMatch();
    };
    if (!validStamp(jobStamp))
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    const QJsonObject safeJobStamp{{QStringLiteral("documentId"), jobStamp.value(QStringLiteral("documentId"))},
                                   {QStringLiteral("revision"), jobStamp.value(QStringLiteral("revision"))},
                                   {QStringLiteral("sessionEpoch"), jobStamp.value(QStringLiteral("sessionEpoch"))}};
    result.insert(QStringLiteral("sourceRuntimeStamp"), safeJobStamp);
    if (jobStamp != runtimeStamp)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("stale"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("stale"));
        return result;
    }

    const QJsonObject sourceSnapshot = provenance.value(QStringLiteral("sourceSnapshotRef")).toObject();
    QJsonObject snapshotStamp = sourceSnapshot.value(QStringLiteral("sourceRuntimeStamp")).toObject();
    if (snapshotStamp.isEmpty())
    {
        const QJsonObject instance = sourceSnapshot.value(QStringLiteral("instanceRef")).toObject();
        if (!instance.isEmpty())
            snapshotStamp = {{QStringLiteral("documentId"), instance.value(QStringLiteral("documentId"))},
                             {QStringLiteral("revision"), instance.value(QStringLiteral("documentRevision"))},
                             {QStringLiteral("sessionEpoch"), instance.value(QStringLiteral("sessionEpoch"))}};
    }
    const QString inputSnapshotSha = job.value(QStringLiteral("inputSnapshotSha256")).toString();
    const QString provenanceInputSha = provenance.value(QStringLiteral("inputSnapshotSha256")).toString();
    static const QRegularExpression sha256Grammar(QStringLiteral("^[0-9a-f]{64}$"));
    const bool frozenIdentityMatches = provenance.value(QStringLiteral("jobId")).toString() == jobId &&
            provenanceStamp == jobStamp && validStamp(snapshotStamp) && snapshotStamp == jobStamp &&
            sha256Grammar.match(inputSnapshotSha).hasMatch() && provenanceInputSha == inputSnapshotSha;
    if (!frozenIdentityMatches)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }

    // A model file SHA is not the host's state fingerprint. Re-read the exact
    // hash-bound solver input and compute the same in-memory fingerprint used
    // for the inspected model before reporting a match.
    const QString inputPath = job.value(QStringLiteral("inputSnapshotRef")).toString();
    const QString inputPackagePath = job.value(QStringLiteral("inputPackageRef")).toString();
    const QFileInfo inputInfo(inputPath);
    const QFileInfo inputPackageInfo(inputPackagePath);
    constexpr qint64 maxInspectionJobInputBytes = 32 * 1024 * 1024;
    const QString canonicalInputPath = inputInfo.canonicalFilePath();
    const QString absoluteInputPath = QDir::cleanPath(inputInfo.absoluteFilePath());
#ifdef Q_OS_WIN
    const bool canonicalPathMatches = canonicalInputPath.compare(absoluteInputPath, Qt::CaseInsensitive) == 0;
#else
    const bool canonicalPathMatches = canonicalInputPath == absoluteInputPath;
#endif
    if (inputPath.isEmpty() || inputPackagePath.isEmpty() || !inputInfo.isAbsolute() ||
        !inputInfo.isFile() || inputInfo.isSymLink() || inputInfo.size() <= 0 ||
        inputInfo.size() > maxInspectionJobInputBytes || !inputPackageInfo.isAbsolute() ||
        !inputPackageInfo.isDir() || inputPackageInfo.isSymLink() ||
        canonicalInputPath.isEmpty() || !canonicalPathMatches)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    QString openedInputPath;
    QString inputOpenError;
    std::unique_ptr<QFile> verifiedInputFile = TrustedArtifactPaths::openFileUnderRoot(
        inputInfo.absoluteFilePath(), inputPackageInfo.absoluteFilePath(),
        &openedInputPath, &inputOpenError, true);
    if (!verifiedInputFile || openedInputPath.isEmpty())
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }

    QString inputSnapshotError;
    QByteArray inputSnapshotBytes = TrustedArtifactPaths::readOpenedFileSnapshot(
        *verifiedInputFile, qsizetype(maxInspectionJobInputBytes), &inputSnapshotError);
    const QByteArray inputSnapshotHash = inputSnapshotBytes.isEmpty()
        ? QByteArray{} : QCryptographicHash::hash(inputSnapshotBytes, QCryptographicHash::Sha256);
    if (inputSnapshotHash.isEmpty() || QString::fromLatin1(inputSnapshotHash.toHex()) != inputSnapshotSha)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    verifiedInputFile.reset();

    const QString inputExtension = inputInfo.suffix();
    if (inputExtension != RModel::getDefaultFileExtension(false) &&
        inputExtension != RModel::getDefaultFileExtension(true))
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    QTemporaryDir privateSnapshotDirectory;
    const QFileDevice::Permissions privateDirectoryPermissions = QFileDevice::ReadOwner |
        QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    const QFileDevice::Permissions privateFilePermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    const QString privateSnapshotPath = QDir(privateSnapshotDirectory.path()).filePath(
        QStringLiteral("verified-input.%1").arg(inputExtension));
    QFile privateSnapshotWriter(privateSnapshotPath);
    if (!privateSnapshotDirectory.isValid() ||
        !QFile::setPermissions(privateSnapshotDirectory.path(), privateDirectoryPermissions) ||
        !privateSnapshotWriter.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        privateSnapshotWriter.write(inputSnapshotBytes) != inputSnapshotBytes.size() ||
        !privateSnapshotWriter.flush() || privateSnapshotWriter.size() != inputSnapshotBytes.size())
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    privateSnapshotWriter.close();
    inputSnapshotBytes.clear();
    if (!QFile::setPermissions(privateSnapshotPath, privateFilePermissions))
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    QString openedPrivateSnapshotPath;
    QString privateSnapshotOpenError;
    std::unique_ptr<QFile> privateSnapshotFile = TrustedArtifactPaths::openFileUnderRoot(
        privateSnapshotPath, privateSnapshotDirectory.path(),
        &openedPrivateSnapshotPath, &privateSnapshotOpenError, true);
    if (!privateSnapshotFile || openedPrivateSnapshotPath.isEmpty())
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    QByteArray privateSnapshotBytes = TrustedArtifactPaths::readOpenedFileSnapshot(
        *privateSnapshotFile, qsizetype(maxInspectionJobInputBytes), &privateSnapshotOpenError);
    const QByteArray privateSnapshotHash = privateSnapshotBytes.isEmpty()
        ? QByteArray{} : QCryptographicHash::hash(privateSnapshotBytes, QCryptographicHash::Sha256);
    if (privateSnapshotHash.isEmpty() || privateSnapshotHash != inputSnapshotHash ||
        QString::fromLatin1(privateSnapshotHash.toHex()) != inputSnapshotSha)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    privateSnapshotBytes.clear();

    QString jobFingerprint;
    try
    {
        RModel jobSourceModel;
        // Parse only the private, hash-verified copy. The solver input path is
        // no longer consulted after its bounded byte snapshot was captured.
        jobSourceModel.readFromVerifiedFile(privateSnapshotPath, *privateSnapshotFile);
        const QByteArray afterParseBytes = TrustedArtifactPaths::readOpenedFileSnapshot(
            *privateSnapshotFile, qsizetype(maxInspectionJobInputBytes), &privateSnapshotOpenError);
        const QByteArray afterParseHash = afterParseBytes.isEmpty()
            ? QByteArray{} : QCryptographicHash::hash(afterParseBytes, QCryptographicHash::Sha256);
        if (afterParseHash.isEmpty() || afterParseHash != privateSnapshotHash ||
            QString::fromLatin1(afterParseHash.toHex()) != inputSnapshotSha)
        {
            result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
            result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
            return result;
        }
        if (trustedInspectionWithinEntityLimit(jobSourceModel))
            jobFingerprint = RModelCommandService::stateFingerprint(jobSourceModel);
    }
    catch (...)
    {
        jobFingerprint.clear();
    }
    if (jobFingerprint.isEmpty())
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }
    result.insert(QStringLiteral("sourceFingerprint"), jobFingerprint);
    if (jobFingerprint != inspectedFingerprint)
    {
        result.insert(QStringLiteral("status"), QStringLiteral("mismatch"));
        result.insert(QStringLiteral("resultSourceStatus"), QStringLiteral("mismatch"));
        return result;
    }

    const QString state = job.value(QStringLiteral("state")).toString();
    const bool manifestExpected = job.value(QStringLiteral("nativeResultCatalogExpected")).toBool();
    const bool verifiedManifest = job.value(QStringLiteral("resultManifestVerified")).toBool() &&
            job.value(QStringLiteral("nativeResultCatalogAvailable")).toBool();
    QString resultSourceStatus = QStringLiteral("pending");
    if (state == QLatin1String("succeeded"))
    {
        if (job.value(QStringLiteral("eligibility")).toString() == QLatin1String("conflict") ||
            !job.value(QStringLiteral("resultArtifactVerified")).toBool() ||
            !job.value(QStringLiteral("resultSourceBindingVerified")).toBool() ||
            (manifestExpected && !verifiedManifest))
            resultSourceStatus = QStringLiteral("mismatch");
        else if (verifiedManifest)
            resultSourceStatus = QStringLiteral("matched");
        else
            resultSourceStatus = QStringLiteral("unverified");
    }
    else if (state == QLatin1String("failed") || state == QLatin1String("cancelled") ||
             state == QLatin1String("interrupted"))
        resultSourceStatus = QStringLiteral("unavailable");

    result.insert(QStringLiteral("status"), resultSourceStatus == QLatin1String("mismatch")
                  ? QStringLiteral("mismatch") : QStringLiteral("matched"));
    result.insert(QStringLiteral("resultSourceStatus"), resultSourceStatus);
    result.insert(QStringLiteral("resultSourceRuntimeStamp"), safeJobStamp);
    result.insert(QStringLiteral("resultSourceFingerprint"), jobFingerprint);
    static const QRegularExpression artifactIdGrammar(QStringLiteral("^artifact-[0-9a-f]{32}$"));
    if (job.value(QStringLiteral("resultArtifactId")).isString() &&
        artifactIdGrammar.match(job.value(QStringLiteral("resultArtifactId")).toString()).hasMatch())
        result.insert(QStringLiteral("resultArtifactId"), job.value(QStringLiteral("resultArtifactId")));
    if (sha256Grammar.match(job.value(QStringLiteral("resultArtifactSha256")).toString()).hasMatch())
        result.insert(QStringLiteral("resultArtifactSha256"), job.value(QStringLiteral("resultArtifactSha256")));
    const QString manifestSha256 = job.value(QStringLiteral("resultManifestSha256")).toString();
    if (sha256Grammar.match(manifestSha256).hasMatch())
        result.insert(QStringLiteral("resultManifestSha256"), manifestSha256);
    result.insert(QStringLiteral("resultArtifactVerified"),
                  job.value(QStringLiteral("resultArtifactVerified")).toBool());
    result.insert(QStringLiteral("resultSourceBindingVerified"),
                  job.value(QStringLiteral("resultSourceBindingVerified")).toBool());
    result.insert(QStringLiteral("resultManifestVerified"), verifiedManifest);
    return result;
}

QJsonObject trustedSolveCost(const QJsonObject &job)
{
    QJsonObject cost;
    const QDateTime started = QDateTime::fromString(job.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
    const QDateTime finished = QDateTime::fromString(job.value(QStringLiteral("finishedAt")).toString(), Qt::ISODateWithMs);
    if (started.isValid() && finished.isValid() && finished >= started)
        cost.insert(QStringLiteral("solveElapsedMs"), double(started.msecsTo(finished)));
    cost.insert(QStringLiteral("processStarted"), job.value(QStringLiteral("processStarted")));
    cost.insert(QStringLiteral("processExitCode"), job.value(QStringLiteral("processExitCode")));
    return cost;
}

bool trustedPrecisionOnlyFailure(const QJsonObject &report, const QString &analysisKind)
{
    if (report.value(QStringLiteral("schema")).toString() != QLatin1String("range.verification-report/v1")
        || report.value(QStringLiteral("passed")).toBool()) return false;
    const QJsonObject metrics = report.value(QStringLiteral("metrics")).toObject();
    bool foundPrecisionFailure = false;
    for (auto it = metrics.begin(); it != metrics.end(); ++it) {
        const QJsonObject metric = it.value().toObject();
        if (!metric.value(QStringLiteral("required")).toBool()) continue;
        const QString status = metric.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("pass")) continue;
        if (status != QLatin1String("fail")) return false;
        const QString name = it.key();
        const bool allowed = analysisKind == QLatin1String("static")
                ? name == QLatin1String("displacement")
            : analysisKind == QLatin1String("modal")
                ? name == QLatin1String("frequency")
            : analysisKind == QLatin1String("linearTransient")
                ? (name == QLatin1String("displacement")
                   || name == QLatin1String("referenceConvergence")
                   || name == QLatin1String("sdofAnalyticDisplacement")
                   || name == QLatin1String("rodHistoryReference"))
                : false;
        if (!allowed) return false;
        foundPrecisionFailure = true;
    }
    return foundPrecisionFailure;
}

QJsonArray experimentJobs(const QString &experimentId)
{
    QJsonArray result;
    auto &manager = SolverManager::getInstance();
    for (int offset = 0; ; offset += 200)
    {
        const auto page = manager.jobsQuery(offset, 200);
        for (const auto &value : page.value("jobs").toArray())
            if (value.toObject().value("trustedExperimentId") == QJsonValue(experimentId))
            {
                result.append(value);
                if (result.size() >= RTrustedExperimentService::HardSolveLimit) return result;
            }
        if (!page.value("hasMore").toBool()) return result;
    }
}

QString boundedFileTail(QFile &file, qsizetype maximumBytes, qint64 *fileSize, bool *truncated,
                        QString *error = nullptr, bool *hasNonWhitespaceContent = nullptr)
{
    if (error) error->clear();
    if (fileSize) *fileSize = 0;
    if (truncated) *truncated = false;
    if (hasNonWhitespaceContent) *hasNonWhitespaceContent = false;
    if (!file.isOpen())
    {
        if (error) *error = QStringLiteral("The solver log handle is no longer open.");
        return {};
    }
    const qint64 size = file.size();
    if (size < 0)
    {
        if (error) *error = QStringLiteral("The solver log size is unavailable.");
        return {};
    }
    if (fileSize) *fileSize = size;
    const bool omittedPrefix = size > maximumBytes;
    const qint64 offset = omittedPrefix ? size - maximumBytes : 0;
    if (!file.seek(offset))
    {
        if (error) *error = QStringLiteral("The solver log tail could not be sought.");
        return {};
    }
    const QByteArray bytes = file.read(maximumBytes);
    if (file.error() != QFileDevice::NoError || bytes.size() != qMin<qint64>(maximumBytes, size - offset))
    {
        if (error) *error = QStringLiteral("The solver log tail could not be read completely.");
        return {};
    }
    QString text = QString::fromUtf8(bytes);
    if (hasNonWhitespaceContent) *hasNonWhitespaceContent = !text.trimmed().isEmpty();
    if (truncated) *truncated = omittedPrefix;
    if (omittedPrefix)
        text.prepend(QStringLiteral("[earlier solver log bytes omitted: %1]\n").arg(offset));
    return text;
}

bool proposeAuthorizedMeshRefinement(const QJsonObject &problemSpec,
                                     const QJsonObject &authorization,
                                     const QJsonObject &verificationReport,
                                     QJsonObject *repairedProblemSpec,
                                     QString *reason)
{
    return RTrustedExperimentService::proposeAuthorizedMeshRefinement(
        problemSpec, authorization, verificationReport, repairedProblemSpec, reason);
}
}

struct ModelCommandHost::State
{
    MeshJobController *meshController = nullptr;
    std::unique_ptr<RTrustedExperimentService> experiments;
    QString experimentRoot;
    QSet<QString> finalizingExperiments;
    struct Preview
    {
        QString id;
        QString documentId;
        QString sessionEpoch;
        QString command;
        QString nativeRevision;
        QString digest;
        quint64 stampGeneration = 0;
        QJsonObject arguments;
        QJsonObject nativePreview;
        QJsonArray readSet;
        RModelCommandService::Delta delta;
        bool hasSupportedDelta = false;
        QString staticBindingHistoryId;
        QString staticBindingHistoryHash;
        QString piezoHistoryId;
        QString piezoHistoryHash;
        QString inputFingerprint;
        QString readSetHash;
        QString jobId;
        QString jobSourceDocumentId;
        QString jobSourceRevision;
        QJsonObject projectSnapshotRef;
        QJsonObject validation;
        QJsonObject recoveryQualification;
        QString meshPreviewId;
        QString experimentId;
        QJsonObject experimentAuthorizationRequest;
        QJsonObject experimentAuthorization;
        QJsonObject experimentSourceBinding;
        QJsonObject experimentProblemSpecInput;
        QJsonObject experimentDiagnostics;
        bool approved = false;
        bool consumed = false;
    };

    struct JobSource
    {
        QString documentId;
        QString sessionEpoch;
        QString revision;
        QJsonObject projectSnapshotRef;
    };

    struct ResultDocument
    {
        QString documentId;
        QString artifactPath;
        QString artifactSha256;
        quint64 openedAtRevision = 0;
        bool normalizedAtOpen = false;
    };

    struct IdempotentResult
    {
        QString requestDigest;
        QString documentId;
        QJsonObject response;
    };

    struct Entry
    {
        quint64 generation = 0;
        std::unique_ptr<RModelCommandSession> editing;
        QString closeRetryId, closeOperationId;
        QJsonObject closeRetryPayload;
    };
    std::map<QString, Entry> entries;
    std::map<QString, Preview> previews;
    std::map<QString, QJsonObject> operations;
    std::map<QString, IdempotentResult> idempotency;
    std::map<QString, JobSource> jobSources;
    std::map<QString, ResultDocument> resultDocuments;
    QMap<QString,QJsonObject> resultPublicationUnknown;
    QSet<QString> resultPublicationsInFlight;
    std::map<QString, QJsonObject> resultDescriptors;
    QList<QString> previewOrder;
    QList<QString> operationOrder;
    QList<QString> idempotencyOrder;
    QList<QString> knownDocuments;
    QList<QJsonObject> events;
    QMap<QString, quint64> lastDocumentRevisions;
    QMap<QString, quint64> selectionRevisions;
    QJsonObject lastSelection;
    QString sessionEpoch = uuid();
    QString activeOperationId;
    QJsonArray activeChangedPaths;
    quint64 nextEventSeq = 1;
    quint64 nextSelectionRevision = 0;
    QString journal;
    QString failure;

    QJsonObject appendEvent(const QString &type, const QString &documentId, const QJsonObject &fields = {})
    {
        QJsonObject event{{QStringLiteral("eventId"), uuid()},
                          {QStringLiteral("eventSeq"), double(nextEventSeq++)},
                          {QStringLiteral("type"), type},
                          {QStringLiteral("sessionEpoch"), sessionEpoch},
                          {QStringLiteral("documentId"), documentId}};
        for (auto it = fields.begin(); it != fields.end(); ++it) event.insert(it.key(), it.value());
        events.append(event);
        if (events.size() > 256) events.removeFirst();
        return event;
    }

    QJsonObject appendRevisionEvent(const QString &documentId, quint64 revision)
    {
        QJsonObject fields{{QStringLiteral("afterRevision"), double(revision)},
                           {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")},
                           {QStringLiteral("changedPaths"), activeChangedPaths}};
        if (lastDocumentRevisions.contains(documentId))
            fields.insert(QStringLiteral("beforeRevision"), double(lastDocumentRevisions.value(documentId)));
        else
            fields.insert(QStringLiteral("beforeRevision"), QJsonValue(QJsonValue::Null));
        if (!activeOperationId.isEmpty()) fields.insert(QStringLiteral("operationId"), activeOperationId);
        lastDocumentRevisions.insert(documentId, revision);
        return appendEvent(activeOperationId.isEmpty() ? QStringLiteral("document.revision_changed")
                                                       : QStringLiteral("document.changed"),
                           documentId, fields);
    }

    Entry *ensure(Session *session, uint id)
    {
        const auto stamp = session->modelCommandStamp(id);
        if (stamp.first.isEmpty()) { failure = QStringLiteral("The model has been closed."); return nullptr; }
        auto &entry = entries[stamp.first];
        if (entry.editing && entry.generation != stamp.second)
        {
            const bool recorded = entry.editing->recordExternalChange("external_edit", session->getModel(id), &failure);
            entry.editing.reset();
            if (!recorded) return nullptr;
        }
        if (!entry.editing)
        {
            RModelCommandSession::JournalOptions options;
            options.path = journal;
            options.actorId = QStringLiteral("gui-user");
            const QString source = modelArtifact(session->getModel(id).getFileName());
            QFile file(source);
            if (!source.isEmpty() && file.open(QIODevice::ReadOnly))
            {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (hash.addData(&file))
                {
                    options.sourcePath = QFileInfo(source).absoluteFilePath();
                    options.sourceSha256 = QString::fromLatin1(hash.result().toHex());
                }
            }
            entry.editing = std::make_unique<RModelCommandSession>(session->getModel(id), options);
            entry.closeRetryId.clear();
            entry.closeOperationId.clear();
            entry.closeRetryPayload = {};
            entry.generation = stamp.second;
        }
        failure.clear();
        return &entry;
    }
};

ModelCommandHost::ModelCommandHost(Session *session) : QObject(session), state(std::make_unique<State>()), session(session)
{
    QString experimentData = Application::instance()->getApplicationSettings()->getDataDir();
    if (experimentData.isEmpty()) experimentData = QDir::tempPath();
    state->experimentRoot = QDir(experimentData).filePath("trusted-experiments");
    state->experiments = std::make_unique<RTrustedExperimentService>(state->experimentRoot);
    // Session constructs this Host before Application::initialize stores the
    // Session pointer. Defer the singleton lookup until the first event turn.
    QTimer::singleShot(0, this, [this] {
        connect(&SolverManager::getInstance(), &SolverManager::nativeJobUpdated, this,
            [this](const QString &jobId, const QJsonObject &) { finalizeExperimentJob(jobId); });
    });
    state->meshController = new MeshJobController(session, this);
    connect(state->meshController, &MeshJobController::previewsInvalidated, this, [this] {
        for (auto &entry : state->previews)
            if (entry.second.command == "mesh.generate" || entry.second.command == "mesh.accept")
                entry.second.consumed = true;
    });
    const QDir directory(QDir(Application::instance()->getApplicationSettings()->getSessionDir()).filePath("command-audit"));
    QDir().mkpath(directory.absolutePath());
    state->journal = directory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json");
    for (uint id = 0; id < session->getNModels(); ++id)
    {
        const auto stamp = session->modelCommandStamp(id);
        state->knownDocuments.append(stamp.first);
        state->lastDocumentRevisions.insert(stamp.first, stamp.second);
    }
    connect(session, &Session::documentRevisionChanged, this, [this](const QString &documentId, quint64 revision) {
        emit eventPublished(state->appendRevisionEvent(documentId, revision));
    });
    connect(session, &Session::modelAdded, this, [this](uint modelID) {
        const auto stamp = this->session->modelCommandStamp(modelID);
        if (stamp.first.isEmpty()) return;
        if (modelID >= uint(state->knownDocuments.size())) state->knownDocuments.append(stamp.first);
        else state->knownDocuments.insert(int(modelID), stamp.first);
        state->lastDocumentRevisions.insert(stamp.first, stamp.second);
        emit eventPublished(state->appendEvent(QStringLiteral("document.opened"), stamp.first,
            {{QStringLiteral("afterRevision"), double(stamp.second)},
             {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")}}));
    });
    connect(session, &Session::modelRemoved, this, [this](uint modelID) {
        QString documentId;
        if (modelID < uint(state->knownDocuments.size())) documentId = state->knownDocuments.takeAt(int(modelID));
        if (!documentId.isEmpty())
            emit eventPublished(state->appendEvent(QStringLiteral("document.closed"), documentId));
        this->pruneClosedModels();
    });
    connect(session, &Session::modelSelectionChanged, this, [this](uint modelID) {
        const auto stamp = this->session->modelCommandStamp(modelID);
        if (stamp.first.isEmpty()) return;
        const quint64 revision = state->selectionRevisions.value(stamp.first) + 1;
        state->selectionRevisions.insert(stamp.first, revision);
        state->nextSelectionRevision = qMax(state->nextSelectionRevision, revision);
        emit eventPublished(state->appendEvent(QStringLiteral("selection.changed"), stamp.first,
            {{QStringLiteral("selectionRevision"), double(revision)},
             {QStringLiteral("documentRevision"), double(stamp.second)},
             {QStringLiteral("revisionScope"), QStringLiteral("selection-change-counter")}}));
    });
}

ModelCommandHost::~ModelCommandHost()
{
    // Session members may already have been destroyed; only owned snapshots
    // are safe to visit while QObject tears down its children.
    for (auto &[identity, entry] : state->entries)
        if (entry.editing && !entry.editing->close())
            RLogger::error("Model session %s closed with incomplete audit.\n", identity.toUtf8().constData());
}
QString ModelCommandHost::journalPath() const { return state->journal; }
QString ModelCommandHost::sessionEpoch() const { return state->sessionEpoch; }

void ModelCommandHost::finalizeExperimentJob(const QString &jobId)
{
    // jobQuery may publish a reconciliation update while validating the record.
    if (state->finalizingExperiments.contains(jobId)) return;
    state->finalizingExperiments.insert(jobId);
    const auto guard = qScopeGuard([&] { state->finalizingExperiments.remove(jobId); });
    const auto job = SolverManager::getInstance().jobQuery(jobId);
    const QString experimentId = job.value("trustedExperimentId").toString();
    const QString attemptId = job.value("trustedExperimentAttemptId").toString();
    const QString jobState = job.value("state").toString();
    if (experimentId.isEmpty() || attemptId.isEmpty()
        || (jobState != "succeeded" && jobState != "failed" && jobState != "cancelled"
            && jobState != "interrupted")) return;
    const auto loaded = state->experiments->query(experimentId);
    const auto record = loaded.data.value("experiment").toObject();
    const auto binding = record.value("authorization").toObject().value("sourceBinding").toObject();
    if (!loaded.ok || record.value("cancelRequested").toBool()
        || record.value("state") != QJsonValue("solving")
        || record.value("activeAttempt").toObject().value("attemptId") != QJsonValue(attemptId)) return;
    const auto source = experimentSource(session, state->sessionEpoch, binding.value("documentId").toString());
    QJsonObject identity{{"artifactId", job.value("resultArtifactId")}, {"jobId", jobId},
        {"inputSnapshotSha256", job.value("inputSnapshotSha256")},
        {"inputPackageSha256", job.value("inputPackageSha256")},
        {"sourceSnapshotRef", job.value("nativeResultProvenance").toObject().value("sourceSnapshotRef")}};
    const bool completeIdentity = !identity.value("artifactId").toString().isEmpty()
        && !identity.value("sourceSnapshotRef").toObject().isEmpty()
        && job.value("resultManifestVerified").toBool()
        && !job.value("resultManifestRef").toString().isEmpty()
        && !job.value("resultManifestSha256").toString().isEmpty()
        && job.value("eligibility") != QJsonValue("conflict");
    const auto persistFailureDiagnosis = [&]() -> bool {
        const QJsonObject authorization = record.value("authorization").toObject();
        const QJsonObject sourceBinding = authorization.value("sourceBinding").toObject();
        const QString sourceId = sourceBinding.value("documentId").toString();
        const QString modelRevision = sourceBinding.value("documentRevision").toString();
        const QJsonObject specInput = record.value("candidateProblemSpecInput").toObject();
        const QJsonObject normalizedSpec = record.value("candidateProblemSpec").toObject();
        const QJsonObject activeAttempt = record.value("activeAttempt").toObject();
        const QString inputPath = job.value("inputSnapshotRef").toString();
        const QString expectedInputSha = job.value("inputSnapshotSha256").toString();
        const QString expectedPackageSha = job.value("inputPackageSha256").toString();
        const QString trustedRunsRoot = QDir(state->experimentRoot).filePath(
            experimentId + QStringLiteral("/.range-runs"));
        QString inputCanonicalPath;
        QString captureError;
        const QJsonObject jobSourceStamp = job.value("sourceRuntimeStamp").toObject();
        QJsonObject identityChecks{
            {QStringLiteral("activeAttemptIdMatches"), activeAttempt.value("attemptId").toString() == attemptId},
            {QStringLiteral("activeInputSnapshotMatches"), activeAttempt.value("inputSnapshotSha256").toString() == expectedInputSha},
            {QStringLiteral("activeInputPackageMatches"), activeAttempt.value("inputPackageSha256").toString() == expectedPackageSha},
            {QStringLiteral("jobExperimentMatches"), job.value("trustedExperimentId").toString() == experimentId},
            {QStringLiteral("jobAttemptMatches"), job.value("trustedExperimentAttemptId").toString() == attemptId},
            {QStringLiteral("sourceDocumentMatches"), jobSourceStamp.value("documentId").toString() == sourceId},
            {QStringLiteral("sourceRevisionMatches"), jobSourceStamp.value("revision").toString() == modelRevision},
            {QStringLiteral("inputPathUnderTrustedRunRoot"), TrustedArtifactPaths::canonicalFileUnderRoot(inputPath, trustedRunsRoot, &inputCanonicalPath)}};
        QStringList identityFailures;
        for (auto check = identityChecks.constBegin(); check != identityChecks.constEnd(); ++check)
            if (!check.value().toBool()) identityFailures.append(check.key());
        bool inputBound = identityFailures.isEmpty();
        if (!inputBound)
            captureError = QStringLiteral("Failed job identity binding checks: %1.").arg(identityFailures.join(QStringLiteral(", ")));

        constexpr qsizetype maxTrustedFailureInputSnapshotBytes = 32 * 1024 * 1024;
        QByteArray inputSnapshotBytes;
        std::unique_ptr<QFile> inputArtifactFile;
        if (inputBound)
        {
            inputArtifactFile = TrustedArtifactPaths::openFileUnderRoot(
                inputPath, trustedRunsRoot, &inputCanonicalPath, &captureError);
            if (!inputArtifactFile) inputBound = false;
        }
        if (inputBound)
        {
            inputSnapshotBytes = TrustedArtifactPaths::readOpenedFileSnapshot(
                *inputArtifactFile, maxTrustedFailureInputSnapshotBytes, &captureError);
            const QByteArray inputHash = inputSnapshotBytes.isEmpty()
                ? QByteArray{} : QCryptographicHash::hash(inputSnapshotBytes, QCryptographicHash::Sha256);
            if (inputHash.isEmpty() || QString::fromLatin1(inputHash.toHex()) != expectedInputSha)
            {
                inputBound = false;
                if (captureError.isEmpty())
                    captureError = QStringLiteral("The frozen candidate input snapshot does not match its submitted SHA-256.");
            }
        }
        QString modelSnapshotPath;
        std::unique_ptr<QTemporaryDir> modelSnapshotDirectory;
        if (inputBound)
        {
            const QString extension = QFileInfo(inputCanonicalPath).suffix();
            if (extension != RModel::getDefaultFileExtension(false) &&
                extension != RModel::getDefaultFileExtension(true))
            {
                inputBound = false;
                captureError = QStringLiteral("The frozen candidate input has an unsupported model extension.");
            }
            else
            {
                modelSnapshotDirectory = std::make_unique<QTemporaryDir>(
                    QDir(trustedRunsRoot).filePath(QStringLiteral("diagnostic-candidate-XXXXXX")));
                modelSnapshotPath = QDir(modelSnapshotDirectory->path()).filePath(
                    QStringLiteral("candidate.%1").arg(extension));
                QFile modelSnapshotWriter(modelSnapshotPath);
                const QFileDevice::Permissions privateDirectoryPermissions =
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
                if (!modelSnapshotDirectory->isValid() ||
                    !QFile::setPermissions(modelSnapshotDirectory->path(), privateDirectoryPermissions) ||
                    !modelSnapshotWriter.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                    modelSnapshotWriter.write(inputSnapshotBytes) != inputSnapshotBytes.size() ||
                    !modelSnapshotWriter.flush() || modelSnapshotWriter.size() != inputSnapshotBytes.size())
                {
                    inputBound = false;
                    captureError = QStringLiteral("The verified candidate bytes could not be materialized in a private diagnostic directory.");
                }
                else
                {
                    modelSnapshotWriter.close();
                }
            }
        }
        const auto validation = RTrustedMechanicsService::validate(specInput);
        if (inputBound && (!validation.valid || validation.normalizedSpec != normalizedSpec
            || activeAttempt.value("physicalSummaryDigest").toString()
                != RTrustedMechanicsService::summaryDigest(RTrustedMechanicsService::physicalSummary(normalizedSpec))
            || activeAttempt.value("numericalConfigDigest").toString()
                != RTrustedMechanicsService::summaryDigest(RTrustedMechanicsService::numericalSummary(normalizedSpec))))
        {
            inputBound = false;
            captureError = QStringLiteral("The failed attempt ProblemSpec no longer matches its validated physical and numerical identity.");
        }

        const QString errorCode = !job.value("processStarted").toBool()
            ? QStringLiteral("SOLVER_PROCESS_START_FAILED")
            : job.value("processExited").toBool() && job.value("processExitCode").toInt(-1) != 0
                ? QStringLiteral("SOLVER_PROCESS_EXIT_NONZERO")
                : job.value("processExitCode").toInt(-1) == 0
                    ? QStringLiteral("SOLVER_RESULT_EVIDENCE_INCOMPLETE")
                    : QStringLiteral("SOLVER_FAILURE_UNCLASSIFIED");

        QString solverLog;
        QString logSource = QStringLiteral("terminal-message-fallback");
        QString logCaptureError;
        QString logArtifactPath;
        qint64 logFileSize = 0;
        bool logTruncated = false;
        bool logTailHasContent = false;
        const QJsonObject logRefs = job.value("logArtifactRefs").toObject();
        const QString registeredLogPath = logRefs.value("solverLog").toString();
        QString logCanonicalPath;
        std::unique_ptr<QFile> solverLogFile;
        QDir trustedAttemptRoot(QFileInfo(inputCanonicalPath).absolutePath());
        if (inputBound && trustedAttemptRoot.cdUp() && !registeredLogPath.isEmpty())
        {
            solverLogFile = TrustedArtifactPaths::openFileUnderRoot(
                registeredLogPath, trustedAttemptRoot.absolutePath(), &logCanonicalPath, &logCaptureError);
            if (solverLogFile)
            {
                logArtifactPath = logCanonicalPath;
                solverLog = boundedFileTail(*solverLogFile, 256 * 1024, &logFileSize,
                                            &logTruncated, &logCaptureError, &logTailHasContent);
                if (logCaptureError.isEmpty() && logTailHasContent)
                    logSource = QStringLiteral("solver-log-tail");
                else if (logCaptureError.isEmpty())
                    logCaptureError = QStringLiteral("The solver log artifact is empty or contains only whitespace.");
            }
        }
        else if (!registeredLogPath.isEmpty())
        {
            logCaptureError = QStringLiteral("The solver log could not be bound to the verified attempt input directory.");
        }
        if (logSource == QStringLiteral("terminal-message-fallback"))
            solverLog = job.value("message").toString().left(2048);

        Model failedCandidate;
        bool modelSnapshotAvailable = false;
        std::unique_ptr<QFile> verifiedModelSnapshot;
        if (inputBound)
        {
            verifiedModelSnapshot = TrustedArtifactPaths::openFileUnderRoot(
                modelSnapshotPath, trustedRunsRoot, nullptr, &captureError, true);
            const QByteArray verifiedSnapshotBytes = verifiedModelSnapshot
                ? TrustedArtifactPaths::readOpenedFileSnapshot(
                    *verifiedModelSnapshot, maxTrustedFailureInputSnapshotBytes, &captureError)
                : QByteArray{};
            const QByteArray verifiedSnapshotHash = verifiedSnapshotBytes.isEmpty()
                ? QByteArray{} : QCryptographicHash::hash(verifiedSnapshotBytes, QCryptographicHash::Sha256);
            if (verifiedSnapshotHash.isEmpty() ||
                QString::fromLatin1(verifiedSnapshotHash.toHex()) != expectedInputSha)
            {
                inputBound = false;
                if (captureError.isEmpty())
                    captureError = QStringLiteral("The private model snapshot does not match the verified candidate bytes.");
            }
            else if (!isDirectModelArtifact(modelSnapshotPath, &captureError))
            {
                inputBound = false;
            }
        }
        if (inputBound)
        {
            try
            {
                failedCandidate.readFromVerifiedFile(modelSnapshotPath, *verifiedModelSnapshot);
                const QByteArray verifiedSnapshotBytes = verifiedModelSnapshot
                    ? TrustedArtifactPaths::readOpenedFileSnapshot(
                        *verifiedModelSnapshot, maxTrustedFailureInputSnapshotBytes, &captureError)
                    : QByteArray{};
                const QByteArray verifiedSnapshotHash = verifiedSnapshotBytes.isEmpty()
                    ? QByteArray{} : QCryptographicHash::hash(verifiedSnapshotBytes, QCryptographicHash::Sha256);
                modelSnapshotAvailable = !verifiedSnapshotHash.isEmpty() &&
                    QString::fromLatin1(verifiedSnapshotHash.toHex()) == expectedInputSha;
                if (!modelSnapshotAvailable && captureError.isEmpty())
                    captureError = QStringLiteral("The model changed while it was being reopened from the verified byte snapshot.");
            }
            catch (const RError &failure)
            {
                captureError = QStringLiteral("The frozen candidate model could not be reopened: %1")
                    .arg(failure.getMessage().left(512));
            }
            catch (const std::exception &failure)
            {
                captureError = QStringLiteral("The frozen candidate model could not be reopened: %1")
                    .arg(QString::fromUtf8(failure.what()).left(512));
            }
        }
        verifiedModelSnapshot.reset();
        const QJsonObject diagnosis = RTrustedMechanicsService::diagnoseFailure(
            specInput, modelSnapshotAvailable ? static_cast<const RModel *>(&failedCandidate) : nullptr,
            sourceId, modelRevision, jobId, errorCode, solverLog);
        const QByteArray problemSpecBytes = QJsonDocument(specInput).toJson(QJsonDocument::Compact);
        const QString problemSpecSha = QString::fromLatin1(QCryptographicHash::hash(
            problemSpecBytes, QCryptographicHash::Sha256).toHex());
        QJsonObject package{{QStringLiteral("schemaVersion"), 1},
                            {QStringLiteral("jobId"), jobId},
                            {QStringLiteral("experimentId"), experimentId},
                            {QStringLiteral("attemptId"), attemptId},
                            {QStringLiteral("sourceId"), sourceId},
                            {QStringLiteral("modelRevision"), modelRevision},
                            {QStringLiteral("problemSpec"), specInput},
                            {QStringLiteral("normalizedProblemSpec"), normalizedSpec},
                            {QStringLiteral("problemSpecSha256"), problemSpecSha},
                            {QStringLiteral("inputSnapshotSha256"), expectedInputSha},
                            {QStringLiteral("inputPackageSha256"), expectedPackageSha},
                            {QStringLiteral("errorCode"), errorCode},
                            {QStringLiteral("terminalMessage"), job.value("message").toString().left(2048)},
                            {QStringLiteral("processEvidence"), QJsonObject{
                                 {QStringLiteral("processStarted"), job.value("processStarted")},
                                 {QStringLiteral("processExited"), job.value("processExited")},
                                 {QStringLiteral("processExitCode"), job.value("processExitCode")}}},
                            {QStringLiteral("solverLogCapture"), QJsonObject{
                                 {QStringLiteral("source"), logSource},
                                 {QStringLiteral("artifactRef"), logArtifactPath},
                                 {QStringLiteral("fileSizeBytes"), double(logFileSize)},
                                 {QStringLiteral("truncated"), logTruncated},
                                 {QStringLiteral("captureError"), logCaptureError}}},
                            {QStringLiteral("modelSnapshotAvailable"), modelSnapshotAvailable},
                            {QStringLiteral("identityChecks"), identityChecks},
                            {QStringLiteral("captureError"), captureError},
                            {QStringLiteral("diagnosticReport"), diagnosis.value("diagnosticReport")},
                            {QStringLiteral("failurePackage"), diagnosis}};
        const QByteArray packageBytes = QJsonDocument(package).toJson(QJsonDocument::Compact);
        const QString packageSha = QString::fromLatin1(QCryptographicHash::hash(
            packageBytes, QCryptographicHash::Sha256).toHex());
        const bool withinSizeLimit = packageBytes.size() <= maxTrustedFailurePackageBytes;
        const bool processStarted = job.value("processStarted").toBool();
        const bool logEvidenceAvailable = (logSource == QStringLiteral("solver-log-tail") && !solverLog.trimmed().isEmpty()) ||
            (!processStarted && !job.value("message").toString().trimmed().isEmpty());
        QJsonObject boundDiagnosis{{QStringLiteral("schemaVersion"), 1},
                                   {QStringLiteral("status"), modelSnapshotAvailable && logEvidenceAvailable
                                        ? QStringLiteral("complete") : QStringLiteral("incomplete")},
                                   {QStringLiteral("packageSha256"), packageSha},
                                   {QStringLiteral("packageBytes"), double(packageBytes.size())}};
        if (withinSizeLimit) boundDiagnosis.insert(QStringLiteral("package"), package);

        SolverManager &manager = SolverManager::getInstance();
        QString persistenceError;
        bool persisted = false;
        if (withinSizeLimit)
        {
            persisted = manager.updateNativeJob(jobId,
                {{QStringLiteral("trustedFailureDiagnosis"), boundDiagnosis},
                 {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
            if (!persisted)
                persistenceError = manager.nativeJobDiagnostics.value(jobId)
                    .value(QStringLiteral("message")).toString();
        }
        else
        {
            persistenceError = QStringLiteral("The diagnostic package exceeded the 24 MiB bounded job-record budget.");
        }
        if (!persisted)
        {
            boundDiagnosis.insert(QStringLiteral("status"), QStringLiteral("persistence_failed"));
            boundDiagnosis.insert(QStringLiteral("persisted"), false);
            if (!withinSizeLimit) boundDiagnosis.insert(QStringLiteral("packageOmitted"), true);
            if (persistenceError.isEmpty())
                persistenceError = QStringLiteral("The failure diagnosis could not be committed to the durable native-job registry.");
            if (manager.nativeJobs.contains(jobId))
                manager.nativeJobs[jobId].insert(QStringLiteral("trustedFailureDiagnosis"), boundDiagnosis);
            manager.nativeJobDiagnostics.insert(jobId,
                QJsonObject{{QStringLiteral("code"), QStringLiteral("TRUSTED_FAILURE_DIAGNOSIS_PERSIST_FAILED")},
                            {QStringLiteral("message"), persistenceError.left(1024)},
                            {QStringLiteral("packageSha256"), packageSha},
                            {QStringLiteral("packageBytes"), double(packageBytes.size())},
                            {QStringLiteral("recordRef"), manager.nativeJobRecordPaths.value(jobId)}});
        }
        return persisted;
    };
    const auto stopAttempt = [&](const QString &code) {
        return state->experiments->finishSolve(experimentId, source, attemptId, false, {}, "stop", code,
            QStringLiteral("Trusted child solve ended without a verified candidate (%1).").arg(code));
    };
    if (jobState != "succeeded" || !completeIdentity)
    {
        const bool failureDiagnosisPersisted = jobState != "failed" || persistFailureDiagnosis();
        stopAttempt(jobState == "succeeded" ? "SOLVE_IDENTITY_MISSING"
            : jobState == "cancelled" ? "SOLVE_CANCELLED"
            : jobState == "interrupted" ? "SOLVE_INTERRUPTED"
            : !failureDiagnosisPersisted ? "FAILURE_DIAGNOSIS_PERSIST_FAILED" : "SOLVE_FAILED");
        return;
    }
    const auto finished = state->experiments->finishSolve(experimentId, source, attemptId, true, identity);
    if (!finished.ok)
    {
        // An incomplete success identity must not leave an active transaction.
        if (finished.code == "SOLVE_IDENTITY_INVALID") stopAttempt(finished.code);
        return;
    }
    const auto verified = state->experiments->verifyCandidate(experimentId, source, identity,
        job.value("resultManifestRef").toString(), job.value("resultManifestSha256").toString(),
        trustedSolveCost(job));
    if (!verified.ok)
        state->experiments->stop(experimentId, source,
            verified.code,
            QStringLiteral("Trusted candidate verification could not be completed."));
    else if (verified.data.value("state") == QJsonValue("repairing"))
    {
        const auto latest = state->experiments->query(experimentId);
        const QJsonObject latestRecord = latest.data.value("experiment").toObject();
        QJsonObject repairedSpec;
        QString repairReason;
        const QJsonObject policy = latestRecord.value("authorization").toObject()
            .value("experimentPolicy").toObject();
        const QJsonObject convergenceStudy = policy.value("convergenceStudy").toObject();
        bool proposalValid = false;
        if (latest.ok && !convergenceStudy.isEmpty())
        {
            const QJsonObject report = latestRecord.value("verificationReport").toObject();
            const QJsonObject analysis = latestRecord.value("candidateProblemSpec").toObject()
                .value("analysis").toObject();
            const QString analysisKind = analysis.value("kind").toString();
            const bool verified = latestRecord.value("verificationPassed").toBool();
            if (verified || trustedPrecisionOnlyFailure(report, analysisKind))
            {
                const auto levels = latestRecord.value("convergenceLevels").toArray();
                const auto proposal = RTrustedConvergenceService::proposeLevel(
                    latestRecord.value("problemSpecInput").toObject(), convergenceStudy, levels.size(), &repairedSpec);
                proposalValid = proposal.ok;
                repairReason = proposal.message;
            }
            else
            {
                repairReason = QStringLiteral("Verification failure is not a permitted convergence refinement case.");
            }
        }
        else if (latest.ok)
        {
            proposalValid = proposeAuthorizedMeshRefinement(
                latestRecord.value("candidateProblemSpecInput").toObject(),
                latestRecord.value("authorization").toObject(),
                latestRecord.value("verificationReport").toObject(), &repairedSpec, &repairReason);
        }
        if (!latest.ok || !proposalValid)
        {
            state->experiments->markNeedsConfirmation(experimentId, source,
                repairReason.isEmpty()
                    ? QStringLiteral("Verification or convergence evidence does not support another numerical refinement.")
                    : repairReason,
                {{QStringLiteral("verificationReport"), latestRecord.value("verificationReport")},
                 {QStringLiteral("convergence"), latestRecord.value("convergence")}});
            return;
        }
        const auto repair = state->experiments->beginRepair(experimentId, source, repairedSpec);
        if (!repair.ok)
        {
            const auto current = state->experiments->query(experimentId);
            if (current.data.value("experiment").toObject().value("state") != QJsonValue("needs_confirmation"))
                state->experiments->stop(experimentId, source, repair.code,
                                         QStringLiteral("Authorized numerical repair could not start."));
            return;
        }
        auto candidate = RTrustedMechanicsService::createCandidate(repairedSpec, source.documentId,
                                                                    source.documentRevision);
        if (!candidate.valid)
        {
            state->experiments->markNeedsConfirmation(experimentId, source,
                QStringLiteral("The bounded numerical candidate could not be generated."),
                {{QStringLiteral("candidateCode"), candidate.code},
                 {QStringLiteral("candidateMessage"), candidate.message}});
            return;
        }
        const auto diagnosis = RTrustedMechanicsService::diagnose(repairedSpec, &candidate.model,
            source.documentId, source.documentRevision);
        if (diagnosis.value("status") == QJsonValue("error"))
        {
            state->experiments->markNeedsConfirmation(experimentId, source,
                QStringLiteral("The rebuilt numerical candidate did not pass physical readback diagnosis."),
                {{QStringLiteral("diagnosis"), diagnosis}});
            return;
        }
        const auto repaired = state->experiments->finishRepair(experimentId, source, repairedSpec,
            RTrustedMechanicsService::physicalSummary(candidate.normalizedSpec));
        if (!repaired.ok)
        {
            state->experiments->markNeedsConfirmation(experimentId, source,
                QStringLiteral("Readback evidence did not confirm preservation of the authorized physical problem."),
                {{QStringLiteral("repairCode"), repaired.code}, {QStringLiteral("diagnosis"), diagnosis}});
            return;
        }

        Model detached;
        static_cast<RModel&>(detached) = candidate.model;
        std::unique_ptr<SolverTask> task;
        try
        {
            auto &manager = SolverManager::getInstance();
            task = std::make_unique<SolverTask>(Application::instance()->getApplicationSettings(), detached,
                qMakePair(source.documentId, source.documentRevision.toULongLong()),
                latestRecord.value("authorization").toObject(),
                QDir(state->experimentRoot).filePath(experimentId), manager.getTaskServerName(),
                SolverTask::TrustedExperimentCandidateTag{});
            QString captureError;
            if (!task->captureTrustedCandidateSnapshot(detached, &captureError))
            {
                state->experiments->stop(experimentId, source, QStringLiteral("CANDIDATE_CAPTURE_FAILED"),
                    QStringLiteral("The authorized numerical candidate could not be frozen."),
                    {{QStringLiteral("diagnostic"), captureError.left(2048)}});
                return;
            }
        }
        catch (const RError &failure)
        {
            state->experiments->stop(experimentId, source, QStringLiteral("CANDIDATE_CAPTURE_FAILED"),
                                     failure.getMessage().left(2048));
            return;
        }
        catch (const std::exception &failure)
        {
            state->experiments->stop(experimentId, source, QStringLiteral("CANDIDATE_CAPTURE_FAILED"),
                                     QString::fromUtf8(failure.what()).left(2048));
            return;
        }
        if (repairedSpec.value(QStringLiteral("analysis")).toObject()
                .value(QStringLiteral("kind")).toString() == QStringLiteral("linearTransient"))
        {
            QString checkpointPreparationError;
            if (!task->prepareNativeCheckpointInput(&checkpointPreparationError))
            {
                state->experiments->stop(experimentId, source,
                    QStringLiteral("CANDIDATE_CHECKPOINT_PREPARATION_FAILED"),
                    QStringLiteral("The authorized transient candidate could not freeze its checkpoint input."),
                    {{QStringLiteral("checkpointPreparationDiagnostic"), checkpointPreparationError.left(2048)}});
                return;
            }
        }
        const auto attempt = state->experiments->beginSolve(experimentId, source, repairedSpec,
            QString::fromLatin1(task->getInputSnapshotSha256().toHex()),
            QString::fromLatin1(task->getInputPackageSha256().toHex()));
        if (!attempt.ok)
        {
            state->experiments->stop(experimentId, source, attempt.code,
                                     QStringLiteral("The verified numerical candidate could not begin its solve."));
            return;
        }
        const QString nextAttemptId = attempt.data.value("attemptId").toString();
        QString submitError;
        QString childJobId;
        if (task->setTrustedExperimentAttemptId(nextAttemptId))
            childJobId = SolverManager::getInstance().submitTrustedDetached(task.release(), &submitError);
        if (childJobId.isEmpty())
        {
            state->experiments->finishSolve(experimentId, source, nextAttemptId, false, {}, "stop",
                QStringLiteral("CHILD_SUBMISSION_FAILED"), submitError.left(2048));
            return;
        }
        // The child uses the same approved policy and is independently finalized
        // by its own terminal registry update; this call handles synchronous exit.
        finalizeExperimentJob(childJobId);
    }
}

bool ModelCommandHost::isBusy()
{
    return SolverManager::getInstance().getNRunning() || SolverManager::getInstance().getNWaiting()
        || RJobManager::getInstance().getNRunning() || RJobManager::getInstance().getNWaiting();
}

bool ModelCommandHost::isMutationBlocked()
{
    SolverManager &manager = SolverManager::getInstance();
    if (RJobManager::getInstance().getNRunning() || RJobManager::getInstance().getNWaiting()
        || manager.hasLiveModelTasks()) return true;

    // Native jobs own frozen source snapshots, so they do not hold the live
    // Session models. Keep legacy SolverManager jobs blocked, while allowing
    // independent edits and reads to proceed during detached native runs.
    const int managerJobs = int(manager.getNRunning() + manager.getNWaiting());
    if (managerJobs == 0) return false;
    int registeredNativeJobs = 0;
    int offset = 0;
    while (true)
    {
        const QJsonObject page = manager.jobsQuery(offset, 200);
        if (!page.value(QStringLiteral("error")).toString().isEmpty()) return true;
        const QJsonArray jobs = page.value(QStringLiteral("jobs")).toArray();
        for (const QJsonValue &value : jobs)
        {
            const QJsonObject job = value.toObject();
            const QString state = job.value(QStringLiteral("state")).toString();
            if (state != QStringLiteral("queued") && state != QStringLiteral("running")
                && state != QStringLiteral("cancelling")) continue;
            const QJsonObject sourceStamp = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
            const QString snapshotPath = job.value(QStringLiteral("inputSnapshotRef")).toString();
            const QString snapshotHash = job.value(QStringLiteral("inputSnapshotSha256")).toString();
            const QString packagePath = job.value(QStringLiteral("inputPackageRef")).toString();
            const QString packageHash = job.value(QStringLiteral("inputPackageSha256")).toString();
            static const QRegularExpression digestGrammar(QStringLiteral("^[0-9a-f]{64}$"));
            const bool frozenInput = !sourceStamp.value(QStringLiteral("documentId")).toString().isEmpty()
                && !sourceStamp.value(QStringLiteral("revision")).toString().isEmpty()
                && QFileInfo(snapshotPath).isFile() && digestGrammar.match(snapshotHash).hasMatch()
                && QFileInfo(packagePath).isDir() && digestGrammar.match(packageHash).hasMatch();
            if (frozenInput) ++registeredNativeJobs;
        }
        offset += jobs.size();
        if (jobs.isEmpty() || offset >= page.value(QStringLiteral("total")).toInt()) break;
    }
    return managerJobs > registeredNativeJobs;
}

void ModelCommandHost::pruneClosedModels()
{
    for (auto it = state->entries.begin(); it != state->entries.end();)
    {
        if (session->findModelByCommandIdentity(it->first) == RConstants::eod)
        {
            if (it->second.editing && !closeEditingDurably(it->first))
            {
                const auto message = tr("The model was closed, but its audit record could not be saved. The pending record will be retried on the next model command.");
                RLogger::error("%s\n", message.toUtf8().constData());
                emit commandRejected(message);
                ++it;
                continue;
            }
            it = state->entries.erase(it);
        }
        else ++it;
    }
}

bool ModelCommandHost::closeEditingDurably(const QString &documentId, QString *error,
                                          const RProjectCatalog::PublicationLeasePtr &publicationLease)
{
    auto found = state->entries.find(documentId);
    if (found == state->entries.end() || !found->second.editing) return true;
    auto &entry = found->second;
    const bool alreadyClosed = entry.editing->query().value("lifecycle") == QJsonValue("closed");
    if (alreadyClosed && entry.closeRetryPayload.isEmpty()) return true;
    auto *catalog = session->getProjectCatalog();
    if (!catalog)
    {
        if (error) *error = tr("The project ledger is unavailable; the close record remains pending.");
        return false;
    }
    if (entry.closeRetryPayload.isEmpty())
    {
        if (entry.closeRetryId.isEmpty()) entry.closeRetryId = uuid();
        auto intent = catalog->beginOperation(entry.closeRetryId,
            {{"command", "model.session.close"}, {"documentId", documentId},
             {"editingIdentity", entry.editing->query().value("modelId")}}, {}, publicationLease, error);
        if (intent.operation.operationId.isEmpty()) return false;
        entry.closeOperationId = intent.operation.operationId;
        const auto payload = entry.editing->prepareCloseRetry(entry.closeRetryId, entry.closeOperationId, error);
        if (payload.isEmpty() || !catalog->queueCloseRetry(entry.closeRetryId, entry.closeOperationId,
                "close_record_pending", payload, publicationLease, error)) return false;
        entry.closeRetryPayload = payload;
        if (!catalog->advanceOperation(entry.closeOperationId, RProjectCatalog::OperationPhase::Accepted,
                                        {}, publicationLease, error)) return false;
    }
    if (alreadyClosed ? !RModelCommandSession::replayCloseRetry(entry.closeRetryPayload, error)
                      : !entry.editing->closeWithRetryPayload(entry.closeRetryPayload, error)) return false;
    RProjectCatalog::OperationAssociation association;
    association.observation = {{"status", "ok"}, {"code", "OK"},
        {"result", QJsonObject{{"operationId", entry.closeOperationId},
            {"command", "model.session.close"},
            {"editingSessionId", entry.closeRetryPayload.value("record").toObject().value("sessionId")},
            {"recordId", entry.closeRetryPayload.value("record").toObject().value("recordId")},
            {"persistence", "durable"}}}};
    if (!catalog->advanceOperation(entry.closeOperationId, RProjectCatalog::OperationPhase::Observed,
                                  association, publicationLease, error)) return false;
    return catalog->completeCloseRetry(entry.closeRetryId, publicationLease, error);
}

bool ModelCommandHost::closeDurableSessions(QString *error)
{
    bool complete = true;
    for (const auto &[identity, entry] : state->entries)
        if (entry.editing && !closeEditingDurably(identity, error)) complete = false;
    return complete;
}

QJsonObject ModelCommandHost::query(uint modelID)
{
    if (QThread::currentThread() != thread()) return rejected("wrong_thread", tr("Model commands require the GUI thread."));
    pruneClosedModels();
    if (isMutationBlocked()) return rejected("model_busy", tr("Wait for active model tasks to finish."));
    QMutexLocker guard(session->getMutex());
    auto *entry = state->ensure(session, modelID);
    if (!entry) return rejected("session_unavailable", state->failure);
    if (!entry->editing->setRunning(false)) return rejected("journal_unavailable", tr("Cannot record model availability."));
    auto result = entry->editing->query();
    result.insert("guiModelIdentity", session->modelCommandStamp(modelID).first);
    return result;
}

QJsonObject ModelCommandHost::execute(uint modelID, const QJsonObject &request)
{
    if (QThread::currentThread() != thread()) return rejected("wrong_thread", tr("Model commands require the GUI thread."));
    pruneClosedModels();
    if (isMutationBlocked() || activeModalInteraction())
        return rejected("model_busy", tr("Wait for active document interaction and model tasks to finish."));
    QJsonObject result;
    {
        QMutexLocker guard(session->getMutex());
        auto *entry = state->ensure(session, modelID);
        if (!entry) return rejected("session_unavailable", state->failure);
        if (!entry->editing->setRunning(false)) return rejected("journal_unavailable", tr("Cannot record model availability."));
        // GUI history is authoritative; callers use the existing Undo/Redo actions.
        if (request.value("commandId") != QJsonValue("analysis.modal.configure"))
            return rejected("unsupported_gui_command", tr("Use the GUI history actions for Undo and Redo."));
        const QString beforeRevision = entry->editing->query().value("revision").toString();
        result = entry->editing->execute(request);
        if (result.value("status") != QJsonValue("accepted")
            || entry->editing->query().value("revision").toString() == beforeRevision) return result;
        session->storeCurentModelVersion(modelID, tr("Configure modal analysis"));
        session->getModel(modelID).getProblemSetup().setModalSetup(entry->editing->model().getProblemSetup().getModalSetup());
        ++entry->generation; // The following notification increments the live stamp once.
    }
    session->setProblemChanged(modelID);
    return result;
}

QString ModelCommandHost::projectWorkspace() const
{
    return QThread::currentThread() == thread() && session ? session->projectWorkspace() : QString();
}

QJsonObject ModelCommandHost::evaluationEvidenceSnapshot(const QString &experimentId) const
{
    if (QThread::currentThread() != thread() || !session || !state->experiments
        || !validToken(experimentId)) return {};

    const auto loaded = state->experiments->query(experimentId);
    if (!loaded.ok) return {};
    const QJsonObject record = loaded.data.value(QStringLiteral("experiment")).toObject();
    if (record.value(QStringLiteral("experimentId")).toString() != experimentId
        || record.value(QStringLiteral("state")).toString() != QLatin1String("candidate_ready")
        || record.value(QStringLiteral("cancelRequested")).toBool()
        || !record.value(QStringLiteral("verificationPassed")).toBool()
        || !record.value(QStringLiteral("activeAttempt")).toObject().isEmpty()) return {};

    const QJsonObject authorization = record.value(QStringLiteral("authorization")).toObject();
    const QJsonObject sourceBinding = authorization.value(QStringLiteral("sourceBinding")).toObject();
    if (!exactKeys(sourceBinding, {QStringLiteral("workspaceId"), QStringLiteral("sessionId"),
                                   QStringLiteral("sessionEpoch"), QStringLiteral("documentId"),
                                   QStringLiteral("documentRevision"), QStringLiteral("snapshotSha256")})
        || !ownsExperiment(record, projectWorkspace(), session->getID(), state->sessionEpoch)) return {};

    static const QRegularExpression revisionGrammar(QStringLiteral("^[0-9]{1,32}$"));
    static const QRegularExpression sha256Grammar(QStringLiteral("^[0-9a-f]{64}$"));
    constexpr int maximumWorkspaceIdCodeUnits = 256;
    const QString workspaceId = sourceBinding.value(QStringLiteral("workspaceId")).toString();
    const QString documentId = sourceBinding.value(QStringLiteral("documentId")).toString();
    const QString revision = sourceBinding.value(QStringLiteral("documentRevision")).toString();
    const QString authorizationDigest = authorization.value(QStringLiteral("authorizationDigest")).toString();
    if (!sourceBinding.value(QStringLiteral("workspaceId")).isString()
        || workspaceId.trimmed().isEmpty() || workspaceId.size() > maximumWorkspaceIdCodeUnits
        || !validToken(sourceBinding.value(QStringLiteral("sessionId")))
        || !validToken(sourceBinding.value(QStringLiteral("sessionEpoch")))
        || !validToken(sourceBinding.value(QStringLiteral("documentId")))
        || !revisionGrammar.match(revision).hasMatch()
        || !sha256Grammar.match(sourceBinding.value(QStringLiteral("snapshotSha256")).toString()).hasMatch()
        || !sha256Grammar.match(authorizationDigest).hasMatch()) return {};
    const QJsonObject sourceRuntimeStamp{{QStringLiteral("documentId"), documentId},
        {QStringLiteral("sessionEpoch"), sourceBinding.value(QStringLiteral("sessionEpoch"))},
        {QStringLiteral("revision"), revision}};

    const QJsonObject solveIdentity = record.value(QStringLiteral("latestSolveIdentity")).toObject();
    if (!exactKeys(solveIdentity, {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                                   QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256"),
                                   QStringLiteral("sourceSnapshotRef")})
        || !validToken(solveIdentity.value(QStringLiteral("artifactId")))
        || !validToken(solveIdentity.value(QStringLiteral("jobId")))
        || !sha256Grammar.match(solveIdentity.value(QStringLiteral("inputSnapshotSha256")).toString()).hasMatch()
        || !sha256Grammar.match(solveIdentity.value(QStringLiteral("inputPackageSha256")).toString()).hasMatch())
        return {};

    const QString attemptId = record.value(QStringLiteral("latestSolveAttemptId")).toString();
    if (!validToken(attemptId)) return {};
    int matchingAttempts = 0;
    QJsonObject completedAttempt;
    for (const QJsonValue &value : record.value(QStringLiteral("attempts")).toArray()) {
        const QJsonObject attempt = value.toObject();
        if (attempt.value(QStringLiteral("attemptId")).toString() != attemptId) continue;
        ++matchingAttempts;
        completedAttempt = attempt;
    }
    if (matchingAttempts != 1 || completedAttempt.value(QStringLiteral("status")).toString() != QLatin1String("completed")
        || completedAttempt.value(QStringLiteral("solveIdentity")).toObject() != solveIdentity
        || completedAttempt.value(QStringLiteral("inputSnapshotSha256")) != solveIdentity.value(QStringLiteral("inputSnapshotSha256"))
        || completedAttempt.value(QStringLiteral("inputPackageSha256")) != solveIdentity.value(QStringLiteral("inputPackageSha256")))
        return {};

    const QJsonObject candidateProblemSpecInput = record.value(QStringLiteral("candidateProblemSpecInput")).toObject();
    const QJsonObject candidateProblemSpec = record.value(QStringLiteral("candidateProblemSpec")).toObject();
    const auto candidateValidation = RTrustedMechanicsService::validate(candidateProblemSpecInput);
    if (!candidateValidation.valid || candidateValidation.normalizedSpec != candidateProblemSpec
        || record.value(QStringLiteral("repairInProgress")).toBool()) return {};
    const QJsonObject originalProblemSpecInput = record.value(QStringLiteral("problemSpecInput")).toObject();
    const auto originalValidation = RTrustedMechanicsService::validate(originalProblemSpecInput);
    if (!originalValidation.valid
        || RTrustedMechanicsService::summaryDigest(originalValidation.normalizedSpec)
            != authorization.value(QStringLiteral("problemSpecDigest")).toString()) return {};
    const QJsonObject authorizationPolicy = authorization.value(QStringLiteral("experimentPolicy")).toObject();
    const QJsonObject counters = record.value(QStringLiteral("counters")).toObject();
    quint64 repairCount = 0;
    if (!unsignedInteger(counters.value(QStringLiteral("repairs")), &repairCount)) return {};
    const QString candidatePhysicalDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::physicalSummary(candidateProblemSpec));
    const QString candidateNumericalDigest = RTrustedMechanicsService::summaryDigest(
        RTrustedMechanicsService::numericalSummary(candidateProblemSpec));
    if (candidatePhysicalDigest != authorization.value(QStringLiteral("physicalSummaryDigest")).toString()
        || completedAttempt.value(QStringLiteral("physicalSummaryDigest")).toString() != candidatePhysicalDigest
        || completedAttempt.value(QStringLiteral("numericalConfigDigest")).toString() != candidateNumericalDigest
        || (record.contains(QStringLiteral("candidateNumericalConfigDigest"))
            && record.value(QStringLiteral("candidateNumericalConfigDigest")).toString() != candidateNumericalDigest))
        return {};
    if (repairCount == 0) {
        if (candidateProblemSpecInput != originalProblemSpecInput
            || RTrustedMechanicsService::summaryDigest(candidateProblemSpec)
                != authorization.value(QStringLiteral("problemSpecDigest")).toString()
            || candidateNumericalDigest != authorization.value(QStringLiteral("numericalConfigDigest")).toString())
            return {};
    } else {
        const QJsonArray allowedRanges = authorizationPolicy.value(QStringLiteral("allowedNumericalRanges")).toArray();
        if (allowedRanges.isEmpty() || allowedRanges.size() > 4) return {};
        const QSet<QString> allowedPaths{QStringLiteral("/geometry/segments"), QStringLiteral("/analysis/modes"),
            QStringLiteral("/analysis/timeStep"), QStringLiteral("/analysis/steps")};
        QSet<QString> seenPaths;
        QJsonObject expectedCandidateSpec = originalValidation.normalizedSpec;
        const auto integerPath = [](const QString &path) {
            return path == QLatin1String("/geometry/segments") || path == QLatin1String("/analysis/modes")
                || path == QLatin1String("/analysis/steps");
        };
        for (const QJsonValue &rangeValue : allowedRanges) {
            const QJsonObject range = rangeValue.toObject();
            const QString path = range.value(QStringLiteral("path")).toString();
            if (!exactKeys(range, {QStringLiteral("path"), QStringLiteral("minimum"), QStringLiteral("maximum")})
                || !allowedPaths.contains(path) || seenPaths.contains(path)
                || !range.value(QStringLiteral("minimum")).isDouble()
                || !range.value(QStringLiteral("maximum")).isDouble()) return {};
            const double minimum = range.value(QStringLiteral("minimum")).toDouble();
            const double maximum = range.value(QStringLiteral("maximum")).toDouble();
            const QStringList pieces = path.mid(1).split(QLatin1Char('/'));
            if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum
                || pieces.size() != 2) return {};
            const QJsonValue candidateValue = candidateProblemSpec.value(pieces.at(0)).toObject().value(pieces.at(1));
            if (!candidateValue.isDouble() || !std::isfinite(candidateValue.toDouble())
                || candidateValue.toDouble() < minimum || candidateValue.toDouble() > maximum
                || (integerPath(path) && std::floor(candidateValue.toDouble()) != candidateValue.toDouble()))
                return {};
            QJsonObject expectedSection = expectedCandidateSpec.value(pieces.at(0)).toObject();
            expectedSection.insert(pieces.at(1), candidateValue);
            expectedCandidateSpec.insert(pieces.at(0), expectedSection);
            seenPaths.insert(path);
        }
        quint64 verifiedRepairs = 0;
        for (const QJsonValue &eventValue : record.value(QStringLiteral("history")).toArray())
            if (eventValue.toObject().value(QStringLiteral("event")).toString() == QLatin1String("repair-verified"))
                ++verifiedRepairs;
        if (verifiedRepairs != repairCount || expectedCandidateSpec != candidateProblemSpec) return {};
    }

    const QJsonObject verificationReport = record.value(QStringLiteral("verificationReport")).toObject();
    const QJsonObject reportIdentity = verificationReport.value(QStringLiteral("identity")).toObject();
    if (verificationReport.value(QStringLiteral("schema")).toString() != QLatin1String("range.verification-report/v1")
        || verificationReport.value(QStringLiteral("status")).toString() != QLatin1String("pass")
        || !verificationReport.value(QStringLiteral("passed")).toBool()) return {};
    for (const QString &key : {QStringLiteral("artifactId"), QStringLiteral("jobId"),
                               QStringLiteral("inputSnapshotSha256"), QStringLiteral("inputPackageSha256"),
                               QStringLiteral("sourceSnapshotRef")})
        if (reportIdentity.value(key) != solveIdentity.value(key)) return {};

    const QJsonObject job = SolverManager::getInstance().jobQuery(solveIdentity.value(QStringLiteral("jobId")).toString());
    const QJsonObject provenance = job.value(QStringLiteral("nativeResultProvenance")).toObject();
    const QJsonObject sourceSnapshotRef = provenance.value(QStringLiteral("sourceSnapshotRef")).toObject();
    const QString manifestPath = job.value(QStringLiteral("resultManifestRef")).toString();
    const QString manifestSha256 = job.value(QStringLiteral("resultManifestSha256")).toString();
    if (!job.value(QStringLiteral("found")).toBool()
        || job.value(QStringLiteral("jobId")) != solveIdentity.value(QStringLiteral("jobId"))
        || job.value(QStringLiteral("trustedExperimentId")).toString() != experimentId
        || job.value(QStringLiteral("trustedExperimentAttemptId")).toString() != attemptId
        || job.value(QStringLiteral("trustedAuthorizationDigest")).toString() != authorizationDigest
        || job.value(QStringLiteral("state")).toString() != QLatin1String("succeeded")
        || job.value(QStringLiteral("inputSnapshotSha256")) != solveIdentity.value(QStringLiteral("inputSnapshotSha256"))
        || job.value(QStringLiteral("inputPackageSha256")) != solveIdentity.value(QStringLiteral("inputPackageSha256"))
        || job.value(QStringLiteral("resultArtifactId")) != solveIdentity.value(QStringLiteral("artifactId"))
        || !job.value(QStringLiteral("resultArtifactVerified")).toBool()
        || !job.value(QStringLiteral("resultSourceBindingVerified")).toBool()
        || !job.value(QStringLiteral("resultManifestVerified")).toBool()
        || !job.value(QStringLiteral("nativeResultCatalogAvailable")).toBool()
        || job.value(QStringLiteral("eligibility")).toString() == QLatin1String("conflict")
        || !QFileInfo(manifestPath).isAbsolute() || !sha256Grammar.match(manifestSha256).hasMatch()
        || reportIdentity.value(QStringLiteral("manifestSha256")).toString() != manifestSha256
        || sourceSnapshotRef.isEmpty() || sourceSnapshotRef != solveIdentity.value(QStringLiteral("sourceSnapshotRef")).toObject()
        || sourceSnapshotRef.value(QStringLiteral("kind")).toString() != QLatin1String("trusted-experiment-input-snapshot/v1")
        || sourceSnapshotRef.value(QStringLiteral("inputDerivedFrom")).toString() != QLatin1String("trusted_experiment_candidate")
        || sourceSnapshotRef.value(QStringLiteral("trustedExperimentAuthorization")).toObject() != authorization
        || sourceSnapshotRef.value(QStringLiteral("inputSnapshotSha256")).toString()
            != solveIdentity.value(QStringLiteral("inputSnapshotSha256")).toString()
        || sourceSnapshotRef.value(QStringLiteral("inputPackageSha256")).toString()
            != solveIdentity.value(QStringLiteral("inputPackageSha256")).toString()
        || sourceSnapshotRef.value(QStringLiteral("modelSha256")).toString()
            != solveIdentity.value(QStringLiteral("inputSnapshotSha256")).toString()
        || sourceSnapshotRef.value(QStringLiteral("sourceRuntimeStamp")).toObject() != sourceRuntimeStamp
        || provenance.value(QStringLiteral("jobId")) != solveIdentity.value(QStringLiteral("jobId"))
        || provenance.value(QStringLiteral("inputSnapshotSha256")) != solveIdentity.value(QStringLiteral("inputSnapshotSha256"))
        || provenance.value(QStringLiteral("inputPackageSha256")) != solveIdentity.value(QStringLiteral("inputPackageSha256"))
        || job.value(QStringLiteral("sourceRuntimeStamp")).toObject() != sourceRuntimeStamp
        || provenance.value(QStringLiteral("sourceRuntimeStamp")).toObject() != sourceRuntimeStamp)
        return {};

    const bool includeConvergenceMeasurements = !authorizationPolicy.value(QStringLiteral("convergenceStudy")).toObject().isEmpty();
    const QJsonObject recomputedReport = RTrustedVerificationService::verify(candidateProblemSpecInput,
        solveIdentity, manifestPath, manifestSha256, includeConvergenceMeasurements);
    if (recomputedReport != verificationReport
        || recomputedReport.value(QStringLiteral("passed")).toBool() != record.value(QStringLiteral("verificationPassed")).toBool())
        return {};

    const QByteArray reportBytes = QJsonDocument(verificationReport).toJson(QJsonDocument::Compact);
    const QString reportSha256 = QString::fromLatin1(
        QCryptographicHash::hash(reportBytes, QCryptographicHash::Sha256).toHex());
    const QJsonObject childJob{{QStringLiteral("jobId"), job.value(QStringLiteral("jobId"))},
        {QStringLiteral("experimentId"), job.value(QStringLiteral("trustedExperimentId"))},
        {QStringLiteral("attemptId"), job.value(QStringLiteral("trustedExperimentAttemptId"))},
        {QStringLiteral("state"), job.value(QStringLiteral("state"))},
        {QStringLiteral("inputSnapshotSha256"), job.value(QStringLiteral("inputSnapshotSha256"))},
        {QStringLiteral("inputPackageSha256"), job.value(QStringLiteral("inputPackageSha256"))},
        {QStringLiteral("resultArtifactId"), job.value(QStringLiteral("resultArtifactId"))},
        {QStringLiteral("resultManifestRef"), manifestPath},
        {QStringLiteral("resultManifestSha256"), manifestSha256},
        {QStringLiteral("resultManifestVerified"), job.value(QStringLiteral("resultManifestVerified"))},
        {QStringLiteral("nativeResultProvenance"), QJsonObject{
            {QStringLiteral("sourceSnapshotRef"), sourceSnapshotRef}}}};
    const QJsonObject snapshot{{QStringLiteral("schema"), QStringLiteral("range.trusted-experiment-evidence-snapshot/v1")},
        {QStringLiteral("experimentId"), experimentId}, {QStringLiteral("attemptId"), attemptId},
        {QStringLiteral("authorizationDigest"), authorizationDigest},
        {QStringLiteral("sourceBinding"), sourceBinding}, {QStringLiteral("sourceRuntimeStamp"), sourceRuntimeStamp},
        {QStringLiteral("latestSolveIdentity"), solveIdentity}, {QStringLiteral("childJob"), childJob},
        {QStringLiteral("verificationReport"), verificationReport},
        {QStringLiteral("verificationReportSha256"), reportSha256}};
    if (QJsonDocument(snapshot).toJson(QJsonDocument::Compact).size() > 512 * 1024) return {};
    return snapshot;
}

MeshJobController *ModelCommandHost::meshController() const { return state->meshController; }

QJsonObject ModelCommandHost::handleReadOnly(const QJsonObject &request, const QJsonArray &pinnedDocuments)
{
    const auto requestId = request.value(QStringLiteral("requestId"));
    const auto denied = [&](const QString &code, const QString &message) {
        return hostRejected(requestId, code, message);
    };
    if (QThread::currentThread() != thread())
        return denied(QStringLiteral("WRONG_THREAD"), tr("GUI host reads require the GUI thread."));
    const QSet<QString> reads{QStringLiteral("capabilities.query"), QStringLiteral("context.query"),
        QStringLiteral("events.read"), QStringLiteral("project.query"), QStringLiteral("document.project.query"),
        QStringLiteral("document.targets.query"), QStringLiteral("document.inputs.query"),
        QStringLiteral("document.source.validate"), QStringLiteral("native.jobs.query"),
        QStringLiteral("native.job.query"), QStringLiteral("native.job.recovery.query"),
        QStringLiteral("native.result.catalog.query"), QStringLiteral("native.result.channels.query"),
        QStringLiteral("native.result.series.query"), QStringLiteral("native.result.summary.query"),
        QStringLiteral("operation.query"), QStringLiteral("operations.query"), QStringLiteral("mesh.job.query"),
        QStringLiteral("experiment.query"), QStringLiteral("analysis.inspect"), QStringLiteral("analysis.diagnose")};
    const QString operation = request.value(QStringLiteral("operation")).toString();
    if (!reads.contains(operation))
        return denied(QStringLiteral("READ_ONLY_DENIED"), tr("This question cannot prepare, approve or execute an action."));
    if (pinnedDocuments.size() > 256)
        return denied(QStringLiteral("INVALID_SOURCE_SNAPSHOT"), tr("The question source snapshot exceeds its limit."));
    QSet<QString> pinnedIds;
    const auto sourceIsCurrent = [&]() {
        if (pinnedDocuments.isEmpty()) return true;
        TrySessionLock guard(session);
        if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction()) return false;
        for (const auto &value : pinnedDocuments)
        {
            if (!value.isObject()) return false;
            const auto pin = value.toObject();
            const QString documentId = pin.value(QStringLiteral("documentId")).toString();
            const QString epoch = pin.value(QStringLiteral("sessionEpoch")).toString();
            quint64 revision = 0;
            if (documentId.isEmpty() || epoch != state->sessionEpoch
                || !unsignedInteger(pin.value(QStringLiteral("documentRevision")), &revision)) return false;
            const uint modelId = session->findModelByCommandIdentity(documentId);
            if (modelId == RConstants::eod || session->modelCommandStamp(modelId) != qMakePair(documentId, revision)) return false;
            const auto project = pin.value(QStringLiteral("project")).toObject();
            const auto currentProject = session->documentProjectContext(modelId);
            for (const auto &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
                                     QStringLiteral("modelSha256"), QStringLiteral("sourceRevision")})
                if (project.contains(field) && project.value(field) != currentProject.value(field)) return false;
        }
        return true;
    };
    for (const auto &value : pinnedDocuments)
    {
        const QString id = value.toObject().value(QStringLiteral("documentId")).toString();
        if (id.isEmpty() || pinnedIds.contains(id))
            return denied(QStringLiteral("INVALID_SOURCE_SNAPSHOT"), tr("The question source snapshot is invalid."));
        pinnedIds.insert(id);
    }
    if (!sourceIsCurrent())
        return denied(QStringLiteral("STALE_SOURCE_SNAPSHOT"), tr("The question source changed; ask again with a fresh context."));
    const auto parameters = request.value(QStringLiteral("parameters")).toObject();
    const QString targetId = parameters.value(QStringLiteral("target")).toObject().value(QStringLiteral("documentId")).toString();
    if (!pinnedIds.isEmpty() && !targetId.isEmpty() && !pinnedIds.contains(targetId))
        return denied(QStringLiteral("READ_ONLY_SCOPE_DENIED"), tr("The requested document is outside this question's source snapshot."));
    if (operation == "experiment.query" && !pinnedIds.isEmpty())
    {
        const auto loaded = state->experiments->query(parameters.value("experimentId").toString());
        const auto binding = loaded.data.value("experiment").toObject().value("authorization")
            .toObject().value("sourceBinding").toObject();
        if (!loaded.ok || !pinnedIds.contains(binding.value("documentId").toString()))
            return denied("READ_ONLY_SCOPE_DENIED", tr("The experiment is outside this question's source snapshot."));
    }
    QJsonObject response;
    if (operation == QStringLiteral("context.query") && parameters.isEmpty() && !pinnedDocuments.isEmpty())
    {
        QJsonArray documents, ids;
        QJsonObject context;
        for (const auto &pin : pinnedDocuments)
        {
            const QString id = pin.toObject().value(QStringLiteral("documentId")).toString();
            auto fixedRequest = request;
            fixedRequest.insert(QStringLiteral("parameters"), QJsonObject{{QStringLiteral("target"),
                QJsonObject{{QStringLiteral("documentId"), id}, {QStringLiteral("sessionEpoch"), state->sessionEpoch}}}});
            const auto previousSelection = state->lastSelection;
            response = handle(fixedRequest);
            state->lastSelection = previousSelection;
            if (response.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok"))) return response;
            context = response.value(QStringLiteral("result")).toObject();
            for (const auto &document : context.value(QStringLiteral("documents")).toArray()) documents.append(document);
            ids.append(id);
        }
        context.insert(QStringLiteral("documents"), documents);
        context.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("availability"), QStringLiteral("query-snapshot")},
            {QStringLiteral("documents"), ids}, {QStringLiteral("entities"), QJsonArray{}}});
        response.insert(QStringLiteral("result"), context);
    }
    else response = handle(request);
    if (!sourceIsCurrent())
        return denied(QStringLiteral("STALE_SOURCE_SNAPSHOT"), tr("The question source changed while it was being read."));
    response.insert(QStringLiteral("accessPolicy"), QStringLiteral("read-only"));
    if ((operation == QStringLiteral("analysis.inspect") || operation == QStringLiteral("analysis.diagnose")
         || operation == QStringLiteral("experiment.query"))
        && QJsonDocument(response).toJson(QJsonDocument::Compact).size() > trustedHostReadbackLimit)
        return denied(QStringLiteral("READBACK_LIMIT_EXCEEDED"), tr("Trusted analysis evidence exceeds the host readback limit."));
    return response;
}

QJsonObject ModelCommandHost::handle(const QJsonObject &request)
{
    const QJsonValue requestId = request.value(QStringLiteral("requestId"));
    QString durableIntentId;
    bool durableSideEffect = false;
    auto invalid = [&](const QString &code, const QString &message) {
        auto response = hostRejected(requestId, code, message);
        if (!durableIntentId.isEmpty())
        {
            auto *catalog = session->getProjectCatalog();
            RProjectCatalog::OperationAssociation association;
            association.diagnosticCode = code.toLower();
            association.observation = {{"status", "rejected"}, {"code", code}, {"result", QJsonObject{}}, {"message", message}};
            QString error;
            if (!catalog || !catalog->advanceOperation(durableIntentId, durableSideEffect
                    ? RProjectCatalog::OperationPhase::Unknown : RProjectCatalog::OperationPhase::Observed,
                    association, {}, &error))
                response.insert("durableObservation", "unavailable");
        }
        return response;
    };
    if (QThread::currentThread() != thread())
        return invalid(QStringLiteral("WRONG_THREAD"), tr("GUI host requests must be handled on the GUI thread."));
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 65536
        || !exactKeys(request, {QStringLiteral("apiVersion"), QStringLiteral("requestId"),
                                QStringLiteral("operation"), QStringLiteral("parameters")})
        || request.value(QStringLiteral("apiVersion")) != QJsonValue(QStringLiteral("range.host/v1"))
        || !validToken(requestId) || !request.value(QStringLiteral("operation")).isString()
        || !request.value(QStringLiteral("parameters")).isObject())
        return invalid(QStringLiteral("INVALID_REQUEST"), tr("Expected the exact range.host/v1 request envelope."));

    const QString operation = request.value(QStringLiteral("operation")).toString();
    const QJsonObject parameters = request.value(QStringLiteral("parameters")).toObject();
    auto ok = [&](const QJsonObject &result, const QString &status = QStringLiteral("ok")) {
        return hostResponse(requestId, status, QStringLiteral("OK"), result);
    };
    auto targetFieldsValid = [&](QString *documentId, QString *sessionEpoch) {
        if (!parameters.value(QStringLiteral("target")).isObject()) return false;
        const auto target = parameters.value(QStringLiteral("target")).toObject();
        if (!exactKeys(target, {QStringLiteral("documentId"), QStringLiteral("sessionEpoch")})
            || !validToken(target.value(QStringLiteral("documentId")))
            || !validToken(target.value(QStringLiteral("sessionEpoch")))) return false;
        *documentId = target.value(QStringLiteral("documentId")).toString();
        *sessionEpoch = target.value(QStringLiteral("sessionEpoch")).toString();
        return true;
    };
    auto ownedMeshAttempt = [&](const QString &attemptId, const QString &documentId,
                                const QString &epoch, quint64 revision) {
        if (epoch != state->sessionEpoch) return QJsonObject{};
        for (const auto &value : state->meshController->jobs())
        {
            const auto job = value.toObject();
            if (job.value("attemptId") != QJsonValue(attemptId)) continue;
            const auto source = job.value("sourceRuntimeStamp").toObject();
            if (source.value("documentId") == QJsonValue(documentId)
                && source.value("sessionEpoch") == QJsonValue(epoch)
                && source.value("documentRevision") == QJsonValue(double(revision))) return job;
        }
        return QJsonObject{};
    };
    if (operation == "mesh.job.query")
    {
        QString documentId, epoch; quint64 revision = 0;
        if (!exactKeys(parameters, {"target", "expectedDocumentRevision", "attemptId"})
            || !targetFieldsValid(&documentId, &epoch)
            || !unsignedInteger(parameters.value("expectedDocumentRevision"), &revision)
            || !validToken(parameters.value("attemptId")))
            return invalid("INVALID_PARAMETERS", tr("Mesh query requires its exact source target, revision and attemptId."));
        const QString attempt = parameters.value("attemptId").toString();
        if (ownedMeshAttempt(attempt, documentId, epoch, revision).isEmpty())
            return invalid("MESH_SOURCE_MISMATCH", tr("No owned mesh attempt matches this immutable source identity."));
        const auto queried = state->meshController->query(attempt);
        if (!queried.value("valid").toBool())
            return invalid(queried.value("code").toString(), queried.value("message").toString());
        return ok(meshReceipt(queried.value("result").toObject()));
    }
    auto describeResultView = [&](const QString &documentId, const QPair<QString, quint64> &stamp) {
        const auto found = state->resultDescriptors.find(documentId);
        if (found == state->resultDescriptors.end()) return QJsonObject{};
        QJsonObject descriptor = found->second;
        const QJsonObject openedStamp = descriptor.value(QStringLiteral("openedAtRuntimeStamp")).toObject();
        const bool modified = openedStamp.value(QStringLiteral("documentId")).toString() != stamp.first
            || openedStamp.value(QStringLiteral("sessionEpoch")).toString() != state->sessionEpoch
            || openedStamp.value(QStringLiteral("revision")).toString() != QString::number(stamp.second);
        const bool normalized = descriptor.value(QStringLiteral("normalizedAtOpen")).toBool();
        descriptor.insert(QStringLiteral("modifiedSinceOpen"), modified);
        descriptor.insert(QStringLiteral("dirtyStatus"), modified ? QStringLiteral("edited_after_open")
            : normalized ? QStringLiteral("normalized_at_open") : QStringLiteral("unchanged_since_open"));
        descriptor.insert(QStringLiteral("documentAvailability"), modified
            ? QStringLiteral("live_edited_session_state") : QStringLiteral("live_session_state"));
        descriptor.insert(QStringLiteral("currentRuntimeStamp"), sourceStampJson(stamp, state->sessionEpoch));
        return descriptor;
    };
    auto makeDocumentSnapshot = [&](const QString &documentId, uint modelID, bool liveRead,
                                    const QJsonObject &cachedState = QJsonObject{}) {
        const auto stamp = session->modelCommandStamp(modelID);
        QJsonObject snapshot{{QStringLiteral("documentId"), documentId},
                             {QStringLiteral("sessionEpoch"), state->sessionEpoch},
                             {QStringLiteral("documentRevision"), double(stamp.second)},
                             {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")},
                             {QStringLiteral("stampGeneration"), double(stamp.second)},
                             {QStringLiteral("stateAvailability"), liveRead ? QStringLiteral("live")
                                 : (cachedState.isEmpty() ? QStringLiteral("unavailable") : QStringLiteral("cached"))},
                             {QStringLiteral("stateFreshness"), liveRead ? QStringLiteral("locked-live")
                                 : (cachedState.isEmpty() ? QStringLiteral("unknown") : QStringLiteral("last-known-only"))},
                             {QStringLiteral("draft"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")},
                                 {QStringLiteral("reason"), QStringLiteral("The GUI does not expose uncommitted form drafts to this host.")}}},
                             {QStringLiteral("unsaved"), QJsonObject{{QStringLiteral("status"), QStringLiteral("unknown")},
                                 {QStringLiteral("reason"), QStringLiteral("The host has no authoritative file-dirty state API.")}}}};
        QJsonObject modal;
        QString modelName;
        bool canConfigureModal = false;
        bool canUndo = false;
        bool canRedo = false;
        bool historyAvailable = liveRead;
        QString modalSessionRevision;
        if (liveRead)
        {
            const auto before = session->modelCommandStamp(modelID);
            const auto &model = session->getModel(modelID);
            modal = RModelCommandSession::modalParameters(model.getProblemSetup().getModalSetup());
            modelName = model.getName();
            canConfigureModal = bool(model.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL);
            canUndo = model.getUndoStackSize() > 0;
            canRedo = model.getRedoStackSize() > 0;
            const auto entry = state->entries.find(documentId);
            if (entry != state->entries.end() && entry->second.editing && entry->second.generation == stamp.second)
                modalSessionRevision = entry->second.editing->query().value(QStringLiteral("revision")).toString();
            const auto after = session->modelCommandStamp(modelID);
            if (before.first != documentId || before.second != after.second)
            {
                modal = {};
                modelName.clear();
                canConfigureModal = canUndo = canRedo = false;
                historyAvailable = false;
                snapshot[QStringLiteral("stateAvailability")] = QStringLiteral("unavailable");
                snapshot[QStringLiteral("stateFreshness")] = QStringLiteral("unknown");
            }
        }
        else if (!cachedState.isEmpty())
        {
            modal = cachedState.value(QStringLiteral("modal")).toObject();
            canConfigureModal = cachedState.value(QStringLiteral("canConfigureModal")).toBool();
            modalSessionRevision = cachedState.value(QStringLiteral("revision")).toString();
            historyAvailable = false;
        }
        if (!modalSessionRevision.isEmpty()) snapshot.insert(QStringLiteral("modalSessionRevision"), modalSessionRevision);
        if (!modelName.isEmpty()) snapshot.insert(QStringLiteral("modelName"), modelName);
        QJsonArray supported;
        if (canConfigureModal)
            supported.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.modal.configure")},
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("requiresPreview"), true},
                                         {QStringLiteral("requiresHostApproval"), true}});
        if (historyAvailable && canUndo)
            supported.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("document.undo")},
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("requiresPreview"), true},
                                         {QStringLiteral("requiresHostApproval"), true}});
        if (historyAvailable && canRedo)
            supported.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("document.redo")},
                                         {QStringLiteral("version"), 1},
                                         {QStringLiteral("requiresPreview"), true},
                                         {QStringLiteral("requiresHostApproval"), true}});
        QJsonObject acceptedState{{QStringLiteral("scope"), QStringLiteral("analysis.modal")},
                                  {QStringLiteral("available"), !modal.isEmpty()},
                                  {QStringLiteral("supportedOperations"), supported}};
        if (!modal.isEmpty()) acceptedState.insert(QStringLiteral("modal"), modal);
        snapshot.insert(QStringLiteral("acceptedState"), acceptedState);
        snapshot.insert(QStringLiteral("history"), QJsonObject{
            {QStringLiteral("availability"), historyAvailable ? QStringLiteral("live") : QStringLiteral("unknown")},
            {QStringLiteral("canUndo"), historyAvailable ? QJsonValue(canUndo) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("canRedo"), historyAvailable ? QJsonValue(canRedo) : QJsonValue(QJsonValue::Null)}});
        snapshot.insert(QStringLiteral("runtimeStamp"), QJsonObject{
            {QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("revision"), QString::number(stamp.second)},
            {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")}});
        const QJsonObject project = session->documentProjectContext(modelID);
        snapshot.insert(QStringLiteral("project"), project);
        snapshot.insert(QStringLiteral("persistentProject"), project);
        const QJsonObject resultDescriptor = describeResultView(documentId, stamp);
        if (!resultDescriptor.isEmpty()) snapshot.insert(QStringLiteral("resultView"), resultDescriptor);
        if (liveRead)
        {
            if (project.value(QStringLiteral("draft")).isObject())
                snapshot.insert(QStringLiteral("draft"), project.value(QStringLiteral("draft")));
            if (project.value(QStringLiteral("unsaved")).isObject())
                snapshot.insert(QStringLiteral("unsaved"), project.value(QStringLiteral("unsaved")));
        }
        return snapshot;
    };

    auto readFixedDocument = [&](const QString &documentId, RModel *model,
                                 QPair<QString, quint64> *stamp, uint *modelID,
                                 bool *liveRead, QString *message) {
        uint current = session->findModelByCommandIdentity(documentId);
        if (current == RConstants::eod)
        {
            if (message) *message = tr("The target document is no longer open.");
            return false;
        }
        QPair<QString, quint64> currentStamp = session->modelCommandStamp(current);
        if (currentStamp.first != documentId)
        {
            if (message) *message = tr("The target document identity changed while it was being read.");
            return false;
        }
        TrySessionLock guard(session);
        if (guard.ownsLock())
        {
            current = session->findModelByCommandIdentity(documentId);
            if (current == RConstants::eod)
            {
                if (message) *message = tr("The target document was closed while it was being read.");
                return false;
            }
            currentStamp = session->modelCommandStamp(current);
            if (currentStamp.first != documentId) return false;
            *model = session->getModel(current);
            if (session->modelCommandStamp(current) != currentStamp)
            {
                if (message) *message = tr("The target document changed while it was being read.");
                return false;
            }
            guard.unlock();
            *stamp = currentStamp;
            *modelID = current;
            *liveRead = true;
            return true;
        }
        const auto entry = state->entries.find(documentId);
        if (entry == state->entries.end() || !entry->second.editing
            || entry->second.generation != currentStamp.second)
        {
            if (message) *message = tr("The live document is busy and no current cached snapshot is available.");
            return false;
        }
        *model = entry->second.editing->model();
        *stamp = currentStamp;
        *modelID = current;
        *liveRead = false;
        return true;
    };

    const QJsonArray hostCommands{
        QJsonObject{{"id", "experiment.run"}, {"version", 1}, {"requiresPreview", true},
            {"requiresHostApproval", true}, {"history", "none"},
            {"arguments", QJsonObject{{"problemSpec", "trusted mechanics ProblemSpec"},
                {"allowedNumericalRanges", "optional array"}, {"budgets", "required object: solves [1,8], repairs [0,3], modelTurns [1,12]"},
                {"stopConditions", "optional object"},
                {"convergenceStudy", "optional bounded mesh or timeStep study"}}}, {"sourceDocumentModified", false}},
        QJsonObject{{"id", "mesh.generate"}, {"version", 1}, {"requiresPreview", true},
            {"requiresHostApproval", true}, {"requiresManualApproval", true}, {"history", "none"},
            {"arguments", QJsonObject{{"unit", "m"}, {"volumeUnit", "m^3"}, {"maxVolume", "number [1e-15,1e18]"},
                {"radiusEdgeRatio", "number [1.2,2.0]"}}}},
        QJsonObject{{"id", "mesh.accept"}, {"version", 1}, {"requiresPreview", true},
            {"requiresHostApproval", true}, {"requiresManualApproval", true}, {"history", "none"},
            {"arguments", QJsonObject{{"attemptId", "owned mesh attempt bound to exact source target/revision"}}},
            {"acceptanceMode", "new_document_from_current_source"}, {"requiresRebinding", true}},
        QJsonObject{{"id", "mesh.cancel"}, {"version", 1}, {"requiresPreview", true},
            {"requiresHostApproval", true}, {"requiresManualApproval", true}, {"history", "none"},
            {"arguments", QJsonObject{{"attemptId", "owned queued/running mesh attempt bound to exact original source"}}}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.run")},
            {QStringLiteral("version"), 1}, {QStringLiteral("requiresPreview"), true},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("warningsAccepted"), QStringLiteral("boolean")}}},
            {QStringLiteral("history"), QStringLiteral("none")},
            {QStringLiteral("sideEffect"), QStringLiteral("submit_native_solver_job")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.cancel")},
            {QStringLiteral("version"), 1}, {QStringLiteral("requiresPreview"), true},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("jobId"), QStringLiteral("registered native job ID")}}},
            {QStringLiteral("history"), QStringLiteral("none")},
            {QStringLiteral("sideEffect"), QStringLiteral("cancel_specific_native_job")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.recover")},
            {QStringLiteral("version"), 1}, {QStringLiteral("requiresPreview"), true},
            {QStringLiteral("requiresHostApproval"), true},
            {QStringLiteral("arguments"), QJsonObject{{"jobId", "registered interrupted job ID"},
                {"checkpointSha256", "verified accepted checkpoint SHA-256"}, {"remainingSteps", "frozen remaining range"}}},
            {QStringLiteral("history"), QStringLiteral("none")},
            {QStringLiteral("sideEffect"), QStringLiteral("submit_detached_native_recovery_attempt")}}};

    auto nativeJob = [&](const QString &jobId) {
        return SolverManager::getInstance().jobQuery(jobId);
    };
    auto addHostJobSource = [&](QJsonObject *job, const QString &jobId) {
        const QString jobState = job->value(QStringLiteral("state")).toString();
        job->insert(QStringLiteral("progressState"), job->value(QStringLiteral("progressStatus")));
        job->insert(QStringLiteral("cancellationState"),
            jobState == QStringLiteral("queued") || jobState == QStringLiteral("running")
                ? QStringLiteral("cancellable_by_jobId")
                : jobState == QStringLiteral("cancelling") ? QStringLiteral("cancellation_requested")
                                                            : QStringLiteral("not_cancellable_terminal"));
        const QJsonObject immutableStamp = job->value(QStringLiteral("sourceRuntimeStamp")).toObject();
        const auto source = state->jobSources.find(jobId);
        if (source != state->jobSources.end())
        {
            if (source->second.documentId != immutableStamp.value(QStringLiteral("documentId")).toString()
                || source->second.revision != immutableStamp.value(QStringLiteral("revision")).toString()
                || (!immutableStamp.value(QStringLiteral("sessionEpoch")).toString().isEmpty()
                    && source->second.sessionEpoch != immutableStamp.value(QStringLiteral("sessionEpoch")).toString())) return;
            job->insert(QStringLiteral("hostSource"), QJsonObject{
                {QStringLiteral("documentId"), source->second.documentId},
                {QStringLiteral("sessionEpoch"), source->second.sessionEpoch},
                {QStringLiteral("revision"), source->second.revision}});
            return;
        }
        const QString immutableEpoch = immutableStamp.value(QStringLiteral("sessionEpoch")).toString();
        if (immutableEpoch == state->sessionEpoch)
            job->insert(QStringLiteral("hostSource"), QJsonObject{
                {QStringLiteral("documentId"), immutableStamp.value(QStringLiteral("documentId"))},
                {QStringLiteral("sessionEpoch"), immutableEpoch},
                {QStringLiteral("revision"), immutableStamp.value(QStringLiteral("revision"))}});
    };

    if (operation == QStringLiteral("document.targets.query")
        || operation == QStringLiteral("document.inputs.query"))
    {
        const bool hasPage = parameters.contains(QStringLiteral("page"));
        if ((!exactKeys(parameters, {QStringLiteral("target")})
             && !exactKeys(parameters, {QStringLiteral("target"), QStringLiteral("page")}))
            || (hasPage && !parameters.value(QStringLiteral("page")).isObject()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A fixed document target and optional page object are required."));
        QString documentId, epoch;
        if (!targetFieldsValid(&documentId, &epoch))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A runtime document target is required."));
        if (epoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The target belongs to a different host session."));
        RModel model;
        QPair<QString, quint64> stamp;
        uint modelID = RConstants::eod;
        bool liveRead = false;
        QString readError;
        if (!readFixedDocument(documentId, &model, &stamp, &modelID, &liveRead, &readError))
            return invalid(QStringLiteral("DOCUMENT_UNAVAILABLE"), readError);
        const QJsonObject page = parameters.value(QStringLiteral("page")).toObject();
        QJsonObject result = operation == QStringLiteral("document.targets.query")
            ? RModelCommandService::targets(model, documentId, QString::number(stamp.second), page)
            : RModelCommandService::context(model, documentId, QString::number(stamp.second), page);
        if (result.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok")))
            return invalid(result.value(QStringLiteral("code")).toString().toUpper(),
                           result.value(QStringLiteral("message")).toString());
        result.insert(QStringLiteral("target"), QJsonObject{
            {QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch}});
        result.insert(QStringLiteral("runtimeStamp"), sourceStampJson(stamp, state->sessionEpoch));
        result.insert(QStringLiteral("stateAvailability"), liveRead ? QStringLiteral("live") : QStringLiteral("cached"));
        result.insert(QStringLiteral("stateFreshness"), liveRead ? QStringLiteral("locked-live") : QStringLiteral("last-known-only"));
        result.insert(QStringLiteral("persistentProject"), session->documentProjectContext(modelID));
        result.insert(QStringLiteral("hostCommands"), hostCommands);
        const QJsonObject descriptor = describeResultView(documentId, stamp);
        if (!descriptor.isEmpty()) result.insert(QStringLiteral("resultView"), descriptor);
        return ok(result);
    }

    if (operation == QStringLiteral("document.source.validate"))
    {
        if (!exactKeys(parameters, {QStringLiteral("snapshotRef")}) || !parameters.value("snapshotRef").isObject())
            return invalid("INVALID_PARAMETERS", tr("An immutable source reference is required."));
        const auto origin = parameters.value("snapshotRef").toObject();
        if (!exactKeys(origin, {"projectId", "persistentDocumentId", "sourceRevision", "modelSha256", "instanceRef"})
            || !origin.value("instanceRef").isObject())
            return invalid("INVALID_PARAMETERS", tr("The source reference has unsupported fields."));
        const auto instance = origin.value("instanceRef").toObject();
        quint64 revision = 0;
        if (!exactKeys(instance, {"documentId", "sessionEpoch", "documentRevision"})
            || !validToken(instance.value("documentId")) || !validToken(instance.value("sessionEpoch"))
            || !unsignedInteger(instance.value("documentRevision"), &revision))
            return invalid("INVALID_PARAMETERS", tr("A valid source runtime stamp is required."));
        if (instance.value("sessionEpoch").toString() != state->sessionEpoch)
            return invalid("STALE_SESSION", tr("The source document belongs to an earlier session."));
        const uint modelID = session->findModelByCommandIdentity(instance.value("documentId").toString());
        if (modelID >= session->getNModels()) return invalid("DOCUMENT_CLOSED", tr("The source document closed."));
        if (!session->trylock(0)) return invalid("MODEL_BUSY", tr("The source document state is busy."));
        const auto stamp = session->modelCommandStamp(modelID);
        const auto context = session->documentProjectContext(modelID);
        const bool draft = session->hasDocumentDraft(modelID);
        session->unlock();
        if (context.value("availability") != QJsonValue("available")
            || context.value("persistence").toObject().value("status") != QJsonValue("available"))
            return invalid("SOURCE_UNAVAILABLE", tr("The source project has a conflict or unavailable persistence state."));
        if (stamp.second != revision || draft)
            return invalid("STALE_SOURCE", tr("The source document was edited or has an unresolved draft."));
        for (const QString &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
                                    QStringLiteral("sourceRevision"), QStringLiteral("modelSha256")})
            if (!origin.value(field).isString() || origin.value(field).toString().isEmpty() || context.value(field) != origin.value(field))
                return invalid("STALE_SOURCE", tr("The persistent source version or hash changed."));
        return ok({{"valid", true}, {"snapshotRef", origin}});
    }

    if (operation == QStringLiteral("native.jobs.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("offset"), QStringLiteral("limit")}))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("native.jobs.query requires offset and limit."));
        quint64 offset = 0, limit = 0;
        if (!unsignedInteger(parameters.value(QStringLiteral("offset")), &offset)
            || !unsignedInteger(parameters.value(QStringLiteral("limit")), &limit)
            || offset > quint64(std::numeric_limits<int>::max()) || limit < 1 || limit > 200)
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("Job pagination requires offset >= 0 and limit from 1 through 200."));
        QJsonObject result = SolverManager::getInstance().jobsQuery(int(offset), int(limit));
        if (!result.value(QStringLiteral("error")).toString().isEmpty())
            return invalid(result.value(QStringLiteral("error")).toString().toUpper(),
                           tr("The native job page could not be read."));
        QJsonArray jobs = result.value(QStringLiteral("jobs")).toArray();
        for (qsizetype i = 0; i < jobs.size(); ++i)
        {
            QJsonObject job = jobs.at(i).toObject();
            addHostJobSource(&job, job.value(QStringLiteral("jobId")).toString());
            jobs.replace(i, job);
        }
        result.insert(QStringLiteral("jobs"), jobs);
        return ok(result);
    }

    if (operation == QStringLiteral("native.job.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("jobId")})
            || !validToken(parameters.value(QStringLiteral("jobId"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("native.job.query requires a jobId."));
        const QString jobId = parameters.value(QStringLiteral("jobId")).toString();
        QJsonObject job = nativeJob(jobId);
        if (!job.value(QStringLiteral("found")).toBool())
            return invalid(QStringLiteral("JOB_NOT_FOUND"), tr("No native job with this ID is registered."));
        addHostJobSource(&job, jobId);
        return ok({{QStringLiteral("job"), job}});
    }
    if (operation == QStringLiteral("native.job.recovery.query"))
    {
        if (!exactKeys(parameters, {QStringLiteral("jobId")}) || !validToken(parameters.value("jobId")))
            return invalid("INVALID_PARAMETERS", tr("Native recovery query requires a registered jobId."));
        auto qualification = SolverManager::getInstance().nativeJobRecoveryQuery(parameters.value("jobId").toString());
        if (!qualification.value("found").toBool()) return invalid("JOB_NOT_FOUND", tr("No native job with this ID is registered."));
        qualification.remove("checkpointPath");
        qualification.remove("checkpointDirectory");
        return ok(qualification);
    }

    if (operation == QStringLiteral("native.result.catalog.query")
        || operation == QStringLiteral("native.result.channels.query")
        || operation == QStringLiteral("native.result.series.query")
        || operation == QStringLiteral("native.result.summary.query"))
    {
        if (!validToken(parameters.value(QStringLiteral("jobId"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A registered native jobId is required."));
        QJsonObject arguments = parameters;
        const QString jobId = arguments.take(QStringLiteral("jobId")).toString();
        const QString queryOperation = operation.section(QLatin1Char('.'), 2, 2);
        const QJsonObject result = SolverManager::getInstance().nativeResultQuery(jobId, queryOperation, arguments);
        if (!result.value(QStringLiteral("valid")).toBool())
            return invalid(result.value(QStringLiteral("code")).toString(), result.value(QStringLiteral("message")).toString());
        return ok(result);
    }

    if (operation == QStringLiteral("native.result.display"))
    {
        if (!exactKeys(parameters, {"jobId", "artifactId", "channelId"})
            || !validToken(parameters.value("jobId")) || !validToken(parameters.value("artifactId"))
            || !validToken(parameters.value("channelId")))
            return invalid("INVALID_PARAMETERS", tr("native.result.display requires jobId, artifactId and channelId only."));
        if (activeModalInteraction())
            return invalid("INTERACTION_BLOCKED", tr("Close the active modal interaction before displaying a solver result."));
        if (isMutationBlocked()) return invalid("MODEL_BUSY", tr("Wait for live model tasks before displaying a solver result."));
        const QString jobId = parameters.value("jobId").toString();
        const QString artifactId = parameters.value("artifactId").toString();
        const QJsonObject job = nativeJob(jobId);
        if (!job.value("found").toBool()) return invalid("JOB_NOT_FOUND", tr("No native job with this ID is registered."));
        if (job.value("state").toString() != "succeeded" || !job.value("resultReadbackComplete").toBool()
            || !job.value("resultArtifactVerified").toBool())
            return invalid("RESULT_UNAVAILABLE", tr("Display requires a verified completed native job."));
        SolverManager &manager = SolverManager::getInstance();
        const QJsonObject catalog = manager.nativeResultQuery(jobId, "catalog", {{"artifactId", artifactId}, {"offset", 0}, {"limit", 1}});
        if (!catalog.value("valid").toBool()) return invalid(catalog.value("code").toString(), catalog.value("message").toString());
        if (catalog.value("artifactId").toString() != artifactId || catalog.value("jobId").toString() != jobId)
            return invalid("ARTIFACT_IDENTITY_MISMATCH", tr("The catalog does not match the requested job and artifact."));
        if (job.value("resultArtifactId").toString() != artifactId)
            return invalid("ARTIFACT_IDENTITY_MISMATCH", tr("Field display requires the registered terminal artifact, not an earlier live snapshot."));
        const QJsonObject summary = manager.nativeResultQuery(jobId, "summary",
            {{"artifactId", artifactId}, {"channelId", parameters.value("channelId")}});
        if (!summary.value("valid").toBool()) return invalid(summary.value("code").toString(), summary.value("message").toString());
        if (summary.value("artifactId").toString() != artifactId || summary.value("channelId") != parameters.value("channelId"))
            return invalid("ARTIFACT_IDENTITY_MISMATCH", tr("The physical channel does not match the requested artifact."));
        const QMap<QString, RVariableType> supported{
            {"var-displacement", R_VARIABLE_DISPLACEMENT}, {"var-velocity", R_VARIABLE_VELOCITY},
            {"var-acceleration", R_VARIABLE_ACCELERATION}, {"var-rotation", R_VARIABLE_ROTATION},
            {"var-angular_velocity", R_VARIABLE_ANGULAR_VELOCITY}, {"var-angular_acceleration", R_VARIABLE_ANGULAR_ACCELERATION}};
        const RVariableType variableType = supported.value(summary.value("variableId").toString(), R_VARIABLE_NONE);
        if (variableType == R_VARIABLE_NONE) return invalid("UNSUPPORTED_CHANNEL", tr("This physical channel has no supported native field display."));
        QJsonObject openRequest = request;
        openRequest.insert("operation", "native.result.open");
        openRequest.insert("parameters", QJsonObject{{"jobId", jobId}, {"expectedSHA", job.value("resultArtifactSha256")}});
        const QJsonObject opened = handle(openRequest);
        if (opened.value("status").toString() != "accepted") return opened;
        QJsonObject result = opened.value("result").toObject();
        const QString documentId = result.value("documentId").toString();
        const uint openedModelID = session->findModelByCommandIdentity(documentId);
        if (openedModelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CLOSED", tr("The result document closed during opening."));
        const quint64 revision = session->modelCommandStamp(openedModelID).second;
        auto currentModel = [&]() -> uint {
            if (isMutationBlocked() || activeModalInteraction()) return RConstants::eod;
            const uint id = session->findModelByCommandIdentity(documentId);
            if (id == RConstants::eod || session->modelCommandStamp(id).second != revision) return RConstants::eod;
            return id;
        };
        // Selection emits synchronous GUI signals: never retain a Model reference
        // or an index across them, and reject a closed or edited result document.
        QStringList selectedDocuments;
        for (uint id : session->getSelectedModelIDs()) selectedDocuments.append(session->modelCommandStamp(id).first);
        for (const QString &selected : selectedDocuments)
        {
            if (currentModel() == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during selection."));
            const uint id = session->findModelByCommandIdentity(selected);
            if (id != RConstants::eod && selected != documentId) session->setModelSelected(id, false);
        }
        uint modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during selection."));
        session->setModelSelected(modelID, true);
        modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during selection."));
        uint nodeID = RConstants::eod;
        uint groupID = RConstants::eod;
        PickItem picked;
        {
            Model &model = session->getModel(modelID);
            const uint physicalId = uint(summary.value("nodePhysicalId").toDouble());
            for (uint i = 0; i < model.getNNodes(); ++i)
                if (model.getNodePhysicalId(i) == physicalId) { nodeID = i; break; }
            if (nodeID == RConstants::eod) return invalid("NODE_NOT_FOUND", tr("The physical node is absent from the verified result document."));
            if (model.findVariable(variableType) == RConstants::eod)
                return invalid("FIELD_UNAVAILABLE", tr("The terminal result document has no requested field."));
            for (uint i = 0; i < model.getNEntityGroups(true) && groupID == RConstants::eod; ++i)
            {
                const auto *group = static_cast<const RElementGroup *>(model.getEntityGroupPtr(i, true));
                for (uint j = 0; j < group->size() && groupID == RConstants::eod; ++j)
                {
                    const uint elementID = group->get(j);
                    const RElement &element = model.getElement(elementID);
                    for (uint k = 0; k < element.size(); ++k)
                        if (element.getNodeId(k) == nodeID)
                        {
                            groupID = i;
                            picked = PickItem(SessionEntityID(modelID, model.getEntityGroupType(i, true),
                                model.getEntityGroupPosition(i, true)), elementID, j, nodeID, k);
                            break;
                        }
                }
            }
            if (groupID == RConstants::eod) return invalid("NODE_NOT_DISPLAYABLE", tr("The physical node has no incident native entity group."));
            auto &data = model.getEntityGroupPtr(groupID, true)->getData();
            data.setVisible(true);
            data.clearVariableDisplayType(R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR);
            data.clearVariableDisplayType(R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT);
            data.getVariableData()[variableType] |= R_ENTITY_GROUP_VARIABLE_DISPLAY_SCALAR;
            if (variableType == R_VARIABLE_DISPLACEMENT)
                data.getVariableData()[variableType] |= R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT;
        }
        session->getPickList().clear();
        modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during highlighting."));
        // PickList signals can also reorder Session storage.
        const SessionEntityID entityID(modelID, picked.getEntityID().getType(), picked.getEntityID().getEid());
        session->getPickList().addItem(PickItem(entityID, picked.getElementID(), picked.getElementPosition(), nodeID, picked.getNodePosition()));
        modelID = currentModel();
        if (modelID == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during highlighting."));
        // Only display masks changed. variableDataChanged invalidates the
        // accepted document revision and is reserved for variable edits.
        session->setDisplayPropertiesChanged(modelID, entityID.getType(), entityID.getEid());
        if (currentModel() == RConstants::eod) return invalid("RESULT_DOCUMENT_CHANGED", tr("The result document changed during field display."));
        result.insert("artifactId", artifactId);
        result.insert("channelId", parameters.value("channelId"));
        result.insert("nodePhysicalId", summary.value("nodePhysicalId"));
        result.insert("variableId", summary.value("variableId"));
        result.insert("unit", summary.value("unit"));
        result.insert("coordinateSystem", "global");
        result.insert("selectedChannelAxis", summary.value("axis"));
        result.insert("fieldDisplayScope", "full-variable-magnitude");
        result.insert("fieldDisplayEntityScope", "incident-entity-group");
        result.insert("displacementGeometry", variableType == R_VARIABLE_DISPLACEMENT);
        result.insert("message", tr("Displayed the full native field magnitude and highlighted the physical node; the selected channel component remains a separate series."));
        return ok(result, "accepted");
    }

    if (operation == QStringLiteral("native.result.open"))
    {
        if (activeModalInteraction())
            return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before opening a solver result document."));
        if (isMutationBlocked())
            return invalid(QStringLiteral("MODEL_BUSY"), tr("A live model task is using Session storage; wait before opening a result document."));
        if (!exactKeys(parameters, {QStringLiteral("jobId"), QStringLiteral("expectedSHA")})
            || !validToken(parameters.value(QStringLiteral("jobId")))
            || !parameters.value(QStringLiteral("expectedSHA")).isString())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("native.result.open requires jobId and expectedSHA."));
        const QString jobId = parameters.value(QStringLiteral("jobId")).toString();
        const QString expectedSha = parameters.value(QStringLiteral("expectedSHA")).toString();
        static const QRegularExpression shaGrammar(QStringLiteral("^[0-9a-f]{64}$"));
        if (!shaGrammar.match(expectedSha).hasMatch())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("expectedSHA must be a lowercase SHA-256 digest."));
        QJsonObject job = nativeJob(jobId);
        if (!job.value(QStringLiteral("found")).toBool())
            return invalid(QStringLiteral("JOB_NOT_FOUND"), tr("No native job with this ID is registered."));
        addHostJobSource(&job, jobId);
        if (job.value(QStringLiteral("state")).toString() != QStringLiteral("succeeded")
            || !job.value(QStringLiteral("resultReadbackComplete")).toBool()
            || !job.value(QStringLiteral("resultArtifactVerified")).toBool())
            return invalid(QStringLiteral("RESULT_UNAVAILABLE"), tr("The native job has no verified terminal result artifact."));
        const QString registeredPath = job.value(QStringLiteral("resultArtifactRef")).toString();
        const QString registeredSha = job.value(QStringLiteral("resultArtifactSha256")).toString();
        if (registeredPath.isEmpty() || registeredSha != expectedSha)
            return invalid(QStringLiteral("ARTIFACT_IDENTITY_MISMATCH"), tr("The expected SHA does not match the immutable registered result identity."));
        QString sourceError;
        const auto beforeBinding = RProjectCatalog::resolveSourceBinding(registeredPath, &sourceError);
        const QJsonObject registeredBinding = job.value(QStringLiteral("resultSourceBinding")).toObject();
        if (!beforeBinding || beforeBinding->sourceArtifacts.isEmpty()
            || beforeBinding->sourceArtifacts.first().sha256 != expectedSha
            || nativeSourceBindingJson(*beforeBinding) != registeredBinding
            || !nativeSourceFilesMatch(*beforeBinding))
            return invalid(QStringLiteral("ARTIFACT_CHANGED"), sourceError.isEmpty()
                ? tr("The native result source chain no longer matches its immutable registered identity.") : sourceError);

        const QString publicationKey=beforeBinding->logicalSourcePath+QLatin1Char('\n')+expectedSha;
        const auto unknownPublication=[&](const QString &documentId,const QString &message) {
            const QJsonObject uncertainty{{QStringLiteral("documentId"),documentId},
                {QStringLiteral("documentMayExist"),true},{QStringLiteral("artifactRef"),registeredPath},
                {QStringLiteral("artifactSHA256"),expectedSha},{QStringLiteral("message"),message}};
            state->resultPublicationUnknown.insert(publicationKey,uncertainty);
            // A failed final callback check must also retire any optimistic
            // descriptor prepared before emitting the publication event.
            state->resultDescriptors.erase(documentId);
            for (auto it=state->resultDocuments.begin(); it!=state->resultDocuments.end();)
                if (it->second.documentId==documentId) it=state->resultDocuments.erase(it);
                else ++it;
            return hostResponse(requestId,QStringLiteral("unknown"),QStringLiteral("RESULT_OPEN_UNKNOWN"),uncertainty);
        };
        if (state->resultPublicationUnknown.contains(publicationKey))
            return hostResponse(requestId,QStringLiteral("unknown"),QStringLiteral("RESULT_OPEN_UNKNOWN"),
                                state->resultPublicationUnknown.value(publicationKey));
        if (state->resultPublicationsInFlight.contains(publicationKey))
            return invalid(QStringLiteral("RESULT_OPEN_IN_PROGRESS"),tr("This verified result is already being published."));
        // Keep the lease through reuse, every publication event, and final
        // reconciliation; a callback cannot open the same artifact mid-check.
        state->resultPublicationsInFlight.insert(publicationKey);
        auto clearInFlight=qScopeGuard([this,publicationKey] { state->resultPublicationsInFlight.remove(publicationKey); });

        QString resultDocumentId;
        bool reused = false;
        bool normalizedAtOpen = false;
        quint64 openedAtRevision = 0;
        if (!session->trylock(0))
            return invalid(QStringLiteral("MODEL_BUSY"), tr("The session is busy and cannot open a result document now."));
        if (isMutationBlocked())
        {
            session->unlock();
            return invalid(QStringLiteral("MODEL_BUSY"), tr("A live model task is using Session storage; wait before opening a result document."));
        }
        // Reuse only a result document whose current Session revision still
        // equals the revision captured when this host opened it.
        for (const auto &[knownJobId, recorded] : state->resultDocuments)
        {
            Q_UNUSED(knownJobId)
            if (recorded.artifactPath != beforeBinding->logicalSourcePath
                || recorded.artifactSha256 != expectedSha) continue;
            const uint currentID = session->findModelByCommandIdentity(recorded.documentId);
            if (currentID == RConstants::eod) continue;
            const auto currentStamp = session->modelCommandStamp(currentID);
            if (currentStamp.second == recorded.openedAtRevision)
            {
                resultDocumentId = recorded.documentId;
                openedAtRevision = recorded.openedAtRevision;
                normalizedAtOpen = recorded.normalizedAtOpen;
                reused = true;
                break;
            }
        }
        // An unchanged native document opened outside this host is reusable
        // only while it is still at its initial runtime revision.
        if (resultDocumentId.isEmpty())
        {
            for (uint i = 0; i < session->getNModels(); ++i)
            {
                const auto stamp = session->modelCommandStamp(i);
                if (stamp.first.isEmpty() || state->resultDescriptors.find(stamp.first) != state->resultDescriptors.end())
                    continue;
                const Model &existing = session->getModel(i);
                const auto &binding = existing.getLoadedSourceBinding();
                if (stamp.second == 0 && binding && *binding == *beforeBinding
                    && !existing.hasNativeReadNormalization())
                {
                    resultDocumentId = stamp.first;
                    openedAtRevision = stamp.second;
                    reused = true;
                    break;
                }
            }
        }
        session->unlock();

        if (resultDocumentId.isEmpty())
        {
            Model model;
            RProjectCatalog::SourceBinding loadedBinding;
            try
            {
                ModelIO::open(model, registeredPath, &loadedBinding);
            }
            catch (const RError &failure)
            {
                return invalid(QStringLiteral("RESULT_OPEN_FAILED"), failure.getMessage());
            }
            normalizedAtOpen = model.hasNativeReadNormalization();
            QString afterError;
            const auto afterBinding = RProjectCatalog::resolveSourceBinding(registeredPath, &afterError);
            if (!afterBinding || *afterBinding != *beforeBinding || loadedBinding != *beforeBinding
                || nativeSourceBindingJson(*afterBinding) != registeredBinding
                || !nativeSourceFilesMatch(*afterBinding))
                return invalid(QStringLiteral("ARTIFACT_CHANGED"), afterError.isEmpty()
                    ? tr("The terminal native artifact changed while the result was being loaded.") : afterError);

            if (!session->trylock(0))
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The session became busy before the result document could be attached."));
            const bool attachBlocked = isMutationBlocked();
            session->unlock();
            if (attachBlocked)
                return invalid(QStringLiteral("MODEL_BUSY"), tr("A live model task started before the result document could be attached."));

            bool documentMayExist=false;
            QString publicationError;
            if (!session->appendVerifiedNativeResult(model,loadedBinding,&resultDocumentId,&documentMayExist,&publicationError))
            {
                if (!documentMayExist)
                    return invalid(QStringLiteral("RESULT_OPEN_FAILED"),publicationError);
                return unknownPublication(resultDocumentId,publicationError);
            }
        }

        const uint resultModelID = session->findModelByCommandIdentity(resultDocumentId);
        if (resultModelID == RConstants::eod)
            return invalid(QStringLiteral("RESULT_DOCUMENT_CLOSED"), tr("The result document closed before its open response was built."));
        const QPair<QString, quint64> resultStamp = session->modelCommandStamp(resultModelID);
        if (reused && resultStamp.second != openedAtRevision)
            return invalid(QStringLiteral("RESULT_DOCUMENT_CHANGED"), tr("The result document changed while its reuse response was being built."));
        if (!reused)
        {
            openedAtRevision = resultStamp.second;
            normalizedAtOpen = normalizedAtOpen || openedAtRevision != 0;
        }
        const QString publicationWorkspace=session->projectWorkspace();
        const QString publicationEpoch=state->sessionEpoch;
        const QString publicationFileName=session->getModel(resultModelID).getFileName();
        const auto publicationBinding=session->getModel(resultModelID).getLoadedSourceBinding();
        const QString publicationState=RModelCommandService::stateFingerprint(session->getModel(resultModelID));
        const QByteArray publicationValues=Session::nativeResultValuesDigest(session->getModel(resultModelID));
        const QJsonObject displayDescriptor{{QStringLiteral("kind"), QStringLiteral("native_solver_result")},
            {QStringLiteral("displayLabel"), tr("Solver result %1").arg(jobId.left(8))},
            {QStringLiteral("sourceJobId"), jobId},
            {QStringLiteral("artifactRef"), registeredPath},
            {QStringLiteral("artifactSHA256"), expectedSha},
            {QStringLiteral("artifactBytesVerifiedAtOpen"), true},
            {QStringLiteral("openedAtRuntimeStamp"), QJsonObject{
                {QStringLiteral("documentId"), resultDocumentId},
                {QStringLiteral("sessionEpoch"), state->sessionEpoch},
                {QStringLiteral("revision"), QString::number(openedAtRevision)}}},
            {QStringLiteral("normalizedAtOpen"), normalizedAtOpen},
            {QStringLiteral("documentStateAtOpen"), normalizedAtOpen
                ? QStringLiteral("normalized_during_open") : QStringLiteral("decoded_from_verified_artifact")},
            {QStringLiteral("artifactModelEquivalence"), normalizedAtOpen
                ? QStringLiteral("not_asserted_after_normalization") : QStringLiteral("verified_source_load")},
            {QStringLiteral("dirtyStatus"), normalizedAtOpen
                ? QStringLiteral("normalized_at_open") : QStringLiteral("unchanged_since_open")},
            {QStringLiteral("modifiedSinceOpen"), false}};
        state->resultDocuments[jobId] = State::ResultDocument{resultDocumentId,
            beforeBinding->logicalSourcePath, expectedSha, openedAtRevision, normalizedAtOpen};
        state->resultDescriptors[resultDocumentId] = displayDescriptor;
        const QJsonObject project = session->documentProjectContext(resultModelID);
        const QJsonObject source = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        QJsonObject inputProvenance;
        for (const QString &field : {QStringLiteral("inputDerivedFrom"),
                                     QStringLiteral("frozenSnapshotVerified"),
                                     QStringLiteral("frozenSnapshotPath"),
                                     QStringLiteral("frozenSnapshotSha256")})
            if (!job.value(field).isUndefined()) inputProvenance.insert(field, job.value(field));
        for (const QString &field : {QStringLiteral("inputSnapshotRef"),
                                     QStringLiteral("inputSnapshotSha256"),
                                     QStringLiteral("inputPackageRef"),
                                     QStringLiteral("inputPackageSha256")})
            if (!job.value(field).isUndefined()) inputProvenance.insert(QStringLiteral("solver_") + field, job.value(field));
        const QJsonObject result{{QStringLiteral("target"), QJsonObject{
                {QStringLiteral("documentId"), resultDocumentId},
                {QStringLiteral("sessionEpoch"), state->sessionEpoch}}},
            {QStringLiteral("documentId"), resultDocumentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("runtimeStamp"), sourceStampJson(resultStamp, state->sessionEpoch)},
            {QStringLiteral("persistentDocumentId"), project.value(QStringLiteral("persistentDocumentId"))},
            {QStringLiteral("projectId"), project.value(QStringLiteral("projectId"))},
            {QStringLiteral("persistentProject"), project},
            {QStringLiteral("displayDescriptor"), displayDescriptor},
            {QStringLiteral("sourceJob"), QJsonObject{{QStringLiteral("jobId"), jobId},
                {QStringLiteral("sourceRuntimeStamp"), source},
                {QStringLiteral("hostSource"), job.value(QStringLiteral("hostSource"))},
                {QStringLiteral("projectSnapshotRef"), job.value(QStringLiteral("projectSnapshotRef"))},
                {QStringLiteral("inputProvenance"), inputProvenance},
                {QStringLiteral("artifactRef"), registeredPath},
                {QStringLiteral("artifactSHA256"), expectedSha}}},
            {QStringLiteral("openResult"), reused ? QStringLiteral("reused") : QStringLiteral("created")},
            {QStringLiteral("resultViewCreated"), !reused},
            {QStringLiteral("message"), reused ? tr("Reused the open independent solver result view.")
                                                : tr("Created a new independent solver result view; the source model and solver state are unchanged.")},
            {QStringLiteral("sourceAndSolverUnchanged"), true}};
        if (!reused)
        {
            QJsonObject event = state->appendEvent(QStringLiteral("document.result_opened"), resultDocumentId,
                {{QStringLiteral("sourceJobId"), jobId},
                 {QStringLiteral("artifactSHA256"), expectedSha},
                 {QStringLiteral("sessionEpoch"), state->sessionEpoch}});
            emit eventPublished(event);
        }
        if (!session->trylock(0))
            return unknownPublication(resultDocumentId,tr("The result was published but final callback reconciliation is busy."));
        auto publicationUnlock=qScopeGuard([this] { session->unlock(); });
        const uint publishedId=session->findModelByCommandIdentity(resultDocumentId);
        if (publishedId==RConstants::eod || session->projectWorkspace()!=publicationWorkspace
            || state->sessionEpoch!=publicationEpoch || session->modelCommandStamp(publishedId)!=resultStamp
            || session->getModel(publishedId).getFileName()!=publicationFileName
            || session->getModel(publishedId).getLoadedSourceBinding()!=publicationBinding
            || RModelCommandService::stateFingerprint(session->getModel(publishedId))!=publicationState
            || Session::nativeResultValuesDigest(session->getModel(publishedId))!=publicationValues
            || !nativeSourceFilesMatch(*beforeBinding))
            return unknownPublication(resultDocumentId,tr("Final result publication callbacks changed the document or verified artifact bytes."));
        return ok(result, QStringLiteral("accepted"));
    }

    if (operation == QStringLiteral("project.query"))
    {
        if (!parameters.isEmpty()) return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("project.query takes no parameters."));
        return ok(session->projectContext());
    }
    if (operation == QStringLiteral("document.project.query"))
    {
        QString documentId, epoch;
        if (!exactKeys(parameters, {QStringLiteral("target")}) || !targetFieldsValid(&documentId, &epoch))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("A runtime document target is required."));
        if (epoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The document belongs to a different session."));
        const uint modelID = session->findModelByCommandIdentity(documentId);
        if (modelID == RConstants::eod)
            return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The document is no longer open."));
        return ok(session->documentProjectContext(modelID));
    }

    if (operation == "experiment.query" || operation == "experiment.cancel")
    {
        if (!exactKeys(parameters, {"experimentId"}) || !validToken(parameters.value("experimentId")))
            return invalid("INVALID_PARAMETERS", tr("An experiment operation requires only experimentId."));
        const QString experimentId = parameters.value("experimentId").toString();
        auto loaded = state->experiments->query(experimentId);
        if (!loaded.ok) return invalid(loaded.code, tr("The experiment record is unavailable."));
        auto record = loaded.data.value("experiment").toObject();
        if (!ownsExperiment(record, projectWorkspace(), session->getID(), state->sessionEpoch))
            return invalid("EXPERIMENT_SCOPE_DENIED", tr("The experiment belongs to another workspace or session."));
        bool cancelAccepted = false;
        bool childCancelFailed = false;
        if (operation == "experiment.cancel")
        {
            const auto cancelled = state->experiments->cancel(experimentId);
            if (!cancelled.ok) return invalid(cancelled.code, tr("The experiment cannot be cancelled in its current state."));
            cancelAccepted = true;
            // The transaction cancellation is durable before worker intent changes.
            for (const auto &value : experimentJobs(experimentId))
            {
                const auto job = value.toObject();
                const QString childState = job.value("state").toString();
                if (childState != "queued" && childState != "running" && childState != "cancelling") continue;
                QString error;
                if (!SolverManager::getInstance().cancelJob(job.value("jobId").toString(), &error))
                    childCancelFailed = true;
            }
            loaded = state->experiments->query(experimentId);
            if (!loaded.ok) return invalid(loaded.code, tr("Cancellation was persisted, but the record could not be reread."));
            record = loaded.data.value("experiment").toObject();
        }
        auto result = experimentSummary(record, experimentJobs(experimentId));
        if (result.isEmpty())
            return invalid("READBACK_LIMIT_EXCEEDED", tr("Trusted experiment evidence exceeds the host readback limit."));
        if (cancelAccepted)
        {
            result.insert("cancellationPersisted", true);
            result.insert("childCancellationRequestFailed", childCancelFailed);
        }
        const auto response = ok(result);
        if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() > trustedHostReadbackLimit)
            return invalid("READBACK_LIMIT_EXCEEDED", tr("Trusted experiment evidence exceeds the host readback limit."));
        return response;
    }

    if (operation == QStringLiteral("analysis.inspect") || operation == QStringLiteral("analysis.diagnose"))
    {
        const QSet<QString> requiredKeys{QStringLiteral("target"), QStringLiteral("expectedDocumentRevision"),
                                         QStringLiteral("basis")};
        QSet<QString> allowedKeys = requiredKeys;
        if (parameters.contains(QStringLiteral("problemSpec")))
            allowedKeys.insert(QStringLiteral("problemSpec"));
        if (operation == QStringLiteral("analysis.inspect") && parameters.contains(QStringLiteral("jobId")))
            allowedKeys.insert(QStringLiteral("jobId"));
        if (!exactKeys(parameters, allowedKeys)
            || !parameters.contains(QStringLiteral("target"))
            || !parameters.contains(QStringLiteral("expectedDocumentRevision"))
            || !parameters.value(QStringLiteral("basis")).isString()
            || (parameters.contains(QStringLiteral("problemSpec"))
                && !parameters.value(QStringLiteral("problemSpec")).isObject())
            || (parameters.contains(QStringLiteral("jobId"))
                && !validToken(parameters.value(QStringLiteral("jobId")))))
            return invalid("INVALID_PARAMETERS", tr("Analysis reads require an exact target, revision, basis and optional ProblemSpec object."));

        QString documentId, epoch;
        quint64 expectedRevision = 0;
        const QString basis = parameters.value(QStringLiteral("basis")).toString();
        if (!targetFieldsValid(&documentId, &epoch)
            || !unsignedInteger(parameters.value(QStringLiteral("expectedDocumentRevision")), &expectedRevision)
            || (basis != QLatin1String("document") && basis != QLatin1String("detached")))
            return invalid("INVALID_PARAMETERS", tr("Analysis source identity or basis is malformed."));
        if (epoch != state->sessionEpoch)
            return invalid("STALE_SESSION", tr("The analysis source belongs to another host session."));
        if (activeModalInteraction() || isMutationBlocked())
            return invalid("MODEL_BUSY", tr("Finish active document interaction or model tasks before trusted analysis readback."));
        TrySessionLock guard(session);
        if (!guard.ownsLock()) return invalid("MODEL_BUSY", tr("The source document is currently being changed."));
        const uint modelId = session->findModelByCommandIdentity(documentId);
        if (modelId == RConstants::eod)
            return invalid("DOCUMENT_CLOSED", tr("The exact analysis source document is no longer open."));
        const auto sourceStamp = session->modelCommandStamp(modelId);
        if (sourceStamp.first != documentId || sourceStamp.second != expectedRevision)
            return invalid("STALE_REVISION", tr("The analysis source revision changed; query the current document identity."));

        const QJsonObject spec = parameters.value(QStringLiteral("problemSpec")).toObject();
        const bool specProvided = parameters.contains(QStringLiteral("problemSpec"));
        const auto validation = RTrustedMechanicsService::validate(spec);
        const bool specValid = specProvided && validation.valid;
        RModel detachedCandidate;
        const RModel *observedModel = nullptr;
        if (basis == QLatin1String("document")) {
            observedModel = &session->getModel(modelId);
            if (!trustedInspectionWithinEntityLimit(*observedModel))
                return invalid("READBACK_LIMIT_EXCEEDED", tr("The source model exceeds the trusted inspection entity limit."));
        }
        else if (specValid) {
            // createCandidate owns ProblemSpec validation and normalizes its raw input. Passing the
            // already-normalized form here would strip the explicit SI unit objects and make the
            // same valid specification fail with SPEC_MISSING_QUANTITY.
            const auto candidate = RTrustedMechanicsService::createCandidate(spec,
                documentId, QString::number(expectedRevision));
            if (!candidate.valid)
                return invalid(candidate.code.isEmpty() ? QStringLiteral("CANDIDATE_CREATION_FAILED") : candidate.code,
                               tr("The detached candidate could not be built from the validated specification."));
            detachedCandidate = candidate.model;
            observedModel = &detachedCandidate;
            if (!trustedInspectionWithinEntityLimit(*observedModel))
                return invalid("READBACK_LIMIT_EXCEEDED", tr("The detached candidate exceeds the trusted inspection entity limit."));
        }

        if (observedModel && !trustedInspectionWithinNestedDataLimits(*observedModel))
            return invalid("READBACK_LIMIT_EXCEEDED",
                           tr("The model's nested physical histories or metadata exceed the trusted readback budget."));

        const QString revision = QString::number(expectedRevision);
        const QJsonObject runtimeStamp = sourceStampJson(sourceStamp, state->sessionEpoch);
        const QJsonObject diagnosis = RTrustedMechanicsService::diagnose(
            spec, observedModel, documentId, revision);
        QJsonObject result;
        if (operation == QStringLiteral("analysis.diagnose")) {
            result = QJsonObject{{QStringLiteral("operation"), QStringLiteral("analysis.diagnose")},
                {QStringLiteral("schemaVersion"), RTrustedMechanicsService::SchemaVersion},
                {QStringLiteral("basis"), basis}, {QStringLiteral("sourceRuntimeStamp"), runtimeStamp},
                {QStringLiteral("problemSpecStatus"), specValid ? QStringLiteral("valid") : QStringLiteral("invalid")},
                {QStringLiteral("diagnosis"), diagnosis}};
        }
        else if (!specValid && (basis == QLatin1String("detached") || specProvided)) {
            result = QJsonObject{{QStringLiteral("operation"), QStringLiteral("analysis.inspect")},
                {QStringLiteral("schemaVersion"), RTrustedMechanicsService::SchemaVersion},
                {QStringLiteral("basis"), basis}, {QStringLiteral("status"), QStringLiteral("invalid_spec")},
                {QStringLiteral("sourceRuntimeStamp"), runtimeStamp},
                {QStringLiteral("diagnostics"), diagnosis.value(QStringLiteral("issues"))}};
        }
        else {
            QJsonObject inspection = RTrustedMechanicsService::inspect(*observedModel, documentId, revision,
                specValid ? validation.normalizedSpec : QJsonObject{});
            const QString modelFingerprint = RModelCommandService::stateFingerprint(*observedModel);
            result = trustedInspectionProjection(inspection, basis, runtimeStamp, modelFingerprint);
            if (specValid)
                result.insert(QStringLiteral("diagnosis"), diagnosis);
        }

        if (operation == QStringLiteral("analysis.inspect") && observedModel)
        {
            const QJsonObject job = trustedInspectionJobIdentity(
                parameters.value(QStringLiteral("jobId")).toString(), runtimeStamp,
                RModelCommandService::stateFingerprint(*observedModel));
            result.insert(QStringLiteral("job"), job);
            QJsonObject resultSource{{QStringLiteral("status"), job.value(QStringLiteral("resultSourceStatus"))}};
            const QJsonValue resultStamp = job.value(QStringLiteral("resultSourceRuntimeStamp"));
            const QJsonValue jobStamp = job.value(QStringLiteral("sourceRuntimeStamp"));
            if (resultStamp.isObject()) resultSource.insert(QStringLiteral("sourceRuntimeStamp"), resultStamp);
            else if (jobStamp.isObject()) resultSource.insert(QStringLiteral("sourceRuntimeStamp"), jobStamp);
            const QJsonValue resultFingerprint = job.value(QStringLiteral("resultSourceFingerprint"));
            const QJsonValue jobFingerprint = job.value(QStringLiteral("sourceFingerprint"));
            if (resultFingerprint.isString()) resultSource.insert(QStringLiteral("sourceFingerprint"), resultFingerprint);
            else if (jobFingerprint.isString()) resultSource.insert(QStringLiteral("sourceFingerprint"), jobFingerprint);
            for (const auto &mapping : {
                     qMakePair(QStringLiteral("resultArtifactId"), QStringLiteral("artifactId")),
                     qMakePair(QStringLiteral("resultArtifactSha256"), QStringLiteral("artifactSha256")),
                     qMakePair(QStringLiteral("resultManifestSha256"), QStringLiteral("manifestSha256")),
                     qMakePair(QStringLiteral("resultArtifactVerified"), QStringLiteral("artifactVerified")),
                     qMakePair(QStringLiteral("resultSourceBindingVerified"), QStringLiteral("sourceBindingVerified")),
                     qMakePair(QStringLiteral("resultManifestVerified"), QStringLiteral("manifestVerified"))})
                if (job.contains(mapping.first)) resultSource.insert(mapping.second, job.value(mapping.first));
            result.insert(QStringLiteral("resultSource"), resultSource);
        }

        if (session->modelCommandStamp(modelId) != sourceStamp)
            return invalid("STALE_REVISION", tr("The source document changed during trusted analysis readback."));
        const auto response = ok(result);
        if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() > trustedHostReadbackLimit)
            return invalid("READBACK_LIMIT_EXCEEDED", tr("Trusted analysis evidence exceeds the host readback limit."));
        return response;
    }

    if (operation == QStringLiteral("capabilities.query"))
    {
        if (!parameters.isEmpty()) return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("capabilities.query takes an empty parameters object."));
        QJsonArray operations{
            QJsonObject{{"id", "experiment.run"}, {"version", 1}, {"requiresPreview", true}, {"requiresHostApproval", true}},
            QJsonObject{{"id", "experiment.query"}, {"version", 1}, {"readOnly", true}},
            QJsonObject{{"id", "analysis.inspect"}, {"version", 1}, {"readOnly", true},
                {"identity", "exact-source-revision-and-detached-candidate"},
                {"optionalEvidence", "exact-job-id-no-latest-lookup"}},
            QJsonObject{{"id", "analysis.diagnose"}, {"version", 1}, {"readOnly", true},
                {"identity", "exact-source-revision-and-detached-candidate"}},
            QJsonObject{{"id", "experiment.cancel"}, {"version", 1}, {"readOnly", false}, {"sideEffect", "cancel_owned_experiment"}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("project.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.project.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("context.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("capabilities.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("events.read")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("command.preview")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("command.execute")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("operation.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("operations.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.targets.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.inputs.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.jobs.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.job.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{"id", "mesh.job.query"}, {"version", 1}, {"readOnly", true},
                {"identity", "owned-attempt-and-exact-source-target-revision"}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.job.recovery.query")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.source.validate")}, {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.result.open")}, {QStringLiteral("version"), 1},
                {QStringLiteral("readOnly"), true}, {QStringLiteral("createsIndependentResultDocument"), true},
                {QStringLiteral("requiresRegisteredArtifactSHA"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("native.result.display")}, {QStringLiteral("version"), 1},
                {QStringLiteral("readOnly"), true}, {QStringLiteral("createsIndependentResultDocument"), true},
                {QStringLiteral("identity"), QStringLiteral("registered-job-artifact-and-channel")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.undo")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("document.redo")}, {QStringLiteral("version"), 1}, {QStringLiteral("requiresHostApproval"), true}}};
        for (const QString &quantity : {QStringLiteral("catalog"), QStringLiteral("channels"),
                                       QStringLiteral("series"), QStringLiteral("summary")})
            operations.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("native.result.") + quantity + QStringLiteral(".query")},
                {QStringLiteral("version"), 1}, {QStringLiteral("readOnly"), true},
                {QStringLiteral("identity"), QStringLiteral("registered-job-and-artifact")}});
        return ok({{QStringLiteral("sessionEpoch"), state->sessionEpoch},
                   {QStringLiteral("identityPersistence"), QStringLiteral("host-session")},
                   {QStringLiteral("capabilityCatalogVersion"), 1},
                   {QStringLiteral("supportedOperations"), operations},
                   {QStringLiteral("hostCommands"), hostCommands},
                   {QStringLiteral("editingScope"), QJsonObject{
                        {QStringLiteral("version"), 1},
                        {QStringLiteral("models"), QStringLiteral("open native documents with supported structural analysis tasks")},
                        {QStringLiteral("elements"), QStringLiteral("physical group edits preserve other model fields; labels are read-only target candidates")},
                        {QStringLiteral("analyses"), QJsonArray{QStringLiteral("structural.modal.configuration"),
                            QStringLiteral("structural.dynamics.configuration"), QStringLiteral("native.solver.run"),
                            QStringLiteral("native.solver.cancel")}},
                        {QStringLiteral("units"), QJsonObject{{QStringLiteral("modal.parameters"), QStringLiteral("1")},
                            {QStringLiteral("material.modulus"), QStringLiteral("Pa")},
                            {QStringLiteral("material.density"), QStringLiteral("kg/m^3")},
                            {QStringLiteral("boundary.displacement"), QStringLiteral("m")},
                            {QStringLiteral("boundary.force"), QStringLiteral("N")}}},
                        {QStringLiteral("refusals"), QJsonArray{QStringLiteral("unknown command version or fields"),
                            QStringLiteral("non-modal analysis for modal configuration"), QStringLiteral("invalid parameter ranges or units"),
                            QStringLiteral("stale or closed document instance"), QStringLiteral("pending known drafts or project conflicts")}}}},
                   {QStringLiteral("project"), session->projectContext()}});
    }

    if (operation == QStringLiteral("events.read"))
    {
        if (!exactKeys(parameters, {QStringLiteral("afterEventSeq"), QStringLiteral("limit")}))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("events.read requires afterEventSeq and limit."));
        quint64 cursor = 0;
        quint64 limit = 0;
        if (!unsignedInteger(parameters.value(QStringLiteral("afterEventSeq")), &cursor)
            || !unsignedInteger(parameters.value(QStringLiteral("limit")), &limit)
            || limit < 1 || limit > 100)
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The event cursor must be non-negative and limit must be 1 through 100."));
        const quint64 latest = state->nextEventSeq - 1;
        if (cursor > latest) return invalid(QStringLiteral("INVALID_CURSOR"), tr("The event cursor is newer than the host event stream."));
        const quint64 earliest = state->events.isEmpty()
            ? state->nextEventSeq : quint64(state->events.constFirst().value(QStringLiteral("eventSeq")).toDouble());
        const bool resync = !state->events.isEmpty() && cursor + 1 < earliest;
        QJsonArray events;
        quint64 returned = 0;
        if (!resync)
        {
            for (const auto &event : state->events)
            {
                const quint64 seq = quint64(event.value(QStringLiteral("eventSeq")).toDouble());
                if (seq > cursor && returned < limit) { events.append(event); ++returned; }
            }
        }
        const quint64 lastReturned = returned ? quint64(events.last().toObject().value(QStringLiteral("eventSeq")).toDouble()) : cursor;
        return ok({{QStringLiteral("events"), events},
                   {QStringLiteral("earliestEventSeq"), double(earliest)},
                   {QStringLiteral("latestEventSeq"), double(latest)},
                   {QStringLiteral("resyncRequired"), resync},
                   {QStringLiteral("hasMore"), !resync && lastReturned < latest}});
    }

    if (operation == QStringLiteral("context.query"))
    {
        QString requestedDocument;
        QString requestedEpoch;
        const bool hasTarget = parameters.contains(QStringLiteral("target"));
        if ((hasTarget && (!exactKeys(parameters, {QStringLiteral("target")})
                           || !targetFieldsValid(&requestedDocument, &requestedEpoch)))
            || (!hasTarget && !parameters.isEmpty()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("context.query accepts only an optional fixed target."));
        if (hasTarget && requestedEpoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The target belongs to a different host session."));

        const bool taskBusy = isBusy();
        bool editingBlocked = isMutationBlocked() || activeModalInteraction();
        QList<uint> selectedModels;
        QList<SessionEntityID> selectedEntities;
        QStringList documentIds;
        bool selectionLive = false;
        if (!editingBlocked && session->trylock(0))
        {
            if (!isMutationBlocked() && !activeModalInteraction())
            {
                selectionLive = true;
                if (hasTarget)
                {
                    const uint targetId = session->findModelByCommandIdentity(requestedDocument);
                    if (targetId == RConstants::eod) { session->unlock(); return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open.")); }
                    documentIds.append(requestedDocument);
                }
                else
                {
                    selectedModels = session->getSelectedModelIDs();
                    for (const uint id : selectedModels)
                    {
                        const auto stamp = session->modelCommandStamp(id);
                        if (stamp.first.isEmpty()) continue;
                        documentIds.append(stamp.first);
                        selectedEntities.append(session->getModel(id).getSelectedEntityIDs(id));
                    }
                }
                QJsonArray documentArray;
                for (const auto &id : documentIds) documentArray.append(id);
                QJsonArray entityArray;
                constexpr qsizetype maxSelectionEntities = 256;
                const qsizetype count = selectedEntities.size();
                for (qsizetype i = 0; i < qMin(count, maxSelectionEntities); ++i)
                {
                    const auto &entity = selectedEntities.at(i);
                    const auto stamp = session->modelCommandStamp(entity.getMid());
                    if (stamp.first.isEmpty()) continue;
                    entityArray.append(QJsonObject{{QStringLiteral("documentId"), stamp.first},
                                                   {QStringLiteral("groupType"), int(entity.getType())},
                                                   {QStringLiteral("entityId"), double(entity.getEid())}});
                }
                state->lastSelection = {{QStringLiteral("revision"), double(state->nextSelectionRevision)},
                                        {QStringLiteral("availability"), QStringLiteral("live")},
                                        {QStringLiteral("documents"), documentArray},
                                        {QStringLiteral("entities"), entityArray},
                                        {QStringLiteral("entityCount"), double(count)},
                                        {QStringLiteral("truncated"), count > maxSelectionEntities}};
            }
            else editingBlocked = true;
            if (!hasTarget && !editingBlocked)
            {
                // The selected document IDs were read while the Session lock was held.
            }
            session->unlock();
        }
        else if (!hasTarget)
        {
            editingBlocked = true;
        }
        if (!selectionLive)
        {
            if (!state->lastSelection.isEmpty())
            {
                state->lastSelection.insert(QStringLiteral("availability"), QStringLiteral("cached"));
            }
            else
            {
                state->lastSelection = {{QStringLiteral("revision"), double(state->nextSelectionRevision)},
                                        {QStringLiteral("availability"), QStringLiteral("unavailable")},
                                        {QStringLiteral("documents"), QJsonArray{}},
                                        {QStringLiteral("entities"), QJsonArray{}},
                                        {QStringLiteral("entityCount"), 0},
                                        {QStringLiteral("truncated"), false}};
            }
            if (!hasTarget)
            {
                for (const auto &value : state->lastSelection.value(QStringLiteral("documents")).toArray())
                    documentIds.append(value.toString());
            }
        }
        if (hasTarget && !documentIds.contains(requestedDocument)) documentIds.append(requestedDocument);

        QJsonArray documents;
        QSet<QString> emitted;
        for (const auto &documentId : documentIds)
        {
            if (documentId.isEmpty() || emitted.contains(documentId)) continue;
            emitted.insert(documentId);
            const uint modelID = session->findModelByCommandIdentity(documentId);
            if (modelID == RConstants::eod) continue;
            const auto stamp = session->modelCommandStamp(modelID);
            if (stamp.first != documentId) continue;
            if (selectionLive && !editingBlocked)
            {
                if (!session->trylock(0))
                {
                    documents.append(makeDocumentSnapshot(documentId, modelID, false));
                    continue;
                }
                if (isMutationBlocked() || activeModalInteraction())
                {
                    documents.append(makeDocumentSnapshot(documentId, modelID, false));
                    session->unlock();
                    continue;
                }
                const auto current = session->findModelByCommandIdentity(documentId);
                if (current == RConstants::eod) { session->unlock(); continue; }
                documents.append(makeDocumentSnapshot(documentId, current, true));
                session->unlock();
            }
            else
            {
                QJsonObject cached;
                const auto entry = state->entries.find(documentId);
                if (entry != state->entries.end() && entry->second.editing && entry->second.generation == stamp.second)
                    cached = entry->second.editing->query();
                documents.append(makeDocumentSnapshot(documentId, modelID, false, cached));
            }
        }
        return ok({{QStringLiteral("sessionEpoch"), state->sessionEpoch},
                   {QStringLiteral("identityPersistence"), QStringLiteral("host-session")},
                   {QStringLiteral("taskBusy"), taskBusy},
                   {QStringLiteral("editingBlocked"), editingBlocked},
                   {QStringLiteral("selection"), state->lastSelection},
                   {QStringLiteral("documents"), documents}});
    }

    if (operation == QStringLiteral("command.preview"))
    {
        if (!parameters.value(QStringLiteral("command")).isString()
            || parameters.value(QStringLiteral("commandVersion")).toInt(-1) != 1
            || !parameters.value(QStringLiteral("arguments")).isObject())
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The command and versioned arguments are required."));
        const QString command = parameters.value(QStringLiteral("command")).toString();
        const QJsonObject arguments = parameters.value(QStringLiteral("arguments")).toObject();
        const bool cancelCommand = command == QStringLiteral("analysis.cancel");
        const bool hasTarget = parameters.contains(QStringLiteral("target"));
        const QSet<QString> standardKeys{QStringLiteral("target"), QStringLiteral("expectedDocumentRevision"),
            QStringLiteral("command"), QStringLiteral("commandVersion"), QStringLiteral("arguments")};
        const QSet<QString> detachedCancelKeys{QStringLiteral("command"), QStringLiteral("commandVersion"),
            QStringLiteral("arguments")};
        if (command == "experiment.run")
        {
            QString documentId, epoch;
            quint64 revision = 0;
            if (!exactKeys(parameters, standardKeys) || !targetFieldsValid(&documentId, &epoch)
                || epoch != state->sessionEpoch
                || !unsignedInteger(parameters.value("expectedDocumentRevision"), &revision)
                || !arguments.value("problemSpec").isObject())
                return invalid("INVALID_PARAMETERS", tr("Experiment preview requires an exact source target, revision and ProblemSpec."));
            const QSet<QString> allowed{"problemSpec", "allowedNumericalRanges", "budgets", "stopConditions",
                                        "convergenceStudy"};
            for (auto it = arguments.begin(); it != arguments.end(); ++it)
                if (!allowed.contains(it.key()))
                    return invalid("INVALID_PARAMETERS", tr("Unknown experiment argument."));
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before experiment preview."));
            const auto source = experimentSource(session, epoch, documentId);
            if (!source.isValid() || source.documentRevision != QString::number(revision))
                return invalid("SOURCE_STALE", tr("The experiment source changed or cannot be read safely."));
            QJsonObject spec = arguments;
            spec.insert("schemaVersion", RTrustedExperimentService::SchemaVersion);
            spec.insert("sourceBinding", source.toJson());
            const auto authorizationPreview = state->experiments->previewAuthorization(spec, source);
            if (!authorizationPreview.ok)
                return invalid(authorizationPreview.code, tr("The experiment specification could not be previewed."));
            const auto authorization = authorizationPreview.data.value("authorization").toObject();
            State::Preview preview;
            preview.id = uuid(); preview.documentId = documentId; preview.sessionEpoch = epoch;
            preview.command = command; preview.stampGeneration = revision;
            preview.experimentId = authorizationPreview.data.value("experimentId").toString();
            preview.experimentAuthorizationRequest = spec;
            preview.experimentAuthorization = authorization;
            preview.experimentSourceBinding = source.toJson();
            // Mechanics APIs accept quantities with units; their normalized SI
            // output is a receipt, not another valid ProblemSpec input.
            preview.experimentProblemSpecInput = authorizationPreview.data.value("problemSpecInput").toObject();
            preview.arguments.insert("problemSpec", authorizationPreview.data.value("problemSpec"));
            const auto policy = preview.experimentAuthorization.value("experimentPolicy").toObject();
            for (auto it = policy.begin(); it != policy.end(); ++it) preview.arguments.insert(it.key(), it.value());
            preview.experimentDiagnostics = RTrustedMechanicsService::diagnose(
                preview.experimentProblemSpecInput, nullptr, documentId, source.documentRevision);
            preview.digest = RTrustedMechanicsService::summaryDigest(QJsonObject{
                {"authorization", preview.experimentAuthorization}, {"arguments", preview.arguments}});
            QJsonObject result{{"command", command}, {"commandVersion", 1}, {"previewId", preview.id},
                {"experimentId", preview.experimentId}, {"digest", preview.digest},
                {"target", parameters.value("target")}, {"expectedDocumentRevision", double(revision)},
                {"sourceBinding", preview.experimentSourceBinding},
                {"arguments", preview.arguments}, {"diagnostics", preview.experimentDiagnostics},
                {"authorizationDigest", preview.experimentAuthorization.value("authorizationDigest")},
                {"requiresHostApproval", true}, {"history", "none"},
                {"after", QJsonObject{{"sourceDocumentModified", false}, {"maximumAttemptsThisRun", 1}}}};
            if (state->previewOrder.size() >= 128) state->previews.erase(state->previewOrder.takeFirst());
            state->previewOrder.append(preview.id);
            state->previews.emplace(preview.id, preview);
            return ok(result, "preview");
        }
        if (command == "mesh.generate" || command == "mesh.accept" || command == "mesh.cancel")
        {
            QString documentId, epoch; quint64 revision = 0;
            if (!exactKeys(parameters, standardKeys) || !targetFieldsValid(&documentId, &epoch)
                || !unsignedInteger(parameters.value("expectedDocumentRevision"), &revision))
                return invalid("INVALID_PARAMETERS", tr("Mesh commands require an exact source target and revision."));
            if (epoch != state->sessionEpoch) return invalid("STALE_SESSION", tr("The mesh source belongs to another session."));
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before mesh actions."));
            QJsonObject native;
            if (command == "mesh.generate")
            {
                const double volume = arguments.value("maxVolume").toDouble(-1);
                const double ratio = arguments.value("radiusEdgeRatio").toDouble(-1);
                if (!exactKeys(arguments, {"unit", "volumeUnit", "maxVolume", "radiusEdgeRatio"})
                    || arguments.value("unit") != QJsonValue("m") || arguments.value("volumeUnit") != QJsonValue("m^3")
                    || !arguments.value("maxVolume").isDouble() || !std::isfinite(volume) || volume < 1e-15 || volume > 1e18
                    || !arguments.value("radiusEdgeRatio").isDouble() || !std::isfinite(ratio) || ratio < 1.2 || ratio > 2.0)
                    return invalid("INVALID_PARAMETERS", tr("Require exact mesh SI units and bounded volume/quality options."));
                const uint id = session->findModelByCommandIdentity(documentId);
                if (id >= session->getNModels() || session->modelCommandStamp(id).second != revision)
                    return invalid("STALE_REVISION", tr("The mesh source document changed; preview its current revision."));
                native = state->meshController->previewGenerate(id, arguments);
            }
            else
            {
                if (!exactKeys(arguments, {"attemptId"}) || !validToken(arguments.value("attemptId")))
                    return invalid("INVALID_PARAMETERS", tr("Mesh acceptance and cancellation require only an owned attemptId."));
                const auto job = ownedMeshAttempt(arguments.value("attemptId").toString(), documentId, epoch, revision);
                if (job.isEmpty()) return invalid("MESH_SOURCE_MISMATCH", tr("No owned mesh attempt matches this immutable source identity."));
                if (command == "mesh.accept") native = state->meshController->previewAccept(arguments.value("attemptId").toString());
                else
                {
                    if (job.value("status") != QJsonValue("queued") && job.value("status") != QJsonValue("running"))
                        return invalid("MESH_ATTEMPT_TERMINAL", tr("Only an owned queued or running mesh attempt can be cancelled."));
                    native = QJsonObject{{"valid", true}, {"result", job}};
                }
            }
            if (!native.value("valid").toBool())
                return invalid(native.value("code").toString(), native.value("message").toString());
            State::Preview preview;
            preview.id = uuid(); preview.documentId = documentId; preview.sessionEpoch = epoch;
            preview.stampGeneration = revision; preview.command = command; preview.arguments = arguments;
            preview.nativePreview = native.value("result").toObject();
            preview.meshPreviewId = preview.nativePreview.value("previewId").toString();
            preview.jobId = preview.nativePreview.value("attemptId").toString();
            preview.digest = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonObject{
                {"command", command}, {"arguments", arguments}, {"target", parameters.value("target")},
                {"revision", double(revision)}, {"meshPreview", preview.nativePreview}})
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject result{{"command", command}, {"commandVersion", 1}, {"arguments", arguments},
                {"target", parameters.value("target")}, {"expectedDocumentRevision", double(revision)},
                {"previewId", preview.id}, {"digest", preview.digest}, {"requiresHostApproval", true},
                {"requiresManualApproval", true}, {"history", "none"}, {"meshPreview", meshReceipt(preview.nativePreview)},
                {"after", QJsonObject{{"sourceDocumentModified", false}, {"requiresRebinding", true},
                    {"createsNewDocument", command == "mesh.accept"}, {"remapsOldPhysicalIds", false}}}};
            if (state->previewOrder.size() >= 128) state->previews.erase(state->previewOrder.takeFirst());
            state->previewOrder.append(preview.id); state->previews.emplace(preview.id, preview);
            return ok(result, "preview");
        }
        if (command == "analysis.recover")
        {
            const int remaining = arguments.value("remainingSteps").toInt(-1);
            static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
            if (!exactKeys(parameters, detachedCancelKeys) || !exactKeys(arguments, {"jobId", "checkpointSha256", "remainingSteps"})
                || !validToken(arguments.value("jobId")) || !arguments.value("checkpointSha256").isString()
                || !shaPattern.match(arguments.value("checkpointSha256").toString()).hasMatch()
                || !arguments.value("remainingSteps").isDouble() || arguments.value("remainingSteps").toDouble() != remaining
                || remaining < 1 || remaining > 5000)
                return invalid("INVALID_PARAMETERS", tr("Recovery requires an opaque job ID, selected checkpoint SHA and exact positive remaining range."));
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before previewing native recovery."));
            const auto qualification = SolverManager::getInstance().nativeJobRecoveryQuery(arguments.value("jobId").toString());
            const auto checkpoint = qualification.value("checkpoint").toObject();
            if (qualification.value("eligibility") != QJsonValue("recoverable")
                || checkpoint.value("checkpointSha256") != arguments.value("checkpointSha256")
                || checkpoint.value("remainingSteps").toInt(-1) != remaining)
                return invalid("RECOVERY_CHANGED", tr("The selected checkpoint or remaining range is unavailable. Query native recovery again."));
            State::Preview preview;
            preview.id = uuid();
            preview.sessionEpoch = state->sessionEpoch;
            preview.command = command;
            preview.jobId = arguments.value("jobId").toString();
            preview.arguments = arguments;
            preview.recoveryQualification = qualification;
            preview.digest = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(qualification)
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject result{{"command", command}, {"commandVersion", 1}, {"previewId", preview.id},
                {"digest", preview.digest}, {"arguments", arguments}, {"requiresHostApproval", true},
                {"before", QJsonObject{{"jobId", preview.jobId}, {"acceptedSeq", checkpoint.value("acceptedSeq")}}},
                {"after", QJsonObject{{"remainingSteps", remaining}, {"targetSteps", checkpoint.value("targetSteps")},
                    {"sourceDocumentModified", false}, {"newAttempt", true}}},
                {"history", "none"}, {"changedPaths", QJsonArray{}}};
            if (state->previewOrder.size() >= 128)
            { state->previews.erase(state->previewOrder.takeFirst()); }
            state->previewOrder.append(preview.id);
            state->previews.emplace(preview.id, preview);
            return ok(result, "preview");
        }
        if (cancelCommand)
        {
            if ((!hasTarget && !exactKeys(parameters, detachedCancelKeys))
                || (hasTarget && !exactKeys(parameters, standardKeys))
                || !exactKeys(arguments, {QStringLiteral("jobId")})
                || !validToken(arguments.value(QStringLiteral("jobId"))))
                return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("analysis.cancel requires only a registered jobId and may include its original runtime target."));
            if (activeModalInteraction())
                return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before cancelling a solver job."));
            const QString jobId = arguments.value(QStringLiteral("jobId")).toString();
            const QJsonObject job = nativeJob(jobId);
            if (!job.value(QStringLiteral("found")).toBool())
                return invalid(QStringLiteral("JOB_NOT_FOUND"), tr("No native job with this ID is registered."));
            const QString jobState = job.value(QStringLiteral("state")).toString();
            if (jobState != QStringLiteral("queued") && jobState != QStringLiteral("running")
                && jobState != QStringLiteral("cancelling"))
                return invalid(QStringLiteral("JOB_TERMINAL"), tr("The selected native job is already terminal."));
            const QJsonObject jobStamp = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
            const QString sourceDocumentId = jobStamp.value(QStringLiteral("documentId")).toString();
            const QString sourceRevision = jobStamp.value(QStringLiteral("revision")).toString();
            if (sourceDocumentId.isEmpty() || sourceRevision.isEmpty())
                return invalid(QStringLiteral("JOB_SOURCE_UNAVAILABLE"), tr("The job has no immutable source document identity."));
            QString sourceEpoch;
            const auto ownedSource = state->jobSources.find(jobId);
            if (ownedSource != state->jobSources.end())
            {
                if (ownedSource->second.documentId != sourceDocumentId || ownedSource->second.revision != sourceRevision)
                    return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The registered job source differs from its host-owned immutable source."));
                const QString managerEpoch = jobStamp.value(QStringLiteral("sessionEpoch")).toString();
                if (!managerEpoch.isEmpty() && managerEpoch != ownedSource->second.sessionEpoch)
                    return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The registered job session epoch differs from its host-owned immutable source."));
                sourceEpoch = ownedSource->second.sessionEpoch;
            }
            else
            {
                sourceEpoch = jobStamp.value(QStringLiteral("sessionEpoch")).toString();
                if (sourceEpoch.isEmpty()) sourceEpoch = job.value(QStringLiteral("projectSnapshotRef")).toObject()
                                  .value(QStringLiteral("instanceRef")).toObject()
                                  .value(QStringLiteral("sessionEpoch")).toString();
                if (sourceEpoch.isEmpty())
                {
                    const uint currentId = session->findModelByCommandIdentity(sourceDocumentId);
                    if (currentId != RConstants::eod
                        && QString::number(session->modelCommandStamp(currentId).second) == sourceRevision)
                        sourceEpoch = state->sessionEpoch;
                }
            }
            if (sourceEpoch != state->sessionEpoch)
                return invalid(QStringLiteral("STALE_SESSION"), tr("The job belongs to a different host session."));
            if (ownedSource == state->jobSources.end())
                state->jobSources[jobId] = State::JobSource{sourceDocumentId, sourceEpoch, sourceRevision,
                    job.value(QStringLiteral("projectSnapshotRef")).toObject()};
            if (hasTarget)
            {
                QString targetDocument, targetEpoch;
                quint64 expectedRevision = 0;
                if (!targetFieldsValid(&targetDocument, &targetEpoch)
                    || !unsignedInteger(parameters.value(QStringLiteral("expectedDocumentRevision")), &expectedRevision)
                    || targetDocument != sourceDocumentId || targetEpoch != sourceEpoch
                    || QString::number(expectedRevision) != sourceRevision)
                    return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The supplied target does not match the job's immutable source."));
            }
            State::Preview preview;
            preview.id = uuid();
            preview.documentId = sourceDocumentId;
            preview.sessionEpoch = sourceEpoch;
            preview.command = command;
            preview.jobId = jobId;
            preview.jobSourceDocumentId = sourceDocumentId;
            preview.jobSourceRevision = sourceRevision;
            preview.arguments = arguments;
            preview.nativePreview = job;
            const QJsonObject readSetObject{{QStringLiteral("jobId"), jobId},
                {QStringLiteral("sourceRuntimeStamp"), jobStamp},
                {QStringLiteral("sessionEpoch"), sourceEpoch}};
            preview.readSetHash = QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(readSetObject).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject digestInput{{QStringLiteral("command"), command},
                {QStringLiteral("commandVersion"), 1}, {QStringLiteral("arguments"), arguments},
                {QStringLiteral("job"), jobStamp}, {QStringLiteral("sessionEpoch"), sourceEpoch},
                {QStringLiteral("readSetHash"), preview.readSetHash}};
            preview.digest = QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(digestInput).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            QJsonObject result{{QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
                {QStringLiteral("arguments"), arguments},
                {QStringLiteral("before"), QJsonObject{{QStringLiteral("jobState"), jobState},
                    {QStringLiteral("sourceRuntimeStamp"), jobStamp}}},
                {QStringLiteral("after"), QJsonObject{{QStringLiteral("cancellationRequested"), true},
                    {QStringLiteral("terminalStateDeterminedBySolver"), true}}},
                {QStringLiteral("changedPaths"), QJsonArray{}},
                {QStringLiteral("readSet"), QJsonArray{QStringLiteral("native.job:%1").arg(jobId),
                    QStringLiteral("native.job.sourceRuntimeStamp"), QStringLiteral("host.sessionEpoch")}},
                {QStringLiteral("readSetHash"), preview.readSetHash},
                {QStringLiteral("previewId"), preview.id}, {QStringLiteral("digest"), preview.digest},
                {QStringLiteral("requiresHostApproval"), true},
                {QStringLiteral("history"), QStringLiteral("none")},
                {QStringLiteral("irreversible"), true}};
            if (state->previewOrder.size() >= 128)
            {
                const QString expired = state->previewOrder.takeFirst();
                state->previews.erase(expired);
            }
            state->previewOrder.append(preview.id);
            state->previews.emplace(preview.id, preview);
            return ok(result, QStringLiteral("preview"));
        }

        if (!exactKeys(parameters, standardKeys))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("command.preview requires target, expectedDocumentRevision, command, commandVersion and arguments."));
        QString documentId;
        QString targetEpoch;
        quint64 expectedRevision = 0;
        if (!targetFieldsValid(&documentId, &targetEpoch)
            || targetEpoch != state->sessionEpoch
            || !unsignedInteger(parameters.value(QStringLiteral("expectedDocumentRevision")), &expectedRevision))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The target revision is malformed or belongs to another host session."));
        const bool runCommand = command == QStringLiteral("analysis.run");
        const bool historyCommand = command == QStringLiteral("document.undo") || command == QStringLiteral("document.redo");
        if (!isCoreEditCommand(command) && !runCommand && !historyCommand)
            return invalid(QStringLiteral("UNSUPPORTED_CAPABILITY"), tr("This host does not support the requested versioned command."));
        if ((historyCommand && !arguments.isEmpty())
            || (runCommand && (!exactKeys(arguments, {QStringLiteral("warningsAccepted")})
                               || !arguments.value(QStringLiteral("warningsAccepted")).isBool()))
            || (isCoreEditCommand(command) && arguments.isEmpty()))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("The command arguments do not match the selected operation."));
        if (activeModalInteraction())
            return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before previewing a write or solver command."));
        if (isMutationBlocked())
            return invalid(QStringLiteral("MODEL_BUSY"), tr("The target model is held by an active legacy task."));
        uint modelID = session->findModelByCommandIdentity(documentId);
        if (modelID == RConstants::eod)
            return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
        QString projectFailure;
        if (!session->canMutateDocument(modelID, &projectFailure))
            return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
        auto stamp = session->modelCommandStamp(modelID);
        if (stamp.first != documentId || stamp.second != expectedRevision)
            return invalid(QStringLiteral("STALE_REVISION"), tr("Query the current document revision and create a new preview."));

        // SolverManager performs its own non-blocking Session lock while it
        // validates a live model. Do not call it from the host's locked model
        // read below: QMutex is not recursive, so that would make a valid
        // source look stale simply because the host already owns the mutex.
        QJsonObject runValidation;
        if (runCommand)
        {
            runValidation = SolverManager::getInstance().validateRun(modelID);
            if (runValidation.value(QStringLiteral("sourceStamp")).toObject()
                != sourceStampJson(stamp, state->sessionEpoch))
                return invalid(QStringLiteral("STALE_REVISION"), tr("The solver validation source changed during preview."));
            if (!runValidation.value(QStringLiteral("valid")).toBool()
                || !runValidation.value(QStringLiteral("errors")).toArray().isEmpty())
            {
                QJsonObject response = invalid(QStringLiteral("SOLVER_VALIDATION_FAILED"),
                    tr("The native solver rejected this model during preview."));
                response.insert(QStringLiteral("validation"), runValidation);
                return response;
            }
            if (!runValidation.value(QStringLiteral("warnings")).toArray().isEmpty()
                && !arguments.value(QStringLiteral("warningsAccepted")).toBool())
            {
                QJsonObject response = invalid(QStringLiteral("WARNINGS_NOT_ACCEPTED"),
                    tr("Explicit warningsAccepted=true is required before this solver run can be approved."));
                response.insert(QStringLiteral("warnings"), runValidation.value(QStringLiteral("warnings")));
                return response;
            }
        }

        State::Preview preview;
        QJsonObject nativePreview;
        QJsonObject before;
        QJsonObject after;
        QJsonArray changedPaths;
        QJsonArray readSet{QStringLiteral("/document/revision")};
        QString candidateFingerprint;
        QString inputFingerprint;
        QString nativeRevision;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The model could not be read safely for preview."));
            modelID = session->findModelByCommandIdentity(documentId);
            if (modelID == RConstants::eod)
                return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            stamp = session->modelCommandStamp(modelID);
            if (stamp.first != documentId || stamp.second != expectedRevision)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed while the preview was being prepared."));
            QString currentProjectFailure;
            if (!session->canMutateDocument(modelID, &currentProjectFailure))
                return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"),
                    currentProjectFailure);
            if (runCommand
                && runValidation.value(QStringLiteral("sourceStamp")).toObject()
                    != sourceStampJson(stamp, state->sessionEpoch))
                return invalid(QStringLiteral("STALE_REVISION"), tr("The solver validation source changed while the preview was being prepared."));
            const Model &model = session->getModel(modelID);
            auto *entry = state->ensure(session, modelID);
            if (!entry) return invalid(QStringLiteral("SESSION_UNAVAILABLE"), state->failure);
            if (!entry->editing->setRunning(false))
                return invalid(QStringLiteral("JOURNAL_UNAVAILABLE"), tr("Cannot record model availability."));
            const QJsonObject nativeQuery = entry->editing->query();
            nativeRevision = nativeQuery.value(QStringLiteral("revision")).toString();
            inputFingerprint = RModelCommandService::stateFingerprint(model);

            if (isCoreEditCommand(command))
            {
                const auto core = RModelCommandService::preview(model, command, arguments,
                    documentId, QString::number(stamp.second));
                if (!core.valid)
                    return invalid(core.code.toUpper(), core.message);
                preview.delta = core.delta;
                preview.hasSupportedDelta = true;
                preview.arguments = core.normalizedArguments;
                before = core.before;
                after = core.after;
                changedPaths = command == QStringLiteral("analysis.modal.configure")
                    ? QJsonArray{QStringLiteral("analysis.modal")}
                    : (core.delta.geometry.noOp ? QJsonArray{}
                        : (core.changedPaths.isEmpty() ? QJsonArray{core.delta.changedPath} : core.changedPaths));
                candidateFingerprint = core.fingerprint;
                readSet.append(QStringLiteral("/analysis/taskTree"));
                readSet.append(QStringLiteral("/problemSetup"));
                if (core.delta.physicalId)
                    readSet.append(QStringLiteral("/entityGroups/g:%1").arg(core.delta.physicalId));
                for (const QJsonValue &path : changedPaths) readSet.append(path);
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("normalizedArguments"), core.normalizedArguments},
                    {QStringLiteral("before"), core.before}, {QStringLiteral("after"), core.after},
                    {QStringLiteral("changedPaths"), changedPaths},
                    {QStringLiteral("candidateFingerprint"), core.fingerprint},
                    {QStringLiteral("diagnostics"), core.diagnostics}};
                if (core.delta.kind == RModelCommandService::Delta::Kind::MeshRebind
                    || core.delta.kind == RModelCommandService::Delta::Kind::PiezoExperiment)
                {
                    nativePreview.insert("report", core.report);
                    nativePreview.insert("bindingReadSet", core.readSet);
                    nativePreview.insert("units", core.units);
                    nativePreview.insert("invalidation", core.invalidation);
                    readSet.append(QStringLiteral("/geometry"));
                    readSet.append(QStringLiteral("/entityGroups"));
                    readSet.append(QStringLiteral("/timeSolver"));
                    readSet.append(QStringLiteral("/results"));
                    readSet.append(QStringLiteral("/derivedMappings"));
                }
            }
            else if (runCommand)
            {
                const bool warningsAccepted = arguments.value(QStringLiteral("warningsAccepted")).toBool();
                preview.arguments = arguments;
                preview.validation = runValidation;
                preview.projectSnapshotRef = session->documentProjectContext(modelID);
                before = {{QStringLiteral("sourceRuntimeStamp"), sourceStampJson(stamp, state->sessionEpoch)},
                          {QStringLiteral("validation"), runValidation}};
                after = {{QStringLiteral("jobState"), QStringLiteral("queued")},
                         {QStringLiteral("warningsAccepted"), warningsAccepted},
                         {QStringLiteral("modelRevisionChanged"), false}};
                readSet.append(QStringLiteral("/solver.validation"));
                readSet.append(QStringLiteral("/project.persistentDocument"));
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("validation"), runValidation},
                    {QStringLiteral("warningsAccepted"), warningsAccepted}};
            }
            else
            {
                const bool undo = command == QStringLiteral("document.undo");
                const bool available = undo ? model.getUndoStackSize() > 0 : model.getRedoStackSize() > 0;
                if (!available)
                    return invalid(QStringLiteral("EMPTY_HISTORY"), tr("No matching operation is available in the document history."));
                nativePreview = {{QStringLiteral("status"), QStringLiteral("preview")},
                    {QStringLiteral("historyAction"), undo ? model.getUndoActionMessage() : model.getRedoActionMessage()},
                    {QStringLiteral("historyDepth"), double(undo ? model.getUndoStackSize() : model.getRedoStackSize())}};
                before = {{QStringLiteral("historyAction"), nativePreview.value(QStringLiteral("historyAction"))},
                          {QStringLiteral("historyDepth"), nativePreview.value(QStringLiteral("historyDepth"))}};
                after = {{QStringLiteral("historyState"), QStringLiteral("computed_on_commit")}};
                changedPaths = QJsonArray{QStringLiteral("document.history")};
                readSet.append(QStringLiteral("/document/history"));
                if (model.hasStaticMeshBindingHistory(undo))
                {
                    QString historyError;
                    if (!model.previewStaticMeshBindingHistory(undo, &preview.staticBindingHistoryHash, &historyError))
                        return invalid(QStringLiteral("UNSUPPORTED_HISTORY"), historyError);
                    preview.staticBindingHistoryId = model.staticMeshBindingHistoryIdentity(undo);
                    candidateFingerprint = preview.staticBindingHistoryHash;
                    nativePreview.insert(QStringLiteral("historyItemId"), preview.staticBindingHistoryId);
                    before.insert(QStringLiteral("historyItemId"), preview.staticBindingHistoryId);
                    after = {{QStringLiteral("supportedStateFingerprint"), candidateFingerprint}};
                    readSet.append(QStringLiteral("/geometry"));
                    readSet.append(QStringLiteral("/entityGroups"));
                    readSet.append(QStringLiteral("/results"));
                }
                else if (model.hasPiezoExperimentHistory(undo))
                {
                    QString historyError;
                    if (!model.previewPiezoExperimentHistory(undo,&preview.piezoHistoryHash,&historyError))
                        return invalid(QStringLiteral("UNSUPPORTED_HISTORY"),historyError);
                    preview.piezoHistoryId=model.piezoExperimentHistoryIdentity(undo);
                    candidateFingerprint=preview.piezoHistoryHash;
                    nativePreview.insert(QStringLiteral("historyItemId"),preview.piezoHistoryId);
                    before.insert(QStringLiteral("historyItemId"),preview.piezoHistoryId);
                    after={{QStringLiteral("supportedStateFingerprint"),candidateFingerprint}};
                    readSet.append(QStringLiteral("/geometry"));readSet.append(QStringLiteral("/entityGroups"));
                    readSet.append(QStringLiteral("/problemSetup"));readSet.append(QStringLiteral("/results"));
                }
            }
            preview.id = uuid();
            preview.documentId = documentId;
            preview.sessionEpoch = state->sessionEpoch;
            preview.command = command;
            preview.stampGeneration = stamp.second;
            preview.nativeRevision = nativeRevision;
            if (!isCoreEditCommand(command) && !runCommand) preview.arguments = arguments;
        }
        preview.nativePreview = nativePreview;
        preview.inputFingerprint = inputFingerprint;
        preview.readSet = readSet;
        QJsonObject readSetSource{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("revision"), QString::number(expectedRevision)},
            {QStringLiteral("inputStateFingerprint"), inputFingerprint},
            {QStringLiteral("readSet"), readSet}};
        preview.readSetHash = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(readSetSource).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QJsonObject digestInput{{QStringLiteral("documentId"), documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("expectedDocumentRevision"), double(expectedRevision)},
            {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("submittedArguments"), arguments},
            {QStringLiteral("normalizedArguments"), preview.arguments},
            {QStringLiteral("before"), before}, {QStringLiteral("after"), after},
            {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("candidateFingerprint"), candidateFingerprint},
            {QStringLiteral("inputStateFingerprint"), inputFingerprint},
            {QStringLiteral("readSetHash"), preview.readSetHash},
            {QStringLiteral("nativeRevision"), nativeRevision}};
        for (const QString &field : {QStringLiteral("report"), QStringLiteral("bindingReadSet"),
                                    QStringLiteral("units"), QStringLiteral("invalidation")})
            if (nativePreview.contains(field)) digestInput.insert(field, nativePreview.value(field));
        preview.digest = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(digestInput).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QJsonObject result{{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                                                                   {QStringLiteral("sessionEpoch"), state->sessionEpoch}}},
            {QStringLiteral("runtimeStamp"), sourceStampJson(stamp, state->sessionEpoch)},
            {QStringLiteral("expectedDocumentRevision"), double(expectedRevision)},
            {QStringLiteral("command"), command}, {QStringLiteral("commandVersion"), 1},
            {QStringLiteral("arguments"), preview.arguments},
            {QStringLiteral("normalizedArguments"), preview.arguments},
            {QStringLiteral("submittedArguments"), arguments}, {QStringLiteral("before"), before},
            {QStringLiteral("after"), after}, {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("readSet"), readSet}, {QStringLiteral("readSetHash"), preview.readSetHash},
            {QStringLiteral("inputStateFingerprint"), inputFingerprint},
            {QStringLiteral("candidateFingerprint"), candidateFingerprint},
            {QStringLiteral("invalidatedResultsAvailability"), QStringLiteral("unknown_not_exposed")},
            {QStringLiteral("previewId"), preview.id}, {QStringLiteral("digest"), preview.digest},
            {QStringLiteral("nativeModalSessionRevision"), preview.nativeRevision}};
        for (const QString &field : {QStringLiteral("report"), QStringLiteral("bindingReadSet"),
                                    QStringLiteral("units"), QStringLiteral("invalidation")})
            if (nativePreview.contains(field)) result.insert(field, nativePreview.value(field));
        if (runCommand)
        {
            result.insert(QStringLiteral("validation"), preview.validation);
            result.insert(QStringLiteral("requiresHostApproval"), true);
            result.insert(QStringLiteral("history"), QStringLiteral("none"));
            result.insert(QStringLiteral("sideEffect"), QStringLiteral("submit_native_solver_job"));
            result.insert(QStringLiteral("irreversible"), true);
        }
        if (state->previewOrder.size() >= 128)
        {
            const QString expired = state->previewOrder.takeFirst();
            state->previews.erase(expired);
        }
        state->previewOrder.append(preview.id);
        state->previews.emplace(preview.id, preview);
        return ok(result, QStringLiteral("preview"));
    }

    if (operation == QStringLiteral("operation.query"))
    {
        if (exactKeys(parameters, {QStringLiteral("idempotencyKey")})
            && validToken(parameters.value(QStringLiteral("idempotencyKey"))))
        {
            auto *catalog = session->getProjectCatalog();
            if (!catalog) return invalid(QStringLiteral("PERSISTENCE_UNAVAILABLE"),
                tr("The project operations ledger is unavailable."));
            QString error;
            const auto persisted = catalog->queryOperationByIdempotencyKey(
                parameters.value(QStringLiteral("idempotencyKey")).toString(), {}, &error);
            if (!error.isEmpty()) return invalid(QStringLiteral("PERSISTENCE_ERROR"), error);
            if (!persisted) return invalid(QStringLiteral("OPERATION_NOT_FOUND"),
                tr("No durable operation has been observed for this lookup reference."));
            return ok(durableOperationJson(*persisted));
        }
        if (!exactKeys(parameters, {QStringLiteral("operationId")}) || !validToken(parameters.value(QStringLiteral("operationId"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("operation.query requires an operation ID or a lookup reference."));
        const auto found = state->operations.find(parameters.value(QStringLiteral("operationId")).toString());
        if (found == state->operations.end())
        {
            auto *catalog = session->getProjectCatalog();
            QString error;
            const auto persisted = catalog ? catalog->queryOperation(parameters.value("operationId").toString(), {}, &error)
                                           : std::optional<RProjectCatalog::DurableOperation>{};
            if (!persisted) return invalid(QStringLiteral("OPERATION_NOT_FOUND"), error.isEmpty() ? tr("No durable operation with this ID is available.") : error);
            return ok(durableOperationJson(*persisted));
        }
        return ok(found->second);
    }

    if (operation == QStringLiteral("operations.query"))
    {
        for (auto it = parameters.begin(); it != parameters.end(); ++it)
            if (it.key() != "limit" && it.key() != "afterOperationId")
                return invalid("INVALID_PARAMETERS", tr("Unknown operations query field."));
        const int limit = parameters.value("limit").toInt(100);
        if ((parameters.contains("limit") && (!parameters.value("limit").isDouble()
                || parameters.value("limit").toDouble() != double(limit))) || limit < 1 || limit > 100
            || (parameters.contains("afterOperationId") && !validToken(parameters.value("afterOperationId"))))
            return invalid("INVALID_PARAMETERS", tr("Operations page limit must be 1–100."));
        auto *catalog = session->getProjectCatalog();
        if (!catalog) return invalid("PERSISTENCE_UNAVAILABLE", tr("The project operations ledger is unavailable."));
        QString error;
        const auto page = catalog->queryOperations(limit, parameters.value("afterOperationId").toString(), {}, &error);
        if (!error.isEmpty()) return invalid("PERSISTENCE_ERROR", error);
        QJsonArray items;
        for (const auto &item : page.operations) items.append(durableOperationJson(item));
        return ok({{"operations", items}, {"nextCursor", page.nextCursor}, {"persistence", "durable"}});
    }

    if (operation == QStringLiteral("command.execute") || operation == QStringLiteral("document.undo")
        || operation == QStringLiteral("document.redo"))
    {
        if (!exactKeys(parameters, {QStringLiteral("previewId"), QStringLiteral("idempotencyKey")})
            || !validToken(parameters.value(QStringLiteral("previewId")))
            || !validToken(parameters.value(QStringLiteral("idempotencyKey"))))
            return invalid(QStringLiteral("INVALID_PARAMETERS"), tr("Execution requires a previewId and idempotencyKey."));
        const QString previewId = parameters.value(QStringLiteral("previewId")).toString();
        const QString idempotencyKey = parameters.value(QStringLiteral("idempotencyKey")).toString();
        const QString requestDigest = QString::fromLatin1(QCryptographicHash::hash(
            (operation + QLatin1Char('\n') + previewId).toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto prior = state->idempotency.find(idempotencyKey);
        if (prior != state->idempotency.end())
        {
            if (prior->second.requestDigest != requestDigest)
                return invalid(QStringLiteral("IDEMPOTENCY_CONFLICT"), tr("This idempotencyKey is already bound to a different preview."));
            QJsonObject response = prior->second.response;
            response.insert(QStringLiteral("requestId"), requestId);
            return response;
        }
        auto *catalog = session->getProjectCatalog();
        if (!catalog) return invalid("PERSISTENCE_UNAVAILABLE", tr("The project operations ledger is unavailable."));
        const QJsonObject normalizedRequest{{"operation", operation}, {"previewId", previewId}};
        const QString durableRequestDigest = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(normalizedRequest).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QString ledgerError;
        const auto durablePrior = catalog->queryOperationByIdempotencyKey(idempotencyKey, {}, &ledgerError);
        if (!ledgerError.isEmpty()) return invalid("PERSISTENCE_ERROR", ledgerError);
        if (durablePrior)
        {
            if (durablePrior->normalizedRequestSha256 != durableRequestDigest)
                return invalid("IDEMPOTENCY_CONFLICT", tr("This key is durably bound to a different request."));
            if (durablePrior->phase == RProjectCatalog::OperationPhase::Observed
                && !durablePrior->association.observation.isEmpty())
            {
                const auto observed = durablePrior->association.observation;
                auto response = hostResponse(requestId, observed.value("status").toString(),
                    observed.value("code").toString(), observed.value("result").toObject());
                if (observed.contains("message")) response.insert("message", observed.value("message"));
                return response;
            }
            return invalid("OPERATION_RECONCILE_REQUIRED", tr("The earlier operation is unobserved; query its durable record before any new action."));
        }
        auto stored = state->previews.find(previewId);
        if (stored == state->previews.end()) return invalid(QStringLiteral("PREVIEW_EXPIRED"), tr("The preview is no longer available in this host session."));
        State::Preview &preview = stored->second;
        const QString expectedCommand = operation == QStringLiteral("document.undo") ? QStringLiteral("document.undo")
            : (operation == QStringLiteral("document.redo") ? QStringLiteral("document.redo") : QString());
        if (!expectedCommand.isEmpty() && preview.command != expectedCommand)
            return invalid(QStringLiteral("PREVIEW_MISMATCH"), tr("The approved preview is for a different document operation."));
        if (!preview.approved) return invalid(QStringLiteral("AUTHORIZATION_REQUIRED"), tr("A trusted GUI caller must approve this preview first."));
        if (preview.consumed) return invalid(QStringLiteral("PREVIEW_ALREADY_EXECUTED"), tr("This preview has already been used."));
        if (preview.sessionEpoch != state->sessionEpoch)
            return invalid(QStringLiteral("STALE_SESSION"), tr("The preview belongs to a different host session."));
        RProjectCatalog::OperationAssociation initialAssociation;
        initialAssociation.candidateSha256 = preview.nativePreview.value("candidateFingerprint").toString();
        initialAssociation.approvalSummarySha256 = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(QJsonObject{{"command", preview.command}, {"arguments", preview.arguments}})
                .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        const auto durableIntent = catalog->beginOperation(idempotencyKey, normalizedRequest, initialAssociation, {}, &ledgerError);
        if (durableIntent.disposition == RProjectCatalog::OperationDisposition::Reused)
        {
            const auto &record = durableIntent.operation;
            if (record.phase == RProjectCatalog::OperationPhase::Observed && !record.association.observation.isEmpty())
            {
                const auto observed = record.association.observation;
                auto response = hostResponse(requestId, observed.value("status").toString(),
                    observed.value("code").toString(), observed.value("result").toObject());
                if (observed.contains("message")) response.insert("message", observed.value("message"));
                return response;
            }
            return invalid("OPERATION_RECONCILE_REQUIRED", tr("Another runtime recorded this operation; query its outcome before any new action."));
        }
        if (durableIntent.operation.operationId.isEmpty() || durableIntent.disposition != RProjectCatalog::OperationDisposition::Created)
            return invalid("PERSISTENCE_ERROR", ledgerError.isEmpty() ? tr("Could not persist this operation intent.") : ledgerError);
        const QString operationId = durableIntent.operation.operationId;
        durableIntentId = operationId;
        const QString previousActiveOperation = state->activeOperationId;
        const QJsonArray previousChangedPaths = state->activeChangedPaths;
        auto finishCompleted = [&](const QJsonObject &operationResult, const QString &status,
                                   const QString &code, const QString &message) {
            QJsonObject observedResult = operationResult;
            RProjectCatalog::OperationAssociation association = initialAssociation;
            const uint currentModel = session->findModelByCommandIdentity(preview.documentId);
            QString snapshotError;
            if (status == "accepted" && !observedResult.value("noOp").toBool()
                && observedResult.contains("afterRevision") && currentModel != RConstants::eod)
            {
                const auto snapshot = session->createDocumentSnapshot(currentModel, &snapshotError);
                if (!snapshot.isEmpty())
                {
                    observedResult.insert("acceptedSnapshotRef", snapshot);
                    observedResult.insert("persistence", "accepted_snapshot");
                    association.acceptedRevision = snapshot.value("sourceRevision").toString();
                    association.acceptedModelSha256 = snapshot.value("modelSha256").toString();
                }
                else
                {
                    observedResult.insert("recoveryProtection", "memory_only");
                    observedResult.insert("recoveryDiagnostic", snapshotError);
                }
            }
            QJsonObject response = hostResponse(requestId, status, code, observedResult);
            if (!message.isEmpty()) response.insert(QStringLiteral("message"), message);
            // The response may contain a trusted-authorization digest or other
            // fields which the project operation ledger deliberately refuses
            // to persist. Keep the caller response intact, but store only a
            // recursively redacted observation for restart/idempotency replay.
            const QJsonObject durableResult = durableObservationValue(QJsonValue(observedResult)).toObject();
            association.observation = {{"status", status}, {"code", code}, {"result", durableResult}};
            if (!message.isEmpty()) association.observation.insert("message", message);
            association.diagnosticCode = status == "accepted" ? QString() : code.toLower();
            const bool acceptedRecorded = status != "accepted" || catalog->advanceOperation(
                operationId, RProjectCatalog::OperationPhase::Accepted, association, {}, &ledgerError);
            if (!acceptedRecorded || !catalog->advanceOperation(operationId, RProjectCatalog::OperationPhase::Observed,
                                            association, {}, &ledgerError))
            {
                response = hostResponse(requestId, "error", "OBSERVED_MODIFIED_UNLOGGED", observedResult);
                response.insert("message", tr("The operation happened, but its durable observation failed: %1").arg(ledgerError));
            }
            preview.consumed = true;
            if (state->operationOrder.size() >= 256)
            {
                const QString expired = state->operationOrder.takeFirst();
                state->operations.erase(expired);
            }
            state->operationOrder.append(operationId);
            // Keep process-local operation.query consistent with the durable
            // observation. The direct command response above remains complete,
            // while later query/replay paths never re-expose catalog-redacted
            // authorization fields.
            state->operations.emplace(operationId, durableResult);
            if (state->idempotencyOrder.size() >= 256)
            {
                const QString expired = state->idempotencyOrder.takeFirst();
                state->idempotency.erase(expired);
            }
            state->idempotencyOrder.append(idempotencyKey);
            state->idempotency.emplace(idempotencyKey,
                State::IdempotentResult{requestDigest, preview.documentId, response});
            durableIntentId.clear();
            return response;
        };
        auto finishAccepted = [&](const QJsonObject &operationResult) {
            return finishCompleted(operationResult, QStringLiteral("accepted"), QStringLiteral("OK"), QString());
        };
        auto finishModifiedUnlogged = [&](const QJsonObject &operationResult, const QString &message) {
            return finishCompleted(operationResult, QStringLiteral("error"),
                                   QStringLiteral("OBSERVED_MODIFIED_UNLOGGED"), message);
        };

        if (preview.command == "experiment.run")
        {
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before experiment execution."));
            const auto source = experimentSource(session, state->sessionEpoch, preview.documentId);
            preview.consumed = true;
            durableSideEffect = true;
            const auto run = state->experiments->run(preview.experimentId, source);
            auto resultFor = [&](const QString &childJobId = QString()) {
                const auto loaded = state->experiments->query(preview.experimentId);
                QJsonObject result{{"operationId", operationId}, {"command", preview.command},
                    {"experimentId", preview.experimentId}, {"jobId", childJobId},
                    {"state", loaded.data.value("experiment").toObject().value("state")},
                    {"sourceDocumentModified", false}, {"history", "none"}};
                return result;
            };
            if (!run.ok)
                return finishCompleted(resultFor(), "rejected", run.code,
                    tr("The trusted experiment could not enter its run state."));
            if (run.data.value("idempotent").toBool())
            {
                const auto jobs = experimentJobs(preview.experimentId);
                return finishAccepted(resultFor(jobs.isEmpty() ? QString() : jobs.last().toObject().value("jobId").toString()));
            }
            const auto stop = [&](const QString &code, const QJsonObject &evidence) {
                const auto stopped = state->experiments->stop(preview.experimentId, source, code,
                    QStringLiteral("Trusted experiment stopped before child submission."), evidence);
                return finishCompleted(resultFor(), "rejected", stopped.ok ? code : stopped.code,
                    tr("The trusted experiment stopped before a child solve was submitted."));
            };
            const auto spec = preview.experimentProblemSpecInput;
            auto diagnosis = RTrustedMechanicsService::diagnose(spec, nullptr, source.documentId, source.documentRevision);
            if (diagnosis.value("status") == QJsonValue("error"))
                return stop("EXPERIMENT_DIAGNOSIS_FAILED", {{"diagnosis", diagnosis}});
            auto candidate = RTrustedMechanicsService::createCandidate(spec, source.documentId, source.documentRevision);
            if (!candidate.valid)
                return stop(candidate.code.isEmpty() ? QStringLiteral("CANDIDATE_BUILD_FAILED") : candidate.code,
                    {{"diagnosis", diagnosis}, {"candidateCode", candidate.code}, {"candidateMessage", candidate.message}});
            diagnosis = RTrustedMechanicsService::diagnose(spec, &candidate.model, source.documentId, source.documentRevision);
            if (diagnosis.value("status") == QJsonValue("error"))
                return stop("EXPERIMENT_DIAGNOSIS_FAILED", {{"diagnosis", diagnosis}});
            std::unique_ptr<SolverTask> task;
            try
            {
                Model detached;
                static_cast<RModel&>(detached) = candidate.model;
                auto &manager = SolverManager::getInstance();
                task = std::make_unique<SolverTask>(Application::instance()->getApplicationSettings(), detached,
                    qMakePair(source.documentId, source.documentRevision.toULongLong()), preview.experimentAuthorization,
                    QDir(state->experimentRoot).filePath(preview.experimentId), manager.getTaskServerName(),
                    SolverTask::TrustedExperimentCandidateTag{});
                QString error;
                if (!task->captureTrustedCandidateSnapshot(detached, &error))
                    return stop("CANDIDATE_CAPTURE_FAILED", {{"diagnosis", diagnosis}, {"captureDiagnostic", error.left(2048)}});
            }
            catch (const RError &)
            {
                return stop("CANDIDATE_CAPTURE_FAILED", {{"diagnosis", diagnosis}});
            }
            catch (const std::exception &)
            {
                return stop("CANDIDATE_CAPTURE_FAILED", {{"diagnosis", diagnosis}});
            }
            if (spec.value(QStringLiteral("analysis")).toObject()
                    .value(QStringLiteral("kind")).toString() == QStringLiteral("linearTransient"))
            {
                QString checkpointPreparationError;
                if (!task->prepareNativeCheckpointInput(&checkpointPreparationError))
                    return stop("CANDIDATE_CHECKPOINT_PREPARATION_FAILED",
                        {{"diagnosis", diagnosis},
                         {"checkpointPreparationDiagnostic", checkpointPreparationError.left(2048)}});
            }
            const auto attempt = state->experiments->beginSolve(preview.experimentId, source, spec,
                QString::fromLatin1(task->getInputSnapshotSha256().toHex()),
                QString::fromLatin1(task->getInputPackageSha256().toHex()));
            if (!attempt.ok) return stop(attempt.code, {{"diagnosis", diagnosis}});
            const QString attemptId = attempt.data.value("attemptId").toString();
            QString error;
            QString childJobId;
            if (task->setTrustedExperimentAttemptId(attemptId))
                childJobId = SolverManager::getInstance().submitTrustedDetached(task.release(), &error);
            if (childJobId.isEmpty())
            {
                const auto finished = state->experiments->finishSolve(preview.experimentId,
                    experimentSource(session, state->sessionEpoch, preview.documentId), attemptId, false, {},
                    "stop", "CHILD_SUBMISSION_FAILED", error.isEmpty()
                        ? QStringLiteral("The detached child solver submission was rejected.") : error.left(2048));
                return finishCompleted(resultFor(), "rejected", finished.ok ? QStringLiteral("CHILD_SUBMISSION_FAILED") : finished.code,
                    tr("The trusted experiment child solve could not be submitted."));
            }
            // Submission may have synchronously emitted a terminal registry update.
            finalizeExperimentJob(childJobId);
            return finishAccepted(resultFor(childJobId));
        }

        if (preview.command == "mesh.generate" || preview.command == "mesh.accept" || preview.command == "mesh.cancel")
        {
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before mesh execution."));
            if (preview.command != "mesh.generate"
                && ownedMeshAttempt(preview.jobId, preview.documentId, preview.sessionEpoch, preview.stampGeneration).isEmpty())
                return invalid("MESH_SOURCE_MISMATCH", tr("The owned mesh attempt no longer matches its approved source."));
            const QString meshCommand = preview.command;
            const QString controllerPreview = preview.meshPreviewId;
            const QString attempt = preview.jobId;
            const QString executionWorkspace = session->projectWorkspace();
            preview.consumed = true; // Controller approval is one-shot, including uncertain publication outcomes.
            durableSideEffect = true;
            const auto executed = meshCommand == "mesh.generate" ? state->meshController->approveGenerate(controllerPreview)
                : meshCommand == "mesh.accept" ? state->meshController->approveAccept(controllerPreview)
                                               : state->meshController->cancel(attempt);
            if (session->projectWorkspace() != executionWorkspace || session->getProjectCatalog() != catalog)
                return invalid("MESH_EXECUTION_UNKNOWN", tr("Mesh callbacks changed the project workspace; reconcile the original attempt before any retry."));
            QJsonObject result = meshReceipt(executed.value("result").toObject());
            result.insert("command", meshCommand); result.insert("operationId", operationId);
            result.insert("sourceDocumentModified", false); result.insert("history", "none");
            if (!executed.value("valid").toBool())
            {
                // The controller owns durable stop/publication uncertainty. Never turn it into a retryable approval.
                return invalid(executed.value("code").toString(), executed.value("message").toString());
            }
            return finishAccepted(result);
        }

        if (preview.command == "analysis.recover")
        {
            if (activeModalInteraction() || isMutationBlocked())
                return invalid("INTERACTION_BLOCKED", tr("Finish the current document interaction before native recovery."));
            const auto qualification = SolverManager::getInstance().nativeJobRecoveryQuery(preview.jobId);
            if (qualification != preview.recoveryQualification)
                return invalid("RECOVERY_CHANGED", tr("Checkpoint qualification changed after preview; a new approval is required."));
            QString error;
            const QString attempt = SolverManager::getInstance().submitNativeRecovery(preview.jobId,
                preview.arguments.value("checkpointSha256").toString(), uint(preview.arguments.value("remainingSteps").toInt()), &error,
                preview.recoveryQualification.value("checkpoint").toObject()
                    .value("pendingRecoveryPlanSha256").toString());
            if (attempt.isEmpty()) return invalid("RECOVERY_REJECTED", error);
            durableSideEffect = true;
            return finishAccepted({{"operationId", operationId}, {"command", preview.command},
                {"jobId", attempt}, {"recoverySourceJobId", preview.jobId},
                {"checkpointSha256", preview.arguments.value("checkpointSha256")},
                {"remainingSteps", preview.arguments.value("remainingSteps")},
                {"sourceDocumentModified", false}, {"newAttempt", true}, {"history", "none"}});
        }

        if (preview.command == QStringLiteral("analysis.cancel"))
        {
            if (activeModalInteraction())
                return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before cancelling a solver job."));
            const auto owned = state->jobSources.find(preview.jobId);
            const QJsonObject currentJob = SolverManager::getInstance().jobQuery(preview.jobId);
            const QJsonObject currentSource = currentJob.value(QStringLiteral("sourceRuntimeStamp")).toObject();
            const QString jobState = currentJob.value(QStringLiteral("state")).toString();
            if (!currentJob.value(QStringLiteral("found")).toBool()
                || owned == state->jobSources.end()
                || owned->second.documentId != preview.jobSourceDocumentId
                || owned->second.sessionEpoch != preview.sessionEpoch
                || owned->second.revision != preview.jobSourceRevision
                || currentSource.value(QStringLiteral("documentId")).toString() != preview.jobSourceDocumentId
                || currentSource.value(QStringLiteral("revision")).toString() != preview.jobSourceRevision
                || (!currentSource.value(QStringLiteral("sessionEpoch")).toString().isEmpty()
                    && currentSource.value(QStringLiteral("sessionEpoch")).toString() != preview.sessionEpoch))
                return invalid(QStringLiteral("JOB_SOURCE_MISMATCH"), tr("The job no longer matches its host-owned immutable source."));
            if (jobState != QStringLiteral("queued") && jobState != QStringLiteral("running")
                && jobState != QStringLiteral("cancelling"))
                return invalid(QStringLiteral("JOB_TERMINAL"), tr("The selected native job is already terminal."));
            QString cancelError;
            if (!SolverManager::getInstance().cancelJob(preview.jobId, &cancelError))
                return invalid(QStringLiteral("CANCEL_REJECTED"), cancelError);
            durableSideEffect = true;
            QJsonObject afterJob = SolverManager::getInstance().jobQuery(preview.jobId);
            addHostJobSource(&afterJob, preview.jobId);
            QJsonObject event = state->appendEvent(QStringLiteral("analysis.job_cancellation_requested"),
                preview.jobSourceDocumentId, {{QStringLiteral("jobId"), preview.jobId},
                    {QStringLiteral("sourceRevision"), preview.jobSourceRevision},
                    {QStringLiteral("cancellationRequested"), true}});
            emit eventPublished(event);
            QJsonObject operationResult{{QStringLiteral("operationId"), operationId},
                {QStringLiteral("command"), preview.command}, {QStringLiteral("jobId"), preview.jobId},
                {QStringLiteral("sourceDocumentId"), preview.jobSourceDocumentId},
                {QStringLiteral("sourceSessionEpoch"), preview.sessionEpoch},
                {QStringLiteral("sourceRevision"), preview.jobSourceRevision},
                {QStringLiteral("job"), afterJob},
                {QStringLiteral("cancellationState"), afterJob.value(QStringLiteral("state"))},
                {QStringLiteral("cancellationRequested"), true},
                {QStringLiteral("terminalStateDeterminedBySolver"), true},
                {QStringLiteral("sideEffect"), QStringLiteral("cancel_specific_native_job")},
                {QStringLiteral("irreversible"), true},
                {QStringLiteral("history"), QStringLiteral("none")},
                {QStringLiteral("eventSeq"), double(state->nextEventSeq - 1)}};
            state->activeOperationId = previousActiveOperation;
            state->activeChangedPaths = previousChangedPaths;
            return finishAccepted(operationResult);
        }

        if (preview.command == QStringLiteral("analysis.run"))
        {
            if (activeModalInteraction())
                return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before starting a solver job."));
            if (isMutationBlocked())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The source model is held by an active legacy task."));
            uint modelID = session->findModelByCommandIdentity(preview.documentId);
            if (modelID == RConstants::eod)
                return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            QString projectFailure;
            if (!session->canMutateDocument(modelID, &projectFailure))
                return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
            auto stamp = session->modelCommandStamp(modelID);
            if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview. Query and preview the command again."));
            QJsonObject validation = SolverManager::getInstance().validateRun(modelID);
            if (!validation.value(QStringLiteral("valid")).toBool()
                || !validation.value(QStringLiteral("errors")).toArray().isEmpty()
                || validation.value(QStringLiteral("sourceStamp")).toObject() != sourceStampJson(stamp, state->sessionEpoch))
                return invalid(QStringLiteral("SOLVER_VALIDATION_FAILED"), tr("The native solver no longer accepts the approved source state."));
            const bool warningsAccepted = preview.arguments.value(QStringLiteral("warningsAccepted")).toBool();
            if (!validation.value(QStringLiteral("warnings")).toArray().isEmpty() && !warningsAccepted)
                return invalid(QStringLiteral("WARNINGS_NOT_ACCEPTED"), tr("The approved command did not accept the current solver warnings."));

            // Snapshot capture acquires/releases the Session mutex internally.
            QString snapshotError;
            const QJsonObject projectSnapshotRef = session->createDocumentSnapshot(modelID, &snapshotError);
            if (projectSnapshotRef.isEmpty())
                return invalid(QStringLiteral("SOURCE_SNAPSHOT_FAILED"), snapshotError);
            const QJsonObject instanceRef = projectSnapshotRef.value(QStringLiteral("instanceRef")).toObject();
            if (instanceRef.value(QStringLiteral("documentId")).toString() != preview.documentId
                || instanceRef.value(QStringLiteral("sessionEpoch")).toString() != preview.sessionEpoch
                || instanceRef.value(QStringLiteral("documentRevision")).toDouble(-1.0) != double(preview.stampGeneration)
                || session->modelCommandStamp(modelID) != stamp)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The source changed while its project snapshot was being captured."));
            const QJsonObject approvedProject = preview.projectSnapshotRef;
            const QJsonObject currentProject = session->documentProjectContext(modelID);
            for (const QString &field : {QStringLiteral("projectId"), QStringLiteral("persistentDocumentId"),
                                         QStringLiteral("sourceRevision")})
                if (approvedProject.value(field) != currentProject.value(field))
                    return invalid(QStringLiteral("PROJECT_CONFLICT"), tr("The persistent project identity changed after preview."));
            if (!approvedProject.value(QStringLiteral("modelSha256")).toString().isEmpty()
                && approvedProject.value(QStringLiteral("modelSha256")) != currentProject.value(QStringLiteral("modelSha256")))
                return invalid(QStringLiteral("PROJECT_CONFLICT"), tr("The accepted project model hash changed after preview."));

            QString submitError;
            const QString jobId = SolverManager::getInstance().submitValidated(
                modelID, projectSnapshotRef, warningsAccepted, &submitError);
            if (jobId.isEmpty())
                return invalid(QStringLiteral("SUBMISSION_REJECTED"), submitError);
            durableSideEffect = true;
            state->jobSources[jobId] = State::JobSource{preview.documentId, preview.sessionEpoch,
                QString::number(preview.stampGeneration), projectSnapshotRef};
            QJsonObject job = SolverManager::getInstance().jobQuery(jobId);
            addHostJobSource(&job, jobId);
            QJsonObject event = state->appendEvent(QStringLiteral("analysis.job_submitted"), preview.documentId,
                {{QStringLiteral("jobId"), jobId},
                 {QStringLiteral("sourceRevision"), QString::number(preview.stampGeneration)},
                 {QStringLiteral("sourceSnapshotRef"), projectSnapshotRef}});
            emit eventPublished(event);
            QJsonObject operationResult{{QStringLiteral("operationId"), operationId},
                {QStringLiteral("command"), preview.command}, {QStringLiteral("jobId"), jobId},
                {QStringLiteral("sourceDocumentId"), preview.documentId},
                {QStringLiteral("sourceSessionEpoch"), preview.sessionEpoch},
                {QStringLiteral("sourceRevision"), QString::number(preview.stampGeneration)},
                {QStringLiteral("sourceSnapshotRef"), projectSnapshotRef},
                {QStringLiteral("job"), job},
                {QStringLiteral("progress"), job.value(QStringLiteral("progress"))},
                {QStringLiteral("progressStatus"), job.value(QStringLiteral("progressStatus"))},
                {QStringLiteral("cancellationState"), job.value(QStringLiteral("cancellationState"))},
                {QStringLiteral("sideEffect"), QStringLiteral("submit_native_solver_job")},
                {QStringLiteral("irreversible"), true},
                {QStringLiteral("history"), QStringLiteral("none")},
                {QStringLiteral("sourceRevisionChanged"), false},
                {QStringLiteral("eventSeq"), double(state->nextEventSeq - 1)}};
            state->activeOperationId = previousActiveOperation;
            state->activeChangedPaths = previousChangedPaths;
            return finishAccepted(operationResult);
        }

        if (activeModalInteraction())
            return invalid(QStringLiteral("INTERACTION_BLOCKED"), tr("Close the active modal interaction before changing document state."));
        if (isMutationBlocked())
            return invalid(QStringLiteral("MODEL_BUSY"), tr("The target model is held by an active legacy task."));
        uint modelID = session->findModelByCommandIdentity(preview.documentId);
        if (modelID == RConstants::eod)
            return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
        QString projectFailure;
        if (!session->canMutateDocument(modelID, &projectFailure))
            return invalid(session->hasDocumentDraft(modelID) ? QStringLiteral("DRAFT_CONFLICT") : QStringLiteral("PROJECT_CONFLICT"), projectFailure);
        auto stamp = session->modelCommandStamp(modelID);
        if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
            return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview. Query and preview the command again."));
        const QJsonArray changedPaths = preview.hasSupportedDelta
            ? preview.nativePreview.value(QStringLiteral("changedPaths")).toArray()
            : QJsonArray{QStringLiteral("document.history")};
        bool auditRecorded = true;
        QString historyAction;
        QString corePreStateHash;
        QString corePostStateHash;
        QString coreJournalRevision;
        QString coreJournalRequestId;
        QString coreJournalIdempotencyKey;
        QString auditCommandId;
        QString preStateHash;
        QString postStateHash;
        QString auditError;
        bool auditReconciled = false;
        REntityGroupType changedGroupType = R_ENTITY_GROUP_NONE;
        uint changedGroupIndex = RConstants::eod;
        const bool coreEdit = preview.hasSupportedDelta;
        const bool undo = preview.command == QStringLiteral("document.undo");
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction())
                return invalid(QStringLiteral("MODEL_BUSY"), tr("The document could not be changed safely."));
            modelID = session->findModelByCommandIdentity(preview.documentId);
            if (modelID == RConstants::eod)
                return invalid(QStringLiteral("DOCUMENT_CLOSED"), tr("The target document is no longer open."));
            stamp = session->modelCommandStamp(modelID);
            if (stamp.first != preview.documentId || stamp.second != preview.stampGeneration)
                return invalid(QStringLiteral("STALE_REVISION"), tr("The document changed after this preview."));
            auto &model = session->getModel(modelID);
            preStateHash = RModelCommandService::stateFingerprint(model);
            if (coreEdit)
            {
                const QString currentFingerprint = RModelCommandService::stateFingerprint(model);
                preStateHash = currentFingerprint;
                QJsonObject currentReadSetSource{{QStringLiteral("documentId"), preview.documentId},
                    {QStringLiteral("sessionEpoch"), preview.sessionEpoch},
                    {QStringLiteral("revision"), QString::number(stamp.second)},
                    {QStringLiteral("inputStateFingerprint"), currentFingerprint},
                    {QStringLiteral("readSet"), preview.readSet}};
                const QString currentReadSetHash = QString::fromLatin1(QCryptographicHash::hash(
                    QJsonDocument(currentReadSetSource).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
                if (currentFingerprint != preview.inputFingerprint || currentReadSetHash != preview.readSetHash)
                    return invalid(QStringLiteral("STALE_READ_SET"), tr("The approved model read set changed after preview."));
                const auto currentCandidate = RModelCommandService::preview(model, preview.command,
                    preview.arguments, preview.documentId, QString::number(stamp.second));
                if (!currentCandidate.valid
                    || currentCandidate.fingerprint != preview.nativePreview.value(QStringLiteral("candidateFingerprint")).toString())
                    return invalid(QStringLiteral("STALE_PREVIEW"), tr("The current model no longer produces the approved candidate fingerprint."));
                auto entry = state->entries.find(preview.documentId);
                if (entry == state->entries.end() || !entry->second.editing
                    || entry->second.generation != stamp.second)
                    return invalid(QStringLiteral("SESSION_UNAVAILABLE"), tr("The accepted command session is no longer current."));
                const QJsonObject editingQuery = entry->second.editing->query();
                if (editingQuery.value(QStringLiteral("revision")).toString() != preview.nativeRevision
                    || RModelCommandService::stateFingerprint(entry->second.editing->model()) != currentFingerprint)
                    return invalid(QStringLiteral("STALE_REVISION"), tr("The accepted command session no longer matches the approved live model."));

                if (preview.delta.geometry.noOp)
                {
                    guard.unlock();
                    return finishAccepted({{"operationId", operationId}, {"documentId", preview.documentId},
                        {"sessionEpoch", state->sessionEpoch}, {"command", preview.command},
                        {"beforeRevision", double(stamp.second)}, {"afterRevision", double(stamp.second)},
                        {"changedPaths", QJsonArray{}}, {"noOp", true}, {"history", "none"},
                        {"fileSaveRequired", false}, {"coreJournalRecorded", false},
                        {"preStateHash", currentFingerprint}, {"postStateHash", currentFingerprint},
                        {"outcome", "no_op"}});
                }

                coreJournalRequestId = uuid();
                coreJournalIdempotencyKey = uuid();
                QJsonObject nativeRequest{{QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                    {QStringLiteral("commandId"), preview.command}, {QStringLiteral("commandVersion"), 1},
                    {QStringLiteral("modelId"), editingQuery.value(QStringLiteral("modelId"))},
                    {QStringLiteral("expectedRevision"), preview.nativeRevision},
                    {QStringLiteral("requestId"), coreJournalRequestId},
                    {QStringLiteral("idempotencyKey"), coreJournalIdempotencyKey},
                    {QStringLiteral("parameters"), preview.arguments}, {QStringLiteral("dryRun"), false}};
                const QJsonObject acceptedCore = entry->second.editing->execute(nativeRequest);
                if (acceptedCore.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("accepted")))
                {
                    const QString coreCode = acceptedCore.value(QStringLiteral("code")).toString();
                    return invalid(coreCode == QStringLiteral("journal_failure")
                                       ? QStringLiteral("COMMAND_JOURNAL_FAILURE")
                                       : coreCode.isEmpty() ? QStringLiteral("COMMAND_REJECTED") : coreCode.toUpper(),
                                   acceptedCore.value(QStringLiteral("message")).toString());
                }
                durableSideEffect = true;
                coreJournalRevision = acceptedCore.value(QStringLiteral("revision")).toString();
                auditCommandId = preview.command;
                corePreStateHash = currentFingerprint;
                corePostStateHash = RModelCommandService::stateFingerprint(currentCandidate.candidate);
                if (entry->second.editing->query().value(QStringLiteral("revision")).toString() != coreJournalRevision
                    || RModelCommandService::stateFingerprint(entry->second.editing->model()) != corePostStateHash)
                {
                    QString reconcileError;
                    const bool reconciled = entry->second.editing->recordExternalChange(
                        QStringLiteral("external_edit"), model, &reconcileError);
                    entry->second.editing.reset();
                    return invalid(QStringLiteral("COMMAND_AUDIT_DIVERGED"), reconciled
                        ? tr("The core journal accepted a candidate that did not match the live model; the session was reconciled without changing the GUI model.")
                        : tr("The core journal accepted a candidate that did not match the live model, and reconciliation failed: %1").arg(reconcileError));
                }
                QString applyError;
                if (!RModelCommandService::applyDelta(model, preview.delta, true).valid)
                    return invalid(QStringLiteral("STALE_PREVIEW"), tr("The approved command no longer matches the current model fields."));
                QString undoMessage;
                if (preview.command == QStringLiteral("analysis.modal.configure")) undoMessage = tr("Configure modal analysis");
                else if (preview.command == QStringLiteral("analysis.structural_dynamics.configure")) undoMessage = tr("Configure structural dynamics");
                else if (preview.command == QStringLiteral("entity.material.set")) undoMessage = tr("Set material properties");
                else if (preview.delta.kind == RModelCommandService::Delta::Kind::Geometry) undoMessage = tr("Edit node geometry");
                else if (preview.delta.kind == RModelCommandService::Delta::Kind::MeshRebind) undoMessage = tr("Bind static mesh analysis");
                else if (preview.delta.kind == RModelCommandService::Delta::Kind::PiezoExperiment) undoMessage = tr("Configure piezo experiment");
                else undoMessage = tr("Set boundary condition");
                if (preview.delta.physicalId
                    && !model.findElementGroupByPhysicalId(preview.delta.physicalId, &changedGroupType, &changedGroupIndex))
                    return invalid(QStringLiteral("STALE_TARGET"), tr("The physical target no longer exists."));
                const bool meshBinding = preview.delta.kind == RModelCommandService::Delta::Kind::MeshRebind;
                const bool piezoExperiment = preview.delta.kind == RModelCommandService::Delta::Kind::PiezoExperiment;
                if (!meshBinding && !piezoExperiment) session->storeCurentModelVersion(modelID, undoMessage);
                const bool applied = meshBinding
                    ? model.commitStaticMeshBindingWithHistory(currentCandidate.candidate, preview.delta,
                        Application::instance()->getApplicationSettings()->getNHistoryRecords(), undoMessage, &applyError)
                    : piezoExperiment
                    ? model.commitPiezoExperimentWithHistory(currentCandidate.candidate,
                        preview.delta.piezoExperiment.beforeStateSha256,preview.delta.piezoExperiment.afterStateSha256,
                        Application::instance()->getApplicationSettings()->getNHistoryRecords(),undoMessage,&applyError)
                    : commitSupportedDelta(model, preview.delta, &applyError);
                if (!applied)
                {
                    QString reconcileError;
                    const bool reconciled = entry->second.editing->recordExternalChange(
                        QStringLiteral("external_edit"), model, &reconcileError);
                    entry->second.editing.reset();
                    return invalid(QStringLiteral("COMMAND_COMMIT_DIVERGED"), reconciled
                        ? tr("The core journal accepted the command, but GUI publication failed before changing the model: %1").arg(applyError)
                        : tr("GUI publication failed and core audit reconciliation also failed: %1").arg(reconcileError));
                }
                if (RModelCommandService::stateFingerprint(model) != corePostStateHash)
                {
                    QString reconcileError;
                    const bool reconciled = entry->second.editing->recordExternalChange(
                        QStringLiteral("external_edit"), model, &reconcileError);
                    entry->second.editing.reset();
                    return invalid(QStringLiteral("COMMAND_COMMIT_DIVERGED"), reconciled
                        ? tr("The published model did not match its accepted core audit state.")
                        : tr("The published model differed from its core audit state and reconciliation failed: %1").arg(reconcileError));
                }
                postStateHash = corePostStateHash;
            }
            else
            {
                const uint depth = undo ? model.getUndoStackSize() : model.getRedoStackSize();
                historyAction = undo ? model.getUndoActionMessage() : model.getRedoActionMessage();
                if (depth == 0 || historyAction != preview.nativePreview.value(QStringLiteral("historyAction")).toString())
                    return invalid(QStringLiteral("STALE_PREVIEW"), tr("The selected GUI history item is no longer current."));
                if (!preview.staticBindingHistoryId.isEmpty())
                {
                    if (preStateHash != preview.inputFingerprint
                        || model.staticMeshBindingHistoryIdentity(undo) != preview.staticBindingHistoryId)
                        return invalid(QStringLiteral("STALE_PREVIEW"), tr("The approved static binding history source or item changed."));
                    QString historyError;
                    if (!model.applyStaticMeshBindingHistory(undo, preview.staticBindingHistoryId,
                                                            preview.staticBindingHistoryHash, &historyError))
                        return invalid(QStringLiteral("UNSUPPORTED_HISTORY"), historyError);
                    if (undo) session->getPickList().removeItems(modelID, R_ENTITY_GROUP_POINT);
                }
                else if (!preview.piezoHistoryId.isEmpty())
                {
                    if (preStateHash!=preview.inputFingerprint
                        || model.piezoExperimentHistoryIdentity(undo)!=preview.piezoHistoryId)
                        return invalid(QStringLiteral("STALE_PREVIEW"),tr("The approved piezo experiment history source or item changed."));
                    const uint nodesBefore=model.getNNodes(),elementsBefore=model.getNElements();
                    QString historyError;
                    if (!model.applyPiezoExperimentHistory(undo,preview.piezoHistoryId,preview.piezoHistoryHash,&historyError))
                        return invalid(QStringLiteral("UNSUPPORTED_HISTORY"),historyError);
                    if (nodesBefore!=model.getNNodes() || elementsBefore!=model.getNElements())session->getPickList().removeItems(modelID);
                }
                else
                {
                    if (model.hasStaticMeshBindingHistory(undo) || model.hasPiezoExperimentHistory(undo))
                        return invalid(QStringLiteral("STALE_PREVIEW"), tr("The selected GUI history item changed."));
                    session->getPickList().removeItems(modelID);
                    if (undo) model.undo(0); else model.redo(0);
                }
                durableSideEffect = true;
                postStateHash = RModelCommandService::stateFingerprint(model);
            }
            guard.unlock();
        }
        state->activeOperationId = operationId;
        state->activeChangedPaths = changedPaths;
        if (coreEdit && (preview.delta.kind == RModelCommandService::Delta::Kind::Material
                         || preview.delta.kind == RModelCommandService::Delta::Kind::Boundary))
        {
            if (preview.delta.kind == RModelCommandService::Delta::Kind::Material)
                session->setMaterialChanged(modelID, changedGroupType, changedGroupIndex);
            else
                session->setBoundaryConditionChanged(modelID, changedGroupType, changedGroupIndex);
        }
        else if (coreEdit && (preview.delta.kind == RModelCommandService::Delta::Kind::Geometry
                             || preview.delta.kind == RModelCommandService::Delta::Kind::MeshRebind
                             || preview.delta.kind == RModelCommandService::Delta::Kind::PiezoExperiment))
        {
            session->setModelChanged(modelID);
        }
        else if (coreEdit)
        {
            session->setProblemChanged(modelID);
        }
        else
        {
            session->setModelChanged(modelID);
        }
        const QPair<QString, quint64> afterStamp = session->modelCommandStamp(
            session->findModelByCommandIdentity(preview.documentId));
        if (coreEdit)
        {
            auto entry = state->entries.find(preview.documentId);
            if (entry != state->entries.end() && entry->second.editing
                && entry->second.editing->query().value(QStringLiteral("revision")).toString() == coreJournalRevision
                && RModelCommandService::stateFingerprint(entry->second.editing->model())
                    == RModelCommandService::stateFingerprint(session->getModel(modelID)))
            {
                entry->second.generation = afterStamp.second;
                auditCommandId = preview.command;
            }
            else
            {
                auditError = tr("The model changed, but its accepted command journal state did not match the published GUI model.");
                auditRecorded = recordExternalChangeAfterNotification(
                    modelID, QStringLiteral("external_edit"), &auditError);
                auditReconciled = auditRecorded;
                if (auditRecorded) auditCommandId = QStringLiteral("model.external.external_edit");
            }
        }
        else
        {
            const QString externalReason = undo ? QStringLiteral("undo") : QStringLiteral("redo");
            auditRecorded = recordExternalChangeAfterNotification(modelID, externalReason, &auditError);
            auditCommandId = QStringLiteral("model.external.") + externalReason;
        }
        if (!auditRecorded)
            RLogger::error("The GUI model changed but its audit could not be completed: %s\n", auditError.toUtf8().constData());
        state->activeOperationId = previousActiveOperation;
        state->activeChangedPaths = previousChangedPaths;
        QJsonObject operationResult{{QStringLiteral("operationId"), operationId},
            {QStringLiteral("documentId"), preview.documentId},
            {QStringLiteral("sessionEpoch"), state->sessionEpoch},
            {QStringLiteral("beforeRevision"), double(preview.stampGeneration)},
            {QStringLiteral("afterRevision"), double(afterStamp.second)},
            {QStringLiteral("revisionScope"), QStringLiteral("session-command-invalidation-stamp")},
            {QStringLiteral("command"), preview.command}, {QStringLiteral("changedPaths"), changedPaths},
            {QStringLiteral("eventSeq"), double(state->nextEventSeq - 1)},
            {QStringLiteral("persistence"), QStringLiteral("memory_only")},
            {QStringLiteral("fileSaveRequired"), true},
            {QStringLiteral("auditRecorded"), auditRecorded},
            {QStringLiteral("auditCommandId"), auditCommandId},
            {QStringLiteral("preStateHash"), preStateHash},
            {QStringLiteral("postStateHash"), postStateHash},
            {QStringLiteral("auditReconciled"), auditReconciled},
            {QStringLiteral("history"), QStringLiteral("one_gui_undo_group")},
            {QStringLiteral("candidateFingerprint"), preview.nativePreview.value(QStringLiteral("candidateFingerprint"))}};
        if (coreEdit)
        {
            operationResult.insert(QStringLiteral("acceptedStateRevision"), QString::number(afterStamp.second));
            operationResult.insert(QStringLiteral("coreJournalRevision"), coreJournalRevision);
            operationResult.insert(QStringLiteral("coreJournalRequestId"), coreJournalRequestId);
            operationResult.insert(QStringLiteral("coreJournalIdempotencyKey"), coreJournalIdempotencyKey);
            operationResult.insert(QStringLiteral("coreJournalRecorded"), !coreJournalRevision.isEmpty());
        }
        else
            operationResult.insert(QStringLiteral("historyAction"), historyAction);
        if (!auditRecorded)
        {
            operationResult.insert(QStringLiteral("outcome"), QStringLiteral("observed_modified_unlogged"));
            operationResult.insert(QStringLiteral("auditError"), auditError);
            operationResult.insert(QStringLiteral("modelModified"), true);
            return finishModifiedUnlogged(operationResult,
                tr("The GUI model was changed, but its audit record could not be completed: %1").arg(auditError));
        }
        operationResult.insert(QStringLiteral("outcome"), QStringLiteral("accepted_and_audited"));
        return finishAccepted(operationResult);
    }

    return invalid(QStringLiteral("UNSUPPORTED_OPERATION"), tr("This range.host/v1 operation is not implemented by the GUI host."));
}

bool ModelCommandHost::approvePreview(const QString &previewId)
{
    if (QThread::currentThread() != thread() || activeModalInteraction()) return false;
    auto found = state->previews.find(previewId);
    if (found == state->previews.end() || found->second.approved || found->second.consumed
        || found->second.sessionEpoch != state->sessionEpoch) return false;
    if (found->second.command == "experiment.run")
    {
        auto &preview = found->second;
        const auto source = experimentSource(session, state->sessionEpoch, preview.documentId);
        if (!source.isValid() || source.toJson() != preview.experimentSourceBinding) return false;
        const auto committed = state->experiments->authorizePreview(
            preview.experimentAuthorizationRequest, source, preview.experimentId, preview.experimentAuthorization);
        if (!committed.ok
            || committed.data.value("authorization").toObject() != preview.experimentAuthorization
            || committed.data.value("state") != QJsonValue("prepared")) return false;
        preview.approved = true;
        return true;
    }
    if (found->second.command == "mesh.generate" || found->second.command == "mesh.accept" || found->second.command == "mesh.cancel")
    {
        if (isMutationBlocked()) return false;
        const auto &preview = found->second;
        if (preview.command != "mesh.cancel")
        {
            const uint id = session->findModelByCommandIdentity(preview.documentId);
            if (id >= session->getNModels() || session->modelCommandStamp(id).second != preview.stampGeneration) return false;
        }
        if (preview.command != "mesh.generate")
        {
            bool owned = false;
            for (const auto &value : state->meshController->jobs())
            {
                const auto job = value.toObject();
                const auto source = job.value("sourceRuntimeStamp").toObject();
                if (job.value("attemptId") == QJsonValue(preview.jobId)
                    && source == QJsonObject{{"documentId", preview.documentId}, {"sessionEpoch", preview.sessionEpoch},
                                             {"documentRevision", double(preview.stampGeneration)}})
                    owned = preview.command != "mesh.cancel" || job.value("status") == QJsonValue("queued")
                        || job.value("status") == QJsonValue("running");
            }
            if (!owned) return false;
        }
        found->second.approved = true;
        return true;
    }
    if (found->second.command == "analysis.recover")
    {
        if (isMutationBlocked() || SolverManager::getInstance().nativeJobRecoveryQuery(found->second.jobId)
                != found->second.recoveryQualification) return false;
        found->second.approved = true;
        return true;
    }
    if (found->second.command == QStringLiteral("analysis.cancel"))
    {
        const QJsonObject job = SolverManager::getInstance().jobQuery(found->second.jobId);
        const QString jobState = job.value(QStringLiteral("state")).toString();
        const QJsonObject source = job.value(QStringLiteral("sourceRuntimeStamp")).toObject();
        const auto owned = state->jobSources.find(found->second.jobId);
        if (!job.value(QStringLiteral("found")).toBool()
            || (jobState != QStringLiteral("queued") && jobState != QStringLiteral("running")
                && jobState != QStringLiteral("cancelling"))
            || source.value(QStringLiteral("documentId")).toString() != found->second.jobSourceDocumentId
            || source.value(QStringLiteral("revision")).toString() != found->second.jobSourceRevision
            || (!source.value(QStringLiteral("sessionEpoch")).toString().isEmpty()
                && source.value(QStringLiteral("sessionEpoch")).toString() != found->second.sessionEpoch)
            || owned == state->jobSources.end()
            || owned->second.documentId != found->second.jobSourceDocumentId
            || owned->second.sessionEpoch != found->second.sessionEpoch
            || owned->second.revision != found->second.jobSourceRevision)
            return false;
        found->second.approved = true;
        return true;
    }
    if (found->second.command == QStringLiteral("analysis.run"))
    {
        if (isMutationBlocked()) return false;
        const QString sourceDocumentId = found->second.documentId;
        const QString sourceEpoch = found->second.sessionEpoch;
        const quint64 expectedRevision = found->second.stampGeneration;
        const bool warningsAccepted = found->second.arguments.value(QStringLiteral("warningsAccepted")).toBool();
        uint modelID = session->findModelByCommandIdentity(sourceDocumentId);
        if (modelID == RConstants::eod) return false;
        QPair<QString, quint64> sourceStamp;
        QJsonObject approvedProject;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction()) return false;
            modelID = session->findModelByCommandIdentity(sourceDocumentId);
            if (modelID == RConstants::eod || !session->canMutateDocument(modelID)) return false;
            sourceStamp = session->modelCommandStamp(modelID);
            if (sourceStamp.first != sourceDocumentId || sourceStamp.second != expectedRevision)
                return false;
            approvedProject = found->second.projectSnapshotRef;
            const QJsonObject currentProject = session->documentProjectContext(modelID);
            for (const QString &field : {QStringLiteral("projectId"),
                                         QStringLiteral("persistentDocumentId"),
                                         QStringLiteral("sourceRevision")})
                if (approvedProject.value(field) != currentProject.value(field)) return false;
            if (!approvedProject.value(QStringLiteral("modelSha256")).toString().isEmpty()
                && approvedProject.value(QStringLiteral("modelSha256"))
                    != currentProject.value(QStringLiteral("modelSha256")))
                return false;
        }

        // validateRun also takes the Session mutex. It must run after the
        // host-owned read lock above has been released.
        const QJsonObject validation = SolverManager::getInstance().validateRun(modelID);
        if (!validation.value(QStringLiteral("valid")).toBool()
            || !validation.value(QStringLiteral("errors")).toArray().isEmpty()
            || validation.value(QStringLiteral("sourceStamp")).toObject()
                != sourceStampJson(sourceStamp, sourceEpoch))
            return false;
        if (!validation.value(QStringLiteral("warnings")).toArray().isEmpty()
            && !warningsAccepted)
            return false;

        // Recheck all live GUI gates after validation so that an intervening
        // edit, draft, project change, or legacy task invalidates approval.
        if (isMutationBlocked() || activeModalInteraction()) return false;
        {
            TrySessionLock guard(session);
            if (!guard.ownsLock() || isMutationBlocked() || activeModalInteraction()) return false;
            modelID = session->findModelByCommandIdentity(sourceDocumentId);
            if (modelID == RConstants::eod || !session->canMutateDocument(modelID)) return false;
            if (session->modelCommandStamp(modelID) != sourceStamp) return false;
            const QJsonObject currentProject = session->documentProjectContext(modelID);
            for (const QString &field : {QStringLiteral("projectId"),
                                         QStringLiteral("persistentDocumentId"),
                                         QStringLiteral("sourceRevision")})
                if (approvedProject.value(field) != currentProject.value(field)) return false;
            if (!approvedProject.value(QStringLiteral("modelSha256")).toString().isEmpty()
                && approvedProject.value(QStringLiteral("modelSha256"))
                    != currentProject.value(QStringLiteral("modelSha256")))
                return false;
        }
        found = state->previews.find(previewId);
        if (found == state->previews.end() || found->second.approved || found->second.consumed
            || found->second.sessionEpoch != state->sessionEpoch || found->second.sessionEpoch != sourceEpoch
            || found->second.documentId != sourceDocumentId
            || found->second.stampGeneration != sourceStamp.second)
            return false;
        found->second.approved = true;
        return true;
    }
    if (isMutationBlocked()) return false;
    uint modelID = session->findModelByCommandIdentity(found->second.documentId);
    if (modelID == RConstants::eod) return false;
    TrySessionLock guard(session);
    if (!guard.ownsLock() || isMutationBlocked()) return false;
    modelID = session->findModelByCommandIdentity(found->second.documentId);
    if (modelID == RConstants::eod) return false;
    if (!session->canMutateDocument(modelID)) return false;
    const auto stamp = session->modelCommandStamp(modelID);
    if (stamp.first != found->second.documentId || stamp.second != found->second.stampGeneration) return false;
    if (isCoreEditCommand(found->second.command))
    {
        const auto entry = state->entries.find(found->second.documentId);
        if (entry == state->entries.end() || !entry->second.editing
            || entry->second.generation != stamp.second
            || entry->second.editing->query().value(QStringLiteral("revision")).toString() != found->second.nativeRevision)
            return false;
    }
    else
    {
        const auto &model = session->getModel(modelID);
        const bool undo = found->second.command == QStringLiteral("document.undo");
        const uint depth = undo ? model.getUndoStackSize() : model.getRedoStackSize();
        const QString action = undo ? model.getUndoActionMessage() : model.getRedoActionMessage();
        if (depth == 0 || action != found->second.nativePreview.value(QStringLiteral("historyAction")).toString()) return false;
    }
    found->second.approved = true;
    return true;
}

bool ModelCommandHost::configureModal(const QList<uint> &modelIDs, const RModalSetup &setup, QString *error)
{
    auto fail = [&](const QString &message) { if (error) *error = message; emit commandRejected(message); return false; };
    if (error) error->clear();
    if (QThread::currentThread() != thread()) return fail(tr("Model commands require the GUI thread."));
    if (isMutationBlocked() || activeModalInteraction()) return fail(tr("Wait until document interaction and live model tasks finish before changing modal settings."));
    if (modelIDs.isEmpty()) return fail(tr("No model is selected."));
    QList<RModelCommandService::Delta> deltas;
    QList<QPair<QString, quint64>> stamps;
    QList<State::Entry *> entries;
    QList<QJsonObject> nativeRequests;
    QList<QJsonObject> acceptedCommands;
    const QJsonObject arguments = RModelCommandService::modalParameters(setup);
    {
        QMutexLocker guard(session->getMutex());
        QSet<uint> unique;
        // Validate all current live models and their journal sessions first.
        for (const uint id : modelIDs)
        {
            if (unique.contains(id)) return fail(tr("A model was selected more than once."));
            unique.insert(id);
            QString projectFailure;
            if (!session->canMutateDocument(id, &projectFailure)) return fail(projectFailure);
            auto *entry = state->ensure(session, id);
            if (!entry) return fail(state->failure);
            if (!entry->editing->setRunning(false)) return fail(tr("Cannot record model availability."));
            const auto stamp = session->modelCommandStamp(id);
            const auto candidate = RModelCommandService::preview(session->getModel(id),
                QStringLiteral("analysis.modal.configure"), arguments, stamp.first,
                QString::number(stamp.second));
            if (!candidate.valid) return fail(candidate.message);
            const QJsonObject query = entry->editing->query();
            if (RModelCommandService::stateFingerprint(entry->editing->model())
                != RModelCommandService::stateFingerprint(session->getModel(id)))
                return fail(tr("The command journal snapshot does not match the live model."));
            nativeRequests.append(QJsonObject{
                {QStringLiteral("apiVersion"), QStringLiteral("range.application/v1")},
                {QStringLiteral("commandId"), QStringLiteral("analysis.modal.configure")},
                {QStringLiteral("commandVersion"), 1},
                {QStringLiteral("modelId"), query.value(QStringLiteral("modelId"))},
                {QStringLiteral("expectedRevision"), query.value(QStringLiteral("revision"))},
                {QStringLiteral("requestId"), uuid()},
                {QStringLiteral("idempotencyKey"), uuid()},
                {QStringLiteral("parameters"), candidate.normalizedArguments},
                {QStringLiteral("dryRun"), false}});
            deltas.append(candidate.delta);
            stamps.append(stamp);
            entries.append(entry);
        }

        // Journal every candidate before publishing any GUI model. If a later
        // journal append fails, close/reconcile the earlier shadow sessions;
        // no live model has changed yet.
        for (qsizetype i = 0; i < modelIDs.size(); ++i)
        {
            if (session->modelCommandStamp(modelIDs[i]) != stamps[i])
                return fail(tr("A model changed while modal candidates were being validated."));
            if (entries[i]->editing->query().value(QStringLiteral("revision")).toString()
                != nativeRequests[i].value(QStringLiteral("expectedRevision")).toString())
                return fail(tr("A model command journal changed while modal candidates were being validated."));
            const QJsonObject accepted = entries[i]->editing->execute(nativeRequests[i]);
            if (accepted.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("accepted")))
            {
                QStringList reconciliationErrors;
                for (qsizetype previous = 0; previous < i; ++previous)
                {
                    QString reconciliationError;
                    if (!entries[previous]->editing->recordExternalChange(
                            QStringLiteral("external_edit"), session->getModel(modelIDs[previous]),
                            &reconciliationError))
                        reconciliationErrors.append(reconciliationError);
                    entries[previous]->editing.reset();
                }
                const QString message = tr("The modal command journal rejected the operation; no GUI model was changed. %1")
                    .arg(accepted.value(QStringLiteral("message")).toString());
                if (!reconciliationErrors.isEmpty())
                    return fail(message + tr(" Earlier journal candidates were reconciled with errors: %1")
                        .arg(reconciliationErrors.join(QStringLiteral("; "))));
                return fail(message);
            }
            acceptedCommands.append(accepted);
        }

        for (qsizetype i = 0; i < modelIDs.size(); ++i)
        {
            if (session->modelCommandStamp(modelIDs[i]) != stamps[i])
            {
                QStringList reconciliationErrors;
                for (qsizetype prior = 0; prior < entries.size(); ++prior)
                {
                    QString reconciliationError;
                    if (!entries[prior]->editing->recordExternalChange(
                            QStringLiteral("external_edit"), session->getModel(modelIDs[prior]),
                            &reconciliationError))
                        reconciliationErrors.append(reconciliationError);
                    entries[prior]->editing.reset();
                }
                return fail(tr("A model changed after modal commands were journaled; no GUI model was changed. %1")
                    .arg(reconciliationErrors.join(QStringLiteral("; "))));
            }
            QString applyError;
            session->storeCurentModelVersion(modelIDs[i], tr("Configure modal analysis"));
            if (!commitSupportedDelta(session->getModel(modelIDs[i]), deltas[i], &applyError))
            {
                QStringList reconciliationErrors;
                for (qsizetype prior = 0; prior < entries.size(); ++prior)
                {
                    QString reconciliationError;
                    if (!entries[prior]->editing->recordExternalChange(
                            QStringLiteral("external_edit"), session->getModel(modelIDs[prior]),
                            &reconciliationError))
                        reconciliationErrors.append(reconciliationError);
                    entries[prior]->editing.reset();
                }
                return fail(tr("A journaled modal command could not be published to the GUI model: %1 %2")
                    .arg(applyError, reconciliationErrors.join(QStringLiteral("; "))));
            }
        }
    }
    for (qsizetype i = 0; i < modelIDs.size(); ++i)
    {
        session->setProblemChanged(modelIDs[i]);
        const QPair<QString, quint64> stamp = session->modelCommandStamp(modelIDs[i]);
        const QJsonObject accepted = acceptedCommands[i];
        if (!entries[i]->editing
            || entries[i]->editing->query().value(QStringLiteral("revision")).toString()
                != accepted.value(QStringLiteral("revision")).toString()
            || RModelCommandService::stateFingerprint(entries[i]->editing->model())
                != RModelCommandService::stateFingerprint(session->getModel(modelIDs[i])))
        {
            QString auditError;
            const bool reconciled = recordExternalChangeAfterNotification(
                modelIDs[i], QStringLiteral("external_edit"), &auditError);
            if (!reconciled)
            {
                const QString message = tr("Modal settings were applied, but their durable command state could not be synchronized: %1")
                    .arg(auditError);
                if (error) *error = message;
                emit commandRejected(message);
                return false;
            }
            continue;
        }
        entries[i]->generation = stamp.second;
    }
    return true;
}

bool ModelCommandHost::recordArtifactPublished(uint modelID, const QString &path, QString *error,
                                              const RProjectCatalog::PublicationLeasePtr &publicationLease)
{
    if (QThread::currentThread() != thread()) { if (error) *error = tr("Publication requires the GUI thread."); return false; }
    QMutexLocker guard(session->getMutex());
    const QString artifact = modelArtifact(path, error);
    if (artifact.isEmpty()) return false;
    const auto stamp = session->modelCommandStamp(modelID);
    auto found = state->entries.find(stamp.first);
    State::Entry *entry = nullptr;
    // A save advances the GUI stamp once, but the saved bytes still belong to
    // the accepted editing revision. Preserve that association in the journal.
    if (found != state->entries.end() && found->second.editing && found->second.generation + 1 == stamp.second)
        entry = &found->second;
    else entry = state->ensure(session, modelID);
    if (!entry) { if (error) *error = state->failure; return false; }
    if (!entry->editing->recordArtifactPublished(artifact, nullptr, error)) return false;
    const QString visiblePath = QFileInfo(path).canonicalFilePath();
    if (visiblePath != artifact && !entry->editing->recordArtifactPublished(visiblePath, nullptr, error)) return false;
    const bool closed = closeEditingDurably(stamp.first, error, publicationLease);
    if (closed) entry->editing.reset();
    if (!closed && error) *error = tr("The model was saved and journaled, but its close bookkeeping remains pending: %1").arg(*error);
    return closed;
}

bool ModelCommandHost::recordExternalChange(uint modelID, const QString &reason, QString *error)
{
    if (QThread::currentThread() != thread()) { if (error) *error = tr("External changes require the GUI thread."); return false; }
    QString failure;
    bool recorded = true;
    {
        QMutexLocker guard(session->getMutex());
        session->invalidateModelCommands(modelID);
        const auto stamp = session->modelCommandStamp(modelID);
        auto found = state->entries.find(stamp.first);
        if (found != state->entries.end() && found->second.editing)
        {
            recorded = found->second.editing->recordExternalChange(reason, session->getModel(modelID), &failure);
            found->second.editing.reset();
        }
    }
    if (!recorded)
    {
        const QString message = tr("The model changed, but its audit record could not be saved: %1").arg(failure);
        if (error) *error = message;
        RLogger::error("%s\n", message.toUtf8().constData());
        emit commandRejected(message);
    }
    return recorded;
}

bool ModelCommandHost::recordExternalChangeAfterNotification(uint modelID, const QString &reason,
                                                              QString *error)
{
    if (QThread::currentThread() != thread())
    {
        if (error) *error = tr("External changes require the GUI thread.");
        return false;
    }
    QString failure;
    bool recorded = true;
    {
        QMutexLocker guard(session->getMutex());
        const auto stamp = session->modelCommandStamp(modelID);
        auto found = state->entries.find(stamp.first);
        if (found == state->entries.end() || !found->second.editing)
        {
            recorded = false;
            failure = tr("No open command session was available to journal the published model change.");
        }
        else
        {
            recorded = found->second.editing->recordExternalChange(reason, session->getModel(modelID), &failure);
            found->second.editing.reset();
        }
    }
    if (!recorded)
    {
        const QString message = tr("The model changed, but its audit record could not be saved: %1").arg(failure);
        if (error) *error = message;
        RLogger::error("%s\n", message.toUtf8().constData());
        emit commandRejected(message);
    }
    return recorded;
}
