#include "runtime_parameter_gate.h"

#include <QUuid>
#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace
{
namespace RP = RStructuralControlRuntimeParameters;
bool sha(const QString &s)
{
    if (s.size() != 64) return false;
    for (QChar c : s)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    return true;
}
bool id(const QString &s)
{
    if (s.isEmpty() || s.size() > 128) return false;
    for (QChar c : s)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
              (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
              c == QLatin1Char('-') || c == QLatin1Char('_') ||
              c == QLatin1Char('.') || c == QLatin1Char(':'))) return false;
    return true;
}
bool same(const RuntimeParameterGate::Identity &a, const RuntimeParameterGate::Identity &b)
{
    return a.jobId == b.jobId && a.runId == b.runId && a.taskId == b.taskId &&
           a.modelSha256 == b.modelSha256 && a.baseControllerRevision == b.baseControllerRevision;
}
bool uuid(const QString &s)
{
    QUuid u(s);
    return s.size() == 36 && !u.isNull() && u.toString(QUuid::WithoutBraces) == s;
}
QString runtimeRevision(const QString &base, const QString &parameter)
{
    QByteArray bytes;
    for (const auto &value : {base.toLatin1(),parameter.toLatin1()})
    {
        const quint32 length=qToBigEndian(quint32(value.size()));
        bytes.append(reinterpret_cast<const char *>(&length),sizeof(length));
        bytes.append(value);
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
std::vector<RP::ActuatorLimit> canonicalLimits(const RuntimeParameterGate::Identity &identity,
                                             std::vector<RP::ActuatorLimit> limits)
{
    return RP::fresh(identity.baseControllerRevision, 1.0, std::move(limits)).parameters.actuatorLimits;
}
}

RuntimeParameterGate::RuntimeParameterGate(Identity identity, State initial, std::vector<Limit> limits)
    : identity_(std::move(identity)), limits_(canonicalLimits(identity_, std::move(limits))),
      accepted_(std::move(initial))
{
    if (!id(identity_.jobId) || !id(identity_.runId) || !id(identity_.taskId) ||
        !sha(identity_.modelSha256) || !sha(identity_.baseControllerRevision))
        throw std::invalid_argument("Invalid runtime gate identity");
    RP::serialize(accepted_);
    validateEnvelope(accepted_.parameters);
    receipts_.reserve(RP::maximumHistoryCount);
    for (const auto &change : accepted_.history)
        receipts_.push_back({identity_, change.proposalId, change.parameterSha256,
                             change.revision, Status::Accepted, Error::None,
                             change.appliedSeq, change.acceptedTimeS,
                             runtimeRevision(identity_.baseControllerRevision,change.parameterSha256),true});
}

void RuntimeParameterGate::validateEnvelope(const Parameters &p) const
{
    if (p.fixedBaseControllerRevision != identity_.baseControllerRevision ||
        p.actuatorLimits.size() != limits_.size())
        throw std::invalid_argument("Runtime gate envelope mismatch");
    for (std::size_t i = 0; i < limits_.size(); ++i)
    {
        const auto &base = limits_[i];
        const auto &next = p.actuatorLimits[i];
        if (base.actuatorId != next.actuatorId || next.minN < base.minN || next.maxN > base.maxN ||
            (base.rateNPerS > 0.0 && (next.rateNPerS == 0.0 || next.rateNPerS > base.rateNPerS)))
            throw std::invalid_argument("Runtime gate limits exceed model envelope");
    }
}

RuntimeParameterGate::Receipt *RuntimeParameterGate::find(const QString &proposalId)
{
    for (auto &receipt : receipts_)
        if (receipt.proposalId == proposalId) return &receipt;
    return nullptr;
}

RuntimeParameterGate::Result RuntimeParameterGate::enqueue(const Request &r)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!same(identity_, r.identity)) return {Error::InvalidIdentity, {}};
    if (!uuid(r.proposalId)) return {Error::InvalidProposalId, {}};
    if (const Receipt *old = find(r.proposalId))
        if (old->parameterSha256 != r.candidateParameters.parameterSha256 ||
            old->parameterRevision != r.candidateParameters.revision ||
            r.expectedParameterRevision != old->parameterRevision - 1)
            return {Error::ProposalConflict, {}};
    // Validate supplied semantic values and both digests, even on duplicate delivery.
    try
    {
        RP::State hashInput = RP::fresh(identity_.baseControllerRevision,
                                       accepted_.parameters.velocityGainNsm,
                                       accepted_.parameters.actuatorLimits);
        // candidate() computes revision 1; use an accepted prefix for other revisions.
        if (r.candidateParameters.revision == 0) return {Error::InvalidParameters, {}};
        const auto &p = r.candidateParameters;
        if (p.revision <= accepted_.parameters.revision)
        {
            // Canonical codec validates arbitrary historical parameters using history.
            hashInput.parameters = p;
            hashInput.history = accepted_.history;
            hashInput.history.resize(std::size_t(p.revision));
            // This temporary validation state is never accepted or published.
            hashInput.history.back().parameterSha256 = p.parameterSha256;
            RP::serialize(hashInput);
        }
        else
        {
            const Parameters computed = RP::candidate(accepted_, p.velocityGainNsm, p.actuatorLimits);
            if (p.revision != computed.revision || p.fixedBaseControllerRevision != computed.fixedBaseControllerRevision ||
                p.parameterSha256 != computed.parameterSha256 || p.runtimeControllerRevision != computed.runtimeControllerRevision)
                return {Error::InvalidParameters, {}};
        }
        validateEnvelope(p);
    }
    catch (const std::invalid_argument &) { return {Error::InvalidParameters, {}}; }
    if (Receipt *old = find(r.proposalId))
    {
        if (old->parameterSha256 != r.candidateParameters.parameterSha256 ||
            old->parameterRevision != r.candidateParameters.revision ||
            r.expectedParameterRevision != old->parameterRevision - 1)
            return {Error::ProposalConflict, {}};
        return {Error::None, *old};
    }
    if (stopped_) return {Error::Shutdown, {}};
    if (r.expectedParameterRevision != accepted_.parameters.revision ||
        r.candidateParameters.revision != accepted_.parameters.revision + 1)
        return {Error::RevisionMismatch, {}};
    if (receipts_.size() >= RP::maximumHistoryCount || accepted_.history.size() >= RP::maximumHistoryCount)
        return {Error::HistoryFull, {}};
    if (pending_) return {Error::QueueFull, {}};
    Receipt receipt{identity_, r.proposalId, r.candidateParameters.parameterSha256,
                    r.candidateParameters.revision, Status::Queued, Error::None, {}, {},
                    r.candidateParameters.runtimeControllerRevision,false};
    pending_ = r;
    receipts_.push_back(receipt);
    return {Error::None, receipt};
}

