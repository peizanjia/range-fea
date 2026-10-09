#include "native_execution_gate.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

#include <cmath>
#include <limits>
#include <stdexcept>

#include <rbl_error.h>

#include "native_accepted_checkpoint.h"

namespace
{
constexpr auto stateSchema = "range.native-execution-state/v1";
constexpr qint64 maximumStateBytes = 16 * 1024;
constexpr quint64 largestExactJsonInteger = 9007199254740991ULL;

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QString::compare(QDir::cleanPath(left),QDir::cleanPath(right),
                            Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(left) == QDir::cleanPath(right);
#endif
}

bool validIdentity(const QString &value)
{
    if (value.isEmpty() || value.size() > 256 || value.trimmed() != value)
        return false;
    for (const QChar character : value)
    {
        if (character.isNull() || character.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

bool isLowerHexSha256(const QString &value)
{
    if (value.size() != 64)
        return false;
    for (const QChar character : value)
    {
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
              (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

QString sha256Bytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        bytes,QCryptographicHash::Sha256).toHex());
}

bool readFileBounded(const QString &fileName, QByteArray &bytes, QString *error)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error) *error = QStringLiteral("Cannot read native execution state: %1")
                                .arg(file.errorString());
        return false;
    }
    if (file.size() < 0 || file.size() > maximumStateBytes)
    {
        if (error) *error = QStringLiteral("Native execution state exceeds its size bound.");
        return false;
    }
    bytes = file.read(maximumStateBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximumStateBytes)
    {
        if (error) *error = QStringLiteral("Cannot read bounded native execution state.");
        return false;
    }
    return true;
}
}

std::shared_ptr<NativeExecutionGate> NativeExecutionGate::create(
    const Config &config, QString *error)
{
    auto fail = [error](const QString &message)
    {
        if (error) *error = message;
        return std::shared_ptr<NativeExecutionGate>();
    };
    if (error) error->clear();

    if (!validIdentity(config.jobId) || !validIdentity(config.taskId))
        return fail(QStringLiteral("Native execution state requires bounded job and task identities."));
    if (config.stateFileName.isEmpty() || !QFileInfo(config.stateFileName).isAbsolute())
        return fail(QStringLiteral("Native execution state path must be absolute."));

    const QFileInfo requestedInfo(QDir::cleanPath(
        QFileInfo(config.stateFileName).absoluteFilePath()));
    if (requestedInfo.fileName().isEmpty())
        return fail(QStringLiteral("Native execution state path must name a file."));
    const QFileInfo parentInfo(requestedInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir() || parentInfo.isSymLink())
        return fail(QStringLiteral("Native execution state parent must be an existing non-symlink directory."));
    const QString absoluteParent = QDir::cleanPath(parentInfo.absoluteFilePath());
    const QString canonicalParent = QDir::cleanPath(parentInfo.canonicalFilePath());
    if (canonicalParent.isEmpty() || !samePath(absoluteParent,canonicalParent))
        return fail(QStringLiteral("Native execution state path must use a canonical parent with no symlinked ancestor."));

    const QString canonicalTarget = QDir(canonicalParent).filePath(requestedInfo.fileName());
    if (!samePath(QDir::cleanPath(requestedInfo.absoluteFilePath()),canonicalTarget))
        return fail(QStringLiteral("Native execution state path must be canonical."));
    const QFileInfo targetInfo(canonicalTarget);
    if (targetInfo.exists() || targetInfo.isSymLink())
        return fail(QStringLiteral("Native execution state target already exists; refusing to overwrite it."));
    if (QFileInfo(canonicalTarget+QStringLiteral(".lock")).isSymLink())
        return fail(QStringLiteral("Native execution state lock path cannot be a symbolic link."));

    NativeAcceptedCheckpointContext checkpointContext;
    try
    {
        checkpointContext = NativeAcceptedCheckpointContext::parse(config.checkpointContextJson);
    }
    catch (const RError &exception)
    {
        return fail(QStringLiteral("Invalid native execution checkpoint context: %1")
                        .arg(exception.getMessage()));
    }
    catch (const std::exception &exception)
    {
        return fail(QStringLiteral("Invalid native execution checkpoint context: %1")
                        .arg(QString::fromUtf8(exception.what())));
    }

    std::shared_ptr<NativeExecutionGate> gate;
    try
    {
        gate.reset(new NativeExecutionGate(config));
    }
    catch (const std::exception &exception)
    {
        return fail(QString::fromUtf8(exception.what()));
    }
    gate->sourceSha256 = checkpointContext.sourceSha256;
    gate->modelSha256 = checkpointContext.modelSha256;
    gate->controlSha256 = checkpointContext.controlSha256;
    gate->runtimeSha256 = checkpointContext.runtimeSha256;
    gate->packageSha256 = checkpointContext.packageSha256;
    gate->contextSha256 = sha256Bytes(config.checkpointContextJson);
    return gate;
}

