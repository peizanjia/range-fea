#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <rpiezo_experiment_service.h>
#include <rmodel_command_service.h>
#include <piezo_axial_backend_bridge.h>
#include <rstructural_piezo_axial_operators.h>
#include <rstructural_piezo_element_operators.h>
#include <rstructural_piezo_native_readout.h>
#include <rbl_error.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace {
using Service=RPiezoExperimentService;
QJsonObject topology(){return {{"kind","axialTruss"},{"length",1.},{"width",.1},{"thickness",.01},{"elements",4}};}
RModel chain(){return Service::templateCandidate(RModel{},topology());}
QJsonObject example(const RModel &m,const QJsonArray &selection={}){return Service::exampleRecipe(m,"axialRod",selection);}
bool near(double x,double y){return std::abs(x-y)<=2e-10*std::max({1e-30,std::abs(x),std::abs(y)});}
QJsonArray six(const std::array<double,3> &v){return {v[0],v[1],v[2],0.,0.,0.};}
QJsonObject external(uint node){return {{"sourcePath","C:/range-tests/external_force.py"},{"functionName","force"},{"sourceSha256",QString(64,'a')},{"dependencyFiles",QJsonArray{}},{"environment",QJsonObject{{"pythonVersion","3.12.10"},{"numpyVersion","2.2.6"},{"scipyVersion","1.15.3"}}},{"channels",QJsonArray{QJsonObject{{"channelId",QString("node:%1:X").arg(node)},{"unit","N"}}}},{"missingPolicy","error"},{"cacheExactTime",true}};}
RModel network(){
    RModel m;for(const auto &n:{RNode(1,1,1),RNode(0,0,0),RNode(2,0,0),RNode(0,2,0),RNode(0,0,2)})m.addNode(n);
    for(uint i=1;i<5;++i){RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,0);e.setNodeId(1,i);m.addElement(e,true,0);}m.getLine(0).setCrossArea(.001);return m;
}
QJsonObject networkRecipe(const RModel &m){
    auto r=example(m,QJsonArray{double(m.getElementPhysicalId(0))});QJsonArray supports;for(uint i=1;i<5;++i)for(const char *dof:{"UX","UY","UZ"})supports.append(QJsonObject{{"physicalNodeId",double(m.getNodePhysicalId(i))},{"dof",dof},{"value",0.}});r["supports"]=supports;r["replaceLegacyConditions"]=true;return r;
}
}
class TestPiezoAxialExperimentService:public QObject {
    Q_OBJECT
private slots:
    void actualTemplateAndReplacementMass();void selectedRodAndNativePhysics();void independentReadoutsAndController();
    void nodalConditionsAndFreshRun();void sourceAtomicAndStale();void invalid_data();void invalid();
    void arbitraryThreeDimensionalNetwork();void preloadedCableWorkingPoint();void materialGroupConflict();
    void externalDeclarationShape();void persistence_data();void persistence();void productionMaterialCertification();
};

