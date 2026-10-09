#include <QtTest>
#include <QCheckBox>

#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QFont>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QUuid>
#include <cstring>
#include <utility>

#include <rml_model.h>

#include "agent_widget.h"
#include "application.h"
#include "application_settings.h"
#include "model_command_host.h"
#include "session.h"
#include "workflow_process.h"

namespace
{
class PreferenceSnapshot
{
public:
    explicit PreferenceSnapshot(QSettings *source) : settings(source)
    {
        if (!settings) return;
        for (const QString &key : {QStringLiteral("aiAgent/providerType"), QStringLiteral("aiAgent/endpoint"),
                                   QStringLiteral("aiAgent/model"), QStringLiteral("aiAgent/effort"),
                                   QStringLiteral("aiAgent/automaticChanges"), QStringLiteral("aiAgent/streamingResponses"),
                                   QStringLiteral("aiAgent/apiKey")})
        {
            present.insert(key, settings->contains(key));
            if (present.value(key)) values.insert(key, settings->value(key));
        }
    }

    ~PreferenceSnapshot()
    {
        if (!settings) return;
        for (auto it = present.cbegin(); it != present.cend(); ++it)
        {
            if (it.value()) settings->setValue(it.key(), values.value(it.key()));
            else settings->remove(it.key());
        }
    }

private:
    QSettings *settings;
    QHash<QString, bool> present;
    QHash<QString, QVariant> values;
};

Model conversationFixture()
{
    Model model;
    model.setName(QStringLiteral("Conversation widget fixture"));
    model.setNNodes(2);
    model.setNode(0, RNode(0.0, 0.0, 0.0));
    model.setNode(1, RNode(1.0, 0.0, 0.0));
    model.setNElements(1);
    RElement element(R_ELEMENT_TRUSS1);
    element.setNodeId(0, 0);
    element.setNodeId(1, 1);
    model.setElement(0, element);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    auto &modal = model.getProblemSetup().getModalSetup();
    modal.setMethod(R_MODAL_MULTIPLE_MODES);
    modal.setNIterations(12);
    modal.setNModesToExtract(2);
    return model;
}

QJsonObject toolCall(const QString &id, const QString &name, const QJsonObject &arguments)
{
    const QString encodedArguments = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    const QJsonObject providerCall{{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name},
            {QStringLiteral("arguments"), encodedArguments}}}};
    const QJsonObject assistant{{QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), QStringLiteral("Preparing the next step.")},
        {QStringLiteral("tool_calls"), QJsonArray{providerCall}}};
    const QJsonObject normalized{{QStringLiteral("id"), id}, {QStringLiteral("name"), name},
        {QStringLiteral("arguments"), arguments}};
    return {{QStringLiteral("text"), QStringLiteral("Preparing the next step.")},
        {QStringLiteral("toolCalls"), QJsonArray{normalized}}, {QStringLiteral("assistantMessage"), assistant},
        {QStringLiteral("usage"), QJsonObject{}}};
}

QJsonObject finalTurn(const QString &text)
{
    const QJsonObject assistant{{QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("content"), text}};
    return {{QStringLiteral("text"), text}, {QStringLiteral("toolCalls"), QJsonArray{}},
        {QStringLiteral("assistantMessage"), assistant}, {QStringLiteral("usage"), QJsonObject{}}};
}

struct NetworkScript
{
    QList<QByteArray> chunks;
    int status = 200;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    int chunkDelayMs = 0;
    bool deliverAfterAbort = false;
};

