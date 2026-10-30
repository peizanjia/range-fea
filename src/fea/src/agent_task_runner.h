#ifndef AGENT_TASK_RUNNER_H
#define AGENT_TASK_RUNNER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <QTimer>
#include <functional>
#include "model_command_host.h"
#include "workflow_process.h"

//! Provider-neutral, bounded tool loop. Credentials and provider HTTP stay outside.
//! Only the trusted GUI calls authorizePending(); model tools cannot grant approval.
class AgentTaskRunner : public QObject
{
    Q_OBJECT
public:
    enum class AccessPolicy { ReadWrite, ReadOnly };
    enum class EvaluationArm { Standard, DirectNl, ToolsFeedback, FullTrusted };
    using ExecutionRecorder = std::function<bool(const QString &taskId, const QString &origin,
                                                 const QJsonObject &safeReference)>;

    explicit AgentTaskRunner(ModelCommandHost *host, WorkflowProcess *workflow, QObject *parent = nullptr,
                             AccessPolicy policy = AccessPolicy::ReadWrite,
                             EvaluationArm evaluationArm = EvaluationArm::Standard);
    bool isActive() const { return active || retirementInProgress; }
    bool isAwaitingInput() const { return !awaitingQuestionId.isEmpty(); }
    QString pendingQuestionId() const { return awaitingQuestionId; }
    QJsonObject pendingProposal() const { return proposal; }
    QString taskId() const { return activityTaskId; }
    bool restoreObservableConversation(const QJsonArray &visibleMessages);
    //! A trusted recorder runs before command submission; false blocks the execution.
    void setExecutionRecorder(ExecutionRecorder recorder);
    //! Trusted UI only: prepare a Host-validated experiment authorization while idle.
    QJsonObject prepareBoundedExperimentAuthorization(const QJsonObject &parameters);
    //! Arm the exact prepared preview after the trusted caller has reviewed it.
    bool armBoundedExperimentAuthorization(const QString &previewId);
    void clearBoundedExperimentAuthorization();
    //! Trusted durable receipt reconciliation; never submits or retries an action.
    void resolveExecutionObservation(const QString &taskId, const QJsonObject &safeReference);
    //! Conservatively classify a persisted Host cancellation using child-job terminal evidence.
    static QJsonObject summarizeChildCancellation(const QJsonObject &experimentSummary,
                                                   bool childCancellationRequestFailed,
                                                   const QStringList &expectedChildJobIds,
                                                   bool expectedChildJobSetComplete = true);
    static bool authorizationDigestMatches(const QJsonObject &summary,
                                          const QString &expectedDigest);
    //! True only for a lifecycle state emitted by the trusted experiment Host.
    static bool isKnownExperimentLifecycleState(const QString &state);
    int pendingExecutionObservationCount() const { return pendingWorkflowExecutions.size(); }
    static QJsonArray toolDefinitions(AccessPolicy policy = AccessPolicy::ReadWrite,
                                      EvaluationArm evaluationArm = EvaluationArm::Standard);
    static QString evaluationArmId(EvaluationArm evaluationArm);

public slots:
    void startTask(const QString &goal, bool executeRequestedActions = false);
    void submitMessage(const QString &text, bool automatic = false);
    void clearConversation();
    void answerQuestion(const QString &questionId, const QString &text);
    void receiveTurn(const QString &requestId, const QJsonObject &turn);
    void providerFailed(const QString &requestId, const QString &message);
    void authorizePending(const QString &proposalId, bool accepted);
    void cancel();

signals:
    //! Trusted task identity is published before any context/provider request begins.
    void taskStarted(const QString &taskId, const QString &goal, bool readOnly);
    //! Submitted messages discarded by Stop before their Agent task began.
    void pendingMessagesCancelled(const QStringList &messages);
    //! The provider must echo requestId to receiveTurn/providerFailed unchanged.
    void turnRequested(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools);
    void providerCancellationRequested(const QString &requestId);
    void stateChanged(const QString &state, const QString &message);
    //! Non-empty assistant text from each accepted intermediate or final turn.
    void assistantMessage(const QString &text);
    //! A bounded user question requested by the model. The id binds the answer to this question.
    void userInputRequested(const QString &questionId, const QString &question, const QJsonArray &options);
    //! proposalId binds the visible approval controls to this exact pending proposal.
    void proposalReady(const QString &origin, const QString &proposalId, const QJsonObject &proposal);
    void actionCompleted(const QString &origin, const QJsonObject &result);
    //! A successful execution result is published independently of the task's later state.
    void executionObserved(const QString &taskId, const QString &origin,
                           const QJsonObject &safeReference, const QJsonObject &result);
    //! A submitted execution request failed without an authoritative accepted result.
    void executionObservationUnknown(const QString &taskId, const QString &origin,
                                     const QJsonObject &safeReference, const QString &code);
    //! Raw trusted Host experiment summary. This is an independent fact channel, not model prose.
    void experimentObserved(const QString &taskId, const QString &experimentId,
                            const QJsonObject &hostSummary);
    //! Exact tool-call identity for joining a Host experiment observation to evaluation capture.
    void experimentEvidenceObserved(const QString &taskId, const QString &toolCallId,
                                    const QString &experimentId, const QJsonObject &hostSummary);
    //! Bounded task-stop cancellation outcome with the approved source binding retained.
    void experimentCancellationObserved(const QString &taskId, const QString &experimentId,
                                        const QJsonObject &sourceBinding,
                                        const QJsonObject &cancellationFact);
    //! Activity facts originate from complete validated tool calls and host replies.
    void toolActivity(const QString &taskId, const QString &toolCallId, const QString &toolName,
                      const QString &phase, const QString &operationId, const QJsonObject &facts);
    //! Exact tool-role bytes delivered to protocol history, represented only by hash and size.
    void toolResultObserved(const QJsonObject &event);
    void finished(const QString &status, const QString &message);

private:
    friend class TestAgentTaskRunner;

