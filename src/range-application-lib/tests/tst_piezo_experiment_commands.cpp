#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QUuid>
#include <rmodel_command_session.h>
#include <rpiezo_experiment_service.h>
#include <rbl_interaction_record_v1.h>
#include <algorithm>

namespace {
RModel beamWithResults()
{
    RModel model;
    for (uint n = 0; n < 3; ++n) {
        model.addNode(RNode(double(n), 0, 0));
        model.setNodePhysicalId(n, 101 + 11*n);
    }
    for (uint n = 0; n < 2; ++n) {
        RElement element(R_ELEMENT_BEAM1);
        element.setNodeId(0, n); element.setNodeId(1, n+1);
        model.addElement(element, true, 0);
        model.setElementPhysicalId(n, 501 + 13*n);
    }
    model.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
    RVariable displacement(R_VARIABLE_DISPLACEMENT_X, R_VARIABLE_APPLY_NODE);
    displacement.resize(1, model.getNNodes(), true);
    displacement.setValue(0, 2, .0125);
    model.addVariable(displacement);
    model.getLine(0).getData().getVariableData()[R_VARIABLE_DISPLACEMENT_X]
        = R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT;
    return model;
}

QJsonObject recipe(const RModel &model, const QString &family = {})
{
    auto result = RPiezoExperimentService::exampleRecipe(model, family);
    result["replaceLegacyConditions"] = true;
    return result;
}

QJsonObject request(const RModelCommandSession &session, const QString &command,
                    const QJsonObject &parameters = {}, bool dryRun = false)
{
    const auto query = session.query();
    return {{"apiVersion", "range.application/v1"}, {"commandId", command},
        {"commandVersion", 1}, {"modelId", query["modelId"]},
        {"expectedRevision", query["revision"]},
        {"requestId", QUuid::createUuid().toString()},
        {"idempotencyKey", QUuid::createUuid().toString()},
        {"parameters", parameters}, {"dryRun", dryRun}};
}

QJsonObject journal(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
}

class TestPiezoExperimentCommands : public QObject
{
    Q_OBJECT
private slots:
    void templateUndoNewBranchKeepsPhysicalIdsFresh();
    void completePhysicsChangesRejectCapturedDelta_data();
    void completePhysicsChangesRejectCapturedDelta();
    void publicationClearsAndRestoresBindingsKeepingPresentation();
    void modalBaselineUndoRestoresTypedSetupAndResults();
    void historicalScopeThreeAppendsScopeFour();
    void journalFailurePreservesPiezoModelHistoryAndLifecycle();
};

void TestPiezoExperimentCommands::templateUndoNewBranchKeepsPhysicalIdsFresh()
{
    RModelCommandSession session{RModel{}};
    const auto parameters = recipe(session.model(), "planarBeam");
    const auto before = session.query();
    const auto preview = session.execute(request(session, "piezo.configure-experiment", parameters, true));
    QCOMPARE(preview["status"].toString(), QString("preview"));
    QCOMPARE(session.model().getNNodes(), 0u);
    QCOMPARE(session.query()["revision"], before["revision"]);
    const auto accepted = session.execute(request(session, "piezo.configure-experiment", parameters));
    QVERIFY2(accepted["status"] == "accepted", qPrintable(QJsonDocument(accepted).toJson()));
    const RModel first(session.model());
    QVERIFY(first.getNNodes() > 0); QVERIFY(first.getNElements() > 0);
    uint maxNode = 0, maxElement = 0, maxGroup = 0;
    for (uint n = 0; n < first.getNNodes(); ++n) maxNode = std::max(maxNode, first.getNodePhysicalId(n));
    for (uint e = 0; e < first.getNElements(); ++e) maxElement = std::max(maxElement, first.getElementPhysicalId(e));
    for (uint g = 0; g < first.getNElementGroups(); ++g) maxGroup = std::max(maxGroup, first.getElementGroupPtr(g)->getPhysicalId());
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QString("accepted"));
    QCOMPARE(session.model().getNNodes(), 0u);
    QVERIFY(session.query()["canRedo"].toBool());
    const auto branch = session.execute(request(session, "piezo.configure-experiment", recipe(session.model(), "planarBeam")));
    QVERIFY2(branch["status"] == "accepted", qPrintable(QJsonDocument(branch).toJson()));
    for (uint n = 0; n < session.model().getNNodes(); ++n) QVERIFY(session.model().getNodePhysicalId(n) > maxNode);
    for (uint e = 0; e < session.model().getNElements(); ++e) QVERIFY(session.model().getElementPhysicalId(e) > maxElement);
    for (uint g = 0; g < session.model().getNElementGroups(); ++g) QVERIFY(session.model().getElementGroupPtr(g)->getPhysicalId() > maxGroup);
    QVERIFY(!session.query()["canRedo"].toBool());
}

