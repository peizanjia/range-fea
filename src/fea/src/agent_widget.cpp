#include "agent_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QSaveFile>
#include <QNetworkReply>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QThread>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextList>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QUrl>
#include <QSharedPointer>
#include <QNetworkRequest>
#include <QTextBlockFormat>
#include <QTextCharFormat>

#include "application.h"
#include "application_settings_dialog.h"
#include "application_settings.h"
#include "main_window.h"
#include "model_command_host.h"
#include "session.h"
#include <ragent_conversation_store.h>
#include <rai_agent.h>
#include <rai_tool_stream.h>

namespace
{
// Model-authored Markdown must never cause local-file or network resource reads.
class ConversationDocument final : public QTextDocument
{
public:
    using QTextDocument::QTextDocument;
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

const QString providerTypeKey = QStringLiteral("aiAgent/providerType");
const QString endpointKey = QStringLiteral("aiAgent/endpoint");
const QString modelKey = QStringLiteral("aiAgent/model");
const QString effortKey = QStringLiteral("aiAgent/effort");
const QString automaticChangesKey = QStringLiteral("aiAgent/automaticChanges");
const QString streamingResponsesKey = QStringLiteral("aiAgent/streamingResponses");
constexpr qsizetype MaxProviderResponseBytes = 1024 * 1024;
constexpr int MaxActivityCards = 64;

AgentWidget *activeAgentWidget()
{
    Application *application = Application::instance();
    MainWindow *window = application ? application->getMainWindow() : nullptr;
    return window ? window->findChild<AgentWidget *>(QStringLiteral("agentTaskWidget")) : nullptr;
}

QSettings *agentPreferences()
{
    Application *application = Application::instance();
    return application ? application->getApplicationSettings() : nullptr;
}

bool isValidAgentEndpoint(const QString &endpoint)
{
    const QUrl url(endpoint.trimmed(), QUrl::StrictMode);
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && !url.host().isEmpty() && !url.authority().contains(QLatin1Char('@'))
        && !url.hasQuery() && !url.hasFragment();
}

bool providerSupportsStreaming(RAgentSettings::Type type)
{
    return type == RAgentSettings::Type::DeepSeek || type == RAgentSettings::Type::OpenAILegacy;
}

QString invalidEndpointMessage()
{
    return QObject::tr("Use an HTTPS endpoint without credentials, a query, or a fragment.");
}

QString optionLabel(const QJsonValue &option)
{
    if (option.isString()) return option.toString();
    const QJsonObject object = option.toObject();
    for (const QString &key : {QStringLiteral("label"), QStringLiteral("title"), QStringLiteral("text"), QStringLiteral("value")})
    {
        const QString value = object.value(key).toVariant().toString();
        if (!value.isEmpty()) return value;
    }
    return QString();
}

QString optionValue(const QJsonValue &option)
{
    if (option.isString()) return option.toString();
    const QJsonObject object = option.toObject();
    for (const QString &key : {QStringLiteral("value"), QStringLiteral("id"), QStringLiteral("label"), QStringLiteral("text")})
    {
        const QString value = object.value(key).toVariant().toString();
        if (!value.isEmpty()) return value;
    }
    return optionLabel(option);
}
}

AgentWidget::AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent,
                         QNetworkAccessManager *networkAccessManager,
                         AgentTaskRunner::AccessPolicy policy, bool restoreLastConversation)
    : QWidget(parent), accessPolicy(policy), runner(host, workflow, this, policy), defaultNetwork(this),
      network(networkAccessManager ? networkAccessManager : &defaultNetwork), commandHost(host),
      connectedWorkflow(workflow),
      providerSettings(RAgentSettings::Type::DeepSeek)
{
    setObjectName(policy == AgentTaskRunner::AccessPolicy::ReadOnly
        ? QStringLiteral("agentReadOnlyTaskWidget") : QStringLiteral("agentTaskWidget"));
    if (QSettings *preferences = agentPreferences())
    {
        const auto requestedType = static_cast<RAgentSettings::Type>(preferences->value(providerTypeKey, int(RAgentSettings::Type::DeepSeek)).toInt());
        if (requestedType == RAgentSettings::Type::DeepSeek || requestedType == RAgentSettings::Type::OpenAILegacy)
            providerSettings.setType(requestedType);
        const RAgentSettings defaults(providerSettings.getType());
        const QString endpoint = preferences->value(endpointKey, defaults.getApiUrl()).toString();
        if (isValidAgentEndpoint(endpoint)) providerSettings.setApiUrl(endpoint.trimmed());
        else
        {
            providerSettings.setApiUrl(defaults.getApiUrl());
            preferences->setValue(endpointKey, defaults.getApiUrl());
        }
        providerSettings.setModel(preferences->value(modelKey, defaults.getModel()).toString());
        selectedEffort = preferences->value(effortKey, QStringLiteral("off")).toString();
    }
    providerSettings.setApiKey(QString());
    if (providerSettings.getType() != RAgentSettings::Type::DeepSeek)
        selectedEffort = QStringLiteral("default");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(policy == AgentTaskRunner::AccessPolicy::ReadOnly ? tr("Read-only question") : tr("Assistant"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAccessibleName(tr("Assistant conversation"));
    header->addWidget(title);
    header->addStretch(1);
    newChat = new QToolButton;
    newChat->setObjectName(QStringLiteral("agentNewChat"));
    newChat->setText(tr("New chat"));
    newChat->setToolButtonStyle(Qt::ToolButtonTextOnly);
    newChat->setAccessibleName(tr("Start a new conversation"));
    header->addWidget(newChat);
    settingsButton = new QToolButton;
    settingsButton->setObjectName(QStringLiteral("agentSettings"));
    settingsButton->setText(tr("Settings"));
    settingsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    settingsButton->setAccessibleName(tr("Open assistant settings"));
    header->addWidget(settingsButton);
    layout->addLayout(header);

    auto *historyRow = new QHBoxLayout;
    conversationChoice = new QComboBox(this);
    conversationChoice->setObjectName(QStringLiteral("agentConversationChoice"));
    conversationChoice->setAccessibleName(tr("Saved conversation"));
    conversationChoice->setMinimumWidth(0);
    historyRow->addWidget(conversationChoice, 1);
    branchConversation = new QToolButton(this);
    branchConversation->setObjectName(QStringLiteral("agentBranchConversation"));
    branchConversation->setText(tr("Branch"));
    branchConversation->setToolTip(tr("Create an independent conversation at the selected historical message. Past actions remain historical facts."));
    historyRow->addWidget(branchConversation);
    historyMenu = new QToolButton(this);
    historyMenu->setObjectName(QStringLiteral("agentConversationMenu"));
    historyMenu->setText(tr("More"));
    historyMenu->setPopupMode(QToolButton::InstantPopup);
    auto *conversationMenu = new QMenu(historyMenu);
    auto *exportAction = conversationMenu->addAction(tr("Export conversation…"));
    exportAction->setObjectName(QStringLiteral("agentExportConversation"));
    auto *deleteAction = conversationMenu->addAction(tr("Delete conversation"));
    deleteAction->setObjectName(QStringLiteral("agentDeleteConversation"));
    historyMenu->setMenu(conversationMenu);
    historyRow->addWidget(historyMenu);
    layout->addLayout(historyRow);
    auto *historyNavigation = new QHBoxLayout;
    branchMessageChoice = new QComboBox(this);
    branchMessageChoice->setObjectName(QStringLiteral("agentBranchMessage"));
    branchMessageChoice->setAccessibleName(tr("Historical message for branching"));
    branchMessageChoice->setMinimumWidth(0);
    historyNavigation->addWidget(branchMessageChoice, 1);
    historyPrevious = new QPushButton(tr("Earlier"), this);
    historyPrevious->setObjectName(QStringLiteral("agentHistoryPrevious"));
    historyNext = new QPushButton(tr("Later"), this);
    historyNext->setObjectName(QStringLiteral("agentHistoryNext"));
    historyNavigation->addWidget(historyPrevious);
    historyNavigation->addWidget(historyNext);
    layout->addLayout(historyNavigation);
    historyStatus = new QLabel(this);
    historyStatus->setObjectName(QStringLiteral("agentHistoryStatus"));
    historyStatus->setWordWrap(true);
    historyStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(historyStatus);
    connect(conversationChoice, &QComboBox::activated, this, [this](int index) {
        if (runner.isActive()) return;
        const QString id = conversationChoice->itemData(index).toString();
        if (id.isEmpty()) createConversation();
        else loadConversation(id);
    });
    connect(branchConversation, &QToolButton::clicked, this, &AgentWidget::branchFromMessage);
    connect(exportAction, &QAction::triggered, this, &AgentWidget::exportConversation);
    connect(deleteAction, &QAction::triggered, this, &AgentWidget::deleteConversation);
    connect(historyPrevious, &QPushButton::clicked, this, [this] { renderHistoryPage(qMax(0, historyOffset - 50)); });
    connect(historyNext, &QPushButton::clicked, this, [this] { renderHistoryPage(historyOffset + 50); });

    auto *hint = new QLabel(tr("Ask about the selected model or supported analysis tasks."));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    state = new QLabel(tr("Ready"));
    state->setObjectName(QStringLiteral("agentConversationStatus"));
    state->setWordWrap(false);
    state->setMinimumHeight(state->fontMetrics().height() + 4);
    state->setMinimumWidth(0);
    state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    state->setToolTip(tr("Conversation status"));
    layout->addWidget(state);

    activityScroll = new QScrollArea(this);
    activityScroll->setObjectName(QStringLiteral("agentActivityHistory"));
    activityScroll->setWidgetResizable(true);
    activityScroll->setFrameShape(QFrame::NoFrame);
    activityScroll->setMaximumHeight(150);
    activityScroll->setAccessibleName(tr("Trusted tool activity history"));
    activityCards = new QWidget(activityScroll);
    activityCards->setObjectName(QStringLiteral("agentActivityCards"));
    activityCardsLayout = new QVBoxLayout(activityCards);
    activityCardsLayout->setContentsMargins(0, 0, 0, 0);
    activityCardsLayout->setSpacing(4);
    activityCardsLayout->addStretch(1);
    activityScroll->setWidget(activityCards);
    activityScroll->hide();
    layout->addWidget(activityScroll);

    auto *eventToggle = new QToolButton;
    eventToggle->setObjectName(QStringLiteral("agentEventHistoryToggle"));
    eventToggle->setText(tr("Recent GUI document events"));
    eventToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    eventToggle->setArrowType(Qt::RightArrow);
    eventToggle->setCheckable(true);
    eventToggle->setChecked(false);
    layout->addWidget(eventToggle);
    auto *eventBody = new QWidget;
    eventBody->setObjectName(QStringLiteral("agentEventHistoryBody"));
    auto *eventLayout = new QVBoxLayout(eventBody);
    eventLayout->setContentsMargins(0, 0, 0, 0);
    auto *eventNotice = new QLabel(tr("In-memory session event history (up to 256 records); this is not a durable audit log."));
    eventNotice->setObjectName(QStringLiteral("agentEventHistoryNotice"));
    eventNotice->setWordWrap(true);
    eventLayout->addWidget(eventNotice);
    eventContextSummary = new QLabel(tr("Current document context has not been queried yet."));
    eventContextSummary->setObjectName(QStringLiteral("agentEventContextSummary"));
    eventContextSummary->setWordWrap(true);
    eventLayout->addWidget(eventContextSummary);
    eventStatus = new QLabel(tr("Waiting for host events."));
    eventStatus->setObjectName(QStringLiteral("agentEventStatus"));
    eventStatus->setWordWrap(true);
    eventLayout->addWidget(eventStatus);
    eventHistory = new QPlainTextEdit;
    eventHistory->setObjectName(QStringLiteral("agentEventHistory"));
    eventHistory->setReadOnly(true);
    eventHistory->setMaximumHeight(150);
    eventHistory->setMaximumBlockCount(256);
    eventLayout->addWidget(eventHistory);
    eventBody->hide();
    eventBody->setMaximumHeight(280);
    layout->addWidget(eventBody);
    connect(eventToggle, &QToolButton::toggled, this, [eventToggle, eventBody](bool expanded) {
        eventBody->setVisible(expanded);
        eventToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });

    transcript = new QTextBrowser;
    transcript->setDocument(new ConversationDocument(transcript));
    transcript->setObjectName(QStringLiteral("agentTranscript"));
    transcript->setReadOnly(true);
    transcript->setOpenExternalLinks(false);
    transcript->setOpenLinks(false);
    transcript->setAccessibleName(tr("Conversation transcript"));
    transcript->setMinimumWidth(0);
    layout->addWidget(transcript, 1);

    approvalCard = new QFrame;
    approvalCard->setObjectName(QStringLiteral("agentApprovalCard"));
    approvalCard->setFrameShape(QFrame::StyledPanel);
    auto *approvalLayout = new QVBoxLayout(approvalCard);
    approvalLayout->setContentsMargins(8, 6, 8, 6);
    auto *approvalTitle = new QLabel(tr("Review proposed change"));
    QFont cardFont = approvalTitle->font();
    cardFont.setBold(true);
    approvalTitle->setFont(cardFont);
    approvalLayout->addWidget(approvalTitle);
    preview = new QTextBrowser;
    preview->setObjectName(QStringLiteral("agentPreview"));
    preview->setReadOnly(true);
    preview->setMaximumHeight(110);
    approvalLayout->addWidget(preview);
    auto *approvalButtons = new QHBoxLayout;
    apply = new QPushButton(tr("Apply"));
    apply->setObjectName(QStringLiteral("agentApproveProposal"));
    apply->setAccessibleName(tr("Apply the displayed proposal"));
    decline = new QPushButton(tr("Reject"));
    decline->setObjectName(QStringLiteral("agentRejectProposal"));
    decline->setAccessibleName(tr("Reject the displayed proposal"));
    approvalButtons->addWidget(apply);
    approvalButtons->addWidget(decline);
    approvalLayout->addLayout(approvalButtons);
    approvalCard->hide();
    layout->addWidget(approvalCard);

    questionCard = new QFrame;
    questionCard->setObjectName(QStringLiteral("agentQuestionCard"));
    questionCard->setFrameShape(QFrame::StyledPanel);
    auto *questionLayout = new QVBoxLayout(questionCard);
    questionLayout->setContentsMargins(8, 6, 8, 6);
    auto *questionTitle = new QLabel(tr("A question needs your answer"));
    questionTitle->setFont(cardFont);
    questionLayout->addWidget(questionTitle);
    questionText = new QLabel;
    questionText->setObjectName(QStringLiteral("agentQuestionText"));
    questionText->setWordWrap(true);
    questionText->setTextFormat(Qt::PlainText);
    questionScroll = new QScrollArea(questionCard);
    questionScroll->setWidgetResizable(true);
    questionScroll->setMaximumHeight(100);
    questionScroll->setFrameShape(QFrame::NoFrame);
    questionScroll->setWidget(questionText);
    questionLayout->addWidget(questionScroll);
    questionOptions = new QWidget(questionCard);
    questionOptionsLayout = new QGridLayout(questionOptions);
    questionOptionsLayout->setContentsMargins(0, 0, 0, 0);
    questionOptionsLayout->setHorizontalSpacing(6);
    questionOptionsLayout->setVerticalSpacing(4);
    questionLayout->addWidget(questionOptions);
    questionCard->hide();
    layout->addWidget(questionCard);

    auto *modelRow = new QHBoxLayout;
    auto *modelLabel = new QLabel(tr("Model"));
    model = new QComboBox;
    model->setObjectName(QStringLiteral("agentModel"));
    model->setEditable(true);
    model->setInsertPolicy(QComboBox::NoInsert);
    model->setAccessibleName(tr("Assistant model"));
    model->lineEdit()->setObjectName(QStringLiteral("agentModelInput"));
    model->lineEdit()->setClearButtonEnabled(false);
    model->setMinimumWidth(80);
    model->setMaximumWidth(180);
    model->setEditText(providerSettings.getModel());
    modelRow->addWidget(modelLabel);
    modelRow->addWidget(model, 2);
    auto *effortLabel = new QLabel(tr("Thinking"));
    effort = new QComboBox;
    effort->setObjectName(QStringLiteral("agentThinkingEffort"));
    effort->setAccessibleName(tr("DeepSeek thinking effort"));
    effort->setMaximumWidth(100);
    updateEffortChoices();
    modelRow->addWidget(effortLabel);
    modelRow->addWidget(effort, 1);
    streamResponses = new QCheckBox(tr("Stream"));
    streamResponses->setObjectName(QStringLiteral("agentStreamResponses"));
    streamResponses->setAccessibleName(tr("Show provider text as it arrives"));
    streamResponses->setToolTip(tr("When enabled, supported Chat Completions providers stream text. Complete JSON responses remain available when streaming is off."));
    if (QSettings *preferences = agentPreferences())
        streamResponses->setChecked(preferences->value(streamingResponsesKey, false).toBool());
    modelRow->addWidget(streamResponses);
    authorize = new QCheckBox(tr("Auto"));
    authorize->setObjectName(QStringLiteral("agentAutomaticChanges"));
    authorize->setAccessibleName(tr("Allow supported changes to run automatically"));
    authorize->setToolTip(tr("Limits automatic changes to documents selected when the task starts and the connected reference workspace. Extension proposals still require review."));
    if (QSettings *preferences = agentPreferences())
        authorize->setChecked(preferences->value(automaticChangesKey, false).toBool());
    modelRow->addWidget(authorize);
    if (policy == AgentTaskRunner::AccessPolicy::ReadOnly)
    {
        authorize->setChecked(false);
        authorize->hide();
        streamResponses->setChecked(false);
        streamResponses->hide();
        settingsButton->hide();
    }

    goal = new QPlainTextEdit;
    goal->setObjectName(QStringLiteral("agentGoal"));
    goal->setAccessibleName(tr("Message to the assistant"));
    goal->setPlaceholderText(tr("Message the assistant…"));
    goal->setMinimumHeight(64);
    goal->setMaximumHeight(92);
    layout->addWidget(goal);

    // Preserve the historic widget lookup while keeping the credential out of
    // the conversation sidebar and all QSettings values.
    key = new QLineEdit(this);
    key->setObjectName(QStringLiteral("agentApiKey"));
    key->setEchoMode(QLineEdit::Password);
    key->setAccessibleName(tr("Assistant API key"));
    key->hide();

    auto *composerButtons = new QHBoxLayout;
    send = new QPushButton(tr("Send"));
    send->setObjectName(QStringLiteral("agentSend"));
    send->setAccessibleName(tr("Send message to the assistant"));
    send->setDefault(true);
    stop = new QPushButton(tr("Stop"));
    stop->setObjectName(QStringLiteral("agentStop"));
    stop->setAccessibleName(tr("Stop assistant work"));
    composerButtons->addWidget(send, 1);
    composerButtons->addWidget(stop);
    readOnlyQuestion = new QPushButton(tr("Read-only question"), this);
    readOnlyQuestion->setObjectName(QStringLiteral("agentAskReadOnly"));
    readOnlyQuestion->setAccessibleName(tr("Ask independently without taking over the current task"));
    readOnlyQuestion->setVisible(policy == AgentTaskRunner::AccessPolicy::ReadWrite);
    composerButtons->addWidget(readOnlyQuestion);
    layout->addLayout(composerButtons);
    layout->addLayout(modelRow);

    connect(newChat, &QToolButton::clicked, this, [this] {
        if (runner.isActive()) return;
        runner.clearConversation();
        currentConversationId.clear();
        conversationRevision.clear();
        conversationMessageCount = 0;
        createConversation();
        transcript->clear();
        for (const QString &cardKey : activityCardOrder)
        {
            if (activityCardFrames.value(cardKey)) activityCardFrames.value(cardKey)->deleteLater();
        }
        activityCardFrames.clear();
        activityCardLabels.clear();
        activityCardOrder.clear();
        activityScroll->hide();
        goal->clear();
        preview->clear();
        presentedProposalId.clear();
        presentedQuestionId.clear();
        questionText->clear();
        state->setText(tr("Ready"));
        updateAvailability();
        goal->setFocus(Qt::OtherFocusReason);
    });
    connect(settingsButton, &QToolButton::clicked, this, &AgentWidget::openSettings);
    connect(send, &QPushButton::clicked, this, &AgentWidget::submitDraft);
    connect(readOnlyQuestion, &QPushButton::clicked, this, [this] { startReadOnlyQuestion(goal->toPlainText()); });
    connect(goal, &QPlainTextEdit::textChanged, this, [this] { updateAvailability(); });
    auto *sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), goal);
    connect(sendShortcut, &QShortcut::activated, this, &AgentWidget::submitDraft);
    auto *sendShortcutEnter = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), goal);
    connect(sendShortcutEnter, &QShortcut::activated, this, &AgentWidget::submitDraft);
    connect(stop, &QPushButton::clicked, &runner, &AgentTaskRunner::cancel);
    connect(apply, &QPushButton::clicked, this, [this] {
        if (presentedProposalId.isEmpty()) return;
        runner.authorizePending(presentedProposalId, true);
        updateAvailability();
    });
    connect(decline, &QPushButton::clicked, this, [this] {
        if (presentedProposalId.isEmpty()) return;
        runner.authorizePending(presentedProposalId, false);
        updateAvailability();
    });
    connect(model, &QComboBox::currentTextChanged, this, [this](const QString &value) {
        providerSettings.setModel(value.trimmed());
        persistConversationPreferences();
    });
    connect(model->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        providerSettings.setModel(model->currentText().trimmed());
        persistConversationPreferences();
    });
    connect(effort, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        selectedEffort = effort->currentData().toString();
        persistConversationPreferences();
    });
    connect(streamResponses, &QCheckBox::toggled, this, [this] { persistConversationPreferences(); });
    connect(authorize, &QCheckBox::toggled, this, [this] { persistConversationPreferences(); });

    connect(&runner, &AgentTaskRunner::turnRequested, this, &AgentWidget::requestTurn);
    connect(&runner, &AgentTaskRunner::taskStarted, this, [this](const QString &taskId, const QString &text, bool) {
        recordMessage(QStringLiteral("user"), text, taskId);
        pruneConversationTaskRoutes();
        if (lastMessagePersisted && taskConversationRoutes.size() < 128)
        {
            taskConversationRoutes.insert(taskId, QJsonObject{{QStringLiteral("workspace"), storeWorkspace},
                {QStringLiteral("projectId"), storeProjectId}, {QStringLiteral("conversationId"), currentConversationId}});
            conversationTaskOrder.append(taskId);
        }
    });
    runner.setExecutionRecorder([this](const QString &taskId, const QString &origin, const QJsonObject &reference) {
        return recordExecutionIntent(taskId, origin, reference);
    });
    connect(&runner, &AgentTaskRunner::executionObserved, this, &AgentWidget::recordExecutionObservation);
    connect(&runner, &AgentTaskRunner::executionObservationUnknown, this,
        [this](const QString &taskId, const QString &, const QJsonObject &reference, const QString &code) {
            auto facts = reference;
            const bool notSubmitted = code == QStringLiteral("NOT_SUBMITTED");
            const bool rejected = notSubmitted || code.startsWith(QStringLiteral("REJECTED:"));
            facts.insert(QStringLiteral("status"), rejected ? QStringLiteral("rejected") : QStringLiteral("unknown"));
            facts.insert(QStringLiteral("code"), notSubmitted ? code : rejected ? code.mid(9).left(128)
                                                                               : QStringLiteral("RESULT_RECONCILE_REQUIRED"));
            if (appendExecutionFact(taskConversationRoutes.value(taskId), taskId,
                notSubmitted ? tr("The recorded execution was not submitted; no action was retried.")
                    : rejected ? tr("The host rejected the recorded execution. No retry was authorized.")
                               : tr("The execution outcome is unknown. Query its recorded reference before another action."), facts)
                && rejected)
                pendingExecutionReferences.remove(taskId + QStringLiteral(":")
                    + reference.value(QStringLiteral("idempotencyKey")).toString(reference.value(QStringLiteral("requestSha256")).toString()));
        });
    if (workflow)
        connect(workflow, &WorkflowProcess::replyReceived, this,
            [this](const QString &id, const QString &operation, const QJsonObject &result) {
                const auto pending = receiptQueries.take(id);
                if (pending.isEmpty() || operation != QStringLiteral("execution.query")) return;
                const auto reference = pending.value(QStringLiteral("reference")).toObject();
                if (result.value(QStringLiteral("projectId")) != reference.value(QStringLiteral("workflowProjectId"))
                    || result.value(QStringLiteral("requestSha256")) != reference.value(QStringLiteral("requestSha256"))) return;
                if (result.value(QStringLiteral("accepted")).toBool())
                    recordReceiptObservation(pending.value(QStringLiteral("route")).toObject(),
                        pending.value(QStringLiteral("taskId")).toString(), reference, result);
            });
    if (workflow)
        connect(workflow, &WorkflowProcess::requestFailed, this,
            [this](const QString &id, const QString &, const QString &, const QString &) { receiptQueries.remove(id); });
    connect(&runner, &AgentTaskRunner::providerCancellationRequested, this, [this](const QString &id) {
        retireProviderRequest(id);
    });
    connect(&runner, &AgentTaskRunner::toolActivity, this, &AgentWidget::onToolActivity);
    connect(&runner, &AgentTaskRunner::stateChanged, this, [this](const QString &status, const QString &message) {
        const QMap<QString, QString> labels{{QStringLiteral("analyzing"), tr("Thinking")},
            {QStringLiteral("awaiting_input"), tr("Waiting for your answer")},
            {QStringLiteral("awaiting_authorization"), tr("Needs approval")},
            {QStringLiteral("executing"), tr("Working")}, {QStringLiteral("waiting"), tr("Waiting for calculation")},
            {QStringLiteral("completed"), tr("Completed")}, {QStringLiteral("failed"), tr("Request failed")},
            {QStringLiteral("budget_exhausted"), tr("Task limit reached")},
            {QStringLiteral("cancelled"), tr("Stopped")}, {QStringLiteral("superseded"), tr("Continuing")}};
        state->setText(labels.value(status, message.left(100)));
        state->setToolTip(message);
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::assistantMessage, this, [this](const QString &message) {
        const QString previousProjectId = storeProjectId;
        const QString previousWorkspace = storeWorkspace;
        const bool bound = bindConversationStore();
        if (!previousProjectId.isEmpty() && (!bound || previousProjectId != storeProjectId || previousWorkspace != storeWorkspace)) return;
        recordMessage(QStringLiteral("assistant"), message, runner.taskId());
        if (finalizingStreamMessage && message == finalizingStreamText)
        {
            const QString requestId = liveStreamRequestId;
            finalizingStreamMessage = false;
            finalizingStreamText.clear();
            finishStreamText(requestId, message, true);
            return;
        }
        if (!message.isEmpty()) appendText(tr("Assistant"), message, true);
    });
    connect(&runner, &AgentTaskRunner::userInputRequested, this,
            [this](const QString &id, const QString &question, const QJsonArray &options) {
        const QString previousProjectId = storeProjectId;
        const QString previousWorkspace = storeWorkspace;
        const bool bound = bindConversationStore();
        if (!previousProjectId.isEmpty() && (!bound || previousProjectId != storeProjectId || previousWorkspace != storeWorkspace)) return;
        presentedQuestionId = id;
        questionText->setText(question);
        recordMessage(QStringLiteral("assistant"), question, runner.taskId());
        appendText(tr("Assistant"), question);
        while (QLayoutItem *item = questionOptionsLayout->takeAt(0))
        {
            if (QWidget *widget = item->widget()) widget->deleteLater();
            delete item;
        }
        int index = 0;
        for (const QJsonValue &option : options)
        {
            const QString label = optionLabel(option);
            const QString value = optionValue(option);
            if (label.isEmpty() || value.isEmpty()) continue;
            auto *button = new QPushButton(label, questionOptions);
            button->setText(button->fontMetrics().elidedText(label, Qt::ElideRight, 170));
            button->setToolTip(label);
            button->setAccessibleName(tr("Answer: %1").arg(label));
            button->setMinimumWidth(0);
            button->setMaximumWidth(180);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            const QString questionId = id;
            connect(button, &QPushButton::clicked, this, [this, questionId, value] {
                answerQuestion(questionId, value, false);
            });
            questionOptionsLayout->addWidget(button, index / 2, index % 2);
            ++index;
        }
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::proposalReady, this,
            [this](const QString &origin, const QString &id, const QJsonObject &value) {
        presentedProposalId = id;
        const auto normalized = value.value("normalized").toObject();
        const auto command = origin == "document" ? value.value("command").toString() : normalized.value("command").toString();
        QStringList lines;
        const QMap<QString, QString> labels{{"analysis.modal.configure", tr("Change modal settings")},
            {"document.undo", tr("Undo last document change")}, {"document.redo", tr("Redo document change")},
            {"workflow.configure", tr("Configure reference experiment")}, {"workflow.run", tr("Run reference experiment")},
            {"workflow.resume", tr("Continue reference calculation")},
            {"extension.stage", tr("Stage extension source and tests for review")},
            {"extension.validate", tr("Execute extension tests in trusted developer mode")},
            {"extension.activate", tr("Activate validated extension version")},
            {"extension.rollback", tr("Restore retained extension version")},
            {"extension.run", tr("Run validated extension")}};
        lines << labels.value(command, command);
        const auto before = value.value("before").toObject();
        const auto after = origin == "document" ? value.value("after").toObject() : normalized.value("parameters").toObject();
        const QMap<QString, QString> fieldLabels{{"iterations", tr("Iterations")}, {"method", tr("Method")},
            {"modes", tr("Modes")}, {"tolerance", tr("Tolerance")}, {"unit", tr("Unit")},
            {"model", tr("Reference model")}, {"dt", tr("Time step (s)")}, {"steps", tr("Steps")},
            {"targetDamping", tr("Target damping ratio")}, {"mode", tr("Control comparison")},
            {"additionalSteps", tr("Additional steps")}, {"independentReference", tr("Independent numerical check")}};
        for (auto it = after.begin(); it != after.end(); ++it)
        {
            if (before.contains(it.key()) && before.value(it.key()) == it.value()) continue;
            const auto rendered = it.value().isObject() || it.value().isArray()
                ? (origin == "extension"
                    ? QString::fromUtf8((it.value().isObject() ? QJsonDocument(it.value().toObject())
                                                              : QJsonDocument(it.value().toArray())).toJson(QJsonDocument::Indented))
                    : tr("Structured data; see task description")) : it.value().toVariant().toString();
            lines << (before.contains(it.key())
                ? tr("%1: %2 → %3").arg(fieldLabels.value(it.key(), it.key()), before.value(it.key()).toVariant().toString(), rendered)
                : tr("%1: %2").arg(fieldLabels.value(it.key(), it.key()), rendered));
        }
        if (before.contains("historyAction")) lines << before.value("historyAction").toString();
        preview->setPlainText(lines.join('\n'));
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::actionCompleted, this, [this](const QString &origin, const QJsonObject &value) {
        const bool nativeTask = origin == "document" && value.value("command").toString().startsWith("analysis.")
            && (value.value("command") == QJsonValue("analysis.run") || value.value("command") == QJsonValue("analysis.recover")
                || value.value("command") == QJsonValue("analysis.cancel"));
        appendText(nativeTask ? tr("Native task action accepted") : origin == "document" ? tr("Document updated") : tr("Task submitted"),
                   nativeTask ? tr("The host accepted the task action. Query its actual progress and terminal result in Document FE output.")
                     : origin == "document" ? tr("The host accepted the change. The document views now reflect its current state.")
                                        : tr("Calculation task: %1. Progress and results are available in the workflow panel.").arg(value.value("jobId").toString()));
        presentedProposalId.clear();
        preview->clear();
        updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::finished, this, [this](const QString &status, const QString &message) {
        recordMessage(QStringLiteral("fact"), tr("Assistant task ended: %1. This does not determine a solver job's result.").arg(status),
                      runner.taskId(), QJsonObject{{QStringLiteral("status"), status}});
        if (status != QStringLiteral("completed") && !message.isEmpty()) appendText(status, message);
        if (status == QStringLiteral("completed")) state->setText(tr("Completed"));
        presentedProposalId.clear();
        presentedQuestionId.clear();
        preview->clear();
        updateAvailability();
    });

    eventPollTimer.setInterval(750);
    connect(&eventPollTimer, &QTimer::timeout, this, &AgentWidget::refreshHostEvents);
    eventPollTimer.start();
    refreshConversationList();
    if (restoreLastConversation)
        if (QSettings *preferences = agentPreferences())
            loadConversation(preferences->value(lastConversationPreferenceKey()).toString());
    refreshHostEvents();
    updateAvailability();
}

AgentWidget::~AgentWidget()
{
    retireProviderRequest();
}

QString AgentWidget::lastConversationPreferenceKey() const
{
    return QStringLiteral("aiAgent/lastConversation/") + storeProjectId
        + (accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly ? QStringLiteral("/readOnly") : QStringLiteral("/primary"));
}

void AgentWidget::pruneConversationTaskRoutes()
{
    QSet<QString> protectedTasks;
    for (const auto &task : pendingExecutionReferences) protectedTasks.insert(task);
    for (const auto &pending : receiptQueries) protectedTasks.insert(pending.value(QStringLiteral("taskId")).toString());
    if (runner.isActive()) protectedTasks.insert(runner.taskId());
    for (qsizetype index = 0; taskConversationRoutes.size() >= 96 && index < conversationTaskOrder.size();)
    {
        const QString task = conversationTaskOrder.at(index);
        if (protectedTasks.contains(task)) { ++index; continue; }
        taskConversationRoutes.remove(task);
        conversationTaskOrder.removeAt(index);
        for (auto it = observedExecutionReferences.begin(); it != observedExecutionReferences.end();)
            if (it->contains(QStringLiteral(":") + task + QStringLiteral(":"))) it = observedExecutionReferences.erase(it);
            else ++it;
    }
}

void AgentWidget::clearConversationView()
{
    runner.clearConversation();
    transcript->clear();
    preview->clear();
    presentedProposalId.clear(); presentedQuestionId.clear();
    questionText->clear(); approvalCard->hide(); questionCard->hide();
    branchMessageChoice->clear();
    for (const auto &key : activityCardOrder)
        if (activityCardFrames.value(key)) activityCardFrames.value(key)->deleteLater();
    activityCardFrames.clear(); activityCardLabels.clear(); activityCardOrder.clear();
    activityScroll->hide();
}

bool AgentWidget::bindConversationStore()
{
    if (rebindingStore || !commandHost) return false;
    const auto projectResponse = hostRequest(QStringLiteral("project.query"));
    const auto project = projectResponse.value(QStringLiteral("result")).toObject();
    const QString workspace = commandHost->projectWorkspace();
    const QString projectId = project.value(QStringLiteral("projectId")).toString();
    if (projectResponse.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("ok"))
        || workspace.isEmpty() || projectId.isEmpty())
    {
        if (conversationStore)
        {
            rebindingStore = true;
            if (runner.isActive()) runner.cancel();
            retireProviderRequest();
            clearConversationView();
            conversationStore.reset(); storeWorkspace.clear(); storeProjectId.clear();
            currentConversationId.clear(); conversationRevision.clear(); conversationMessageCount = 0;
            const QSignalBlocker blocker(conversationChoice);
            conversationChoice->clear();
            historyStatus->setText(tr("Project history is unavailable. Previous records were preserved."));
            rebindingStore = false;
        }
        return false;
    }
    if (conversationStore && workspace == storeWorkspace && projectId == storeProjectId) return true;
    rebindingStore = true;
    if (conversationStore && runner.isActive()) runner.cancel();
    retireProviderRequest();
    clearConversationView();
    conversationStore = std::make_unique<RAgentConversationStore>();
    QString error;
    const bool configured = conversationStore->configure(workspace, projectId, &error);
    storeWorkspace = configured ? workspace : QString();
    storeProjectId = configured ? projectId : QString();
    currentConversationId.clear();
    conversationRevision.clear();
    conversationMessageCount = 0;
    if (!configured)
    {
        conversationStore.reset();
        historyStatus->setText(tr("Conversation storage is unavailable. Visible messages remain in this window."));
    }
    rebindingStore = false;
    return configured;
}

void AgentWidget::refreshConversationList()
{
    if (!bindConversationStore()) return;
    QString error;
    const auto listing = conversationStore->list(0, 32, &error);
    if (!error.isEmpty())
    {
        historyStatus->setText(tr("Saved history could not be verified. Existing records were preserved."));
        return;
    }
    const QSignalBlocker blocker(conversationChoice);
    conversationChoice->clear();
    conversationChoice->addItem(tr("New conversation"), QString());
    for (const auto &value : listing.value(QStringLiteral("conversations")).toArray())
    {
        const auto summary = value.toObject();
        const QString id = summary.value(QStringLiteral("conversationId")).toString();
        conversationChoice->addItem(summary.value(QStringLiteral("title")).toString().left(100)
            + QStringLiteral(" · ") + id.left(8), id);
    }
    const int current = conversationChoice->findData(currentConversationId);
    if (current >= 0) conversationChoice->setCurrentIndex(current);
}

void AgentWidget::createConversation()
{
    if (runner.isActive() || !bindConversationStore()) return;
    QString error;
    const auto summary = conversationStore->create(accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly
        ? tr("Read-only question") : tr("Conversation"), &error);
    if (summary.isEmpty())
    {
        historyStatus->setText(tr("A new conversation could not be saved. Existing history was preserved."));
        return;
    }
    clearConversationView();
    currentConversationId = summary.value(QStringLiteral("conversationId")).toString();
    conversationRevision = summary.value(QStringLiteral("revision")).toString();
    conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
    if (QSettings *preferences = agentPreferences()) preferences->setValue(lastConversationPreferenceKey(), currentConversationId);
    historyOffset = 0;
    branchMessageChoice->clear();
    refreshConversationList();
    updateAvailability();
}

void AgentWidget::recordMessage(const QString &role, const QString &text, const QString &taskId,
                                const QJsonObject &facts)
{
    lastMessagePersisted = false;
    if (restoringHistory || rebindingStore || text.isEmpty()) return;
    const QString previousProjectId = storeProjectId;
    const QString previousWorkspace = storeWorkspace;
    if (!bindConversationStore() || (!previousProjectId.isEmpty()
        && (previousProjectId != storeProjectId || previousWorkspace != storeWorkspace))) return;
    conversationStore->setSensitiveValues({getProviderSettings().getApiKey(), requestSettingsSnapshot.getApiKey()});
    if (currentConversationId.isEmpty())
    {
        QString error;
        const auto summary = conversationStore->create(accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly
            ? tr("Read-only question") : tr("Conversation"), &error);
        if (summary.isEmpty()) { historyStatus->setText(tr("This conversation could not be saved.")); return; }
        currentConversationId = summary.value(QStringLiteral("conversationId")).toString();
        conversationRevision = summary.value(QStringLiteral("revision")).toString();
        conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
    }
    QJsonObject entry{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                      {QStringLiteral("role"), role}, {QStringLiteral("text"), text}};
    if (!taskId.isEmpty()) entry.insert(QStringLiteral("taskId"), taskId);
    if (!facts.isEmpty()) entry.insert(QStringLiteral("facts"), facts);
    QString error;
    QJsonObject summary;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        summary = conversationStore->append(currentConversationId, conversationRevision, QJsonArray{entry}, &error);
        if (!summary.isEmpty()) break;
        const auto current = conversationStore->page(currentConversationId, 0, 1, &error);
        const QString revision = current.value(QStringLiteral("revision")).toString();
        if (current.isEmpty() || revision == conversationRevision) break;
        conversationRevision = revision;
    }
    if (summary.isEmpty())
    {
        historyStatus->setText(tr("This message remains visible but could not be persisted. Existing history was preserved."));
        return;
    }
    conversationRevision = summary.value(QStringLiteral("revision")).toString();
    conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
    lastMessagePersisted = true;
    historyOffset = qMax(0, conversationMessageCount - 50);
    branchMessageChoice->addItem(role + QStringLiteral(": ") + text.left(60), entry.value(QStringLiteral("messageId")).toString());
    if (branchMessageChoice->count() > 50) branchMessageChoice->removeItem(0);
    branchMessageChoice->setCurrentIndex(branchMessageChoice->count() - 1);
    if (QSettings *preferences = agentPreferences()) preferences->setValue(lastConversationPreferenceKey(), currentConversationId);
    historyStatus->setText(tr("Saved conversation · %1 messages. Historical text grants no approval.").arg(conversationMessageCount));
    refreshConversationList();
    updateAvailability();
}

bool AgentWidget::appendExecutionFact(const QJsonObject &route, const QString &taskId,
                                     const QString &text, const QJsonObject &facts)
{
    if (route.isEmpty() || taskId.isEmpty()) return false;
    RAgentConversationStore store;
    QString error;
    if (!store.configure(route.value(QStringLiteral("workspace")).toString(),
                         route.value(QStringLiteral("projectId")).toString(), &error)) return false;
    const QString id = route.value(QStringLiteral("conversationId")).toString();
    const auto page = store.page(id, 0, 1, &error);
    if (page.isEmpty()) return false;
    const QJsonObject entry{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("taskId"), taskId}, {QStringLiteral("role"), QStringLiteral("fact")},
        {QStringLiteral("text"), text}, {QStringLiteral("facts"), facts}};
    const auto summary = store.append(id, page.value(QStringLiteral("revision")).toString(), QJsonArray{entry}, &error);
    if (summary.isEmpty()) return false;
    if (route.value(QStringLiteral("workspace")).toString() == storeWorkspace
        && route.value(QStringLiteral("projectId")).toString() == storeProjectId && id == currentConversationId)
    {
        conversationRevision = summary.value(QStringLiteral("revision")).toString();
        conversationMessageCount = summary.value(QStringLiteral("messageCount")).toInt();
        historyStatus->setText(tr("Execution evidence was recorded in this conversation. Historical references grant no permission."));
    }
    return true;
}

