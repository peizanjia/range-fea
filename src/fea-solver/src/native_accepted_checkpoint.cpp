#include "native_accepted_checkpoint.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <rml_model.h>
#include <rml_structural_dynamics_restart_data.h>
#include <rml_structural_dynamics_setup.h>
#include <rml_time_solver.h>

namespace
{
constexpr auto contextSchema = "range.native-checkpoint-context/v1";
constexpr auto envelopeSchema = "range.native-checkpoint/v1";
constexpr auto pointerSchema = "range.native-checkpoint-pointer/v1";
constexpr auto pendingMarkerName = "pending-publication.json";
constexpr auto policyId = "accepted-steps-or-sim-time-v1";
constexpr uint frozenMaxAcceptedSteps = 10;
constexpr double frozenMaxSimulationSeconds = 1.0;
constexpr qsizetype maxContextBytes = 16384;
constexpr qint64 maxModelBytes = 1024ll*1024ll*1024ll;

void requireNoSymlinkComponents(const QString &absolutePath);

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toUtf8().constData());
}

bool isLowerHexSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value)
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
              (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
            return false;
    return true;
}

QString sha256File(const QString &path)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() > maxModelBytes)
        fail(QStringLiteral("Checkpoint artifact must be a regular file of at most 1 GiB: '%1'.").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Cannot read checkpoint file '%1': %2")
                 .arg(path,file.errorString()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot hash checkpoint file '%1': %2")
                 .arg(path,file.errorString()));
    return QString::fromLatin1(hash.result().toHex());
}

QByteArray readFile(const QString &path, qsizetype maximumBytes = -1)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink())
        fail(QStringLiteral("Checkpoint artifact is not a regular file: '%1'.").arg(path));
    if (maximumBytes >= 0 && info.size() > maximumBytes)
        fail(QStringLiteral("Checkpoint metadata exceeds the %1-byte limit: '%2'.")
                 .arg(maximumBytes).arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("Cannot open checkpoint metadata '%1': %2")
                 .arg(path,file.errorString()));
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        fail(QStringLiteral("Cannot read checkpoint metadata '%1': %2")
                 .arg(path,file.errorString()));
    return bytes;
}

void writeAtomic(const QString &path, const QByteArray &bytes)
{
    requireNoSymlinkComponents(QFileInfo(path).absolutePath());
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile()))
        fail(QStringLiteral("Checkpoint publication refuses a non-regular target: '%1'.")
                 .arg(path));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        fail(QStringLiteral("Cannot stage checkpoint publication '%1': %2")
                 .arg(path,file.errorString()));
    if (file.write(bytes) != bytes.size())
    {
        const QString error = file.errorString();
        file.cancelWriting();
        fail(QStringLiteral("Cannot write checkpoint publication '%1': %2")
                 .arg(path,error));
    }
    if (!file.commit())
        fail(QStringLiteral("Cannot atomically publish checkpoint '%1': %2")
                 .arg(path,file.errorString()));
}

QJsonObject parseObject(const QByteArray &bytes, const QString &what,
                        qsizetype maximumBytes)
{
    if (bytes.isEmpty() || bytes.size() > maximumBytes)
        fail(QStringLiteral("%1 must contain 1..%2 bytes.").arg(what).arg(maximumBytes));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes,&error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("%1 is not a valid JSON object: %2.")
                 .arg(what,error.errorString()));
    return document.object();
}

void requireOnlyKeys(const QJsonObject &object, QStringList expected,
                     const QString &what)
{
    QStringList actual = object.keys();
    std::sort(actual.begin(),actual.end());
    std::sort(expected.begin(),expected.end());
    if (actual != expected)
        fail(QStringLiteral("%1 has missing or unknown fields.").arg(what));
}

void requireHashField(const QJsonObject &object, const QString &key)
{
    if (!object.value(key).isString() ||
        !isLowerHexSha256(object.value(key).toString()))
        fail(QStringLiteral("Checkpoint context field '%1' must be a lowercase SHA-256.")
                 .arg(key));
}

bool closeTime(double actual, double expected)
{
    const double scale = std::max({1.0,std::abs(actual),std::abs(expected)});
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual-expected) <= 1.0e-12*scale;
}

NativeAcceptedCheckpointStateView legacyStateView(const RModel &model)
{
    const auto &setup = model.getProblemSetup().getStructuralDynamicsSetup();
    const auto &state = model.getProblemSetup().getStructuralDynamicsRestartData();
    state.validate();
    if (!setup.getEnabled() || !std::isfinite(setup.getTimeStep()) ||
        setup.getTimeStep() <= 0.0 ||
        (state.available && state.fixedTimeStep != setup.getTimeStep()))
        fail(QStringLiteral("Native checkpoint requires enabled structural dynamics."));
    QByteArray bytes;
    QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    const auto vector = [&stream](const std::vector<double> &values)
    {
        stream << quint64(values.size());
        for (double value : values)
        {
            if (!std::isfinite(value))
                fail(QStringLiteral("Checkpoint state contains a non-finite binary64 value."));
            stream << value;
        }
    };
    stream << QByteArrayLiteral("range.native-checkpoint.legacy-state/v1")
           << state.available << state.completedSteps << state.time << state.fixedTimeStep
           << state.meshNodeCount << state.meshElementCount << state.physicalDofCount
           << state.meshFingerprint << state.dofFingerprint << state.systemFingerprint;
    vector(state.q); vector(state.v); vector(state.a); vector(state.externalForce);
    stream << state.cumulativeExternalWork << state.cumulativeDampingDissipation
           << state.initialMechanicalEnergy << state.supportResponseAvailable
           << state.supportFingerprint;
    vector(state.supportReactions); vector(state.supportPowers); vector(state.cumulativeSupportWork);
    stream << state.controlAvailable << state.controlProtocolVersion << state.controlFingerprint
           << quint64(state.controlChannelIds.size());
    for (const auto &id : state.controlChannelIds) stream << id;
    vector(state.controlLastCommands); vector(state.controlPowers); vector(state.cumulativeControlWork);
    stream << state.controlControllerState << state.controlDeterministicSeed;
    if (stream.status() != QDataStream::Ok ||
        !std::isfinite(state.time) || !std::isfinite(state.fixedTimeStep) ||
        !std::isfinite(state.cumulativeExternalWork) ||
        !std::isfinite(state.cumulativeDampingDissipation) ||
        !std::isfinite(state.initialMechanicalEnergy))
        fail(QStringLiteral("Checkpoint restart cannot be encoded as finite binary64 state."));
    return {state.available ? state.completedSteps : 0,
            state.available ? state.time : 0.0,
            state.available ? state.fixedTimeStep : setup.getTimeStep(),
            QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)};
}

