#include "rnative_result_broker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <rml_model.h>

namespace
{

constexpr qint64 MaxManifestBytes = 16 * 1024 * 1024;
constexpr qint64 MaxFrameBytes = 256 * 1024 * 1024;
constexpr int MaxFrames = 10000;
constexpr uint MaxNodes = 100000;
constexpr int MaxPageSize = 200;
const QString ManifestName = QStringLiteral("native-result.manifest.json");
const QString ManifestApiVersion = QStringLiteral("range.native-results/v1");
const QString RequiredProofScope = QStringLiteral("solver-and-declared-controller-runtime/v1");

struct BrokerFailure
{
    QString code;
    QString message;
};

[[noreturn]] void fail(const QString &code, const QString &message)
{
    throw BrokerFailure{code, message};
}

QByteArray compactArray(const QJsonArray &array)
{
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QString digestBytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool isSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value)
    {
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
              (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
        {
            return false;
        }
    }
    return true;
}

QString hashFile(const QString &path, qint64 maximumBytes, const QString &code)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(code, QStringLiteral("Could not read a published result file."));
    if (file.size() < 0 || file.size() > maximumBytes)
        fail(QStringLiteral("UNSUPPORTED_SIZE_LIMIT"),
             QStringLiteral("A result file exceeds the broker's explicit size limit."));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 totalBytes = 0;
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError)
            fail(code, QStringLiteral("Could not read a published result file."));
        totalBytes += chunk.size();
        if (totalBytes > maximumBytes)
            fail(QStringLiteral("UNSUPPORTED_SIZE_LIMIT"),
                 QStringLiteral("A result file grew beyond the broker's explicit size limit while being read."));
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString canonicalDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.exists() || !info.isDir())
        fail(QStringLiteral("INVALID_RESULT_DIRECTORY"),
             QStringLiteral("The result directory must be an existing non-symbolic-link directory."));
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        fail(QStringLiteral("INVALID_RESULT_DIRECTORY"),
             QStringLiteral("The result directory has no canonical path."));
    return QDir::cleanPath(canonical);
}

QString safeChildPath(const QString &canonicalRoot,
                      const QString &path,
                      const QString &code,
                      bool mustExist = true)
{
    const QFileInfo info(path);
    if (info.isSymLink())
        fail(code, QStringLiteral("A result artifact cannot be a symbolic link."));
    if (mustExist && (!info.exists() || !info.isFile()))
        fail(code, QStringLiteral("A result artifact is missing or is not a regular file."));

    const QString parent = QFileInfo(info.absoluteFilePath()).absoluteDir().canonicalPath();
    if (parent.isEmpty() || QDir::cleanPath(parent) != canonicalRoot)
        fail(code, QStringLiteral("A result artifact must be a direct child of the canonical result directory."));
    return QDir(canonicalRoot).filePath(info.fileName());
}

bool isBinaryModelPath(const QString &path)
{
    const QString extension = QFileInfo(path).suffix().toLower();
    if (extension == QLatin1String("rbm")) return true;
    if (extension == QLatin1String("ram")) return false;
    fail(QStringLiteral("UNSUPPORTED_NATIVE_FILE"),
         QStringLiteral("Native result artifacts must use .ram or .rbm."));
}

RFileHeader readHeader(const QString &path)
{
    const bool binary = isBinaryModelPath(path);
    if (binary)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            fail(QStringLiteral("INVALID_NATIVE_FILE"), QStringLiteral("A native result artifact could not be opened."));
        const QByteArray fixedHeader = file.read(24);
        if (fixedHeader.size() != 24)
            fail(QStringLiteral("PARTIAL_NATIVE_FILE"), QStringLiteral("A native result artifact has an incomplete header."));
        const auto nativeInt = [&fixedHeader](int offset)
        {
            qint32 value = 0;
            std::memcpy(&value, fixedHeader.constData() + offset, sizeof(value));
            return value;
        };
        const qint32 major = nativeInt(0);
        const qint32 minor = nativeInt(4);
        const qint32 release = nativeInt(8);
        const qint32 fileType = nativeInt(16);
        const qint32 informationBytes = nativeInt(20);
        if (major < 0 || minor < 0 || release < 0 || fileType < R_FILE_TYPE_NONE ||
            fileType >= R_FILE_N_TYPES || informationBytes < 0 || informationBytes > 4096 ||
            qint64(informationBytes) > file.size() - 24)
            fail(QStringLiteral("PARTIAL_NATIVE_FILE"), QStringLiteral("A native result artifact has an invalid bounded header."));
        const QByteArray information = file.read(informationBytes);
        if (information.size() != informationBytes)
            fail(QStringLiteral("PARTIAL_NATIVE_FILE"), QStringLiteral("A native result artifact has an incomplete header."));
        return RFileHeader(RFileType(fileType), RVersion(uint(major), uint(minor), uint(release)),
                           QString::fromUtf8(information));
    }

    RFile file(path, binary ? RFile::BINARY : RFile::ASCII);
    const QIODevice::OpenMode mode = binary ? QIODevice::ReadOnly
                                            : QIODevice::ReadOnly | QIODevice::Text;
    if (!file.open(mode))
        fail(QStringLiteral("INVALID_NATIVE_FILE"), QStringLiteral("A native result artifact could not be opened."));
    RFileHeader header;
    try
    {
        if (binary) RFileIO::readBinary(file, header);
        else RFileIO::readAscii(file, header);
    }
    catch (...)
    {
        fail(QStringLiteral("PARTIAL_NATIVE_FILE"), QStringLiteral("A native result artifact has an incomplete header."));
    }
    return header;
}

QString resolveLinkTarget(const QString &canonicalRoot,
                          const QString &logicalPath,
                          const RFileHeader &header)
{
    if (header.getType() != R_FILE_TYPE_LINK)
        fail(QStringLiteral("LATEST_ARTIFACT_NOT_LINK"),
             QStringLiteral("The latest artifact must be the native logical LINK file."));
    const QString information = header.getInformation().trimmed();
    if (information.isEmpty())
        fail(QStringLiteral("INVALID_RESULT_LINK"), QStringLiteral("The latest LINK has no target."));
    QString targetName = information;
    if (targetName.size() >= 2 && targetName.front() == QLatin1Char('"') && targetName.back() == QLatin1Char('"'))
        targetName = targetName.mid(1, targetName.size() - 2);
    if (targetName.isEmpty() || QFileInfo(targetName).isAbsolute())
        fail(QStringLiteral("UNSAFE_RESULT_PATH"),
             QStringLiteral("The latest LINK target must be a relative path inside the result directory."));
    const QString resolved = QDir(QFileInfo(logicalPath).absolutePath()).filePath(targetName);
    return safeChildPath(canonicalRoot, resolved, QStringLiteral("UNSAFE_RESULT_PATH"));
}

struct Provenance
{
    QJsonObject json;
    QString jobId;
    QString inputPackageSha256;
    QString mode;
    QString sourceKind;
    bool controlEnabled = false;
    QString controllerId;
};

Provenance validateProvenance(const QJsonObject &value)
{
    const QString jobId = value.value(QStringLiteral("jobId")).toString();
    const QString inputSnapshotSha256 = value.value(QStringLiteral("inputSnapshotSha256")).toString();
    const QString inputPackageSha256 = value.value(QStringLiteral("inputPackageSha256")).toString();
    const QString proofScope = value.value(QStringLiteral("proofScope")).toString();
    const QJsonObject sourceRuntimeStamp = value.value(QStringLiteral("sourceRuntimeStamp")).toObject();
    const QJsonObject sourceSnapshotRef = value.value(QStringLiteral("sourceSnapshotRef")).toObject();
    const QJsonObject runtime = value.value(QStringLiteral("runtime")).toObject();
    const QJsonObject control = value.value(QStringLiteral("control")).toObject();
    const bool controlEnabled = control.value(QStringLiteral("enabled")).toBool(false);
    const QString controllerId = control.value(QStringLiteral("controllerId")).toString();
    const QString controllerVersionSha256 = control.value(QStringLiteral("controllerVersionSha256")).toString();

    if (jobId.isEmpty() || jobId.size() > 256 || jobId.trimmed() != jobId ||
        sourceRuntimeStamp.isEmpty() || sourceSnapshotRef.isEmpty() ||
        !isSha256(inputSnapshotSha256) || !isSha256(inputPackageSha256) ||
        proofScope != RequiredProofScope ||
        !isSha256(runtime.value(QStringLiteral("fingerprintSha256")).toString()) ||
        runtime.value(QStringLiteral("scope")).toString().isEmpty() ||
        control.isEmpty() || !control.value(QStringLiteral("enabled")).isBool() ||
        !control.value(QStringLiteral("controllerId")).isString() ||
        !control.value(QStringLiteral("controllerVersionSha256")).isString())
    {
        fail(QStringLiteral("INVALID_PROVENANCE"),
             QStringLiteral("Required source, input, runtime, control, or proof-scope provenance is missing or invalid."));
    }
    if (controlEnabled)
    {
        if (controllerId.isEmpty() || controllerId.size() > 128 ||
            !isSha256(controllerVersionSha256))
            fail(QStringLiteral("INVALID_PROVENANCE"),
                 QStringLiteral("Enabled control requires a controller ID and version fingerprint."));
    }
    else if (!controllerId.isEmpty() || !controllerVersionSha256.isEmpty())
    {
        fail(QStringLiteral("INVALID_PROVENANCE"),
             QStringLiteral("Disabled control must not claim a controller identity."));
    }

    const QString mode = controlEnabled ? QStringLiteral("active") : QStringLiteral("passive");
    return Provenance{value, jobId, inputPackageSha256, mode,
                      controlEnabled ? QStringLiteral("native-active-fe")
                                     : QStringLiteral("native-passive-fe"),
                      controlEnabled, controllerId};
}