bool AgentWidget::recordExecutionIntent(const QString &taskId, const QString &, const QJsonObject &reference)
{
    auto facts = reference;
    facts.insert(QStringLiteral("status"), QStringLiteral("pending"));
    facts.insert(QStringLiteral("code"), QStringLiteral("OUTCOME_NOT_OBSERVED"));
    const bool recorded = appendExecutionFact(taskConversationRoutes.value(taskId), taskId,
        tr("Execution intent recorded; acceptance is not yet observed. Query this reference before retrying."), facts);
    if (recorded) pendingExecutionReferences.insert(taskId + QStringLiteral(":")
        + reference.value(QStringLiteral("idempotencyKey")).toString(reference.value(QStringLiteral("requestSha256")).toString()), taskId);
    return recorded;
}

void AgentWidget::recordExecutionObservation(const QString &taskId, const QString &,
                                             const QJsonObject &reference, const QJsonObject &result)
{
    recordReceiptObservation(taskConversationRoutes.value(taskId), taskId, reference, result);
}

void AgentWidget::recordReceiptObservation(const QJsonObject &route, const QString &taskId,
                                            const QJsonObject &reference, const QJsonObject &result)
{
    if (route.isEmpty()) return;
    const QString key = route.value(QStringLiteral("workspace")).toString() + QStringLiteral(":")
        + route.value(QStringLiteral("conversationId")).toString() + QStringLiteral(":")
        + taskId + QStringLiteral(":") + reference.value(QStringLiteral("idempotencyKey"))
        .toString(reference.value(QStringLiteral("requestSha256")).toString());
    if (observedExecutionReferences.contains(key)) return;
    QJsonObject facts = reference;
    facts.insert(QStringLiteral("status"), QStringLiteral("accepted"));
    facts.insert(QStringLiteral("code"), QStringLiteral("OK"));
    for (const auto &field : {QStringLiteral("command"), QStringLiteral("operationId"), QStringLiteral("jobId"),
                             QStringLiteral("sourceDocumentId"), QStringLiteral("sourceRevision"),
                             QStringLiteral("acceptedModelSha256")})
        if (result.contains(field) && !result.value(field).toVariant().toString().isEmpty())
            facts.insert(field, result.value(field).toVariant().toString());
    const auto snapshot = result.value(QStringLiteral("acceptedSnapshotRef")).toObject();
    if (!snapshot.value(QStringLiteral("modelSha256")).toString().isEmpty())
        facts.insert(QStringLiteral("acceptedModelSha256"), snapshot.value(QStringLiteral("modelSha256")));
    if (appendExecutionFact(route, taskId,
        tr("The host accepted the referenced action. A submitted job's numerical result must be queried separately."), facts))
    {
        observedExecutionReferences.insert(key);
        pendingExecutionReferences.remove(taskId + QStringLiteral(":")
            + reference.value(QStringLiteral("idempotencyKey")).toString(reference.value(QStringLiteral("requestSha256")).toString()));
        runner.resolveExecutionObservation(taskId, reference);
    }
}

