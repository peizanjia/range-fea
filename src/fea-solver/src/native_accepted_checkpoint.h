#ifndef NATIVE_ACCEPTED_CHECKPOINT_H
#define NATIVE_ACCEPTED_CHECKPOINT_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <exception>
#include <memory>

#include <raccepted_step_publication.h>

class QLockFile;
class RModel;

//! Exact accepted restart identity; the extractor validates the complete
//! mode-specific restart state and fingerprints its binary64 representation.
struct NativeAcceptedCheckpointStateView
{
    quint64 sequence = 0;
    double timeS = 0.0;
    double dtS = 0.0;
    QByteArray stateFingerprint32;
};

//! Bounded, provenance-bound context supplied by the native workflow host.
struct NativeAcceptedCheckpointContext
{
    QString sourceSha256;
    QString modelSha256;
    QString controlSha256;
    QString runtimeSha256;
    QString packageSha256;
    QString policy;
    uint maxAcceptedSteps = 0;
    double maxSimulationSeconds = 0.0;
    double dtS = 0.0;

    static NativeAcceptedCheckpointContext parse(const QByteArray &canonicalJson);
    static QString noControlSha256();
    QJsonObject toJsonObject() const;
};

//! Writes immutable binary accepted-state MODELs and verified envelopes.
//! latest.json is an atomically replaced pointer; lastvalid.json keeps the
//! previous verified pointer for diagnosis, never for automatic restart.
class NativeAcceptedCheckpointStore final : public RAcceptedStepParticipant
{
public:
    using StateViewExtractor = std::function<NativeAcceptedCheckpointStateView(const RModel &)>;
    enum class Phase
    {
        BeforeModelWrite, AfterModelWrite, AfterModelReadBack,
        BeforeEnvelopeWrite, AfterEnvelopeWrite, AfterEnvelopeReadBack,
        BeforeLastValid, AfterLastValid, BeforeLatest, AfterLatest,
        AfterLatestReadBack, BeforeRollbackLatest, AfterRollbackLatest,
        BeforeRollbackLastValid, AfterRollbackLastValid,
        BeforeEvidenceWrite, AfterEvidenceReadBack,
        BeforeMarkerWrite, AfterMarkerWrite, BeforeMarkerRemove
    };
    using PhaseHook = std::function<void(Phase)>;
    struct VerifiedCheckpoint
    {
        QString modelPath;
        QJsonObject envelope;
        bool usedLastValid = false;
    };

    NativeAcceptedCheckpointStore(const QString &directory,
                                  const QByteArray &contextJson,
                                  const QString &inputModelSha256,
                                  const QString &controlSha256,
                                  const QString &runtimeSha256,
                                  bool restart,
                                  const RModel &initialModel,
                                  StateViewExtractor extractor = {});
    ~NativeAcceptedCheckpointStore();

    NativeAcceptedCheckpointStore(const NativeAcceptedCheckpointStore &) = delete;
    NativeAcceptedCheckpointStore &operator=(const NativeAcceptedCheckpointStore &) = delete;

    //! Called after every fully committed structural step. A due write failure
    //! throws, stopping the solver before it can accept another step.
    void accept(const RModel &acceptedModel);

    //! finalStep forces publication at the last accepted step of this invocation;
    //! it may precede the unchanged physical time-grid end. The physical final
    //! step must set finalStep, and no candidate may exceed the original grid.
    std::unique_ptr<RAcceptedStepPublication> stage(
        const RModel &candidateModel, const RAcceptedStepIdentity &identity) override;

    //! A due checkpoint must be the host's final publication. Successful
    //! pending-marker removal is its durable decision point; only noexcept
    //! physical/participant promotion may follow, then postcommit observers.
    //! Durable abort/reopen protection covers failed publication; additional
    //! rollback after successful publish is a separate compensation attempt.

    bool isRetryable() const noexcept;
    const VerifiedCheckpoint *committedReceipt() const noexcept;
    //! One invocation at the selected phase, cleared before invoking the hook.
    void setPhaseHook(Phase phase, PhaseHook hook);
    QString rollbackDiagnostic() const;
    //! Diagnostic artifacts only: never scanned or used as restart authority.
    const QStringList &rollbackEvidencePaths() const noexcept;

    //! Resolve committed latest; active or unresolved publication fails closed.
    VerifiedCheckpoint readLatest() const;

    const NativeAcceptedCheckpointContext &context() const;

private:
    struct PublicationState;
    class Publication;
    QString directory;
    NativeAcceptedCheckpointContext checkpointContext;
    std::unique_ptr<QLockFile> writerLock;
    quint64 lastObservedSequence = 0;
    quint64 lastPublishedSequence = 0;
    double lastPublishedTimeS = 0.0;
    bool started = false;
    bool hasObservedStep = false;
    bool poisoned = false;
    StateViewExtractor stateViewExtractor;
    Publication *activePublication = nullptr;
    std::unique_ptr<VerifiedCheckpoint> receipt;
    std::unique_ptr<PublicationState> failedRollbackState;
    std::exception_ptr rollbackFailure;
    QStringList failedRollbackEvidencePaths;
    Phase hookPhase = Phase::BeforeModelWrite;
    PhaseHook phaseHook;

    VerifiedCheckpoint readPointer(const QString &pointerName,
                                  bool usedLastValid) const;
    VerifiedCheckpoint readEnvelope(const QString &envelopeFile,
                                    const QString &envelopeHash,
                                    bool usedLastValid) const;
    std::unique_ptr<RAcceptedStepPublication> stageImpl(
        const RModel &candidateModel, const RAcceptedStepIdentity &identity,
        bool allowLegacyFirstCallback);
    void invokePhase(Phase phase);
};

#endif // NATIVE_ACCEPTED_CHECKPOINT_H