void validateView(const NativeAcceptedCheckpointStateView &view)
{
    if (view.stateFingerprint32.size() != 32 || !std::isfinite(view.timeS) ||
        !std::isfinite(view.dtS) || view.dtS <= 0.0 ||
        view.sequence > std::numeric_limits<uint>::max())
        fail(QStringLiteral("Native checkpoint extractor returned an invalid state identity."));
}

bool sameView(const NativeAcceptedCheckpointStateView &a,
              const NativeAcceptedCheckpointStateView &b)
{
    return a.sequence == b.sequence && a.timeS == b.timeS && a.dtS == b.dtS &&
           a.stateFingerprint32 == b.stateFingerprint32;
}

bool safeBasename(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") &&
           name != QStringLiteral("..") &&
           QFileInfo(name).fileName() == name &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

void requireNoSymlinkComponents(const QString &absolutePath)
{
    const QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(absolutePath));
    QString root;
    QString remainder;
    if (normalized.startsWith(QStringLiteral("//")))
    {
        QStringList parts = normalized.mid(2).split(QLatin1Char('/'),Qt::SkipEmptyParts);
        if (parts.size() < 2)
            fail(QStringLiteral("Checkpoint UNC path has no server/share root."));
        const QString server = parts.takeFirst();
        const QString share = parts.takeFirst();
        root = QStringLiteral("//%1/%2/").arg(server,share);
        remainder = parts.join(QLatin1Char('/'));
    }
    else if (normalized.size() >= 3 && normalized[0].isLetter() &&
             normalized[1] == QLatin1Char(':') && normalized[2] == QLatin1Char('/'))
    {
        root = normalized.left(3);
        remainder = normalized.mid(3);
    }
    else if (normalized.startsWith(QLatin1Char('/')))
    {
        root = QStringLiteral("/");
        remainder = normalized.mid(1);
    }
    else
    {
        fail(QStringLiteral("Checkpoint directory has no absolute filesystem root."));
    }
    QString current = root;
    const QStringList components = remainder.split(QLatin1Char('/'),Qt::SkipEmptyParts);
    for (const QString &component : components)
    {
        current = QDir(current).filePath(component);
        if (QFileInfo(current).isSymLink())
            fail(QStringLiteral("Native checkpoint path contains a symbolic-link component: '%1'.")
                     .arg(current));
    }
}

QJsonObject pointerObject(const QString &envelopeFile, const QString &envelopeHash)
{
    return {{QStringLiteral("schema"),QString::fromLatin1(pointerSchema)},
            {QStringLiteral("envelopeFile"),envelopeFile},
            {QStringLiteral("envelopeSha256"),envelopeHash}};
}
}

NativeAcceptedCheckpointContext NativeAcceptedCheckpointContext::parse(
    const QByteArray &canonicalJson)
{
    if (canonicalJson.size() > maxContextBytes)
        fail(QStringLiteral("Native checkpoint context exceeds 16384 bytes."));
    const QJsonObject object = parseObject(canonicalJson,
        QStringLiteral("Native checkpoint context"),maxContextBytes);
    requireOnlyKeys(object,{
        QStringLiteral("schema"),QStringLiteral("sourceSha256"),
        QStringLiteral("modelSha256"),QStringLiteral("controlSha256"),
        QStringLiteral("runtimeSha256"),QStringLiteral("packageSha256"),
        QStringLiteral("policy"),QStringLiteral("maxAcceptedSteps"),
        QStringLiteral("maxSimulationSeconds"),QStringLiteral("dtS")
    },QStringLiteral("Native checkpoint context"));
    if (object.value(QStringLiteral("schema")).toString() != QString::fromLatin1(contextSchema))
        fail(QStringLiteral("Unsupported native checkpoint context schema."));

    NativeAcceptedCheckpointContext context;
    for (const QString &key : {QStringLiteral("sourceSha256"),QStringLiteral("modelSha256"),
                               QStringLiteral("controlSha256"),QStringLiteral("runtimeSha256"),
                               QStringLiteral("packageSha256")})
        requireHashField(object,key);
    context.sourceSha256 = object.value(QStringLiteral("sourceSha256")).toString();
    context.modelSha256 = object.value(QStringLiteral("modelSha256")).toString();
    context.controlSha256 = object.value(QStringLiteral("controlSha256")).toString();
    context.runtimeSha256 = object.value(QStringLiteral("runtimeSha256")).toString();
    context.packageSha256 = object.value(QStringLiteral("packageSha256")).toString();
    context.policy = object.value(QStringLiteral("policy")).toString();
    if (context.policy != QString::fromLatin1(policyId))
        fail(QStringLiteral("Native checkpoint policy is not the frozen v1 policy."));

    const QJsonValue stepsValue = object.value(QStringLiteral("maxAcceptedSteps"));
    const QJsonValue secondsValue = object.value(QStringLiteral("maxSimulationSeconds"));
    const QJsonValue dtValue = object.value(QStringLiteral("dtS"));
    if (!stepsValue.isDouble() || stepsValue.toDouble() != double(frozenMaxAcceptedSteps) ||
        !secondsValue.isDouble() || secondsValue.toDouble() != frozenMaxSimulationSeconds ||
        !dtValue.isDouble() || !std::isfinite(dtValue.toDouble()) ||
        dtValue.toDouble() <= 0.0 || dtValue.toDouble() > frozenMaxSimulationSeconds)
        fail(QStringLiteral("Native checkpoint interval or dtS differs from frozen v1 bounds."));
    context.maxAcceptedSteps = frozenMaxAcceptedSteps;
    context.maxSimulationSeconds = frozenMaxSimulationSeconds;
    context.dtS = dtValue.toDouble();
    return context;
}