class FixtureReply final : public QNetworkReply
{
public:
    FixtureReply(const QNetworkRequest &request, QNetworkAccessManager *manager, NetworkScript script)
        : QNetworkReply(manager), response(std::move(script))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::PostOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        setHeader(QNetworkRequest::ContentTypeHeader,
                  response.status == 200 ? QByteArray("application/json") : QByteArray("text/plain"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void begin()
    {
        QTimer::singleShot(response.chunkDelayMs, this, &FixtureReply::sendNextChunk);
    }

    void abort() override
    {
        if (isFinished()) return;
        if (!response.deliverAfterAbort)
            complete(QNetworkReply::OperationCanceledError, QStringLiteral("fixture request cancelled"));
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return unread.size() + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *destination, qint64 maximum) override
    {
        if (unread.isEmpty()) return isFinished() ? -1 : 0;
        const qint64 count = qMin(maximum, qint64(unread.size()));
        std::memcpy(destination, unread.constData(), size_t(count));
        unread.remove(0, qsizetype(count));
        return count;
    }

private:
    void sendNextChunk()
    {
        if (isFinished()) return;
        if (nextChunk < response.chunks.size())
        {
            unread.append(response.chunks.at(nextChunk++));
            emit readyRead();
            QTimer::singleShot(response.chunkDelayMs, this, &FixtureReply::sendNextChunk);
            return;
        }
        complete(response.error, response.error == QNetworkReply::NoError
            ? QString() : QStringLiteral("fixture transport failure"));
    }

    void complete(QNetworkReply::NetworkError error, const QString &message)
    {
        if (isFinished()) return;
        if (error != QNetworkReply::NoError)
        {
            setError(error, message);
            emit errorOccurred(error);
        }
        setFinished(true);
        emit finished();
    }

    NetworkScript response;
    QByteArray unread;
    qsizetype nextChunk = 0;
};

class FixtureNetworkAccessManager final : public QNetworkAccessManager
{
public:
    void enqueue(NetworkScript script) { scripts.append(std::move(script)); }
    QList<QNetworkRequest> requests;
    QList<QByteArray> requestBodies;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        Q_UNUSED(operation);
        requests.append(request);
        requestBodies.append(outgoingData ? outgoingData->readAll() : QByteArray());
        NetworkScript script;
        if (!scripts.isEmpty()) script = scripts.takeFirst();
        auto *reply = new FixtureReply(request, this, std::move(script));
        QTimer::singleShot(0, reply, [reply] { reply->begin(); });
        return reply;
    }

private:
    QList<NetworkScript> scripts;
};

QByteArray streamEvent(const QJsonObject &chunk)
{
    return QByteArray("data: ") + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\r\n\r\n";
}

QByteArray streamDone()
{
    return QByteArray("data: [DONE]\r\n\r\n");
}

QJsonObject streamTextChunk(const QString &text)
{
    return {{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("index"), 0},
        {QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), text}}},
        {QStringLiteral("finish_reason"), QJsonValue(QJsonValue::Null)}}}}};
}

QJsonObject streamFinishChunk(const QString &reason)
{
    return {{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("index"), 0}, {QStringLiteral("delta"), QJsonObject{}},
        {QStringLiteral("finish_reason"), reason}}}}};
}

NetworkScript streamingTextScript(const QString &firstText, const QString &secondText, int delayMs = 0)
{
    const QByteArray firstEvent = streamEvent(streamTextChunk(firstText));
    QByteArray secondEvent = streamEvent(streamTextChunk(secondText));
    NetworkScript script;
    script.chunkDelayMs = delayMs;
    const QByteArray utf8Needle = QStringLiteral("λ").toUtf8();
    secondEvent.replace(QByteArray("\\u03bb"), utf8Needle);
    secondEvent.replace(QByteArray("\\u03BB"), utf8Needle);
    const qsizetype utf8Position = secondEvent.indexOf(utf8Needle);
    script.chunks << firstEvent;
    if (utf8Position >= 0 && utf8Position + 1 < secondEvent.size())
    {
        script.chunks << secondEvent.left(utf8Position + 1)
                      << secondEvent.mid(utf8Position + 1, 1)
                      << secondEvent.mid(utf8Position + 2)
                      + streamEvent(streamFinishChunk(QStringLiteral("stop"))) + streamDone();
    }
    else
    {
        const qsizetype split = secondEvent.size() / 2;
        script.chunks << secondEvent.left(split)
                      << secondEvent.mid(split)
                      + streamEvent(streamFinishChunk(QStringLiteral("stop"))) + streamDone();
    }
    return script;
}

NetworkScript completeJsonScript(const QString &text)
{
    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                              {QStringLiteral("content"), text}};
    const QJsonObject response{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("finish_reason"), QStringLiteral("stop")},
        {QStringLiteral("message"), message}}}},
        {QStringLiteral("usage"), QJsonObject{}}};
    NetworkScript script;
    script.chunks << QJsonDocument(response).toJson(QJsonDocument::Compact);
    return script;
}