void TestPiezoAxialExperimentService::actualTemplateAndReplacementMass(){
    RModel source;auto r=example(source);QCOMPARE(r["template"].toObject()["kind"].toString(),QString("axialTruss"));QCOMPARE(r["replaceLegacyConditions"].toBool(),false);
    auto preview=Service::preview(source,r);QVERIFY2(preview.valid,qPrintable(preview.message));QCOMPARE(source.getNNodes(),0u);QCOMPARE(source.getNElements(),0u);QCOMPARE(preview.candidate.getNNodes(),5u);
    const auto repeated=Service::preview(source,r);QVERIFY2(repeated.valid,qPrintable(repeated.message));
    QCOMPARE(RModelCommandService::stateFingerprint(repeated.candidate),RModelCommandService::stateFingerprint(preview.candidate));
    uint rods=0;for(uint i=0;i<preview.candidate.getNElements();++i){auto type=preview.candidate.getElement(i).getType();QVERIFY(type==R_ELEMENT_TRUSS1||type==R_ELEMENT_POINT);if(type==R_ELEMENT_TRUSS1)++rods;}QCOMPARE(rods,4u);
    const auto &p=preview.candidate.getProblemSetup();QCOMPARE(p.getMassMatrixType(),RMassMatrixType::Lumped);QCOMPARE(p.getStructuralTrussDynamicsSetup().activeMembers.size(),size_t(4));QVERIFY(!p.getStructuralPiezoLayeredSetup().enabled);
    QVERIFY(near(preview.report["totalMass"].toDouble(),7.5));QCOMPARE(preview.report["unusedMaterialIds"].toArray(),QJsonArray{"host"});
    auto stored=Service::recipeFromModel(preview.candidate);QVERIFY(!stored.isEmpty());QVERIFY(!stored.contains("template"));QCOMPARE(stored["replaceLegacyConditions"].toBool(),false);
    auto again=Service::preview(preview.candidate,stored);QVERIFY2(again.valid,qPrintable(again.message));QCOMPARE(again.candidate.getNNodes(),5u);QCOMPARE(again.candidate.getNElements(),preview.candidate.getNElements());QCOMPARE(again.candidate.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration,p.getStructuralPiezoAxialSetup().recipeConfiguration);
}
void TestPiezoAxialExperimentService::selectedRodAndNativePhysics(){
    auto m=chain();uint selected=m.getElementPhysicalId(1);auto r=example(m,QJsonArray{double(selected)});auto preview=Service::preview(m,r);QVERIFY2(preview.valid,qPrintable(preview.message));auto &p=preview.candidate.getProblemSetup();const auto &truss=p.getStructuralTrussDynamicsSetup();QCOMPARE(truss.activeMembers.size(),size_t(1));QCOMPARE(truss.passiveMembers.size(),size_t(3));QCOMPARE(truss.activeMembers[0].physicalElementId,selected);QCOMPARE(p.getStructuralPiezoReadoutSetup().ports[0].role,RStructuralPiezoPortRole::ActuatorSensor);
    QVERIFY(near(preview.report["totalMass"].toDouble(),3.9));for(uint i=0;i<4;++i){QCOMPARE(preview.candidate.getElementPhysicalId(i),m.getElementPhysicalId(i));QCOMPARE(preview.candidate.getElement(i).getNodeId(0),m.getElement(i).getNodeId(0));QCOMPARE(preview.candidate.getElement(i).getNodeId(1),m.getElement(i).getNodeId(1));}
    auto material=RStructuralPiezoContinuumMaterial::isotropic(7e10,.3,7500);material.piezoelectric=true;material.coupling[0]=.1;material.permittivity={{1e-8,0,0,0,1e-8,0,0,0,1e-8}};const auto expected=rStructuralPiezoCompileAxialMember(material,.001,.25);const auto &actual=truss.activeMembers[0];QVERIFY(near(actual.axialStiffness,expected.k));QVERIFY(near(actual.coupling,expected.g));QVERIFY(near(actual.capacitance,expected.Ce));QVERIFY(near(actual.fixedMass,expected.fixedMass));QCOMPARE(actual.massSource,RStructuralTrussMassSource::FixedMemberMass);
    const auto op=RStructuralPiezoAxialOperators::compile(preview.candidate);QCOMPARE(op.free.dofs,size_t(4));QCOMPARE(op.full.ports,size_t(1));QVERIFY(op.reference.equilibriumCertified);
    const auto idx=preview.candidate.getLine(0).getMaterial().findPosition(RMaterialProperty::ModulusOfElasiticity);QVERIFY(near(preview.candidate.getLine(0).getMaterial().get(idx).get(293.15),7e10));
}
void TestPiezoAxialExperimentService::independentReadoutsAndController(){
    auto m=chain();auto r=example(m);auto ports=r["ports"].toArray();QCOMPARE(ports.size(),4);QCOMPARE(ports[0].toObject()["role"].toString(),QString("actuator"));std::set<QString> ids;for(auto v:ports)QVERIFY(ids.insert(v.toObject()["portId"].toString()).second);for(int i=1;i<4;++i)QCOMPARE(ports[i].toObject()["readout"].toString(),QString("openCircuitVoltage"));
    auto sensor=ports[1].toObject()["portId"].toString();r["controller"]=QJsonObject{{"kind","PI"},{"kp",1.},{"ki",.1},{"selectedOutputs",QJsonArray{sensor}},{"reference",QJsonArray{0.}},{"bias",QJsonArray{0.}}};auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));
    auto op=RStructuralPiezoAxialOperators::compile(p.candidate);QVERIFY(std::any_of(op.full.KfixedCharge.begin(),op.full.KfixedCharge.end(),[](double x){return x>0;}));
    auto bad=r;bad["controller"]=QJsonObject{{"kind","PI"},{"kp",1.},{"ki",.1},{"selectedOutputs",QJsonArray{"missingSensor"}}};QVERIFY(!Service::preview(m,bad).valid);
}
void TestPiezoAxialExperimentService::nodalConditionsAndFreshRun(){
    auto m=chain();auto r=example(m);uint tip=m.getNodePhysicalId(4);r["steps"]=2;r["initialDisplacements"]=QJsonArray{QJsonObject{{"physicalNodeId",double(tip)},{"values",QJsonArray{.001,0.,0.,0.,0.,0.}}}};r["initialVelocities"]=QJsonArray{QJsonObject{{"physicalNodeId",double(tip)},{"values",QJsonArray{.01,0.,0.,0.,0.,0.}}}};
    auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));RStructuralTrussDynamicsModelAdapter adapter(p.candidate);auto state=adapter.sourceInitialState();QVERIFY(near(state.displacements[12],.001));QVERIFY(near(state.velocities[12],.01));auto readings=adapter.piezoReadout().observe(state);QVERIFY(std::any_of(readings.begin(),readings.end(),[](const auto &s){return s.mode==RStructuralPiezoReadoutMode::OpenCircuitVoltage&&std::abs(s.value)>0;}));
    uint initialGroups=0;for(uint i=0;i<p.candidate.getNPoints();++i){const auto &point=p.candidate.getPoint(i);if(point.getNInitialConditions()){++initialGroups;QCOMPARE(point.size(),1u);QCOMPARE(p.candidate.getElement(point.get(0)).getNodeId(0),4u);}}QCOMPARE(initialGroups,1u);
    for(uint step=1;step<=2;++step)state=adapter.trial(state,step-1,0).state;QVERIFY(std::isfinite(state.storedEnergy));QVERIFY(state.time>adapter.initialTime());
    auto withResults=adapter.candidateModel(p.candidate,state,2,0);withResults.getProblemSetup().setStructuralTrussDynamicsRestartData({});auto stored=Service::recipeFromModel(withResults);QVERIFY(!stored.isEmpty());auto fresh=Service::preview(withResults,stored);QVERIFY2(fresh.valid,qPrintable(fresh.message));QVERIFY(!fresh.candidate.getProblemSetup().getRestart());QVERIFY(!fresh.candidate.getProblemSetup().getStructuralTrussDynamicsRestartData().available);RStructuralTrussDynamicsModelAdapter reset(fresh.candidate);QVERIFY(near(reset.sourceInitialState().displacements[12],.001));
}
void TestPiezoAxialExperimentService::sourceAtomicAndStale(){
    auto m=chain();auto r=example(m);auto supports=r["supports"].toArray();supports.append(supports[0]);r["supports"]=supports;auto invalid=Service::preview(m,r);QVERIFY(!invalid.valid);QCOMPARE(invalid.candidate.getNElements(),m.getNElements());QVERIFY(!m.getProblemSetup().getStructuralPiezoAxialSetup().enabled);
    auto valid=Service::preview(m,example(m));QVERIFY(valid.valid);auto stored=Service::recipeFromModel(valid.candidate);valid.candidate.getNode(4).setX(1.01);QVERIFY(Service::recipeFromModel(valid.candidate).isEmpty());QVERIFY(!Service::preview(valid.candidate,stored).valid);
}
void TestPiezoAxialExperimentService::invalid_data(){QTest::addColumn<int>("mutation");const char *names[]={"rotation-support","rotation-field","rotation-output","shared-circuit","missing-rod","unknown-field","hidden-passive-electricity","beam-is-not-rod","one-element-template","legacy-without-consent","unsupported-axis-force","invalid-source-sha","unused-invalid-material"};for(int i=0;i<13;++i)QTest::newRow(names[i])<<i;}
void TestPiezoAxialExperimentService::invalid(){
    QFETCH(int,mutation);auto m=chain();auto r=example(m);switch(mutation){
    case 0:{auto a=r["supports"].toArray();auto o=a[0].toObject();o["dof"]="RX";a[0]=o;r["supports"]=a;break;}
    case 1:r["initialDisplacements"]=QJsonArray{QJsonObject{{"physicalNodeId",double(m.getNodePhysicalId(4))},{"values",QJsonArray{0.,0.,0.,1.,0.,0.}}}};break;
    case 2:{auto d=r["design"].toObject();auto a=d["outputs"].toArray();auto o=a[0].toObject();o["dof"]="RX";o["unit"]="rad";a[0]=o;d["outputs"]=a;r["design"]=d;break;}
    case 3:{auto a=r["members"].toArray();auto o=a[1].toObject();o["portId"]=a[0].toObject()["portId"];a[1]=o;r["members"]=a;break;}
    case 4:{auto a=r["members"].toArray();a.removeLast();r["members"]=a;break;}
    case 5:r["typo"]=1;break;
    case 6:{auto a=r["materials"].toArray();auto o=a[0].toObject();o["coupling"]=a[1].toObject()["coupling"];a[0]=o;r["materials"]=a;break;}
    case 7:m.getElement(0).setType(R_ELEMENT_BEAM1);break;
    case 8:{m=RModel{};r=example(m);auto t=r["template"].toObject();t["elements"]=1;r["template"]=t;break;}
    case 9:m.getLine(0).addBoundaryCondition(RBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));break;
    case 10:r["excitation"]=QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId",QString("node:%1:RX").arg(m.getNodePhysicalId(4))},{"unit","N m"},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",1.}}}}}}}};break;
    case 11:{auto a=r["materials"].toArray();auto o=a[1].toObject();o["sourceSha256"]="missing";a[1]=o;r["materials"]=a;break;}
    case 12:{auto a=r["materials"].toArray();auto o=a[0].toObject();auto c=o["elasticityOrCompliance"].toArray();c[0]=-1.;o["elasticityOrCompliance"]=c;a[0]=o;r["materials"]=a;break;}
    }
    auto p=Service::preview(m,r);QVERIFY(!p.valid);QVERIFY(!p.diagnostics.isEmpty());QCOMPARE(p.candidate.getNNodes(),m.getNNodes());QCOMPARE(p.candidate.getNElements(),m.getNElements());QVERIFY(!m.getProblemSetup().getStructuralPiezoAxialSetup().enabled);
}
void TestPiezoAxialExperimentService::arbitraryThreeDimensionalNetwork(){
    auto m=network();auto r=networkRecipe(m);auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));auto op=RStructuralPiezoAxialOperators::compile(p.candidate);QCOMPARE(op.free.dofs,size_t(3));QVERIFY(op.minimumNormalizedStiffnessPivot>0);for(uint i=0;i<5;++i){QCOMPARE(p.candidate.getNode(i).getX(),m.getNode(i).getX());QCOMPARE(p.candidate.getNode(i).getY(),m.getNode(i).getY());QCOMPARE(p.candidate.getNode(i).getZ(),m.getNode(i).getZ());}
    r["supports"]=QJsonArray{};auto mechanism=Service::preview(m,r);QVERIFY(!mechanism.valid);QVERIFY(!mechanism.message.isEmpty());
}
void TestPiezoAxialExperimentService::preloadedCableWorkingPoint(){
    auto m=network();auto r=networkRecipe(m);auto baseline=Service::preview(m,r);QVERIFY2(baseline.valid,qPrintable(baseline.message));auto op=RStructuralPiezoAxialOperators::compile(baseline.candidate);auto members=op.nativeSystem.members;const uint cable=m.getElementPhysicalId(1);size_t index=0;for(;index<members.size();++index)if(members[index].physicalElementId==cable)break;QVERIFY(index<members.size());auto old=std::get<RStructuralTrussPassiveLaw>(members[index].law);members[index].law=RStructuralTrussCableLaw{10.,old.axialRigidity};RStructuralTrussDynamicsModelAdapter adapter(baseline.candidate);auto inputs=adapter.initialCompleteInputs().memberInputs;inputs[index]=RStructuralTrussCableLength{old.referenceLength};auto assembly=RStructuralTrussAssembly::evaluate(op.nativeSystem.referenceNodes,std::vector<double>(15,0.),members,inputs);
    auto entries=r["members"].toArray();auto entry=entries[1].toObject();entry["kind"]="cable";entry["initialTension"]=10.;entries[1]=entry;r["members"]=entries;QJsonArray nodes;for(uint i=0;i<5;++i){std::array<double,3> f{};if(i==0)for(uint a=0;a<3;++a)f[a]=assembly.internalForces[3*i+a];nodes.append(QJsonObject{{"physicalNodeId",double(m.getNodePhysicalId(i))},{"q0",six({{0,0,0}})},{"f0",six(f)}});if(i==0)r["backgroundForces"]=QJsonArray{QJsonObject{{"physicalNodeId",double(m.getNodePhysicalId(0))},{"values",six(f)}}};}auto port=r["ports"].toArray()[0].toObject()["portId"];r["workpoint"]=QJsonObject{{"available",true},{"nodes",nodes},{"ports",QJsonArray{QJsonObject{{"portId",port},{"V0",0.},{"Q0",0.}}}}};
    auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));auto loaded=RStructuralPiezoAxialOperators::compile(p.candidate);QVERIFY(loaded.reference.explicitWorkpoint);QVERIFY(loaded.reference.equilibriumCertified);QVERIFY(std::any_of(loaded.full.Kgeometric.begin(),loaded.full.Kgeometric.end(),[](double x){return x>0;}));QCOMPARE(p.candidate.getProblemSetup().getStructuralTrussDynamicsSetup().cableMembers[0].initialTension,10.);
}
void TestPiezoAxialExperimentService::materialGroupConflict(){
    auto m=chain();auto r=example(m,QJsonArray{double(m.getElementPhysicalId(0))});auto materials=r["materials"].toArray();auto other=materials[0].toObject();other["materialId"]="differentHost";other["density"]=3000.;materials.append(other);r["materials"]=materials;auto members=r["members"].toArray();auto member=members[2].toObject();member["materialId"]="differentHost";members[2]=member;r["members"]=members;
    auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));QCOMPARE(m.getNLines(),1u);
    QCOMPARE(p.candidate.getNLines(),3u);QCOMPARE(p.candidate.getLine(0).getPhysicalId(),m.getLine(0).getPhysicalId());
    QVERIFY(near(p.report["totalMass"].toDouble(),3.975));QCOMPARE(p.report["automaticLineGrouping"].toArray().size(),3);
    std::set<uint> covered;for(uint i=0;i<p.candidate.getNLines();++i)for(uint j=0;j<p.candidate.getLine(i).size();++j)QVERIFY(covered.insert(p.candidate.getLine(i).get(j)).second);
    QCOMPARE(covered.size(),size_t(4));for(uint i=0;i<4;++i)QCOMPARE(p.candidate.getElementPhysicalId(i),m.getElementPhysicalId(i));
    const auto stored=Service::recipeFromModel(p.candidate);auto again=Service::preview(p.candidate,stored);QVERIFY2(again.valid,qPrintable(again.message));
    QCOMPARE(again.candidate.getNLines(),p.candidate.getNLines());QVERIFY(again.report["automaticLineGrouping"].toArray().isEmpty());
    for(uint i=0;i<p.candidate.getNLines();++i)QCOMPARE(again.candidate.getLine(i).getPhysicalId(),p.candidate.getLine(i).getPhysicalId());
    member=members[3].toObject();member["area"]=.002;members[3]=member;r["members"]=members;auto area=Service::preview(m,r);
    QVERIFY2(area.valid,qPrintable(area.message));QVERIFY(near(area.report["totalMass"].toDouble(),4.65));
    try{const auto backend=PiezoAxialBackendBridge::create(area.candidate);QVERIFY(near(backend.exportPackage["totalMass"].toDouble(),4.65));}
    catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
}
void TestPiezoAxialExperimentService::externalDeclarationShape(){
    auto m=chain();auto r=example(m);auto e=r["excitation"].toObject();auto f=external(m.getNodePhysicalId(4));e["externalFunctions"]=QJsonArray{f};r["excitation"]=e;auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));QCOMPARE(Service::recipeFromModel(p.candidate)["excitation"].toObject(),e);
    for(int mutation=0;mutation<4;++mutation){auto invalid=f;if(mutation==0)invalid["unknown"]=1;if(mutation==1){auto env=f["environment"].toObject();env["unknown"]=1;invalid["environment"]=env;}if(mutation==2){auto channels=f["channels"].toArray();channels.append(channels[0]);invalid["channels"]=channels;}if(mutation==3)invalid["cacheExactTime"]="true";auto changed=e;changed["externalFunctions"]=QJsonArray{invalid};auto bad=r;bad["excitation"]=changed;QVERIFY(!Service::preview(m,bad).valid);}
}
void TestPiezoAxialExperimentService::persistence_data(){QTest::addColumn<bool>("binary");QTest::newRow("RAM")<<false;QTest::newRow("RBM")<<true;}
void TestPiezoAxialExperimentService::persistence(){
    QFETCH(bool,binary);QTemporaryDir dir;QVERIFY(dir.isValid());auto source=chain();auto p=Service::preview(source,example(source));QVERIFY2(p.valid,qPrintable(p.message));auto before=Service::recipeFromModel(p.candidate);auto path=dir.filePath("axial."+RModel::getDefaultFileExtension(binary));p.candidate.write(path,false);RModel restored;restored.read(path);restored.getProblemSetup().getStructuralPiezoAxialSetup().validate(restored);QCOMPARE(Service::recipeFromModel(restored),before);QCOMPARE(restored.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration,p.candidate.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);auto again=Service::preview(restored,before);QVERIFY2(again.valid,qPrintable(again.message));QVERIFY(near(again.report["totalMass"].toDouble(),7.5));
}
void TestPiezoAxialExperimentService::productionMaterialCertification(){
    auto m=chain();auto r=example(m,QJsonArray{double(m.getElementPhysicalId(1))});r["steps"]=2;auto p=Service::preview(m,r);QVERIFY2(p.valid,qPrintable(p.message));
    try{auto backend=PiezoAxialBackendBridge::create(p.candidate);QCOMPARE(backend.exportPackage["schema"].toString(),QString("range.piezo-native-axial-fe-export/v1"));QVERIFY(near(backend.exportPackage["totalMass"].toDouble(),3.9));QCOMPARE(backend.exportPackage["materialQualification"].toString(),QString("declared-continuum-tensors-certified-to-native-effective-laws-and-fixed-mass"));}
    catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
}
QTEST_GUILESS_MAIN(TestPiezoAxialExperimentService)
#include "tst_piezo_axial_experiment_service.moc"