void AgentWidget::reconcileHistoryFacts(const QJsonArray &messages)
{
    const QJsonObject route{{QStringLiteral("workspace"), storeWorkspace}, {QStringLiteral("projectId"), storeProjectId},
                            {QStringLiteral("conversationId"), currentConversationId}};
    QString error;
    const auto complete = conversationStore ? conversationStore->exportConversation(currentConversationId, &error) : QJsonObject{};
    QSet<QString> resolved;
    for (const auto &value : complete.value(QStringLiteral("messages")).toArray())
    {
        const auto entry = value.toObject();
        const auto facts = entry.value(QStringLiteral("facts")).toObject();
        if (facts.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("accepted"))
            || facts.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("rejected")))
            resolved.insert(entry.value(QStringLiteral("taskId")).toString() + QStringLiteral(":")
                + facts.value(QStringLiteral("idempotencyKey")).toString(facts.value(QStringLiteral("requestSha256")).toString()));
    }
    for (const auto &value : messages)
    {
        const auto entry = value.toObject();
        const auto facts = entry.value(QStringLiteral("facts")).toObject();
        if (entry.value(QStringLiteral("role")) != QJsonValue(QStringLiteral("fact"))
            || (facts.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("pending"))
                && facts.value(QStringLiteral("status")) != QJsonValue(QStringLiteral("unknown")))) continue;
        const QString taskId = entry.value(QStringLiteral("taskId")).toString();
        if (taskId.isEmpty() || resolved.contains(taskId + QStringLiteral(":")
            + facts.value(QStringLiteral("idempotencyKey")).toString(facts.value(QStringLiteral("requestSha256")).toString()))) continue;
        QJsonObject reference;
        for (const auto &field : {QStringLiteral("command"), QStringLiteral("idempotencyKey"),
                                 QStringLiteral("requestSha256"), QStringLiteral("workflowProjectId")})
            if (facts.contains(field)) reference.insert(field, facts.value(field));
        if (reference.contains(QStringLiteral("idempotencyKey")))
        {
            const QJsonObject request{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("operation"), QStringLiteral("operation.query")},
                {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("idempotencyKey"), reference.value(QStringLiteral("idempotencyKey"))}}}};
            const auto reply = commandHost ? commandHost->handleReadOnly(request) : QJsonObject{};
            const auto operation = reply.value(QStringLiteral("result")).toObject();
            const auto observation = operation.value(QStringLiteral("observation")).toObject();
            if (observation.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("accepted")))
                recordReceiptObservation(route, taskId, reference, observation.value(QStringLiteral("result")).toObject());
            else if (operation.value(QStringLiteral("phase")) == QJsonValue(QStringLiteral("observed"))
                && observation.value(QStringLiteral("status")) == QJsonValue(QStringLiteral("rejected")))
            {
                auto rejected = reference;
                rejected.insert(QStringLiteral("status"), QStringLiteral("rejected"));
                rejected.insert(QStringLiteral("code"), observation.value(QStringLiteral("code")));
                if (appendExecutionFact(route, taskId, tr("The durable host record confirms this execution was rejected."), rejected))
                {
                    runner.resolveExecutionObservation(taskId, reference);
                    pendingExecutionReferences.remove(taskId + QStringLiteral(":") + reference.value(QStringLiteral("idempotencyKey")).toString());
                }
            }
        }
        else if (reference.contains(QStringLiteral("requestSha256")) && connectedWorkflow && connectedWorkflow->isReady()
                 && receiptQueries.size() < 16)
        {
            const QString id = connectedWorkflow->request(QStringLiteral("execution.query"),
                {{QStringLiteral("requestSha256"), reference.value(QStringLiteral("requestSha256"))}});
            if (!id.isEmpty()) receiptQueries.insert(id, QJsonObject{{QStringLiteral("taskId"), taskId},
                {QStringLiteral("reference"), reference}, {QStringLiteral("route"), route}});
        }
    }
}

