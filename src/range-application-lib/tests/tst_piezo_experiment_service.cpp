#include <QtTest>
#include <QTemporaryDir>
#include <rpiezo_experiment_service.h>
#include <rstructural_piezo_layered_model_adapter.h>
#include <limits>
#include <rbl_error.h>

namespace {
RModel source() {
    RModel m;for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<2;++i){RElement e(R_ELEMENT_BEAM1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));return m;
}
QJsonObject recipe(const RModel &m) {auto r=RPiezoExperimentService::exampleRecipe(m);r["replaceLegacyConditions"]=true;return r;}
}
class TestPiezoExperimentService:public QObject {
    Q_OBJECT
private slots:
    void actualSourceAndMass();void selectedContinuousElectrodes();void legacyReplacement();void invalid_data();void invalid();
    void templates_data();void templates();void binding();void persistence_data();void persistence();void sourceRecipeRoundtrip();
};
void TestPiezoExperimentService::actualSourceAndMass() {
    auto m=source();auto r=recipe(m);auto p=RPiezoExperimentService::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));
    QCOMPARE(m.getProblemSetup().getStructuralPiezoLayeredSetup().enabled,false);QCOMPARE(p.candidate.getNNodes(),m.getNNodes());QCOMPARE(p.candidate.getNElements(),m.getNElements());
    const auto &s=p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup();RStructuralPiezoLayeredModelAdapter a(p.candidate,s);
    // Two 1 m hosts, width .1 m, .01 m thickness plus two .0005 m piezo layers.
    QVERIFY(std::abs(a.mesh().operators.totalMass-6.9)<1e-10);QCOMPARE(p.report["totalMass"].toDouble(),a.mesh().operators.totalMass);
    QCOMPARE(s.members.size(),size_t(2));QCOMPARE(s.members[0].layers.size(),size_t(3));QCOMPARE(s.ports.size(),size_t(2));
    QCOMPARE(p.candidate.getTimeSolver().getInputStartTime(),m.getTimeSolver().getInputStartTime());QCOMPARE(p.candidate.getProblemTaskTree().getNIterations(),1u);
}
void TestPiezoExperimentService::selectedContinuousElectrodes() {
    auto m=source();auto r=RPiezoExperimentService::exampleRecipe(m,{},QJsonArray{501});r["replaceLegacyConditions"]=true;auto p=RPiezoExperimentService::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));auto &s=p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup();QCOMPARE(s.members[0].layers.size(),size_t(3));QCOMPARE(s.members[1].layers.size(),size_t(1));
    auto all=RPiezoExperimentService::preview(m,recipe(m));QVERIFY(all.valid);const auto &a=all.candidate.getProblemSetup().getStructuralPiezoLayeredSetup();QCOMPARE(a.members[0].layers[1].portId,a.members[1].layers[1].portId);QCOMPARE(a.members[0].layers[2].portId,a.members[1].layers[2].portId);
    QVERIFY(RPiezoExperimentService::exampleRecipe(m,{},QJsonArray{999}).isEmpty());
}
void TestPiezoExperimentService::legacyReplacement() {
    auto m=source();auto r=RPiezoExperimentService::exampleRecipe(m);auto bad=RPiezoExperimentService::preview(m,r);QVERIFY(!bad.valid);QVERIFY(bad.message.contains("replaceLegacyConditions"));
    r["replaceLegacyConditions"]=true;auto good=RPiezoExperimentService::preview(m,r);QVERIFY2(good.valid,qPrintable(good.message));QVERIFY(!good.report["removedLegacyConditions"].toArray().isEmpty());QVERIFY(good.report["legacyConditionPolicy"].toString().contains("REPLACED"));QCOMPARE(good.candidate.getLine(0).getNEnvironmentConditions(),0u);QVERIFY(m.getLine(0).getNEnvironmentConditions()>0);
}
void TestPiezoExperimentService::invalid_data() {
    QTest::addColumn<int>("mutation");const char *names[]={"unknown-field","wrong-unit","nonfinite","short-tensor","unknown-port","duplicate-host","unsupported-source","unknown-output","empty-grid","template-overwrite"};for(int i=0;i<10;++i)QTest::newRow(names[i])<<i;
}
void TestPiezoExperimentService::invalid() {
    QFETCH(int,mutation);auto m=source();auto r=recipe(m);switch(mutation){case 0:r["typo"]=1;break;case 1:{auto u=r["units"].toObject();u["length"]="mm";r["units"]=u;break;}case 2:r["dt"]=std::numeric_limits<double>::infinity();break;case 3:{auto a=r["materials"].toArray();auto o=a[0].toObject();o["elasticityOrCompliance"]=QJsonArray{1};a[0]=o;r["materials"]=a;break;}case 4:{auto a=r["members"].toArray();auto o=a[0].toObject();auto ls=o["layers"].toArray();auto l=ls[1].toObject();l["portId"]="missing";ls[1]=l;o["layers"]=ls;a[0]=o;r["members"]=a;break;}case 5:{auto a=r["members"].toArray();a.append(a[0]);r["members"]=a;break;}case 6:m.getElement(0).setType(R_ELEMENT_TRUSS1);break;case 7:{auto d=r["design"].toObject();auto a=d["outputs"].toArray();auto o=a[0].toObject();o["physicalNodeId"]=999;a[0]=o;d["outputs"]=a;r["design"]=d;break;}case 8:r["steps"]=0;break;case 9:r["template"]=QJsonObject{{"kind","cantileverBeam"},{"length",1.},{"width",.1},{"thickness",.01},{"elements",2}};break;}
    auto p=RPiezoExperimentService::preview(m,r);QVERIFY(!p.valid);QVERIFY(!p.diagnostics.isEmpty());QCOMPARE(p.candidate.getNNodes(),m.getNNodes());QCOMPARE(p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup().enabled,false);QCOMPARE(m.getNElements(),2u);
}
void TestPiezoExperimentService::templates_data() {QTest::addColumn<QString>("family");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");}
void TestPiezoExperimentService::templates() {
    QFETCH(QString,family);RModel m;auto r=RPiezoExperimentService::exampleRecipe(m,family);QVERIFY(!r.isEmpty());r["replaceLegacyConditions"]=true;auto p=RPiezoExperimentService::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));QCOMPARE(m.getNNodes(),0u);QCOMPARE(p.candidate.getNElements(),4u);for(uint i=0;i<4;++i)QCOMPARE(p.candidate.getElement(i).getType(),family=="flatPlate"?R_ELEMENT_QUAD1:R_ELEMENT_BEAM1);
    auto &s=p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup();for(auto &mat:s.materials){QCOMPARE(mat.sourceKind,RStructuralPiezoMaterialSourceKind::Example);QVERIFY(mat.notes.contains("Synthetic SI"));QCOMPARE(mat.sourceSha256.size(),64);}RStructuralPiezoLayeredModelAdapter compiled(p.candidate,s);QVERIFY(compiled.mesh().operators.totalMass>0);
}
void TestPiezoExperimentService::binding() {auto m=source();auto p=RPiezoExperimentService::preview(m,recipe(m));QVERIFY(p.valid);auto s=p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup();s.validate(p.candidate);p.candidate.getNode(2).setX(2.1);QVERIFY_EXCEPTION_THROWN(s.validate(p.candidate),RError);}
void TestPiezoExperimentService::persistence_data() {QTest::addColumn<bool>("binary");QTest::newRow("RAM")<<false;QTest::newRow("RBM")<<true;}
void TestPiezoExperimentService::persistence() {QFETCH(bool,binary);QTemporaryDir dir;QVERIFY(dir.isValid());auto m=source();auto p=RPiezoExperimentService::preview(m,recipe(m));QVERIFY(p.valid);auto path=dir.filePath("experiment."+RModel::getDefaultFileExtension(binary));p.candidate.write(path,false);RModel restored;restored.read(path);const auto &s=restored.getProblemSetup().getStructuralPiezoLayeredSetup();s.validate(restored);QCOMPARE(s.sourceFingerprint,p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup().sourceFingerprint);RStructuralPiezoLayeredModelAdapter a(restored,s);QVERIFY(std::abs(a.mesh().operators.totalMass-6.9)<1e-10);}
void TestPiezoExperimentService::sourceRecipeRoundtrip() {auto m=source();auto p=RPiezoExperimentService::preview(m,recipe(m));QVERIFY(p.valid);auto r=RPiezoExperimentService::recipeFromModel(p.candidate);QVERIFY(!r.isEmpty());auto again=RPiezoExperimentService::preview(p.candidate,r);QVERIFY2(again.valid,qPrintable(again.message));QCOMPARE(again.candidate.getProblemSetup().getStructuralPiezoLayeredSetup().identityBytes(),p.candidate.getProblemSetup().getStructuralPiezoLayeredSetup().identityBytes());}
QTEST_APPLESS_MAIN(TestPiezoExperimentService)
#include "tst_piezo_experiment_service.moc"
