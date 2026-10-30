#ifndef SOLVER_TASK_H
#define SOLVER_TASK_H

#include <atomic>
#include <memory>
#include <functional>

#include <QByteArray>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QPair>
#include <QProcess>

#include <rbl_job.h>

#include "application_settings.h"
#include "model.h"
#include "solver_task_id.h"

class SolverTask : public RJob
{

    Q_OBJECT

    public:
        struct TrustedExperimentCandidateTag {};
        //! Monotone cancellation intent shared with queued completion handlers.
        //! The manager gets read-only access; storage outlives this QObject.
        struct CompletionIntent
        {
            std::atomic_bool killRequested{false}, gracefulStopRequested{false};
            bool isGracefulStop() const noexcept
            {
                return gracefulStopRequested.load(std::memory_order_acquire) &&
                       !killRequested.load(std::memory_order_acquire);
            }
        };

    protected:

        //! Application settings.
        const ApplicationSettings *applicationSettings;
        //! Model ID
        uint modelID;
        //! Stable identity captured when the task was created.
        QString sourceDocumentId;
        //! Source identity and revision captured when the task is scheduled.
        QPair<QString, quint64> sourceStamp;
        //! Trusted detached authorization source stamp retained until candidate capture.
        QPair<QString, quint64> trustedSourceStamp;
        //! Original source model path retained for provenance and dependency resolution.
        QString sourceModelFileName;
        //! Solver task ID.
        SolverTaskID taskID;
        //! Solver process.
        QProcess *solverProcess;
        //! The worker owns QProcess; only these monotone request bits are shared.
        const std::shared_ptr<CompletionIntent> completionIntent = std::make_shared<CompletionIntent>();
        //! Solver executable.
        QString solverExecutable;
        //! Solver executable arguments.
        QStringList solverArguments;
        //! Model file name.
        QString modelFileName;
        //! Log file name.
        QString logFileName;
        //! Convergence file name.
        QString convergenceFileName;
        //! Monitoring file name.
        QString monitoringFileName;
        //! Native progress journal path inside this task's private run directory.
        QString nativeProgressFileName;
        //! Actual persisted result-frame journal, separate from solver progress.
        QString nativeResultJournalFileName;
        //! Frozen bytes for the exact initial MODEL opened by a checkpoint-enabled solver.
        QString nativeCheckpointInputModelFileName;
        QByteArray nativeCheckpointModelSha256;
        //! Frozen by the manager before queueing, never inferred from artifacts.
        QJsonObject nativeAcceptedTraceExecutionContext;
        //! Detached model copy captured on the document thread at scheduling.
        std::unique_ptr<Model> inputSnapshot;
        //! Detached result copy owned and updated by the worker.
        std::unique_ptr<Model> resultSnapshot;
        //! Immutable task input file and retained solver result artifact path.
        QString inputSnapshotFileName;
        QString resultArtifactFileName;
        //! SHA-256 of the serialized immutable input model file.
        QByteArray inputSnapshotSha256;
        //! SHA-256 of the immutable input package, including staged references and history.
        QByteArray inputPackageSha256;
        //! Immutable provenance and executable digest captured before queueing.
        QJsonObject nativeResultProvenance;
        QByteArray frozenSolverExecutableSha256;
        bool nativeResultProvenanceFrozen = false;
        //! Provenance assigned by the validated manager before queueing.
        QString inputDerivedFrom;
        //! Service attempt identity paired with this detached experiment child job.
        QString trustedExperimentAttemptId;
        QString frozenSnapshotPath;
        QString frozenSnapshotSha256;
        bool frozenSnapshotVerified = false;
        //! Immutable ancestry and source-frame proof for a detached native recovery attempt.
        QJsonObject nativeRecoveryProvenance;
        //! One-shot trusted experiment binding for a detached candidate that must never publish to Session.
        QJsonObject trustedExperimentProvenance;
        QString nativeRecoveryArtifactRoot;
        //! Cross-process lease for this logical recovery lineage, held while the attempt is active.
        std::shared_ptr<QLockFile> nativeRecoveryLogicalLock;
        //! Durable seed that binds a recovery attempt's initial accepted state to its new checkpoint context.
        QJsonObject nativeCheckpointSeedProvenance;
        //! Guards pre-queue validation copies from a task that has begun execution.
        std::atomic_bool performStarted{false};
        //! Serializes queued execution-control configuration against perform() startup.
        QMutex nativeExecutionConfigurationMutex;
        bool nativeExecutionBoundaryConfigurationRejected = false;
        uint nativeExecutionBoundaryPauseSequence = 0;
        QString nativeExecutionStateFileName;

