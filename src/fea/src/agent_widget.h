#ifndef AGENT_WIDGET_H
#define AGENT_WIDGET_H

#include <QWidget>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
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

//! Compact conversation UI. Provider credentials remain in memory only.
class AgentWidget : public QWidget
{
    Q_OBJECT
public:
    AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent = nullptr);
    AgentTaskRunner *taskRunner() { return &runner; }
    void setProviderSettings(const RAgentSettings &settings);
    RAgentSettings getProviderSettings() const;
    static QWidget *createSettingsPage(QWidget *parent = nullptr);
    void startTask(const QString &goal, bool authorized = false);

signals:
    void providerUsage(const QJsonObject &usage);

private:
    AgentTaskRunner runner;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> reply;
    QPointer<ModelCommandHost> commandHost;
    RAgentSettings providerSettings;
    RAgentSettings requestSettingsSnapshot;
    QString selectedEffort = QStringLiteral("off");
    QString requestEffortSnapshot = QStringLiteral("off");
    QComboBox *model;
    QComboBox *effort;
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
    QTimer eventPollTimer;
    QString eventSessionEpoch;
    QString presentedProposalId;
    QString presentedQuestionId;
    quint64 lastEventSeq = 0;
    QString contextSummaryBody;
    bool contextRefreshPending = false;

    void submitDraft();
    void answerQuestion(const QString &questionId, const QString &answer, bool clearDraft);
    void updateEffortChoices();
    void openSettings();
    void appendText(const QString &title, const QString &text, bool markdown = false);
    void updateAvailability();
    QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {}) const;
    bool queryFreshContext();
    void markContextStale();
    void appendHostEvent(const QJsonObject &event);
    void persistConversationPreferences();

private slots:
    void refreshHostEvents();
    void requestTurn(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools);
};

#endif