QJsonArray idsToJson(const QVector<uint> &ids)
{
    QJsonArray array;
    for (uint id : ids) array.append(double(id));
    return array;
}

QString physicalMapSha256(const QJsonArray &ids)
{
    return digestBytes(compactArray(ids));
}

struct FieldDefinition
{
    RVariableType type;
    const char *id;
    const char *quantity;
    bool angular;
};

const std::vector<FieldDefinition> &fieldDefinitions()
{
    static const std::vector<FieldDefinition> values{
        {R_VARIABLE_DISPLACEMENT, "var-displacement", "displacement", false},
        {R_VARIABLE_VELOCITY, "var-velocity", "velocity", false},
        {R_VARIABLE_ACCELERATION, "var-acceleration", "acceleration", false},
        {R_VARIABLE_ROTATION, "var-rotation", "rotation", true},
        {R_VARIABLE_ANGULAR_VELOCITY, "var-angular_velocity", "angularVelocity", true},
        {R_VARIABLE_ANGULAR_ACCELERATION, "var-angular_acceleration", "angularAcceleration", true}
    };
    return values;
}

bool hasNativeBeam(const RModel &model)
{
    for (uint index = 0; index < model.getNElements(); ++index)
    {
        const RElementType type = model.getElement(index).getType();
        if (type == R_ELEMENT_BEAM1 || type == R_ELEMENT_BEAM2) return true;
    }
    return false;
}

QVector<uint> physicalNodeIds(const RModel &model)
{
    if (model.getNNodes() == 0 || model.getNNodes() > MaxNodes)
        fail(QStringLiteral("UNSUPPORTED_NODE_LIMIT"),
             QStringLiteral("A result model must contain between 1 and 100000 nodes."));

    QVector<uint> ids;
    ids.reserve(int(model.getNNodes()));
    QSet<uint> seen;
    for (uint position = 0; position < model.getNNodes(); ++position)
    {
        const uint id = model.getNode(position).getPhysicalId();
        if (id == 0 || seen.contains(id))
            fail(QStringLiteral("INVALID_NODE_MAPPING"),
                 QStringLiteral("Result node physical identifiers must be nonzero and unique."));
        ids.append(id);
        seen.insert(id);
    }
    return ids;
}

QSet<QString> inspectSupportedVariables(const RModel &model, bool beamModel)
{
    QSet<QString> valid;
    const uint nodeCount = model.getNNodes();
    QHash<int, int> counts;
    for (uint index = 0; index < model.getNVariables(); ++index)
        ++counts[int(model.getVariable(index).getType())];

    for (const FieldDefinition &field : fieldDefinitions())
    {
        if (field.angular && !beamModel) continue;
        const uint position = model.findVariable(field.type);
        const QString id = QString::fromLatin1(field.id);
        if (position == RConstants::eod)
            continue;
        if (counts.value(int(field.type)) != 1)
            fail(QStringLiteral("INVALID_RESULT_FIELD"),
                 QStringLiteral("A supported nodal field is duplicated in a native MODEL."));

        const RVariable &variable = model.getVariable(position);
        const QString enumUnits = RVariable::getUnits(field.type);
        if (variable.getApplyType() != R_VARIABLE_APPLY_NODE ||
            variable.getNVectors() != 3 || variable.getNValues() != nodeCount ||
            enumUnits.isEmpty() || variable.getUnits() != enumUnits)
        {
            fail(QStringLiteral("INVALID_RESULT_FIELD"),
                 QStringLiteral("A supported nodal field has invalid dimensions, apply type, or enum units."));
        }
        for (uint component = 0; component < 3; ++component)
        {
            for (uint node = 0; node < nodeCount; ++node)
            {
                if (!std::isfinite(variable.getValue(component, node)))
                    fail(QStringLiteral("NONFINITE_RESULT_FIELD"),
                         QStringLiteral("A supported nodal field contains a non-finite value."));
            }
        }
        valid.insert(id);
    }

    return valid;
}

bool closeTime(double a, double b)
{
    return std::fabs(a - b) <= 1.0e-10 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

struct ModelInfo
{
    QString analysisType;
    int modeIndex = -1;
    int recordNo = -1;
    double timeS = 0.0;
    QString sha256;
    QString meshFingerprint;
    QVector<uint> nodeIds;
    QString nodeMapSha256;
    QSet<QString> variables;
    bool beamModel = false;
};

ModelInfo inspectModel(const QString &path,
                       int expectedRecordNo,
                       const Provenance &provenance,
                       bool verifyPersistedFrame,
                       RModel *parsedModel = nullptr,
                       const QString &expectedFileSha = QString())
{
    const QFileInfo fileInfo(path);
    if (fileInfo.size() <= 0 || fileInfo.size() > MaxFrameBytes)
        fail(QStringLiteral("UNSUPPORTED_FRAME_SIZE"),
             QStringLiteral("A native MODEL frame exceeds the 256 MiB broker limit."));
    const QString beforeSha = hashFile(path, MaxFrameBytes, QStringLiteral("INVALID_NATIVE_FILE"));
    if (!expectedFileSha.isEmpty() && beforeSha != expectedFileSha)
        fail(QStringLiteral("RESULT_FRAME_TAMPERED"), QStringLiteral("A MODEL frame no longer matches its published SHA-256."));
    const RFileHeader header = readHeader(path);
    if (header.getType() != R_FILE_TYPE_MODEL)
        fail(QStringLiteral("NUMBERED_RECORD_NOT_MODEL"),
             QStringLiteral("Every numbered result record must be a raw MODEL, never a LINK."));

    RModel temporaryModel;
    RModel &model = parsedModel ? *parsedModel : temporaryModel;
    try
    {
        model.read(path);
    }
    catch (...)
    {
        fail(QStringLiteral("PARTIAL_NATIVE_FILE"),
             QStringLiteral("A numbered native MODEL is partial or malformed."));
    }
    if (hashFile(path, MaxFrameBytes, QStringLiteral("INVALID_NATIVE_FILE")) != beforeSha)
        fail(QStringLiteral("RESULT_CHANGED_DURING_READ"),
             QStringLiteral("A native MODEL changed while the broker was reading it."));

    const RProblemTypeMask taskTypes = model.getProblemTaskTree().getProblemTypeMask();
    const RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    const RStructuralControlSetup &control = model.getProblemSetup().getStructuralControlSetup();
    try { control.validate(); }
    catch (...) { fail(QStringLiteral("INVALID_CONTROL_METADATA"),
                       QStringLiteral("The native MODEL has invalid persisted control metadata.")); }
    if (control.enabled != provenance.controlEnabled ||
        (control.enabled && control.controllerId != provenance.controllerId))
    {
        fail(QStringLiteral("CONTROL_PROVENANCE_MISMATCH"),
             QStringLiteral("The MODEL control setup disagrees with caller-declared control provenance."));
    }

    ModelInfo info;
    info.sha256 = beforeSha;
    info.nodeIds = physicalNodeIds(model);
    const QJsonArray nodeIdsJson = idsToJson(info.nodeIds);
    info.nodeMapSha256 = physicalMapSha256(nodeIdsJson);
    info.beamModel = hasNativeBeam(model);

    if (taskTypes == R_PROBLEM_STRESS && dynamics.getEnabled())
    {
        try { dynamics.validate(); }
        catch (...) { fail(QStringLiteral("INVALID_TRANSIENT_METADATA"),
                           QStringLiteral("The native MODEL has invalid structural-dynamics settings.")); }
        if (!model.getTimeSolver().getEnabled() || model.getTimeSolver().getNTimeSteps() == 0 ||
            model.getTimeSolver().getCurrentTimeStep() >= model.getTimeSolver().getNTimeSteps())
            fail(QStringLiteral("INVALID_TRANSIENT_METADATA"),
                 QStringLiteral("A dynamics result must persist an actual accepted time and step."));
        const uint step = model.getTimeSolver().getCurrentTimeStep();
        if (step >= uint(std::numeric_limits<int>::max()) || int(step + 1) != expectedRecordNo)
            fail(QStringLiteral("RECORD_STEP_MISMATCH"),
                 QStringLiteral("A numbered MODEL record does not match its persisted accepted step."));
        info.timeS = model.getTimeSolver().getCurrentTime();
        if (!std::isfinite(info.timeS))
            fail(QStringLiteral("INVALID_TRANSIENT_METADATA"),
                 QStringLiteral("A dynamics frame has no finite persisted simulation time."));

        if (verifyPersistedFrame)
        {
            const RStructuralDynamicsRestartData &checkpoint =
                model.getProblemSetup().getStructuralDynamicsRestartData();
            try { checkpoint.validate(); }
            catch (...) { fail(QStringLiteral("INVALID_ACCEPTED_CHECKPOINT"),
                               QStringLiteral("A dynamics MODEL does not contain a valid persisted accepted checkpoint.")); }
            if (!checkpoint.available || checkpoint.completedSteps != uint(expectedRecordNo) ||
                checkpoint.meshNodeCount != model.getNNodes() ||
                checkpoint.meshElementCount != model.getNElements() ||
                checkpoint.meshFingerprint != RStructuralDynamicsRestartData::computeMeshFingerprint(model) ||
                !closeTime(checkpoint.time, info.timeS) ||
                !closeTime(checkpoint.fixedTimeStep, dynamics.getTimeStep()) ||
                checkpoint.controlAvailable != control.enabled)
            {
                fail(QStringLiteral("INVALID_ACCEPTED_CHECKPOINT"),
                     QStringLiteral("A dynamics MODEL lacks checkpoint metadata for this persisted accepted step."));
            }
            // The accepted checkpoint is the persisted solver time source. The
            // RTimeSolver array is used only as a consistency check above.
            info.timeS = checkpoint.time;
            info.meshFingerprint = checkpoint.meshFingerprint;
        }
        info.analysisType = QStringLiteral("structural-dynamics");
        info.recordNo = expectedRecordNo;
        info.variables = inspectSupportedVariables(model, info.beamModel);
        if (!info.variables.contains(QStringLiteral("var-displacement")))
            fail(QStringLiteral("MISSING_REQUIRED_FIELD"),
                 QStringLiteral("A structural dynamics frame must persist nodal displacement values."));
    }
    else if (taskTypes == R_PROBLEM_STRESS_MODAL && !dynamics.getEnabled())
    {
        const uint modeIndex = model.getProblemSetup().getModalSetup().getMode();
        if (modeIndex >= uint(std::numeric_limits<int>::max()) || int(modeIndex + 1) != expectedRecordNo)
            fail(QStringLiteral("RECORD_STEP_MISMATCH"),
                 QStringLiteral("The modal MODEL record does not match its persisted mode index."));
        info.analysisType = QStringLiteral("modal");
        info.modeIndex = int(modeIndex);
        info.recordNo = expectedRecordNo;
        info.variables = inspectSupportedVariables(model, info.beamModel);
    }
    else if (taskTypes == R_PROBLEM_STRESS && !dynamics.getEnabled() &&
             !model.getTimeSolver().getEnabled())
    {
        if (expectedRecordNo != 0)
            fail(QStringLiteral("RECORD_STEP_MISMATCH"),
                 QStringLiteral("A static MODEL must use native record number zero."));
        info.analysisType = QStringLiteral("static");
        info.recordNo = 0;
        info.variables = inspectSupportedVariables(model, info.beamModel);
    }
    else
    {
        fail(QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"),
             QStringLiteral("Only stress static, stress modal, and persisted stress structural-dynamics results are supported."));
    }
    return info;
}

struct NumberedFile
{
    int recordNo = -1;
    QString path;
    QString relativePath;
};

QVector<NumberedFile> enumerateNumberedModels(const QString &canonicalRoot,
                                             const QString &logicalPath,
                                             const QString &terminalPath)
{
    const QString extension = QFileInfo(logicalPath).suffix().toLower();
    QString logicalBase = RFileUtils::getFileNameWithOutTimeStep(QFileInfo(logicalPath).fileName(), 5);
    static const QRegularExpression directRecord(QStringLiteral("^(.*)-(\\d{5,10})\\.(ram|rbm)$"));
    const QRegularExpressionMatch directMatch = directRecord.match(QFileInfo(logicalPath).fileName());
    if (directMatch.hasMatch()) logicalBase = directMatch.captured(1) + QLatin1Char('.') + directMatch.captured(3);
    const int dot = logicalBase.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot < 0 ? logicalBase : logicalBase.left(dot);
    const QString expression = QLatin1Char('^') + QRegularExpression::escape(stem) +
        QStringLiteral("-(\\d{5,10})\\.") + QRegularExpression::escape(extension) + QLatin1Char('$');
    const QRegularExpression pattern(expression);
    if (!pattern.isValid())
        fail(QStringLiteral("INVALID_RESULT_NAME"), QStringLiteral("The latest artifact name is invalid."));

    QVector<NumberedFile> files;
    const QDir dir(canonicalRoot);
    const QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names)
    {
        const QRegularExpressionMatch match = pattern.match(name);
        if (!match.hasMatch()) continue;
        bool parsed = false;
        const int recordNo = match.captured(1).toInt(&parsed);
        if (!parsed || recordNo < 0 || recordNo > 1000000)
            fail(QStringLiteral("INVALID_RECORD_NAME"), QStringLiteral("A numbered native MODEL filename is invalid."));
        const QString path = safeChildPath(canonicalRoot, dir.filePath(name),
                                           QStringLiteral("UNSAFE_RESULT_PATH"));
        files.append(NumberedFile{recordNo, path, name});
    }
    std::sort(files.begin(), files.end(), [](const NumberedFile &left, const NumberedFile &right)
    {
        return left.recordNo < right.recordNo;
    });
    if (files.isEmpty())
        fail(QStringLiteral("NO_NUMBERED_MODEL_RECORDS"),
             QStringLiteral("No actual numbered MODEL records were found for the latest LINK."));
    if (files.size() > MaxFrames)
        fail(QStringLiteral("UNSUPPORTED_FRAME_LIMIT"),
             QStringLiteral("A result contains more than 10000 numbered MODEL frames."));
    if (files.back().path != terminalPath)
        fail(QStringLiteral("STALE_NUMBERED_RECORD"),
             QStringLiteral("The latest LINK does not target the highest numbered native MODEL record."));
    return files;
}