NetworkScript streamToolCallScript(const QString &id, const QString &name, const QJsonObject &arguments)
{
    const QString argumentText = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    const QJsonObject fragment{{QStringLiteral("index"), 0}, {QStringLiteral("id"), id},
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name},
            {QStringLiteral("arguments"), argumentText}}}};
    const QJsonObject toolChunk{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("index"), 0},
        {QStringLiteral("delta"), QJsonObject{{QStringLiteral("tool_calls"), QJsonArray{fragment}}}},
        {QStringLiteral("finish_reason"), QJsonValue(QJsonValue::Null)}}}}};
    NetworkScript script;
    script.chunks << streamEvent(toolChunk)
                  << streamEvent(streamFinishChunk(QStringLiteral("tool_calls")))
                  << streamDone();
    return script;
}
}

class TestAgentConversationWidget : public QObject
{
    Q_OBJECT
private slots:
    void composerSettingsQuestionApprovalAndFollowup()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        PreferenceSnapshot restore(applicationSettings);
        applicationSettings->setValue(QStringLiteral("aiAgent/automaticChanges"), false);
        const QString persistedEndpointKey = QStringLiteral("aiAgent/endpoint");
        applicationSettings->setValue(persistedEndpointKey,
            QStringLiteral("https://api.deepseek.com/chat/completions?api_key=legacy-endpoint-secret"));

        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);
        const auto stamp = session->modelCommandStamp(0);
        QVERIFY(!stamp.first.isEmpty());

        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow);
        auto *composer = widget.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        auto *send = widget.findChild<QPushButton *>(QStringLiteral("agentSend"));
        auto *stop = widget.findChild<QPushButton *>(QStringLiteral("agentStop"));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(composer);
        QVERIFY(send);
        QVERIFY(stop);
        QVERIFY(transcript);
        QVERIFY(!transcript->openLinks());
        QVERIFY(!transcript->openExternalLinks());
        QVERIFY(widget.findChild<QLineEdit *>(QStringLiteral("agentSettingsApiKey")) == nullptr);

        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        const QString secret = QStringLiteral("conversation-widget-secret-sentinel");
        settings.setApiKey(secret);
        widget.setProviderSettings(settings);
        const QString lastValidEndpoint = widget.getProviderSettings().getApiUrl();
        const QString persistedEndpoint = applicationSettings->value(persistedEndpointKey).toString();
        QVERIFY(!persistedEndpoint.contains(QStringLiteral("legacy-endpoint-secret")));
        RAgentSettings credentialUrl = settings;
        credentialUrl.setApiUrl(QStringLiteral("https://api.deepseek.com/chat/completions?api_key=endpoint-secret-sentinel"));
        widget.setProviderSettings(credentialUrl);
        QCOMPARE(widget.getProviderSettings().getApiUrl(), lastValidEndpoint);
        QCOMPARE(applicationSettings->value(persistedEndpointKey).toString(), persistedEndpoint);
        QVERIFY(widget.findChild<QLabel *>(QStringLiteral("agentConversationStatus"))->text().contains(QStringLiteral("Invalid")));
        for (const QString &key : applicationSettings->allKeys())
            QVERIFY2(applicationSettings->value(key).toString() != secret, "API key was written to QSettings");

        QWidget settingsPage;
        auto *settingsLayout = new QVBoxLayout(&settingsPage);
        settingsLayout->addWidget(AgentWidget::createSettingsPage(&settingsPage));
        auto *visibleKey = settingsPage.findChild<QLineEdit *>(QStringLiteral("agentSettingsApiKey"));
        auto *visibleEndpoint = settingsPage.findChild<QLineEdit *>(QStringLiteral("agentSettingsEndpoint"));
        auto *endpointValidation = settingsPage.findChild<QLabel *>(QStringLiteral("agentEndpointValidation"));
        auto *provider = settingsPage.findChild<QComboBox *>(QStringLiteral("agentSettingsProvider"));
        QVERIFY(visibleKey);
        QVERIFY(visibleEndpoint);
        QVERIFY(endpointValidation);
        QVERIFY(provider);
        visibleKey->setText(QStringLiteral("settings-page-secret-sentinel"));
        for (const QString &key : applicationSettings->allKeys())
            QVERIFY(applicationSettings->value(key).toString() != QStringLiteral("settings-page-secret-sentinel"));
        const QString pageAcceptedEndpoint = visibleEndpoint->text();
        visibleEndpoint->setText(QStringLiteral("https://user:password@api.deepseek.com/chat/completions"));
        QVERIFY(QMetaObject::invokeMethod(visibleEndpoint, "editingFinished", Qt::DirectConnection));
        QCOMPARE(visibleEndpoint->text(), pageAcceptedEndpoint);
        QVERIFY(!endpointValidation->text().isEmpty());
        QVERIFY(!applicationSettings->value(persistedEndpointKey).toString().contains(QStringLiteral("password")));

        // Keep the UI and runner's state connections, replacing only the live
        // provider adapter so this lifecycle test never opens a network request.
        QVERIFY(QObject::disconnect(widget.taskRunner(),
            &AgentTaskRunner::turnRequested, &widget, nullptr));
        QSignalSpy requests(widget.taskRunner(), &AgentTaskRunner::turnRequested);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);

        widget.show();
        composer->setPlainText(QStringLiteral("Help me choose a modal setting."));
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 1000);
        QVERIFY(widget.taskRunner()->isActive());
        QCOMPARE(send->text(), QStringLiteral("Send now"));
        QVERIFY(composer->isEnabled());
        QVERIFY(stop->isVisible());

        const QJsonObject askArguments{{QStringLiteral("question"), QStringLiteral("<b>Which comparison do you need?</b>")},
            {QStringLiteral("options"), QJsonArray{QStringLiteral("Fixed base"), QStringLiteral("Moving base")}}};
        widget.taskRunner()->receiveTurn(requests.at(0).at(0).toString(),
            toolCall(QStringLiteral("ask-question-1"), QStringLiteral("ask_user"), askArguments));
        QTRY_VERIFY_WITH_TIMEOUT(widget.findChild<QFrame *>(QStringLiteral("agentQuestionCard"))->isVisible(), 1000);
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("<b>Which comparison do you need?</b>")));

        // Settings can change while a question is pending; the answer belongs
        // to the existing task, and the old task snapshot remains sufficient.
        RAgentSettings changed = widget.getProviderSettings();
        changed.setApiUrl(QStringLiteral("https://changed.invalid/chat/completions"));
        changed.setApiKey(QString());
        widget.setProviderSettings(changed);
        composer->setPlainText(QStringLiteral("Fixed base"));
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 2, 1000);
        QVERIFY(!widget.taskRunner()->isAwaitingInput());
        QVERIFY(!widget.findChild<QFrame *>(QStringLiteral("agentQuestionCard"))->isVisible());

        const auto context = host->handle({{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), QStringLiteral("context.query")},
            {QStringLiteral("parameters"), QJsonObject{}}}).value(QStringLiteral("result")).toObject();
        const auto documents = context.value(QStringLiteral("documents")).toArray();
        QVERIFY(!documents.isEmpty());
        const auto currentDocument = documents.first().toObject();
        const QJsonObject proposalArguments{{QStringLiteral("documentId"), stamp.first},
            {QStringLiteral("documentRevision"), currentDocument.value(QStringLiteral("documentRevision"))},
            {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("method"), QStringLiteral("multiple")},
                {QStringLiteral("iterations"), 24}, {QStringLiteral("modes"), 2},
                {QStringLiteral("tolerance"), 1e-6}, {QStringLiteral("unit"), QStringLiteral("1")}}}};
        widget.taskRunner()->receiveTurn(requests.at(1).at(0).toString(),
            toolCall(QStringLiteral("document-proposal-1"), QStringLiteral("document_propose"), proposalArguments));
        auto *approval = widget.findChild<QFrame *>(QStringLiteral("agentApprovalCard"));
        auto *reject = widget.findChild<QPushButton *>(QStringLiteral("agentRejectProposal"));
        QVERIFY(approval);
        QVERIFY(reject);
        QTRY_VERIFY_WITH_TIMEOUT(approval->isVisible(), 1000);
        QVERIFY(reject->isEnabled());
        reject->click();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 3, 1000);
        QVERIFY(widget.taskRunner()->pendingProposal().isEmpty());
        QVERIFY(!approval->isVisible());

        widget.taskRunner()->receiveTurn(requests.at(2).at(0).toString(), finalTurn(QStringLiteral("Proposal rejected.")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        widget.setProviderSettings(settings);
        composer->setPlainText(QStringLiteral("A follow-up message."));
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 4, 1000);
        QCOMPARE(send->text(), QStringLiteral("Send now"));
        widget.taskRunner()->receiveTurn(requests.at(3).at(0).toString(), finalTurn(QStringLiteral("- Follow-up **finished** once.")));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 1000);
        QCOMPARE(transcript->toPlainText().count(QStringLiteral("Follow-up finished once.")), 1);
        QVERIFY(transcript->document()->lastBlock().textList() == nullptr);

        session->clear();
    }

    void streamingRenderPreservesComposerAndScroll()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        PreferenceSnapshot restore(applicationSettings);
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        WorkflowProcess workflow;
        FixtureNetworkAccessManager network;
        network.enqueue(streamingTextScript(QStringLiteral("First part. "),
            QStringLiteral("UTF-8 λ 🚀 and **bold** <b>raw</b>\n\n- item\n\n|Case|Result|\n|---|---|\n|fixture|ok|\n\n```cpp\nint value = 1;\n```"), 35));
        AgentWidget widget(host, &workflow, nullptr, &network);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("offline-stream-secret"));
        widget.setProviderSettings(settings);
        auto *streaming = widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        auto *composer = widget.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(streaming);
        QVERIFY(composer);
        QVERIFY(transcript);
        streaming->setChecked(true);

        QStringList history;
        for (int i = 0; i < 80; ++i) history.append(QStringLiteral("Earlier message %1").arg(i));
        transcript->setPlainText(history.join(QLatin1Char('\n')));
        widget.resize(480, 520);
        widget.show();
        QTRY_VERIFY_WITH_TIMEOUT(transcript->verticalScrollBar()->maximum() > 0, 1000);
        transcript->verticalScrollBar()->setValue(0);

        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        widget.startTask(QStringLiteral("Stream the fixture reply."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 1000);
        const QJsonObject requestBody = QJsonDocument::fromJson(network.requestBodies.first()).object();
        QVERIFY(requestBody.value(QStringLiteral("stream")).toBool());
        QCOMPARE(network.requests.first().rawHeader("Accept"), QByteArray("text/event-stream"));
        QVERIFY(!network.requestBodies.first().contains("offline-stream-secret"));
        RAgentSettings changedSettings = settings;
        changedSettings.setApiUrl(QStringLiteral("https://changed.invalid/chat/completions"));
        changedSettings.setApiKey(QStringLiteral("changed-secret"));
        widget.setProviderSettings(changedSettings);
        streaming->setChecked(false); // The active turn keeps its original provider and transport snapshot.

        composer->setPlainText(QStringLiteral("draft to keep"));
        QTextCursor draftSelection = composer->textCursor();
        draftSelection.setPosition(9);
        draftSelection.setPosition(13, QTextCursor::KeepAnchor);
        composer->setTextCursor(draftSelection);
        composer->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(composer->hasFocus(), 1000);

        QTRY_VERIFY_WITH_TIMEOUT(transcript->toPlainText().contains(QStringLiteral("First part.")), 2000);
        QCOMPARE(composer->toPlainText(), QStringLiteral("draft to keep"));
        QCOMPARE(composer->textCursor().selectedText(), QStringLiteral("keep"));
        QVERIFY(composer->hasFocus());
        QCOMPARE(transcript->verticalScrollBar()->value(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        const QString rendered = transcript->toPlainText();
        QCOMPARE(rendered.count(QStringLiteral("First part.")), 1);
        QVERIFY(rendered.contains(QStringLiteral("UTF-8 λ 🚀")));
        QVERIFY(rendered.contains(QStringLiteral("<b>raw</b>")));
        QVERIFY(rendered.contains(QStringLiteral("item")));
        const QString html = transcript->document()->toHtml();
        QVERIFY(html.contains(QStringLiteral("<ul")));
        QVERIFY(html.contains(QStringLiteral("<table")));
        QVERIFY(html.contains(QStringLiteral("<pre")));
        const QString renderDirectory = qEnvironmentVariable("RANGE_AGENT_STREAM_EVIDENCE_DIRECTORY");
        if (!renderDirectory.isEmpty())
        {
            QVERIFY(QDir().mkpath(renderDirectory));
            QVERIFY(widget.grab().save(QDir(renderDirectory).filePath("stream-render.png")));
        }
        const QTextCursor boldText = transcript->document()->find(QStringLiteral("bold"));
        QVERIFY(!boldText.isNull());
        QVERIFY(boldText.charFormat().fontWeight() >= QFont::Bold);
        QCOMPARE(composer->textCursor().selectedText(), QStringLiteral("keep"));
        QVERIFY(composer->hasFocus());
        QCOMPARE(transcript->verticalScrollBar()->value(), 0);

        if (!renderDirectory.isEmpty())
        {
            transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum());
            QVERIFY(widget.grab().save(QDir(renderDirectory).filePath("stream-markdown-tail.png")));
        }
        network.enqueue(streamingTextScript(QStringLiteral("Tail-follow "), QStringLiteral("confirmed."), 20));
        streaming->setChecked(true);
        transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum());
        widget.startTask(QStringLiteral("Continue while following the latest reply."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(transcript->toPlainText().contains(QStringLiteral("Tail-follow")), 2000);
        QCOMPARE(transcript->verticalScrollBar()->value(), transcript->verticalScrollBar()->maximum());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 3000);
        QCOMPARE(transcript->verticalScrollBar()->value(), transcript->verticalScrollBar()->maximum());
        QCOMPARE(composer->textCursor().selectedText(), QStringLiteral("keep"));
        QVERIFY(composer->hasFocus());

        session->clear();
    }

    void malformedToolArgumentsAreNeverDispatchedAndJsonFallbackWorks()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        PreferenceSnapshot restore(applicationSettings);
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);
        const auto before = session->modelCommandStamp(0);

        const QJsonObject malformedFragment{{QStringLiteral("index"), 0},
            {QStringLiteral("id"), QStringLiteral("call-partial")},
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), QStringLiteral("document_propose")},
                {QStringLiteral("arguments"), QStringLiteral("{\"command\":")}}}};
        const QJsonObject malformedChunk{{QStringLiteral("choices"), QJsonArray{QJsonObject{
            {QStringLiteral("index"), 0},
            {QStringLiteral("delta"), QJsonObject{{QStringLiteral("tool_calls"), QJsonArray{malformedFragment}}}},
            {QStringLiteral("finish_reason"), QJsonValue(QJsonValue::Null)}}}}};
        NetworkScript malformed;
        malformed.chunks << streamEvent(malformedChunk)
                         << streamEvent(streamFinishChunk(QStringLiteral("tool_calls")))
                         << streamDone();

        WorkflowProcess workflow;
        FixtureNetworkAccessManager network;
        network.enqueue(std::move(malformed));
        network.enqueue(completeJsonScript(QStringLiteral("JSON fallback completed.")));
        AgentWidget widget(host, &workflow, nullptr, &network);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("offline-fallback-secret"));
        widget.setProviderSettings(settings);
        auto *streaming = widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(streaming);
        QVERIFY(transcript);
        streaming->setChecked(true);
        QSignalSpy activities(widget.taskRunner(), &AgentTaskRunner::toolActivity);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);

        widget.startTask(QStringLiteral("Malformed partial action."));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("failed"));
        QVERIFY(activities.isEmpty());
        QVERIFY(widget.taskRunner()->pendingProposal().isEmpty());
        const auto after = session->modelCommandStamp(0);
        QCOMPARE(after.first, before.first);
        QCOMPARE(after.second, before.second);

        streaming->setChecked(false);
        widget.startTask(QStringLiteral("Use complete JSON fallback."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 1000);
        const QJsonObject fallbackBody = QJsonDocument::fromJson(network.requestBodies.last()).object();
        QVERIFY(!fallbackBody.value(QStringLiteral("stream")).toBool());
        QCOMPARE(network.requests.last().rawHeader("Accept"), QByteArray("application/json"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 2000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(transcript->toPlainText().count(QStringLiteral("JSON fallback completed.")), 1);

        session->clear();
    }

    void stopTimeoutAndLateBytesStayWithTheirRequest()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        PreferenceSnapshot restore(applicationSettings);
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        NetworkScript late = streamingTextScript(QStringLiteral("Old partial. "),
            QStringLiteral("Late stale bytes"), 35);
        late.deliverAfterAbort = true;
        NetworkScript timeout;
        timeout.error = QNetworkReply::TimeoutError;
        NetworkScript providerError;
        providerError.status = 401;
        providerError.chunks << QByteArray("credential-echo-sentinel provider error body");
        WorkflowProcess workflow;
        FixtureNetworkAccessManager network;
        network.enqueue(std::move(late));
        network.enqueue(std::move(timeout));
        network.enqueue(std::move(providerError));
        AgentWidget widget(host, &workflow, nullptr, &network);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("offline-cancel-secret"));
        widget.setProviderSettings(settings);
        widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"))->setChecked(true);
        auto *stop = widget.findChild<QPushButton *>(QStringLiteral("agentStop"));
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(stop);
        QVERIFY(transcript);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        widget.show();

        widget.startTask(QStringLiteral("Stop the old request."));
        QTRY_VERIFY_WITH_TIMEOUT(transcript->toPlainText().contains(QStringLiteral("Old partial.")), 2000);
        stop->click();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("cancelled"));

        widget.startTask(QStringLiteral("Exercise the scoped timeout."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 2, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 2000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
        widget.startTask(QStringLiteral("Hide provider error details."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 2000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("failed"));
        QTest::qWait(250);
        QVERIFY(!transcript->toPlainText().contains(QStringLiteral("Late stale bytes")));
        QVERIFY(!transcript->toPlainText().contains(QStringLiteral("credential-echo-sentinel")));
        QVERIFY(!transcript->toPlainText().contains(QStringLiteral("provider error body")));

        session->clear();
    }

    void reentrantFollowupAndTrustedActivityKeepStableIds()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        PreferenceSnapshot restore(applicationSettings);
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        WorkflowProcess workflow;
        FixtureNetworkAccessManager network;
        network.enqueue(streamToolCallScript(QStringLiteral("call-context-1"),
            QStringLiteral("context_query"), QJsonObject{}));
        network.enqueue(streamingTextScript(QStringLiteral("The host returned "),
            QStringLiteral("context.")));
        network.enqueue(streamingTextScript(QStringLiteral("Follow-up "), QStringLiteral("received.")));
        AgentWidget widget(host, &workflow, nullptr, &network);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("offline-activity-secret"));
        widget.setProviderSettings(settings);
        widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"))->setChecked(true);
        auto *activityCards = widget.findChild<QWidget *>(QStringLiteral("agentActivityCards"));
        QVERIFY(activityCards);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        bool steeredReentrantly = false;
        connect(widget.taskRunner(), &AgentTaskRunner::assistantMessage, &widget,
                [&widget, &steeredReentrantly](const QString &message) {
            if (steeredReentrantly || !message.contains(QStringLiteral("context."))) return;
            steeredReentrantly = true;
            widget.taskRunner()->submitMessage(QStringLiteral("Follow up during signal delivery."), false);
        }, Qt::DirectConnection);

        widget.startTask(QStringLiteral("Read the current model context."));
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(!activityCards->findChildren<QFrame *>(QStringLiteral("agentActivityCard")).isEmpty(), 2000);
        const auto cardFound = [activityCards] {
            for (QFrame *card : activityCards->findChildren<QFrame *>(QStringLiteral("agentActivityCard")))
                if (card->property("agentToolCallId").toString() == QStringLiteral("call-context-1"))
                    return card;
            return static_cast<QFrame *>(nullptr);
        };
        QTRY_VERIFY_WITH_TIMEOUT(cardFound(), 2000);
        QVERIFY(!cardFound()->property("agentTaskId").toString().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(cardFound()->property("agentActivityPhase").toString(),
                                  QStringLiteral("completed"), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(network.requests.size() >= 2, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(network.requests.size(), 3, 1000);
        QVERIFY(steeredReentrantly);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 3000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("superseded"));
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("completed"));
        auto *renderedTranscript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(renderedTranscript);
        QVERIFY(renderedTranscript->toPlainText().contains(
            QRegularExpression(QStringLiteral("context\\.\\s+superseded"))));

        const QString renderDirectory = qEnvironmentVariable("RANGE_AGENT_STREAM_EVIDENCE_DIRECTORY");
        if (!renderDirectory.isEmpty())
        {
            widget.resize(760, 900);
            widget.show();
            QVERIFY(QTest::qWaitForWindowExposed(&widget, 5000));
            QVERIFY(QDir().mkpath(renderDirectory));
            QVERIFY(widget.grab().save(QDir(renderDirectory).filePath("stream-activity.png")));
        }

        session->clear();
    }
};

int runAgentConversationWidgetTests(int argc, char **argv)
{
    TestAgentConversationWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_conversation_widget.moc"
