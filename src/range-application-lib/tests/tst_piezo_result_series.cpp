#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <piezo_result_series.h>
#include <rpiezo_experiment_service.h>
#include <piezo_axial_backend_bridge.h>
#include <piezo_native_backend_bridge.h>
#include <piezo_numeric_transport.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rbl_error.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace {
RModel scene(bool axial)
{
    RModel m;for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<2;++i){RElement e(axial?R_ELEMENT_TRUSS1:R_ELEMENT_BEAM1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));m.getTimeSolver().setInputStartTime(.001);
    auto recipe=RPiezoExperimentService::exampleRecipe(m,axial?"axialRod":"planarBeam");recipe["replaceLegacyConditions"]=true;recipe["steps"]=4;
    recipe["controller"]=QJsonObject{{"kind","PI"},{"kp",.3},{"ki",.02},{"selectedOutputs",QJsonArray{"tip_displacement"}},{"bias",QJsonArray{.3}},{"controlEveryFESteps",3}};
    auto result=RPiezoExperimentService::preview(m,recipe);if(!result.valid)throw std::invalid_argument(qPrintable(result.message));return result.candidate;
}
QStringList records(const RModel &source,const QString &root,bool axial)
{
    QStringList paths;
    const auto save=[&](const RModel &m,uint step){const QString path=QDir(root).filePath(QString("accepted%1.%2").arg(step).arg(step%2?"ram":"rbm"));m.write(path,false);paths.append(m.nativeWritePaths(path,false).first());};
    if(axial){const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun run(source,backend.callbacks);
        save(run.acceptedModel(source),0);for(uint i=1;i<=4;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);save(run.acceptedModel(source),i);}}
    else {const auto backend=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);
        save(run.acceptedModel(source),0);for(uint i=1;i<=4;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);save(run.acceptedModel(source),i);}}
    return paths;
}
const PiezoResultSeries::Signal &find(const PiezoResultSeries::Frame &f,const QString &id)
{
    const auto it=std::find_if(f.channels.begin(),f.channels.end(),[&](const auto &s){return s.channelId==id;});
    if(it==f.channels.end())throw std::invalid_argument("Requested certified signal missing");return *it;
}
template<class Operation> bool rejects(Operation operation)
{
    try{operation();}catch(const RError &){return true;}catch(const std::exception &){return true;}return false;
}
QByteArray bits(double value){return QByteArray(reinterpret_cast<const char *>(&value),sizeof(value));}
QByteArray fileBytes(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Trace fixture file read failed");return file.readAll();}
struct TraceFixture {PiezoResultSeries::TraceProof proof;PiezoAcceptedTrace::Header header;std::vector<QByteArray> payloads;};
TraceFixture compactFixture(const QString &family,const QString &directory,uint baseSeq=0,const QJsonObject &excitation={},uint targetSteps=4)
{
    RModel empty;empty.getTimeSolver().setInputStartTime(.001);auto recipe=RPiezoExperimentService::exampleRecipe(empty,family);recipe["replaceLegacyConditions"]=true;recipe["steps"]=int(targetSteps);
    recipe["controller"]=QJsonObject{{"kind","PI"},{"kp",.3},{"ki",.02},{"selectedOutputs",QJsonArray{"tip_displacement"}},{"bias",QJsonArray{.3}},{"controlEveryFESteps",3}};
    if(!excitation.isEmpty())recipe["excitation"]=excitation;
    const auto composed=RPiezoExperimentService::preview(empty,recipe);if(!composed.valid)throw std::runtime_error(qPrintable(composed.message));const auto &source=composed.candidate;TraceFixture fixture;
    const auto store=[&](auto &run,auto encode){
        for(uint i=0;i<baseSeq;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);}
        auto base=run.acceptedModel(source);const auto path=QDir(directory).filePath(family=="flatPlate"?"base.ram":"base.rbm");base.write(path,false);fixture.proof.baseModelPath=base.nativeWritePaths(path,false).first();
        const auto &state=run.acceptedRuntime();auto &header=fixture.header;header.family=family=="axialRod"?"axial":"layered";header.sourceFingerprint=state.sourceFingerprint;header.configurationIdentity=QString::fromLatin1(state.configurationIdentity.toHex());header.backendIdentity=state.backendIdentity;
        header.baseModelFile=QFileInfo(fixture.proof.baseModelPath).fileName();header.baseModelSha256=QCryptographicHash::hash(fileBytes(fixture.proof.baseModelPath),QCryptographicHash::Sha256);header.contextSha256=QCryptographicHash::hash("actual-reader-test-context",QCryptographicHash::Sha256);
        header.baseSeq=baseSeq;header.targetSteps=state.targetSteps;header.initialTime=state.initialTime;header.dt=state.dt;header.targetTimeGridSha256=PiezoAcceptedTrace::targetTimeGridSha256(header.initialTime,header.dt,header.targetSteps);
        fixture.proof.tracePath=QDir(directory).filePath("accepted.trace");QFile trace(fixture.proof.tracePath);if(!trace.open(QIODevice::ReadWrite|QIODevice::Truncate))throw std::runtime_error("Trace fixture open failed");header=PiezoAcceptedTrace::writeHeader(trace,header);auto cursor=PiezoAcceptedTrace::initialCursor(header);
        for(uint sequence=baseSeq;sequence<=targetSteps;++sequence){if(sequence>baseSeq){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);}const auto bytes=encode(run.acceptedRuntime());fixture.payloads.push_back(bytes);const double time=sequence?header.initialTime+sequence*header.dt:header.initialTime;cursor=PiezoAcceptedTrace::append(trace,header,cursor,sequence,time,bytes);}
        fixture.proof.baseModelSha256=header.baseModelSha256;fixture.proof.expectedHeaderSha256=header.headerSha32;fixture.proof.expectedContextSha256=header.contextSha256;fixture.proof.expectedSourceFingerprint=header.sourceFingerprint;fixture.proof.cursor=cursor;
    };
    if(family=="axialRod"){const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun run(source,backend.callbacks);store(run,[](const auto &state){return RStructuralPiezoAxialRestartCodec::encode(state);});}
    else{const auto backend=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);store(run,[](const auto &state){return RStructuralPiezoLayeredRestartCodec::encode(state);});}
    return fixture;
}
void rewriteTrace(TraceFixture &fixture)
{
    QFile trace(fixture.proof.tracePath);if(!trace.open(QIODevice::ReadWrite|QIODevice::Truncate))throw std::runtime_error("Trace rewrite open failed");fixture.header=PiezoAcceptedTrace::writeHeader(trace,fixture.header);auto cursor=PiezoAcceptedTrace::initialCursor(fixture.header);
    for(size_t i=0;i<fixture.payloads.size();++i){const auto sequence=fixture.header.baseSeq+i;cursor=PiezoAcceptedTrace::append(trace,fixture.header,cursor,sequence,sequence?fixture.header.initialTime+sequence*fixture.header.dt:fixture.header.initialTime,fixture.payloads[i]);}
    fixture.proof.expectedHeaderSha256=fixture.header.headerSha32;fixture.proof.expectedContextSha256=fixture.header.contextSha256;fixture.proof.cursor=cursor;
}
template<class Run,class Codec,class Callbacks,class ChangePhysics,class ChangeRestart,class NodeValue>
void verifyFrozenCertification(const RModel &source,const RModel &last,const Callbacks &callbacks,
                               ChangePhysics changePhysics,ChangeRestart changeRestart,NodeValue nodeValue)
{
    Run frozen(source,callbacks);const Run reference(last,callbacks);
    const auto baseline=Codec::encode(frozen.acceptedRuntime());const auto expected=Codec::encode(reference.acceptedRuntime());
    const auto sourceIdentity=frozen.acceptedRuntime().sourceFingerprint;
    const auto times=last.getTimeSolver().getTimes();const auto computedTime=last.getTimeSolver().getComputedTime();
    const auto inputRestart=last.getProblemSetup().getRestart();
    PiezoResultSeries::Reader reader(source);
    for(int i=0;i<3;++i){
        const auto certified=frozen.certifyAcceptedRuntime(last);QCOMPARE(Codec::encode(certified),expected);
        frozen.certifyAcceptedRuntime(certified);
        const auto frame=reader.certifyAcceptedModel(last);
        QCOMPARE(frame->stateSha256,QString::fromLatin1(QCryptographicHash::hash(expected,QCryptographicHash::Sha256).toHex()));
        QCOMPARE(bits(find(*frame,"node:123:displacement:X").value),bits(nodeValue(reference.acceptedRuntime())));
        QCOMPARE(frame->controlTick,certified.controlTick);QCOMPARE(frame->controlPhase,certified.controlPhase);
        QCOMPARE(Codec::encode(frozen.acceptedRuntime()),baseline);QCOMPARE(frozen.completedSteps(),0u);QVERIFY(frozen.isRetryable());
    }
    const auto certified=frozen.certifyAcceptedRuntime(last);
    auto bad=certified;++bad.controlPhase;QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;bad.opaqueControllerState="malformed opaque state";QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;QVERIFY(!bad.actions.empty());bad.actions.front().requested+=.01;QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;changePhysics(bad);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;bad.sourceFingerprint=QString(64,'a');QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;bad.configurationIdentity=QByteArray(32,'a');QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;bad.backendIdentity+="changed";QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    bad=certified;bad.dt*=2;QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(bad);}));
    auto model=last;model.getNode(1).setX(1.01);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;changeRestart(model);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;model.getTimeSolver().setInputTimeStepSize(2*certified.dt);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;auto wrongGrid=times;wrongGrid.at(1)+=.01;model.getTimeSolver().setTimes(wrongGrid);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;model.getTimeSolver().setComputedTime(computedTime+.01);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;model.getTimeSolver().setCurrentTimeStep(0);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;model.getTimeSolver().setOutputFrequency(last.getTimeSolver().getOutputFrequency()+1);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;model.getTimeSolver().setInputNTimeSteps(certified.targetSteps+1);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    model=last;model.getTimeSolver().setEnabled(false);QVERIFY(rejects([&]{frozen.certifyAcceptedRuntime(model);}));
    QCOMPARE(Codec::encode(frozen.acceptedRuntime()),baseline);QCOMPARE(frozen.completedSteps(),0u);QVERIFY(frozen.isRetryable());
    QCOMPARE(frozen.acceptedRuntime().sourceFingerprint,sourceIdentity);
    QVERIFY(last.getTimeSolver().getTimes()==times);QCOMPARE(last.getTimeSolver().getComputedTime(),computedTime);
    QCOMPARE(last.getProblemSetup().getRestart(),inputRestart);QCOMPARE(Codec::encode(frozen.certifyAcceptedRuntime(last)),expected);
    // Validation leaves the frozen driver usable for its original next transaction.
    auto step=frozen.prepare(source);frozen.validatePrepared(*step);QCOMPARE(step->completedSteps(),1u);
    QCOMPARE(Codec::encode(frozen.acceptedRuntime()),baseline);
}
}