QString artifactIdFor(const Provenance &provenance)
{
    const QByteArray seed = provenance.jobId.toUtf8() + '\n' + provenance.mode.toUtf8() + '\n' +
                            provenance.inputPackageSha256.toLatin1();
    return QStringLiteral("artifact-") + digestBytes(seed).left(32);
}

QStringList sortedVariableIds(const QSet<QString> &values)
{
    QStringList sorted = values.values();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

struct Channel
{
    QString id;
    QString variableId;
    QString quantity;
    QString axis;
    QString unit;
};

QVector<FieldDefinition> availableChannelFields(const QJsonObject &manifest)
{
    const QString analysisType = manifest.value(QStringLiteral("analysis")).toObject()
                                     .value(QStringLiteral("type")).toString();
    if (analysisType != QLatin1String("structural-dynamics")) return {};

    const QJsonArray available = manifest.value(QStringLiteral("availableVariables")).toArray();
    QVector<FieldDefinition> fields;
    for (const FieldDefinition &field : fieldDefinitions())
    {
        const QString variableId = QString::fromLatin1(field.id);
        if (!available.contains(variableId)) continue;
        fields.append(field);
    }
    return fields;
}

int totalChannelCount(const QJsonObject &manifest)
{
    const qint64 count = qint64(availableChannelFields(manifest).size()) * 3 *
                         manifest.value(QStringLiteral("nodePhysicalIds")).toArray().size();
    if (count > std::numeric_limits<int>::max())
        fail(QStringLiteral("UNSUPPORTED_CHANNEL_LIMIT"), QStringLiteral("The channel catalog exceeds its explicit index limit."));
    return int(count);
}

QVector<Channel> channelPage(const QJsonObject &manifest, int offset, int limit)
{
    const QVector<FieldDefinition> fields = availableChannelFields(manifest);
    const QJsonArray nodeIds = manifest.value(QStringLiteral("nodePhysicalIds")).toArray();
    const QStringList axes{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
    const int nodesPerQuantity = int(nodeIds.size()) * 3;
    QVector<Channel> page;
    page.reserve(limit);
    for (int index = offset; index < std::min(totalChannelCount(manifest), offset + limit); ++index)
    {
        const int fieldIndex = index / nodesPerQuantity;
        const int withinField = index % nodesPerQuantity;
        const int axisIndex = withinField / nodeIds.size();
        const int nodeIndex = withinField % nodeIds.size();
        const FieldDefinition &field = fields.at(fieldIndex);
        const uint nodeId = uint(nodeIds.at(nodeIndex).toDouble());
        const QString quantity = QString::fromLatin1(field.quantity);
        const QString axis = axes.at(axisIndex);
        page.append(Channel{QStringLiteral("node:%1:%2:%3").arg(nodeId).arg(quantity, axis),
                            QString::fromLatin1(field.id), quantity, axis,
                            RVariable::getUnits(field.type)});
    }
    return page;
}

bool parseChannelId(const QString &id, uint *nodeId, QString *quantity, QString *axis)
{
    static const QRegularExpression pattern(QStringLiteral("^node:([1-9][0-9]{0,9}):([A-Za-z]+):(X|Y|Z)$"));
    const QRegularExpressionMatch match = pattern.match(id);
    if (!match.hasMatch()) return false;
    bool parsed = false;
    const qulonglong value = match.captured(1).toULongLong(&parsed);
    if (!parsed || value == 0 || value >= quint64(RConstants::eod)) return false;
    const QString parsedQuantity = match.captured(2);
    const QStringList allowed{QStringLiteral("displacement"), QStringLiteral("velocity"),
                              QStringLiteral("acceleration"), QStringLiteral("rotation"),
                              QStringLiteral("angularVelocity"), QStringLiteral("angularAcceleration")};
    if (!allowed.contains(parsedQuantity)) return false;
    *nodeId = uint(value);
    *quantity = parsedQuantity;
    *axis = match.captured(3);
    return true;
}

QString variableIdForQuantity(const QString &quantity)
{
    for (const FieldDefinition &field : fieldDefinitions())
        if (QString::fromLatin1(field.quantity) == quantity) return QString::fromLatin1(field.id);
    return QString();
}

RVariableType variableTypeForId(const QString &variableId)
{
    for (const FieldDefinition &field : fieldDefinitions())
        if (QString::fromLatin1(field.id) == variableId) return field.type;
    return R_VARIABLE_NONE;
}

QJsonObject framePublic(const QJsonObject &frame)
{
    return QJsonObject{{QStringLiteral("recordNo"), frame.value(QStringLiteral("recordNo"))},
                       {QStringLiteral("timeS"), frame.value(QStringLiteral("timeS"))},
                       {QStringLiteral("relativePath"), frame.value(QStringLiteral("relativePath"))},
                       {QStringLiteral("sha256"), frame.value(QStringLiteral("sha256"))}};
}

QString encodeCursor(const QString &artifactId, int offset)
{
    const QByteArray cursor = artifactId.toUtf8() + '\n' + QByteArray::number(offset);
    return QString::fromLatin1(cursor.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

int pageOffset(const QJsonObject &arguments, const QString &artifactId, int count)
{
    if (arguments.contains(QStringLiteral("cursor")) && arguments.contains(QStringLiteral("offset")))
        fail(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("Specify either offset or cursor, not both."));
    if (arguments.contains(QStringLiteral("cursor")))
    {
        const QString token = arguments.value(QStringLiteral("cursor")).toString();
        const QByteArray decoded = QByteArray::fromBase64(token.toLatin1(),
            QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
        const QList<QByteArray> parts = decoded.split('\n');
        bool parsed = false;
        if (parts.size() != 2 || QString::fromUtf8(parts.at(0)) != artifactId)
            fail(QStringLiteral("INVALID_CURSOR"), QStringLiteral("The cursor does not belong to this artifact."));
        const int offset = parts.at(1).toInt(&parsed);
        if (!parsed || offset < 0 || offset > count)
            fail(QStringLiteral("INVALID_CURSOR"), QStringLiteral("The cursor offset is invalid."));
        return offset;
    }
    if (!arguments.contains(QStringLiteral("offset"))) return 0;
    const QJsonValue offsetValue = arguments.value(QStringLiteral("offset"));
    const double raw = offsetValue.toDouble(-1.0);
    if (!offsetValue.isDouble() || !std::isfinite(raw) || std::floor(raw) != raw || raw < 0.0 || raw > count)
        fail(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("offset must be a bounded nonnegative integer."));
    return int(raw);
}

int pageLimit(const QJsonObject &arguments)
{
    if (!arguments.contains(QStringLiteral("limit"))) return MaxPageSize;
    const QJsonValue limitValue = arguments.value(QStringLiteral("limit"));
    const double raw = limitValue.toDouble(-1.0);
    if (!limitValue.isDouble() || !std::isfinite(raw) || std::floor(raw) != raw || raw < 1.0 || raw > MaxPageSize)
        fail(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("limit must be an integer from 1 through 200."));
    return int(raw);
}

void requireOnlyKeys(const QJsonObject &arguments, const QStringList &keys)
{
    for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it)
        if (!keys.contains(it.key()))
            fail(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("The query contains an unsupported argument."));
}

void validateManifestShape(const QString &root, const QJsonObject &manifest);

QJsonObject loadManifest(const QString &manifestPath,
                         const QString &expectedSha256,
                         QString *canonicalRoot)
{
    if (!isSha256(expectedSha256))
        fail(QStringLiteral("INVALID_EXPECTED_SHA256"), QStringLiteral("expectedSha256 must be a lowercase SHA-256 digest."));
    const QFileInfo info(manifestPath);
    if (info.isSymLink() || QFileInfo(info.absolutePath()).isSymLink() || !info.exists() || !info.isFile())
        fail(QStringLiteral("INVALID_MANIFEST_PATH"), QStringLiteral("The manifest must be a regular non-symbolic-link file."));
    const QString root = info.absoluteDir().canonicalPath();
    if (root.isEmpty())
        fail(QStringLiteral("INVALID_MANIFEST_PATH"), QStringLiteral("The manifest directory has no canonical path."));
    if (info.fileName() != ManifestName)
        fail(QStringLiteral("INVALID_MANIFEST_PATH"), QStringLiteral("The manifest filename is not the published native result manifest."));

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > MaxManifestBytes)
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The manifest is unreadable or exceeds 16 MiB."));
    const QByteArray bytes = file.read(MaxManifestBytes + 1);
    if (bytes.size() > MaxManifestBytes || !file.atEnd())
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The manifest exceeds 16 MiB while being read."));
    if (file.error() != QFile::NoError || digestBytes(bytes) != expectedSha256)
        fail(QStringLiteral("STALE_MANIFEST_SHA256"), QStringLiteral("The manifest bytes do not match expectedSha256."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The hash-bound manifest is not a JSON object."));
    const QJsonObject manifest = document.object();
    if (manifest.value(QStringLiteral("apiVersion")).toString() != ManifestApiVersion ||
        manifest.value(QStringLiteral("validationScope")).toString() != QLatin1String("caller-file-owner"))
        fail(QStringLiteral("UNSUPPORTED_MANIFEST_SCHEMA"), QStringLiteral("The manifest schema or validation scope is unsupported."));

    *canonicalRoot = QDir::cleanPath(root);
    validateManifestShape(*canonicalRoot, manifest);
    return manifest;
}

void validateManifestShape(const QString &root, const QJsonObject &manifest)
{
    const QString artifactId = manifest.value(QStringLiteral("artifactId")).toString();
    const QString jobId = manifest.value(QStringLiteral("jobId")).toString();
    const QString sourceKind = manifest.value(QStringLiteral("sourceKind")).toString();
    const QJsonObject analysis = manifest.value(QStringLiteral("analysis")).toObject();
    const QJsonObject logicalLatest = manifest.value(QStringLiteral("logicalLatest")).toObject();
    const QJsonObject terminalModel = manifest.value(QStringLiteral("terminalModel")).toObject();
    const QJsonArray nodeIds = manifest.value(QStringLiteral("nodePhysicalIds")).toArray();
    const QJsonArray frames = manifest.value(QStringLiteral("frames")).toArray();
    const QJsonArray available = manifest.value(QStringLiteral("availableVariables")).toArray();
    const QJsonObject control = manifest.value(QStringLiteral("control")).toObject();
    const bool controlEnabled = control.value(QStringLiteral("enabled")).toBool(false);
    const QString mode = controlEnabled ? QStringLiteral("active") : QStringLiteral("passive");
    const QByteArray expectedArtifactSeed = jobId.toUtf8() + '\n' + mode.toUtf8() + '\n' +
        manifest.value(QStringLiteral("inputPackageSha256")).toString().toLatin1();
    if (!artifactId.startsWith(QStringLiteral("artifact-")) ||
        artifactId != QStringLiteral("artifact-") + digestBytes(expectedArtifactSeed).left(32) ||
        manifest.value(QStringLiteral("mode")).toString() != mode || jobId.isEmpty() || jobId.size() > 256 ||
        jobId.trimmed() != jobId ||
        manifest.value(QStringLiteral("sourceRuntimeStamp")).toObject().isEmpty() ||
        manifest.value(QStringLiteral("sourceSnapshotRef")).toObject().isEmpty() ||
        (sourceKind != QLatin1String("native-active-fe") && sourceKind != QLatin1String("native-passive-fe")) ||
        !isSha256(manifest.value(QStringLiteral("inputSnapshotSha256")).toString()) ||
        !isSha256(manifest.value(QStringLiteral("inputPackageSha256")).toString()) ||
        !isSha256(manifest.value(QStringLiteral("runtime")).toObject()
                      .value(QStringLiteral("fingerprintSha256")).toString()) ||
        manifest.value(QStringLiteral("runtime")).toObject().value(QStringLiteral("scope")).toString().isEmpty() ||
        manifest.value(QStringLiteral("proofScope")).toString() != RequiredProofScope ||
        control.isEmpty() || !control.value(QStringLiteral("enabled")).isBool() ||
        (controlEnabled && (control.value(QStringLiteral("controllerId")).toString().isEmpty() ||
                            !isSha256(control.value(QStringLiteral("controllerVersionSha256")).toString()))) ||
        (!controlEnabled && (!control.value(QStringLiteral("controllerId")).toString().isEmpty() ||
                             !control.value(QStringLiteral("controllerVersionSha256")).toString().isEmpty())) ||
        (controlEnabled != (sourceKind == QLatin1String("native-active-fe"))) ||
        manifest.value(QStringLiteral("sampleScope")).toString() !=
            (analysis.value(QStringLiteral("type")).toString() == QLatin1String("structural-dynamics")
                ? QStringLiteral("all-persisted-numbered-model-records")
                : QStringLiteral("single-terminal-model")) ||
        !logicalLatest.value(QStringLiteral("relativePath")).isString() ||
        !isSha256(logicalLatest.value(QStringLiteral("sha256")).toString()) ||
        (logicalLatest.value(QStringLiteral("type")).toString() != QLatin1String("LINK") &&
         logicalLatest.value(QStringLiteral("type")).toString() != QLatin1String("MODEL")) ||
        !terminalModel.value(QStringLiteral("relativePath")).isString() ||
        !isSha256(terminalModel.value(QStringLiteral("sha256")).toString()) ||
        nodeIds.isEmpty() || nodeIds.size() > int(MaxNodes) ||
        !isSha256(manifest.value(QStringLiteral("nodePhysicalIdMapSha256")).toString()) ||
        frames.size() > MaxFrames || available.size() > int(fieldDefinitions().size()))
    {
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The manifest is missing required bounded result metadata."));
    }

    QSet<uint> seenIds;
    for (const QJsonValue &value : nodeIds)
    {
        const double raw = value.toDouble(-1.0);
        if (!value.isDouble() || !std::isfinite(raw) || std::floor(raw) != raw || raw < 1.0 ||
            raw >= double(RConstants::eod) || seenIds.contains(uint(raw)))
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The manifest node map is invalid."));
        seenIds.insert(uint(raw));
    }
    if (physicalMapSha256(nodeIds) != manifest.value(QStringLiteral("nodePhysicalIdMapSha256")).toString())
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The manifest node-map digest is invalid."));

    const QString analysisType = analysis.value(QStringLiteral("type")).toString();
    if (analysis.value(QStringLiteral("domain")).toString() != QLatin1String("structural-fe") ||
        (analysisType != QLatin1String("structural-dynamics") && analysisType != QLatin1String("static") &&
         analysisType != QLatin1String("modal")))
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The manifest analysis classification is invalid."));
    if (logicalLatest.value(QStringLiteral("type")).toString() == QLatin1String("MODEL") &&
        analysisType != QLatin1String("static"))
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("Only a static result may use a raw MODEL as logicalLatest."));
    if (analysisType == QLatin1String("structural-dynamics") && frames.isEmpty())
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A dynamics manifest must contain actual MODEL frames."));
    if (analysisType != QLatin1String("structural-dynamics") && !frames.isEmpty())
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A static or modal result cannot be promoted to a time history."));
    if (analysisType == QLatin1String("modal"))
    {
        const double modeIndex = analysis.value(QStringLiteral("modeIndex")).toDouble(-1.0);
        if (!std::isfinite(modeIndex) || std::floor(modeIndex) != modeIndex || modeIndex < 0.0 || modeIndex > 1000.0)
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The modal index is invalid."));
    }
    else if (!analysis.value(QStringLiteral("modeIndex")).isNull())
    {
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A non-modal result cannot claim a mode index."));
    }
    const QJsonValue terminalRecordValue = terminalModel.value(QStringLiteral("recordNo"));
    const double terminalRecordRaw = terminalRecordValue.toDouble(-1.0);
    if (!terminalRecordValue.isDouble() || !std::isfinite(terminalRecordRaw) ||
        std::floor(terminalRecordRaw) != terminalRecordRaw || terminalRecordRaw < 0.0 || terminalRecordRaw > 1000000.0)
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The terminal MODEL record number is invalid."));
    const int terminalRecordNo = int(terminalRecordRaw);
    if ((analysisType == QLatin1String("static") && terminalRecordNo != 0) ||
        (analysisType == QLatin1String("modal") && terminalRecordNo != analysis.value(QStringLiteral("modeIndex")).toInt() + 1))
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The terminal MODEL record does not match its analysis type."));

    const QString latestPath = safeChildPath(root, QDir(root).filePath(logicalLatest.value(QStringLiteral("relativePath")).toString()),
                                             QStringLiteral("UNSAFE_RESULT_PATH"));
    const qint64 latestMaxBytes = logicalLatest.value(QStringLiteral("type")).toString() == QLatin1String("MODEL")
        ? MaxFrameBytes : 1024 * 1024;
    if (hashFile(latestPath, latestMaxBytes, QStringLiteral("RESULT_ARTIFACT_TAMPERED")) !=
        logicalLatest.value(QStringLiteral("sha256")).toString())
        fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("The logical LINK no longer matches its published SHA-256."));
    const RFileHeader linkHeader = readHeader(latestPath);
    QString targetPath;
    if (logicalLatest.value(QStringLiteral("type")).toString() == QLatin1String("LINK"))
    {
        targetPath = resolveLinkTarget(root, latestPath, linkHeader);
        if (linkHeader.getType() != R_FILE_TYPE_LINK)
            fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("The logical artifact is no longer a LINK."));
    }
    else
    {
        if (linkHeader.getType() != R_FILE_TYPE_MODEL)
            fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("The logical artifact is no longer a raw MODEL."));
        targetPath = latestPath;
        if (latestPath != QDir(root).filePath(terminalModel.value(QStringLiteral("relativePath")).toString()))
            fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("A raw logical MODEL must be its terminal MODEL."));
    }
    if (QFileInfo(targetPath).fileName() != terminalModel.value(QStringLiteral("relativePath")).toString())
        fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("The logical LINK no longer targets its published terminal MODEL."));
    if (QFileInfo(targetPath).suffix() != QFileInfo(latestPath).suffix())
        fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("The logical artifact and terminal MODEL extensions disagree."));
    if ((logicalLatest.value(QStringLiteral("type")).toString() == QLatin1String("MODEL") &&
         logicalLatest.value(QStringLiteral("sha256")).toString() !=
             terminalModel.value(QStringLiteral("sha256")).toString()) ||
        (logicalLatest.value(QStringLiteral("type")).toString() == QLatin1String("LINK") &&
         hashFile(targetPath, MaxFrameBytes, QStringLiteral("RESULT_ARTIFACT_TAMPERED")) !=
             terminalModel.value(QStringLiteral("sha256")).toString()))
        fail(QStringLiteral("RESULT_ARTIFACT_TAMPERED"), QStringLiteral("The terminal MODEL no longer matches its published SHA-256."));

    QString resultStem = RFileUtils::getFileNameWithOutTimeStep(
        logicalLatest.value(QStringLiteral("relativePath")).toString(), 5);
    static const QRegularExpression directRecord(QStringLiteral("^(.*)-(\\d{5,10})\\.(ram|rbm)$"));
    const QRegularExpressionMatch directMatch = directRecord.match(
        logicalLatest.value(QStringLiteral("relativePath")).toString());
    if (directMatch.hasMatch())
        resultStem = directMatch.captured(1) + QLatin1Char('.') + directMatch.captured(3);
    const QString resultExtension = QFileInfo(resultStem).suffix();
    const QString baseName = QFileInfo(resultStem).completeBaseName();
    const QString expectedTerminalName = baseName + QLatin1Char('-') +
        QString::number(terminalRecordNo).rightJustified(5, QLatin1Char('0')) + QLatin1Char('.') + resultExtension;
    if (analysisType != QLatin1String("structural-dynamics") &&
        terminalModel.value(QStringLiteral("relativePath")).toString() != expectedTerminalName)
        fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The terminal MODEL filename does not match its persisted record number."));
    int previousRecord = -1;
    double previousTime = -std::numeric_limits<double>::infinity();
    for (const QJsonValue &value : frames)
    {
        if (!value.isObject()) fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A manifest frame entry is invalid."));
        const QJsonObject frame = value.toObject();
        const double recordRaw = frame.value(QStringLiteral("recordNo")).toDouble(-1.0);
        const double timeS = frame.value(QStringLiteral("timeS")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const QString relativePath = frame.value(QStringLiteral("relativePath")).toString();
        const QString sha = frame.value(QStringLiteral("sha256")).toString();
        if (!std::isfinite(recordRaw) || std::floor(recordRaw) != recordRaw || recordRaw < 1.0 || recordRaw > 1000000.0)
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A frame record number is invalid."));
        const int recordNo = int(recordRaw);
        if (recordNo <= previousRecord || !std::isfinite(timeS) || timeS <= previousTime || !isSha256(sha) ||
            frame.value(QStringLiteral("nodePhysicalIdMapSha256")).toString() !=
                manifest.value(QStringLiteral("nodePhysicalIdMapSha256")).toString())
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A frame record, time, SHA, or physical node map is invalid."));
        const QString expectedFrameName = baseName + QLatin1Char('-') +
            QString::number(recordNo).rightJustified(5, QLatin1Char('0')) + QLatin1Char('.') + resultExtension;
        if (!frame.value(QStringLiteral("relativePath")).isString() || relativePath != expectedFrameName)
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A frame path does not follow the native numbered MODEL pattern."));
        const QString framePath = safeChildPath(root, QDir(root).filePath(relativePath),
                                                QStringLiteral("UNSAFE_RESULT_PATH"));
        Q_UNUSED(framePath);
        previousRecord = recordNo;
        previousTime = timeS;
    }
    if (analysisType == QLatin1String("structural-dynamics"))
    {
        if (frames.last().toObject().value(QStringLiteral("recordNo")).toInt() !=
                terminalRecordNo ||
            frames.last().toObject().value(QStringLiteral("sha256")).toString() !=
                terminalModel.value(QStringLiteral("sha256")).toString() ||
            frames.last().toObject().value(QStringLiteral("relativePath")).toString() !=
                terminalModel.value(QStringLiteral("relativePath")).toString())
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The terminal MODEL is not the last listed dynamics frame."));
        if (!available.contains(QStringLiteral("var-displacement")))
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("A dynamics result cannot omit nodal displacement."));
    }

    QSet<QString> seenVariables;
    for (const QJsonValue &value : available)
    {
        if (!value.isString() || seenVariables.contains(value.toString()) ||
            variableTypeForId(value.toString()) == R_VARIABLE_NONE)
            fail(QStringLiteral("INVALID_MANIFEST"), QStringLiteral("The available-variable catalog is invalid."));
        seenVariables.insert(value.toString());
    }
}

