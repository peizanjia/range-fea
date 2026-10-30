#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <stdexcept>
#include <rml_model.h>
#include <rbl_error.h>
#include <rpiezo_experiment_service.h>
#include <piezo_axial_backend_bridge.h>
#include <piezo_native_backend_bridge.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include "native_accepted_checkpoint.h"

namespace {
using Store=NativeAcceptedCheckpointStore;
struct ExactModelWriter : RModel {explicit ExactModelWriter(const RModel &m):RModel(m){} using RModel::writeBinary;};
QByteArray digest(const QByteArray &b){return QCryptographicHash::hash(b,QCryptographicHash::Sha256);}
QString fileHash(const QString &path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Test input missing");return QString::fromLatin1(digest(f.readAll()).toHex());}
RModel source(bool axial){RModel m;for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<2;++i){RElement e(axial?R_ELEMENT_TRUSS1:R_ELEMENT_BEAM1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));m.getTimeSolver().setInputStartTime(.001);
    auto recipe=RPiezoExperimentService::exampleRecipe(m,axial?"axialRod":"planarBeam");recipe["replaceLegacyConditions"]=true;recipe["steps"]=12;
    recipe["controller"]=QJsonObject{{"kind","PI"},{"kp",.3},{"ki",.02},{"selectedOutputs",QJsonArray{"tip_displacement"}},{"bias",QJsonArray{.3}},{"controlEveryFESteps",3}};
    const auto compiled=RPiezoExperimentService::preview(m,recipe);if(!compiled.valid)throw std::runtime_error(qPrintable(compiled.message));return compiled.candidate;
}
template<class Run,class Codec,class Callbacks,class Payload,class Time>
void genuineTrace(const RModel &model,const Callbacks &callbacks,const QString &family,Payload modelPayload,Time physicalTime,bool mutateNonDue=false)
{
    QTemporaryDir dir;QVERIFY(dir.isValid());const QString input=dir.filePath("input.rbm");ExactModelWriter(model).writeBinary(input);
    auto frozen=std::make_shared<Run>(model,callbacks);Run producer(model,callbacks);
    const auto &initial=frozen->acceptedRuntime();const auto configuration=initial.configurationIdentity;
    const auto backend=initial.backendIdentity;
    Store::TraceOptions options;options.enabled=true;options.header.family=family;options.header.sourceFingerprint=initial.sourceFingerprint;
    options.header.configurationIdentity=QString::fromLatin1(configuration.toHex());options.header.backendIdentity=backend;
    options.header.baseSeq=initial.completedFE;options.header.targetSteps=initial.targetSteps;options.header.initialTime=initial.initialTime;options.header.dt=initial.dt;
    const auto extract=[frozen,modelPayload](const RModel &m){frozen->validateCurrentSource(m);const auto bytes=modelPayload(m);
        return bytes.isEmpty()?Codec::encode(frozen->acceptedRuntime()):bytes;};
    const auto certify=[frozen,physicalTime](const QByteArray &bytes){const auto state=Codec::decode(bytes);frozen->certifyAcceptedRuntime(state);
        return NativeAcceptedCheckpointStateView{state.completedFE,physicalTime(state),state.dt,digest(Codec::encode(state))};};
    const auto view=[extract,certify](const RModel &m){return certify(extract(m));};
    options.extractPayload=extract;options.certifyPayload=certify;
    options.rebuildAcceptedModel=[frozen](const RModel &m,const QByteArray &b){return frozen->modelForAcceptedRuntime(m,Codec::decode(b));};
    const QString inputSha=fileHash(input),controlSha=QString::fromLatin1(digest(callbacks.backendIdentity.toUtf8()).toHex()),runtimeSha=QString(64,'c');
    const QByteArray context=QJsonDocument(QJsonObject{{"schema","range.native-checkpoint-context/v1"},{"sourceSha256",inputSha},
        {"modelSha256",inputSha},{"controlSha256",controlSha},{"runtimeSha256",runtimeSha},{"packageSha256",QString(64,'d')},
        {"policy","accepted-steps-or-sim-time-v1"},{"maxAcceptedSteps",10},{"maxSimulationSeconds",1.},{"dtS",initial.dt}}).toJson(QJsonDocument::Compact);
    const QString checkpointDir=dir.filePath("checkpoints");auto store=std::make_unique<Store>(checkpointDir,context,inputSha,controlSha,runtimeSha,false,model,view,options);
    const auto baseline=Codec::encode(frozen->acceptedRuntime());RModel stepFive;QByteArray fivePayload;
    if(mutateNonDue){
        const auto base=store->readLatest();const QString pointerPath=QDir(checkpointDir).filePath("latest.json");
        QFile pointer(pointerPath);QVERIFY(pointer.open(QIODevice::ReadOnly));const auto oldPointer=pointer.readAll();pointer.close();
        const auto beforeHash=fileHash(base.modelPath);bool changed=false;auto step=producer.prepare(model);producer.validatePrepared(*step);
        auto publication=store->stage(step->candidateModel(),step->identity());QVERIFY(publication);
        QCOMPARE(store->committedReceipt()->modelSequence,quint64(0));
        store->setPhaseHook(Store::Phase::AfterLatest,[&](Store::Phase){QFile file(base.modelPath);
            if(!file.open(QIODevice::ReadWrite))throw std::runtime_error("Immutable MODEL mutation denied by pin");
            const auto original=file.read(1);if(original.size()!=1 || !file.seek(0) || file.write(QByteArray(1,char(original[0]^1)))!=1 || !file.flush())
                throw std::runtime_error("Immutable MODEL test mutation failed");changed=true;});
        QVERIFY_EXCEPTION_THROWN(publication->publish(),std::exception);const bool restored=publication->rollback();publication.reset();
        QCOMPARE(Codec::encode(producer.acceptedRuntime()),baseline);QCOMPARE(Codec::encode(frozen->acceptedRuntime()),baseline);
        QVERIFY(pointer.open(QIODevice::ReadOnly));QCOMPARE(pointer.readAll(),oldPointer);pointer.close();
#ifdef Q_OS_WIN
        QVERIFY(!changed);QVERIFY(restored);QVERIFY(store->isRetryable());QCOMPARE(fileHash(base.modelPath),beforeHash);
        QCOMPARE(store->readLatest().envelope["acceptedSeq"].toInt(),0);QVERIFY(!QFileInfo::exists(QDir(checkpointDir).filePath("pending-publication.json")));
#else
        QVERIFY(changed);QVERIFY(!restored);QVERIFY(!store->isRetryable());QVERIFY(QFileInfo::exists(QDir(checkpointDir).filePath("pending-publication.json")));
        QVERIFY_EXCEPTION_THROWN(store->readLatest(),std::exception);
#endif
        return;
    }
    for(uint i=1;i<=5;++i){auto step=producer.prepare(model);producer.validatePrepared(*step);
        auto publication=store->stage(step->candidateModel(),step->identity());publication->publish();producer.commit(*step);publication->commit();
        QCOMPARE(store->committedReceipt()->acceptedRuntime,Codec::encode(producer.acceptedRuntime()));
    }
    const auto five=store->readLatest();QCOMPARE(five.modelSequence,quint64(0));QCOMPARE(five.envelope["acceptedSeq"].toInt(),5);QVERIFY(five.acceptedModel);
    fivePayload=five.acceptedRuntime;stepFive=*five.acceptedModel;QCOMPARE(Codec::encode(frozen->certifyAcceptedRuntime(stepFive)),fivePayload);
    QCOMPARE(Codec::encode(frozen->acceptedRuntime()),baseline);
    // Reopen from the genuinely older periodic MODEL as source. Authority and
    // the resumed producer come from terminal runtime 5, not that MODEL's 0.
    RModel fullSource;fullSource.read(five.modelPath);store.reset();
    store=std::make_unique<Store>(checkpointDir,context,inputSha,controlSha,runtimeSha,true,fullSource,view,options);
    QCOMPARE(store->committedReceipt()->envelope["acceptedSeq"].toInt(),5);QCOMPARE(store->readLatest().acceptedRuntime,fivePayload);
    store.reset();store=std::make_unique<Store>(checkpointDir,context,inputSha,controlSha,runtimeSha,true,stepFive,view,options);
    QCOMPARE(store->readLatest().acceptedRuntime,fivePayload);
    stepFive.getProblemSetup().setRestart(true);Run resumed(stepFive,callbacks);
    for(uint i=6;i<=12;++i){auto reference=producer.prepare(model);producer.validatePrepared(*reference);producer.commit(*reference);
        auto step=resumed.prepare(stepFive);resumed.validatePrepared(*step);auto publication=store->stage(step->candidateModel(),step->identity());
        publication->publish();resumed.commit(*step);publication->commit();QCOMPARE(Codec::encode(resumed.acceptedRuntime()),Codec::encode(producer.acceptedRuntime()));}
    const auto final=store->readLatest();QCOMPARE(final.modelSequence,quint64(12));QCOMPARE(final.acceptedRuntime,Codec::encode(producer.acceptedRuntime()));
    QCOMPARE(Codec::encode(frozen->certifyAcceptedRuntime(*final.acceptedModel)),final.acceptedRuntime);
    QFile trace(QDir(checkpointDir).filePath("accepted-trace.rpt"));QVERIFY(trace.open(QIODevice::ReadOnly));
    const auto prefix=PiezoAcceptedTrace::verifyPrefix(trace,final.traceCursor);QCOMPARE(prefix.records,quint64(13));
    QCOMPARE(prefix.header.sourceFingerprint,initial.sourceFingerprint);QCOMPARE(prefix.header.backendIdentity,backend);
    QCOMPARE(prefix.header.configurationIdentity,QString::fromLatin1(configuration.toHex()));QCOMPARE(prefix.terminal.payload,final.acceptedRuntime);
}
}
class TestNativePiezoAcceptedTrace:public QObject {
    Q_OBJECT
private slots:
    void completeActualRuntimeAndOlderModelRestore_data(){QTest::addColumn<bool>("axial");QTest::newRow("actual-axial-PI")<<true;QTest::newRow("actual-layered-PI")<<false;}
    void completeActualRuntimeAndOlderModelRestore(){try{
        QFETCH(bool,axial);const auto model=source(axial);
        if(axial){const auto b=PiezoAxialBackendBridge::create(model);
            genuineTrace<RStructuralPiezoAxialNativeRun,RStructuralPiezoAxialRestartCodec>(model,b.callbacks,"axial",
                [](const RModel &m){return m.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState;},[](const auto &s){return s.physical.time;});
        }else {const auto b=PiezoNativeBackendBridge::create(model);
            genuineTrace<RStructuralPiezoLayeredNativeRun,RStructuralPiezoLayeredRestartCodec>(model,b.callbacks,"layered",
                [](const RModel &m){return m.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes;},[](const auto &s){return s.physical.integrator.state.time;});}
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}}
    void actualNonDueImmutableModelMutation_data(){QTest::addColumn<bool>("axial");QTest::newRow("actual-axial-seq1")<<true;QTest::newRow("actual-layered-seq1")<<false;}
    void actualNonDueImmutableModelMutation(){try{
        QFETCH(bool,axial);const auto model=source(axial);
        if(axial){const auto b=PiezoAxialBackendBridge::create(model);
            genuineTrace<RStructuralPiezoAxialNativeRun,RStructuralPiezoAxialRestartCodec>(model,b.callbacks,"axial",
                [](const RModel &m){return m.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState;},[](const auto &s){return s.physical.time;},true);
        }else {const auto b=PiezoNativeBackendBridge::create(model);
            genuineTrace<RStructuralPiezoLayeredNativeRun,RStructuralPiezoLayeredRestartCodec>(model,b.callbacks,"layered",
                [](const RModel &m){return m.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes;},[](const auto &s){return s.physical.integrator.state.time;},true);}
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}}
};
QTEST_GUILESS_MAIN(TestNativePiezoAcceptedTrace)
#include "tst_native_piezo_accepted_trace.moc"