QString NativeAcceptedCheckpointContext::noControlSha256()
{
    const QByteArray domain = QByteArrayLiteral("range.native-checkpoint.no-control.v1");
    return QString::fromLatin1(QCryptographicHash::hash(
        domain,QCryptographicHash::Sha256).toHex());
}

QJsonObject NativeAcceptedCheckpointContext::toJsonObject() const
{
    return {{QStringLiteral("schema"),QString::fromLatin1(contextSchema)},
            {QStringLiteral("sourceSha256"),this->sourceSha256},
            {QStringLiteral("modelSha256"),this->modelSha256},
            {QStringLiteral("controlSha256"),this->controlSha256},
            {QStringLiteral("runtimeSha256"),this->runtimeSha256},
            {QStringLiteral("packageSha256"),this->packageSha256},
            {QStringLiteral("policy"),this->policy},
            {QStringLiteral("maxAcceptedSteps"),double(this->maxAcceptedSteps)},
            {QStringLiteral("maxSimulationSeconds"),this->maxSimulationSeconds},
            {QStringLiteral("dtS"),this->dtS}};
}

NativeAcceptedCheckpointStore::NativeAcceptedCheckpointStore(
    const QString &directory,
    const QByteArray &contextJson,
    const QString &inputModelSha256,
    const QString &controlSha256,
    const QString &runtimeSha256,
    bool restart,
    const RModel &initialModel,
    StateViewExtractor extractor)
    : checkpointContext(NativeAcceptedCheckpointContext::parse(contextJson)),
      stateViewExtractor(extractor ? std::move(extractor) : StateViewExtractor(legacyStateView))
{
    const QFileInfo requestedDirectory(directory);
    if (!requestedDirectory.isAbsolute())
        fail(QStringLiteral("Native checkpoint directory must be absolute."));
    const QString requestedAbsoluteDirectory =
        QDir::cleanPath(requestedDirectory.absoluteFilePath());
    requireNoSymlinkComponents(requestedAbsoluteDirectory);
    if (!QDir().mkpath(requestedAbsoluteDirectory))
        fail(QStringLiteral("Cannot create native checkpoint directory '%1'.").arg(directory));
    requireNoSymlinkComponents(requestedAbsoluteDirectory);
    QFileInfo canonicalDirectory(requestedAbsoluteDirectory);
    if (!canonicalDirectory.isDir() || canonicalDirectory.isSymLink() ||
        canonicalDirectory.canonicalFilePath().isEmpty())
        fail(QStringLiteral("Native checkpoint directory is not a regular directory."));
    this->directory = QDir::cleanPath(canonicalDirectory.canonicalFilePath());
    const QFileInfo pendingMarker(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)));
    if (pendingMarker.exists() || pendingMarker.isSymLink())
        fail(QStringLiteral("Native checkpoint has an unresolved publication marker; explicit repair is required before recovery."));

    if (controlSha256 != this->checkpointContext.controlSha256)
        fail(QStringLiteral("Checkpoint context controlSha256 does not match the active controller."));
    if (runtimeSha256 != this->checkpointContext.runtimeSha256)
        fail(QStringLiteral("Checkpoint context runtimeSha256 does not match this solver executable."));

    const auto initialView = this->stateViewExtractor(initialModel);
    validateView(initialView);
    if (initialView.dtS != this->checkpointContext.dtS)
        fail(QStringLiteral("Input structural checkpoint dt does not match context dtS."));

    const QString lockPath = QDir(this->directory).filePath(
        QStringLiteral(".native-checkpoint.lock"));
    if (QFileInfo(lockPath).isSymLink())
        fail(QStringLiteral("Native checkpoint lock path cannot be a symbolic link."));
    this->writerLock.reset(new QLockFile(lockPath));
    this->writerLock->setStaleLockTime(0);
    if (!this->writerLock->tryLock(0))
        fail(QStringLiteral("Native checkpoint directory is already locked or has a stale writer lock."));
    const QFileInfo lockedPendingMarker(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)));
    if (lockedPendingMarker.exists() || lockedPendingMarker.isSymLink())
        fail(QStringLiteral("Native checkpoint has an unresolved publication marker; explicit repair is required before recovery."));

    const QString latestPath = QDir(this->directory).filePath(QStringLiteral("latest.json"));
    const QString lastValidPath = QDir(this->directory).filePath(QStringLiteral("lastvalid.json"));
    if (QFileInfo(latestPath).isSymLink() || QFileInfo(lastValidPath).isSymLink())
        fail(QStringLiteral("Native checkpoint pointers cannot be symbolic links."));
    bool existingCheckpoint = false;
    VerifiedCheckpoint latest;
    if (QFileInfo::exists(latestPath))
    {
        latest = this->readPointer(QStringLiteral("latest.json"),false);
        existingCheckpoint = true;
    }
    else if (QFileInfo::exists(lastValidPath))
    {
        fail(QStringLiteral("Committed latest native checkpoint is missing; lastvalid is diagnostic history only."));
    }

    if (!existingCheckpoint && inputModelSha256 != this->checkpointContext.modelSha256)
        fail(QStringLiteral("Checkpoint context modelSha256 does not match the input MODEL."));

    if (existingCheckpoint)
    {
        if (!restart || initialView.sequence == 0)
            fail(QStringLiteral("Existing checkpoint directories can only continue from their verified latest accepted MODEL."));
        RModel verifiedModel;
        verifiedModel.read(latest.modelPath);
        const auto verifiedView = this->stateViewExtractor(verifiedModel);
        validateView(verifiedView);
        if (!sameView(initialView,verifiedView))
            fail(QStringLiteral("Restart MODEL does not match the existing verified native checkpoint."));
        this->receipt = std::make_unique<VerifiedCheckpoint>(std::move(latest));
    }
    this->lastPublishedSequence = initialView.sequence;
    this->lastPublishedTimeS = initialView.timeS;
    this->lastObservedSequence = initialView.sequence;
    this->started = true;
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readPointer(const QString &pointerName,
                                           bool usedLastValid) const
{
    requireNoSymlinkComponents(this->directory);
    const QString pointerPath = QDir(this->directory).filePath(pointerName);
    if (!QFileInfo::exists(pointerPath))
        fail(QStringLiteral("Checkpoint pointer does not exist: '%1'.").arg(pointerName));
    if (QFileInfo(pointerPath).isSymLink())
        fail(QStringLiteral("Checkpoint pointer cannot be a symbolic link."));
    const QJsonObject pointer = parseObject(readFile(pointerPath,64*1024),
        QStringLiteral("Native checkpoint pointer"),64*1024);
    requireOnlyKeys(pointer,{QStringLiteral("schema"),QStringLiteral("envelopeFile"),
                             QStringLiteral("envelopeSha256")},
                    QStringLiteral("Native checkpoint pointer"));
    if (pointer.value(QStringLiteral("schema")).toString() != QString::fromLatin1(pointerSchema))
        fail(QStringLiteral("Unsupported native checkpoint pointer schema."));
    const QString envelopeFile = pointer.value(QStringLiteral("envelopeFile")).toString();
    const QString envelopeHash = pointer.value(QStringLiteral("envelopeSha256")).toString();
    if (!safeBasename(envelopeFile) || !isLowerHexSha256(envelopeHash))
        fail(QStringLiteral("Native checkpoint pointer contains an invalid envelope reference."));
    return this->readEnvelope(envelopeFile,envelopeHash,usedLastValid);
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readEnvelope(const QString &envelopeFile,
                                            const QString &envelopeHash,
                                            bool usedLastValid) const
{
    requireNoSymlinkComponents(this->directory);
    if (!safeBasename(envelopeFile) || !isLowerHexSha256(envelopeHash))
        fail(QStringLiteral("Native checkpoint envelope reference is invalid."));
    const QString envelopePath = QDir(this->directory).filePath(envelopeFile);
    const QByteArray envelopeBytes = readFile(envelopePath,64*1024);
    if (QString::fromLatin1(QCryptographicHash::hash(
            envelopeBytes,QCryptographicHash::Sha256).toHex()) != envelopeHash)
        fail(QStringLiteral("Native checkpoint envelope SHA-256 does not match its pointer."));
    const QJsonObject envelope = parseObject(envelopeBytes,
        QStringLiteral("Native checkpoint envelope"),64*1024);
    const QByteArray canonicalEnvelope = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (canonicalEnvelope != envelopeBytes)
        fail(QStringLiteral("Native checkpoint envelope is not canonical JSON."));
    requireOnlyKeys(envelope,{
        QStringLiteral("schema"),QStringLiteral("sourceSha256"),
        QStringLiteral("inputModelSha256"),QStringLiteral("controlSha256"),
        QStringLiteral("runtimeSha256"),QStringLiteral("packageSha256"),
        QStringLiteral("policy"),QStringLiteral("maxAcceptedSteps"),
        QStringLiteral("maxSimulationSeconds"),QStringLiteral("acceptedSeq"),
        QStringLiteral("timeS"),QStringLiteral("dtS"),
        QStringLiteral("modelFile"),QStringLiteral("checkpointModelSha256")
    },QStringLiteral("Native checkpoint envelope"));
    if (envelope.value(QStringLiteral("schema")).toString() != QString::fromLatin1(envelopeSchema) ||
        envelope.value(QStringLiteral("sourceSha256")).toString() != this->checkpointContext.sourceSha256 ||
        envelope.value(QStringLiteral("inputModelSha256")).toString() != this->checkpointContext.modelSha256 ||
        envelope.value(QStringLiteral("controlSha256")).toString() != this->checkpointContext.controlSha256 ||
        envelope.value(QStringLiteral("runtimeSha256")).toString() != this->checkpointContext.runtimeSha256 ||
        envelope.value(QStringLiteral("packageSha256")).toString() != this->checkpointContext.packageSha256 ||
        envelope.value(QStringLiteral("policy")).toString() != this->checkpointContext.policy ||
        envelope.value(QStringLiteral("maxAcceptedSteps")).toDouble(-1.0) != double(this->checkpointContext.maxAcceptedSteps) ||
        envelope.value(QStringLiteral("maxSimulationSeconds")).toDouble(-1.0) != this->checkpointContext.maxSimulationSeconds)
        fail(QStringLiteral("Native checkpoint envelope does not match the supplied recovery context."));
    const QJsonValue sequenceValue = envelope.value(QStringLiteral("acceptedSeq"));
    const QJsonValue timeValue = envelope.value(QStringLiteral("timeS"));
    const QJsonValue dtValue = envelope.value(QStringLiteral("dtS"));
    const QString modelFile = envelope.value(QStringLiteral("modelFile")).toString();
    const QString modelHash = envelope.value(QStringLiteral("checkpointModelSha256")).toString();
    if (!sequenceValue.isDouble() || sequenceValue.toDouble() < 1.0 ||
        std::floor(sequenceValue.toDouble()) != sequenceValue.toDouble() ||
        sequenceValue.toDouble() > double(std::numeric_limits<uint>::max()) ||
        !timeValue.isDouble() || !std::isfinite(timeValue.toDouble()) ||
        !dtValue.isDouble() || dtValue.toDouble() != this->checkpointContext.dtS ||
        !safeBasename(modelFile) || !modelFile.endsWith(QStringLiteral(".rbm")) ||
        !isLowerHexSha256(modelHash))
        fail(QStringLiteral("Native checkpoint envelope has invalid accepted-state fields."));
    const QString modelPath = QDir(this->directory).filePath(modelFile);
    if (QFileInfo(modelPath).isSymLink() || sha256File(modelPath) != modelHash)
        fail(QStringLiteral("Immutable checkpoint MODEL is missing or its SHA-256 does not match."));
    RModel checkpointModel;
    checkpointModel.read(modelPath);
    if (sha256File(modelPath) != modelHash)
        fail(QStringLiteral("Immutable checkpoint MODEL changed during verification."));
    const auto state = this->stateViewExtractor(checkpointModel);
    validateView(state);
    if (state.sequence != quint64(sequenceValue.toDouble()) ||
        state.timeS != timeValue.toDouble() || state.dtS != dtValue.toDouble() ||
        state.sequence != quint64(checkpointModel.getTimeSolver().getCurrentTimeStep())+1 ||
        !closeTime(state.timeS,checkpointModel.getTimeSolver().getComputedTime()))
        fail(QStringLiteral("Checkpoint MODEL does not match its accepted sequence, time or dt."));
    return {modelPath,envelope,usedLastValid};
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readLatest() const
{
    if (this->poisoned)
        fail(QStringLiteral("Native checkpoint rollback is incomplete; latest is not accepted recovery authority for this store. Inspect the retained rollback evidence."));
    if (this->activePublication)
        fail(QStringLiteral("Native checkpoint publication has not committed."));
    requireNoSymlinkComponents(this->directory);
    const QFileInfo marker(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)));
    if (marker.exists() || marker.isSymLink())
        fail(QStringLiteral("Native checkpoint has an unresolved publication marker; explicit repair is required before recovery."));
    return this->readPointer(QStringLiteral("latest.json"),false);
}

