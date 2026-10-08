#ifndef NATIVE_PROGRESS_JOURNAL_H
#define NATIVE_PROGRESS_JOURNAL_H

#include <QFile>
#include <QMutex>
#include <QString>

#include <rbl_progress.h>

#include <memory>
#include <mutex>

class NativeProgressJournal final
{
public:
    static constexpr const char *protocolVersion = "range.native-progress/v1";
    static constexpr const char *jobIdEnvironmentVariable = "RANGE_NATIVE_JOB_ID";
    static constexpr const char *pathEnvironmentVariable = "RANGE_NATIVE_PROGRESS_PATH";

    NativeProgressJournal(const NativeProgressJournal &) = delete;
    NativeProgressJournal &operator=(const NativeProgressJournal &) = delete;
    ~NativeProgressJournal() = default;

    //! Returns null without an error when neither trusted-parent variable is set.
    //! A partial environment handshake or invalid path returns an error.
    static std::unique_ptr<NativeProgressJournal> createFromEnvironment(QString *error = nullptr);

    //! Opens a new, private JSONL file after validating its parent directory.
    static std::unique_ptr<NativeProgressJournal> create(const QString &jobId,
                                                          const QString &fileName,
                                                          QString *error = nullptr);

    bool isAvailable() const;
    QString unavailableReason() const;

    void phaseStarted(const QString &message, bool pulse);
    void fraction(double value);
    void phaseFinished(const QString &message);

private:
    NativeProgressJournal(const QString &jobId, const QString &fileName);

    bool appendLocked(const QString &kind, const QString &message,
                      bool pulse, bool hasFraction, double fraction);
    void disableLocked(const QString &reason);

    const QString jobId;
    mutable QMutex mutex;
    QFile file;
    quint64 sequence = 1;
    quint64 phaseId = 0;
    qint64 bytesWritten = 0;
    int lastFractionBucket = 0;
    quint64 pulseCallbacks = 0;
    QString currentMessage;
    QString failureReason;
    bool currentPulse = false;
    bool hasCurrentPhase = false;
    bool available = false;
};

//! Temporarily binds RProgress's process-global callbacks to one journal.
//! It is active only for an available journal and always restores prior handlers.
class NativeProgressHandlerScope final
{
public:
    explicit NativeProgressHandlerScope(NativeProgressJournal *journal);
    ~NativeProgressHandlerScope();

    NativeProgressHandlerScope(const NativeProgressHandlerScope &) = delete;
    NativeProgressHandlerScope &operator=(const NativeProgressHandlerScope &) = delete;

private:
    std::unique_lock<std::mutex> scopeLock;
    RProgressPrintHandler previousPrintHandler = nullptr;
    RProgressInitializeHandler previousInitializeHandler = nullptr;
    RProgressFinalizeHandler previousFinalizeHandler = nullptr;
    bool installed = false;
};

#endif // NATIVE_PROGRESS_JOURNAL_H
