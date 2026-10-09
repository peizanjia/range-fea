#ifndef AGENT_WIDGET_H
#define AGENT_WIDGET_H

#include <QWidget>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTextCursor>
#include <QTimer>
#include <QHash>
#include <QStringList>
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

//! Compact conversation UI. Provider credentials remain in memory only.
class AgentWidget : public QWidget
{
    Q_OBJECT
public:
    //! An injected network manager remains owned by the caller and must outlive this widget.
    AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent = nullptr,
                QNetworkAccessManager *networkAccessManager = nullptr);
    ~AgentWidget() override;
    AgentTaskRunner *taskRunner() { return &runner; }
    void setProviderSettings(const RAgentSettings &settings);
    RAgentSettings getProviderSettings() const;
    static QWidget *createSettingsPage(QWidget *parent = nullptr);
    void startTask(const QString &goal, bool authorized = false);

signals:
    void providerUsage(const QJsonObject &usage);

private:
    AgentTaskRunner runner;
    QNetworkAccessManager defaultNetwork;
    QPointer<QNetworkAccessManager> network;
    QPointer<QNetworkReply> reply;
    QPointer<ModelCommandHost> commandHost;
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

private slots:
    void refreshHostEvents();
    void requestTurn(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools);
    void onToolActivity(const QString &taskId, const QString &toolCallId, const QString &toolName,
                        const QString &phase, const QString &operationId, const QJsonObject &facts);
};

#endif
