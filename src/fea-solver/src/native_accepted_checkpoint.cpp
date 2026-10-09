#include "native_accepted_checkpoint.h"

#include <QCryptographicHash>
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
constexpr auto policyId = "accepted-steps-or-sim-time-v1";
constexpr uint frozenMaxAcceptedSteps = 10;
constexpr double frozenMaxSimulationSeconds = 1.0;
constexpr qsizetype maxContextBytes = 16384;

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
    if (QFileInfo(path).isSymLink())
        fail(QStringLiteral("Checkpoint publication refuses a symbolic-link target: '%1'.")
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

bool sameRestartState(const RStructuralDynamicsRestartData &left,
                      const RStructuralDynamicsRestartData &right)
{
    return left.available == right.available &&
           left.completedSteps == right.completedSteps &&
           left.time == right.time && left.fixedTimeStep == right.fixedTimeStep &&
           left.meshNodeCount == right.meshNodeCount &&
           left.meshElementCount == right.meshElementCount &&
           left.physicalDofCount == right.physicalDofCount &&
           left.meshFingerprint == right.meshFingerprint &&
           left.dofFingerprint == right.dofFingerprint &&
           left.systemFingerprint == right.systemFingerprint &&
           left.q == right.q && left.v == right.v && left.a == right.a &&
           left.externalForce == right.externalForce &&
           left.cumulativeExternalWork == right.cumulativeExternalWork &&
           left.cumulativeDampingDissipation == right.cumulativeDampingDissipation &&
           left.initialMechanicalEnergy == right.initialMechanicalEnergy &&
           left.supportResponseAvailable == right.supportResponseAvailable &&
           left.supportFingerprint == right.supportFingerprint &&
           left.supportReactions == right.supportReactions &&
           left.supportPowers == right.supportPowers &&
           left.cumulativeSupportWork == right.cumulativeSupportWork &&
           left.controlAvailable == right.controlAvailable &&
           left.controlProtocolVersion == right.controlProtocolVersion &&
           left.controlFingerprint == right.controlFingerprint &&
           left.controlChannelIds == right.controlChannelIds &&
           left.controlLastCommands == right.controlLastCommands &&
           left.controlPowers == right.controlPowers &&
           left.cumulativeControlWork == right.cumulativeControlWork &&
           left.controlControllerState == right.controlControllerState &&
           left.controlDeterministicSeed == right.controlDeterministicSeed;
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
    const RModel &initialModel)
    : checkpointContext(NativeAcceptedCheckpointContext::parse(contextJson))
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

    if (controlSha256 != this->checkpointContext.controlSha256)
        fail(QStringLiteral("Checkpoint context controlSha256 does not match the active controller."));
    if (runtimeSha256 != this->checkpointContext.runtimeSha256)
        fail(QStringLiteral("Checkpoint context runtimeSha256 does not match this solver executable."));

    const RStructuralDynamicsSetup &setup =
        initialModel.getProblemSetup().getStructuralDynamicsSetup();
    if (!setup.getEnabled() || !closeTime(setup.getTimeStep(),this->checkpointContext.dtS))
        fail(QStringLiteral("Checkpoint context dtS does not match enabled structural dynamics."));
    const RStructuralDynamicsRestartData &initialRestart =
        initialModel.getProblemSetup().getStructuralDynamicsRestartData();
    if (initialRestart.available)
    {
        initialRestart.validate();
        if (!closeTime(initialRestart.fixedTimeStep,this->checkpointContext.dtS))
            fail(QStringLiteral("Input accepted checkpoint dt does not match context dtS."));
    }

    const QString lockPath = QDir(this->directory).filePath(
        QStringLiteral(".native-checkpoint.lock"));
    if (QFileInfo(lockPath).isSymLink())
        fail(QStringLiteral("Native checkpoint lock path cannot be a symbolic link."));
    this->writerLock.reset(new QLockFile(lockPath));
    this->writerLock->setStaleLockTime(0);
    if (!this->writerLock->tryLock(0))
        fail(QStringLiteral("Native checkpoint directory is already locked or has a stale writer lock."));

    const QString latestPath = QDir(this->directory).filePath(QStringLiteral("latest.json"));
    const QString lastValidPath = QDir(this->directory).filePath(QStringLiteral("lastvalid.json"));
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
        if (!restart || !initialRestart.available)
            fail(QStringLiteral("Existing checkpoint directories can only continue from their verified latest accepted MODEL."));
        RModel verifiedModel;
        verifiedModel.read(latest.modelPath);
        const RStructuralDynamicsRestartData &verifiedRestart =
            verifiedModel.getProblemSetup().getStructuralDynamicsRestartData();
        if (!sameRestartState(initialRestart,verifiedRestart))
            fail(QStringLiteral("Restart MODEL does not match the existing verified native checkpoint."));
        this->lastPublishedSequence = initialRestart.completedSteps;
        this->lastPublishedTimeS = initialRestart.time;
    }
    else if (initialRestart.available)
    {
        this->lastPublishedSequence = initialRestart.completedSteps;
        this->lastPublishedTimeS = initialRestart.time;
    }
    else
    {
        this->lastPublishedSequence = 0;
        this->lastPublishedTimeS = 0.0;
    }
    this->lastObservedSequence = initialRestart.available
        ? initialRestart.completedSteps : 0;
    this->started = true;
}