bool AgentWidget::loadConversation(const QString &id, QString *error)
{
    if (id.isEmpty() || runner.isActive() || !bindConversationStore()) return false;
    QString localError;
    const auto page = conversationStore->page(id, 0, 1, &localError);
    if (page.isEmpty())
    {
        if (error) *error = localError;
        historyStatus->setText(tr("This history is unavailable or invalid; existing records were preserved."));
        return false;
    }
    clearConversationView();
    currentConversationId = id;
    conversationRevision = page.value(QStringLiteral("revision")).toString();
    conversationMessageCount = page.value(QStringLiteral("messageCount")).toInt();
    { const QSignalBlocker blocker(authorize); authorize->setChecked(false); }
    renderHistoryPage(qMax(0, conversationMessageCount - 50));
    if (QSettings *preferences = agentPreferences()) preferences->setValue(lastConversationPreferenceKey(), id);
    refreshConversationList();
    updateAvailability();
    if (error) error->clear();
    return true;
}

void AgentWidget::renderHistoryPage(int offset)
{
    if (runner.isActive() || currentConversationId.isEmpty() || !bindConversationStore()) return;
    QString error;
    const auto page = conversationStore->page(currentConversationId, qMax(0, offset), 50, &error);
    if (page.isEmpty()) { historyStatus->setText(tr("Historical messages could not be verified.")); return; }
    restoringHistory = true;
    transcript->clear();
    branchMessageChoice->clear();
    QJsonArray context, historicalFacts;
    for (const auto &value : page.value(QStringLiteral("messages")).toArray())
    {
        const auto entry = value.toObject();
        const QString role = entry.value(QStringLiteral("role")).toString();
        const QString text = entry.value(QStringLiteral("text")).toString();
        QString visibleText = text;
        if (role == QStringLiteral("fact"))
        {
            const auto facts = entry.value(QStringLiteral("facts")).toObject();
            for (const auto &field : {QStringLiteral("command"), QStringLiteral("operationId"), QStringLiteral("jobId"),
                                     QStringLiteral("sourceRevision"), QStringLiteral("acceptedModelSha256"),
                                     QStringLiteral("idempotencyKey"), QStringLiteral("requestSha256")})
                if (facts.contains(field)) visibleText += QStringLiteral("\n") + field + QStringLiteral(": ") + facts.value(field).toString();
            QJsonObject historical{{QStringLiteral("type"), QStringLiteral("history_fact")},
                {QStringLiteral("text"), tr("Historical evidence; query its reference before relying on it: %1").arg(visibleText).left(2000)}};
            if (facts.contains(QStringLiteral("jobId")))
                historical.insert(QStringLiteral("artifactRef"), QJsonObject{{QStringLiteral("jobId"), facts.value(QStringLiteral("jobId"))}});
            const auto sourceRefs = entry.value(QStringLiteral("sourceRefs")).toArray();
            if (!sourceRefs.isEmpty())
            {
                const auto source = sourceRefs.first().toObject();
                historical.insert(QStringLiteral("sourceRef"), QJsonObject{{QStringLiteral("sourceId"), source.value(QStringLiteral("sourceId"))},
                    {QStringLiteral("sourceRevision"), source.value(QStringLiteral("revision"))},
                    {QStringLiteral("modelSha256"), source.value(QStringLiteral("sha256"))}});
            }
            historicalFacts.append(historical);
        }
        appendText(role == QStringLiteral("user") ? tr("Historical user")
            : role == QStringLiteral("fact") ? tr("Historical host fact") : tr("Historical assistant"),
            visibleText, role == QStringLiteral("assistant"));
        branchMessageChoice->addItem(role + QStringLiteral(": ") + text.left(60), entry.value(QStringLiteral("messageId")).toString());
        if (role == QStringLiteral("user") || role == QStringLiteral("assistant"))
            context.append(QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), text.left(2000)}});
    }
    QJsonArray boundedHistory;
    for (const auto &fact : historicalFacts)
    {
        if (boundedHistory.size() >= 8) break;
        boundedHistory.append(fact);
    }
    const qsizetype remaining = 12 - boundedHistory.size();
    for (qsizetype index = qMax(qsizetype(0), context.size() - remaining); index < context.size(); ++index)
        boundedHistory.append(context.at(index));
    runner.restoreObservableConversation(boundedHistory);
    historyOffset = page.value(QStringLiteral("offset")).toInt();
    conversationRevision = page.value(QStringLiteral("revision")).toString();
    conversationMessageCount = page.value(QStringLiteral("messageCount")).toInt();
    restoringHistory = false;
    historyStatus->setText(tr("Historical messages %1–%2 of %3. No pending proposal or approval was restored.")
        .arg(conversationMessageCount ? historyOffset + 1 : 0)
        .arg(historyOffset + page.value(QStringLiteral("messages")).toArray().size()).arg(conversationMessageCount));
    reconcileHistoryFacts(page.value(QStringLiteral("messages")).toArray());
    updateAvailability();
}

