#ifndef AGENT_WIDGET_H
#define AGENT_WIDGET_H

#include <QWidget>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTextCursor>
#include <QTimer>
#include <QHash>
#include <QStringList>
#include <memory>
#include <rai_agent_settings.h>
#include "agent_task_runner.h"

class QComboBox;
class QFrame;
class QGridLayout;
class QLineEdit;
class QPlainTextEdit;
class QScrollArea;
class QTextBrowser;
class QPushButton;
class QCheckBox;
class QLabel;
class QNetworkReply;
class QToolButton;
class QVBoxLayout;
class QDialog;
class RAgentConversationStore;

//! Compact conversation UI. Provider credentials remain in memory only.
class AgentWidget : public QWidget
{
    Q_OBJECT
public:
    //! An injected network manager remains owned by the caller and must outlive this widget.
    AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent = nullptr,
                QNetworkAccessManager *networkAccessManager = nullptr,
                AgentTaskRunner::AccessPolicy policy = AgentTaskRunner::AccessPolicy::ReadWrite,
                bool restoreLastConversation = false);
    ~AgentWidget() override;
    AgentTaskRunner *taskRunner() { return &runner; }
    void setProviderSettings(const RAgentSettings &settings);
    RAgentSettings getProviderSettings() const;
    static QWidget *createSettingsPage(QWidget *parent = nullptr);
    void startTask(const QString &goal, bool authorized = false);
    void startReadOnlyQuestion(const QString &text);
    AgentWidget *readOnlyWidget() const { return readOnlySidecar; }
    QString conversationId() const { return currentConversationId; }
    bool loadConversation(const QString &id, QString *error = nullptr);

signals:
    void providerUsage(const QJsonObject &usage);

private:
    const AgentTaskRunner::AccessPolicy accessPolicy;
    AgentTaskRunner runner;
    QNetworkAccessManager defaultNetwork;
    QPointer<QNetworkAccessManager> network;
    QPointer<QNetworkReply> reply;
    QPointer<ModelCommandHost> commandHost;
    QPointer<WorkflowProcess> connectedWorkflow;
    QPointer<AgentWidget> readOnlySidecar;
    QPointer<QDialog> readOnlyDialog;
    std::unique_ptr<RAgentConversationStore> conversationStore;
    QString storeWorkspace, storeProjectId, currentConversationId, conversationRevision;
    int conversationMessageCount = 0, historyOffset = 0;
    bool restoringHistory = false, rebindingStore = false;
    bool lastMessagePersisted = false;
    QHash<QString, QJsonObject> taskConversationRoutes, receiptQueries;
    QHash<QString, QString> pendingExecutionReferences;
    QSet<QString> observedExecutionReferences;
    QStringList conversationTaskOrder;
    QComboBox *conversationChoice = nullptr, *branchMessageChoice = nullptr;
    QToolButton *branchConversation = nullptr, *historyMenu = nullptr;
    QPushButton *historyPrevious = nullptr, *historyNext = nullptr, *readOnlyQuestion = nullptr;
    QLabel *historyStatus = nullptr;
    RAgentSettings providerSettings;
    RAgentSettings requestSettingsSnapshot;
    QString selectedEffort = QStringLiteral("off");
    QString requestEffortSnapshot = QStringLiteral("off");
    bool requestStreamingSnapshot = false;
    QComboBox *model;
    QComboBox *effort;
    QCheckBox *streamResponses;
    QPlainTextEdit *goal;
    QLineEdit *key;
    QCheckBox *authorize;
    QPushButton *send, *stop, *apply, *decline;
    QToolButton *newChat, *settingsButton;
    QLabel *state;
    QTextBrowser *transcript, *preview;
    QFrame *approvalCard, *questionCard;
    QLabel *questionText;
    QScrollArea *questionScroll;
    QWidget *questionOptions;
    QGridLayout *questionOptionsLayout;
    QLabel *eventContextSummary, *eventStatus;
    QPlainTextEdit *eventHistory;
    QWidget *activityCards;
    QScrollArea *activityScroll;
    QVBoxLayout *activityCardsLayout;
    QHash<QString, QPointer<QFrame>> activityCardFrames;
    QHash<QString, QPointer<QLabel>> activityCardLabels;
    QStringList activityCardOrder;
    QTimer eventPollTimer;
    QString eventSessionEpoch;
    QString presentedProposalId;
    QString presentedQuestionId;
    quint64 lastEventSeq = 0;
    QString contextSummaryBody;
    bool contextRefreshPending = false;
    quint64 providerRequestGeneration = 0;
    QString activeProviderRequestId;
    QString liveStreamRequestId;
    QString liveStreamText;
    int liveStreamStartPosition = -1;
    int liveStreamTitleStartPosition = -1;
    int liveStreamTitleEndPosition = -1;
    int liveStreamContentStartPosition = -1;
    int liveStreamEndPosition = -1;
    bool finalizingStreamMessage = false;
    QString finalizingStreamText;

    void submitDraft();
    void answerQuestion(const QString &questionId, const QString &answer, bool clearDraft);
    void updateEffortChoices();
    void openSettings();
    void appendText(const QString &title, const QString &text, bool markdown = false);
    void updateAvailability();
    void updateActivityCard(const QString &taskId, const QString &toolCallId,
                            const QString &toolName, const QString &phase,
                            const QString &operationId, const QJsonObject &facts);
    bool isCurrentProviderRequest(const QString &requestId, quint64 generation,
                                  QNetworkReply *expectedReply) const;
    void retireProviderRequest(const QString &requestId = QString());
    void appendStreamText(const QString &requestId, const QString &text);
    void finishStreamText(const QString &requestId, const QString &text, bool accepted);
    void clearStreamText(const QString &requestId);
    bool transcriptFollowsTail() const;
    void restoreTranscriptScroll(bool followTail, int previousValue);
    QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {}) const;
    bool queryFreshContext();
    void markContextStale();
    void appendHostEvent(const QJsonObject &event);
    void persistConversationPreferences();
    bool bindConversationStore();
    void refreshConversationList();
    void createConversation();
    void recordMessage(const QString &role, const QString &text, const QString &taskId = {},
                       const QJsonObject &facts = {});
    void renderHistoryPage(int offset);
    QString lastConversationPreferenceKey() const;
    void branchFromMessage();
    void deleteConversation();
    void exportConversation();
    void clearConversationView();
    bool recordExecutionIntent(const QString &taskId, const QString &origin, const QJsonObject &reference);
    void recordExecutionObservation(const QString &taskId, const QString &origin,
                                    const QJsonObject &reference, const QJsonObject &result);
    void recordReceiptObservation(const QJsonObject &route, const QString &taskId,
                                  const QJsonObject &reference, const QJsonObject &result);
    bool appendExecutionFact(const QJsonObject &route, const QString &taskId,
                             const QString &text, const QJsonObject &facts);
    void reconcileHistoryFacts(const QJsonArray &messages);
    void pruneConversationTaskRoutes();

private slots:
    void refreshHostEvents();
    void requestTurn(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools);
    void onToolActivity(const QString &taskId, const QString &toolCallId, const QString &toolName,
                        const QString &phase, const QString &operationId, const QJsonObject &facts);
};

#endif