NativeAcceptedCheckpointStore::~NativeAcceptedCheckpointStore() = default;

void NativeAcceptedCheckpointStore::accept(const RModel &acceptedModel)
{
    if (!this->started)
        fail(QStringLiteral("Native checkpoint store was not initialized."));
    const RStructuralDynamicsRestartData &accepted =
        acceptedModel.getProblemSetup().getStructuralDynamicsRestartData();
    accepted.validate();
    const RStructuralDynamicsSetup &setup =
        acceptedModel.getProblemSetup().getStructuralDynamicsSetup();
    const RTimeSolver &time = acceptedModel.getTimeSolver();
    if (!accepted.available || accepted.completedSteps == 0 ||
        !setup.getEnabled() || !closeTime(accepted.fixedTimeStep,this->checkpointContext.dtS) ||
        accepted.completedSteps != time.getCurrentTimeStep()+1 ||
        !closeTime(accepted.time,time.getComputedTime()))
        fail(QStringLiteral("Accepted structural checkpoint does not match the committed model clock."));
    if (this->hasObservedStep && accepted.completedSteps != this->lastObservedSequence+1)
        fail(QStringLiteral("Accepted structural checkpoint sequence is not contiguous."));
    if (!std::isfinite(accepted.time) || !std::isfinite(accepted.fixedTimeStep))
        fail(QStringLiteral("Accepted structural checkpoint time is non-finite."));
    this->hasObservedStep = true;
    this->lastObservedSequence = accepted.completedSteps;

    const double elapsed = accepted.time-this->lastPublishedTimeS;
    if (elapsed < 0.0)
        fail(QStringLiteral("Accepted structural checkpoint time moved backwards."));
    const bool stepDue = accepted.completedSteps-this->lastPublishedSequence >=
                         this->checkpointContext.maxAcceptedSteps;
    // Check before the next fixed step would exceed one simulated second so
    // non-divisor dt values remain within the frozen time bound.
    const bool timeDue = elapsed >= this->checkpointContext.maxSimulationSeconds ||
        elapsed+accepted.fixedTimeStep > this->checkpointContext.maxSimulationSeconds;
    const bool finalStep = time.getCurrentTimeStep()+1 >= time.getNTimeSteps();
    if (stepDue || timeDue || finalStep)
    {
        this->publish(acceptedModel,accepted.completedSteps,accepted.time,
                      accepted.fixedTimeStep);
        this->lastPublishedSequence = accepted.completedSteps;
        this->lastPublishedTimeS = accepted.time;
    }
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readPointer(const QString &pointerName,
                                           bool usedLastValid) const
{
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
        !dtValue.isDouble() || !closeTime(dtValue.toDouble(),this->checkpointContext.dtS) ||
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
    const RStructuralDynamicsRestartData &restart =
        checkpointModel.getProblemSetup().getStructuralDynamicsRestartData();
    restart.validate();
    if (!restart.available || restart.completedSteps != quint64(sequenceValue.toDouble()) ||
        !closeTime(restart.time,timeValue.toDouble()) ||
        !closeTime(restart.fixedTimeStep,dtValue.toDouble()))
        fail(QStringLiteral("Checkpoint MODEL does not match its accepted sequence, time or dt."));
    return {modelPath,envelope,usedLastValid};
}

NativeAcceptedCheckpointStore::VerifiedCheckpoint
NativeAcceptedCheckpointStore::readLatest() const
{
    return this->readPointer(QStringLiteral("latest.json"),false);
}

void NativeAcceptedCheckpointStore::publish(const RModel &acceptedModel,
                                             quint64 acceptedSequence,
                                             double acceptedTimeS,
                                             double dtS)
{
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString modelFile = QStringLiteral("checkpoint-%1-%2.rbm")
        .arg(acceptedSequence,10,10,QChar('0')).arg(token);
    const QString modelPath = QDir(this->directory).filePath(modelFile);
    if (QFileInfo::exists(modelPath) || QFileInfo(modelPath).isSymLink())
        fail(QStringLiteral("Refusing to replace an immutable checkpoint MODEL."));

    const QString writtenPath = acceptedModel.write(modelPath,false);
    if (QFileInfo(writtenPath).absoluteFilePath() != QFileInfo(modelPath).absoluteFilePath())
        fail(QStringLiteral("Native checkpoint MODEL was written to an unexpected path."));
    const QString modelHash = sha256File(modelPath);
    RModel readBack;
    readBack.read(modelPath);
    const RStructuralDynamicsRestartData &expected =
        acceptedModel.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsRestartData &actual =
        readBack.getProblemSetup().getStructuralDynamicsRestartData();
    actual.validate();
    if (sha256File(modelPath) != modelHash || !sameRestartState(expected,actual))
        fail(QStringLiteral("Binary checkpoint MODEL read-back differs from the accepted structural state."));

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
    writeAtomic(envelopePath,envelopeBytes);
    const QString envelopeHash = QString::fromLatin1(
        QCryptographicHash::hash(envelopeBytes,QCryptographicHash::Sha256).toHex());
    if (sha256File(envelopePath) != envelopeHash)
        fail(QStringLiteral("Native checkpoint envelope failed post-write hash verification."));

    const VerifiedCheckpoint verifiedEnvelope =
        this->readEnvelope(envelopeFile,envelopeHash,false);
    if (verifiedEnvelope.envelope.value(QStringLiteral("acceptedSeq")).toDouble(-1.0) !=
        double(acceptedSequence))
        fail(QStringLiteral("Native checkpoint envelope read-back has a different accepted sequence."));

    const QJsonObject newPointer = pointerObject(envelopeFile,envelopeHash);
    const QByteArray newPointerBytes = QJsonDocument(newPointer).toJson(QJsonDocument::Compact);
    const QString latestPath = QDir(this->directory).filePath(QStringLiteral("latest.json"));
    const QString lastValidPath = QDir(this->directory).filePath(QStringLiteral("lastvalid.json"));
    if (QFileInfo::exists(latestPath))
    {
        (void)this->readPointer(QStringLiteral("latest.json"),false);
        const QByteArray previousPointer = readFile(latestPath,64*1024);
        writeAtomic(lastValidPath,previousPointer);
        if (readFile(lastValidPath,64*1024) != previousPointer)
            fail(QStringLiteral("Previous valid checkpoint pointer failed read-back verification."));
    }
    else if (QFileInfo::exists(lastValidPath))
    {
        fail(QStringLiteral("Committed latest native checkpoint disappeared during publication."));
    }
    writeAtomic(latestPath,newPointerBytes);
    if (readFile(latestPath,64*1024) != newPointerBytes)
        fail(QStringLiteral("Latest checkpoint pointer failed read-back verification."));
    const VerifiedCheckpoint verified = this->readPointer(QStringLiteral("latest.json"),false);
    if (verified.envelope.value(QStringLiteral("acceptedSeq")).toDouble(-1.0) !=
        double(acceptedSequence))
        fail(QStringLiteral("Latest checkpoint pointer resolved to a different accepted sequence."));
}

const NativeAcceptedCheckpointContext &NativeAcceptedCheckpointStore::context() const
{
    return this->checkpointContext;
}
