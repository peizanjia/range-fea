#include <QtTest>
#include "runtime_parameter_gate.h"

#include <atomic>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
namespace RP = RStructuralControlRuntimeParameters;
using Gate = RuntimeParameterGate;
Gate::Identity identity()
{
    return {QStringLiteral("job-1"), QStringLiteral("run-1"), QStringLiteral("task-1"),
            QString(64, QLatin1Char('a')), QString(64, QLatin1Char('b'))};
}
std::vector<Gate::Limit> limits() { return {{QStringLiteral("force-1"), -100, 100, 10}}; }
Gate::State initial() { return RP::fresh(identity().baseControllerRevision, 1, limits()); }
QString proposal(int i)
{
    return QStringLiteral("00000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0'));
}
Gate::Request request(const Gate::State &s, int i = 1, double gain = 2)
{
    return {identity(), proposal(i), s.parameters.revision,
            RP::candidate(s, gain, limits())};
}
}

class RuntimeParameterGateTest : public QObject
{
    Q_OBJECT
private slots:
    void invalidRequest_data()
    {
        QTest::addColumn<int>("kind");
        for (const auto &row : std::vector<std::pair<const char *, int>>{
                 {"wrong-job", 0}, {"wrong-run", 1}, {"wrong-task", 2},
                 {"wrong-model", 3}, {"wrong-base", 4}, {"invalid-proposal", 5},
                 {"wrong-expected-revision", 6}, {"wrong-parameter-revision", 7},
                 {"wrong-hash", 8}, {"wrong-runtime-hash", 9}, {"unknown-actuator", 10},
                 {"nonfinite-gain", 11}, {"amplitude-widening", 12},
                 {"positive-rate-to-unlimited", 13}, {"rate-widening", 14},
                 {"nonfinite-limit", 15}, {"changed-parameter-base", 16}})
            QTest::newRow(row.first) << row.second;
    }
    void invalidRequest()
    {
        QFETCH(int, kind);
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        switch (kind)
        {
        case 0: r.identity.jobId = "job-2"; break;
        case 1: r.identity.runId = "run-2"; break;
        case 2: r.identity.taskId = "task-2"; break;
        case 3: r.identity.modelSha256 = QString(64, 'c'); break;
        case 4: r.identity.baseControllerRevision = QString(64, 'c'); break;
        case 5: r.proposalId = "not-a-uuid"; break;
        case 6: r.expectedParameterRevision = 99; break;
        case 7: r.candidateParameters.revision = 99; break;
        case 8: r.candidateParameters.parameterSha256 = QString(64, 'c'); break;
        case 9: r.candidateParameters.runtimeControllerRevision = QString(64, 'c'); break;
        case 10:
        {
            auto unknown = RP::fresh(identity().baseControllerRevision, 1, {{"unknown", -100, 100, 10}});
            r.candidateParameters = RP::candidate(unknown, 2, unknown.parameters.actuatorLimits);
            break;
        }
        case 11: r.candidateParameters.velocityGainNsm = std::numeric_limits<double>::infinity(); break;
        case 12: r.candidateParameters = RP::candidate(initial(), 2, {{"force-1", -101, 100, 10}}); break;
        case 13: r.candidateParameters = RP::candidate(initial(), 2, {{"force-1", -100, 100, 0}}); break;
        case 14: r.candidateParameters = RP::candidate(initial(), 2, {{"force-1", -100, 100, 11}}); break;
        case 15: r.candidateParameters.actuatorLimits[0].maxN = std::numeric_limits<double>::quiet_NaN(); break;
        case 16: r.candidateParameters.fixedBaseControllerRevision = QString(64, 'c'); break;
        }
        QVERIFY(gate.enqueue(r).error != Gate::Error::None);
        QVERIFY(!gate.query(r.proposalId));
        QCOMPARE(RP::serialize(gate.acceptedState()), RP::serialize(initial()));
        QCOMPARE(gate.enqueue(request(initial(), 2)).error, Gate::Error::None);
    }
    void queueIdempotencyAndConflict()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        auto result = gate.enqueue(r);
        QCOMPARE(result.receipt->status, Gate::Status::Queued);
        QVERIFY(!result.receipt->appliedSeq);
        QVERIFY(!result.receipt->acceptedTimeS);
        QCOMPARE(gate.enqueue(r).receipt->status, Gate::Status::Queued);
        QCOMPARE(gate.enqueue(request(initial(), 2)).error, Gate::Error::QueueFull);
        QCOMPARE(gate.enqueue(request(initial(), 1, 3)).error, Gate::Error::ProposalConflict);
        QCOMPARE(gate.cancel(r.proposalId).receipt->status, Gate::Status::Rejected);
        QCOMPARE(gate.enqueue(r).receipt->reason, Gate::Error::Cancelled);
        QVERIFY(!gate.freezeCandidate(initial()));
    }
    void freezeCancelRollback()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        gate.enqueue(r);
        auto frozen = gate.freezeCandidate(initial());
        QVERIFY(frozen);
        QCOMPARE(frozen->parameters.parameterSha256, r.candidateParameters.parameterSha256);
        frozen->parameters.velocityGainNsm = 999; // Copies cannot mutate gate state.
        QCOMPARE(gate.query(r.proposalId)->status, Gate::Status::Frozen);
        QCOMPARE(gate.enqueue(r).receipt->status, Gate::Status::Frozen);
        QCOMPARE(gate.cancel(r.proposalId).error, Gate::Error::AlreadyFrozen);
        QVERIFY(!gate.freezeCandidate(initial()));
        QCOMPARE(gate.onRollback(proposal(99)).error, Gate::Error::UnknownProposal);
        QCOMPARE(gate.onRollback(r.proposalId).receipt->reason, Gate::Error::RolledBack);
        QCOMPARE(RP::serialize(gate.acceptedState()), RP::serialize(initial()));
        QCOMPARE(gate.enqueue(r).receipt->status, Gate::Status::Rejected);
    }
    void authoritativeCommit()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        gate.enqueue(r);
        auto committed = RP::acceptedChange(initial(), r.proposalId, r.candidateParameters, 17, 0.25);
        QCOMPARE(gate.onCommitted(committed).error, Gate::Error::StateMismatch);
        QVERIFY(gate.freezeCandidate(initial()));
        auto wrong = RP::acceptedChange(initial(), proposal(2), r.candidateParameters, 17, 0.25);
        QCOMPARE(gate.onCommitted(wrong).error, Gate::Error::StateMismatch);
        wrong = RP::acceptedChange(initial(), r.proposalId, request(initial(), 1, 3).candidateParameters, 17, 0.25);
        QCOMPARE(gate.onCommitted(wrong).error, Gate::Error::StateMismatch);
        auto result = gate.onCommitted(committed);
        QCOMPARE(result.error, Gate::Error::None);
        QCOMPARE(result.receipt->status, Gate::Status::Accepted);
        QCOMPARE(*result.receipt->appliedSeq, quint64(17));
        QCOMPARE(*result.receipt->acceptedTimeS, 0.25);
        QCOMPARE(RP::serialize(gate.acceptedState()), RP::serialize(committed));
        QCOMPARE(gate.enqueue(r).receipt->status, Gate::Status::Accepted);
        QCOMPARE(gate.onRollback(r.proposalId).error, Gate::Error::StateMismatch);
        QCOMPARE(gate.cancel(r.proposalId).receipt->status, Gate::Status::Accepted);
        // Receipt publication is outside this gate; querying cannot undo commit.
        QCOMPARE(RP::serialize(gate.acceptedState()), RP::serialize(committed));
    }
    void freezeRejectsStaleAcceptedState()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        gate.enqueue(r);
        auto other = RP::fresh(identity().baseControllerRevision, 3, limits());
        QVERIFY(!gate.freezeCandidate(other));
        QCOMPARE(gate.query(r.proposalId)->reason, Gate::Error::StateMismatch);
        QCOMPARE(RP::serialize(gate.acceptedState()), RP::serialize(initial()));
    }
    void queueDoesNotPredictFutureForceFeasibility()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        r.candidateParameters = RP::candidate(initial(), 2, {{"force-1", 90, 100, 1}});
        QCOMPARE(gate.enqueue(r).error, Gate::Error::None);
        QVERIFY(gate.freezeCandidate(initial()));
        QMap<QString, double> last{{"force-1", 0}};
        QVERIFY_EXCEPTION_THROWN(RP::validateCandidateAgainstBase(r.candidateParameters, limits(), last, 0.01), std::invalid_argument);
        gate.onRollback(r.proposalId);
        QCOMPARE(gate.acceptedState().parameters.revision, quint64(0));
    }
    void historyCapacityNoEviction()
    {
        Gate gate(identity(), initial(), limits());
        for (int i = 1; i <= 128; ++i)
        {
            auto r = request(initial(), i);
            QCOMPARE(gate.enqueue(r).error, Gate::Error::None);
            gate.cancel(r.proposalId);
        }
        QCOMPARE(gate.enqueue(request(initial(), 129)).error, Gate::Error::HistoryFull);
        QCOMPARE(gate.query(proposal(1))->reason, Gate::Error::Cancelled);
        QCOMPARE(gate.enqueue(request(initial(), 1)).receipt->status, Gate::Status::Rejected);
        QVERIFY(!gate.query(proposal(129)));
    }
    void restoreAcceptedReceipts()
    {
        auto r = request(initial());
        auto s = RP::acceptedChange(initial(), r.proposalId, r.candidateParameters, 1, 0.1);
        Gate gate(identity(), s, limits());
        QCOMPARE(gate.query(r.proposalId)->status, Gate::Status::Accepted);
        QVERIFY(gate.query(r.proposalId)->restored);
        QCOMPARE(gate.query(r.proposalId)->runtimeControllerRevision,s.parameters.runtimeControllerRevision);
        QCOMPARE(gate.enqueue(r).receipt->status, Gate::Status::Accepted);
        QCOMPARE(gate.enqueue(request(s, 2)).error, Gate::Error::None);
    }
    void twoAcceptedRevisionsPreserveHistoryAndRejectInvalidBoundaries()
    {
        Gate gate(identity(),initial(),limits());
        auto first=request(initial(),1);
        QVERIFY(gate.enqueue(first).receipt);
        QVERIFY(gate.freezeCandidate(initial()));
        auto s=RP::acceptedChange(initial(),first.proposalId,first.candidateParameters,1,0.1);
        auto result=gate.onCommitted(s);
        QVERIFY(result.receipt);
        QVERIFY(!result.receipt->restored);
        QCOMPARE(result.receipt->runtimeControllerRevision,s.parameters.runtimeControllerRevision);
        const auto prefix=s.history.front();
        auto second=request(s,2,3);
        QVERIFY(gate.enqueue(second).receipt);
        QVERIFY(gate.freezeCandidate(s));
        auto committed=RP::acceptedChange(s,second.proposalId,second.candidateParameters,2,0.2);
        auto invalid=committed;
        for(int kind=0;kind<5;++kind)
        {
            invalid=committed;
            if(kind==0)invalid.history.back().appliedSeq=0;
            if(kind==1)invalid.history.back().appliedSeq=1;
            if(kind==2)invalid.history.back().acceptedTimeS=0.1;
            if(kind==3)invalid.history.back().acceptedTimeS=std::numeric_limits<double>::infinity();
            if(kind==4)invalid.history.front().acceptedTimeS=0.09; // Valid codec, changed accepted prefix.
            QCOMPARE(gate.onCommitted(invalid).error,Gate::Error::StateMismatch);
            QCOMPARE(RP::serialize(gate.acceptedState()),RP::serialize(s));
            QCOMPARE(gate.query(second.proposalId)->status,Gate::Status::Frozen);
        }
        QCOMPARE(gate.onCommitted(committed).error,Gate::Error::None);
        QCOMPARE(gate.acceptedState().history.size(),std::size_t(2));
        QCOMPARE(gate.acceptedState().history.front().proposalId,prefix.proposalId);
        QCOMPARE(gate.acceptedState().history.front().acceptedTimeS,prefix.acceptedTimeS);
        QCOMPARE(gate.enqueue(first).receipt->status,Gate::Status::Accepted);
        auto corrupt=first;corrupt.candidateParameters.velocityGainNsm=7;
        QCOMPARE(gate.enqueue(corrupt).error,Gate::Error::InvalidParameters);
    }
    void threadedEnqueueExactlyOneWinner()
    {
        Gate gate(identity(), initial(), limits());
        std::atomic<int> ready{0}, winners{0}, queueFull{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        for (int i = 1; i <= 16; ++i)
            threads.emplace_back([&, i] {
                auto r = request(initial(), i);
                ++ready;
                while (!start.load()) std::this_thread::yield();
                const auto result = gate.enqueue(r);
                if (result.error == Gate::Error::None) ++winners;
                if (result.error == Gate::Error::QueueFull) ++queueFull;
            });
        while (ready.load() != 16) std::this_thread::yield();
        start = true;
        for (auto &t : threads) t.join();
        QCOMPARE(winners.load(), 1);
        QCOMPARE(queueFull.load(), 15);
    }
    void shutdownConcurrentQueryAndNoReplay()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        gate.enqueue(r);
        std::thread reader([&] { for (int i = 0; i < 1000; ++i) gate.query(r.proposalId); });
        std::thread stopper([&] { gate.shutdown(); });
        reader.join();
        stopper.join();
        QCOMPARE(gate.query(r.proposalId)->reason, Gate::Error::Shutdown);
        QVERIFY(!gate.freezeCandidate(initial()));
        QCOMPARE(gate.enqueue(r).receipt->status, Gate::Status::Rejected);
        QCOMPARE(gate.enqueue(request(initial(), 2)).error, Gate::Error::Shutdown);
        Gate restored(identity(), gate.acceptedState(), limits());
        QVERIFY(!restored.query(r.proposalId));
        QVERIFY(!restored.freezeCandidate(initial()));
    }
    void shutdownCannotWithdrawFrozenCommit()
    {
        Gate gate(identity(), initial(), limits());
        auto r = request(initial());
        gate.enqueue(r);
        QVERIFY(gate.freezeCandidate(initial()));
        gate.shutdown();
        QCOMPARE(gate.cancel(r.proposalId).error, Gate::Error::AlreadyFrozen);
        auto s = RP::acceptedChange(initial(), r.proposalId, r.candidateParameters, 1, 0.1);
        QCOMPARE(gate.onCommitted(s).receipt->status, Gate::Status::Accepted);
        QCOMPARE(gate.enqueue(request(s, 2)).error, Gate::Error::Shutdown);
    }
};

QTEST_MAIN(RuntimeParameterGateTest)
#include "tst_runtime_parameter_gate.moc"
