#include "native_progress_journal.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QIODevice>

#include <atomic>
#include <cmath>
#include <mutex>

namespace
{
constexpr qint64 maximumJournalBytes = 16 * 1024 * 1024;
constexpr quint64 largestExactJsonInteger = (quint64(1) << 53) - 1;
constexpr int maximumMessageCharacters = 256;

std::atomic<NativeProgressJournal *> activeJournal{nullptr};
std::mutex progressScopeMutex;

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return QString::compare(QDir::cleanPath(left),QDir::cleanPath(right),
                            Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(left) == QDir::cleanPath(right);
#endif
}

QString boundedMessage(const QString &message)
{
    if (message.size() <= maximumMessageCharacters)
    {
        return message;
    }
    return message.left(maximumMessageCharacters);
}

void progressPrint(double fraction)
{
    if (NativeProgressJournal *journal = activeJournal.load(std::memory_order_acquire))
    {
        journal->fraction(fraction);
    }
}

void progressInitialize(const QString &message, bool pulse)
{
    if (NativeProgressJournal *journal = activeJournal.load(std::memory_order_acquire))
    {
        journal->phaseStarted(message,pulse);
    }
}

void progressFinalize(const QString &message)
{
    if (NativeProgressJournal *journal = activeJournal.load(std::memory_order_acquire))
    {
        journal->phaseFinished(message);
    }
}
}

NativeProgressJournal::NativeProgressJournal(const QString &jobId, const QString &fileName)
    : jobId(jobId)
    , file(fileName)
{
}

std::unique_ptr<NativeProgressJournal> NativeProgressJournal::createFromEnvironment(QString *error)
{
    const QString jobId = qEnvironmentVariable(jobIdEnvironmentVariable);
    const QString fileName = qEnvironmentVariable(pathEnvironmentVariable);
    if (jobId.isEmpty() && fileName.isEmpty())
    {
        return nullptr;
    }
    if (jobId.isEmpty() || fileName.isEmpty())
    {
        if (error)
        {
            *error = QStringLiteral("Both RANGE_NATIVE_JOB_ID and RANGE_NATIVE_PROGRESS_PATH are required");
        }
        return nullptr;
    }
    return create(jobId,fileName,error);
}

std::unique_ptr<NativeProgressJournal> NativeProgressJournal::create(const QString &jobId,
                                                                      const QString &fileName,
                                                                      QString *error)
{
    auto fail = [error](const QString &message)
    {
        if (error)
        {
            *error = message;
        }
        return std::unique_ptr<NativeProgressJournal>();
    };

    if (error)
    {
        error->clear();
    }
    if (jobId.trimmed().isEmpty() || jobId.size() > maximumMessageCharacters)
    {
        return fail(QStringLiteral("Native progress job ID is empty or too long"));
    }
    const QFileInfo requestedInfo(fileName);
    if (fileName.isEmpty() || !requestedInfo.isAbsolute())
    {
        return fail(QStringLiteral("Native progress path must be absolute"));
    }

    const QString cleanedPath = QDir::cleanPath(requestedInfo.absoluteFilePath());
    const QFileInfo cleanedInfo(cleanedPath);
    if (cleanedInfo.fileName().isEmpty())
    {
        return fail(QStringLiteral("Native progress path must name a file"));
    }
    const QFileInfo parentInfo(cleanedInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir() || parentInfo.isSymLink())
    {
        return fail(QStringLiteral("Native progress parent must be an existing non-symlink directory"));
    }
    const QString absoluteParent = QDir::cleanPath(parentInfo.absoluteFilePath());
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty() || !samePath(canonicalParent,absoluteParent))
    {
        return fail(QStringLiteral("Native progress parent must use its canonical path and contain no symlinked ancestor"));
    }

    const QString canonicalTarget = QDir(canonicalParent).filePath(cleanedInfo.fileName());
    const QFileInfo targetInfo(canonicalTarget);
    if (targetInfo.exists() || targetInfo.isSymLink())
    {
        return fail(QStringLiteral("Native progress target already exists; refusing to overwrite it"));
    }

    std::unique_ptr<NativeProgressJournal> journal(
        new NativeProgressJournal(jobId,canonicalTarget));
    if (!journal->file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return fail(QStringLiteral("Cannot create native progress file '%1': %2")
                        .arg(canonicalTarget,journal->file.errorString()));
    }
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!journal->file.setPermissions(privatePermissions))
    {
        const QString reason = QStringLiteral("Cannot restrict native progress file permissions: %1")
                                   .arg(journal->file.errorString());
        journal->file.close();
        return fail(reason);
    }
    journal->available = true;
    return journal;
}

bool NativeProgressJournal::isAvailable() const
{
    QMutexLocker locker(&this->mutex);
    return this->available;
}

QString NativeProgressJournal::unavailableReason() const
{
    QMutexLocker locker(&this->mutex);
    return this->failureReason;
}