class TestPiezoResultSeries:public QObject
{
    Q_OBJECT
private slots:
    void explicitFormalFactoryAndLegacySourceStaySeparate() { try {
        auto source=scene(false);auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
        setup.excitationConfiguration.clear();setup.controllerConfiguration.clear();setup.designConfiguration.clear();setup.damping={};setup.bindToModel(source);
        RStructuralPiezoLayeredNativeRun legacy(source);const auto backend=PiezoNativeBackendBridge::create(source);
        RStructuralPiezoLayeredNativeRun formal(source,backend.callbacks);const auto legacyBytes=RStructuralPiezoLayeredRestartCodec::encode(legacy.acceptedRuntime());
        const auto formalBytes=RStructuralPiezoLayeredRestartCodec::encode(formal.acceptedRuntime());
        PiezoResultSeries::Reader oldReader(source),traceReader(source,true);
        QCOMPARE(oldReader.certifyAcceptedRuntime(legacyBytes)->backendIdentity,legacy.acceptedRuntime().backendIdentity);
        QCOMPARE(traceReader.certifyAcceptedRuntime(formalBytes)->backendIdentity,formal.acceptedRuntime().backendIdentity);
        QVERIFY(rejects([&]{oldReader.certifyAcceptedRuntime(formalBytes);}));QVERIFY(rejects([&]{traceReader.certifyAcceptedRuntime(legacyBytes);}));
        // Metadata cannot select a provider or replace the factory's complete
        // opaque/physical/source certificate with a copied compatible tuple.
        auto forged=legacy.acceptedRuntime();const auto &identity=formal.acceptedRuntime();forged.backendIdentity=identity.backendIdentity;
        forged.configurationIdentity=identity.configurationIdentity;forged.forcingIdentity=identity.forcingIdentity;forged.controllerIdentity=identity.controllerIdentity;
        QVERIFY(rejects([&]{traceReader.certifyAcceptedRuntime(RStructuralPiezoLayeredRestartCodec::encode(forged));}));
        QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(legacy.acceptedRuntime()),legacyBytes);
        QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(formal.acceptedRuntime()),formalBytes);
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void runtimeIdentityDiagnosticsKeepFullCertification_data(){QTest::addColumn<QString>("field");
        for(const auto &field:{"sourceFingerprint","backendIdentity","configurationIdentity","forcingIdentity","controllerIdentity","dtBits"})QTest::newRow(field)<<QString::fromLatin1(field);}
    void runtimeIdentityDiagnosticsKeepFullCertification() { try {
        QFETCH(QString,field);const auto source=scene(false);const auto backend=PiezoNativeBackendBridge::create(source);
        RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);const auto baseline=RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime());
        PiezoResultSeries::Reader reader(source);auto changed=run.acceptedRuntime();
        if(field=="sourceFingerprint")changed.sourceFingerprint=QString(64,'f');if(field=="backendIdentity")changed.backendIdentity+="-stale";
        if(field=="configurationIdentity")changed.configurationIdentity=QByteArray(32,'f');if(field=="forcingIdentity")changed.forcingIdentity+="-stale";
        if(field=="controllerIdentity")changed.controllerIdentity+="-stale";if(field=="dtBits")changed.dt=std::nextafter(changed.dt,1.);
        QString diagnostic;try{reader.certifyAcceptedRuntime(RStructuralPiezoLayeredRestartCodec::encode(changed));}
        catch(const RError &error){diagnostic=error.getMessage();}catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
        QVERIFY2(diagnostic.contains(field+" expected="),qPrintable(diagnostic));QVERIFY(diagnostic.contains(" actual="));
        QCOMPARE(reader.certifyAcceptedRuntime(baseline)->stateSha256,QString::fromLatin1(QCryptographicHash::hash(baseline,QCryptographicHash::Sha256).toHex()));
        changed=run.acceptedRuntime();changed.opaqueControllerState="malformed";QVERIFY(rejects([&]{reader.certifyAcceptedRuntime(RStructuralPiezoLayeredRestartCodec::encode(changed));}));
        QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime()),baseline);
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void lateExternalSourceOrDependencyChangeRejectsPublication_data(){QTest::addColumn<bool>("dependency");QTest::newRow("source-code")<<false;QTest::newRow("declared-dependency")<<true;}
    void lateExternalSourceOrDependencyChangeRejectsPublication() { try {
        QFETCH(bool,dependency);QTemporaryDir directory;QVERIFY(directory.isValid());const auto sourcePath=directory.filePath("force.py"),dependencyPath=directory.filePath("gain.txt");
        const auto example=RPiezoExperimentService::exampleRecipe(RModel{},"planarBeam");const auto tip=example["design"].toObject()["outputs"].toArray()[0].toObject()["physicalNodeId"].toDouble();const auto channel=QString("node:%1:local:EZ").arg(tip,0,'f',0);
        const auto code=("def force(t):\n    return {'"+channel+"': {'value': 0.001*t, 'unit': 'N'}}\n").toUtf8();const QByteArray declared("declared reference data\n");
        const auto write=[&](const QString &path,const QByteArray &bytes){QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)||file.write(bytes)!=bytes.size())throw std::runtime_error("Temporary external source write failed");};write(sourcePath,code);write(dependencyPath,declared);
        const auto runtime=PiezoNumericTransport::defaultRuntime();PiezoNumericTransport metadataProvider(runtime.first,runtime.second);const auto metadata=metadataProvider.runtimeMetadata();
        const QJsonObject function{{"sourcePath",sourcePath},{"functionName","force"},{"sourceSha256",QString::fromLatin1(QCryptographicHash::hash(code,QCryptographicHash::Sha256).toHex())},
            {"dependencyFiles",QJsonArray{QJsonObject{{"path",dependencyPath},{"sha256",QString::fromLatin1(QCryptographicHash::hash(declared,QCryptographicHash::Sha256).toHex())}}}},
            {"environment",QJsonObject{{"pythonVersion",metadata["python"]},{"numpyVersion",metadata["numpy"]},{"scipyVersion",metadata["scipy"]}}},{"channels",QJsonArray{QJsonObject{{"channelId",channel},{"unit","N"}}}},{"missingPolicy","error"},{"cacheExactTime",true}};
        const auto fixture=compactFixture("planarBeam",directory.path(),0,{{"channels",QJsonArray{}},{"externalFunctions",QJsonArray{function}}});const auto baseBytes=fileBytes(fixture.proof.baseModelPath),traceBytes=fileBytes(fixture.proof.tracePath);
        const auto unchanged=PiezoResultSeries::readAcceptedTrace(fixture.proof);QVERIFY2(unchanged.valid,qPrintable(unchanged.message));uint admitted=0;bool changed=false;
        const auto path=dependency?dependencyPath:sourcePath;const auto original=dependency?declared:code;
        const auto rejected=PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[&](const auto &,const auto &){++admitted;},[&](const auto &){write(path,original+"# changed after all accepted records were certified\n");changed=true;});
        QCOMPARE(admitted,5u);QVERIFY(changed);QVERIFY(!rejected.valid);QVERIFY(!rejected.dataset);QVERIFY2(rejected.message.contains("changed",Qt::CaseInsensitive)||rejected.message.contains("sha",Qt::CaseInsensitive),qPrintable(rejected.message));
        QCOMPARE(fileBytes(fixture.proof.baseModelPath),baseBytes);QCOMPARE(fileBytes(fixture.proof.tracePath),traceBytes);write(path,original);QVERIFY(PiezoResultSeries::readAcceptedTrace(fixture.proof).valid);
        // The MODEL path's final recheck also uses the same frozen provider.
        RModel base;base.read(fixture.proof.baseModelPath);base.getProblemSetup().setRestart(true);PiezoResultSeries::Reader reader(base);const auto accepted=reader.certifyAcceptedModel(base);QVERIFY(accepted);
        write(path,original+"# changed after MODEL certification\n");QVERIFY(rejects([&]{reader.recheckSources();}));QVERIFY(!PiezoResultSeries::readAcceptedRecords({fixture.proof.baseModelPath}).valid);write(path,original);QVERIFY(PiezoResultSeries::readAcceptedRecords({fixture.proof.baseModelPath}).valid);
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void compactTraceMatchesOriginalRuntimeBits_data(){QTest::addColumn<QString>("family");QTest::addColumn<uint>("baseSeq");for(auto family:{"axialRod","planarBeam","flatPlate"})for(uint base:{0u,2u})QTest::newRow(qPrintable(QString("%1-base%2").arg(family).arg(base)))<<QString(family)<<base;}
    void compactTraceMatchesOriginalRuntimeBits() { try {
        QFETCH(QString,family);QFETCH(uint,baseSeq);QTemporaryDir directory;QVERIFY(directory.isValid());const auto fixture=compactFixture(family,directory.path(),baseSeq);const auto read=PiezoResultSeries::readAcceptedTrace(fixture.proof);QVERIFY2(read.valid,qPrintable(read.message));const auto &numeric=*read.dataset;
        QCOMPARE(numeric.family,fixture.header.family);QCOMPARE(numeric.modelFamily,family);QCOMPARE(numeric.baseSeq,quint64(baseSeq));QCOMPARE(numeric.targetSteps,quint64(4));QCOMPARE(numeric.times.size(),qsizetype(5-baseSeq));QCOMPARE(numeric.columns.size(),numeric.channels.size());QCOMPARE(numeric.sourceFingerprint,fixture.header.sourceFingerprint);QCOMPARE(numeric.artifactSha256.size(),64);QVERIFY(numeric.sampledCoverage);
        const auto traceBytes=fileBytes(fixture.proof.tracePath);const auto prefixSha=QCryptographicHash::hash(traceBytes.left(qsizetype(fixture.proof.cursor.byteEnd)),QCryptographicHash::Sha256);
        const auto &cursor=fixture.proof.cursor;const QJsonObject priorWitness{{"schema","range.piezo-accepted-numeric-trace/v1"},{"baseModelSha256",QString::fromLatin1(fixture.proof.baseModelSha256.toHex())},{"headerSha256",QString::fromLatin1(fixture.header.headerSha32.toHex())},{"contextSha256",QString::fromLatin1(fixture.header.contextSha256.toHex())},{"committedPrefixSha256",QString::fromLatin1(prefixSha.toHex())},
            {"sourceFingerprint",fixture.header.sourceFingerprint},{"configurationIdentity",fixture.header.configurationIdentity},{"backendIdentity",fixture.header.backendIdentity},{"cursor",QJsonObject{{"byteEnd",double(cursor.byteEnd)},{"lastRecordOffset",double(cursor.lastRecordOffset)},{"sequence",double(cursor.sequence)},{"recordSha256",QString::fromLatin1(cursor.recordSha32.toHex())},{"stateSha256",QString::fromLatin1(cursor.stateSha32.toHex())}}}};
        QCOMPARE(numeric.artifactSha256,QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(priorWitness).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()));
        RModel base;base.read(fixture.proof.baseModelPath);base.getProblemSetup().setRestart(true);PiezoResultSeries::Reader reader(base);reader.prepareAcceptedRuntimeBatch(fixture.payloads);
        for(size_t i=0;i<fixture.payloads.size();++i){const auto frame=reader.certifyAcceptedRuntime(fixture.payloads[i]);QCOMPARE(frame->family,fixture.header.family);QCOMPARE(frame->modelFamily,family);QCOMPARE(bits(numeric.times[qsizetype(i)]),bits(frame->timeS));QCOMPARE(numeric.completedFE[qsizetype(i)],frame->completedFE);QCOMPARE(numeric.channels.size(),frame->channels.size());
            for(qsizetype c=0;c<frame->channels.size();++c){QCOMPARE(numeric.channels[c].channelId,frame->channels[c].channelId);QCOMPARE(numeric.channels[c].unit,frame->channels[c].unit);QCOMPARE(bits(numeric.columns[c][qsizetype(i)]),bits(frame->channels[c].value));QCOMPARE(numeric.channels[c].value,0.);}
        }reader.recheckSources();
        QStringList chosen{numeric.channels.last().channelId,numeric.channels.first().channelId};const auto subset=PiezoResultSeries::readAcceptedTrace(fixture.proof,chosen);QVERIFY2(subset.valid,qPrintable(subset.message));QCOMPARE(subset.dataset->columns.size(),2);QCOMPARE(subset.dataset->artifactSha256,numeric.artifactSha256);QCOMPARE(subset.dataset->channels.first().channelId,chosen.first());
        uint visited=0;const auto streamed=PiezoResultSeries::streamAcceptedTrace(fixture.proof,chosen,[&](const auto &frame,const auto &metadata){QCOMPARE(metadata.size(),2);QCOMPARE(frame.completedFE,baseSeq+visited);++visited;});QVERIFY2(streamed.valid,qPrintable(streamed.message));QCOMPARE(visited,5-baseSeq);QVERIFY(streamed.dataset->columns.isEmpty());QCOMPARE(streamed.dataset->artifactSha256,numeric.artifactSha256);
        QVERIFY(!PiezoResultSeries::readAcceptedTrace(fixture.proof,{chosen.first(),chosen.first()}).valid);QVERIFY(!PiezoResultSeries::readAcceptedTrace(fixture.proof,{"missing-actual-port"}).valid);
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void layeredReaderRejectsUnknownFamilyAndMixedTopology() { try {
        auto source=scene(false);
        auto unknown=source;unknown.getProblemSetup().getStructuralPiezoLayeredSetup().family=static_cast<RStructuralPiezoLayeredModelFamily>(99);
        QVERIFY(rejects([&]{PiezoResultSeries::Reader invalid(unknown,true);}));
        auto mismatched=source;mismatched.getProblemSetup().getStructuralPiezoLayeredSetup().family=RStructuralPiezoLayeredModelFamily::FlatPlate;
        QVERIFY(rejects([&]{PiezoResultSeries::Reader invalid(mismatched,true);}));
        auto mixed=source;RElement unsupported(R_ELEMENT_QUAD1);mixed.addElement(unsupported,true,0);mixed.setElementPhysicalId(mixed.getNElements()-1,9001);
        QVERIFY(rejects([&]{PiezoResultSeries::Reader invalid(mixed,true);}));
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void compactTraceRejectsAuthorityChainPhysicalOpaqueAndSourceChanges() { try {
        QTemporaryDir directory;auto fixture=compactFixture("planarBeam",directory.path());const auto originalTrace=fileBytes(fixture.proof.tracePath);const auto originalBase=fileBytes(fixture.proof.baseModelPath);
        auto bad=fixture.proof;bad.expectedSourceFingerprint=QString(64,'f');QVERIFY(!PiezoResultSeries::readAcceptedTrace(bad).valid);bad=fixture.proof;bad.expectedContextSha256=QByteArray(32,'f');QVERIFY(!PiezoResultSeries::readAcceptedTrace(bad).valid);bad=fixture.proof;bad.expectedHeaderSha256=QByteArray(32,'f');QVERIFY(!PiezoResultSeries::readAcceptedTrace(bad).valid);bad=fixture.proof;bad.baseModelSha256=QByteArray(32,'f');QVERIFY(!PiezoResultSeries::readAcceptedTrace(bad).valid);
        QFile tail(fixture.proof.tracePath);QVERIFY(tail.open(QIODevice::Append));tail.write("uncommitted");tail.close();QVERIFY(!PiezoResultSeries::readAcceptedTrace(fixture.proof).valid);
        QVERIFY(!PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[](const auto &,const auto &){}).valid);
        const auto projected=PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[](const auto &,const auto &){},{},true);
        QVERIFY2(projected.valid,qPrintable(projected.message));
        const auto restore=[&]{QFile file(fixture.proof.tracePath);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(originalTrace),qint64(originalTrace.size()));};restore();
        QFile altered(fixture.proof.tracePath);QVERIFY(altered.open(QIODevice::ReadWrite));QVERIFY(altered.seek(fixture.proof.cursor.lastRecordOffset+40));const auto byte=altered.read(1);QVERIFY(altered.seek(fixture.proof.cursor.lastRecordOffset+40));altered.write(QByteArray(1,char(byte[0]^1)));altered.close();QVERIFY(!PiezoResultSeries::readAcceptedTrace(fixture.proof).valid);restore();
        auto changed=fixture;auto state=RStructuralPiezoLayeredRestartCodec::decode(changed.payloads[2]);state.physical.physicalDisplacements[6]+=.01;changed.payloads[2]=RStructuralPiezoLayeredRestartCodec::encode(state);rewriteTrace(changed);QVERIFY(!PiezoResultSeries::readAcceptedTrace(changed.proof).valid);restore();
        changed=fixture;state=RStructuralPiezoLayeredRestartCodec::decode(changed.payloads[2]);state.opaqueControllerState="{}";changed.payloads[2]=RStructuralPiezoLayeredRestartCodec::encode(state);rewriteTrace(changed);QVERIFY(!PiezoResultSeries::readAcceptedTrace(changed.proof).valid);restore();
        changed=fixture;changed.header.backendIdentity+="stale";rewriteTrace(changed);QVERIFY(!PiezoResultSeries::readAcceptedTrace(changed.proof).valid);restore();
        // Positive and negative zero are distinct original-clock authority.
        changed=fixture;QCOMPARE(bits(changed.header.initialTime),bits(0.));changed.header.initialTime=-0.;rewriteTrace(changed);QVERIFY(!PiezoResultSeries::readAcceptedTrace(changed.proof).valid);restore();
        RModel base;base.read(fixture.proof.baseModelPath);base.getProblemSetup().setRestart(true);PiezoResultSeries::Reader reader(base);reader.prepareAcceptedRuntimeBatch({fixture.payloads.front()});QVERIFY(rejects([&]{reader.certifyAcceptedRuntime(fixture.payloads.back());}));
        PiezoResultSeries::Reader failedPreparation(base);QVERIFY(rejects([&]{failedPreparation.prepareAcceptedRuntimeBatch({});}));QVERIFY(rejects([&]{failedPreparation.certifyAcceptedRuntime(fixture.payloads.front());}));
        bool modified=false;const auto changedDuringStaging=PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[](const auto &,const auto &){},[&](const auto &){QFile file(fixture.proof.baseModelPath);if(!file.open(QIODevice::Append))throw std::runtime_error("Base change fixture failed");file.write("changed source");modified=true;});QVERIFY(modified);QVERIFY(!changedDuringStaging.valid);
        QFile resetBase(fixture.proof.baseModelPath);QVERIFY(resetBase.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(resetBase.write(originalBase),qint64(originalBase.size()));resetBase.close();QVERIFY(PiezoResultSeries::readAcceptedTrace(fixture.proof).valid);
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void prefixFailureRejectsBeforeRuntimeAdmissionAndRecovers() { try {
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto fixture=compactFixture("planarBeam",directory.path());const auto original=fileBytes(fixture.proof.tracePath);
        QFile altered(fixture.proof.tracePath);QVERIFY(altered.open(QIODevice::ReadWrite));QVERIFY(altered.seek(fixture.proof.cursor.lastRecordOffset+40));const auto byte=altered.read(1);QCOMPARE(byte.size(),1);QVERIFY(altered.seek(fixture.proof.cursor.lastRecordOffset+40));QCOMPARE(altered.write(QByteArray(1,char(byte[0]^1))),qint64(1));altered.close();
        uint admitted=0;bool completed=false;const auto rejected=PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[&](const auto &,const auto &){++admitted;},[&](const auto &){completed=true;});
        QVERIFY(!rejected.valid);QVERIFY(!rejected.dataset);QCOMPARE(admitted,0u);QVERIFY(!completed);
        QFile restore(fixture.proof.tracePath);QVERIFY(restore.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(restore.write(original),qint64(original.size()));restore.close();
        const auto recovered=PiezoResultSeries::readAcceptedTrace(fixture.proof);QVERIFY2(recovered.valid,qPrintable(recovered.message));QCOMPARE(recovered.dataset->times.size(),qsizetype(5));
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void inFlightArtifactChangeRejectsWholeRead_data(){QTest::addColumn<bool>("base");QTest::newRow("base-model")<<true;QTest::newRow("trace")<<false;}
    void inFlightArtifactChangeRejectsWholeRead() { try {
        QFETCH(bool,base);QTemporaryDir directory;QVERIFY(directory.isValid());const auto fixture=compactFixture("planarBeam",directory.path());const auto path=base?fixture.proof.baseModelPath:fixture.proof.tracePath;const auto original=fileBytes(path);uint admitted=0;
        const auto rejected=PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[&](const auto &,const auto &){
            if(++admitted==1){QFile file(path);if(!file.open(QIODevice::Append)||file.write("in-flight change")!=16||!file.flush())throw std::runtime_error("In-flight artifact change fixture failed");}
        });
        QCOMPARE(admitted,5u);QVERIFY(!rejected.valid);QVERIFY(!rejected.dataset);QVERIFY2(rejected.message.contains(base?"base MODEL changed":"prefix length changed"),qPrintable(rejected.message));
        QFile restore(path);QVERIFY(restore.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(restore.write(original),qint64(original.size()));restore.close();
        const auto recovered=PiezoResultSeries::readAcceptedTrace(fixture.proof);QVERIFY2(recovered.valid,qPrintable(recovered.message));QCOMPARE(recovered.dataset->times.size(),qsizetype(5));
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void pipelineConsumerFailureAbortsPendingProviderAndRecovers_data(){QTest::addColumn<QString>("family");QTest::newRow("axial")<<QString("axialRod");QTest::newRow("layered")<<QString("planarBeam");}
    void pipelineConsumerFailureAbortsPendingProviderAndRecovers() { try {
        QFETCH(QString,family);QTemporaryDir directory;QVERIFY(directory.isValid());const auto fixture=compactFixture(family,directory.path(),0,{},260);
        uint admitted=0;const auto rejected=PiezoResultSeries::streamAcceptedTrace(fixture.proof,{},[&](const auto &,const auto &){++admitted;throw std::runtime_error("controlled consumer failure with next RPC pending");});
        QVERIFY(!rejected.valid);QVERIFY(!rejected.dataset);QCOMPARE(admitted,1u);QVERIFY(rejected.message.contains("controlled consumer failure"));
        const auto recovered=PiezoResultSeries::readAcceptedTrace(fixture.proof);QVERIFY2(recovered.valid,qPrintable(recovered.message));QCOMPARE(recovered.dataset->times.size(),qsizetype(261));
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void compactTraceRejectsModelLinks() { try {
        QTemporaryDir directory;const auto fixture=compactFixture("axialRod",directory.path());auto proof=fixture.proof;const auto link=directory.filePath("base-link.rbm");RModel::writeLink(link,proof.baseModelPath);proof.baseModelPath=link;proof.baseModelSha256=QCryptographicHash::hash(fileBytes(link),QCryptographicHash::Sha256);const auto read=PiezoResultSeries::readAcceptedTrace(proof);QVERIFY(!read.valid);QVERIFY(read.message.contains("LINK"));
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void compactTraceRejectsSymbolicAliases() { try {
        QTemporaryDir directory;const auto fixture=compactFixture("planarBeam",directory.path());const auto alias=directory.filePath("trace-alias");
        const auto path=[](const QString &value){
#ifdef Q_OS_WIN
            return std::filesystem::path(value.toStdWString());
#else
            return std::filesystem::path(value.toStdString());
#endif
        };
        std::error_code error;std::filesystem::create_symlink(path(fixture.proof.tracePath),path(alias),error);if(error)QSKIP(qPrintable(QString("Real symbolic-link creation unavailable: %1").arg(QString::fromStdString(error.message()))));
        QVERIFY(QFileInfo(alias).isSymLink());auto proof=fixture.proof;proof.tracePath=alias;QVERIFY(!PiezoResultSeries::readAcceptedTrace(proof).valid);
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());} }
    void frozenCertificationIsPureAndMatchesReconstruction_data(){QTest::addColumn<bool>("axial");QTest::newRow("axial")<<true;QTest::newRow("layered")<<false;}
    void frozenCertificationIsPureAndMatchesReconstruction() { try
    {
        QFETCH(bool,axial);QTemporaryDir dir;QVERIFY(dir.isValid());const auto source=scene(axial);const auto paths=records(source,dir.path(),axial);
        RModel last;last.read(paths.last());last.getProblemSetup().setRestart(true);
        if(axial){const auto backend=PiezoAxialBackendBridge::create(source);
            verifyFrozenCertification<RStructuralPiezoAxialNativeRun,RStructuralPiezoAxialRestartCodec>(source,last,backend.callbacks,
                [](auto &s){s.physical.displacements.at(6)+=.01;},
                [](RModel &m){m.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState="malformed codec";},
                [](const auto &s){return s.physical.displacements.at(6);});
        }else {const auto backend=PiezoNativeBackendBridge::create(source);
            verifyFrozenCertification<RStructuralPiezoLayeredNativeRun,RStructuralPiezoLayeredRestartCodec>(source,last,backend.callbacks,
                [](auto &s){s.physical.physicalDisplacements.at(12)+=.01;},
                [](RModel &m){m.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes="malformed codec";},
                [](const auto &s){return s.physical.physicalDisplacements.at(12);});
        }
    }
    catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}
    catch(const std::exception &error){QFAIL(error.what());}
    }
    void acceptedNamedSignalsAndFullControllerState_data(){QTest::addColumn<bool>("axial");QTest::newRow("axial")<<true;QTest::newRow("layered")<<false;}
    void acceptedNamedSignalsAndFullControllerState() { try
    {
        QFETCH(bool,axial);QTemporaryDir dir;QVERIFY(dir.isValid());const auto source=scene(axial);const auto paths=records(source,dir.path(),axial);
        const auto read=PiezoResultSeries::readAcceptedRecords(paths);QVERIFY2(read.valid,qPrintable(read.message));QVERIFY(read.dataset);
        const auto &d=*read.dataset;QCOMPARE(d.family,QString(axial?"axial":"layered"));QCOMPARE(d.modelFamily,QString(axial?"axialRod":"planarBeam"));QCOMPARE(d.frames.size(),qsizetype(5));QVERIFY(d.continuousBetweenSelectedRecords);
        QCOMPARE(d.artifactSha256.size(),64);QVERIFY(d.coverage.contains("no interpolation"));
        const auto &f=*d.frames.back();QCOMPARE(f.family,d.family);QCOMPARE(f.modelFamily,d.modelFamily);QCOMPARE(f.completedFE,4u);QCOMPARE(f.targetSteps,4u);QCOMPARE(f.controlTick,2u);QCOMPARE(f.controlPhase,1u);QCOMPARE(f.controlKind,QString("PI"));
        QVERIFY(!f.readouts.isEmpty());QVERIFY(!f.actions.isEmpty());QCOMPARE(f.actions.first().intervalPresent,true);
        for(const auto &port:f.readouts){
            const QString base="piezo:port:"+port.portId+":";
            QCOMPARE(find(f,base+"readout:absolute").value,port.value);QCOMPARE(find(f,base+"readout:delta").value,port.deltaValue);
            QCOMPARE(find(f,base+"readout:absolute").unit,port.unit);QCOMPARE(find(f,base+"charge:absolute").unit,QString("C"));
        }
        QCOMPARE(find(f,"piezo:electricalWork").value,f.electricalWorkJ);QCOMPARE(find(f,"piezo:electricalWork").unit,QString("J"));QVERIFY(f.electricalWorkSource.contains("not mechanical"));
        // Each sample is re-certified against the actual original FE source,
        // backend bundle and opaque history, including multisample phase.
        RModel last;last.read(paths.last());last.getProblemSetup().setRestart(true);PiezoResultSeries::Reader reader(last);
        const auto original=reader.certifyAcceptedModel(last);
        auto displayChanged=last;displayChanged.getVariable(displayChanged.findVariable(R_VARIABLE_DISPLACEMENT)).setValue(0,0,123.);
        const auto displayIndependent=reader.certifyAcceptedModel(displayChanged);
        QCOMPARE(displayIndependent->stateSha256,original->stateSha256);
        QCOMPARE(find(*displayIndependent,"node:123:displacement:X").value,find(*original,"node:123:displacement:X").value);
        auto wrongSource=last;wrongSource.getNode(1).setX(1.01);bool rejected=false;
        try{reader.certifyAcceptedModel(wrongSource);}catch(const RError &){rejected=true;}catch(const std::exception &){rejected=true;}QVERIFY(rejected);
        auto hidden=last;
        if(axial){auto &r=hidden.getProblemSetup().getStructuralPiezoAxialRestart();auto state=RStructuralPiezoAxialRestartCodec::decode(r.runtimeState);++state.controlTick;r.runtimeState=RStructuralPiezoAxialRestartCodec::encode(state);}
        else {auto &r=hidden.getProblemSetup().getStructuralPiezoLayeredRestart();auto state=RStructuralPiezoLayeredRestartCodec::decode(r.stateBytes);++state.controlTick;r.stateBytes=RStructuralPiezoLayeredRestartCodec::encode(state);}
        rejected=false;try{reader.certifyAcceptedModel(hidden);}catch(const RError &){rejected=true;}catch(const std::exception &){rejected=true;}QVERIFY(rejected);
        const auto sparse=PiezoResultSeries::readAcceptedRecords({paths[0],paths[2],paths[4]},d.sourceFingerprint);QVERIFY2(sparse.valid,qPrintable(sparse.message));QVERIFY(!sparse.dataset->continuousBetweenSelectedRecords);QCOMPARE(sparse.dataset->frames.size(),qsizetype(3));
        QVERIFY(!PiezoResultSeries::readAcceptedRecords({paths[1],paths[1]}).valid);
        QVERIFY(!PiezoResultSeries::readAcceptedRecords(paths,QString(64,'a')).valid);
        QVERIFY(!PiezoResultSeries::readAcceptedRecords({paths[1]},d.sourceFingerprint,{QString(64,'a')}).valid);
        if(!axial){ // Planar beam has actual UX/UZ/RY, not artificial other-plane rotation or translation channels.
            QVERIFY(std::none_of(f.channels.begin(),f.channels.end(),[](const auto &s){return s.physicalNodeId && (s.channelId.endsWith(":rotation:X") || s.channelId.endsWith(":rotation:Z") || s.channelId.endsWith(":displacement:Y"));}));
            QCOMPARE(find(f,"node:123:rotation:Y").unit,QString("rad"));
        }
    }
    catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}
    catch(const std::exception &error){QFAIL(error.what());}
    }
};
QTEST_GUILESS_MAIN(TestPiezoResultSeries)
#include "tst_piezo_result_series.moc"
