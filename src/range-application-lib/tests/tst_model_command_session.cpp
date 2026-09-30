#include <QtTest>
#include <rmodel_command_session.h>
#include <limits>

namespace {
RModel modalModel()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    return model;
}
QJsonObject request(const RModelCommandSession &session, QString command = "analysis.modal.configure")
{
    const auto current = session.query();
    auto parameters = current["modal"].toObject();
    parameters["modes"] = 3;
    parameters["method"] = "multiple";
    return {{"apiVersion", "range.application/v1"}, {"commandId", command},
        {"commandVersion", 1}, {"modelId", current["modelId"]},
        {"expectedRevision", current["revision"]},
        {"requestId", QUuid::createUuid().toString()},
        {"idempotencyKey", QUuid::createUuid().toString()},
        {"parameters", command == "analysis.modal.configure" ? parameters : QJsonObject{}},
        {"dryRun", false}};
}
}

class TestModelCommandSession : public QObject
{
    Q_OBJECT
private slots:
    void ownedModelPreviewAndReplay();
    void rejectWithoutMutation_data();
    void rejectWithoutMutation();
    void undoRedoAndStaleReplay();
    void unsupportedModelAndForeignIdentity();
    void boundedHistory();
    void branchAndRetryPreserveModel();
};

void TestModelCommandSession::ownedModelPreviewAndReplay()
{
    auto input = modalModel();
    const auto originalModes = input.getProblemSetup().getModalSetup().getNModesToExtract();
    RModelCommandSession session(input);
    auto command = request(session);
    const auto original = session.query();
    command["dryRun"] = true;
    QCOMPARE(session.execute(command)["status"].toString(), QString("preview"));
    QCOMPARE(session.query(), original);
    command["dryRun"] = false;
    const auto accepted = session.execute(command);
    QCOMPARE(accepted["status"].toString(), QString("accepted"));
    QCOMPARE(session.model().getProblemSetup().getModalSetup().getNModesToExtract(), 3u);
    QCOMPARE(input.getProblemSetup().getModalSetup().getNModesToExtract(), originalModes);
    const auto after = session.query();
    QCOMPARE(session.execute(command), accepted);
    QCOMPARE(session.query(), after);
    command["requestId"] = "different-request";
    QCOMPARE(session.execute(command)["code"].toString(), QString("idempotency_conflict"));
    QCOMPARE(session.query(), after);
}

void TestModelCommandSession::rejectWithoutMutation_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<QJsonValue>("value");
    QTest::newRow("zero modes") << QString("modes") << QJsonValue(0);
    QTest::newRow("fraction") << QString("iterations") << QJsonValue(1.5);
    QTest::newRow("huge") << QString("iterations") << QJsonValue(1000001);
    QTest::newRow("bool") << QString("modes") << QJsonValue(true);
    QTest::newRow("string") << QString("modes") << QJsonValue("3");
    QTest::newRow("zero tolerance") << QString("tolerance") << QJsonValue(0);
    QTest::newRow("large tolerance") << QString("tolerance") << QJsonValue(2);
    QTest::newRow("nan") << QString("tolerance") << QJsonValue(std::numeric_limits<double>::quiet_NaN());
    QTest::newRow("bad method") << QString("method") << QJsonValue("unknown");
    QTest::newRow("units") << QString("unit") << QJsonValue("Hz");
    QTest::newRow("unknown") << QString("extra") << QJsonValue(1);
}

void TestModelCommandSession::rejectWithoutMutation()
{
    QFETCH(QString, field);
    QFETCH(QJsonValue, value);
    RModelCommandSession session(modalModel());
    const auto before = session.query();
    auto command = request(session);
    auto parameters = command["parameters"].toObject();
    parameters[field] = value;
    command["parameters"] = parameters;
    QCOMPARE(session.execute(command)["code"].toString(), QString("invalid_parameters"));
    QCOMPARE(session.query(), before);
    command = request(session);
    command["extra"] = 1;
    QCOMPARE(session.execute(command)["code"].toString(), QString("invalid_request"));
    QCOMPARE(session.query(), before);
}