struct ParsedQueryChannel
{
    uint nodeId = 0;
    QString quantity;
    QString axis;
    QString variableId;
    RVariableType type = R_VARIABLE_NONE;
    uint component = 0;
    QString unit;
};

ParsedQueryChannel resolveQueryChannel(const QJsonObject &manifest, const QJsonObject &arguments)
{
    const QString id = arguments.value(QStringLiteral("channelId")).toString();
    ParsedQueryChannel channel;
    if (!parseChannelId(id, &channel.nodeId, &channel.quantity, &channel.axis))
        fail(QStringLiteral("INVALID_CHANNEL_ID"), QStringLiteral("channelId must identify a known node, quantity, and global axis."));
    channel.variableId = variableIdForQuantity(channel.quantity);
    channel.type = variableTypeForId(channel.variableId);
    channel.component = channel.axis == QLatin1String("X") ? 0U
                      : channel.axis == QLatin1String("Y") ? 1U : 2U;
    channel.unit = RVariable::getUnits(channel.type);

    const QJsonObject analysis = manifest.value(QStringLiteral("analysis")).toObject();
    if (analysis.value(QStringLiteral("type")).toString() != QLatin1String("structural-dynamics"))
        fail(QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"),
             QStringLiteral("Series and summary are available only for structural-dynamics results."));
    if (!manifest.value(QStringLiteral("availableVariables")).toArray().contains(channel.variableId))
        fail(QStringLiteral("CHANNEL_UNAVAILABLE"), QStringLiteral("The requested native result field is unavailable."));
    const QJsonArray nodeIds = manifest.value(QStringLiteral("nodePhysicalIds")).toArray();
    bool nodeFound = false;
    for (const QJsonValue &value : nodeIds)
        if (uint(value.toDouble()) == channel.nodeId) { nodeFound = true; break; }
    if (!nodeFound)
        fail(QStringLiteral("CHANNEL_UNAVAILABLE"), QStringLiteral("The requested node physical identifier is unavailable."));
    if (arguments.contains(QStringLiteral("nodePhysicalId")))
    {
        const QJsonValue optionalId = arguments.value(QStringLiteral("nodePhysicalId"));
        const double raw = optionalId.toDouble(-1.0);
        if (!optionalId.isDouble() || !std::isfinite(raw) || std::floor(raw) != raw || raw != double(channel.nodeId))
            fail(QStringLiteral("INVALID_ARGUMENTS"), QStringLiteral("nodePhysicalId must match the node encoded by channelId."));
    }
    return channel;
}

