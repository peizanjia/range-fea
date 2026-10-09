#ifndef RUNTIME_PARAMETER_GATE_H
#define RUNTIME_PARAMETER_GATE_H

#include "rstructural_control_runtime_parameters.h"
#include <mutex>
#include <optional>
#include <vector>

// The solver owns force feasibility and commit; the live bridge only queues.
class RuntimeParameterGate
{
public:
    using Parameters = RStructuralControlRuntimeParameters::Parameters;
    using State = RStructuralControlRuntimeParameters::State;
    using Limit = RStructuralControlRuntimeParameters::ActuatorLimit;
    struct Identity
    {
        QString jobId, runId, taskId, modelSha256, baseControllerRevision;
    };
    struct Request
    {
        Identity identity;
        QString proposalId;
        quint64 expectedParameterRevision = 0;
        Parameters candidateParameters; // Prepared with candidate(); hashes are checked.
    };
    enum class Status { Queued, Frozen, Accepted, Rejected };
    enum class Error
    {
        None, InvalidIdentity, InvalidProposalId, InvalidParameters,
        RevisionMismatch, ProposalConflict, QueueFull, HistoryFull,
        Shutdown, UnknownProposal, AlreadyFrozen, StateMismatch,
        Cancelled, RolledBack
    };
    struct Receipt
    {
        Identity identity;
        QString proposalId, parameterSha256;
        quint64 parameterRevision = 0;
        Status status = Status::Rejected;
        Error reason = Error::None;
        // Populated only by authoritative accepted history.
        std::optional<quint64> appliedSeq;
        std::optional<double> acceptedTimeS;
        QString runtimeControllerRevision;
        bool restored = false;
    };
    struct Result
    {
        Error error = Error::None;
        std::optional<Receipt> receipt;
    };
    struct FrozenCandidate
    {
        QString proposalId;
        Parameters parameters; // Value copy; modifying it cannot modify the gate.
    };

    RuntimeParameterGate(Identity identity, State acceptedInitialState,
                         std::vector<Limit> immutableModelLimits);
    Result enqueue(const Request &request);
    std::optional<Receipt> query(const QString &proposalId) const;
    Result cancel(const QString &proposalId);
    std::optional<FrozenCandidate> freezeCandidate(const State &actualAcceptedState);
    Result onRollback(const QString &proposalId, Error reason = Error::RolledBack);
    Result onCommitted(const State &actualCommittedState);
    State acceptedState() const;
    // Drops queued work. A frozen solver transaction must still commit/rollback.
    void shutdown();

private:
    void validateEnvelope(const Parameters &parameters) const;
    Receipt *find(const QString &proposalId);
    const Identity identity_;
    const std::vector<Limit> limits_;
    mutable std::mutex mutex_;
    State accepted_;
    std::vector<Receipt> receipts_;
    std::optional<Request> pending_;
    bool stopped_ = false;
};

#endif
