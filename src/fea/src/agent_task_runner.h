#ifndef AGENT_TASK_RUNNER_H
#define AGENT_TASK_RUNNER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <QTimer>
#include "model_command_host.h"
#include "workflow_process.h"

//! Provider-neutral, bounded tool loop. Credentials and provider HTTP stay outside.
//! Only the trusted GUI calls authorizePending(); model tools cannot grant approval.
class AgentTaskRunner : public QObject
{
    Q_OBJECT
public:
    explicit AgentTaskRunner(ModelCommandHost *host, WorkflowProcess *workflow, QObject *parent = nullptr);
    bool isActive() const { return active; }
    QJsonObject pendingProposal() const { return proposal; }
    static QJsonArray toolDefinitions();

public slots:
    void startTask(const QString &goal, bool executeRequestedActions = false);
    void receiveTurn(const QString &requestId, const QJsonObject &turn);
    void providerFailed(const QString &requestId, const QString &message);
    void authorizePending(const QString &proposalId, bool accepted);
    void cancel();

signals:
    //! The provider must echo requestId to receiveTurn/providerFailed unchanged.
    void turnRequested(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools);
    void providerCancellationRequested(const QString &requestId);
    void stateChanged(const QString &state, const QString &message);
    //! proposalId binds the visible approval controls to this exact pending proposal.
    void proposalReady(const QString &origin, const QString &proposalId, const QJsonObject &proposal);
    void actionCompleted(const QString &origin, const QJsonObject &result);
    void finished(const QString &status, const QString &message);

private:
    ModelCommandHost *host;
    WorkflowProcess *workflow;
    QTimer budgetTimer;
    QJsonArray history, calls;
    QSet<QString> allowedDocuments, completedCallIds;
    QHash<QString, quint64> automaticDocumentRevisions;
    QJsonObject currentCall, proposal, proposedArguments;
    QString sessionEpoch, workflowWorkspace, providerRequestId, waitingRequest, waitingOperation;
    QString proposalOrigin, pendingProposalId, awaitedJob;
    quint64 taskGeneration = 0, waitingRequestGeneration = 0;
    int nextCall = 0, turns = 0, toolCount = 0;
    bool active = false, providerPending = false, automatic = false;
    QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {});
    bool validateTurn(const QJsonObject &turn, QString *error) const;
    void requestTurn();
    void nextTool();
    void finishTool(const QJsonObject &result);
    void failTool(const QString &code, const QString &message);
    void finishTask(const QString &status, const QString &message);
    void workflowReply(const QString &id, const QString &operation, const QJsonObject &result);
    void submitWorkflow(const QString &operation, const QJsonObject &parameters = {});
    void propose(const QString &origin, const QJsonObject &value);
    void scheduleWorkflowPoll(const QString &jobId);
    QString connectedWorkflowWorkspace() const;
};

#endif