void AgentWidget::branchFromMessage()
{
    if (runner.isActive() || currentConversationId.isEmpty() || !bindConversationStore()) return;
    const QString messageId = branchMessageChoice->currentData().toString();
    if (messageId.isEmpty()) return;
    QString error;
    const auto summary = conversationStore->branch(currentConversationId, messageId, tr("Conversation branch"), &error);
    if (summary.isEmpty()) { historyStatus->setText(tr("The branch could not be created. Existing history was preserved.")); return; }
    loadConversation(summary.value(QStringLiteral("conversationId")).toString());
    goal->setFocus(Qt::OtherFocusReason);
}

void AgentWidget::deleteConversation()
{
    if (runner.isActive() || currentConversationId.isEmpty() || !bindConversationStore()) return;
    QString error;
    if (!conversationStore->remove(currentConversationId, conversationRevision, &error))
    { historyStatus->setText(tr("This conversation could not be deleted. Existing data was preserved.")); return; }
    currentConversationId.clear(); conversationRevision.clear(); conversationMessageCount = 0;
    runner.clearConversation(); transcript->clear(); branchMessageChoice->clear();
    historyStatus->setText(tr("Conversation deleted. Its historical actions and independent branches remain unchanged."));
    refreshConversationList(); updateAvailability();
}

void AgentWidget::exportConversation()
{
    if (currentConversationId.isEmpty() || !bindConversationStore()) return;
    QString error;
    const auto snapshot = conversationStore->exportConversation(currentConversationId, &error);
    if (snapshot.isEmpty()) { historyStatus->setText(tr("This history could not be verified for export.")); return; }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export conversation"), QString(), tr("Conversation JSON (*.json)"));
    if (path.isEmpty()) return;
    const QByteArray bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    { historyStatus->setText(tr("The export could not be published.")); return; }
    historyStatus->setText(tr("Exported verified historical messages and facts; no execution permissions are included."));
}

void AgentWidget::startReadOnlyQuestion(const QString &text)
{
    if (accessPolicy != AgentTaskRunner::AccessPolicy::ReadWrite || text.trimmed().isEmpty()) return;
    if (!readOnlySidecar)
    {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("agentReadOnlyDialog"));
        dialog->setWindowTitle(tr("Read-only question · current task continues"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->resize(600, 850);
        auto *layout = new QVBoxLayout(dialog);
        auto *sidecar = new AgentWidget(commandHost, connectedWorkflow, dialog, network,
                                        AgentTaskRunner::AccessPolicy::ReadOnly);
        layout->addWidget(sidecar);
        readOnlyDialog = dialog; readOnlySidecar = sidecar;
        connect(dialog, &QDialog::finished, this, [this] { if (goal) goal->setFocus(Qt::OtherFocusReason); });
    }
    auto *sidecar = readOnlySidecar.data();
    if (!sidecar) return;
    sidecar->setProviderSettings(getProviderSettings());
    sidecar->selectedEffort = selectedEffort;
    sidecar->updateEffortChoices();
    sidecar->streamResponses->setChecked(streamResponses->isChecked());
    sidecar->streamResponses->setChecked(false);
    readOnlyDialog->show(); readOnlyDialog->raise(); readOnlyDialog->activateWindow();
    if (sidecar->runner.isActive())
    {
        sidecar->goal->setPlainText(text);
        sidecar->submitDraft();
    }
    else sidecar->startTask(text, false);
}

RAgentSettings AgentWidget::getProviderSettings() const
{
    RAgentSettings settings(providerSettings);
    settings.setModel(model ? model->currentText().trimmed() : providerSettings.getModel());
    settings.setApiKey(key ? key->text() : providerSettings.getApiKey());
    return settings;
}

void AgentWidget::setProviderSettings(const RAgentSettings &settings)
{
    if (settings.getType() != RAgentSettings::Type::DeepSeek
        && settings.getType() != RAgentSettings::Type::OpenAILegacy) return;
    if (!isValidAgentEndpoint(settings.getApiUrl()))
    {
        if (state)
        {
            state->setText(tr("Invalid endpoint"));
            state->setToolTip(invalidEndpointMessage());
        }
        return;
    }
    providerSettings = settings;
    if (key) key->setText(settings.getApiKey());
    if (model && model->currentText() != settings.getModel()) model->setEditText(settings.getModel());
    updateEffortChoices();
    persistConversationPreferences();
    updateAvailability();
}

void AgentWidget::updateEffortChoices()
{
    if (!effort) return;
    const QSignalBlocker blocker(effort);
    effort->clear();
    if (providerSettings.getType() == RAgentSettings::Type::DeepSeek)
    {
        if (selectedEffort == QStringLiteral("default")) selectedEffort = QStringLiteral("off");
        effort->addItem(tr("Off"), QStringLiteral("off"));
        effort->addItem(tr("Low"), QStringLiteral("low"));
        effort->addItem(tr("High"), QStringLiteral("high"));
        effort->addItem(tr("Max"), QStringLiteral("max"));
        if (effort->findData(selectedEffort) < 0) selectedEffort = QStringLiteral("off");
        effort->setEnabled(true);
    }
    else
    {
        selectedEffort = QStringLiteral("default");
        effort->addItem(tr("Default"), QStringLiteral("default"));
        effort->setEnabled(false);
    }
    effort->setCurrentIndex(qMax(0, effort->findData(selectedEffort)));
}

void AgentWidget::startTask(const QString &text, bool authorized)
{
    if (runner.isActive()) return;
    const QString message = text.trimmed();
    if (message.isEmpty() || key->text().trimmed().isEmpty())
    {
        state->setText(message.isEmpty() ? tr("Enter a message first.") : tr("Add an API key in assistant settings."));
        return;
    }
    requestSettingsSnapshot = getProviderSettings();
    bindConversationStore();
    requestEffortSnapshot = selectedEffort;
    requestStreamingSnapshot = accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && streamResponses->isChecked()
        && providerSupportsStreaming(requestSettingsSnapshot.getType());
    appendText(tr("You"), text);
    runner.startTask(text, accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && authorized);
}

void AgentWidget::submitDraft()
{
    const QString message = goal->toPlainText().trimmed();
    if (message.isEmpty()) return;
    if (runner.isAwaitingInput())
    {
        answerQuestion(runner.pendingQuestionId(), message, true);
        return;
    }
    if (key->text().trimmed().isEmpty())
    {
        state->setText(tr("Add an API key in assistant settings."));
        openSettings();
        return;
    }
    requestSettingsSnapshot = getProviderSettings();
    bindConversationStore();
    requestEffortSnapshot = selectedEffort;
    requestStreamingSnapshot = accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && streamResponses->isChecked()
        && providerSupportsStreaming(requestSettingsSnapshot.getType());
    appendText(tr("You"), message);
    goal->clear();
    runner.submitMessage(message, accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && authorize->isChecked());
    updateAvailability();
}

void AgentWidget::answerQuestion(const QString &questionId, const QString &answer, bool clearDraft)
{
    if (questionId.isEmpty() || questionId != runner.pendingQuestionId() || answer.trimmed().isEmpty()) return;
    if (clearDraft && goal->toPlainText().trimmed() != answer.trimmed()) return;
    appendText(tr("You"), answer);
    recordMessage(QStringLiteral("user"), answer, runner.taskId());
    if (clearDraft) goal->clear();
    runner.answerQuestion(questionId, answer);
    updateAvailability();
}

void AgentWidget::appendText(const QString &title, const QString &text, bool markdown)
{
    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (markdown)
    {
        ConversationDocument message;
        message.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                  | QTextDocument::MarkdownNoHTML);
        QTextCursor cursor(transcript->document());
        cursor.movePosition(QTextCursor::End);
        const auto newParagraph = [&cursor] {
            cursor.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
            if (auto *list = cursor.currentList()) list->remove(cursor.block());
        };
        if (!transcript->document()->isEmpty()) newParagraph();
        cursor.insertHtml(QStringLiteral("<p><b>%1</b></p>").arg(title.toHtmlEscaped()));
        newParagraph();
        cursor.insertFragment(QTextDocumentFragment(&message));
        newParagraph();
        restoreTranscriptScroll(followTail, previousScroll);
        return;
    }
    QString escapedText = text.toHtmlEscaped();
    escapedText.replace(QStringLiteral("\r\n"), QStringLiteral("<br/>"));
    escapedText.replace(QLatin1Char('\r'), QLatin1String("<br/>"));
    escapedText.replace(QLatin1Char('\n'), QLatin1String("<br/>"));
    const QString roleColor = title == tr("You")
        ? transcript->palette().color(QPalette::Link).name()
        : transcript->palette().color(QPalette::Text).name();
    QTextCursor cursor(transcript->document());
    cursor.movePosition(QTextCursor::End);
    if (!transcript->document()->isEmpty())
    {
        cursor.insertBlock(QTextBlockFormat{}, QTextCharFormat{});
        if (auto *list = cursor.currentList()) list->remove(cursor.block());
    }
    cursor.insertHtml(QStringLiteral("<p style='margin:9px 0 3px;color:%1'><b>%2</b></p>"
                                      "<p style='margin:0 0 7px;white-space:pre-wrap'>%3</p>")
                           .arg(roleColor, title.toHtmlEscaped(), escapedText));
    restoreTranscriptScroll(followTail, previousScroll);
}