NativeExecutionGate::NativeExecutionGate(const Config &config)
    : stateFileName(QDir::cleanPath(QFileInfo(config.stateFileName).absoluteFilePath()))
    , jobId(config.jobId)
    , taskId(config.taskId)
    , checkpointContextJson(config.checkpointContextJson)
    , requestedPauseSequence(config.initialPauseSequence)
{
    this->stateLock.reset(new QLockFile(this->stateFileName + QStringLiteral(".lock")));
    this->stateLock->setStaleLockTime(0);
    if (!this->stateLock->tryLock(0))
        throw std::runtime_error("Native execution state path is already locked.");
}

NativeExecutionGate::~NativeExecutionGate() = default;

bool NativeExecutionGate::configureAcceptedRange(quint64 baselineSequence,
                                                  quint64 targetSequence,
                                                  double baselineTimeS,
                                                  QString *error)
{
    auto fail = [error](const QString &message)
    {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();
    if (!std::isfinite(baselineTimeS) || baselineSequence >= targetSequence ||
        targetSequence > largestExactJsonInteger)
        return fail(QStringLiteral("Native execution accepted-step range is invalid."));

    std::lock_guard<std::mutex> locker(this->mutex);
    if (this->configured)
        return fail(QStringLiteral("Native execution accepted-step range was configured more than once."));
    if (this->requestedPauseSequence != 0 &&
        (this->requestedPauseSequence <= baselineSequence ||
         this->requestedPauseSequence > targetSequence))
        return fail(QStringLiteral("Pause sequence must be greater than the current accepted baseline and no greater than the requested target."));

    this->baselineSequence = baselineSequence;
    this->targetSequence = targetSequence;
    this->lastAcceptedSequence = baselineSequence;
    this->baselineTimeS = baselineTimeS;
    this->lastAcceptedTimeS = baselineTimeS;
    this->configured = true;
    return true;
}

bool NativeExecutionGate::isEnabled() const
{
    return true;
}

bool NativeExecutionGate::isCancelled() const
{
    std::lock_guard<std::mutex> locker(this->mutex);
    return this->cancellationRequested || this->runState == RunState::Cancelled ||
           this->runState == RunState::Failed;
}

bool NativeExecutionGate::hasPauseAt(quint64 acceptedSequence) const
{
    std::lock_guard<std::mutex> locker(this->mutex);
    return this->requestedPauseSequence == acceptedSequence;
}

bool NativeExecutionGate::acceptedBoundary(quint64 acceptedSequence,
                                           double timeS,
                                           const CheckpointReceipt &latestCheckpoint,
                                           QString *error)
{
    if (error) error->clear();
    std::unique_lock<std::mutex> locker(this->mutex);
    auto fail = [this,error](const QString &message)
    {
        if (error) *error = message;
        this->failLocked(message);
        return false;
    };

    if (!this->configured)
        return fail(QStringLiteral("Native execution gate reached an accepted step before its range was configured."));
    if (!std::isfinite(timeS) || timeS <= this->lastAcceptedTimeS ||
        acceptedSequence != this->lastAcceptedSequence + 1 ||
        acceptedSequence > this->targetSequence)
        return fail(QStringLiteral("Native execution accepted boundary is non-contiguous or outside the configured range."));
    if (latestCheckpoint.available &&
        (latestCheckpoint.acceptedSequence == 0 ||
         latestCheckpoint.acceptedSequence > acceptedSequence ||
         !std::isfinite(latestCheckpoint.timeS) ||
         !isLowerHexSha256(latestCheckpoint.envelopeSha256) ||
         !isLowerHexSha256(latestCheckpoint.modelSha256)))
        return fail(QStringLiteral("Native execution latest checkpoint receipt is invalid."));

    this->lastAcceptedSequence = acceptedSequence;
    this->lastAcceptedTimeS = timeS;
    this->latestCheckpoint = latestCheckpoint;
    this->hasAcceptedBoundary = true;

    if (this->runState == RunState::Failed)
    {
        if (error) *error = this->failureReason;
        return false;
    }
    if (this->runState == RunState::Cancelled)
        return false;

    if (this->cancellationRequested)
    {
        if (!this->writeStateLocked(QStringLiteral("cancelled"),this->lastPauseSequence))
        {
            if (error) *error = this->failureReason;
            return false;
        }
        this->runState = RunState::Cancelled;
        this->condition.notify_all();
        return false;
    }

    if (this->requestedPauseSequence == acceptedSequence)
    {
        this->lastPauseSequence = acceptedSequence;
        this->requestedPauseSequence = 0;
        if (!this->writeStateLocked(QStringLiteral("paused"),this->lastPauseSequence))
        {
            if (error) *error = this->failureReason;
            return false;
        }
        this->runState = RunState::Paused;
        this->condition.wait(locker,[this]
        {
            return this->runState != RunState::Paused;
        });
        if (this->runState == RunState::Failed)
        {
            if (error) *error = this->failureReason;
            return false;
        }
        return this->runState != RunState::Cancelled;
    }
    return true;
}

bool NativeExecutionGate::requestResume()
{
    std::lock_guard<std::mutex> locker(this->mutex);
    if (this->runState != RunState::Paused)
        return false;
    if (!this->writeStateLocked(QStringLiteral("resumed"),this->lastPauseSequence))
    {
        this->condition.notify_all();
        return false;
    }
    this->runState = RunState::Armed;
    this->condition.notify_all();
    return true;
}

bool NativeExecutionGate::requestPauseAt(quint64 acceptedSequence)
{
    std::lock_guard<std::mutex> locker(this->mutex);
    if (!this->configured || this->runState != RunState::Armed ||
        this->cancellationRequested ||
        acceptedSequence <= this->lastAcceptedSequence ||
        acceptedSequence > this->targetSequence)
        return false;
    this->requestedPauseSequence = acceptedSequence;
    return true;
}

bool NativeExecutionGate::requestCancel()
{
    std::lock_guard<std::mutex> locker(this->mutex);
    if (this->runState == RunState::Cancelled)
        return true;
    if (this->runState == RunState::Failed || this->runState == RunState::Finished)
    {
        return false;
    }
    if (this->cancellationRequested)
        return true;
    this->requestedPauseSequence = 0;
    this->cancellationRequested = true;
    if (this->runState == RunState::Paused)
    {
        if (!this->writeStateLocked(QStringLiteral("cancelled"),this->lastPauseSequence))
        {
            this->condition.notify_all();
            return false;
        }
        this->runState = RunState::Cancelled;
    }
    else if (this->hasAcceptedBoundary &&
             !this->writeStateLocked(QStringLiteral("cancel_requested"),
                                     this->lastPauseSequence))
    {
        this->condition.notify_all();
        return false;
    }
    this->condition.notify_all();
    return true;
}

bool NativeExecutionGate::requestPauseNext()
{
    std::lock_guard<std::mutex> locker(this->mutex);
    if (!this->configured || this->runState != RunState::Armed ||
        this->cancellationRequested || this->lastAcceptedSequence >= this->targetSequence)
        return false;
    this->requestedPauseSequence = this->lastAcceptedSequence + 1;
    return true;
}

bool NativeExecutionGate::markCompleted(QString *error)
{
    if (error) error->clear();
    std::lock_guard<std::mutex> locker(this->mutex);
    if (this->runState == RunState::Armed)
    {
        this->requestedPauseSequence = 0;
        if (this->cancellationRequested && this->hasAcceptedBoundary)
        {
            if (!this->writeStateLocked(QStringLiteral("cancelled"),
                                        this->lastPauseSequence))
            {
                if (error) *error = this->failureReason;
                this->condition.notify_all();
                return false;
            }
            this->runState = RunState::Cancelled;
        }
        else
        {
            this->cancellationRequested = false;
            this->runState = RunState::Finished;
        }
    }
    this->condition.notify_all();
    return this->runState != RunState::Failed;
}

QString NativeExecutionGate::lastError() const
{
    std::lock_guard<std::mutex> locker(this->mutex);
    return this->failureReason;
}

bool NativeExecutionGate::writeStateLocked(const QString &state, quint64 pauseSequence)
{
    if (this->generation >= largestExactJsonInteger)
        return this->failLocked(QStringLiteral("Native execution state generation exceeded the exact JSON integer bound."));
    if (!this->hasAcceptedBoundary || this->lastAcceptedSequence < this->baselineSequence)
        return this->failLocked(QStringLiteral("Native execution state cannot publish before an accepted boundary."));

    const quint64 nextGeneration = this->generation + 1;
    QJsonObject stateObject{
        {QStringLiteral("schema"),QString::fromLatin1(stateSchema)},
        {QStringLiteral("generation"),double(nextGeneration)},
        {QStringLiteral("state"),state},
        {QStringLiteral("jobId"),this->jobId},
        {QStringLiteral("taskId"),this->taskId},
        {QStringLiteral("pauseAtAcceptedSeq"),double(pauseSequence)},
        {QStringLiteral("acceptedSeq"),double(this->lastAcceptedSequence)},
        {QStringLiteral("timeS"),this->lastAcceptedTimeS},
        {QStringLiteral("sourceSha256"),this->sourceSha256},
        {QStringLiteral("modelSha256"),this->modelSha256},
        {QStringLiteral("controlSha256"),this->controlSha256},
        {QStringLiteral("runtimeSha256"),this->runtimeSha256},
        {QStringLiteral("packageSha256"),this->packageSha256},
        {QStringLiteral("contextSha256"),this->contextSha256},
        {QStringLiteral("checkpointAvailable"),this->latestCheckpoint.available},
        {QStringLiteral("checkpointAcceptedSeq"),
         double(this->latestCheckpoint.available ? this->latestCheckpoint.acceptedSequence : 0)},
        {QStringLiteral("checkpointTimeS"),
         this->latestCheckpoint.available ? this->latestCheckpoint.timeS : 0.0},
        {QStringLiteral("checkpointEnvelopeSha256"),this->latestCheckpoint.envelopeSha256},
        {QStringLiteral("checkpointModelSha256"),this->latestCheckpoint.modelSha256}
    };
    const QByteArray bytes = QJsonDocument(stateObject).toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() || bytes.size() > maximumStateBytes)
        return this->failLocked(QStringLiteral("Native execution state exceeded its bounded JSON size."));

    QString targetError;
    if (!this->verifyTargetBeforeWriteLocked(this->lastOwnedBytes.isEmpty(),&targetError))
        return this->failLocked(targetError);

    QSaveFile file(this->stateFileName);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return this->failLocked(QStringLiteral("Cannot open atomic native execution state: %1")
                                    .arg(file.errorString()));
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!file.setPermissions(privatePermissions))
    {
        const QString reason = QStringLiteral("Cannot restrict native execution state permissions: %1")
                                   .arg(file.errorString());
        file.cancelWriting();
        return this->failLocked(reason);
    }
    if (file.write(bytes) != bytes.size() || !file.flush())
    {
        const QString reason = QStringLiteral("Cannot write atomic native execution state: %1")
                                   .arg(file.errorString());
        file.cancelWriting();
        return this->failLocked(reason);
    }

    // Refuse to replace content changed after the preflight read. The private
    // caller directory and per-run lock constrain the remaining rename window.
    if (!this->verifyTargetBeforeWriteLocked(this->lastOwnedBytes.isEmpty(),&targetError))
    {
        file.cancelWriting();
        return this->failLocked(targetError);
    }
    if (!file.commit())
        return this->failLocked(QStringLiteral("Cannot atomically commit native execution state: %1")
                                    .arg(file.errorString()));

    const QFileInfo committedInfo(this->stateFileName);
    const QString committedCanonical = QDir::cleanPath(committedInfo.canonicalFilePath());
    const QString expectedCanonical = QDir::cleanPath(this->stateFileName);
    QByteArray committedBytes;
    if (!committedInfo.exists() || !committedInfo.isFile() || committedInfo.isSymLink() ||
        committedCanonical.isEmpty() || !samePath(committedCanonical,expectedCanonical) ||
        !readFileBounded(this->stateFileName,committedBytes,&targetError) ||
        committedBytes != bytes)
        return this->failLocked(targetError.isEmpty()
            ? QStringLiteral("Native execution state changed during atomic publication.")
            : targetError);

    this->generation = nextGeneration;
    this->lastAcknowledgedSequence = this->lastAcceptedSequence;
    this->lastOwnedBytes = bytes;
    return true;
}