void TestPiezoExperimentCommands::completePhysicsChangesRejectCapturedDelta_data()
{
    QTest::addColumn<int>("mutation");
    QTest::newRow("stored-result-value") << 0;
    QTest::newRow("initial-condition") << 1;
    QTest::newRow("environment-condition") << 2;
    QTest::newRow("monitoring-point") << 3;
    QTest::newRow("line-section") << 4;
    QTest::newRow("matrix-solver-tolerance") << 5;
}

void TestPiezoExperimentCommands::completePhysicsChangesRejectCapturedDelta()
{
    QFETCH(int, mutation);
    const RModel source = beamWithResults();
    const auto parameters = recipe(source);
    const auto captured = RModelCommandService::preview(source, "piezo.configure-experiment", parameters, "document", "revision");
    QVERIFY2(captured.valid, qPrintable(captured.message));
    RModel changed(source);
    switch (mutation) {
    case 0: changed.getVariable(0).setValue(0, 2, .025); break;
    case 1: changed.getLine(0).addInitialCondition(RInitialCondition(R_INITIAL_CONDITION_VELOCITY)); break;
    case 2: changed.getLine(0).addEnvironmentCondition(REnvironmentCondition(R_ENVIRONMENT_CONDITION_CUSTOM)); break;
    case 3: changed.getMonitoringPointManager().push_back(RMonitoringPoint(R_VARIABLE_DISPLACEMENT_X, RR3Vector(2,0,0))); break;
    case 4: changed.getLine(0).setCrossArea(changed.getLine(0).getCrossArea() + .01); break;
    case 5: {
        auto &solver = changed.getMatrixSolverConf(RMatrixSolverConf::CG);
        solver.setSolverCvgValue(solver.getSolverCvgValue() + 1e-7);
        break;
    }
    }
    const auto changedHash = RModelCommandService::stateFingerprint(changed);
    QVERIFY(changedHash != RModelCommandService::stateFingerprint(source));
    const auto stale = RModelCommandService::applyDelta(changed, captured.delta, true);
    QVERIFY(!stale.valid); QCOMPARE(stale.code, QString("stale_history"));
    QCOMPARE(RModelCommandService::stateFingerprint(changed), changedHash);
    // Repreview uses the same document/revision tokens: physical changes alone
    // must invalidate the fingerprint a host captured for approval.
    const auto refreshed = RModelCommandService::preview(changed, "piezo.configure-experiment", parameters, "document", "revision");
    QVERIFY2(refreshed.valid, qPrintable(refreshed.message));
    QVERIFY(refreshed.fingerprint != captured.fingerprint);
}

