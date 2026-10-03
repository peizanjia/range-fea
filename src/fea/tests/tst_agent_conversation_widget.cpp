#include <QtTest>

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QTextBlock>
#include <QVBoxLayout>
#include <QUuid>

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
                                   QStringLiteral("aiAgent/automaticChanges"), QStringLiteral("aiAgent/apiKey")})
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
};

int runAgentConversationWidgetTests(int argc, char **argv)
{
    TestAgentConversationWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_conversation_widget.moc"