std::optional<RuntimeParameterGate::Receipt> RuntimeParameterGate::query(const QString &proposalId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &r : receipts_) if (r.proposalId == proposalId) return r;
    return {};
}

RuntimeParameterGate::Result RuntimeParameterGate::cancel(const QString &proposalId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Receipt *r = find(proposalId);
    if (!r) return {Error::UnknownProposal, {}};
    if (r->status == Status::Frozen) return {Error::AlreadyFrozen, *r};
    if (r->status == Status::Queued)
    {
        r->status = Status::Rejected;
        r->reason = Error::Cancelled;
        pending_.reset();
    }
    return {Error::None, *r};
}

std::optional<RuntimeParameterGate::FrozenCandidate> RuntimeParameterGate::freezeCandidate(const State &actual)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || stopped_) return {};
    Receipt *r = find(pending_->proposalId);
    if (r->status != Status::Queued) return {};
    try
    {
        if (RP::serialize(actual) != RP::serialize(accepted_))
            throw std::invalid_argument("Runtime accepted state mismatch");
    }
    catch (const std::invalid_argument &)
    {
        r->status = Status::Rejected;
        r->reason = Error::StateMismatch;
        pending_.reset();
        return {};
    }
    r->status = Status::Frozen;
    return FrozenCandidate{pending_->proposalId, pending_->candidateParameters};
}

RuntimeParameterGate::Result RuntimeParameterGate::onRollback(const QString &proposalId, Error reason)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Receipt *r = find(proposalId);
    if (!r) return {Error::UnknownProposal, {}};
    if (!pending_ || pending_->proposalId != proposalId || r->status != Status::Frozen)
        return {Error::StateMismatch, *r};
    r->status = Status::Rejected;
    r->reason = reason == Error::None ? Error::RolledBack : reason;
    pending_.reset();
    return {Error::None, *r};
}

RuntimeParameterGate::Result RuntimeParameterGate::onCommitted(const State &actual)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_) return {Error::StateMismatch, {}};
    Receipt *r = find(pending_->proposalId);
    if (r->status != Status::Frozen || actual.history.empty()) return {Error::StateMismatch, *r};
    try
    {
        const auto &change = actual.history.back();
        const State expected = RP::acceptedChange(accepted_, pending_->proposalId,
            pending_->candidateParameters, change.appliedSeq, change.acceptedTimeS);
        if (RP::serialize(actual) != RP::serialize(expected)) return {Error::StateMismatch, *r};
        // Allocate the authoritative copy before modifying gate state.
        State nextAccepted = actual;
        accepted_ = std::move(nextAccepted);
        r->status = Status::Accepted;
        r->reason = Error::None;
        r->appliedSeq = change.appliedSeq;
        r->acceptedTimeS = change.acceptedTimeS;
        pending_.reset();
        return {Error::None, *r};
    }
    catch (const std::invalid_argument &) { return {Error::StateMismatch, *r}; }
}

RuntimeParameterGate::State RuntimeParameterGate::acceptedState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_;
}

void RuntimeParameterGate::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    if (pending_)
    {
        Receipt *r = find(pending_->proposalId);
        if (r->status == Status::Queued)
        {
            r->status = Status::Rejected;
            r->reason = Error::Shutdown;
            pending_.reset();
        }
    }
}