void NativeProgressJournal::phaseStarted(const QString &message, bool pulse)
{
    QMutexLocker locker(&this->mutex);
    if (!this->available)
    {
        return;
    }
    if (this->phaseId >= largestExactJsonInteger)
    {
        this->disableLocked(QStringLiteral("Native progress phase ID exceeded the exact JSON integer limit"));
        return;
    }
    ++this->phaseId;
    this->currentMessage = boundedMessage(message);
    this->currentPulse = pulse;
    this->hasCurrentPhase = true;
    this->lastFractionBucket = 0;
    this->pulseCallbacks = 0;
    this->appendLocked(QStringLiteral("phase_started"),this->currentMessage,
                       pulse,!pulse,0.0);
}

void NativeProgressJournal::fraction(double value)
{
    QMutexLocker locker(&this->mutex);
    if (!this->available || !this->hasCurrentPhase)
    {
        return;
    }
    // RProgress::initialize calls Print(0) immediately before its initialize
    // callback. The phase_started record represents this baseline and avoids
    // attributing that callback to the phase that just ended.
    if (value == 0.0)
    {
        return;
    }
    if (this->currentPulse)
    {
        // A pulse callback carries no quantitative progress. Sample every 64th
        // source callback so long-running kernels cannot flood the side log.
        ++this->pulseCallbacks;
        if (this->pulseCallbacks == 1 || this->pulseCallbacks % 64 == 0)
        {
            this->appendLocked(QStringLiteral("fraction"),this->currentMessage,
                               true,false,0.0);
        }
        return;
    }
    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
    {
        this->disableLocked(QStringLiteral("Native progress callback returned a non-finite or out-of-range fraction"));
        return;
    }
    const int bucket = int(std::floor(value * 100.0));
    if (bucket <= this->lastFractionBucket)
    {
        return;
    }
    if (this->appendLocked(QStringLiteral("fraction"),this->currentMessage,
                           false,true,value))
    {
        this->lastFractionBucket = bucket;
    }
}

void NativeProgressJournal::phaseFinished(const QString &message)
{
    QMutexLocker locker(&this->mutex);
    if (!this->available || !this->hasCurrentPhase)
    {
        return;
    }
    const QString finishMessage = message.isEmpty() ? this->currentMessage
                                                     : boundedMessage(message);
    this->appendLocked(QStringLiteral("phase_finished"),finishMessage,
                       this->currentPulse,!this->currentPulse,1.0);
    this->hasCurrentPhase = false;
}

bool NativeProgressJournal::appendLocked(const QString &kind, const QString &message,
                                         bool pulse, bool hasFraction, double fraction)
{
    if (!this->available)
    {
        return false;
    }
    if (this->sequence > largestExactJsonInteger)
    {
        this->disableLocked(QStringLiteral("Native progress sequence exceeded the exact JSON integer limit"));
        return false;
    }
    QJsonObject record{
        {QStringLiteral("protocolVersion"),QString::fromLatin1(protocolVersion)},
        {QStringLiteral("jobId"),this->jobId},
        {QStringLiteral("seq"),double(this->sequence)},
        {QStringLiteral("phaseId"),double(this->phaseId)},
        {QStringLiteral("kind"),kind},
        {QStringLiteral("message"),message},
        {QStringLiteral("pulse"),pulse},
        {QStringLiteral("fraction"),hasFraction ? QJsonValue(fraction)
                                                   : QJsonValue(QJsonValue::Null)}};
    QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (line.size() > maximumJournalBytes - this->bytesWritten)
    {
        this->disableLocked(QStringLiteral("Native progress journal reached its 16 MiB size limit"));
        return false;
    }

    qint64 offset = 0;
    while (offset < line.size())
    {
        const qint64 written = this->file.write(line.constData() + offset,line.size() - offset);
        if (written <= 0)
        {
            this->disableLocked(QStringLiteral("Cannot append native progress file: %1")
                                    .arg(this->file.errorString()));
            return false;
        }
        offset += written;
    }
    if (!this->file.flush())
    {
        this->disableLocked(QStringLiteral("Cannot flush native progress file: %1")
                                .arg(this->file.errorString()));
        return false;
    }
    this->bytesWritten += line.size();
    ++this->sequence;
    return true;
}

void NativeProgressJournal::disableLocked(const QString &reason)
{
    this->available = false;
    this->failureReason = reason;
}

NativeProgressHandlerScope::NativeProgressHandlerScope(NativeProgressJournal *journal)
{
    if (!journal || !journal->isAvailable())
    {
        return;
    }
    this->scopeLock = std::unique_lock<std::mutex>(progressScopeMutex);
    RProgress &progress = RProgress::getInstance();
    this->previousPrintHandler = progress.getProgressPrintHandler();
    this->previousInitializeHandler = progress.getProgressInitializeHandler();
    this->previousFinalizeHandler = progress.getProgressFinalizeHandler();
    progress.setProgressPrintHandler(progressPrint);
    progress.setProgressInitializeHandler(progressInitialize);
    progress.setProgressFinalizeHandler(progressFinalize);
    activeJournal.store(journal,std::memory_order_release);
    this->installed = true;
}

NativeProgressHandlerScope::~NativeProgressHandlerScope()
{
    if (!this->installed)
    {
        return;
    }
    activeJournal.store(nullptr,std::memory_order_release);
    RProgress &progress = RProgress::getInstance();
    progress.setProgressPrintHandler(this->previousPrintHandler);
    progress.setProgressInitializeHandler(this->previousInitializeHandler);
    progress.setProgressFinalizeHandler(this->previousFinalizeHandler);
}
