#include <QtTest>
#include "model.h"
#include <rpiezo_experiment_service.h>

namespace {
RModel fixture(bool plate = false)
{
    RModel m;
    const double xy[4][2]{{0,0},{1,0},{1,1},{0,1}};
    for (uint n = 0; n < (plate ? 4u : 2u); ++n) m.addNode(RNode(xy[n][0],xy[n][1],0));
    RElement e(plate ? R_ELEMENT_QUAD1 : R_ELEMENT_BEAM1);
    for (uint n = 0; n < e.size(); ++n) e.setNodeId(n,n);
    m.addElement(e,true,0);
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
    auto &d = m.getProblemSetup().getStructuralDynamicsSetup();
    d.setEnabled(true); d.setTimeStep(.02); d.setNSteps(7);
    m.getTimeSolver().setInputStartTime(.02);
    RStructuralPiezoLayeredSetup setup;
    setup.enabled = true;
    if (plate) setup.family = RStructuralPiezoLayeredModelFamily::FlatPlate;
    RStructuralPiezoLayeredMaterial material;
    material.materialId = "host"; material.density = 7800;
    material.sourceId = "publication-fixture"; material.sourceSha256 = QString(64,'a');
    for (uint i = 0; i < 6; ++i) material.elasticityOrCompliance[6*i+i] = 1e9;
    setup.materials = {material};
    setup.members = {{m.getElementPhysicalId(0),{{"host",-.01,.01,plate ? 1.0 : .1,"",1,{{-1,1,-1,1}}}}}};
    setup.supports = {{m.getNodePhysicalId(0),RStructuralPiezoPhysicalDof::UX,0}};
    setup.excitationConfiguration = "whole-excitation";
    setup.controllerConfiguration = "whole-controller";
    setup.designConfiguration = "whole-design";
    setup.bindToModel(m);
    m.getProblemSetup().setStructuralPiezoLayeredSetup(setup);
    return m;
}

QString fingerprint(const RModel &model) { return RModelCommandService::stateFingerprint(model); }
}

class TestPiezoExperimentHistory : public QObject {
    Q_OBJECT
private slots:
    void wholeOperationAndStaleApproval();
    void generatedIdentityAndBarriers();
    void failedCommitRetainsStacks();
    void axialWholeOperationHistory();
};

void TestPiezoExperimentHistory::axialWholeOperationHistory()
{
    Model live;
    const auto before=fingerprint(live);
    const auto preview=RPiezoExperimentService::preview(live,RPiezoExperimentService::exampleRecipe(live,"axialRod"));
    QVERIFY2(preview.valid,qPrintable(preview.message));
    const auto after=fingerprint(preview.candidate);
    QString error;
    QVERIFY2(live.commitPiezoExperimentWithHistory(preview.candidate,before,after,10,"Axial experiment",&error),qPrintable(error));
    QVERIFY(live.getProblemSetup().getStructuralPiezoAxialSetup().enabled);
    QCOMPARE(fingerprint(live),after);
    QCOMPARE(live.getUndoStackSize(),1u);
    const auto physical=live.getElementPhysicalId(0);
    live.undo(0);
    QCOMPARE(fingerprint(live),before);
    QVERIFY(!live.getProblemSetup().getStructuralPiezoAxialSetup().enabled);
    live.redo(0);
    QCOMPARE(fingerprint(live),after);
    QCOMPARE(live.getElementPhysicalId(0),physical);
    QVERIFY(!RPiezoExperimentService::recipeFromModel(live).isEmpty());
    auto recipe=RPiezoExperimentService::recipeFromModel(live);auto members=recipe["members"].toArray();
    auto member=members[1].toObject();member["area"]=member["area"].toDouble()*2;members[1]=member;recipe["members"]=members;
    const auto regrouped=RPiezoExperimentService::preview(live,recipe);
    QVERIFY2(regrouped.valid,qPrintable(regrouped.message));QCOMPARE(regrouped.candidate.getNLines(),2u);
    const auto regroupedIdentity=fingerprint(regrouped.candidate);
    QVERIFY2(live.commitPiezoExperimentWithHistory(regrouped.candidate,after,regroupedIdentity,10,"Per-member area",&error),qPrintable(error));
    const auto newGroupId=live.getLine(1).getPhysicalId();
    live.undo(0);QCOMPARE(fingerprint(live),after);QCOMPARE(live.getNLines(),1u);
    live.redo(0);QCOMPARE(fingerprint(live),regroupedIdentity);QCOMPARE(live.getNLines(),2u);
    QCOMPARE(live.getLine(1).getPhysicalId(),newGroupId);QCOMPARE(live.getElementPhysicalId(0),physical);
}

