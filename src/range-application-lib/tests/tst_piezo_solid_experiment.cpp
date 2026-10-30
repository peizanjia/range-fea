#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <rpiezo_experiment_service.h>
#include <piezo_native_backend_bridge.h>
#include <rstructural_piezo_layered_model_adapter.h>
#include <rstructural_piezo_layered_native_run.h>
#include <rstructural_piezo_layered_restart_codec.h>

class SolidExperimentTest:public QObject {
    Q_OBJECT
private slots:
    void templateAndControlledRun_data(){QTest::addColumn<QString>("kind");QTest::addColumn<bool>("external");QTest::newRow("hex8")<<QString("solidHex")<<false;QTest::newRow("tet4")<<QString("solidTet")<<false;QTest::newRow("hex8-external")<<QString("solidHex")<<true;QTest::newRow("tet4-external")<<QString("solidTet")<<true;}
    void templateAndControlledRun(){try{
        QFETCH(QString,kind);QFETCH(bool,external);RModel empty;
        QJsonObject parameters{{"kind",kind},{"length",.2},{"width",.04},{"thickness",.01},{"patchThickness",.001},{"elements",2}};
        auto mesh=RPiezoExperimentService::templateCandidate(empty,parameters);QCOMPARE(mesh.getNNodes(),18u);QCOMPARE(mesh.getNElements(),kind=="solidHex"?4u:24u);
        auto recipe=RPiezoExperimentService::exampleRecipe(mesh,"solid3D");QVERIFY(!recipe.isEmpty());QCOMPARE(recipe.value("family").toString(),QString("solid3D"));
        recipe["replaceLegacyConditions"]=true;
        auto recipePorts=recipe.value("ports").toArray();auto drive=recipePorts.first().toObject();drive["initialValue"]=.25;recipePorts[0]=drive;recipe["ports"]=recipePorts;
        recipe["dt"]=1e-5;recipe["steps"]=4;
        const auto output=recipe["design"].toObject()["outputs"].toArray()[0].toObject();
        recipe["initialDisplacements"]=QJsonArray{QJsonObject{{"physicalNodeId",output["physicalNodeId"]},{"values",QJsonArray{0.,0.,1e-8,0.,0.,0.}}}};
        recipe["controller"]=QJsonObject{{"kind","velocity_feedback"},{"stateSource","sensor_only"},{"selectedOutputs",QJsonArray{"tip_velocity"}},{"gain",1.},{"voltageLower",QJsonArray{-10.}},{"voltageUpper",QJsonArray{10.}},{"deltaVoltageLimits",QJsonArray{1.}}};
        if(external){
            const auto program=QFileInfo(QDir(QStringLiteral(RANGE_PIEZO_BACKEND_MODULE_ROOT)).filePath("../../fea-solver/examples/external_io/pure_provider.py")).canonicalFilePath();QVERIFY(!program.isEmpty());
            const auto sdk=QFileInfo(QFileInfo(program).dir().filePath("range_external_sdk.py")).canonicalFilePath();
            QJsonObject connection{{"transport","stdio"},{"executable",QStringLiteral(RANGE_PIEZO_BACKEND_PYTHON)},{"args",QJsonArray{"-B","-u",program}},{"dependencies",QJsonArray{sdk}},{"algorithm",QJsonObject{{"id","range-example-pure-provider"},{"version","1"}}},{"requestTimeout",5.},{"handshakeTimeout",10.}};
            recipe["controller"]=QJsonObject{{"kind","external"},{"stateSource","sensor_only"},{"selectedOutputs",QJsonArray{"drive"}},{"controlEveryFESteps",2},{"voltageLower",QJsonArray{-10.}},{"voltageUpper",QJsonArray{10.}},{"deltaVoltageLimits",QJsonArray{1.}},{"external",QJsonObject{{"connection",connection},{"initialState",QJsonObject{{"integral",0.}}}}}};
            recipe["excitation"]=QJsonObject{{"channels",QJsonArray{}},{"externalSources",QJsonArray{QJsonObject{{"connection",connection},{"channelIds",QJsonArray{QString("node:%1:local:EZ").arg(output["physicalNodeId"].toDouble(),0,'f',0)}},{"sampleEveryFESteps",2},{"initialState",QJsonObject{{"samples","0"}}}}}}};
        }
        auto preview=RPiezoExperimentService::preview(mesh,recipe);QVERIFY2(preview.valid,qPrintable(preview.message));
        auto model=preview.candidate;auto bridge=PiezoNativeBackendBridge::create(model);
        bool localTranslation=false;for(const auto &value:bridge.exportPackage.value("forceCatalog").toArray()){
            const auto binding=value.toObject();if(binding.value("coordinateSystem")==QJsonValue("local")){
                QVERIFY(binding.value("dof").toString()=="EX"||binding.value("dof").toString()=="EY"||binding.value("dof").toString()=="EZ");
                if(binding.value("dof")==QJsonValue("EZ"))localTranslation=true;
            }
        }QVERIFY(localTranslation);
        RStructuralPiezoLayeredNativeRun run(model,bridge.callbacks);
        QCOMPARE(run.adapter().mesh().operators.dofs,3u*mesh.getNNodes());
        while(run.hasRemainingSteps()){auto step=run.prepare(model,true);QVERIFY(step);run.validatePrepared(*step);model=step->candidateModel();run.commit(*step);}
        QCOMPARE(run.completedSteps(),4u);QVERIFY(std::isfinite(run.acceptedState().integrator.state.time));
        auto restored=run.acceptedModel(model);restored.getProblemSetup().setRestart(true);auto resumedBridge=PiezoNativeBackendBridge::create(restored);RStructuralPiezoLayeredNativeRun resumed(restored,resumedBridge.callbacks);QCOMPARE(resumed.completedSteps(),4u);
        QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime()),RStructuralPiezoLayeredRestartCodec::encode(resumed.acceptedRuntime()));
    }catch(const std::exception &error){QFAIL(error.what());}}
    void defaultHexTemplateReducedOperatorsAreSymmetric(){
        RModel empty;const QJsonObject parameters{{"kind","solidHex"},{"length",1.},{"width",.1},{"thickness",.01},{"patchThickness",.0005},{"elements",4}};
        const auto mesh=RPiezoExperimentService::templateCandidate(empty,parameters);auto recipe=RPiezoExperimentService::exampleRecipe(mesh,"solid3D");QVERIFY(!recipe.isEmpty());recipe["replaceLegacyConditions"]=true;
        const auto preview=RPiezoExperimentService::preview(mesh,recipe);QVERIFY2(preview.valid,qPrintable(preview.message));const auto model=preview.candidate;
        const auto &setup=model.getProblemSetup().getStructuralPiezoLayeredSetup();const RStructuralPiezoLayeredModelAdapter adapter(model,setup);
        const auto &full=adapter.mesh().operators;const auto &condensed=adapter.electrical().condensed();const auto &reduced=adapter.data();
        for(size_t i=0;i<full.dofs;++i)for(size_t j=i+1;j<full.dofs;++j){QCOMPARE(full.stiffness[i*full.dofs+j],full.stiffness[j*full.dofs+i]);QCOMPARE(condensed.stiffness[i*full.dofs+j],condensed.stiffness[j*full.dofs+i]);}
        for(size_t i=0;i<reduced.freeDofs;++i)for(size_t j=i+1;j<reduced.freeDofs;++j)QCOMPARE(reduced.stiffness[i*reduced.freeDofs+j],reduced.stiffness[j*reduced.freeDofs+i]);
        try{const auto bridge=PiezoNativeBackendBridge::create(model);QVERIFY(!bridge.exportPackage.isEmpty());}catch(const std::exception &error){QFAIL(error.what());}
    }
    void badSelectionFails(){RModel empty;QJsonObject parameters{{"kind","solidHex"},{"length",.2},{"width",.04},{"thickness",.01},{"elements",1}};auto mesh=RPiezoExperimentService::templateCandidate(empty,parameters);QVERIFY(RPiezoExperimentService::exampleRecipe(mesh,"solid3D",QJsonArray{999999.}).isEmpty());}
};
QTEST_APPLESS_MAIN(SolidExperimentTest)
#include "tst_piezo_solid_experiment.moc"