bool AgentWidget::transcriptFollowsTail() const
{
    const auto *scrollBar = transcript->verticalScrollBar();
    return scrollBar->value() >= scrollBar->maximum() - 1;
}

void AgentWidget::restoreTranscriptScroll(bool followTail, int previousValue)
{
    auto *scrollBar = transcript->verticalScrollBar();
    scrollBar->setValue(followTail ? scrollBar->maximum()
                                   : qMin(previousValue, scrollBar->maximum()));
}

void AgentWidget::updateActivityCard(const QString &taskId, const QString &toolCallId,
                                    const QString &toolName, const QString &phase,
                                    const QString &operationId, const QJsonObject &facts)
{
    if (taskId.isEmpty() || taskId.size() > 128 || toolCallId.isEmpty() || toolCallId.size() > 256
        || toolName.size() > 128 || operationId.size() > 128) return;
    const QString cardKey = taskId + QLatin1Char('/') + toolCallId;
    QPointer<QFrame> frame = activityCardFrames.value(cardKey);
    QPointer<QLabel> label = activityCardLabels.value(cardKey);
    auto *activityScrollBar = activityScroll->verticalScrollBar();
    const bool followActivityTail = activityScrollBar->value() >= activityScrollBar->maximum() - 1;
    if (!frame || !label)
    {
        frame = new QFrame(activityCards);
        frame->setObjectName(QStringLiteral("agentActivityCard"));
        frame->setFrameShape(QFrame::StyledPanel);
        auto *cardLayout = new QVBoxLayout(frame);
        cardLayout->setContentsMargins(7, 4, 7, 4);
        label = new QLabel(frame);
        label->setObjectName(QStringLiteral("agentActivityCardText"));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setAccessibleName(tr("Trusted tool activity"));
        cardLayout->addWidget(label);
        const int stretchIndex = qMax(0, activityCardsLayout->count() - 1);
        activityCardsLayout->insertWidget(stretchIndex, frame);
        activityCardFrames.insert(cardKey, frame);
        activityCardLabels.insert(cardKey, label);
        activityCardOrder.append(cardKey);
        while (activityCardOrder.size() > MaxActivityCards)
        {
            const QString oldest = activityCardOrder.takeFirst();
            if (activityCardFrames.value(oldest)) activityCardFrames.value(oldest)->deleteLater();
            activityCardFrames.remove(oldest);
            activityCardLabels.remove(oldest);
        }
    }

    const QMap<QString, QString> phaseLabels{
        {QStringLiteral("reading"), tr("Reading")},
        {QStringLiteral("preparing"), tr("Preparing")},
        {QStringLiteral("awaiting_authorization"), tr("Waiting for approval")},
        {QStringLiteral("executing"), tr("Executing")},
        {QStringLiteral("completed"), tr("Host response validated")},
        {QStringLiteral("rejected"), tr("Rejected")},
        {QStringLiteral("failed"), tr("Failed")}};
    QStringList lines{tr("Task %1 · tool %2").arg(taskId, toolCallId),
                      tr("%1 — %2").arg(toolName, phaseLabels.value(phase, tr("Working")))};
    if (!operationId.isEmpty()) lines.append(tr("Operation %1").arg(operationId));
    for (const QString &factKey : {QStringLiteral("status"), QStringLiteral("code"),
                                   QStringLiteral("proposalId"), QStringLiteral("jobId")})
    {
        const QString value = facts.value(factKey).toString().left(128);
        if (!value.isEmpty()) lines.append(tr("%1: %2").arg(factKey, value));
    }
    label->setText(lines.join(QLatin1Char('\n')));
    frame->setProperty("agentTaskId", taskId);
    frame->setProperty("agentToolCallId", toolCallId);
    frame->setProperty("agentOperationId", operationId);
    frame->setProperty("agentActivityPhase", phase);
    activityScroll->show();
    if (followActivityTail) activityScrollBar->setValue(activityScrollBar->maximum());
}

void AgentWidget::onToolActivity(const QString &taskId, const QString &toolCallId,
                                 const QString &toolName, const QString &phase,
                                 const QString &operationId, const QJsonObject &facts)
{
    updateActivityCard(taskId, toolCallId, toolName, phase, operationId, facts);
}

void AgentWidget::appendStreamText(const QString &requestId, const QString &text)
{
    if (requestId.isEmpty() || text.isEmpty()) return;
    if (!liveStreamRequestId.isEmpty() && liveStreamRequestId != requestId)
        finishStreamText(liveStreamRequestId, liveStreamText, false);

    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (liveStreamRequestId.isEmpty())
    {
        QTextCursor cursor(transcript->document());
        cursor.movePosition(QTextCursor::End);
        if (!transcript->document()->isEmpty()) cursor.insertBlock();
        liveStreamStartPosition = cursor.position();
        liveStreamTitleStartPosition = cursor.position();
        QTextCharFormat titleFormat;
        titleFormat.setFontWeight(QFont::Bold);
        cursor.insertText(tr("Assistant"), titleFormat);
        liveStreamTitleEndPosition = cursor.position();
        cursor.insertBlock();
        liveStreamContentStartPosition = cursor.position();
        liveStreamEndPosition = cursor.position();
        liveStreamRequestId = requestId;
        liveStreamText.clear();
    }

    QTextCursor cursor(transcript->document());
    cursor.setPosition(liveStreamEndPosition);
    cursor.insertText(text);
    liveStreamEndPosition = cursor.position();
    liveStreamText.append(text);
    restoreTranscriptScroll(followTail, previousScroll);
}

void AgentWidget::finishStreamText(const QString &requestId, const QString &text, bool accepted)
{
    if (requestId.isEmpty() || requestId != liveStreamRequestId) return;
    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (accepted)
    {
        ConversationDocument message;
        message.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                      | QTextDocument::MarkdownNoHTML);
        QTextCursor cursor(transcript->document());
        cursor.setPosition(liveStreamContentStartPosition);
        cursor.setPosition(liveStreamEndPosition, QTextCursor::KeepAnchor);
        cursor.insertFragment(QTextDocumentFragment(&message));
    }
    else if (liveStreamTitleStartPosition >= 0 && liveStreamTitleEndPosition >= liveStreamTitleStartPosition)
    {
        QTextCursor title(transcript->document());
        title.setPosition(liveStreamTitleStartPosition);
        title.setPosition(liveStreamTitleEndPosition, QTextCursor::KeepAnchor);
        title.insertText(tr("Assistant (incomplete)"));
    }
    liveStreamRequestId.clear();
    liveStreamText.clear();
    liveStreamStartPosition = -1;
    liveStreamTitleStartPosition = -1;
    liveStreamTitleEndPosition = -1;
    liveStreamContentStartPosition = -1;
    liveStreamEndPosition = -1;
    restoreTranscriptScroll(followTail, previousScroll);
}