double valueAt(const QString &root,
               const QJsonObject &manifest,
               const QJsonObject &frame,
               const ParsedQueryChannel &channel)
{
    const QString framePath = safeChildPath(root,
        QDir(root).filePath(frame.value(QStringLiteral("relativePath")).toString()),
        QStringLiteral("UNSAFE_RESULT_PATH"));
    RModel model;
    const int recordNo = frame.value(QStringLiteral("recordNo")).toInt(-1);
    const ModelInfo info = inspectModel(framePath, recordNo,
        Provenance{QJsonObject{}, manifest.value(QStringLiteral("jobId")).toString(),
                   manifest.value(QStringLiteral("inputPackageSha256")).toString(),
                   manifest.value(QStringLiteral("sourceKind")).toString() == QLatin1String("native-active-fe")
                       ? QStringLiteral("active") : QStringLiteral("passive"),
                   manifest.value(QStringLiteral("sourceKind")).toString(),
                   manifest.value(QStringLiteral("control")).toObject().value(QStringLiteral("enabled")).toBool(),
                   manifest.value(QStringLiteral("control")).toObject().value(QStringLiteral("controllerId")).toString()},
        true, &model, frame.value(QStringLiteral("sha256")).toString());
    if (info.analysisType != QLatin1String("structural-dynamics") ||
        !closeTime(info.timeS, frame.value(QStringLiteral("timeS")).toDouble()) ||
        info.nodeMapSha256 != frame.value(QStringLiteral("nodePhysicalIdMapSha256")).toString() ||
        info.nodeIds != [&manifest]() {
            QVector<uint> ids;
            for (const QJsonValue &value : manifest.value(QStringLiteral("nodePhysicalIds")).toArray())
                ids.append(uint(value.toDouble()));
            return ids;
        }())
        fail(QStringLiteral("RESULT_FRAME_METADATA_CHANGED"), QStringLiteral("A MODEL frame's persisted time or domain changed."));

    const int nodeIndex = info.nodeIds.indexOf(channel.nodeId);
    if (nodeIndex < 0)
        fail(QStringLiteral("CHANNEL_UNAVAILABLE"), QStringLiteral("The requested node physical identifier is unavailable in this MODEL frame."));
    const uint nodePosition = uint(nodeIndex);
    const uint variablePosition = model.findVariable(channel.type);
    if (variablePosition == RConstants::eod)
        fail(QStringLiteral("CHANNEL_UNAVAILABLE"), QStringLiteral("The requested nodal field is missing from a frame."));
    const RVariable &variable = model.getVariable(variablePosition);
    if (variable.getApplyType() != R_VARIABLE_APPLY_NODE || variable.getNVectors() != 3 ||
        variable.getNValues() != model.getNNodes() || variable.getUnits() != channel.unit || channel.unit.isEmpty())
        fail(QStringLiteral("RESULT_FRAME_INVALID"), QStringLiteral("A requested field has invalid dimensions or units."));
    const double value = variable.getValue(channel.component, nodePosition);
    if (!std::isfinite(value))
        fail(QStringLiteral("NONFINITE_RESULT_FIELD"), QStringLiteral("A requested native result value is non-finite."));
    return value;
}