void TestPiezoExperimentCommands::publicationClearsAndRestoresBindingsKeepingPresentation()
{
    const auto source = beamWithResults();
    const auto preview = RModelCommandService::preview(source, "piezo.configure-experiment", recipe(source));
    QVERIFY2(preview.valid, qPrintable(preview.message));
    RModel live(source);
    live.setName("Live document"); live.getLine(0).setName("Live group");
    live.getLine(0).getData().setVisible(false);
    live.getLine(0).getData().setDrawWire(true);
    live.getLine(0).getData().setColor(13, 29, 47, 61);
    QString error;
    QVERIFY2(live.applyPiezoExperimentCandidate(preview.candidate, [](const RModel &) { return true; }, &error), qPrintable(error));
    QCOMPARE(live.getNVariables(), 0u);
    QVERIFY(live.getLine(0).getData().getVariableData().empty());
    QCOMPARE(live.getName(), QString("Live document"));
    QCOMPARE(live.getLine(0).getName(), QString("Live group"));
    QVERIFY(!live.getLine(0).getData().getVisible()); QVERIFY(live.getLine(0).getData().getDrawWire());
    int red, green, blue, alpha;
    live.getLine(0).getData().getColor(red, green, blue, alpha);
    QCOMPARE(red,13); QCOMPARE(green,29); QCOMPARE(blue,47); QCOMPARE(alpha,61);
    const auto undo = RModelCommandService::applyDelta(live, preview.delta, false);
    QVERIFY2(undo.valid, qPrintable(undo.message));
    QVERIFY2(live.applyPiezoExperimentCandidate(undo.candidate, [](const RModel &) { return true; }, &error), qPrintable(error));
    QCOMPARE(live.getNVariables(), 1u);
    QCOMPARE(live.getVariable(0).getValue(0,2), .0125);
    QVERIFY(live.getLine(0).getData().getVariableData() == source.getLine(0).getData().getVariableData());
    QCOMPARE(live.getLine(0).getName(), QString("Live group"));
    QVERIFY(!live.getLine(0).getData().getVisible()); QVERIFY(live.getLine(0).getData().getDrawWire());
    live.getLine(0).getData().getColor(red, green, blue, alpha);
    QCOMPARE(red,13); QCOMPARE(green,29); QCOMPARE(blue,47); QCOMPARE(alpha,61);
}

void TestPiezoExperimentCommands::modalBaselineUndoRestoresTypedSetupAndResults()
{
    auto source = beamWithResults();
    source.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS_MODAL));
    auto &modal = source.getProblemSetup().getModalSetup();
    modal.setNModesToExtract(3); modal.setNIterations(73);
    modal.setConvergenceValue(2e-9); modal.setMode(2); modal.setFrequency(17.25);
    const auto sourceHash = RModelCommandService::stateFingerprint(source);
    const auto originalParameters = RModelCommandService::modalParameters(modal);
    RModelCommandSession session(source);
    const auto configured = session.execute(request(session, "piezo.configure-experiment", recipe(source)));
    QVERIFY2(configured["status"] == "accepted", qPrintable(QJsonDocument(configured).toJson()));
    QCOMPARE(session.model().getProblemTaskTree().getProblemTypeMask(), R_PROBLEM_STRESS);
    QCOMPARE(session.model().getNVariables(), 0u);
    QVERIFY(session.model().getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
    const auto undone = session.execute(request(session, "model.undo"));
    QVERIFY2(undone["status"] == "accepted", qPrintable(QJsonDocument(undone).toJson()));
    QCOMPARE(session.model().getProblemTaskTree().getProblemTypeMask(), R_PROBLEM_STRESS_MODAL);
    QCOMPARE(RModelCommandService::modalParameters(session.model().getProblemSetup().getModalSetup()), originalParameters);
    QCOMPARE(session.model().getProblemSetup().getModalSetup().getMode(), 2u);
    QCOMPARE(session.model().getProblemSetup().getModalSetup().getFrequency(), 17.25);
    QCOMPARE(session.model().getNVariables(), 1u);
    QCOMPARE(session.model().getVariable(0).getValue(0,2), .0125);
    QCOMPARE(RModelCommandService::stateFingerprint(session.model()), sourceHash);
}

