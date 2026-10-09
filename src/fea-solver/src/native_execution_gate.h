#ifndef NATIVE_EXECUTION_GATE_H
#define NATIVE_EXECUTION_GATE_H

#include <QByteArray>
#include <QString>

#include <condition_variable>
#include <memory>
#include <mutex>

class QLockFile;

//! Per-run accepted-boundary pause/resume gate shared by the solver worker and
//! its local IPC command processor.
class NativeExecutionGate final
{
public:
    struct Config
    {
        QString stateFileName;
        QString jobId;
        QString taskId;
        quint64 initialPauseSequence = 0;
        QByteArray checkpointContextJson;
    };

    struct CheckpointReceipt
    {
        bool available = false;
        quint64 acceptedSequence = 0;
        double timeS = 0.0;
        QString envelopeSha256;
        QString modelSha256;
    };

    static std::shared_ptr<NativeExecutionGate> create(const Config &config,
                                                        QString *error = nullptr);
    ~NativeExecutionGate();

    NativeExecutionGate(const NativeExecutionGate &) = delete;
    NativeExecutionGate &operator=(const NativeExecutionGate &) = delete;

    //! Bind the public pause request to the model's immutable accepted-step range.
    bool configureAcceptedRange(quint64 baselineSequence,
                                quint64 targetSequence,
                                double baselineTimeS,
                                QString *error = nullptr);

    bool isEnabled() const;
    bool isCancelled() const;
    bool hasPauseAt(quint64 acceptedSequence) const;

    //! Called only after the complete accepted transaction and checkpoint observer.
    //! Blocks the solver worker at the requested boundary until resume/cancel.
    bool acceptedBoundary(quint64 acceptedSequence,
                          double timeS,
                          const CheckpointReceipt &latestCheckpoint,
                          QString *error = nullptr);

    //! Commands are accepted only for the current run and current pause request.
    bool requestResume();
    bool requestPauseAt(quint64 acceptedSequence);
    bool requestCancel();
    bool markCompleted(QString *error = nullptr);

    QString lastError() const;

private:
    enum class RunState
    {
        Armed,
        Paused,
        Cancelled,
        Finished,
        Failed
    };

    explicit NativeExecutionGate(const Config &config);

    bool writeStateLocked(const QString &state, quint64 pauseSequence);
    bool verifyTargetBeforeWriteLocked(bool firstWrite, QString *error) const;
    bool failLocked(const QString &error);

    QString stateFileName;
    QString jobId;
    QString taskId;
    QByteArray checkpointContextJson;
    QString sourceSha256;
    QString modelSha256;
    QString controlSha256;
    QString runtimeSha256;
    QString packageSha256;
    QString contextSha256;
    std::unique_ptr<QLockFile> stateLock;

    mutable std::mutex mutex;
    std::condition_variable condition;
    RunState runState = RunState::Armed;
    quint64 requestedPauseSequence = 0;
    quint64 lastPauseSequence = 0;
    quint64 baselineSequence = 0;
    quint64 targetSequence = 0;
    quint64 lastAcceptedSequence = 0;
    quint64 lastAcknowledgedSequence = 0;
    quint64 generation = 0;
    double baselineTimeS = 0.0;
    double lastAcceptedTimeS = 0.0;
    CheckpointReceipt latestCheckpoint;
    QByteArray lastOwnedBytes;
    QString failureReason;
    bool hasAcceptedBoundary = false;
    bool cancellationRequested = false;
    bool configured = false;
};

#endif // NATIVE_EXECUTION_GATE_H