QJsonObject failureJson(const QString &code, const QString &message)
{
    return QJsonObject{{QStringLiteral("valid"), false},
                       {QStringLiteral("code"), code},
                       {QStringLiteral("message"), message}};
}

} // namespace

RNativeResultBroker::PublishResult RNativeResultBroker::publishResult(
    const QString &resultDirectory,
    const QString &latestArtifactPath,
    const QJsonObject &provenanceObject)
{
    PublishResult result;
    try
    {
        const QString root = canonicalDirectory(resultDirectory);
        const QString logicalPath = safeChildPath(root,
            QFileInfo(latestArtifactPath).isAbsolute() ? latestArtifactPath : QDir(root).filePath(latestArtifactPath),
            QStringLiteral("UNSAFE_RESULT_PATH"));
        const Provenance provenance = validateProvenance(provenanceObject);
        const QFileInfo logicalInfo(logicalPath);
        const QString logicalShaBefore = hashFile(logicalPath, MaxFrameBytes,
                                                   QStringLiteral("INVALID_LATEST_ARTIFACT"));
        const RFileHeader logicalHeader = readHeader(logicalPath);
        QString terminalPath;
        QString logicalType;
        const qint64 logicalMaxBytes = logicalHeader.getType() == R_FILE_TYPE_MODEL
            ? MaxFrameBytes : 1024 * 1024;
        if (logicalInfo.size() <= 0 || logicalInfo.size() > logicalMaxBytes)
            fail(QStringLiteral("INVALID_LATEST_ARTIFACT"),
                 QStringLiteral("The latest artifact exceeds its explicit native file size limit."));
        const QString logicalSha = hashFile(logicalPath, logicalMaxBytes, QStringLiteral("INVALID_LATEST_ARTIFACT"));
        if (logicalSha != logicalShaBefore)
            fail(QStringLiteral("RESULT_CHANGED_DURING_READ"),
                 QStringLiteral("The logical latest artifact changed while the broker was reading it."));
        if (logicalHeader.getType() == R_FILE_TYPE_LINK)
        {
            logicalType = QStringLiteral("LINK");
            terminalPath = resolveLinkTarget(root, logicalPath, logicalHeader);
        }
        else if (logicalHeader.getType() == R_FILE_TYPE_MODEL)
        {
            logicalType = QStringLiteral("MODEL");
            terminalPath = safeChildPath(root, logicalPath, QStringLiteral("UNSAFE_RESULT_PATH"));
        }
        else
        {
            fail(QStringLiteral("INVALID_LATEST_ARTIFACT"),
                 QStringLiteral("The latest artifact must be a native LINK or MODEL."));
        }
        const QFileInfo terminalInfo(terminalPath);
        if (terminalInfo.size() <= 0 || terminalInfo.size() > MaxFrameBytes)
            fail(QStringLiteral("UNSUPPORTED_FRAME_SIZE"), QStringLiteral("The terminal MODEL exceeds 256 MiB."));
        if (QFileInfo(terminalPath).suffix() != QFileInfo(logicalPath).suffix())
            fail(QStringLiteral("INVALID_RESULT_LINK"), QStringLiteral("The logical artifact and terminal MODEL extensions must match."));
        const QFileInfo terminalName(terminalPath);
        static const QRegularExpression numberedName(QStringLiteral("-(\\d{5,10})\\.(ram|rbm)$"));
        const QRegularExpressionMatch targetMatch = numberedName.match(terminalName.fileName());
        if (!targetMatch.hasMatch())
            fail(QStringLiteral("UNSUPPORTED_RESULT_NAME"),
                 QStringLiteral("The terminal native MODEL must be a numbered 5-digit record."));
        bool recordParsed = false;
        const int linkedRecordNo = targetMatch.captured(1).toInt(&recordParsed);
        if (!recordParsed || linkedRecordNo > 1000000)
            fail(QStringLiteral("INVALID_RECORD_NAME"), QStringLiteral("The terminal MODEL record number is invalid."));

        const ModelInfo terminalModelInfo = inspectModel(terminalPath, linkedRecordNo, provenance, true);
        const QString terminalSha = terminalModelInfo.sha256;
        if (logicalType == QLatin1String("MODEL") && logicalSha != terminalSha)
            fail(QStringLiteral("RESULT_CHANGED_DURING_READ"),
                 QStringLiteral("The raw logical MODEL changed while the broker was reading it."));
        if (logicalType == QLatin1String("LINK") &&
            hashFile(logicalPath, logicalMaxBytes, QStringLiteral("INVALID_LATEST_ARTIFACT")) != logicalSha)
            fail(QStringLiteral("RESULT_CHANGED_DURING_READ"),
                 QStringLiteral("The logical LINK changed while the broker was reading it."));
        const QString terminalAnalysisType = terminalModelInfo.analysisType;
        if (logicalType == QLatin1String("MODEL") && terminalAnalysisType != QLatin1String("static"))
            fail(QStringLiteral("LATEST_ARTIFACT_NOT_LINK"),
                 QStringLiteral("A raw terminal MODEL can identify a single static result; modal and dynamics require the logical LINK."));
        QVector<NumberedFile> actualFiles;
        QVector<QJsonObject> frameObjects;
        QVector<uint> canonicalNodeIds = terminalModelInfo.nodeIds;
        QSet<QString> sharedVariables = terminalModelInfo.variables;

        if (terminalAnalysisType == QLatin1String("structural-dynamics"))
        {
            actualFiles = enumerateNumberedModels(root, logicalPath, terminalPath);
            frameObjects.reserve(actualFiles.size());
            sharedVariables.clear();
            bool firstFrame = true;
            double previousTime = -std::numeric_limits<double>::infinity();
            for (const NumberedFile &file : actualFiles)
            {
                const ModelInfo frame = inspectModel(file.path, file.recordNo, provenance, true);
                if (frame.analysisType != terminalAnalysisType || frame.nodeIds != canonicalNodeIds ||
                    frame.meshFingerprint != terminalModelInfo.meshFingerprint ||
                    frame.beamModel != terminalModelInfo.beamModel || frame.timeS <= previousTime)
                    fail(QStringLiteral("INCONSISTENT_FRAME_METADATA"),
                         QStringLiteral("Numbered MODEL frames disagree on analysis, physical node order, or actual time."));
                previousTime = frame.timeS;
                if (firstFrame)
                {
                    sharedVariables = frame.variables;
                    firstFrame = false;
                }
                else
                {
                    for (auto it = sharedVariables.begin(); it != sharedVariables.end(); )
                    {
                        if (!frame.variables.contains(*it)) it = sharedVariables.erase(it);
                        else ++it;
                    }
                }
                frameObjects.append(QJsonObject{{QStringLiteral("recordNo"), file.recordNo},
                    {QStringLiteral("timeS"), frame.timeS},
                    {QStringLiteral("relativePath"), file.relativePath},
                    {QStringLiteral("sha256"), frame.sha256},
                    {QStringLiteral("nodePhysicalIdMapSha256"), frame.nodeMapSha256}});
            }
            if (frameObjects.isEmpty() || frameObjects.last().value(QStringLiteral("relativePath")).toString() !=
                    QFileInfo(terminalPath).fileName())
                fail(QStringLiteral("INVALID_RESULT_LINK"),
                     QStringLiteral("The latest LINK target must be the final accepted dynamics frame."));
        }
        else if (terminalAnalysisType == QLatin1String("static"))
        {
            if (linkedRecordNo != 0)
                fail(QStringLiteral("RECORD_STEP_MISMATCH"), QStringLiteral("Static results must use record number zero."));
        }
        else if (terminalAnalysisType == QLatin1String("modal"))
        {
            if (terminalModelInfo.modeIndex < 0 || linkedRecordNo != terminalModelInfo.modeIndex + 1)
                fail(QStringLiteral("RECORD_STEP_MISMATCH"), QStringLiteral("The latest modal record does not match its persisted mode."));
        }

        QJsonArray nodeIds = idsToJson(canonicalNodeIds);
        const QString nodeMapSha = physicalMapSha256(nodeIds);
        QJsonArray variables;
        const QStringList sortedVariables = sortedVariableIds(sharedVariables);
        for (const QString &variableId : sortedVariables) variables.append(variableId);

        const QString artifactId = artifactIdFor(provenance);
        const QString manifestPath = QDir(root).filePath(ManifestName);
        const QFileInfo manifestInfo(manifestPath);
        if (manifestInfo.exists() || manifestInfo.isSymLink())
            fail(QStringLiteral("MANIFEST_ALREADY_PUBLISHED"),
                 QStringLiteral("The result manifest is immutable and already exists."));
        const QString lockPath = manifestPath + QStringLiteral(".lock");
        if (QFileInfo(lockPath).isSymLink())
            fail(QStringLiteral("UNSAFE_MANIFEST_PATH"), QStringLiteral("The manifest lock path cannot be a symbolic link."));
        QLockFile lock(lockPath);
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            fail(QStringLiteral("MANIFEST_PUBLICATION_BUSY"), QStringLiteral("Another publisher is using this result directory."));
        if (QFileInfo(manifestPath).exists())
            fail(QStringLiteral("MANIFEST_ALREADY_PUBLISHED"),
                 QStringLiteral("The result manifest is immutable and already exists."));

        QJsonObject analysis{{QStringLiteral("domain"), QStringLiteral("structural-fe")},
                             {QStringLiteral("type"), terminalAnalysisType}};
        if (terminalModelInfo.modeIndex >= 0)
            analysis.insert(QStringLiteral("modeIndex"), terminalModelInfo.modeIndex);
        else
            analysis.insert(QStringLiteral("modeIndex"), QJsonValue(QJsonValue::Null));
        const QJsonArray framesArray = [&frameObjects]()
        {
            QJsonArray array;
            for (const QJsonObject &frame : frameObjects) array.append(frame);
            return array;
        }();
        const QJsonObject manifest{
            {QStringLiteral("apiVersion"), ManifestApiVersion},
            {QStringLiteral("artifactId"), artifactId},
            {QStringLiteral("jobId"), provenance.jobId},
            {QStringLiteral("sourceKind"), provenance.sourceKind},
            {QStringLiteral("mode"), provenance.mode},
            {QStringLiteral("validationScope"), QStringLiteral("caller-file-owner")},
            {QStringLiteral("sampleScope"), terminalAnalysisType == QLatin1String("structural-dynamics")
                ? QStringLiteral("all-persisted-numbered-model-records")
                : QStringLiteral("single-terminal-model")},
            {QStringLiteral("proofScope"), provenanceObject.value(QStringLiteral("proofScope"))},
            {QStringLiteral("sourceRuntimeStamp"), provenanceObject.value(QStringLiteral("sourceRuntimeStamp"))},
            {QStringLiteral("sourceSnapshotRef"), provenanceObject.value(QStringLiteral("sourceSnapshotRef"))},
            {QStringLiteral("inputSnapshotSha256"), provenanceObject.value(QStringLiteral("inputSnapshotSha256"))},
            {QStringLiteral("inputPackageSha256"), provenanceObject.value(QStringLiteral("inputPackageSha256"))},
            {QStringLiteral("runtime"), provenanceObject.value(QStringLiteral("runtime"))},
            {QStringLiteral("control"), provenanceObject.value(QStringLiteral("control"))},
            {QStringLiteral("analysis"), analysis},
            {QStringLiteral("logicalLatest"), QJsonObject{
                {QStringLiteral("relativePath"), QFileInfo(logicalPath).fileName()},
                {QStringLiteral("sha256"), logicalSha},
                {QStringLiteral("type"), logicalType}}},
            {QStringLiteral("terminalModel"), QJsonObject{
                {QStringLiteral("relativePath"), QFileInfo(terminalPath).fileName()},
                {QStringLiteral("sha256"), terminalSha},
                {QStringLiteral("recordNo"), linkedRecordNo}}},
            {QStringLiteral("nodePhysicalIds"), nodeIds},
            {QStringLiteral("nodePhysicalIdMapSha256"), nodeMapSha},
            {QStringLiteral("availableVariables"), variables},
            {QStringLiteral("frames"), framesArray}
        };
        const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        if (manifestBytes.size() > MaxManifestBytes)
            fail(QStringLiteral("UNSUPPORTED_MANIFEST_SIZE"),
                 QStringLiteral("The validated frame index exceeds the 16 MiB manifest limit."));

        QSaveFile output(manifestPath);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly) || output.write(manifestBytes) != manifestBytes.size() || !output.commit())
            fail(QStringLiteral("MANIFEST_PUBLISH_FAILED"), QStringLiteral("The immutable result manifest could not be committed atomically."));

        result.valid = true;
        result.code = QStringLiteral("PUBLISHED");
        result.message = QStringLiteral("The hash-bound native result manifest was published.");
        result.manifestPath = manifestPath;
        result.manifestSha256 = digestBytes(manifestBytes);
        result.artifactId = artifactId;
        result.manifest = manifest;
    }
    catch (const BrokerFailure &failure)
    {
        result.code = failure.code;
        result.message = failure.message;
    }
    catch (const std::exception &exception)
    {
        result.code = QStringLiteral("NATIVE_RESULT_ERROR");
        result.message = QString::fromUtf8(exception.what());
    }
    catch (...)
    {
        result.code = QStringLiteral("NATIVE_RESULT_ERROR");
        result.message = QStringLiteral("Native result publication failed.");
    }
    return result;
}