    public:

        //! Constructor.
        explicit SolverTask(const ApplicationSettings *applicationSettings, uint modelID);

        //! Construct a task from a detached, already-authorized native recovery model.
        //! The source stamp is retained as provenance; no live model ID is rebound.
        SolverTask(const ApplicationSettings *applicationSettings, const Model &model,
                   const QPair<QString, quint64> &sourceStamp,
                   const QJsonObject &recoveryProvenance,
                   const QString &artifactRoot,
                   const QString &taskServerName,
                   std::shared_ptr<QLockFile> logicalRecoveryLock = {});

        //! Construct a solver task for an authorized, detached trusted-experiment candidate.
        //! Completion is retained as an artifact and can never update the live Session model.
        SolverTask(const ApplicationSettings *applicationSettings, const Model &model,
                   const QPair<QString, quint64> &sourceStamp,
                   const QJsonObject &trustedExperimentProvenance,
                   const QString &artifactRoot,
                   const QString &taskServerName,
                   TrustedExperimentCandidateTag);

        //! Capture the input model and its source stamp before the job is queued.
        bool captureInputSnapshot(const Model &model,
                                  const QPair<QString, quint64> &sourceStamp,
                                  uint currentModelID,
                                  const QString &inputDerivedFrom = QString(),
                                  const QString &frozenSnapshotPath = QString(),
                                  const QString &frozenSnapshotSha256 = QString());

        //! Capture the detached model bound to this immutable experiment authorization.
        bool captureTrustedCandidateSnapshot(const Model &model, QString *error = nullptr);

        //! Capture an already detached model copy and source stamp.
        bool captureInputSnapshot(std::unique_ptr<Model> modelSnapshot,
                                  const QPair<QString, quint64> &sourceStamp,
                                  uint currentModelID,
                                  const QString &inputDerivedFrom = QString(),
                                  const QString &frozenSnapshotPath = QString(),
                                  const QString &frozenSnapshotSha256 = QString());

        //! Return a detached copy for pre-queue setup validation.
        std::unique_ptr<Model> copyCapturedInputForValidation() const;

        //! Provenance of the detached captured input.
        const QString &getInputDerivedFrom() const;
        const QString &getFrozenSnapshotPath() const;
        const QString &getFrozenSnapshotSha256() const;
        bool getFrozenSnapshotVerified() const;

        //! Return the pinned ancestry facts for this detached recovery attempt.
        const QJsonObject &getNativeRecoveryProvenance() const;
        const QJsonObject &getTrustedExperimentProvenance() const;
        bool setTrustedExperimentAttemptId(const QString &attemptId);
        const QString &getTrustedExperimentAttemptId() const;
        bool isNativeRecoveryTask() const;
        bool isTrustedDetachedTask() const;
        bool hasNativeRecoveryLogicalLock() const;
        void releaseNativeRecoveryLogicalLock();

        //! Return the stable source identity selected when this task was created.
        const QString &getSourceDocumentId() const;

        //! Return the captured source identity and revision.
        const QPair<QString, quint64> &getSourceStamp() const;

        //! Return the original source model path captured for this task.
        const QString &getSourceModelFileName() const;

        //! Return the immutable input snapshot path.
        const QString &getInputSnapshotFileName() const;

        //! Return the SHA-256 of the serialized immutable input model file.
        const QByteArray &getInputSnapshotSha256() const;

        //! Return the SHA-256 of the complete immutable input package.
        const QByteArray &getInputPackageSha256() const;

        //! Freeze the source and declared-runtime provenance before queueing.
        bool freezeNativeResultProvenance(const QJsonObject &sourceRuntimeStamp,
                                          const QJsonObject &sourceSnapshotRef);

        //! Capture the exact serialized solver input in the immutable input package.
        bool prepareNativeCheckpointInput(QString *error = nullptr);

        const QString &getNativeCheckpointInputModelFileName() const;
        const QByteArray &getNativeCheckpointModelSha256() const;

