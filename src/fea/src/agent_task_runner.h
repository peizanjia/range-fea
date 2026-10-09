#ifndef AGENT_TASK_RUNNER_H
#define AGENT_TASK_RUNNER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QHash>
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
    using ExecutionRecorder = std::function<bool(const QString &taskId, const QString &origin,
                                                 const QJsonObject &safeReference)>;

    explicit AgentTaskRunner(ModelCommandHost *host, WorkflowProcess *workflow, QObject *parent = nullptr,
                             AccessPolicy policy = AccessPolicy::ReadWrite);
    bool isActive() const { return active; }
    bool isAwaitingInput() const { return !awaitingQuestionId.isEmpty(); }
    QString pendingQuestionId() const { return awaitingQuestionId; }
    QJsonObject pendingProposal() const { return proposal; }
    QString taskId() const { return activityTaskId; }
    bool restoreObservableConversation(const QJsonArray &visibleMessages);
    //! A trusted recorder runs before command submission; false blocks the execution.
    void setExecutionRecorder(ExecutionRecorder recorder);
    //! Trusted durable receipt reconciliation; never submits or retries an action.
    void resolveExecutionObservation(const QString &taskId, const QJsonObject &safeReference);
    int pendingExecutionObservationCount() const { return pendingWorkflowExecutions.size(); }
    static QJsonArray toolDefinitions(AccessPolicy policy = AccessPolicy::ReadWrite);

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
    //! Activity facts originate from complete validated tool calls and host replies.
    void toolActivity(const QString &taskId, const QString &toolCallId, const QString &toolName,
                      const QString &phase, const QString &operationId, const QJsonObject &facts);
    void finished(const QString &status, const QString &message);

private:
    struct PendingExecution
    {
        QString taskId;
        QString origin;
        QJsonObject safeReference;
        bool unknownReported = false;
    };

    const AccessPolicy accessPolicy;
    ModelCommandHost *host;
    WorkflowProcess *workflow;
    QTimer budgetTimer;
    QJsonArray history, calls, completedActions;
    QJsonArray restoredObservableConversation;
    QJsonArray readOnlyPinnedDocuments;
    QSet<QString> readOnlyNativeJobIds, readOnlyWorkflowJobIds;
    QHash<QString, QJsonObject> readOnlyNativeJobSources;
    //! Retained across task retirement so late workflow execution results keep their original task binding.
    QHash<QString, PendingExecution> pendingWorkflowExecutions;
    QSet<QString> allowedDocuments, completedCallIds;
    QHash<QString, quint64> automaticDocumentRevisions;
    QHash<QString, QSet<quint64>> automaticDocumentRevisionLineage;
    QHash<QString, QJsonObject> taskNativeJobSources;
    QJsonObject currentCall, proposal, proposedArguments;
    QJsonObject readOnlyWorkflowIdentity, pendingReadOnlyWorkflowResult, pendingReadOnlyTurn;
    QJsonObject readOnlyGuiProjectIdentity;
    QJsonObject readOnlyPendingWorkflowParameters;
    ExecutionRecorder executionRecorder;
    QString sessionEpoch, workflowWorkspace, providerRequestId, waitingRequest, waitingOperation;
    QString readOnlySessionEpoch, readOnlyWorkspace, readOnlyPendingOperation;
    QString readOnlyPostflightOperation;
    QString proposalOrigin, pendingProposalId, awaitedJob;
    QString activityTaskId;
    QString awaitingQuestionId;
    quint64 taskGeneration = 0, waitingRequestGeneration = 0;
    int nextCall = 0, turns = 0, toolCount = 0;
    bool active = false, providerPending = false, automatic = false;
    bool authorizationInProgress = false;
    bool readOnlyBindingsCaptured = false, readOnlyWorkflowIdentityCaptured = false;
    bool readOnlyIdentityBootstrap = false, readOnlyWorkflowPreflight = false;
    bool readOnlyWorkflowPostflight = false;
    bool readOnlyWorkflowJobsBootstrap = false, readOnlyWorkflowBootstrapFinalCheck = false;
    bool readOnlyTurnValidationPending = false;
    bool isReadOnly() const { return accessPolicy == AccessPolicy::ReadOnly; }
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
    QJsonArray summarizeConversation(bool interrupted) const;
    bool validateTurn(const QJsonObject &turn, QString *error) const;
    void requestTurn();
    void nextTool();
    void finishTool(const QJsonObject &result);
    void failTool(const QString &code, const QString &message);
    bool publishToolActivity(const QString &phase, const QJsonObject &facts = {},
                             const QString &operationId = {});
    void finishTask(const QString &status, const QString &message);
    void workflowReply(const QString &id, const QString &operation, const QJsonObject &result);
    void submitWorkflow(const QString &operation, const QJsonObject &parameters = {});
    void propose(const QString &origin, const QJsonObject &value,
                 const QJsonObject &automaticSourceBinding = {});
    void scheduleWorkflowPoll(const QString &jobId);
    QString connectedWorkflowWorkspace() const;
};

#endif
