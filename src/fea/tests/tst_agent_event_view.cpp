#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QUuid>

#include <rml_model.h>

#include "agent_widget.h"
#include "application.h"
#include "main_window.h"
#include "model_command_host.h"
#include "session.h"
#include "workflow_widget.h"

namespace
{
Model eventViewFixture()
{
    Model model;
    model.setName(QStringLiteral("Event view fixture"));
    model.setFileName(QStringLiteral("C:/private/event-view-source-marker.ram"));
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

QJsonObject request(ModelCommandHost *host, const QString &operation, const QJsonObject &parameters = {})
{
    return host->handle({{QStringLiteral("apiVersion"), QStringLiteral("range.host/v1")},
                         {QStringLiteral("requestId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                         {QStringLiteral("operation"), operation},
                         {QStringLiteral("parameters"), parameters}});
}

QJsonObject configureIterations(ModelCommandHost *host, const QString &documentId,
                                const QString &idempotencyKey, int iterations)
{
    const auto capabilities = request(host, QStringLiteral("capabilities.query"));
    const QString epoch = capabilities.value(QStringLiteral("result")).toObject()
                              .value(QStringLiteral("sessionEpoch")).toString();
    if (capabilities.value(QStringLiteral("status")).toString() != QStringLiteral("ok") || epoch.isEmpty())
        return capabilities;
    const auto contextResponse = request(host, QStringLiteral("context.query"),
        {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                                               {QStringLiteral("sessionEpoch"), epoch}}}});
    if (contextResponse.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) return contextResponse;
    const auto documents = contextResponse.value(QStringLiteral("result")).toObject().value(QStringLiteral("documents")).toArray();
    if (documents.isEmpty()) return contextResponse;
    const auto document = documents.first().toObject();
    auto arguments = document.value(QStringLiteral("acceptedState")).toObject().value(QStringLiteral("modal")).toObject();
    arguments.insert(QStringLiteral("iterations"), iterations);
    const auto previewResponse = request(host, QStringLiteral("command.preview"),
        {{QStringLiteral("target"), QJsonObject{{QStringLiteral("documentId"), documentId},
                                               {QStringLiteral("sessionEpoch"), epoch}}},
         {QStringLiteral("expectedDocumentRevision"), document.value(QStringLiteral("documentRevision"))},
         {QStringLiteral("command"), QStringLiteral("analysis.modal.configure")},
         {QStringLiteral("commandVersion"), 1}, {QStringLiteral("arguments"), arguments}});
    if (previewResponse.value(QStringLiteral("status")).toString() != QStringLiteral("preview")) return previewResponse;
    const QString previewId = previewResponse.value(QStringLiteral("result")).toObject()
                                  .value(QStringLiteral("previewId")).toString();
    if (previewId.isEmpty() || !host->approvePreview(previewId))
        return {{QStringLiteral("status"), QStringLiteral("test_error")}};
    return request(host, QStringLiteral("command.execute"),
        {{QStringLiteral("previewId"), previewId}, {QStringLiteral("idempotencyKey"), idempotencyKey}});
}
}

class TestAgentEventView : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(!ModelCommandHost::isBusy(), 15000);
        Application::instance()->getSession()->clear();
    }

    void resynchronizesAfterGapAndKeepsUserDraftAndCredential()
    {
        auto *application = Application::instance();
        QVERIFY(application);
        auto *session = application->getSession();
        auto *host = session->getModelCommandHost();
        auto *window = application->getMainWindow();
        QVERIFY(session);
        QVERIFY(host);
        QVERIFY(window);
        auto *workflow = window->findChild<WorkflowWidget *>();
        QVERIFY(workflow);

        session->clear();
        session->addModel(eventViewFixture());
        session->setModelSelected(0, true);
        const QString documentId = session->modelCommandStamp(0).first;
        QVERIFY(!documentId.isEmpty());

        // Exercise the real widget and host without starting a provider request.
        AgentWidget widget(host, workflow->transport());
        widget.show();
        auto *eventHistory = widget.findChild<QPlainTextEdit *>(QStringLiteral("agentEventHistory"));
        auto *contextSummary = widget.findChild<QLabel *>(QStringLiteral("agentEventContextSummary"));
        auto *eventStatus = widget.findChild<QLabel *>(QStringLiteral("agentEventStatus"));
        auto *eventToggle = widget.findChild<QToolButton *>(QStringLiteral("agentEventHistoryToggle"));
        auto *goal = widget.findChild<QPlainTextEdit *>(QStringLiteral("agentGoal"));
        auto *key = widget.findChild<QLineEdit *>(QStringLiteral("agentApiKey"));
        QVERIFY(eventHistory);
        QVERIFY(contextSummary);
        QVERIFY(eventStatus);
        QVERIFY(eventToggle);
        QVERIFY(goal);
        QVERIFY(key);
        eventToggle->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(eventHistory->isVisible(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(contextSummary->text().contains(QStringLiteral("iterations 12")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(eventStatus->text().startsWith(QStringLiteral("Up to date")), 3000);

        const QString draft = QStringLiteral("keep this unfinished request");
        const QString credential = QStringLiteral("private-key-sentinel-must-not-appear");
        const QString sourceMarker = QStringLiteral("event-view-source-marker.ram");
        goal->setPlainText(draft);
        key->setText(credential);
        goal->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY_WITH_TIMEOUT(goal->hasFocus(), 1000);

        // Synchronously overrun the host's 256-record ring without yielding to
        // the widget's 750 ms poll timer, then make a real approved host edit.
        for (int i = 0; i < 300; ++i) session->setModelSelectionChanged(0);
        const auto changed = configureIterations(host, documentId, QStringLiteral("agent-event-resync-41"), 41);
        QCOMPARE(changed.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));

        QTRY_VERIFY_WITH_TIMEOUT(eventHistory->toPlainText().contains(QStringLiteral("Event history gap")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(contextSummary->text().contains(QStringLiteral("iterations 41")), 1000);
        QVERIFY(goal->hasFocus());
        QCOMPARE(goal->toPlainText(), draft);
        QCOMPARE(key->text(), credential);
        QVERIFY(eventHistory->blockCount() <= 256);

        const QString visibleAfterResync = eventHistory->toPlainText() + QLatin1Char('\n') + contextSummary->text();
        QVERIFY(!visibleAfterResync.contains(sourceMarker));
        QVERIFY(!visibleAfterResync.contains(credential));

        const auto nextChange = configureIterations(host, documentId, QStringLiteral("agent-event-next-58"), 58);
        QCOMPARE(nextChange.value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
        const QString nextOperation = nextChange.value(QStringLiteral("result")).toObject()
                                         .value(QStringLiteral("operationId")).toString();
        QVERIFY(!nextOperation.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(eventHistory->toPlainText().contains(QStringLiteral("Document changed")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(eventHistory->toPlainText().contains(nextOperation), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(contextSummary->text().contains(QStringLiteral("iterations 58")), 1000);
        QVERIFY(goal->hasFocus());
        QCOMPARE(goal->toPlainText(), draft);
        QCOMPARE(key->text(), credential);

        // Force one resync while the Session cannot be read. The next timer
        // poll must retry the cached context without waiting for another event.
        for (int i = 0; i < 300; ++i) session->setModelSelectionChanged(0);
        session->lock();
        const bool blockedRefreshInvoked = QMetaObject::invokeMethod(
            &widget, "refreshHostEvents", Qt::DirectConnection);
        session->unlock();
        QVERIFY(blockedRefreshInvoked);
        QVERIFY(contextSummary->text().contains(QStringLiteral("may be stale")));
        QTRY_VERIFY_WITH_TIMEOUT(contextSummary->text().contains(QStringLiteral("Live selected-document context"))
                                 && contextSummary->text().contains(QStringLiteral("iterations 58")), 5000);

        const QString completeView = eventHistory->toPlainText() + QLatin1Char('\n') + contextSummary->text();
        QVERIFY(!completeView.contains(sourceMarker));
        QVERIFY(!completeView.contains(credential));
    }
};

int runAgentEventViewTests(int argc, char **argv)
{
    TestAgentEventView test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_agent_event_view.moc"