        //! Enable the bounded accepted-state checkpoint channel before this task is queued.
        bool enableNativeAcceptedCheckpoints(const QString &directory,
                                             const QByteArray &canonicalContextJson,
                                             QString *error = nullptr);

        //! Configure one accepted-step pause before process start, persisting the request through the callback.
        bool requestNativeAcceptedBoundaryPause(
            uint acceptedSequence,
            const std::function<bool(const QString &, QString *)> &persistRequest,
            QString *error = nullptr);
        const QString &getNativeExecutionStateFileName() const;

        //! Return the provenance for a seeded recovery checkpoint, or an empty object for ordinary jobs.
        const QJsonObject &getNativeCheckpointSeedProvenance() const;

        //! Return the immutable native-result provenance frozen before queueing.
        const QJsonObject &getNativeResultProvenance() const;
        bool expectsNativeResultCatalog() const;

        //! Return the retained result artifact path.
        const QString &getResultArtifactFileName() const;

        //! Return the convergence log path.
        const QString &getConvergenceFileName() const;

        //! Return the monitoring log path.
        const QString &getMonitoringFileName() const;

        //! Return the private native progress journal path.
        const QString &getNativeProgressFileName() const;
        const QString &getNativeResultJournalFileName() const;

        //! Return model ID.
        uint getModelID() const;

        //! Refresh the transient list index after verifying the stable source identity.
        bool rebindModelIDForSubmit(uint currentModelID);

        //! Return const reference to solver task ID.
        const SolverTaskID & getTaskID() const;

        //! Return reference to solver task ID.
        SolverTaskID & getTaskID();

        //! Return const reference to solver task ID.
        const QString getStopCommand() const;

        //! Called only after the registered job's cancellation intent is durable.
        void requestGracefulStop() noexcept;
        bool hasGracefulStopRequest() const noexcept;
        std::shared_ptr<const CompletionIntent> getCompletionIntent() const noexcept;

        //! Kill task.
        void kill();

        //! Return log file name.
        const QString & getLogFileName() const;

        //! Return command line.
        QString getCommandLine() const;

    protected:

        void initializeTaskPaths(const Model &model, const QString &artifactRoot = QString(),
                                 const QString &taskServerName = QString(),
                                 bool trustedDetachedTask = false);
        bool seedNativeAcceptedCheckpoint(const QString &directory,
                                          const QJsonObject &checkpointContext,
                                          QString *error);

        //! Run job.
        virtual int perform() override final;

    private slots:

        //! Catch ready read standard output signal.
        void onProcessReadyReadStandardOutput();

        //! Catch ready read standard error signal.
        void onProcessReadyReadStandardError();

    signals:

        //! Message on standard output is availabe.
        void readyReadStandardOutput(QString message);

        //! Message on standard error is availabe.
        void readyReadStandardError(QString message);

        //! A complete task result artifact was read back successfully.
        void resultArtifactCreated(QString fileName, QString sha256, QJsonObject sourceBinding);

        //! The operating system confirmed that this task's native solver process started.
        void nativeProcessStarted(QString jobId, qint64 processId);

        //! A verified immutable manifest descriptor copied to the manager thread.
        void nativeResultManifestPublished(QString jobId, QJsonObject descriptor);

        //! The result broker explicitly classified a completed result as unsupported.
        void nativeResultCatalogUnavailable(QString jobId, QString code, QString message);

        //! Worker-reported terminal evidence copied for the manager thread.
        void terminalReported(QString jobId, QString state, bool processStarted,
                              bool processExited, int exitCode, bool resultReadbackComplete,
                              QString resultArtifactPath, QString resultArtifactSha256,
                              bool resultArtifactVerified, QString message);

        //! A validated native progress event copied for the manager thread.
        void nativeProgressReported(QString jobId, QJsonObject progressEvent);

        //! Native progress journal could not provide trustworthy progress.
        void nativeProgressUnavailable(QString jobId, QString reason);

        //! A malformed, stale, or skipped native progress record was diagnosed.
        void nativeProgressDiagnostic(QString jobId, QString message);

        //! A hash-verified persisted frame, copied to the manager thread.
        void nativeResultFrameReported(QString jobId, QJsonObject frame);
        void nativeResultFramesUnavailable(QString jobId, QString reason);

};

#endif // SOLVER_TASK_H
