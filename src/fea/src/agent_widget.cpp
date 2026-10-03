#include "agent_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTextBrowser>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include "model_command_host.h"
#include <rai_agent.h>

AgentWidget::AgentWidget(ModelCommandHost *host, WorkflowProcess *workflow, QWidget *parent)
    : QWidget(parent), runner(host, workflow, this), network(this), commandHost(host)
{
    setObjectName("agentTaskWidget");
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Describe a task. The assistant reads current state, proposes supported changes, and shows the results here. Calculations continue independently when you stop the assistant."));
    intro->setWordWrap(true); layout->addWidget(intro);
    auto *form = new QFormLayout;
    provider = new QComboBox;
    provider->addItem("DeepSeek", int(RAgentSettings::Type::DeepSeek));
    provider->addItem(tr("OpenAI-compatible Chat Completions"), int(RAgentSettings::Type::OpenAILegacy));
    endpoint = new QLineEdit("https://api.deepseek.com/chat/completions");
    model = new QLineEdit("deepseek-flash");
    key = new QLineEdit; key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(tr("API key (this session only)"));
    key->setObjectName("agentApiKey");
    connect(provider, &QComboBox::currentIndexChanged, this, [this] {
        const RAgentSettings defaults(static_cast<RAgentSettings::Type>(provider->currentData().toInt()));
        endpoint->setText(defaults.getApiUrl()); model->setText(defaults.getModel()); key->clear();
    });
    form->addRow(tr("Provider"), provider); form->addRow(tr("Endpoint"), endpoint);
    form->addRow(tr("Model"), model); form->addRow(tr("API key"), key);
    layout->addLayout(form);
    auto *loadKey = new QPushButton(tr("Load key from local file…"));
    layout->addWidget(loadKey);
    connect(&runner, &AgentTaskRunner::stateChanged, loadKey, [this, loadKey] {
        loadKey->setEnabled(!runner.isActive());
    });
    connect(loadKey, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Select local credentials file"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        { state->setText(tr("Cannot read the credentials file.")); return; }
        const auto name = provider->currentIndex() == 0 ? "DEEPSEEK_API_KEY=" : "OPENAI_API_KEY=";
        for (auto line : QString::fromUtf8(file.readAll()).split('\n'))
        {
            line = line.trimmed();
            if (!line.startsWith(name)) continue;
            auto value = line.mid(int(QString(name).size())).trimmed();
            if (value.size() >= 2 && ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith('\'') && value.endsWith('\''))))
                value = value.mid(1, value.size() - 2);
            key->setText(value); state->setText(tr("Credential loaded for this session.")); return;
        }
        state->setText(tr("The file has no key for the selected provider."));
    });
    goal = new QPlainTextEdit;
    goal->setPlaceholderText(tr("For example: inspect the selected model and suggest modal settings, or run and compare the beam control experiment."));
    goal->setMaximumHeight(120); goal->setObjectName("agentGoal"); layout->addWidget(goal);
    authorize = new QCheckBox(tr("Apply authorized task changes automatically"));
    authorize->setToolTip(tr("Limited to documents selected at task start and the connected reference workspace. Extension proposals still require review."));
    authorize->setChecked(false); layout->addWidget(authorize);
    auto *scope = new QLabel(tr("Automatic changes are limited to the initially selected documents and connected reference workspace. Extension proposals require separate review."));
    scope->setWordWrap(true); layout->addWidget(scope);
    auto *buttons = new QHBoxLayout;
    start = new QPushButton(tr("Start task")); stop = new QPushButton(tr("Stop assistant"));
    buttons->addWidget(start); buttons->addWidget(stop); layout->addLayout(buttons);
    state = new QLabel(tr("Ready")); state->setWordWrap(true); layout->addWidget(state);
    auto *eventToggle = new QToolButton;
    eventToggle->setObjectName("agentEventHistoryToggle");
    eventToggle->setText(tr("Recent GUI document events"));
    eventToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    eventToggle->setArrowType(Qt::RightArrow);
    eventToggle->setCheckable(true);
    eventToggle->setChecked(false);
    layout->addWidget(eventToggle);
    auto *eventBody = new QWidget;
    eventBody->setObjectName("agentEventHistoryBody");
    auto *eventLayout = new QVBoxLayout(eventBody);
    eventLayout->setContentsMargins(0, 0, 0, 0);
    auto *eventNotice = new QLabel(tr("In-memory session event history (up to 256 records); this is not a durable audit log."));
    eventNotice->setObjectName("agentEventHistoryNotice");
    eventNotice->setWordWrap(true);
    eventLayout->addWidget(eventNotice);
    eventContextSummary = new QLabel(tr("Current document context has not been queried yet."));
    eventContextSummary->setObjectName("agentEventContextSummary");
    eventContextSummary->setWordWrap(true);
    eventLayout->addWidget(eventContextSummary);
    eventStatus = new QLabel(tr("Waiting for host events."));
    eventStatus->setObjectName("agentEventStatus");
    eventStatus->setWordWrap(true);
    eventLayout->addWidget(eventStatus);
    eventHistory = new QPlainTextEdit;
    eventHistory->setObjectName("agentEventHistory");
    eventHistory->setReadOnly(true);
    eventHistory->setMaximumHeight(180);
    eventHistory->setMaximumBlockCount(256);
    eventLayout->addWidget(eventHistory);
    eventBody->hide();
    layout->addWidget(eventBody);
    connect(eventToggle, &QToolButton::toggled, this, [eventToggle, eventBody](bool expanded) {
        eventBody->setVisible(expanded);
        eventToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    transcript = new QTextBrowser; transcript->setObjectName("agentTranscript"); layout->addWidget(transcript, 1);
    preview = new QTextBrowser; preview->setMaximumHeight(170); layout->addWidget(preview);
    auto *approval = new QHBoxLayout;
    apply = new QPushButton(tr("Apply proposal")); decline = new QPushButton(tr("Decline"));
    approval->addWidget(apply); approval->addWidget(decline); layout->addLayout(approval);
    connect(start, &QPushButton::clicked, this, [this] { startTask(goal->toPlainText(), authorize->isChecked()); });
    connect(stop, &QPushButton::clicked, &runner, &AgentTaskRunner::cancel);
    connect(apply, &QPushButton::clicked, this, [this] { runner.authorizePending(presentedProposalId, true); updateAvailability(); });
    connect(decline, &QPushButton::clicked, this, [this] { runner.authorizePending(presentedProposalId, false); updateAvailability(); });
    connect(&runner, &AgentTaskRunner::turnRequested, this, &AgentWidget::requestTurn);
    connect(&runner, &AgentTaskRunner::providerCancellationRequested, this, [this](const QString &id) {
        if (reply && reply->property("agentRequestId").toString() == id) reply->abort();
    });
    connect(&runner, &AgentTaskRunner::stateChanged, this, [this](const QString &, const QString &message) {
        state->setText(message); updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::proposalReady, this, [this](const QString &origin, const QString &id, const QJsonObject &value) {
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
        appendText(origin == "document" ? tr("Document updated") : tr("Task submitted"),
                   origin == "document" ? tr("The host accepted the change. The document views now reflect its current state.")
                                        : tr("Calculation task: %1. Progress and results are available in the workflow panel.").arg(value.value("jobId").toString()));
        presentedProposalId.clear(); preview->clear(); updateAvailability();
    });
    connect(&runner, &AgentTaskRunner::finished, this, [this](const QString &status, const QString &message) {
        appendText(status, message); presentedProposalId.clear(); preview->clear(); updateAvailability();
    });
    eventPollTimer.setInterval(750);
    connect(&eventPollTimer, &QTimer::timeout, this, &AgentWidget::refreshHostEvents);
    eventPollTimer.start();
    refreshHostEvents();
    updateAvailability();
}

void AgentWidget::setProviderSettings(const RAgentSettings &settings)
{
    if (runner.isActive()) return;
    const int index = provider->findData(int(settings.getType()));
    if (index < 0) return;
    provider->setCurrentIndex(index); endpoint->setText(settings.getApiUrl());
    model->setText(settings.getModel()); key->setText(settings.getApiKey());
}

void AgentWidget::startTask(const QString &text, bool authorized)
{
    if (runner.isActive()) return;
    if (text.trimmed().isEmpty() || key->text().trimmed().isEmpty())
    { state->setText(tr("Enter a task and configure the API key.")); return; }
    appendText(tr("Task"), text);
    runner.startTask(text, authorized);
}

void AgentWidget::appendText(const QString &title, const QString &text)
{
    transcript->append("<b>" + title.toHtmlEscaped() + "</b><pre style='white-space:pre-wrap'>" + text.toHtmlEscaped() + "</pre>");
}

void AgentWidget::updateAvailability()
{
    const bool busy = runner.isActive();
    start->setEnabled(!busy); stop->setEnabled(busy);
    provider->setEnabled(!busy); endpoint->setEnabled(!busy); model->setEnabled(!busy); key->setEnabled(!busy);
    authorize->setEnabled(!busy);
    const bool pending = busy && !runner.pendingProposal().isEmpty();
    apply->setEnabled(pending); decline->setEnabled(pending);
}

QJsonObject AgentWidget::hostRequest(const QString &operation, const QJsonObject &parameters) const
{
    if (!commandHost) return {};
    return commandHost->handle({{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                                {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("operation"), operation},
                                {QStringLiteral("parameters"), parameters}});
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
    if (reply) reply->abort();
    const QUrl url(endpoint->text().trimmed());
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment())
    { runner.providerFailed(requestId, tr("Use an HTTPS provider endpoint without embedded credentials.")); return; }
    QByteArray body; QString error;
    if (!RAgent::buildToolRequest(model->text().trimmed(), 2048, 1.0, messages, tools,
                                 provider->currentIndex() == 0, &body, &error))
    { runner.providerFailed(requestId, error); return; }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + key->text().trimmed().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto *pending = network.post(request, body); reply = pending;
    pending->setReadBufferSize(64 * 1024);
    pending->setProperty("agentRequestId", requestId);
    auto bytes = QSharedPointer<QByteArray>::create();
    connect(pending, &QNetworkReply::readyRead, this, [pending, bytes] {
        const qint64 remaining = 1024 * 1024 + 1 - bytes->size();
        if (remaining > 0) bytes->append(pending->read(remaining));
        if (bytes->size() > 1024 * 1024) pending->abort();
    });
    connect(pending, &QNetworkReply::finished, this, [this, pending, bytes, requestId] {
        const qint64 remaining = 1024 * 1024 + 1 - bytes->size();
        if (remaining > 0) bytes->append(pending->read(remaining));
        const auto statusCode = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = pending->error();
        if (reply == pending) reply.clear();
        pending->deleteLater();
        if (networkError != QNetworkReply::NoError || statusCode != 200 || bytes->size() > 1024 * 1024)
        {
            // Provider error bodies may echo request data; never put them in logs or the transcript.
            runner.providerFailed(requestId, tr("Provider request failed (HTTP %1, network code %2). Check credentials, connection and quota.").arg(statusCode).arg(int(networkError)));
            return;
        }
        QJsonObject turn; QString parseError;
        if (!RAgent::parseToolResponse(*bytes, &turn, &parseError)) { runner.providerFailed(requestId, parseError); return; }
        emit providerUsage(turn.value("usage").toObject());
        runner.receiveTurn(requestId, turn);
    });
}
