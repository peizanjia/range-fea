#ifndef AGENT_WIDGET_H
#define AGENT_WIDGET_H

#include <QWidget>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <rai_agent_settings.h>
#include "agent_task_runner.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTextBrowser;
class QPushButton;
class QCheckBox;
class QLabel;
class QNetworkReply;

//! Live task UI. Provider credentials remain in memory and are never in task context.
class AgentWidget : public QWidget
{
    Q_OBJECT
public:
    AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent = nullptr);
    AgentTaskRunner *taskRunner() { return &runner; }
    void setProviderSettings(const RAgentSettings &settings);
    void startTask(const QString &goal, bool authorized = false);
signals:
    void providerUsage(const QJsonObject &usage);
private:
    AgentTaskRunner runner;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> reply;
    QPointer<ModelCommandHost> commandHost;
    QComboBox *provider;
    QLineEdit *endpoint, *model, *key;
    QPlainTextEdit *goal;
    QCheckBox *authorize;
    QPushButton *start, *stop, *apply, *decline;
    QLabel *state;
    QTextBrowser *transcript, *preview;
    QLabel *eventContextSummary, *eventStatus;
    QPlainTextEdit *eventHistory;
    QTimer eventPollTimer;
    QString eventSessionEpoch;
    quint64 lastEventSeq = 0;
    QString contextSummaryBody;
    bool contextRefreshPending = false;
    QString presentedProposalId;
    void requestTurn(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools);
    void appendText(const QString &title, const QString &text);
    void updateAvailability();
    QJsonObject hostRequest(const QString &operation, const QJsonObject &parameters = {}) const;
    bool queryFreshContext();
    void markContextStale();
    void appendHostEvent(const QJsonObject &event);

private slots:
    void refreshHostEvents();
};

#endif