void TestPiezoExperimentCommands::historicalScopeThreeAppendsScopeFour()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath("interaction.json");
    {
        RModelCommandSession historical(RModel{}, options);
        QVERIFY(historical.close());
    }
    auto envelope = journal(options.path);
    auto records = envelope["records"].toArray(); QVERIFY(!records.isEmpty());
    // Historical hashes are opaque evidence. Use a valid old record chain and
    // retain its exact metadata and hash bytes across the new session append.
    for (int i = 0; i < records.size(); ++i) {
        auto record = records[i].toObject();
        auto snapshot = record["selectionSnapshot"].toObject();
        snapshot["stateHashScopeVersion"] = 3;
        record["selectionSnapshot"] = snapshot;
        record["preStateHash"] = QString(64, 'a');
        record["postStateHash"] = QString(64, 'b');
        records[i] = record;
    }
    envelope["records"] = records;
    QFile file(options.path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
    RModelCommandSession current(RModel{}, options);
    const auto accepted = current.execute(request(current, "piezo.configure-experiment", recipe(current.model(), "planarBeam")));
    QVERIFY2(accepted["status"] == "accepted", qPrintable(QJsonDocument(accepted).toJson()));
    const auto appended = journal(options.path)["records"].toArray();
    QVERIFY(appended.size() > records.size());
    QUuid parent;
    bool sawConfigure = false;
    for (int i = 0; i < appended.size(); ++i) {
        RInteractionRecordV1 record; QString error;
        QVERIFY2(RInteractionRecordV1::fromJson(QJsonDocument(appended[i].toObject()).toJson(), record, &error), qPrintable(error));
        QCOMPARE(record.parentRecordId, parent); parent = record.recordId;
        if (i < records.size()) QCOMPARE(appended[i], records[i]);
        else QCOMPARE(record.selectionSnapshot["stateHashScopeVersion"].toInt(), 4);
        sawConfigure = sawConfigure || record.commandId == "piezo.configure-experiment";
    }
    QVERIFY(sawConfigure);
}

void TestPiezoExperimentCommands::journalFailurePreservesPiezoModelHistoryAndLifecycle()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath("interaction.json");
    const RModel source = beamWithResults();
    RModelCommandSession session(source, options);

    const auto preview = session.execute(request(session, "piezo.configure-experiment", recipe(source), true));
    QVERIFY2(preview["status"] == "preview", qPrintable(QJsonDocument(preview).toJson()));
    const auto before = session.query();
    const auto beforeHash = RModelCommandService::stateFingerprint(session.model());
    QFile journalFile(options.path);
    QVERIFY(journalFile.open(QIODevice::ReadOnly));
    const auto journalBefore = journalFile.readAll();
    journalFile.close();
    QVERIFY(!before["canUndo"].toBool());
    QVERIFY(!before["canRedo"].toBool());
    QCOMPARE(before["lifecycle"].toString(), QStringLiteral("ready"));

    const auto command = request(session, "piezo.configure-experiment", recipe(source));
    QLockFile lock(options.path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    const auto failed = session.execute(command);
    QCOMPARE(failed["code"].toString(), QStringLiteral("journal_failure"));
    QCOMPARE(session.query(), before);
    QCOMPARE(RModelCommandService::stateFingerprint(session.model()), beforeHash);
    QFile journalAfterFile(options.path);
    QVERIFY(journalAfterFile.open(QIODevice::ReadOnly));
    QCOMPARE(journalAfterFile.readAll(), journalBefore);
    QCOMPARE(session.query()["lifecycle"].toString(), QStringLiteral("ready"));
    QVERIFY(!session.query()["canUndo"].toBool());
    QVERIFY(!session.query()["canRedo"].toBool());
}

QTEST_APPLESS_MAIN(TestPiezoExperimentCommands)
#include "tst_piezo_experiment_commands.moc"