void AgentWidget::clearStreamText(const QString &requestId)
{
    if (requestId.isEmpty() || requestId != liveStreamRequestId) return;
    const bool followTail = transcriptFollowsTail();
    const int previousScroll = transcript->verticalScrollBar()->value();
    if (liveStreamStartPosition >= 0 && liveStreamEndPosition >= liveStreamStartPosition)
    {
        QTextCursor cursor(transcript->document());
        cursor.setPosition(liveStreamStartPosition);
        cursor.setPosition(liveStreamEndPosition, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    liveStreamRequestId.clear();
    liveStreamText.clear();
    liveStreamStartPosition = -1;
    liveStreamTitleStartPosition = -1;
    liveStreamTitleEndPosition = -1;
    liveStreamContentStartPosition = -1;
    liveStreamEndPosition = -1;
    restoreTranscriptScroll(followTail, previousScroll);
}

bool AgentWidget::isCurrentProviderRequest(const QString &requestId, quint64 generation,
                                          QNetworkReply *expectedReply) const
{
    return !requestId.isEmpty() && requestId == activeProviderRequestId
        && generation == providerRequestGeneration && expectedReply && reply == expectedReply;
}

void AgentWidget::retireProviderRequest(const QString &requestId)
{
    if (!requestId.isEmpty() && requestId != activeProviderRequestId) return;
    const QString retiringId = activeProviderRequestId;
    const QPointer<QNetworkReply> previousReply = reply;
    if (retiringId.isEmpty() && !previousReply) return;

    // Invalidate before abort: some backends emit finished synchronously from abort().
    activeProviderRequestId.clear();
    reply.clear();
    ++providerRequestGeneration;
    if (!retiringId.isEmpty() && liveStreamRequestId == retiringId)
        finishStreamText(retiringId, liveStreamText, false);
    if (previousReply) previousReply->abort();
}

void AgentWidget::openSettings()
{
    Application *application = Application::instance();
    if (!application || !application->getApplicationSettings())
    {
        state->setText(tr("Assistant settings are unavailable."));
        return;
    }
    ApplicationSettingsDialog dialog(application->getApplicationSettings(), this, QStringLiteral("Agent"));
    dialog.exec();
}

void AgentWidget::persistConversationPreferences()
{
    if (accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly) return;
    QSettings *preferences = agentPreferences();
    if (!preferences) return;
    if (model) providerSettings.setModel(model->currentText().trimmed());
    if (!isValidAgentEndpoint(providerSettings.getApiUrl()))
    {
        if (state)
        {
            state->setText(tr("Invalid endpoint"));
            state->setToolTip(invalidEndpointMessage());
        }
        return;
    }
    preferences->setValue(providerTypeKey, int(providerSettings.getType()));
    preferences->setValue(endpointKey, providerSettings.getApiUrl());
    preferences->setValue(modelKey, providerSettings.getModel());
    preferences->setValue(effortKey, selectedEffort);
    preferences->setValue(automaticChangesKey, authorize && authorize->isChecked());
    preferences->setValue(streamingResponsesKey, streamResponses && streamResponses->isChecked());
    // API keys are deliberately excluded from persistent settings.
}

QWidget *AgentWidget::createSettingsPage(QWidget *parent)
{
    QWidget *page = new QWidget(parent);
    page->setObjectName(QStringLiteral("agentSettingsPage"));
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    auto *provider = new QComboBox(page);
    provider->setObjectName(QStringLiteral("agentSettingsProvider"));
    provider->addItem(tr("DeepSeek"), int(RAgentSettings::Type::DeepSeek));
    provider->addItem(tr("OpenAI-compatible Chat Completions"), int(RAgentSettings::Type::OpenAILegacy));
    auto *endpoint = new QLineEdit(page);
    endpoint->setObjectName(QStringLiteral("agentSettingsEndpoint"));
    endpoint->setAccessibleName(tr("Assistant provider HTTPS endpoint"));
    auto *apiKey = new QLineEdit(page);
    apiKey->setObjectName(QStringLiteral("agentSettingsApiKey"));
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setPlaceholderText(tr("API key (kept in memory for this session)"));
    apiKey->setAccessibleName(tr("Assistant API key"));
    auto *loadKey = new QPushButton(tr("Load key file…"), page);
    loadKey->setObjectName(QStringLiteral("agentLoadKeyFile"));
    loadKey->setAccessibleName(tr("Load an API key from a local file"));
    auto *keyRow = new QWidget(page);
    auto *keyRowLayout = new QHBoxLayout(keyRow);
    keyRowLayout->setContentsMargins(0, 0, 0, 0);
    keyRowLayout->addWidget(apiKey, 1);
    keyRowLayout->addWidget(loadKey);
    form->addRow(tr("Provider"), provider);
    form->addRow(tr("Endpoint"), endpoint);
    auto *endpointError = new QLabel(page);
    endpointError->setObjectName(QStringLiteral("agentEndpointValidation"));
    endpointError->setWordWrap(true);
    form->addRow(QString(), endpointError);
    form->addRow(tr("API key"), keyRow);
    layout->addLayout(form);
    auto *note = new QLabel(tr("The API key stays in memory for this session and is never written to application settings."), page);
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch(1);

    QPointer<AgentWidget> agent = activeAgentWidget();
    RAgentSettings settings = agent ? agent->getProviderSettings() : RAgentSettings(RAgentSettings::Type::DeepSeek);
    QSettings *preferences = agentPreferences();
    if (!agent && preferences)
    {
        const auto type = static_cast<RAgentSettings::Type>(preferences->value(providerTypeKey, int(RAgentSettings::Type::DeepSeek)).toInt());
        if (type == RAgentSettings::Type::DeepSeek || type == RAgentSettings::Type::OpenAILegacy)
            settings.setType(type);
        const RAgentSettings defaults(settings.getType());
        const QString savedEndpoint = preferences->value(endpointKey, defaults.getApiUrl()).toString();
        if (isValidAgentEndpoint(savedEndpoint)) settings.setApiUrl(savedEndpoint.trimmed());
        else
        {
            settings.setApiUrl(defaults.getApiUrl());
            preferences->setValue(endpointKey, defaults.getApiUrl());
        }
        settings.setModel(preferences->value(modelKey, defaults.getModel()).toString());
    }
    const int providerIndex = provider->findData(int(settings.getType()));
    provider->setCurrentIndex(providerIndex >= 0 ? providerIndex : 0);
    endpoint->setText(settings.getApiUrl());
    endpoint->setProperty("acceptedAgentEndpoint", endpoint->text());
    apiKey->setText(settings.getApiKey());

    auto applySettings = [agent, preferences, endpoint, endpointError](const RAgentSettings &updated) {
        const QString endpointValue = updated.getApiUrl().trimmed();
        if (!isValidAgentEndpoint(endpointValue))
        {
            endpointError->setText(invalidEndpointMessage());
            endpoint->setText(endpoint->property("acceptedAgentEndpoint").toString());
            return;
        }
        if (agent) agent->setProviderSettings(updated);
        if (preferences)
        {
            preferences->setValue(providerTypeKey, int(updated.getType()));
            preferences->setValue(endpointKey, endpointValue);
            preferences->setValue(modelKey, updated.getModel());
        }
        endpoint->setProperty("acceptedAgentEndpoint", endpointValue);
        endpoint->setText(endpointValue);
        endpointError->clear();
    };
    connect(provider, &QComboBox::currentIndexChanged, page, [provider, endpoint, apiKey, applySettings](int index) {
        if (index < 0) return;
        RAgentSettings updated(static_cast<RAgentSettings::Type>(provider->itemData(index).toInt()));
        apiKey->clear();
        endpoint->setText(updated.getApiUrl());
        applySettings(updated);
    });
    connect(endpoint, &QLineEdit::editingFinished, page, [agent, endpoint, apiKey, provider, endpointError, applySettings] {
        const QString endpointValue = endpoint->text().trimmed();
        if (!isValidAgentEndpoint(endpointValue))
        {
            endpointError->setText(invalidEndpointMessage());
            endpoint->setText(endpoint->property("acceptedAgentEndpoint").toString());
            return;
        }
        RAgentSettings updated(static_cast<RAgentSettings::Type>(provider->currentData().toInt()));
        updated.setApiUrl(endpointValue);
        updated.setModel(agent && agent->model ? agent->model->currentText().trimmed() : QString());
        updated.setApiKey(apiKey->text());
        applySettings(updated);
    });
    connect(apiKey, &QLineEdit::textChanged, page, [agent, endpoint, apiKey, provider, applySettings](const QString &value) {
        RAgentSettings updated(static_cast<RAgentSettings::Type>(provider->currentData().toInt()));
        updated.setApiUrl(endpoint->text().trimmed());
        updated.setModel(agent && agent->model ? agent->model->currentText().trimmed() : QString());
        updated.setApiKey(value);
        applySettings(updated);
    });
    connect(loadKey, &QPushButton::clicked, page, [page, loadKey, apiKey, provider] {
        const QString path = QFileDialog::getOpenFileName(page, tr("Select local API key file"), QString(),
                                                          tr("Key files (*.key *.txt *.env*);;All files (*)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        {
            loadKey->setToolTip(tr("Cannot read this key file."));
            return;
        }
        const QString contents = QString::fromUtf8(file.readAll());
        const QString variable = provider->currentData().toInt() == int(RAgentSettings::Type::DeepSeek)
            ? QStringLiteral("DEEPSEEK_API_KEY=") : QStringLiteral("OPENAI_API_KEY=");
        QString value;
        for (QString line : contents.split(QLatin1Char('\n')))
        {
            line = line.trimmed();
            if (!line.startsWith(variable)) continue;
            value = line.mid(variable.size()).trimmed();
            if (value.size() >= 2 && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                                      || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
                value = value.mid(1, value.size() - 2);
            break;
        }
        const QString rawLine = contents.trimmed();
        if (value.isEmpty() && !rawLine.contains(QLatin1Char('\n')) && !rawLine.contains(QLatin1Char('\r'))
            && !rawLine.contains(QLatin1Char('=')))
            value = rawLine;
        if (value.isEmpty() || value.size() > 4096
            || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r')))
        {
            loadKey->setToolTip(tr("No single-line key for the selected provider was found."));
            return;
        }
        apiKey->setText(value);
        loadKey->setToolTip(tr("API key loaded for this session."));
    });
    return page;
}

void AgentWidget::updateAvailability()
{
    const bool busy = runner.isActive();
    newChat->setEnabled(!busy);
    send->setText(runner.isAwaitingInput() ? tr("Answer") : (busy ? tr("Send now") : tr("Send")));
    send->setEnabled(!goal->toPlainText().trimmed().isEmpty());
    stop->setEnabled(busy);
    stop->setVisible(busy);
    goal->setEnabled(true);
    model->setEnabled(true);
    effort->setEnabled(providerSettings.getType() == RAgentSettings::Type::DeepSeek);
    streamResponses->setEnabled(accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && providerSupportsStreaming(providerSettings.getType()));
    streamResponses->setToolTip(providerSupportsStreaming(providerSettings.getType())
        ? tr("When enabled, supported Chat Completions providers stream text. Complete JSON responses remain available when streaming is off.")
        : tr("This provider does not support streaming Chat Completions."));
    authorize->setEnabled(accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite);
    readOnlyQuestion->setEnabled(accessPolicy == AgentTaskRunner::AccessPolicy::ReadWrite && !goal->toPlainText().trimmed().isEmpty());
    conversationChoice->setEnabled(!busy);
    branchConversation->setEnabled(!busy && !currentConversationId.isEmpty() && branchMessageChoice->count() > 0);
    branchMessageChoice->setEnabled(!busy);
    historyMenu->setEnabled(!currentConversationId.isEmpty());
    historyPrevious->setEnabled(!busy && historyOffset > 0);
    historyNext->setEnabled(!busy && historyOffset + 50 < conversationMessageCount);

    const bool pendingProposal = busy && !runner.pendingProposal().isEmpty() && !presentedProposalId.isEmpty();
    approvalCard->setVisible(pendingProposal);
    apply->setEnabled(pendingProposal);
    decline->setEnabled(pendingProposal);
    if (!pendingProposal)
    {
        presentedProposalId.clear();
        preview->clear();
    }
    const bool pendingQuestion = busy && runner.isAwaitingInput() && !runner.pendingQuestionId().isEmpty();
    questionCard->setVisible(pendingQuestion);
    if (!pendingQuestion) presentedQuestionId.clear();
    if (pendingQuestion && runner.pendingQuestionId() != presentedQuestionId)
    {
        questionCard->setVisible(false);
        send->setText(busy ? tr("Send now") : tr("Send"));
    }
}

QJsonObject AgentWidget::hostRequest(const QString &operation, const QJsonObject &parameters) const
{
    if (!commandHost) return {};
    const QJsonObject request{{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                                {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("operation"), operation},
                                {QStringLiteral("parameters"), parameters}};
    return accessPolicy == AgentTaskRunner::AccessPolicy::ReadOnly
        ? commandHost->handleReadOnly(request) : commandHost->handle(request);
}

bool AgentWidget::queryFreshContext()
{
    const auto response = hostRequest(QStringLiteral("context.query"));
    if (response.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
    {
        contextRefreshPending = true;
        contextSummaryBody = tr("Current document context could not be queried.");
        eventContextSummary->setText(contextSummaryBody);
        return false;
    }
    const auto result = response.value(QStringLiteral("result")).toObject();
    const auto documents = result.value(QStringLiteral("documents")).toArray();
    const auto selection = result.value(QStringLiteral("selection")).toObject();
    QStringList lines;
    bool allDocumentsLive = true;
    auto appendDocument = [this, &lines, &allDocumentsLive](const QJsonObject &document) {
        const QString documentId = document.value(QStringLiteral("documentId")).toString();
        if (documentId.isEmpty()) return;
        allDocumentsLive &= document.value(QStringLiteral("stateAvailability")).toString() == QStringLiteral("live");
        const QString revision = QString::number(quint64(document.value(QStringLiteral("documentRevision")).toDouble()));
        QString line = tr("Document %1 · revision %2 · state %3")
            .arg(documentId, revision, document.value(QStringLiteral("stateAvailability")).toString());
        const QJsonObject accepted = document.value(QStringLiteral("acceptedState")).toObject();
        const QJsonObject modal = accepted.value(QStringLiteral("modal")).toObject();
        QStringList values;
        const QMap<QString, QString> labels{{QStringLiteral("iterations"), tr("iterations")},
            {QStringLiteral("modes"), tr("modes")}, {QStringLiteral("method"), tr("method")},
            {QStringLiteral("tolerance"), tr("tolerance")}, {QStringLiteral("unit"), tr("unit")}};
        for (auto it = labels.cbegin(); it != labels.cend(); ++it)
        {
            const auto value = modal.value(it.key());
            if (value.isUndefined() || value.isNull()) continue;
            values.append(QStringLiteral("%1 %2").arg(it.value(), value.toVariant().toString()));
        }
        if (!values.isEmpty()) line += tr(" · modal: %1").arg(values.join(QStringLiteral(", ")));
        lines.append(line);
    };
    constexpr qsizetype maxContextDocuments = 8;
    const qsizetype count = qMin(documents.size(), maxContextDocuments);
    for (qsizetype i = 0; i < count; ++i) appendDocument(documents.at(i).toObject());
    if (documents.size() > maxContextDocuments)
        lines.append(tr("%1 additional selected documents are omitted from this summary.").arg(documents.size() - maxContextDocuments));
    const bool live = selection.value(QStringLiteral("availability")).toString() == QStringLiteral("live")
        && !result.value(QStringLiteral("editingBlocked")).toBool()
        && (documents.isEmpty() || allDocumentsLive);
    const QString details = lines.isEmpty()
        ? tr("no selected open document is available") : lines.join(QStringLiteral("\n"));
    contextSummaryBody = live
        ? tr("Live selected-document context: %1").arg(details)
        : tr("Cached or unavailable context; document state may be stale: %1").arg(details);
    contextRefreshPending = !live;
    eventContextSummary->setText(contextSummaryBody);
    return true;
}

void AgentWidget::markContextStale()
{
    contextRefreshPending = true;
    if (contextSummaryBody.isEmpty()) contextSummaryBody = tr("No document context snapshot is available.");
    eventContextSummary->setText(tr("Prior context may be stale. %1").arg(contextSummaryBody));
}

void AgentWidget::appendHostEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    const QString documentId = event.value(QStringLiteral("documentId")).toString();

    QString readableType;
    if (type == QStringLiteral("document.opened")) readableType = tr("Document opened");
    else if (type == QStringLiteral("document.closed")) readableType = tr("Document closed");
    else if (type == QStringLiteral("document.changed")) readableType = tr("Document changed");
    else if (type == QStringLiteral("document.revision_changed")) readableType = tr("Document revision changed");
    else if (type == QStringLiteral("selection.changed")) readableType = tr("Selection changed");
    else readableType = type.isEmpty() ? tr("Host event") : tr("Host event (%1)").arg(type);

    QString revision;
    const auto before = event.value(QStringLiteral("beforeRevision"));
    const auto after = event.value(QStringLiteral("afterRevision"));
    if (!before.isUndefined() || !after.isUndefined())
        revision = tr("%1 → %2").arg(before.isNull() || before.isUndefined() ? tr("new") : before.toVariant().toString(),
                                      after.isUndefined() ? tr("unknown") : after.toVariant().toString());
    else if (event.contains(QStringLiteral("documentRevision")))
        revision = event.value(QStringLiteral("documentRevision")).toVariant().toString();
    else
        revision = tr("unknown");
    const QString operationId = event.value(QStringLiteral("operationId")).toString();
    eventHistory->appendPlainText(tr("#%1 · %2 · document %3 · revision %4 · operation %5")
        .arg(QString::number(quint64(event.value(QStringLiteral("eventSeq")).toDouble())), readableType,
             documentId.isEmpty() ? tr("none") : documentId, revision,
             operationId.isEmpty() ? tr("none") : operationId));
}

void AgentWidget::refreshHostEvents()
{
    if (isVisible()) bindConversationStore();
    auto *eventBody = findChild<QWidget *>(QStringLiteral("agentEventHistoryBody"));
    if (!commandHost || QThread::currentThread() != thread() || !isVisible()
        || !eventBody || !eventBody->isVisible()) return;
    const auto capabilities = hostRequest(QStringLiteral("capabilities.query"));
    if (capabilities.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
    {
        markContextStale();
        eventStatus->setText(tr("Host event history is unavailable."));
        return;
    }
    const QString currentEpoch = capabilities.value(QStringLiteral("result")).toObject()
                                     .value(QStringLiteral("sessionEpoch")).toString();
    if (currentEpoch.isEmpty())
    {
        markContextStale();
        eventStatus->setText(tr("Host event history is unavailable."));
        return;
    }
    if (eventSessionEpoch != currentEpoch)
    {
        const bool reset = !eventSessionEpoch.isEmpty();
        eventSessionEpoch = currentEpoch;
        lastEventSeq = 0;
        eventHistory->clear();
        eventHistory->appendPlainText(reset ? tr("Host session changed; loading fresh document context.")
                                            : tr("Connected to the current host session."));
        if (!queryFreshContext())
            eventStatus->setText(tr("Host session changed; fresh context could not be queried yet."));
    }

    // The host retains at most 256 records. Three pages cover that entire ring;
    // a larger gap is represented by a fresh context snapshot instead.
    bool contextChanged = false;
    for (int page = 0; page < 3; ++page)
    {
        const auto response = hostRequest(QStringLiteral("events.read"),
            {{QStringLiteral("afterEventSeq"), double(lastEventSeq)}, {QStringLiteral("limit"), 100}});
        if (response.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
        {
            markContextStale();
            eventStatus->setText(tr("Host event history could not be read; it will be retried."));
            return;
        }
        const auto result = response.value(QStringLiteral("result")).toObject();
        if (result.value(QStringLiteral("resyncRequired")).toBool())
        {
            eventHistory->appendPlainText(tr("Event history gap; older in-memory events have expired."));
            if (!queryFreshContext())
            {
                eventStatus->setText(tr("Event history gap; context refresh failed and will be retried."));
                return;
            }
            lastEventSeq = quint64(result.value(QStringLiteral("latestEventSeq")).toDouble());
            eventStatus->setText(tr("Event cursor advanced through %1; context freshness is shown above.").arg(QString::number(lastEventSeq)));
            return;
        }
        const auto events = result.value(QStringLiteral("events")).toArray();
        bool epochChanged = false;
        for (const auto &value : events)
        {
            const auto event = value.toObject();
            if (event.value(QStringLiteral("sessionEpoch")).toString() != eventSessionEpoch)
            {
                epochChanged = true;
                break;
            }
            appendHostEvent(event);
            lastEventSeq = quint64(event.value(QStringLiteral("eventSeq")).toDouble());
            const QString type = event.value(QStringLiteral("type")).toString();
            contextChanged |= type.startsWith(QStringLiteral("document."))
                || type == QStringLiteral("selection.changed");
        }
        if (epochChanged)
        {
            eventHistory->appendPlainText(tr("Event stream changed; loading fresh document context."));
            if (!queryFreshContext())
            {
                eventStatus->setText(tr("Event stream changed; context refresh failed and will be retried."));
                return;
            }
            lastEventSeq = quint64(result.value(QStringLiteral("latestEventSeq")).toDouble());
            eventStatus->setText(tr("Event cursor advanced through %1; context freshness is shown above.").arg(QString::number(lastEventSeq)));
            return;
        }
        if (!result.value(QStringLiteral("hasMore")).toBool()) break;
    }
    if (contextChanged || contextRefreshPending) queryFreshContext();
    eventStatus->setText(tr("Up to date through event %1.").arg(QString::number(lastEventSeq)));
}

void AgentWidget::requestTurn(const QString &requestId, const QJsonArray &messages, const QJsonArray &tools)
{
    retireProviderRequest();
    const quint64 generation = ++providerRequestGeneration;
    activeProviderRequestId = requestId;
    const RAgentSettings settings(requestSettingsSnapshot);
    const QString providerKeyValue = settings.getApiKey().trimmed();
    const QString endpointValue = settings.getApiUrl().trimmed();
    const QUrl url(endpointValue);
    const auto failBeforeRequest = [this, &requestId](const QString &message) {
        retireProviderRequest(requestId);
        runner.providerFailed(requestId, message);
    };
    if (!isValidAgentEndpoint(endpointValue))
    { failBeforeRequest(invalidEndpointMessage()); return; }
    if (providerKeyValue.isEmpty())
    { failBeforeRequest(tr("Add an API key in assistant settings.")); return; }
    if (!network)
    { failBeforeRequest(tr("Assistant networking is unavailable.")); return; }
    const bool deepSeek = settings.getType() == RAgentSettings::Type::DeepSeek;
    const bool streamEnabled = requestStreamingSnapshot && providerSupportsStreaming(settings.getType());
    const bool thinkingDisabled = deepSeek && requestEffortSnapshot == QStringLiteral("off");
    const QString deepSeekEffort = deepSeek && !thinkingDisabled ? requestEffortSnapshot : QString();
    const int maxTokens = deepSeek && !thinkingDisabled ? 8192 : 2048;
    QByteArray body; QString error;
    if (!RAgent::buildToolRequest(settings.getModel().trimmed(), maxTokens, 1.0, messages, tools,
                                 thinkingDisabled, &body, &error, deepSeekEffort, streamEnabled))
    { failBeforeRequest(error); return; }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", streamEnabled ? "text/event-stream" : "application/json");
    request.setRawHeader("Authorization", "Bearer " + providerKeyValue.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto *pending = network->post(request, body);
    if (!pending)
    { failBeforeRequest(tr("The provider request could not be started.")); return; }
    reply = pending;
    pending->setReadBufferSize(64 * 1024);
    auto responseBytes = QSharedPointer<QByteArray>::create();
    auto streamParser = QSharedPointer<RAgentToolStream>::create();
    auto tooLarge = QSharedPointer<bool>::create(false);
    auto streamFailure = QSharedPointer<QString>::create();
    const auto consumeAvailable = [this, pending, requestId, generation, streamEnabled,
                                   responseBytes, streamParser, tooLarge, streamFailure] {
        const QByteArray chunk = pending->readAll();
        if (chunk.isEmpty()) return;
        if (!isCurrentProviderRequest(requestId, generation, pending)) return;
        const int statusCode = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) return; // Provider error bodies are never parsed or displayed.
        if (streamEnabled)
        {
            if (!streamFailure->isEmpty()) return;
            const bool parsed = streamParser->feed(chunk);
            appendStreamText(requestId, streamParser->takeTextDelta());
            if (!parsed)
            {
                *streamFailure = streamParser->errorMessage();
                pending->abort();
            }
            return;
        }
        if (*tooLarge) return;
        if (chunk.size() > MaxProviderResponseBytes - responseBytes->size())
        {
            *tooLarge = true;
            responseBytes->clear();
            pending->abort();
            return;
        }
        responseBytes->append(chunk);
    };
    connect(pending, &QNetworkReply::readyRead, this, [consumeAvailable] {
        consumeAvailable();
    });
    connect(pending, &QNetworkReply::finished, this,
            [this, pending, requestId, generation, streamEnabled, consumeAvailable,
             responseBytes, streamParser, tooLarge, streamFailure] {
        consumeAvailable();
        const bool wasCurrent = isCurrentProviderRequest(requestId, generation, pending);
        const auto statusCode = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = pending->error();
        if (reply == pending) reply.clear();
        pending->deleteLater();
        if (!wasCurrent) return;

        // Invalidate this request before delivering a turn; runner signals may start a new one inline.
        activeProviderRequestId.clear();
        ++providerRequestGeneration;
        if (networkError != QNetworkReply::NoError || statusCode != 200 || *tooLarge
            || !streamFailure->isEmpty())
        {
            // Provider error bodies may echo request data; never put them in logs or the transcript.
            if (streamEnabled && !streamFailure->isEmpty())
                state->setToolTip(*streamFailure);
            if (liveStreamRequestId == requestId)
                finishStreamText(requestId, liveStreamText, false);
            const QString message = !streamFailure->isEmpty()
                ? *streamFailure
                : (*tooLarge ? tr("Provider response exceeds the 1 MiB limit.")
                             : tr("Provider request failed (HTTP %1, network code %2). Check credentials, connection and quota.")
                                   .arg(statusCode).arg(int(networkError)));
            runner.providerFailed(requestId, message);
            return;
        }

        QJsonObject turn;
        QString parseError;
        if (streamEnabled)
        {
            turn = streamParser->finish();
            if (turn.isEmpty())
            {
                if (liveStreamRequestId == requestId)
                    finishStreamText(requestId, liveStreamText, false);
                runner.providerFailed(requestId, streamParser->errorMessage().isEmpty()
                    ? tr("The provider stream ended before a complete response was received.")
                    : streamParser->errorMessage());
                return;
            }
        }
        else if (!RAgent::parseToolResponse(*responseBytes, &turn, &parseError))
        { runner.providerFailed(requestId, parseError); return; }

        if (streamEnabled && liveStreamRequestId == requestId)
        {
            finalizingStreamMessage = true;
            finalizingStreamText = turn.value(QStringLiteral("text")).toString();
        }
        runner.receiveTurn(requestId, turn);
        if (finalizingStreamMessage && liveStreamRequestId == requestId)
        {
            finalizingStreamMessage = false;
            finalizingStreamText.clear();
            clearStreamText(requestId);
        }
        emit providerUsage(turn.value("usage").toObject());
    });
}