bool NativeExecutionGate::verifyTargetBeforeWriteLocked(bool firstWrite,
                                                         QString *error) const
{
    const QFileInfo info(this->stateFileName);
    if (info.isSymLink())
    {
        if (error) *error = QStringLiteral("Native execution state target became a symbolic link.");
        return false;
    }
    const QFileInfo parentInfo(info.absolutePath());
    const QString absoluteParent = QDir::cleanPath(parentInfo.absoluteFilePath());
    const QString canonicalParent = QDir::cleanPath(parentInfo.canonicalFilePath());
    if (!parentInfo.exists() || !parentInfo.isDir() || canonicalParent.isEmpty() ||
        !samePath(absoluteParent,canonicalParent) ||
        !samePath(QDir::cleanPath(info.absoluteFilePath()),
                  QDir(canonicalParent).filePath(info.fileName())))
    {
        if (error) *error = QStringLiteral("Native execution state path is no longer canonical.");
        return false;
    }
    if (firstWrite)
    {
        if (info.exists())
        {
            if (error) *error = QStringLiteral("Native execution state target appeared before its first ACK.");
            return false;
        }
        return true;
    }
    if (!info.exists() || !info.isFile())
    {
        if (error) *error = QStringLiteral("Native execution state target disappeared or changed type.");
        return false;
    }
    QByteArray existingBytes;
    if (!readFileBounded(this->stateFileName,existingBytes,error))
        return false;
    if (existingBytes != this->lastOwnedBytes)
    {
        if (error) *error = QStringLiteral("Native execution state content changed outside this solver run.");
        return false;
    }
    return true;
}

bool NativeExecutionGate::failLocked(const QString &error)
{
    this->failureReason = error;
    this->runState = RunState::Failed;
    this->condition.notify_all();
    return false;
}
