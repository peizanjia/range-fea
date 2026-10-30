#include <QtTest>
#include <QCheckBox>

#include <QCryptographicHash>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFrame>
#include <QFont>
#include <QGridLayout>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSet>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <rml_model.h>
#include <ragent_evaluation_capture_store.h>
#include <ragent_conversation_store.h>

#include "agent_widget.h"
#include "application.h"
#include "application_settings.h"
#include "main_window.h"
#include "model_command_host.h"
#include "session.h"
#include "workflow_process.h"

namespace
{
class PreferenceSnapshot
{
public:
    explicit PreferenceSnapshot(QSettings *source, const QStringList &additionalKeys = {}) : settings(source)
    {
        if (!settings) return;
        QStringList keys{QStringLiteral("aiAgent/providerType"), QStringLiteral("aiAgent/endpoint"),
                         QStringLiteral("aiAgent/model"), QStringLiteral("aiAgent/effort"),
                         QStringLiteral("aiAgent/automaticChanges"), QStringLiteral("aiAgent/streamingResponses"),
                         QStringLiteral("aiAgent/apiKey")};
        keys.append(additionalKeys);
        for (const QString &key : keys)
        {
            if (present.contains(key)) continue;
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

NetworkScript completeJsonScript(const QString &text, const QString &providerModel = {},
                                 const QJsonObject &usage = {})
{
    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("assistant")},
                              {QStringLiteral("content"), text}};
    QJsonObject response{{QStringLiteral("choices"), QJsonArray{QJsonObject{
        {QStringLiteral("finish_reason"), QStringLiteral("stop")},
        {QStringLiteral("message"), message}}}},
        {QStringLiteral("usage"), QJsonObject{}}};
    if (!providerModel.isEmpty()) response.insert(QStringLiteral("model"), providerModel);
    if (!usage.isEmpty()) response.insert(QStringLiteral("usage"), usage);
    NetworkScript script;
    script.chunks << QJsonDocument(response).toJson(QJsonDocument::Compact);
    return script;
}

class ConversationCleanup final
{
public:
    explicit ConversationCleanup(RAgentConversationStore *store) : store(store) {}
    void track(const QString &id)
    {
        if (!id.isEmpty() && !conversationIds.contains(id)) conversationIds.append(id);
    }
    ~ConversationCleanup()
    {
        if (!store) return;
        for (const QString &id : conversationIds)
        {
            QString error;
            const QJsonObject page = store->page(id, 0, 1, &error);
            if (!page.isEmpty()) store->remove(id, page.value(QStringLiteral("revision")).toString(), &error);
        }
    }

private:
    RAgentConversationStore *store = nullptr;
    QStringList conversationIds;
};

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
    void trustedExperimentAuthorizationReviewIncludesDiagnosticsAndBlocksErrors()
    {
        const QJsonObject base{{QStringLiteral("previewId"), QStringLiteral("preview-1")},
            {QStringLiteral("experimentId"), QStringLiteral("experiment-1")},
            {QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('a'))},
            {QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), QStringLiteral("doc-1")},
                {QStringLiteral("sessionEpoch"), QStringLiteral("epoch-1")}}},
            {QStringLiteral("expectedDocumentRevision"), 7},
            {QStringLiteral("sourceBinding"), QJsonObject{{QStringLiteral("documentId"), QStringLiteral("doc-1")},
                {QStringLiteral("documentRevision"), QStringLiteral("7")},
                {QStringLiteral("sessionId"), QStringLiteral("session-1")},
                {QStringLiteral("sessionEpoch"), QStringLiteral("epoch-1")},
                {QStringLiteral("snapshotSha256"), QString(64, QLatin1Char('b'))}}},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1}}}}}};
        const auto diagnostics = [](const QString &status, const QJsonArray &issues) {
            return QJsonObject{{QStringLiteral("operation"), QStringLiteral("analysis.diagnose")},
                {QStringLiteral("schemaVersion"), 1}, {QStringLiteral("status"), status},
                {QStringLiteral("sourceId"), QStringLiteral("doc-1")},
                {QStringLiteral("modelRevision"), QStringLiteral("7")},
                {QStringLiteral("issues"), issues}};
        };
        const QJsonObject readyDiagnostics = diagnostics(QStringLiteral("ready"), QJsonArray{});
        QJsonObject candidate = base;
        candidate.insert(QStringLiteral("diagnostics"), readyDiagnostics);
        bool acknowledgementRequired = true;
        QString error;
        const QJsonObject readyReview = AgentWidget::trustedExperimentAuthorizationReview(
            candidate, &acknowledgementRequired, &error);
        QVERIFY2(!readyReview.isEmpty(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(!acknowledgementRequired);
        QCOMPARE(readyReview.value(QStringLiteral("previewId")), base.value(QStringLiteral("previewId")));
        QCOMPARE(readyReview.value(QStringLiteral("authorizationDigest")), base.value(QStringLiteral("authorizationDigest")));
        QCOMPARE(readyReview.value(QStringLiteral("diagnostics")), readyDiagnostics);

        const QJsonObject warningIssue{{QStringLiteral("code"), QStringLiteral("ZERO_EXCITATION")},
                {QStringLiteral("severity"), QStringLiteral("warning")},
                {QStringLiteral("message"), QStringLiteral("A zero response may be expected.")},
                {QStringLiteral("evidence"), QJsonObject{{QStringLiteral("nonzeroExcitation"), false}}}};
        const QJsonObject warningDiagnostics = diagnostics(QStringLiteral("warning"), QJsonArray{warningIssue});
        candidate.insert(QStringLiteral("diagnostics"), warningDiagnostics);
        const QJsonObject warningReview = AgentWidget::trustedExperimentAuthorizationReview(
            candidate, &acknowledgementRequired, &error);
        QVERIFY(!warningReview.isEmpty());
        QVERIFY(acknowledgementRequired);
        QCOMPARE(warningReview.value(QStringLiteral("diagnostics")), warningDiagnostics);

        QJsonObject malformedWarning = warningIssue;
        malformedWarning.remove(QStringLiteral("code"));
        candidate.insert(QStringLiteral("diagnostics"), diagnostics(QStringLiteral("warning"), QJsonArray{malformedWarning}));
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("code, explanation, or evidence")));
        QVERIFY(!acknowledgementRequired);

        malformedWarning = warningIssue;
        malformedWarning.insert(QStringLiteral("message"), QStringLiteral("  "));
        candidate.insert(QStringLiteral("diagnostics"), diagnostics(QStringLiteral("warning"), QJsonArray{malformedWarning}));
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());

        malformedWarning = warningIssue;
        malformedWarning.remove(QStringLiteral("evidence"));
        candidate.insert(QStringLiteral("diagnostics"), diagnostics(QStringLiteral("warning"), QJsonArray{malformedWarning}));
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());

        malformedWarning = warningIssue;
        malformedWarning.insert(QStringLiteral("evidence"), QStringLiteral("not an object"));
        candidate.insert(QStringLiteral("diagnostics"), diagnostics(QStringLiteral("warning"), QJsonArray{malformedWarning}));
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());

        QJsonObject mismatchedDiagnostics = diagnostics(QStringLiteral("warning"), QJsonArray{warningIssue});
        mismatchedDiagnostics.insert(QStringLiteral("sourceId"), QStringLiteral("other-document"));
        candidate.insert(QStringLiteral("diagnostics"), mismatchedDiagnostics);
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("source document and revision")));

        mismatchedDiagnostics = diagnostics(QStringLiteral("warning"), QJsonArray{warningIssue});
        mismatchedDiagnostics.insert(QStringLiteral("modelRevision"), QStringLiteral("8"));
        candidate.insert(QStringLiteral("diagnostics"), mismatchedDiagnostics);
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());

        const QJsonObject errorIssue{{QStringLiteral("code"), QStringLiteral("BC_UNDERCONSTRAINED")},
                {QStringLiteral("severity"), QStringLiteral("error")},
                {QStringLiteral("message"), QStringLiteral("The support leaves a rigid-body mode free.")},
                {QStringLiteral("evidence"), QJsonObject{{QStringLiteral("rigidBodyModeFree"), true}}}};
        const QJsonObject errorDiagnostics = diagnostics(QStringLiteral("error"), QJsonArray{errorIssue});
        candidate.insert(QStringLiteral("diagnostics"), errorDiagnostics);
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("rigid-body mode")));
        QVERIFY(!acknowledgementRequired);

        candidate.remove(QStringLiteral("diagnostics"));
        QVERIFY(AgentWidget::trustedExperimentAuthorizationReview(candidate,
            &acknowledgementRequired, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("missing or malformed")));
    }

    void trustedExperimentWarningDialogRequiresVisibleAcknowledgement()
    {
        const QJsonObject candidate{{QStringLiteral("previewId"), QStringLiteral("preview-dialog")},
            {QStringLiteral("experimentId"), QStringLiteral("experiment-dialog")},
            {QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('c'))},
            {QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), QStringLiteral("dialog-doc")},
                {QStringLiteral("sessionEpoch"), QStringLiteral("dialog-epoch")}}},
            {QStringLiteral("expectedDocumentRevision"), 3},
            {QStringLiteral("sourceBinding"), QJsonObject{{QStringLiteral("documentId"), QStringLiteral("dialog-doc")},
                {QStringLiteral("documentRevision"), QStringLiteral("3")},
                {QStringLiteral("sessionId"), QStringLiteral("dialog-session")},
                {QStringLiteral("sessionEpoch"), QStringLiteral("dialog-epoch")},
                {QStringLiteral("snapshotSha256"), QString(64, QLatin1Char('d'))}}},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("problemSpec"), QJsonObject{{QStringLiteral("schemaVersion"), 1}}}}},
            {QStringLiteral("diagnostics"), QJsonObject{{QStringLiteral("operation"), QStringLiteral("analysis.diagnose")},
                {QStringLiteral("schemaVersion"), 1}, {QStringLiteral("status"), QStringLiteral("warning")},
                {QStringLiteral("sourceId"), QStringLiteral("dialog-doc")},
                {QStringLiteral("modelRevision"), QStringLiteral("3")},
                {QStringLiteral("issues"), QJsonArray{QJsonObject{
                    {QStringLiteral("code"), QStringLiteral("ZERO_EXCITATION")},
                    {QStringLiteral("severity"), QStringLiteral("warning")},
                    {QStringLiteral("message"), QStringLiteral("No nonzero load was supplied.")},
                    {QStringLiteral("evidence"), QJsonObject{{QStringLiteral("loadCount"), 0}}}}}}}}};

        bool dialogFound = false;
        bool warningShown = false;
        bool okInitiallyDisabled = false;
        bool disabledClickStayedOpen = false;
        bool acknowledgementEnabledOk = false;
        bool dialogResolved = false;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != QStringLiteral("agentBoundedExperimentReview"))
            {
                if (dialog) dialog->reject();
                return;
            }
            dialogFound = true;
            const auto *details = dialog->findChild<QPlainTextEdit *>(QStringLiteral("agentBoundedExperimentReviewDetails"));
            auto *acknowledgement = dialog->findChild<QCheckBox *>(QStringLiteral("agentBoundedExperimentWarningAcknowledgement"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QPushButton *accept = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            warningShown = details && details->toPlainText().contains(QStringLiteral("No nonzero load was supplied."));
            if (!details || !acknowledgement || !accept)
            {
                dialogResolved = true;
                dialog->reject();
                return;
            }
            okInitiallyDisabled = !accept->isEnabled();
            accept->click();
            disabledClickStayedOpen = dialog->isVisible();
            acknowledgement->setChecked(true);
            acknowledgementEnabledOk = accept->isEnabled();
            accept->click();
            dialogResolved = true;
        });
        QTimer::singleShot(1500, [&] {
            if (!dialogResolved) {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (dialog && dialog->objectName() == QStringLiteral("agentBoundedExperimentReview"))
                dialog->reject();
            }
        });
        QString error;
        QVERIFY2(AgentWidget::confirmTrustedExperimentAuthorization(nullptr, candidate, &error), qPrintable(error));
        QVERIFY(dialogFound);
        QVERIFY(warningShown);
        QVERIFY(okInitiallyDisabled);
        QVERIFY(disabledClickStayedOpen);
        QVERIFY(acknowledgementEnabledOk);
    }

    void convergenceEvidenceRendersStructuredMeasurementsAndMissingValues()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        WorkflowProcess workflow;
        AgentWidget widget(application->getSession()->getModelCommandHost(), &workflow);
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(transcript);
        const QJsonObject change{{"status", "undefined"}, {"aggregateRelativeChange", QJsonValue::Null},
            {"fixedToleranceRelative", 0.01}, {"belowTolerance", false},
            {"qoiChanges", QJsonArray{QJsonObject{{"key", "tip|x"}, {"quantity", "displacement"},
                {"status", "undefined"}, {"relativeChange", QJsonValue::Null},
                {"absoluteChange", 0.125}, {"fineScale", 0.0}, {"belowTolerance", false}}}}};
        const QJsonObject quantity{{"quantity", "displacement"}, {"selector", QJsonObject{{"kind", "tip"}}},
            {"component", "x"}, {"coordinateM", QJsonArray{1.0, 0.0, 0.0}}, {"sampleCount", 11},
            {"peakAbsoluteM", 0.25}, {"rmsM", std::numeric_limits<double>::infinity()},
            {"firstValueM", 0.0}, {"lastValueM", 0.125}, {"firstTimeS", 0.0}, {"lastTimeS", 1.0}};
        const QJsonObject measured{{"level", 1}, {"verificationPassed", true},
            {"changeFromPrevious", change}, {"frequencyHz", 12.5}, {"mode", 1},
            {"quantities", QJsonArray{quantity}},
            {"referenceErrors", QJsonObject{{"tipReference", QJsonObject{{"status", "passed"},
                {"evidence", QJsonObject{{"relativeError", 0.002}, {"maximumAbsoluteErrorM", 0.0005}}}}}}}};
        const QJsonObject failed{{"level", 2}, {"frequencyHz", "NaN"},
            {"comparisonError", QJsonObject{{"code", "MEASUREMENT_QOI_MISSING"}, {"message", "No comparable QOI."}}}};
        QJsonArray levels{measured, failed, QJsonObject{{"level", 3}}, QJsonObject{{"level", 4}},
                          QJsonObject{{"level", 5}, {"frequencyHz", 987654.0}}};
        widget.taskRunner()->experimentObserved(QStringLiteral("fixture-task"), QStringLiteral("structured"),
            QJsonObject{{"experimentId", "structured"}, {"state", "completed"},
                {"convergence", QJsonObject{{"status", "not_converged"}, {"levels", levels}}}});
        const QString rendered = transcript->toPlainText();
        QVERIFY(rendered.contains(QStringLiteral("levels: not reported")));
        QVERIFY(rendered.contains(QStringLiteral("status=undefined")));
        QVERIFY(rendered.contains(QStringLiteral("aggregateRelativeChange=not reported")));
        QVERIFY(rendered.contains(QStringLiteral("relativeChange=not reported")));
        QVERIFY(rendered.contains(QStringLiteral("absoluteChange=0.125")));
        QVERIFY(rendered.contains(QStringLiteral("fineScale=0")));
        QVERIFY(rendered.contains(QStringLiteral("belowTolerance=false")));
        QVERIFY(rendered.contains(QStringLiteral("code=MEASUREMENT_QOI_MISSING")));
        QVERIFY(rendered.contains(QStringLiteral("message=No comparable QOI.")));
        QVERIFY(rendered.contains(QStringLiteral("frequencyHz=12.5")));
        QVERIFY(rendered.contains(QStringLiteral("frequencyHz=not reported")));
        QVERIFY(rendered.contains(QStringLiteral("coordinateM=1,0,0")));
        QVERIFY(rendered.contains(QStringLiteral("peakAbsoluteM=0.25")));
        QVERIFY(rendered.contains(QStringLiteral("rmsM=not reported")));
        QVERIFY(rendered.contains(QStringLiteral("lastTimeS=1")));
        QVERIFY(rendered.contains(QStringLiteral("Reference error tipReference: passed")));
        QVERIFY(rendered.contains(QStringLiteral("relativeError=0.002")));
        QVERIFY(rendered.contains(QStringLiteral("maximumAbsoluteErrorM=0.0005")));
        QVERIFY(rendered.contains(QStringLiteral("QOI: not reported")));
        QVERIFY(rendered.contains(QStringLiteral("Reference errors: not reported")));
        QVERIFY(!rendered.contains(QStringLiteral("Level 5")));
        QVERIFY(!rendered.contains(QStringLiteral("987654")));
    }

    void convergenceEvidenceKeepsUtf8DisplayWithin24KiB()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        WorkflowProcess workflow;
        AgentWidget widget(application->getSession()->getModelCommandHost(), &workflow);
        auto *transcript = widget.findChild<QTextBrowser *>(QStringLiteral("agentTranscript"));
        QVERIFY(transcript);
        // Leave less space than the cap marker after the first two lines.
        // This catches appending the marker after consuming the entire budget.
        const QString heading = QStringLiteral("Trusted Host evidence · experiment bounded");
        QString state(8000, QChar(0x6e2c));
        const qsizetype targetBytes = 24 * 1024 - 20;
        const qsizetype currentBytes = heading.toUtf8().size() + 1
            + QStringLiteral("Host state: ").toUtf8().size() + state.toUtf8().size() + 1;
        state += QString(targetBytes - currentBytes, QLatin1Char('x'));
        widget.taskRunner()->experimentObserved(QStringLiteral("fixture-task"), QStringLiteral("bounded"),
            QJsonObject{{"experimentId", "bounded"}, {"state", state},
                {"convergence", QJsonObject{{"levels", QJsonArray{QJsonObject{{"level", 1}}}}}}});
        const QString rendered = transcript->toPlainText();
        const qsizetype start = rendered.indexOf(QStringLiteral("Trusted Host evidence · experiment bounded"));
        QVERIFY(start >= 0);
        const QString evidence = rendered.mid(start).trimmed();
        QVERIFY(evidence.contains(QStringLiteral("[Host evidence display capped at 24 KiB;")));
        QVERIFY2(evidence.toUtf8().size() <= 24 * 1024, qPrintable(QString::number(evidence.toUtf8().size())));
    }

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

    void providerTelemetryJoinsAcceptedTurnAndKeepsUsageCompatibility()
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

        const QJsonObject nonStreamUsage{{QStringLiteral("prompt_tokens"), 11},
                                         {QStringLiteral("completion_tokens"), 4},
                                         {QStringLiteral("total_tokens"), 15}};
        NetworkScript stream;
        const QString streamModel = QStringLiteral("configured-model-v4");
        QJsonObject streamedText{{QStringLiteral("model"), streamModel},
            {QStringLiteral("choices"), QJsonArray{QJsonObject{
                {QStringLiteral("index"), 0},
                {QStringLiteral("delta"), QJsonObject{{QStringLiteral("content"), QStringLiteral("stream reply")},
                    {QStringLiteral("reasoning_content"), QStringLiteral("private-stream-reasoning")}}},
                {QStringLiteral("finish_reason"), QJsonValue(QJsonValue::Null)}}}}};
        QJsonObject streamedFinish = streamFinishChunk(QStringLiteral("stop"));
        streamedFinish.insert(QStringLiteral("model"), streamModel);
        const QJsonObject streamUsage{{QStringLiteral("prompt_tokens"), 9},
                                      {QStringLiteral("completion_tokens"), 2},
                                      {QStringLiteral("total_tokens"), 11}};
        const QJsonObject usageOnly{{QStringLiteral("model"), streamModel},
            {QStringLiteral("choices"), QJsonArray{}}, {QStringLiteral("usage"), streamUsage}};
        stream.chunks << streamEvent(streamedText) << streamEvent(streamedFinish)
                      << streamEvent(usageOnly) << streamDone();

        FixtureNetworkAccessManager network;
        network.enqueue(completeJsonScript(QStringLiteral("non-stream reply"),
            QStringLiteral("reported-json-model"), nonStreamUsage));
        network.enqueue(std::move(stream));
        network.enqueue(completeJsonScript(QStringLiteral("model identity unavailable")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("telemetry-key-secret-sentinel"));
        const QString configuredModel = QStringLiteral("configured-model-v4");
        const QString configuredModelSha256 = QString::fromLatin1(
            QCryptographicHash::hash(configuredModel.toUtf8(), QCryptographicHash::Sha256).toHex());
        settings.setModel(configuredModel);
        widget.setProviderSettings(settings);
        auto *streaming = widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(streaming);
        streaming->setChecked(false);

        QSignalSpy telemetry(&widget, &AgentWidget::providerTurnTelemetry);
        QSignalSpy legacyUsage(&widget, &AgentWidget::providerUsage);
        QSignalSpy requests(widget.taskRunner(), &AgentTaskRunner::turnRequested);
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        QVERIFY(telemetry.isValid());
        QVERIFY(legacyUsage.isValid());

        widget.startTask(QStringLiteral("request-body-secret-sentinel"));
        const QString firstTaskId = widget.taskRunner()->taskId();
        const QString conversationId = widget.conversationId();
        QVERIFY(!firstTaskId.isEmpty());
        QVERIFY(!conversationId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(telemetry.size(), 1, 1000);
        const QJsonObject first = telemetry.first().first().toJsonObject();
        QCOMPARE(first.value(QStringLiteral("requestId")).toString(), requests.first().first().toString());
        QCOMPARE(first.value(QStringLiteral("taskId")).toString(), firstTaskId);
        QCOMPARE(first.value(QStringLiteral("conversationId")).toString(), conversationId);
        QCOMPARE(first.value(QStringLiteral("provider")).toString(), QStringLiteral("DeepSeek"));
        QCOMPARE(first.value(QStringLiteral("configuredModelSha256")).toString(), configuredModelSha256);
        QCOMPARE(first.value(QStringLiteral("providerModelStatus")).toString(), QStringLiteral("reported"));
        QCOMPARE(first.value(QStringLiteral("providerModelSha256")).toString(), QString::fromLatin1(
            QCryptographicHash::hash(QByteArrayLiteral("reported-json-model"), QCryptographicHash::Sha256).toHex()));
        QVERIFY(!first.contains(QStringLiteral("providerModel")));
        QVERIFY(!QJsonDocument(first).toJson(QJsonDocument::Compact).contains("reported-json-model"));
        QVERIFY(!QJsonDocument(first).toJson(QJsonDocument::Compact).contains(configuredModel.toUtf8()));
        const QJsonObject firstUsage = first.value(QStringLiteral("usage")).toObject();
        QCOMPARE(firstUsage.value(QStringLiteral("promptTokens")).toInt(), 11);
        QCOMPARE(firstUsage.value(QStringLiteral("completionTokens")).toInt(), 4);
        QCOMPARE(firstUsage.value(QStringLiteral("totalTokens")).toInt(), 15);

        streaming->setChecked(true);
        widget.startTask(QStringLiteral("streamed request"));
        const QString secondTaskId = widget.taskRunner()->taskId();
        QVERIFY(secondTaskId != firstTaskId);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 2000);
        QCOMPARE(finished.at(1).at(0).toString(), QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(telemetry.size(), 2, 1000);
        const QJsonObject second = telemetry.at(1).first().toJsonObject();
        QCOMPARE(second.value(QStringLiteral("requestId")).toString(), requests.at(1).first().toString());
        QCOMPARE(second.value(QStringLiteral("taskId")).toString(), secondTaskId);
        QCOMPARE(second.value(QStringLiteral("conversationId")).toString(), conversationId);
        QCOMPARE(second.value(QStringLiteral("configuredModelSha256")).toString(), configuredModelSha256);
        QCOMPARE(second.value(QStringLiteral("providerModelStatus")).toString(), QStringLiteral("reported"));
        QCOMPARE(second.value(QStringLiteral("providerModelSha256")).toString(), QString::fromLatin1(
            QCryptographicHash::hash(streamModel.toUtf8(), QCryptographicHash::Sha256).toHex()));
        QVERIFY(!second.contains(QStringLiteral("providerModel")));
        QVERIFY(!QJsonDocument(second).toJson(QJsonDocument::Compact).contains(streamModel.toUtf8()));
        QVERIFY(!QJsonDocument(second).toJson(QJsonDocument::Compact).contains(configuredModel.toUtf8()));
        const QJsonObject secondUsage = second.value(QStringLiteral("usage")).toObject();
        QCOMPARE(secondUsage.value(QStringLiteral("promptTokens")).toInt(), 9);
        QCOMPARE(secondUsage.value(QStringLiteral("completionTokens")).toInt(), 2);
        QCOMPARE(secondUsage.value(QStringLiteral("totalTokens")).toInt(), 11);

        streaming->setChecked(false);
        widget.startTask(QStringLiteral("response without model metadata"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 3, 2000);
        QCOMPARE(finished.at(2).at(0).toString(), QStringLiteral("completed"));
        QTRY_COMPARE_WITH_TIMEOUT(telemetry.size(), 3, 1000);
        const QJsonObject third = telemetry.at(2).first().toJsonObject();
        QCOMPARE(third.value(QStringLiteral("providerModelStatus")).toString(), QStringLiteral("unknown"));
        QCOMPARE(third.value(QStringLiteral("configuredModelSha256")).toString(), configuredModelSha256);
        QVERIFY(!third.contains(QStringLiteral("providerModel")));
        QVERIFY(!third.contains(QStringLiteral("providerModelSha256")));
        QCOMPARE(third.value(QStringLiteral("requestId")).toString(), requests.at(2).first().toString());
        QCOMPARE(third.value(QStringLiteral("conversationId")).toString(), conversationId);

        QCOMPARE(legacyUsage.size(), 3);
        QCOMPARE(legacyUsage.first().first().toJsonObject(), firstUsage);
        QCOMPARE(legacyUsage.at(1).first().toJsonObject(), secondUsage);
        for (const QJsonObject &event : {first, second, third})
        {
            QSet<QString> expectedKeys{QStringLiteral("schema"), QStringLiteral("requestId"),
                QStringLiteral("taskId"), QStringLiteral("conversationId"), QStringLiteral("provider"),
                QStringLiteral("configuredModelSha256"), QStringLiteral("providerModelStatus"),
                QStringLiteral("usage")};
            if (event.value(QStringLiteral("providerModelStatus")) == QJsonValue(QStringLiteral("reported")))
                expectedKeys.insert(QStringLiteral("providerModelSha256"));
            QSet<QString> actualKeys;
            for (const QString &key : event.keys()) actualKeys.insert(key);
            QCOMPARE(actualKeys, expectedKeys);
            QCOMPARE(event.value(QStringLiteral("schema")).toString(),
                     QStringLiteral("range.provider-turn-telemetry/v2"));
            QVERIFY(QJsonDocument(event).toJson(QJsonDocument::Compact).size() <= 4096);
            for (const QString &forbidden : {QStringLiteral("endpoint"), QStringLiteral("apiKey"),
                                             QStringLiteral("requestBody"), QStringLiteral("reasoning"),
                                             QStringLiteral("reasoning_content")})
                QVERIFY(!event.contains(forbidden));
            const QByteArray encoded = QJsonDocument(event).toJson(QJsonDocument::Compact);
            QVERIFY(!encoded.contains(configuredModel.toUtf8()));
            QVERIFY(!encoded.contains("telemetry-key-secret-sentinel"));
            QVERIFY(!encoded.contains("private-stream-reasoning"));
            QVERIFY(!encoded.contains("request-body-secret-sentinel"));
        }

        session->clear();
    }

    void providerCaptureRecordsActualRequestsResponsesAndToolResult()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        PreferenceSnapshot restore(application->getApplicationSettings());
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RAgentEvaluationCaptureStore store;
        QString storeError;
        QVERIFY2(store.configure(temporary.filePath(QStringLiteral("capture.jsonl")), &storeError), qPrintable(storeError));

        const QJsonObject usage{{QStringLiteral("prompt_tokens"), 23},
                                {QStringLiteral("completion_tokens"), 5},
                                {QStringLiteral("total_tokens"), 28},
                                {QStringLiteral("prompt_cache_hit_tokens"), 2},
                                {QStringLiteral("debug"), QStringLiteral("capture-provider-metadata-secret-sentinel")},
                                {QStringLiteral("fractional_tokens"), 1.5},
                                {QStringLiteral("oversized_tokens"), 9007199254740992.0}};
        const QJsonObject rawToolArguments{{QStringLiteral("jobId"), QStringLiteral("capture-free-text-secret-sentinel")}};
        QJsonObject assistant = toolCall(QStringLiteral("capture-provider-tool-secret-sentinel"),
            QStringLiteral("native_job"),
            rawToolArguments).value(QStringLiteral("assistantMessage")).toObject();
        assistant.insert(QStringLiteral("reasoning_content"), QStringLiteral("capture-reasoning-secret-sentinel"));
        const QJsonObject providerResponse{{QStringLiteral("id"), QStringLiteral("capture-provider-id-secret-sentinel")},
            {QStringLiteral("model"), QStringLiteral("capture-provider-model-secret-sentinel")},
            {QStringLiteral("usage"), usage},
            {QStringLiteral("choices"), QJsonArray{QJsonObject{
                {QStringLiteral("finish_reason"), QStringLiteral("tool_calls")},
                {QStringLiteral("message"), assistant}}}}};
        NetworkScript toolResponse;
        toolResponse.chunks << QJsonDocument(providerResponse).toJson(QJsonDocument::Compact);

        FixtureNetworkAccessManager network;
        network.enqueue(std::move(toolResponse));
        NetworkScript finalResponse = completeJsonScript(QStringLiteral("capture flow completed"),
                                                         QStringLiteral("configured-capture-model"));
        QJsonObject finalPayload = QJsonDocument::fromJson(finalResponse.chunks.first()).object();
        finalPayload.insert(QStringLiteral("id"), QString(1024, QLatin1Char('x')));
        finalResponse.chunks = {QJsonDocument(finalPayload).toJson(QJsonDocument::Compact)};
        network.enqueue(std::move(finalResponse));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("capture-api-key-secret-sentinel"));
        settings.setModel(QStringLiteral("configured-capture-model"));
        widget.setProviderSettings(settings);
        auto *streaming = widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(streaming);
        streaming->setChecked(false);

        QList<QJsonObject> events;
        int intentCount = 0;
        bool intentsPrecededDispatch = true;
        bool terminalReconfigurationWasRejected = false;
        QVERIFY(widget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            if (event.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("task_terminal")))
                terminalReconfigurationWasRejected = !widget.setProviderCaptureRecorder(
                    AgentWidget::ProviderCaptureRecorder{});
            if (event.value(QStringLiteral("kind")) == QJsonValue(QStringLiteral("dispatch_intent")))
            {
                intentsPrecededDispatch = intentsPrecededDispatch && network.requests.size() == intentCount;
                ++intentCount;
            }
            events.append(event);
            return store.append(event, &storeError);
        }));
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        QVERIFY(finished.isValid());
        widget.startTask(QStringLiteral("capture-user-prompt-secret-sentinel"));
        QVERIFY(!widget.setProviderCaptureRecorder([](const QJsonObject &) { return true; }));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QVERIFY2(finished.first().at(0).toString() == QStringLiteral("completed"),
                 qPrintable(finished.first().at(1).toString()));
        QCOMPARE(network.requests.size(), 2);
        QVERIFY(intentsPrecededDispatch);
        QVERIFY(terminalReconfigurationWasRejected);
        QCOMPARE(events.size(), 6);
        const QStringList expectedKinds{QStringLiteral("dispatch_intent"), QStringLiteral("provider_response"),
            QStringLiteral("tool_result"), QStringLiteral("dispatch_intent"),
            QStringLiteral("provider_response"), QStringLiteral("task_terminal")};
        for (qsizetype index = 0; index < events.size(); ++index)
        {
            QCOMPARE(events.at(index).value(QStringLiteral("kind")).toString(), expectedKinds.at(index));
            QCOMPARE(events.at(index).value(QStringLiteral("evaluationArm")).toString(),
                     QStringLiteral("standard"));
        }

        for (qsizetype index = 0; index < 2; ++index)
        {
            const QString bodyHash = QString::fromLatin1(QCryptographicHash::hash(
                network.requestBodies.at(index), QCryptographicHash::Sha256).toHex());
            QCOMPARE(events.at(index == 0 ? 0 : 3).value(QStringLiteral("requestSha256")).toString(), bodyHash);
            const QJsonObject request = QJsonDocument::fromJson(network.requestBodies.at(index)).object();
            QCOMPARE(request.value(QStringLiteral("stream")).toBool(), false);
        }
        QCOMPARE(events.first().value(QStringLiteral("destinationScheme")).toString(), QStringLiteral("https"));
        QCOMPARE(events.first().value(QStringLiteral("destinationHost")).toString(), QStringLiteral("fixture.invalid"));
        QCOMPARE(events.first().value(QStringLiteral("destinationPort")).toInt(), 443);
        QCOMPARE(events.first().value(QStringLiteral("endpointPathSha256")).toString(),
            QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("/v1/chat/completions"),
                                                         QCryptographicHash::Sha256).toHex()));
        const QJsonObject capturedResponse = events.at(1);
        QVERIFY(!capturedResponse.contains(QStringLiteral("providerResponseId")));
        QVERIFY(!capturedResponse.contains(QStringLiteral("returnedModel")));
        QCOMPARE(capturedResponse.value(QStringLiteral("providerResponseIdStatus")).toString(), QStringLiteral("present"));
        QCOMPARE(capturedResponse.value(QStringLiteral("returnedModelStatus")).toString(), QStringLiteral("different"));
        QCOMPARE(capturedResponse.value(QStringLiteral("returnedModelSha256")).toString(),
            QString::fromLatin1(QCryptographicHash::hash(
                QByteArrayLiteral("capture-provider-model-secret-sentinel"), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(capturedResponse.value(QStringLiteral("finishReason")).toString(), QStringLiteral("tool_calls"));
        QCOMPARE(events.at(4).value(QStringLiteral("providerResponseIdStatus")).toString(), QStringLiteral("invalid"));
        QCOMPARE(events.at(4).value(QStringLiteral("returnedModelStatus")).toString(), QStringLiteral("matches_configured"));
        QCOMPARE(events.at(4).value(QStringLiteral("returnedModelSha256")).toString(),
            QString::fromLatin1(QCryptographicHash::hash(
                QByteArrayLiteral("configured-capture-model"), QCryptographicHash::Sha256).toHex()));
        const QJsonObject expectedCapturedUsage{
            {QStringLiteral("prompt_tokens"), 23}, {QStringLiteral("completion_tokens"), 5},
            {QStringLiteral("total_tokens"), 28}, {QStringLiteral("prompt_cache_hit_tokens"), 2}};
        QCOMPARE(capturedResponse.value(QStringLiteral("usage")).toObject(), expectedCapturedUsage);
        const QJsonObject capturedCall = capturedResponse.value(QStringLiteral("toolCalls")).toArray().first().toObject();
        QCOMPARE(capturedCall.value(QStringLiteral("toolCallId")).toString(), QStringLiteral("tool-1"));
        QCOMPARE(events.at(2).value(QStringLiteral("toolCallId")).toString(), QStringLiteral("tool-1"));
        QCOMPARE(capturedCall.value(QStringLiteral("argumentsSha256")).toString(),
            QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(rawToolArguments).toJson(QJsonDocument::Compact),
                                                         QCryptographicHash::Sha256).toHex()));
        QVERIFY(!capturedCall.contains(QStringLiteral("argumentsJson")));

        const QJsonArray secondMessages = QJsonDocument::fromJson(network.requestBodies.at(1)).object()
            .value(QStringLiteral("messages")).toArray();
        QJsonObject toolMessage;
        for (const QJsonValue &value : secondMessages)
            if (value.toObject().value(QStringLiteral("role")) == QJsonValue(QStringLiteral("tool")))
                toolMessage = value.toObject();
        QVERIFY(!toolMessage.isEmpty());
        QCOMPARE(events.at(2).value(QStringLiteral("contentSha256")).toString(),
            QString::fromLatin1(QCryptographicHash::hash(toolMessage.value(QStringLiteral("content")).toString().toUtf8(),
                                                         QCryptographicHash::Sha256).toHex()));
        QCOMPARE(events.last().value(QStringLiteral("status")).toString(), QStringLiteral("completed"));

        QFile ledger(temporary.filePath(QStringLiteral("capture.jsonl")));
        QVERIFY(ledger.open(QIODevice::ReadOnly));
        const QByteArray persisted = ledger.readAll();
        for (const QByteArray &secret : {QByteArrayLiteral("capture-api-key-secret-sentinel"),
                                         QByteArrayLiteral("capture-user-prompt-secret-sentinel"),
                                         QByteArrayLiteral("capture-reasoning-secret-sentinel"),
                                         QByteArrayLiteral("capture-free-text-secret-sentinel"),
                                         QByteArrayLiteral("capture-provider-metadata-secret-sentinel"),
                                         QByteArrayLiteral("capture-provider-id-secret-sentinel"),
                                         QByteArrayLiteral("capture-provider-model-secret-sentinel"),
                                         QByteArrayLiteral("capture-provider-tool-secret-sentinel"),
                                         QByteArrayLiteral("capture-provider-finish-secret-sentinel")})
            QVERIFY2(!persisted.contains(secret), secret.constData());
        QVERIFY(persisted.contains("tool-1"));
        QVERIFY(!persisted.contains(QByteArray(129, 'x')));
        // Explicit test-only export supports cross-runtime evaluator checks.
        // Never overwrite an earlier evidence receipt.
        const QString exportPath = qEnvironmentVariable("RANGE_TEST_CAPTURE_EXPORT");
        if (!exportPath.isEmpty())
        {
            QFile exported(exportPath);
            QVERIFY2(exported.open(QIODevice::WriteOnly | QIODevice::NewOnly), qPrintable(exported.errorString()));
            QCOMPARE(exported.write(persisted), qint64(persisted.size()));
            QVERIFY(exported.flush());
        }
        session->clear();
    }

    void evaluationArmIsPinnedInCaptureContextWithoutExposingPrompt()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RAgentEvaluationCaptureStore store;
        QString storeError;
        QVERIFY2(store.configure(temporary.filePath(QStringLiteral("capture.jsonl")), &storeError),
                 qPrintable(storeError));
        FixtureNetworkAccessManager network;
        network.enqueue(completeJsonScript(QStringLiteral("fixture completed"),
                                           QStringLiteral("configured-evaluation-model")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
            AgentTaskRunner::AccessPolicy::ReadWrite, false,
            AgentTaskRunner::EvaluationArm::DirectNl);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("offline-fixture-placeholder"));
        settings.setModel(QStringLiteral("configured-evaluation-model"));
        widget.setProviderSettings(settings);
        auto *streaming = widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(streaming);
        streaming->setChecked(false);

        QList<QJsonObject> events;
        QVERIFY(widget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            events.append(event);
            return store.append(event, &storeError);
        }));
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        widget.startTask(QStringLiteral("private direct-nl fixture prompt"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(network.requests.size(), 1);
        QVERIFY(QJsonDocument::fromJson(network.requestBodies.first()).object()
                    .value(QStringLiteral("tools")).toArray().isEmpty());
        QCOMPARE(events.size(), 3);
        for (const auto &event : events)
        {
            QCOMPARE(event.value(QStringLiteral("evaluationArm")).toString(), QStringLiteral("direct_nl"));
            QVERIFY(!event.contains(QStringLiteral("prompt")));
            QVERIFY(!event.contains(QStringLiteral("messages")));
        }
        session->clear();
    }

    void evaluationSlotBindingConfigurationIsAllOrNoneAndStrict()
    {
        AgentEvaluationSlotBinding binding;
        QString error;
        QVERIFY2(Application::parseEvaluationSlotBinding(
            false, QString(), false, QString(), false, QString(), false, QString(), false, QString(),
            false, QString(), false, QString(), AgentTaskRunner::EvaluationArm::DirectNl,
            &binding, &error), qPrintable(error));
        QVERIFY(!binding.enabled);

        const QString manifestDigest(64, QLatin1Char('a'));
        const QString promptDigest(64, QLatin1Char('b'));
        const auto parseAll = [&](const QString &slotId, const QString &caseId,
                                  const QString &repeat, const QString &manifest,
                                  const QString &prompt, AgentTaskRunner::EvaluationArm arm) {
            return Application::parseEvaluationSlotBinding(
                true, QStringLiteral("run-001"), true, QStringLiteral("holdout-2026"),
                true, slotId, true, caseId, true, repeat, true, manifest, true, prompt,
                arm, &binding, &error);
        };
        QVERIFY2(parseAll(QStringLiteral("direct_nl:holdout-2026:case-01:r2"),
                          QStringLiteral("case-01"), QStringLiteral("2"), manifestDigest,
                          promptDigest, AgentTaskRunner::EvaluationArm::DirectNl), qPrintable(error));
        QVERIFY(binding.enabled);
        QCOMPARE(binding.runId, QStringLiteral("run-001"));
        QCOMPARE(binding.repeat, 2);

        QVERIFY(!Application::parseEvaluationSlotBinding(
            true, QStringLiteral("run-001"), true, QStringLiteral("holdout-2026"),
            false, QString(), true, QStringLiteral("case-01"), true, QStringLiteral("2"),
            true, manifestDigest, true, promptDigest,
            AgentTaskRunner::EvaluationArm::DirectNl, &binding, &error));
        QVERIFY(!binding.enabled);
        QVERIFY(!parseAll(QStringLiteral("tools_feedback:holdout-2026:case-01:r2"),
                          QStringLiteral("case-01"), QStringLiteral("2"), manifestDigest,
                          promptDigest, AgentTaskRunner::EvaluationArm::DirectNl));
        QVERIFY(!parseAll(QStringLiteral("direct_nl:holdout-2026:case-01:r01"),
                          QStringLiteral("case-01"), QStringLiteral("01"), manifestDigest,
                          promptDigest, AgentTaskRunner::EvaluationArm::DirectNl));
        QVERIFY(!parseAll(QStringLiteral("direct_nl:holdout-2026:case-01:r2"),
                          QStringLiteral("case-01"), QStringLiteral("2"), manifestDigest.toUpper(),
                          promptDigest, AgentTaskRunner::EvaluationArm::DirectNl));
        QVERIFY(!parseAll(QStringLiteral("direct_nl:holdout-2026:case-01:r2"),
                          QStringLiteral("case-01"), QStringLiteral("2"), manifestDigest,
                          promptDigest, AgentTaskRunner::EvaluationArm::Standard));
    }

    void applicationRejectsSelfConsistentUnsignedSlotBeforeCaptureSetup()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        const QStringList names{
            QStringLiteral("RANGE_AGENT_EVALUATION_ARM"),
            QStringLiteral("RANGE_AGENT_EVALUATION_CAPTURE"),
            QStringLiteral("RANGE_AGENT_EVALUATION_RUN_ID"),
            QStringLiteral("RANGE_AGENT_EVALUATION_HOLDOUT_ID"),
            QStringLiteral("RANGE_AGENT_EVALUATION_SLOT_ID"),
            QStringLiteral("RANGE_AGENT_EVALUATION_CASE_ID"),
            QStringLiteral("RANGE_AGENT_EVALUATION_REPEAT"),
            QStringLiteral("RANGE_AGENT_EVALUATION_MANIFEST_SHA256"),
            QStringLiteral("RANGE_AGENT_EVALUATION_PROMPT_SHA256"),
            QStringLiteral("RANGE_AGENT_EVALUATION_MANIFEST_PATH"),
            QStringLiteral("RANGE_AGENT_EVALUATION_PUBLIC_CASES_PATH"),
            QStringLiteral("RANGE_AGENT_EVALUATION_PUBLIC_KEY_PATH"),
            QStringLiteral("RANGE_AGENT_EVALUATION_PUBLIC_KEY_SHA256"),
            QStringLiteral("RANGE_AGENT_EVALUATION_EXTERNAL_PRE_RUN_COMMITMENT_SHA256")};
        QHash<QString, QByteArray> savedValues;
        QSet<QString> previouslySet;
        for (const QString &name : names)
        {
            if (qEnvironmentVariableIsSet(name.toUtf8().constData()))
            {
                previouslySet.insert(name);
                savedValues.insert(name, qgetenv(name.toUtf8().constData()));
            }
        }
        const auto restoreEnvironment = qScopeGuard([&] {
            for (const QString &name : names)
            {
                const QByteArray encodedName = name.toUtf8();
                if (previouslySet.contains(name)) qputenv(encodedName.constData(), savedValues.value(name));
                else qunsetenv(encodedName.constData());
            }
        });
        const auto set = [](const QString &name, const QString &value) {
            return qputenv(name.toUtf8().constData(), value.toUtf8());
        };

        const QString manifestPath = temporary.filePath(QStringLiteral("manifest.json"));
        const QString casesPath = temporary.filePath(QStringLiteral("public-cases.json"));
        const QString keyPath = temporary.filePath(QStringLiteral("public-key.json"));
        for (const QString &path : {manifestPath, casesPath, keyPath})
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("{}"), qint64(2));
        }
        const QString capturePath = temporary.filePath(QStringLiteral("capture.jsonl"));
        const QString manifestDigest(64, QLatin1Char('a'));
        const QString promptDigest(64, QLatin1Char('b'));
        const QList<QPair<QString, QString>> values{
            {names[0], QStringLiteral("direct_nl")},
            {names[1], capturePath},
            {names[2], QStringLiteral("run-001")},
            {names[3], QStringLiteral("holdout-2026")},
            {names[4], QStringLiteral("direct_nl:holdout-2026:case-01:r1")},
            {names[5], QStringLiteral("case-01")},
            {names[6], QStringLiteral("1")},
            {names[7], manifestDigest},
            {names[8], promptDigest},
            {names[9], manifestPath},
            {names[10], casesPath},
            {names[11], keyPath},
            {names[12], QString(64, QLatin1Char('c'))},
            {names[13], manifestDigest}};
        for (const auto &value : values) QVERIFY(set(value.first, value.second));

        QString error;
        QVERIFY(!application->configureEvaluationCaptureFromEnvironment(&error));
        QVERIFY2(error.contains(QStringLiteral("Signed Stage 7 public-pack verification failed")),
                 qPrintable(error));
        QVERIFY(!QFile::exists(capturePath));
    }

    void evaluationSlotBindingPrecedesDispatchAndMismatchStopsNetwork()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        PreferenceSnapshot restore(application->getApplicationSettings());
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        const QString prompt = QStringLiteral("An exact public Stage 7 case prompt.");
        const QString promptDigest = QString::fromLatin1(QCryptographicHash::hash(
            prompt.toUtf8(), QCryptographicHash::Sha256).toHex());
        AgentEvaluationSlotBinding binding;
        binding.enabled = true;
        binding.runId = QStringLiteral("run-001");
        binding.holdoutId = QStringLiteral("holdout-2026");
        binding.slotId = QStringLiteral("direct_nl:holdout-2026:case-01:r1");
        binding.caseId = QStringLiteral("case-01");
        binding.repeat = 1;
        binding.manifestCommitmentSha256 = QString(64, QLatin1Char('a'));
        binding.promptSha256 = promptDigest;

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        RAgentEvaluationCaptureStore store;
        QString storeError;
        QVERIFY2(store.configure(temporary.filePath(QStringLiteral("slot-capture.jsonl")), &storeError),
                 qPrintable(storeError));
        FixtureNetworkAccessManager network;
        network.enqueue(completeJsonScript(QStringLiteral("slot fixture complete"),
                                           QStringLiteral("configured-slot-model")));
        WorkflowProcess workflow;
        AgentWidget widget(host, &workflow, nullptr, &network,
            AgentTaskRunner::AccessPolicy::ReadWrite, false,
            AgentTaskRunner::EvaluationArm::DirectNl, binding);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("slot-capture-api-key-secret"));
        settings.setModel(QStringLiteral("configured-slot-model"));
        widget.setProviderSettings(settings);
        auto *streaming = widget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(streaming);
        streaming->setChecked(false);
        QList<QJsonObject> events;
        QVERIFY(widget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            events.append(event);
            return store.append(event, &storeError);
        }));
        QSignalSpy finished(widget.taskRunner(), &AgentTaskRunner::finished);
        QVERIFY(finished.isValid());
        widget.startTask(prompt);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("completed"));
        QCOMPARE(network.requests.size(), 1);
        QCOMPARE(events.size(), 4);
        QCOMPARE(events.at(0).value(QStringLiteral("kind")).toString(), QStringLiteral("slot_binding"));
        QCOMPARE(events.at(1).value(QStringLiteral("kind")).toString(), QStringLiteral("dispatch_intent"));
        QCOMPARE(events.at(0).value(QStringLiteral("requestId")).toString(),
                 events.at(1).value(QStringLiteral("requestId")).toString());
        QCOMPARE(events.at(0).value(QStringLiteral("slotId")).toString(), binding.slotId);
        QCOMPARE(events.at(0).value(QStringLiteral("promptSha256")).toString(), promptDigest);
        QCOMPARE(events.at(0).value(QStringLiteral("manifestCommitmentSha256")).toString(),
                 binding.manifestCommitmentSha256);
        QVERIFY(!events.at(0).contains(QStringLiteral("prompt")));
        const QJsonObject firstRequest = QJsonDocument::fromJson(network.requestBodies.first()).object();
        const QJsonArray requestMessages = firstRequest.value(QStringLiteral("messages")).toArray();
        QVERIFY(std::any_of(requestMessages.cbegin(), requestMessages.cend(), [&](const QJsonValue &value) {
            const QJsonObject message = value.toObject();
            return message.value(QStringLiteral("role")).toString() == QStringLiteral("user")
                && message.value(QStringLiteral("content")).toString() == prompt;
        }));
        const QString requestDigest = QString::fromLatin1(QCryptographicHash::hash(
            network.requestBodies.first(), QCryptographicHash::Sha256).toHex());
        QCOMPARE(events.at(1).value(QStringLiteral("requestSha256")).toString(), requestDigest);
        QVERIFY(requestDigest != promptDigest);

        AgentEvaluationSlotBinding mismatchBinding = binding;
        mismatchBinding.promptSha256 = QString(64, QLatin1Char('c'));
        FixtureNetworkAccessManager rejectedNetwork;
        rejectedNetwork.enqueue(completeJsonScript(QStringLiteral("must not be sent")));
        WorkflowProcess rejectedWorkflow;
        AgentWidget rejectedWidget(host, &rejectedWorkflow, nullptr, &rejectedNetwork,
            AgentTaskRunner::AccessPolicy::ReadWrite, false,
            AgentTaskRunner::EvaluationArm::DirectNl, mismatchBinding);
        rejectedWidget.setProviderSettings(settings);
        auto *rejectedStreaming = rejectedWidget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(rejectedStreaming);
        rejectedStreaming->setChecked(false);
        QList<QJsonObject> rejectedEvents;
        QVERIFY(rejectedWidget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            rejectedEvents.append(event);
            return true;
        }));
        QSignalSpy rejectedFinished(rejectedWidget.taskRunner(), &AgentTaskRunner::finished);
        rejectedWidget.startTask(prompt);
        QTRY_COMPARE_WITH_TIMEOUT(rejectedFinished.size(), 1, 3000);
        QCOMPARE(rejectedFinished.first().at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(rejectedNetwork.requests.size(), 0);
        QVERIFY(!rejectedEvents.isEmpty());
        QCOMPARE(rejectedEvents.first().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("slot_binding_rejected"));
        QVERIFY(std::none_of(rejectedEvents.cbegin(), rejectedEvents.cend(), [](const QJsonObject &event) {
            return event.value(QStringLiteral("kind")).toString() == QStringLiteral("dispatch_intent");
        }));
        session->clear();
    }

    void evaluationSlotCaptureStartsWithCleanStoredConversation()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        auto *applicationSettings = application->getApplicationSettings();
        QVERIFY(applicationSettings);
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        const QJsonObject projectResponse = host->handle({
            {QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
            {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("operation"), QStringLiteral("project.query")},
            {QStringLiteral("parameters"), QJsonObject{}}});
        QCOMPARE(projectResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        const QString projectId = projectResponse.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("projectId")).toString();
        QVERIFY(!projectId.isEmpty());

        RAgentConversationStore conversationStore;
        QString storeError;
        QVERIFY2(conversationStore.configure(host->projectWorkspace(), projectId, &storeError),
                 qPrintable(storeError));
        ConversationCleanup conversationCleanup(&conversationStore);
        const QJsonObject previous = conversationStore.create(QStringLiteral("Previous stored conversation"),
                                                               &storeError);
        QVERIFY2(!previous.isEmpty(), qPrintable(storeError));
        const QString previousConversationId = previous.value(QStringLiteral("conversationId")).toString();
        QVERIFY(!previousConversationId.isEmpty());
        conversationCleanup.track(previousConversationId);

        const QString previousSentinel = QStringLiteral("stored-conversation-sentinel-7e911d");
        const QJsonArray previousMessages{
            QJsonObject{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("text"), QStringLiteral("Earlier user prompt: ") + previousSentinel}},
            QJsonObject{{QStringLiteral("messageId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("role"), QStringLiteral("assistant")},
                        {QStringLiteral("text"), QStringLiteral("Earlier assistant reply: ") + previousSentinel}}};
        const QJsonObject appended = conversationStore.append(previousConversationId,
            previous.value(QStringLiteral("revision")).toString(), previousMessages, &storeError);
        QVERIFY2(!appended.isEmpty(), qPrintable(storeError));

        const QString lastConversationKey = QStringLiteral("aiAgent/lastConversation/")
            + projectId + QStringLiteral("/primary");
        PreferenceSnapshot restore(applicationSettings,
            {lastConversationKey, QStringLiteral("mainWindow/featureLayoutVersion")});
        applicationSettings->setValue(lastConversationKey, previousConversationId);
        const QJsonObject seeded = conversationStore.page(previousConversationId, 0, 10, &storeError);
        QCOMPARE(seeded.value(QStringLiteral("messages")).toArray().size(), 2);
        QVERIFY(seeded.value(QStringLiteral("messages")).toArray().first().toObject()
            .value(QStringLiteral("text")).toString().contains(previousSentinel));
        QVERIFY(seeded.value(QStringLiteral("messages")).toArray().last().toObject()
            .value(QStringLiteral("text")).toString().contains(previousSentinel));

        const QString prompt = QStringLiteral("Use only this exact Stage 7 slot prompt.");
        AgentEvaluationSlotBinding binding;
        binding.enabled = true;
        binding.runId = QStringLiteral("run-clean-start-001");
        binding.holdoutId = QStringLiteral("holdout-clean-start");
        binding.slotId = QStringLiteral("direct_nl:holdout-clean-start:case-01:r1");
        binding.caseId = QStringLiteral("case-01");
        binding.repeat = 1;
        binding.manifestCommitmentSha256 = QString(64, QLatin1Char('a'));
        binding.promptSha256 = QString::fromLatin1(QCryptographicHash::hash(
            prompt.toUtf8(), QCryptographicHash::Sha256).toHex());

        MainWindow restoredWindow(session, nullptr, applicationSettings, application->getActionList());
        AgentWidget *restoredWidget = restoredWindow.findChild<AgentWidget *>(QStringLiteral("agentTaskWidget"));
        QVERIFY(restoredWidget);
        QCOMPARE(restoredWidget->conversationId(), previousConversationId);

        MainWindow window(session, nullptr, applicationSettings, application->getActionList(), nullptr,
                          AgentTaskRunner::EvaluationArm::DirectNl, {}, binding);
        AgentWidget *widget = window.findChild<AgentWidget *>(QStringLiteral("agentTaskWidget"));
        QVERIFY(widget);
        QVERIFY(widget->conversationId().isEmpty());
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("local-slot-capture-test-key"));
        settings.setModel(QStringLiteral("local-slot-capture-test-model"));
        widget->setProviderSettings(settings);
        auto *streaming = widget->findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(streaming);
        streaming->setChecked(false);
        QList<QJsonObject> captureEvents;
        bool cancelledAtDispatchIntent = false;
        QVERIFY(widget->setProviderCaptureRecorder([&](const QJsonObject &event) {
            captureEvents.append(event);
            if (event.value(QStringLiteral("kind")).toString() == QStringLiteral("dispatch_intent")
                && !cancelledAtDispatchIntent)
            {
                cancelledAtDispatchIntent = true;
                widget->taskRunner()->cancel();
            }
            return true;
        }));
        QSignalSpy requests(widget->taskRunner(), &AgentTaskRunner::turnRequested);
        QVERIFY(requests.isValid());
        QSignalSpy finished(widget->taskRunner(), &AgentTaskRunner::finished);
        QVERIFY(finished.isValid());

        widget->startTask(prompt);
        conversationCleanup.track(widget->conversationId());
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("cancelled"));
        QVERIFY(cancelledAtDispatchIntent);
        QVERIFY(captureEvents.size() >= 4);
        QCOMPARE(captureEvents.at(0).value(QStringLiteral("kind")).toString(), QStringLiteral("slot_binding"));
        QCOMPARE(captureEvents.at(1).value(QStringLiteral("kind")).toString(), QStringLiteral("dispatch_intent"));
        QCOMPARE(requests.first().at(0).toString(), captureEvents.at(0)
            .value(QStringLiteral("requestId")).toString());
        const QJsonArray outgoingMessages = requests.first().at(1).toJsonArray();
        QVERIFY(!outgoingMessages.isEmpty());
        QVERIFY(std::none_of(outgoingMessages.cbegin(), outgoingMessages.cend(), [&](const QJsonValue &value) {
            return value.toObject().value(QStringLiteral("content")).toString().contains(previousSentinel);
        }));
        QVERIFY(std::any_of(outgoingMessages.cbegin(), outgoingMessages.cend(), [&](const QJsonValue &value) {
            const QJsonObject message = value.toObject();
            return message.value(QStringLiteral("role")).toString() == QStringLiteral("user")
                && message.value(QStringLiteral("content")).toString() == prompt;
        }));
        session->clear();
    }

    void providerCaptureRefusesDispatchAndResponseDeliveryWhenRecorderFails()
    {
        Application *application = Application::instance();
        QVERIFY(application);
        Session *session = application->getSession();
        QVERIFY(session);
        auto *host = session->getModelCommandHost();
        QVERIFY(host);
        PreferenceSnapshot restore(application->getApplicationSettings());
        session->clear();
        session->addModel(conversationFixture());
        session->setModelSelected(0, true);

        FixtureNetworkAccessManager intentNetwork;
        intentNetwork.enqueue(completeJsonScript(QStringLiteral("should never be requested")));
        WorkflowProcess intentWorkflow;
        AgentWidget intentWidget(host, &intentWorkflow, nullptr, &intentNetwork);
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setApiUrl(QStringLiteral("https://fixture.invalid/v1/chat/completions"));
        settings.setApiKey(QStringLiteral("capture-failure-key"));
        intentWidget.setProviderSettings(settings);
        auto *intentStreaming = intentWidget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(intentStreaming);
        intentStreaming->setChecked(false);
        QVERIFY(intentWidget.setProviderCaptureRecorder([](const QJsonObject &) { return false; }));
        QSignalSpy intentFinished(intentWidget.taskRunner(), &AgentTaskRunner::finished);
        intentWidget.startTask(QStringLiteral("intent rejection fixture"));
        QTRY_COMPARE_WITH_TIMEOUT(intentFinished.size(), 1, 2000);
        QCOMPARE(intentFinished.first().at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(intentNetwork.requests.size(), 0);

        FixtureNetworkAccessManager responseNetwork;
        const QJsonObject assistant = toolCall(QStringLiteral("capture-tool-rejected"),
            QStringLiteral("host_capabilities"), QJsonObject{}).value(QStringLiteral("assistantMessage")).toObject();
        const QJsonObject response{{QStringLiteral("choices"), QJsonArray{QJsonObject{
            {QStringLiteral("finish_reason"), QStringLiteral("tool_calls")},
            {QStringLiteral("message"), assistant}}}}};
        NetworkScript script;
        script.chunks << QJsonDocument(response).toJson(QJsonDocument::Compact);
        responseNetwork.enqueue(std::move(script));
        responseNetwork.enqueue(completeJsonScript(QStringLiteral("must not reach the tool")));
        WorkflowProcess responseWorkflow;
        AgentWidget responseWidget(host, &responseWorkflow, nullptr, &responseNetwork);
        responseWidget.setProviderSettings(settings);
        auto *responseStreaming = responseWidget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(responseStreaming);
        responseStreaming->setChecked(false);
        QStringList attemptedKinds;
        QVERIFY(responseWidget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            attemptedKinds.append(event.value(QStringLiteral("kind")).toString());
            return event.value(QStringLiteral("kind")) != QJsonValue(QStringLiteral("provider_response"));
        }));
        QSignalSpy responseFinished(responseWidget.taskRunner(), &AgentTaskRunner::finished);
        responseWidget.startTask(QStringLiteral("response rejection fixture"));
        QTRY_COMPARE_WITH_TIMEOUT(responseFinished.size(), 1, 2500);
        QCOMPARE(responseFinished.first().at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(responseNetwork.requests.size(), 1);
        QCOMPARE(attemptedKinds, QStringList({QStringLiteral("dispatch_intent"), QStringLiteral("provider_response")}));

        FixtureNetworkAccessManager cancellationNetwork;
        WorkflowProcess cancellationWorkflow;
        AgentWidget cancellationWidget(host, &cancellationWorkflow, nullptr, &cancellationNetwork);
        cancellationWidget.setProviderSettings(settings);
        auto *cancellationStreaming = cancellationWidget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(cancellationStreaming);
        cancellationStreaming->setChecked(false);
        bool cancellationIssued = false;
        bool cancellationReconfigurationWasRejected = true;
        QStringList cancellationKinds;
        QVERIFY(cancellationWidget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            const QString kind = event.value(QStringLiteral("kind")).toString();
            cancellationKinds.append(kind);
            if (kind == QStringLiteral("provider_interruption") || kind == QStringLiteral("task_terminal"))
                cancellationReconfigurationWasRejected = cancellationReconfigurationWasRejected
                    && !cancellationWidget.setProviderCaptureRecorder(AgentWidget::ProviderCaptureRecorder{});
            if (kind == QStringLiteral("dispatch_intent") && !cancellationIssued)
            {
                cancellationIssued = true;
                cancellationWidget.taskRunner()->cancel();
            }
            return true;
        }));
        QSignalSpy cancellationFinished(cancellationWidget.taskRunner(), &AgentTaskRunner::finished);
        cancellationWidget.startTask(QStringLiteral("reentrant cancellation fixture"));
        QTRY_COMPARE_WITH_TIMEOUT(cancellationFinished.size(), 1, 2000);
        QCOMPARE(cancellationFinished.first().at(0).toString(), QStringLiteral("cancelled"));
        QCOMPARE(cancellationNetwork.requests.size(), 0);
        QCOMPARE(cancellationKinds, QStringList({QStringLiteral("dispatch_intent"),
            QStringLiteral("provider_interruption"), QStringLiteral("task_terminal")}));
        QVERIFY(cancellationReconfigurationWasRejected);

        FixtureNetworkAccessManager interruptedNetwork;
        NetworkScript unavailable;
        unavailable.status = 503;
        unavailable.error = QNetworkReply::HostNotFoundError;
        unavailable.chunks << QByteArrayLiteral("private-error-body-sentinel");
        interruptedNetwork.enqueue(std::move(unavailable));
        WorkflowProcess interruptedWorkflow;
        AgentWidget interruptedWidget(host, &interruptedWorkflow, nullptr, &interruptedNetwork);
        interruptedWidget.setProviderSettings(settings);
        auto *interruptedStreaming = interruptedWidget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(interruptedStreaming);
        interruptedStreaming->setChecked(false);
        bool interruptionReconfigurationWasRejected = false;
        QStringList interruptedKinds;
        QVERIFY(interruptedWidget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            const QString kind = event.value(QStringLiteral("kind")).toString();
            interruptedKinds.append(kind);
            if (kind == QStringLiteral("provider_interruption"))
                interruptionReconfigurationWasRejected = !interruptedWidget.setProviderCaptureRecorder(
                    AgentWidget::ProviderCaptureRecorder{});
            return true;
        }));
        QSignalSpy interruptedFinished(interruptedWidget.taskRunner(), &AgentTaskRunner::finished);
        interruptedWidget.startTask(QStringLiteral("interruption fixture"));
        QTRY_COMPARE_WITH_TIMEOUT(interruptedFinished.size(), 1, 2500);
        QCOMPARE(interruptedFinished.first().at(0).toString(), QStringLiteral("failed"));
        QCOMPARE(interruptedNetwork.requests.size(), 1);
        QVERIFY(interruptedKinds.contains(QStringLiteral("provider_interruption")));
        QVERIFY(interruptionReconfigurationWasRejected);

        FixtureNetworkAccessManager terminalFailureNetwork;
        terminalFailureNetwork.enqueue(completeJsonScript(QStringLiteral("terminal capture failure fixture")));
        WorkflowProcess terminalFailureWorkflow;
        AgentWidget terminalFailureWidget(host, &terminalFailureWorkflow, nullptr, &terminalFailureNetwork);
        terminalFailureWidget.setProviderSettings(settings);
        auto *terminalFailureStreaming = terminalFailureWidget.findChild<QCheckBox *>(QStringLiteral("agentStreamResponses"));
        QVERIFY(terminalFailureStreaming);
        terminalFailureStreaming->setChecked(false);
        QStringList terminalFailureKinds;
        QVERIFY(terminalFailureWidget.setProviderCaptureRecorder([&](const QJsonObject &event) {
            const QString kind = event.value(QStringLiteral("kind")).toString();
            terminalFailureKinds.append(kind);
            return kind != QStringLiteral("task_terminal");
        }));
        QSignalSpy terminalFailureFinished(terminalFailureWidget.taskRunner(), &AgentTaskRunner::finished);
        terminalFailureWidget.startTask(QStringLiteral("terminal failure fixture"));
        QTRY_COMPARE_WITH_TIMEOUT(terminalFailureFinished.size(), 1, 2500);
        QCOMPARE(terminalFailureFinished.first().at(0).toString(), QStringLiteral("completed"));
        QLabel *terminalStatus = terminalFailureWidget.findChild<QLabel *>(QStringLiteral("agentConversationStatus"));
        QVERIFY(terminalStatus);
        QVERIFY(terminalStatus->text().contains(QStringLiteral("capture incomplete"), Qt::CaseInsensitive));
        QVERIFY(terminalFailureKinds.contains(QStringLiteral("task_terminal")));
        session->clear();
    }
};

int runAgentConversationWidgetTests(int argc, char **argv)
{
    TestAgentConversationWidget test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_conversation_widget.moc"