void TestPiezoExperimentHistory::wholeOperationAndStaleApproval()
{
    Model live;
    const auto generated = fixture();
    RModel baseline(generated);
    baseline.getProblemSetup().setStructuralPiezoLayeredSetup({});
    baseline.getProblemSetup().getStructuralDynamicsSetup().setTimeStep(.07);
    baseline.getTimeSolver().setInputStartTime(.4);
    baseline.getTimeSolver().setInputTimeStepSize(.07);
    const uint variable = baseline.addVariable(RVariable(R_VARIABLE_DISPLACEMENT));
    baseline.getVariable(variable).resize(1,baseline.getNNodes());
    baseline.getVariable(variable).setValue(0,0,1.25);
    static_cast<RModel &>(live) = baseline;
    live.setFileName("retained-document.rbm");
    const QString before = fingerprint(live), after = fingerprint(generated);
    QString error;
    QVERIFY2(live.commitPiezoExperimentWithHistory(generated,before,after,10,"Complete experiment",&error),qPrintable(error));
    QCOMPARE(live.getNVariables(),0u);
    QCOMPARE(live.getUndoStackSize(),1u);
    QVERIFY(live.hasPiezoExperimentHistory(true));
    const auto identity = live.piezoExperimentHistoryIdentity(true);
    QString preview;
    QVERIFY2(live.previewPiezoExperimentHistory(true,&preview,&error),qPrintable(error));
    QCOMPARE(preview,before);
    QVERIFY(!live.applyPiezoExperimentHistory(true,"stale-uuid",preview,&error));
    QVERIFY(!live.applyPiezoExperimentHistory(true,identity,QString(64,'b'),&error));
    QVERIFY(!live.applyPiezoExperimentHistory(true,identity,preview,&error,[](const RModel &) { return false; }));
    QCOMPARE(fingerprint(live),after);
    QCOMPARE(live.getUndoStackSize(),1u);
    QCOMPARE(live.getRedoStackSize(),0u);
    QVERIFY2(live.applyPiezoExperimentHistory(true,identity,preview,&error),qPrintable(error));
    QCOMPARE(fingerprint(live),before);
    QCOMPARE(live.getNVariables(),1u);
    QCOMPARE(live.getVariable(0).getValue(0,0),1.25);
    QCOMPARE(live.getProblemSetup().getStructuralDynamicsSetup().getTimeStep(),.07);
    QCOMPARE(live.getTimeSolver().getInputStartTime(),.4);
    QCOMPARE(live.getFileName(),QString("retained-document.rbm"));
    QVERIFY(live.hasPiezoExperimentHistory(false));
    QCOMPARE(live.piezoExperimentHistoryIdentity(false),identity);
    live.redo(0);
    QCOMPARE(fingerprint(live),after);
    live.getProblemSetup().getStructuralPiezoLayeredSetup().controllerConfiguration = "changed-source";
    const auto dirty = fingerprint(live);
    QVERIFY(!live.previewPiezoExperimentHistory(true,&preview,&error));
    QVERIFY(!live.applyPiezoExperimentHistory(true,identity,before,&error));
    QCOMPARE(fingerprint(live),dirty);
    QCOMPARE(live.getUndoStackSize(),1u);
}

void TestPiezoExperimentHistory::generatedIdentityAndBarriers()
{
    Model live;
    live.storeCurentVersion(10,"Earlier ordinary operation");
    const auto generated = fixture();
    QString error;
    const auto empty = fingerprint(live);
    QVERIFY2(live.commitPiezoExperimentWithHistory(generated,empty,fingerprint(generated),10,"Generated experiment",&error),qPrintable(error));
    QVERIFY_EXCEPTION_THROWN(live.undo(1),RError);
    QCOMPARE(live.getNNodes(),2u);
    live.undo(0);
    QCOMPARE(live.getNNodes(),0u);
    live.redo(0);
    QCOMPARE(live.getNodePhysicalId(1),generated.getNodePhysicalId(1));
    QCOMPARE(live.getElementPhysicalId(0),generated.getElementPhysicalId(0));
    QCOMPARE(live.getLine(0).getPhysicalId(),generated.getLine(0).getPhysicalId());
    live.undo(0);
    const uint n = live.getNNodes(); live.addNode(RNode(3,0,0));
    QVERIFY(live.getNodePhysicalId(n) > generated.getNodePhysicalId(1));
    const uint group = live.getNLines(); live.addLine(RLine());
    QVERIFY(live.getLine(group).getPhysicalId() > generated.getLine(0).getPhysicalId());
    const uint e = live.getNElements(); live.addElement(RElement(R_ELEMENT_POINT));
    QVERIFY(live.getElementPhysicalId(e) > generated.getElementPhysicalId(0));
}

void TestPiezoExperimentHistory::failedCommitRetainsStacks()
{
    Model live;
    live.storeCurentVersion(10,"Existing undo");
    const auto generated = fixture();
    const auto before = fingerprint(live);
    QString error;
    QVERIFY(!live.commitPiezoExperimentWithHistory(generated,before,fingerprint(generated),10,"Experiment",&error,
        [](const RModel &) { throw std::bad_alloc(); return false; }));
    QCOMPARE(fingerprint(live),before);
    QCOMPARE(live.getUndoStackSize(),1u);
    QCOMPARE(live.getRedoStackSize(),0u);
    QVERIFY(!live.hasPiezoExperimentHistory(true));
    QVERIFY(!live.commitPiezoExperimentWithHistory(generated,"stale",fingerprint(generated),10,"Experiment",&error));
    QCOMPARE(fingerprint(live),before);
    QCOMPARE(live.getUndoStackSize(),1u);
}

int runPiezoExperimentHistoryTests(int argc, char **argv)
{
    TestPiezoExperimentHistory test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_piezo_experiment_history.moc"