    struct ExperimentCancellationEvent
    {
        QString taskId;
        QString experimentId;
        QJsonObject sourceBinding;
        QJsonObject fact;
    };

    struct PendingExecution
    {
        QString taskId;
        QString origin;
        QJsonObject safeReference;
        bool unknownReported = false;
    };

    const AccessPolicy accessPolicy;
    const EvaluationArm evaluationArm;
    ModelCommandHost *host;
    WorkflowProcess *workflow;
    QTimer budgetTimer;
    int remainingExecutionBudgetMs = 0;
    bool executionBudgetPaused = false;
    bool pauseExecutionBudget();
    bool resumeExecutionBudget();
    void clearExecutionBudget();
    QJsonArray history, calls, completedActions;
    QJsonArray restoredObservableConversation;
    QJsonArray readOnlyPinnedDocuments;
    QSet<QString> readOnlyNativeJobIds, readOnlyWorkflowJobIds;
    QHash<QString, QJsonObject> readOnlyNativeJobSources;
    //! Retained across task retirement so late workflow execution results keep their original task binding.
    QHash<QString, PendingExecution> pendingWorkflowExecutions;
    QSet<QString> allowedDocuments, completedCallIds, taskExperimentIds;
    QHash<QString, QJsonObject> taskExperimentSourceBindings;
    QHash<QString, quint64> automaticDocumentRevisions;
    QHash<QString, QSet<quint64>> automaticDocumentRevisionLineage;
    QHash<QString, QJsonObject> taskNativeJobSources;
    QJsonObject currentCall, proposal, proposedArguments;
    QJsonObject preparedExperimentAuthorization, armedExperimentAuthorization, taskExperimentAuthorization;
    QJsonObject executingBoundedExperimentAuthorization;
    bool boundedExperimentAutoApproval = false;
    QJsonObject experimentWaitLastSummary;
    QJsonObject readOnlyWorkflowIdentity, pendingReadOnlyWorkflowResult, pendingReadOnlyTurn;
    QJsonObject readOnlyGuiProjectIdentity;
    QJsonObject readOnlyPendingWorkflowParameters;
    ExecutionRecorder executionRecorder;
    QString sessionEpoch, workflowWorkspace, providerRequestId, waitingRequest, waitingOperation;
    QString readOnlySessionEpoch, readOnlyWorkspace, readOnlyPendingOperation;
    QString readOnlyPostflightOperation;
    QString proposalOrigin, pendingProposalId, awaitedJob;
    QString awaitedExperiment, awaitedExperimentCallId;
    QString activityTaskId;
    QString awaitingQuestionId;
    struct PendingRetirementMessage
    {
        QString text;
        bool automatic = false;
        QJsonArray priorConversation;
        bool priorConversationCaptured = false;
    };
    QList<PendingRetirementMessage> pendingRetirementMessages;
    quint64 taskGeneration = 0, waitingRequestGeneration = 0;
    int nextCall = 0, turns = 0, toolCount = 0;
    //! Optional per-task cap lowered only after a trusted experiment is Host-approved.
    int taskModelTurnLimit = 0;
    int experimentWaitPolls = 0;
    bool active = false, retirementInProgress = false, providerPending = false, automatic = false;
    bool authorizationInProgress = false;
    bool readOnlyBindingsCaptured = false, readOnlyWorkflowIdentityCaptured = false;
    bool readOnlyIdentityBootstrap = false, readOnlyWorkflowPreflight = false;
    bool readOnlyWorkflowPostflight = false;
    bool readOnlyWorkflowJobsBootstrap = false, readOnlyWorkflowBootstrapFinalCheck = false;
    bool readOnlyTurnValidationPending = false;
    bool isReadOnly() const
    {
        return evaluationArm == EvaluationArm::ToolsFeedback
            || (evaluationArm == EvaluationArm::Standard && accessPolicy == AccessPolicy::ReadOnly);
    }
    bool readOnlyToolAllowed(const QString &name) const;
    bool readOnlyHostOperationAllowed(const QString &operation) const;
    bool readOnlyWorkflowOperationAllowed(const QString &operation) const;
    bool readOnlySnapshotIsCurrent();
    bool readOnlyWorkflowIdentityMatches(const QJsonObject &snapshot, bool captureIfMissing);
    void failReadOnlySnapshot();
    QString issueWorkflowRequest(const QString &operation, const QJsonObject &parameters);
    void processWorkflowResult(const QString &operation, const QJsonObject &result);
    void acceptProviderTurn(const QJsonObject &turn);
    QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {});
    bool workflowSourceIsCurrent(const QJsonObject &preview);
    void beginTask(const QString &goal, bool executeRequestedActions, const QJsonArray &priorConversation);
    void retireTaskForSteering();
    void startPendingRetirementMessage();
    QJsonArray summarizeConversation(bool interrupted) const;
    bool validateTurn(const QJsonObject &turn, QString *error) const;
    void requestTurn();
    void nextTool();
    void finishTool(const QJsonObject &result);
    void failTool(const QString &code, const QString &message);
    bool publishToolActivity(const QString &phase, const QJsonObject &facts = {},
                             const QString &operationId = {});
    void finishTask(const QString &status, const QString &message);
    static QJsonObject budgetExhaustionExperimentEvidence(const QString &experimentId,
                                                          const QJsonObject &sourceBinding,
                                                          const QJsonObject &hostResponse);
    static QJsonObject boundedBudgetExhaustionEvidence(int providerTurnRequestsIssued,
                                                       int providerTurnLimit,
                                                       const QJsonArray &experiments);
    void workflowReply(const QString &id, const QString &operation, const QJsonObject &result);
    void submitWorkflow(const QString &operation, const QJsonObject &parameters = {});
    void propose(const QString &origin, const QJsonObject &value,
                 const QJsonObject &automaticSourceBinding = {});
    void scheduleWorkflowPoll(const QString &jobId);
    void queryOwnedExperiment(const QString &experimentId, bool waiting);
    void scheduleExperimentPoll(const QString &experimentId, const QString &callId);
    bool cancelTaskExperiments(const QString &trigger, const QString &taskId,
                               quint64 expectedGeneration,
                               QList<ExperimentCancellationEvent> *events,
                               bool *hadUnresolvedExperiment);
    QString connectedWorkflowWorkspace() const;
};

#endif