void TestModelCommandSession::undoRedoAndStaleReplay()
{
    RModelCommandSession session(modalModel());
    const auto initial = session.query();
    auto command = request(session);
    QCOMPARE(session.execute(command)["status"].toString(), QString("accepted"));
    const auto firstRevision = session.query().value("revision").toString();
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QString("accepted"));
    QCOMPARE(session.query()["modal"], initial["modal"]);
    QVERIFY(session.query()["revision"] != initial["revision"]);
    QCOMPARE(session.execute(command)["code"].toString(), QString("stale_revision"));
    QCOMPARE(session.execute(request(session, "model.redo"))["status"].toString(), QString("accepted"));
    QCOMPARE(session.model().getProblemSetup().getModalSetup().getNModesToExtract(), 3u);
    QVERIFY(session.query().value("revision").toString() != firstRevision);
    const auto before = session.query();
    auto stale = request(session);
    stale["expectedRevision"] = initial["revision"];
    QCOMPARE(session.execute(stale)["code"].toString(), QString("stale_revision"));
    QCOMPARE(session.query(), before);
    QCOMPARE(session.execute(request(session, "model.redo"))["code"].toString(), QString("empty_history"));
}

void TestModelCommandSession::unsupportedModelAndForeignIdentity()
{
    RModelCommandSession noModal{RModel{}};
    QVERIFY(!noModal.query()["canConfigureModal"].toBool());
    QCOMPARE(noModal.execute(request(noModal))["code"].toString(), QString("unsupported_analysis"));
    RModelCommandSession first(modalModel()), second(modalModel());
    QCOMPARE(second.execute(request(first))["code"].toString(), QString("wrong_model"));
    auto command = request(first);
    command["commandVersion"] = 2;
    QCOMPARE(first.execute(command)["code"].toString(), QString("invalid_request"));
}

void TestModelCommandSession::boundedHistory()
{
    RModelCommandSession session(modalModel());
    auto first = request(session);
    QCOMPARE(session.execute(first)["status"].toString(), QString("accepted"));
    for (int i = 1; i < RModelCommandSession::MaxCommands; ++i)
        QCOMPARE(session.execute(request(session))["status"].toString(), QString("accepted"));
    const auto before = session.query();
    QCOMPARE(session.execute(request(session))["code"].toString(), QString("session_limit"));
    QCOMPARE(session.query(), before);
    QCOMPARE(session.execute(first)["code"].toString(), QString("stale_revision"));
}

void TestModelCommandSession::branchAndRetryPreserveModel()
{
    auto source = modalModel();
    source.addNode(RNode(1.25, -2.5, 3.75));
    RModelCommandSession session(source);
    auto command = request(session);
    auto parameters = command["parameters"].toObject();
    parameters["modes"] = -1;
    command["parameters"] = parameters;
    QCOMPARE(session.execute(command)["status"].toString(), QString("rejected"));
    parameters["modes"] = 3;
    command["parameters"] = parameters;
    QCOMPARE(session.execute(command)["status"].toString(), QString("accepted"));
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QString("accepted"));
    QVERIFY(session.query()["canRedo"].toBool());
    auto branch = request(session);
    parameters["modes"] = 7;
    branch["parameters"] = parameters;
    QCOMPARE(session.execute(branch)["status"].toString(), QString("accepted"));
    QVERIFY(!session.query()["canRedo"].toBool());
    QCOMPARE(session.execute(request(session, "model.redo"))["code"].toString(), QString("empty_history"));
    QCOMPARE(session.model().getNNodes(), source.getNNodes());
    QCOMPARE(session.model().getNode(0).getX(), 1.25);
    QCOMPARE(session.model().getNode(0).getY(), -2.5);
    QCOMPARE(session.model().getNode(0).getZ(), 3.75);
}

QTEST_APPLESS_MAIN(TestModelCommandSession)
#include "tst_model_command_session.moc"