QJsonObject RNativeResultBroker::query(const QString &manifestPath,
                                       const QString &expectedSha256,
                                       const QString &operation,
                                       const QJsonObject &arguments)
{
    try
    {
        QString root;
        const QJsonObject manifest = loadManifest(manifestPath, expectedSha256, &root);
        const QString artifactId = manifest.value(QStringLiteral("artifactId")).toString();
        const QJsonArray frames = manifest.value(QStringLiteral("frames")).toArray();
        const int totalFrames = int(frames.size());
        const QString analysisType = manifest.value(QStringLiteral("analysis")).toObject()
                                         .value(QStringLiteral("type")).toString();

        if (operation == QLatin1String("catalog"))
        {
            requireOnlyKeys(arguments, {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("cursor")});
            const int offset = pageOffset(arguments, artifactId, totalFrames);
            const int limit = pageLimit(arguments);
            QJsonArray page;
            for (int i = offset; i < std::min(totalFrames, offset + limit); ++i)
                page.append(framePublic(frames.at(i).toObject()));
            const bool more = offset + int(page.size()) < totalFrames;
            QJsonObject response{{QStringLiteral("valid"), true},
                {QStringLiteral("operation"), operation},
                {QStringLiteral("artifactId"), artifactId},
                {QStringLiteral("jobId"), manifest.value(QStringLiteral("jobId"))},
                {QStringLiteral("sourceKind"), manifest.value(QStringLiteral("sourceKind"))},
                {QStringLiteral("mode"), manifest.value(QStringLiteral("mode"))},
                {QStringLiteral("validationScope"), manifest.value(QStringLiteral("validationScope"))},
                {QStringLiteral("proofScope"), manifest.value(QStringLiteral("proofScope"))},
                {QStringLiteral("sourceRuntimeStamp"), manifest.value(QStringLiteral("sourceRuntimeStamp"))},
                {QStringLiteral("sourceSnapshotRef"), manifest.value(QStringLiteral("sourceSnapshotRef"))},
                {QStringLiteral("inputSnapshotSha256"), manifest.value(QStringLiteral("inputSnapshotSha256"))},
                {QStringLiteral("inputPackageSha256"), manifest.value(QStringLiteral("inputPackageSha256"))},
                {QStringLiteral("runtime"), manifest.value(QStringLiteral("runtime"))},
                {QStringLiteral("control"), manifest.value(QStringLiteral("control"))},
                {QStringLiteral("sampleScope"), manifest.value(QStringLiteral("sampleScope"))},
                {QStringLiteral("analysis"), manifest.value(QStringLiteral("analysis"))},
                {QStringLiteral("frameCount"), frames.size()},
                {QStringLiteral("frames"), page},
                {QStringLiteral("offset"), offset},
                {QStringLiteral("limit"), limit},
                {QStringLiteral("nodeCount"), manifest.value(QStringLiteral("nodePhysicalIds")).toArray().size()},
                {QStringLiteral("availableVariables"), manifest.value(QStringLiteral("availableVariables"))},
                {QStringLiteral("logicalLatest"), manifest.value(QStringLiteral("logicalLatest"))},
                {QStringLiteral("terminalModel"), manifest.value(QStringLiteral("terminalModel"))}};
            if (more) response.insert(QStringLiteral("nextCursor"), encodeCursor(artifactId, offset + page.size()));
            return response;
        }

        if (operation == QLatin1String("channels"))
        {
            requireOnlyKeys(arguments, {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("cursor")});
            const int count = totalChannelCount(manifest);
            const int offset = pageOffset(arguments, artifactId, count);
            const int limit = pageLimit(arguments);
            QJsonArray page;
            const QVector<Channel> channels = channelPage(manifest, offset, limit);
            for (const Channel &channel : channels)
            {
                page.append(QJsonObject{{QStringLiteral("channelId"), channel.id},
                    {QStringLiteral("variableId"), channel.variableId},
                    {QStringLiteral("quantity"), channel.quantity},
                    {QStringLiteral("axis"), channel.axis},
                    {QStringLiteral("unit"), channel.unit},
                    {QStringLiteral("nodeTarget"), QStringLiteral("persistent-node-physical-id")}});
            }
            const bool more = offset + page.size() < count;
            QJsonObject response{{QStringLiteral("valid"), true},
                {QStringLiteral("operation"), operation},
                {QStringLiteral("artifactId"), artifactId},
                {QStringLiteral("analysis"), manifest.value(QStringLiteral("analysis"))},
                {QStringLiteral("channelCount"), count},
                {QStringLiteral("channels"), page},
                {QStringLiteral("offset"), offset},
                {QStringLiteral("limit"), limit}};
            if (more) response.insert(QStringLiteral("nextCursor"), encodeCursor(artifactId, offset + page.size()));
            return response;
        }

        if (operation == QLatin1String("series"))
        {
            requireOnlyKeys(arguments, {QStringLiteral("channelId"), QStringLiteral("nodePhysicalId"),
                                        QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("cursor")});
            const ParsedQueryChannel channel = resolveQueryChannel(manifest, arguments);
            const int offset = pageOffset(arguments, artifactId, totalFrames);
            const int limit = pageLimit(arguments);
            QJsonArray samples;
            for (int i = offset; i < std::min(totalFrames, offset + limit); ++i)
            {
                const QJsonObject frame = frames.at(i).toObject();
                const double value = valueAt(root, manifest, frame, channel);
                samples.append(QJsonObject{{QStringLiteral("recordNo"), frame.value(QStringLiteral("recordNo"))},
                    {QStringLiteral("timeS"), frame.value(QStringLiteral("timeS"))},
                    {QStringLiteral("value"), value}});
            }
            const bool more = offset + int(samples.size()) < totalFrames;
            QJsonObject response{{QStringLiteral("valid"), true},
                {QStringLiteral("operation"), operation},
                {QStringLiteral("artifactId"), artifactId},
                {QStringLiteral("channelId"), arguments.value(QStringLiteral("channelId"))},
                {QStringLiteral("nodePhysicalId"), double(channel.nodeId)},
                {QStringLiteral("variableId"), channel.variableId},
                {QStringLiteral("axis"), channel.axis},
                {QStringLiteral("unit"), channel.unit},
                {QStringLiteral("samples"), samples},
                {QStringLiteral("offset"), offset},
                {QStringLiteral("limit"), limit}};
            if (more) response.insert(QStringLiteral("nextCursor"), encodeCursor(artifactId, offset + samples.size()));
            return response;
        }

        if (operation == QLatin1String("summary"))
        {
            requireOnlyKeys(arguments, {QStringLiteral("channelId"), QStringLiteral("nodePhysicalId")});
            const ParsedQueryChannel channel = resolveQueryChannel(manifest, arguments);
            QVector<double> values;
            values.reserve(frames.size());
            double minimum = std::numeric_limits<double>::infinity();
            double maximum = -std::numeric_limits<double>::infinity();
            double scale = 0.0;
            for (const QJsonValue &frameValue : frames)
            {
                const QJsonObject frame = frameValue.toObject();
                const double value = valueAt(root, manifest, frame, channel);
                values.append(value);
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
                scale = std::max(scale, std::fabs(value));
            }
            if (values.isEmpty())
                fail(QStringLiteral("NO_RESULT_SAMPLES"), QStringLiteral("No actual native result samples are available."));
            double normalizedSum = 0.0;
            double normalizedCompensation = 0.0;
            double normalizedSquares = 0.0;
            for (double value : values)
            {
                const double normalized = scale == 0.0 ? 0.0 : value / scale;
                const double adjusted = normalized - normalizedCompensation;
                const double next = normalizedSum + adjusted;
                normalizedCompensation = (next - normalizedSum) - adjusted;
                normalizedSum = next;
                normalizedSquares += normalized * normalized;
            }
            const double normalizedMean = std::clamp(normalizedSum / values.size(), -1.0, 1.0);
            const double normalizedRms = std::min(1.0, std::sqrt(normalizedSquares / values.size()));
            const double mean = scale == 0.0 ? 0.0 : scale * normalizedMean;
            const double rms = scale == 0.0 ? 0.0 : scale * normalizedRms;
            if (!std::isfinite(mean) || !std::isfinite(rms))
                fail(QStringLiteral("SUMMARY_RANGE_ERROR"), QStringLiteral("The stable summary could not be represented as finite doubles."));
            return QJsonObject{{QStringLiteral("valid"), true},
                {QStringLiteral("operation"), operation},
                {QStringLiteral("artifactId"), artifactId},
                {QStringLiteral("channelId"), arguments.value(QStringLiteral("channelId"))},
                {QStringLiteral("nodePhysicalId"), double(channel.nodeId)},
                {QStringLiteral("variableId"), channel.variableId},
                {QStringLiteral("axis"), channel.axis},
                {QStringLiteral("unit"), channel.unit},
                {QStringLiteral("sampleCount"), values.size()},
                {QStringLiteral("mean"), mean},
                {QStringLiteral("rms"), rms},
                {QStringLiteral("min"), minimum},
                {QStringLiteral("max"), maximum},
                {QStringLiteral("firstRecordNo"), frames.first().toObject().value(QStringLiteral("recordNo"))},
                {QStringLiteral("lastRecordNo"), frames.last().toObject().value(QStringLiteral("recordNo"))}};
        }

        Q_UNUSED(analysisType);
        fail(QStringLiteral("UNSUPPORTED_OPERATION"),
             QStringLiteral("Supported native result operations are catalog, channels, series, and summary."));
    }
    catch (const BrokerFailure &failure)
    {
        return failureJson(failure.code, failure.message);
    }
    catch (const std::exception &exception)
    {
        return failureJson(QStringLiteral("NATIVE_RESULT_ERROR"), QString::fromUtf8(exception.what()));
    }
    catch (...)
    {
        return failureJson(QStringLiteral("NATIVE_RESULT_ERROR"), QStringLiteral("Native result query failed."));
    }
}