struct NativeAcceptedCheckpointStore::PublicationState
{
    struct PointerSnapshot
    {
        bool exists = false;
        QByteArray bytes;
    };
    RAcceptedStepIdentity identity;
    NativeAcceptedCheckpointStateView expected;
    PointerSnapshot latest;
    PointerSnapshot lastValid;
    QByteArray newPointerBytes;
    std::unique_ptr<VerifiedCheckpoint> verifiedReceipt;
    QStringList evidencePaths;
    QString markerPath;
    QByteArray markerBytes;
    bool due = false;
    bool markerCreated = false;
    bool pointersTouched = false;
    bool published = false;
};

class NativeAcceptedCheckpointStore::Publication final : public RAcceptedStepPublication
{
public:
    NativeAcceptedCheckpointStore *owner;
    std::unique_ptr<PublicationState> state;
    bool committed = false;
    bool rolledBack = false;

    explicit Publication(NativeAcceptedCheckpointStore *owner,
                         std::unique_ptr<PublicationState> state)
        : owner(owner), state(std::move(state)) {}

    ~Publication() override
    {
        if (!this->committed && !this->rolledBack) (void)this->rollback();
    }

    void ensureMarker()
    {
        auto &store = *this->owner;
        requireNoSymlinkComponents(store.directory);
        const QFileInfo marker(this->state->markerPath);
        if (marker.exists() || marker.isSymLink())
        {
            if (readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
                fail(QStringLiteral("Pending publication marker differs from the staged barrier."));
            this->state->markerCreated = true;
            return;
        }
        this->state->markerCreated = true;
        store.invokePhase(Phase::BeforeMarkerWrite);
        if (QFileInfo(this->state->markerPath).exists() || QFileInfo(this->state->markerPath).isSymLink())
            fail(QStringLiteral("Pending publication marker appeared during staging."));
        writeAtomic(this->state->markerPath,this->state->markerBytes);
        store.invokePhase(Phase::AfterMarkerWrite);
        if (readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
            fail(QStringLiteral("Pending publication marker failed read-back verification."));
    }

    //! Its successful removal is the final potentially failing operation in
    //! publish. No I/O, callbacks or allocation follow it before commit.
    void removeMarker(bool allowAbsent)
    {
        auto &store = *this->owner;
        requireNoSymlinkComponents(store.directory);
        const QFileInfo marker(this->state->markerPath);
        if (!marker.exists() && !marker.isSymLink() && allowAbsent)
        {
            this->state->markerCreated = false;
            return;
        }
        if (readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
            fail(QStringLiteral("Refusing to remove a different pending publication marker."));
        if (!QFile::remove(this->state->markerPath))
            fail(QStringLiteral("Cannot remove the pending publication marker."));
        this->state->markerCreated = false;
    }

    void publish() override
    {
        if (!this->owner || !this->state || this->committed || this->rolledBack ||
            this->state->published || this->owner->poisoned)
            fail(QStringLiteral("Native checkpoint publication is not publishable."));
        if (!this->state->due)
        {
            this->state->published = true;
            return;
        }
        auto &store = *this->owner;
        requireNoSymlinkComponents(store.directory);
        if (!this->state->markerCreated ||
            readFile(this->state->markerPath,64*1024) != this->state->markerBytes)
            fail(QStringLiteral("Native checkpoint publication has no verified pending barrier."));
        const QString latestPath = QDir(store.directory).filePath(QStringLiteral("latest.json"));
        const QString lastValidPath = QDir(store.directory).filePath(QStringLiteral("lastvalid.json"));
        const auto unchanged = [](const QString &path, const PublicationState::PointerSnapshot &snapshot)
        {
            const QFileInfo info(path);
            if (info.isSymLink() || info.exists() != snapshot.exists ||
                (snapshot.exists && readFile(path,64*1024) != snapshot.bytes))
                fail(QStringLiteral("Checkpoint pointer changed after staging: '%1'.").arg(path));
        };
        unchanged(latestPath,this->state->latest);
        unchanged(lastValidPath,this->state->lastValid);
        if (this->state->latest.exists)
        {
            this->state->pointersTouched = true;
            store.invokePhase(Phase::BeforeLastValid);
            requireNoSymlinkComponents(store.directory);
            writeAtomic(lastValidPath,this->state->latest.bytes);
            store.invokePhase(Phase::AfterLastValid);
            if (readFile(lastValidPath,64*1024) != this->state->latest.bytes)
                fail(QStringLiteral("Previous valid checkpoint pointer failed read-back verification."));
        }
        this->state->pointersTouched = true;
        store.invokePhase(Phase::BeforeLatest);
        requireNoSymlinkComponents(store.directory);
        writeAtomic(latestPath,this->state->newPointerBytes);
        store.invokePhase(Phase::AfterLatest);
        if (readFile(latestPath,64*1024) != this->state->newPointerBytes)
            fail(QStringLiteral("Latest checkpoint pointer failed read-back verification."));
        const auto verified = store.readPointer(QStringLiteral("latest.json"),false);
        if (verified.modelPath != this->state->verifiedReceipt->modelPath ||
            verified.envelope != this->state->verifiedReceipt->envelope)
            fail(QStringLiteral("Latest checkpoint pointer resolved to a different staged checkpoint."));
        store.invokePhase(Phase::AfterLatestReadBack);
        store.invokePhase(Phase::BeforeMarkerRemove);
        if (readFile(latestPath,64*1024) != this->state->newPointerBytes)
            fail(QStringLiteral("Latest checkpoint pointer changed after read-back verification."));
        const auto finalVerified = store.readPointer(QStringLiteral("latest.json"),false);
        if (finalVerified.modelPath != this->state->verifiedReceipt->modelPath ||
            finalVerified.envelope != this->state->verifiedReceipt->envelope)
            fail(QStringLiteral("Checkpoint state changed after the latest read-back hook."));
        if (this->state->latest.exists)
        {
            if (readFile(lastValidPath,64*1024) != this->state->latest.bytes)
                fail(QStringLiteral("Last-valid checkpoint pointer changed during publication."));
        }
        else
            unchanged(lastValidPath,this->state->lastValid);
        // The host must publish this participant last, then commit without
        // further throwing operations. This barrier is not a crash journal.
        this->removeMarker(false);
        this->state->published = true;
    }

    bool rollback() noexcept override
    {
        if (this->committed || this->rolledBack) return true;
        if (!this->owner || !this->state) return false;
        auto &store = *this->owner;
        bool restored = true;
        const auto restore = [&store,&restored](const QString &name,
            const PublicationState::PointerSnapshot &snapshot, Phase before, Phase after)
        {
            try
            {
                store.invokePhase(before);
                requireNoSymlinkComponents(store.directory);
                const QString path = QDir(store.directory).filePath(name);
                const QFileInfo info(path);
                if (info.isSymLink() || (info.exists() && !info.isFile()))
                    fail(QStringLiteral("Rollback refuses a non-regular pointer target: '%1'.").arg(path));
                if (snapshot.exists)
                {
                    writeAtomic(path,snapshot.bytes);
                    if (readFile(path,64*1024) != snapshot.bytes)
                        fail(QStringLiteral("Rollback pointer read-back differs from its snapshot."));
                }
                else
                {
                    if (info.exists() && !QFile::remove(path))
                        fail(QStringLiteral("Rollback cannot remove the newly published pointer."));
                    if (QFileInfo(path).exists() || QFileInfo(path).isSymLink())
                        fail(QStringLiteral("Rollback did not restore pointer absence."));
                }
                store.invokePhase(after);
                requireNoSymlinkComponents(store.directory);
                if (snapshot.exists)
                {
                    if (readFile(path,64*1024) != snapshot.bytes)
                        fail(QStringLiteral("Rollback pointer changed after restoration."));
                }
                else if (QFileInfo(path).exists() || QFileInfo(path).isSymLink())
                    fail(QStringLiteral("Rollback pointer absence changed after restoration."));
            }
            catch (...)
            {
                restored = false;
                if (!store.rollbackFailure) store.rollbackFailure = std::current_exception();
            }
        };
        if (this->state->pointersTouched)
        {
            try
            {
                // Also guards an explicit rollback after successful publish,
                // which has already removed the original pending barrier.
                this->ensureMarker();
            }
            catch (...)
            {
                restored = false;
                if (!store.rollbackFailure) store.rollbackFailure = std::current_exception();
            }
            if (restored)
            {
                restore(QStringLiteral("latest.json"),this->state->latest,
                        Phase::BeforeRollbackLatest,Phase::AfterRollbackLatest);
                restore(QStringLiteral("lastvalid.json"),this->state->lastValid,
                        Phase::BeforeRollbackLastValid,Phase::AfterRollbackLastValid);
            }
        }
        if (restored && this->state->markerCreated)
        {
            try
            {
                store.invokePhase(Phase::BeforeMarkerRemove);
                const auto verifySnapshot = [&store](const QString &name,
                    const PublicationState::PointerSnapshot &snapshot)
                {
                    const QString path = QDir(store.directory).filePath(name);
                    const QFileInfo info(path);
                    if (info.isSymLink() || info.exists() != snapshot.exists ||
                        (snapshot.exists && readFile(path,64*1024) != snapshot.bytes))
                        fail(QStringLiteral("Rollback pointer pair differs from the original snapshots."));
                };
                requireNoSymlinkComponents(store.directory);
                verifySnapshot(QStringLiteral("latest.json"),this->state->latest);
                verifySnapshot(QStringLiteral("lastvalid.json"),this->state->lastValid);
                this->removeMarker(!this->state->pointersTouched);
            }
            catch (...)
            {
                restored = false;
                if (!store.rollbackFailure) store.rollbackFailure = std::current_exception();
            }
        }
        store.activePublication = nullptr;
        if (!restored)
        {
            store.poisoned = true;
            store.failedRollbackEvidencePaths.swap(this->state->evidencePaths);
            store.failedRollbackState = std::move(this->state);
            return false;
        }
        this->rolledBack = true;
        return true;
    }

    void commit() noexcept override
    {
        if (!this->owner || !this->state || this->committed || this->rolledBack ||
            !this->state->published || this->owner->poisoned) return;
        auto &store = *this->owner;
        store.hasObservedStep = true;
        store.lastObservedSequence = this->state->identity.sequence;
        if (this->state->due)
        {
            store.lastPublishedSequence = this->state->identity.sequence;
            store.lastPublishedTimeS = this->state->identity.timeS;
            store.receipt.swap(this->state->verifiedReceipt);
        }
        store.activePublication = nullptr;
        this->committed = true;
    }
};

NativeAcceptedCheckpointStore::~NativeAcceptedCheckpointStore()
{
    if (this->activePublication)
    {
        auto *publication = this->activePublication;
        (void)publication->rollback();
        publication->owner = nullptr;
    }
}

std::unique_ptr<RAcceptedStepPublication> NativeAcceptedCheckpointStore::stage(
    const RModel &candidateModel, const RAcceptedStepIdentity &identity)
{
    return this->stageImpl(candidateModel,identity,false);
}

std::unique_ptr<RAcceptedStepPublication> NativeAcceptedCheckpointStore::stageImpl(
    const RModel &candidateModel, const RAcceptedStepIdentity &identity,
    bool allowLegacyFirstCallback)
{
    if (!this->started || this->poisoned)
        fail(QStringLiteral("Native checkpoint store is not retryable or initialized."));
    if (this->activePublication)
        fail(QStringLiteral("Native checkpoint store already has a staged publication."));
    requireNoSymlinkComponents(this->directory);
    const QFileInfo pendingMarker(QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName)));
    if (pendingMarker.exists() || pendingMarker.isSymLink())
        fail(QStringLiteral("Native checkpoint has an unresolved publication marker; explicit repair is required before staging."));
    const auto expected = this->stateViewExtractor(candidateModel);
    validateView(expected);
    const auto &clock = candidateModel.getTimeSolver();
    if (identity.sequence == 0 || identity.sequence != expected.sequence ||
        identity.timeS != expected.timeS || identity.dtS != expected.dtS ||
        identity.dtS != this->checkpointContext.dtS ||
        identity.sequence != quint64(clock.getCurrentTimeStep())+1 ||
        !closeTime(identity.timeS,clock.getComputedTime()) ||
        clock.getNTimeSteps() == 0 || identity.sequence > quint64(clock.getNTimeSteps()) ||
        (identity.sequence == quint64(clock.getNTimeSteps()) && !identity.finalStep))
        fail(QStringLiteral("Accepted structural checkpoint does not match the candidate model clock."));
    if ((!allowLegacyFirstCallback || this->hasObservedStep) &&
        identity.sequence != this->lastObservedSequence+1)
        fail(QStringLiteral("Accepted structural checkpoint sequence is not contiguous."));
    if (identity.sequence <= this->lastObservedSequence ||
        identity.timeS < this->lastPublishedTimeS)
        fail(QStringLiteral("Accepted structural checkpoint sequence or time moved backwards."));

    auto state = std::make_unique<PublicationState>();
    state->identity = identity;
    state->expected = expected;
    const double elapsed = identity.timeS-this->lastPublishedTimeS;
    state->due = identity.finalStep ||
        identity.sequence-this->lastPublishedSequence >= this->checkpointContext.maxAcceptedSteps ||
        elapsed >= this->checkpointContext.maxSimulationSeconds ||
        elapsed+identity.dtS > this->checkpointContext.maxSimulationSeconds;
    requireNoSymlinkComponents(this->directory);
    const auto snapshot = [this](const QString &name)
    {
        PublicationState::PointerSnapshot saved;
        const QString path = QDir(this->directory).filePath(name);
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isFile()))
            fail(QStringLiteral("Checkpoint pointer is not a regular file: '%1'.").arg(path));
        saved.exists = info.exists();
        if (saved.exists) saved.bytes = readFile(path,64*1024);
        return saved;
    };
    state->latest = snapshot(QStringLiteral("latest.json"));
    state->lastValid = snapshot(QStringLiteral("lastvalid.json"));
    if (state->latest.exists)
        (void)this->readPointer(QStringLiteral("latest.json"),false);
    else if (state->lastValid.exists)
        fail(QStringLiteral("Committed latest checkpoint is missing; lastvalid is diagnostic history only."));
    auto publication = std::make_unique<Publication>(this,std::move(state));
    this->activePublication = publication.get();
    if (!publication->state->due) return publication;

    // All immutable writes and mode-specific validation finish before any
    // recovery pointer is published. No mutable RModel is retained by the token.
    const quint64 acceptedSequence = identity.sequence;
    const double acceptedTimeS = identity.timeS;
    const double dtS = identity.dtS;
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Durable evidence precedes pointer publication and is never a recovery
    // authority. Small successful/abandoned preparations may remain unreferenced.
    this->invokePhase(Phase::BeforeEvidenceWrite);
    const auto evidenceFile = [this,&publication](const QString &name, const QByteArray &bytes)
    {
        if (bytes.size() > 64*1024)
            fail(QStringLiteral("Native checkpoint rollback evidence exceeds 64 KiB."));
        const QString path = QDir(this->directory).filePath(name);
        if (QFileInfo(path).exists() || QFileInfo(path).isSymLink())
            fail(QStringLiteral("Refusing to replace immutable rollback evidence: '%1'.").arg(path));
        writeAtomic(path,bytes);
        if (readFile(path,64*1024) != bytes)
            fail(QStringLiteral("Native checkpoint rollback evidence failed read-back verification."));
        publication->state->evidencePaths.append(path);
        return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    };
    const auto backup = [&token,&evidenceFile](const QString &pointerName,
        const PublicationState::PointerSnapshot &snapshot)
    {
        QJsonObject descriptor{{QStringLiteral("exists"),snapshot.exists}};
        if (snapshot.exists)
        {
            const QString backupName = QStringLiteral("rollback-%1-%2.pointer").arg(token,pointerName);
            const QString hash = evidenceFile(backupName,snapshot.bytes);
            descriptor.insert(QStringLiteral("file"),backupName);
            descriptor.insert(QStringLiteral("sha256"),hash);
        }
        return descriptor;
    };
    const QJsonObject latestBackup = backup(QStringLiteral("latest"),publication->state->latest);
    const QJsonObject lastValidBackup = backup(QStringLiteral("lastvalid"),publication->state->lastValid);
    const QByteArray contextBytes = QJsonDocument(this->checkpointContext.toJsonObject())
        .toJson(QJsonDocument::Compact);
    const QJsonObject evidenceManifest{
        {QStringLiteral("schema"),QStringLiteral("range.native-checkpoint-rollback-evidence/v1")},
        {QStringLiteral("candidateSeq"),double(identity.sequence)},
        {QStringLiteral("sourceSha256"),this->checkpointContext.sourceSha256},
        {QStringLiteral("contextSha256"),QString::fromLatin1(QCryptographicHash::hash(
            contextBytes,QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("latest"),latestBackup},
        {QStringLiteral("lastvalid"),lastValidBackup}
    };
    const QString evidenceManifestName = QStringLiteral("rollback-evidence-%1.json").arg(token);
    const QString evidenceManifestHash = evidenceFile(evidenceManifestName,
        QJsonDocument(evidenceManifest).toJson(QJsonDocument::Compact));
    this->invokePhase(Phase::AfterEvidenceReadBack);

    const QString modelFile = QStringLiteral("checkpoint-%1-%2.rbm")
        .arg(acceptedSequence,10,10,QChar('0')).arg(token);
    const QString modelPath = QDir(this->directory).filePath(modelFile);
    if (QFileInfo::exists(modelPath) || QFileInfo(modelPath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint MODEL."));

    this->invokePhase(Phase::BeforeModelWrite);
    requireNoSymlinkComponents(this->directory);
    if (QFileInfo::exists(modelPath) || QFileInfo(modelPath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint MODEL."));
    const QString writtenPath = candidateModel.write(modelPath,false);
    this->invokePhase(Phase::AfterModelWrite);
    if (QFileInfo(writtenPath).absoluteFilePath() != QFileInfo(modelPath).absoluteFilePath())
        fail(QStringLiteral("Native checkpoint MODEL was written to an unexpected path."));
    const QString modelHash = sha256File(modelPath);
    RModel readBack;
    readBack.read(modelPath);
    const auto actual = this->stateViewExtractor(readBack);
    validateView(actual);
    if (sha256File(modelPath) != modelHash || !sameView(expected,actual))
        fail(QStringLiteral("Binary checkpoint MODEL read-back differs from the accepted structural state."));
    this->invokePhase(Phase::AfterModelReadBack);

    QJsonObject envelope = this->checkpointContext.toJsonObject();
    envelope.insert(QStringLiteral("schema"),QString::fromLatin1(envelopeSchema));
    envelope.remove(QStringLiteral("modelSha256"));
    envelope.insert(QStringLiteral("inputModelSha256"),this->checkpointContext.modelSha256);
    envelope.insert(QStringLiteral("acceptedSeq"),double(acceptedSequence));
    envelope.insert(QStringLiteral("timeS"),acceptedTimeS);
    envelope.insert(QStringLiteral("dtS"),dtS);
    envelope.insert(QStringLiteral("modelFile"),modelFile);
    envelope.insert(QStringLiteral("checkpointModelSha256"),modelHash);
    const QString envelopeFile = QStringLiteral("envelope-%1-%2.json")
        .arg(acceptedSequence,10,10,QChar('0')).arg(token);
    const QString envelopePath = QDir(this->directory).filePath(envelopeFile);
    if (QFileInfo::exists(envelopePath) || QFileInfo(envelopePath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint envelope."));
    const QByteArray envelopeBytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    this->invokePhase(Phase::BeforeEnvelopeWrite);
    writeAtomic(envelopePath,envelopeBytes);
    this->invokePhase(Phase::AfterEnvelopeWrite);
    const QString envelopeHash = QString::fromLatin1(
        QCryptographicHash::hash(envelopeBytes,QCryptographicHash::Sha256).toHex());
    if (sha256File(envelopePath) != envelopeHash)
        fail(QStringLiteral("Native checkpoint envelope failed post-write hash verification."));

    const VerifiedCheckpoint verifiedEnvelope =
        this->readEnvelope(envelopeFile,envelopeHash,false);
    if (verifiedEnvelope.envelope.value(QStringLiteral("acceptedSeq")).toDouble(-1.0) !=
        double(acceptedSequence))
        fail(QStringLiteral("Native checkpoint envelope read-back has a different accepted sequence."));
    this->invokePhase(Phase::AfterEnvelopeReadBack);

    const QJsonObject newPointer = pointerObject(envelopeFile,envelopeHash);
    publication->state->newPointerBytes = QJsonDocument(newPointer).toJson(QJsonDocument::Compact);
    publication->state->verifiedReceipt = std::make_unique<VerifiedCheckpoint>(verifiedEnvelope);
    const QJsonObject marker{
        {QStringLiteral("schema"),QStringLiteral("range.native-checkpoint-pending-publication/v1")},
        {QStringLiteral("candidateSeq"),double(identity.sequence)},
        {QStringLiteral("contextSha256"),evidenceManifest.value(QStringLiteral("contextSha256"))},
        {QStringLiteral("rollbackManifestFile"),evidenceManifestName},
        {QStringLiteral("rollbackManifestSha256"),evidenceManifestHash}
    };
    publication->state->markerPath = QDir(this->directory).filePath(QString::fromLatin1(pendingMarkerName));
    publication->state->markerBytes = QJsonDocument(marker).toJson(QJsonDocument::Compact);
    if (publication->state->markerBytes.size() > 64*1024)
        fail(QStringLiteral("Native checkpoint pending publication marker exceeds 64 KiB."));
    publication->state->evidencePaths.append(publication->state->markerPath);
    publication->ensureMarker();
    return publication;
}

void NativeAcceptedCheckpointStore::accept(const RModel &acceptedModel)
{
    const auto view = this->stateViewExtractor(acceptedModel);
    const auto &clock = acceptedModel.getTimeSolver();
    const RAcceptedStepIdentity identity{view.sequence,view.timeS,view.dtS,
        quint64(clock.getCurrentTimeStep())+1 >= clock.getNTimeSteps()};
    // The old postcommit observer may be attached after earlier accepted steps.
    // The precommit participant always requires initial sequence + 1.
    auto publication = this->stageImpl(acceptedModel,identity,true);
    try
    {
        publication->publish();
        publication->commit();
    }
    catch (...)
    {
        (void)publication->rollback();
        throw;
    }
}

bool NativeAcceptedCheckpointStore::isRetryable() const noexcept
{
    return this->started && !this->poisoned;
}

const NativeAcceptedCheckpointStore::VerifiedCheckpoint *
NativeAcceptedCheckpointStore::committedReceipt() const noexcept
{
    return this->receipt.get();
}

void NativeAcceptedCheckpointStore::setPhaseHook(Phase phase, PhaseHook hook)
{
    this->hookPhase = phase;
    this->phaseHook = std::move(hook);
}

void NativeAcceptedCheckpointStore::invokePhase(Phase phase)
{
    if (this->phaseHook && this->hookPhase == phase)
    {
        PhaseHook hook;
        hook.swap(this->phaseHook);
        hook(phase);
    }
}

QString NativeAcceptedCheckpointStore::rollbackDiagnostic() const
{
    if (!this->rollbackFailure) return {};
    try { std::rethrow_exception(this->rollbackFailure); }
    catch (const std::exception &error) { return QString::fromUtf8(error.what()); }
    catch (...) { return QStringLiteral("Native checkpoint rollback failed with an unknown exception."); }
}

const QStringList &NativeAcceptedCheckpointStore::rollbackEvidencePaths() const noexcept
{
    return this->failedRollbackEvidencePaths;
}

const NativeAcceptedCheckpointContext &NativeAcceptedCheckpointStore::context() const
{
    return this->checkpointContext;
}
