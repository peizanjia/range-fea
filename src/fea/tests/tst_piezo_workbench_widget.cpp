#include <QtTest>
#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCryptographicHash>
#include <QPushButton>
#include <QDockWidget>
#include <QMainWindow>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTemporaryDir>
#include <QScrollArea>
#include <QScrollBar>
#include <QPointer>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDir>
#include <rpiezo_experiment_service.h>
#include <rmodel_command_service.h>
#include <rpiezo_axial_experiment_service.h>
#include <rstructural_piezo_element_operators.h>
#include <piezo_native_backend_bridge.h>
#include <piezo_numeric_transport.h>
#include <piezo_axial_backend_bridge.h>
#include <piezo_result_series.h>
#include <piezo_spectrum.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rstructural_piezo_layered_model_adapter.h>
#include <rbl_error.h>
#include "application.h"
#include "model_io.h"
#include "model_tree.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include "piezo_workbench_widget.h"
#include "workflow_widget.h"
#include "session.h"
#include "solver_manager.h"
#include "series_plot_widget.h"
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <complex>
#include <limits>

namespace {
Model beam() {
    Model m;m.setName("Actual layered beam source");
    for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<2;++i){RElement e(R_ELEMENT_BEAM1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));return m;
}
Model rods() {
    Model m;m.setName("Actual axial rod source");
    for(uint i=0;i<3;++i){m.addNode(RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<2;++i){RElement e(R_ELEMENT_TRUSS1);e.setNodeId(0,i);e.setNodeId(1,i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));return m;
}
Model plate() {
    Model m;m.setName("Actual layered plate source");for(uint i=0;i<3;++i)for(uint j=0;j<2;++j){m.addNode(RNode(double(i),.1*j,0));m.setNodePhysicalId(2*i+j,101+11*(2*i+j));}
    for(uint i=0;i<2;++i){RElement e(R_ELEMENT_QUAD1);e.setNodeId(0,2*i);e.setNodeId(1,2*i+2);e.setNodeId(2,2*i+3);e.setNodeId(3,2*i+1);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));return m;
}
QString nullRecipePath(const QJsonValue &value,const QString &path={}) {
    if(value.isNull()||value.isUndefined())return path.isEmpty()?"/":path;
    if(value.isArray()){const auto array=value.toArray();for(int i=0;i<array.size();++i){auto bad=nullRecipePath(array[i],path+"/"+QString::number(i));if(!bad.isEmpty())return bad;}}
    if(value.isObject()){const auto object=value.toObject();for(auto it=object.constBegin();it!=object.constEnd();++it){auto bad=nullRecipePath(it.value(),path+"/"+it.key());if(!bad.isEmpty())return bad;}}
    return {};
}
QString sha256(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QStringList acceptedRecords(const RModel &source,const QString &directory,bool axial,uint count=3) {
    QStringList paths;
    const auto save=[&](const RModel &accepted,uint step){auto path=QDir(directory).filePath(QString("accepted-%1.%2").arg(step).arg(step%2?"ram":"rbm"));accepted.write(path,false);paths.append(accepted.nativeWritePaths(path,false).first());};
    if(axial){const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun run(source,backend.callbacks);save(run.acceptedModel(source),0);
        for(uint i=1;i<=count;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);save(run.acceptedModel(source),i);}
    }else{const auto backend=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);save(run.acceptedModel(source),0);
        for(uint i=1;i<=count;++i){auto step=run.prepare(source);run.validatePrepared(*step);run.commit(*step);save(run.acceptedModel(source),i);}
    }return paths;
}
PiezoResultSeries::TraceProof acceptedTrace(const RModel &source,const QString &directory,bool axial,uint count=5) {
    PiezoResultSeries::TraceProof proof;
    const auto store=[&](auto &run,auto encode){
        const auto base=run.acceptedModel(source);const auto path=QDir(directory).filePath("base.rbm");base.write(path,false);proof.baseModelPath=base.nativeWritePaths(path,false).first();
        QFile baseFile(proof.baseModelPath);if(!baseFile.open(QIODevice::ReadOnly))throw std::runtime_error("Trace base cannot be read");proof.baseModelSha256=QCryptographicHash::hash(baseFile.readAll(),QCryptographicHash::Sha256);
        const auto &state=run.acceptedRuntime();PiezoAcceptedTrace::Header header;header.family=axial?"axial":"layered";header.sourceFingerprint=state.sourceFingerprint;header.configurationIdentity=QString::fromLatin1(state.configurationIdentity.toHex());header.backendIdentity=state.backendIdentity;header.baseModelFile=QFileInfo(proof.baseModelPath).fileName();header.baseModelSha256=proof.baseModelSha256;header.contextSha256=QCryptographicHash::hash("trusted GUI fixture context",QCryptographicHash::Sha256);header.targetSteps=state.targetSteps;header.initialTime=state.initialTime;header.dt=state.dt;header.targetTimeGridSha256=PiezoAcceptedTrace::targetTimeGridSha256(header.initialTime,header.dt,header.targetSteps);
        proof.tracePath=QDir(directory).filePath("accepted-trace.rpt");QFile trace(proof.tracePath);if(!trace.open(QIODevice::ReadWrite|QIODevice::NewOnly))throw std::runtime_error("Trace cannot be written");header=PiezoAcceptedTrace::writeHeader(trace,header);auto cursor=PiezoAcceptedTrace::initialCursor(header);
        for(uint step=0;step<=count;++step){if(step){auto prepared=run.prepare(source);run.validatePrepared(*prepared);run.commit(*prepared);}const auto bytes=encode(run.acceptedRuntime());cursor=PiezoAcceptedTrace::append(trace,header,cursor,step,header.initialTime+double(step)*header.dt,bytes);}
        proof.expectedHeaderSha256=header.headerSha32;proof.expectedContextSha256=header.contextSha256;proof.expectedSourceFingerprint=header.sourceFingerprint;proof.cursor=cursor;
    };
    if(axial){const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun run(source,backend.callbacks);store(run,[](const auto &state){return RStructuralPiezoAxialRestartCodec::encode(state);});}
    else{const auto backend=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);store(run,[](const auto &state){return RStructuralPiezoLayeredRestartCodec::encode(state);});}return proof;
}
QString status(PiezoWorkbenchWidget &widget) {return widget.findChild<QLabel*>("piezoStatus")->text();}
void permitLegacyReplacement(PiezoWorkbenchWidget &widget) {widget.findChild<QCheckBox*>("piezoReplaceLegacy")->setChecked(true);}
}

class TestPiezoWorkbenchWidget : public QObject {
    Q_OBJECT
private slots:
    void solidTemplatesExposeRealVolumeBindings_data(){QTest::addColumn<QString>("kind");QTest::newRow("hex8")<<QString("solidHex");QTest::newRow("tet4")<<QString("solidTet");}
    void solidTemplatesExposeRealVolumeBindings(){
        QFETCH(QString,kind);static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));Model empty;empty.setName("Empty solid template source");empty.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));session->addModel(empty);session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);auto *choice=widget.findChild<QComboBox*>("piezoTemplateKind");QVERIFY(choice);choice->setCurrentIndex(choice->findData(kind));QTRY_COMPARE(widget.draftRecipe().value("family").toString(),QString("solid3D"));QTRY_VERIFY(widget.findChild<QTableWidget*>("piezoSolidVolumes"));QVERIFY(widget.findChild<QDoubleSpinBox*>("piezoTemplatePatchThickness"));
        const auto recipe=widget.draftRecipe();QVERIFY(recipe.value("members").toArray().size()>0);for(const auto &m:recipe.value("members").toArray())QCOMPARE(m.toObject().value("layers").toArray().size(),1);QVERIFY(!widget.allowedForceDirections().contains("RX"));
        const auto direct=RPiezoExperimentService::preview(session->getModel(0),widget.draftRecipe());QVERIFY2(direct.valid,qPrintable(direct.message));
        const auto stamp=session->modelCommandStamp(0);const auto first=RModelCommandService::preview(session->getModel(0),"piezo.configure-experiment",widget.draftRecipe(),stamp.first,QString::number(stamp.second));
        const auto second=RModelCommandService::preview(session->getModel(0),"piezo.configure-experiment",widget.draftRecipe(),stamp.first,QString::number(stamp.second));
        QVERIFY2(first.valid&&second.valid,qPrintable(first.message+" / "+second.message));
        QCOMPARE(first.fingerprint,second.fingerprint);
        const auto &directSetup=direct.candidate.getProblemSetup().getStructuralPiezoLayeredSetup();const RStructuralPiezoLayeredModelAdapter compiled(direct.candidate,directSetup);
        const auto &compiledData=compiled.data();for(size_t i=0;i<compiledData.freeDofs;++i)for(size_t j=i+1;j<compiledData.freeDofs;++j)QCOMPARE(compiledData.stiffness[i*compiledData.freeDofs+j],compiledData.stiffness[j*compiledData.freeDofs+i]);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();QVERIFY2(session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled,qPrintable(status(widget)));QCOMPARE(widget.draftRecipe().value("family").toString(),QString("solid3D"));
    }
    void externalConnectionFreezeHandshakeAndStaleFiles(){
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.configureController("external","sensor_only");QTRY_VERIFY(widget.findChild<QPushButton*>("controllerExternalFreeze"));
        const auto runtime=PiezoNumericTransport::defaultRuntime();const auto example=QFileInfo(QDir(runtime.second).filePath("../../fea-solver/examples/external_io/pure_provider.py")).canonicalFilePath();QVERIFY(!example.isEmpty());
        QTemporaryDir dependency;const auto dep=dependency.filePath("weights.json");{QFile f(dep);QVERIFY(f.open(QIODevice::WriteOnly));f.write("{}");}
        const auto path=QStringList{"controller","external"};QJsonObject config{{"transport","stdio"},{"executable",runtime.first},{"args",QJsonArray{"-B","-u",example}},{"algorithm",QJsonObject{{"id","range-example-pure-provider"},{"version","1"}}},{"dependencies",QJsonArray{dep,QFileInfo(example).dir().filePath("range_external_sdk.py")}}};widget.setValue(path+QStringList{"connection"},config);
        widget.inspectExternalConnection(path,false);QTRY_VERIFY_WITH_TIMEOUT(widget.valueAt(path+QStringList{"connection","expectedIdentity"}).isObject(),10000);
        const auto frozen=widget.valueAt(path+QStringList{"connection"}).toObject();
        widget.setValue({"excitation","externalSources"},QJsonArray{QJsonObject{{"connection",frozen},{"channelIds",QJsonArray{"node:123:local:EZ"}},{"sampleEveryFESteps",1},{"initialState",QJsonObject{}},{"seed",0}}});
        widget.inspectExternalConnection(path,true);QTRY_VERIFY_WITH_TIMEOUT(widget.valueAt(path+QStringList{"initialState"}).toObject().contains("integral"),10000);
        QTRY_VERIFY_WITH_TIMEOUT(widget.valueAt({"excitation","externalSources"}).toArray().first().toObject().value("initialState").toObject().contains("samples"),10000);QVERIFY(status(widget).contains("Combined external roles"));QVERIFY(widget.externalSourcesCurrent());
        {QFile f(dep);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write("{\"gain\":2}");}QVERIFY(!widget.externalSourcesCurrent());QVERIFY(!widget.hasCurrentPreview());
    }
    void generatePc11ApprovedNativeSources() {try {
        const auto rootPath=qEnvironmentVariable("RANGE_PIEZO_PC11_SOURCE_DIR");if(rootPath.isEmpty())QSKIP("Opt-in PC11 source preparation only; no accepted history or performance qualification.");
        const QFileInfo root(rootPath);QVERIFY(root.isAbsolute());QVERIFY(root.isDir());QVERIFY(!root.isSymLink());QVERIFY(!root.canonicalFilePath().isEmpty());
        for(QFileInfo component(root.absoluteFilePath());;){QVERIFY(!component.isSymLink());const auto parent=component.absolutePath();if(parent==component.absoluteFilePath())break;component=QFileInfo(parent);}
        QVERIFY(!QFileInfo::exists(QDir(root.canonicalFilePath()).filePath("sources.json")));auto *session=Application::instance()->getSession();QJsonArray sources;
        for(const auto &family:QStringList{"axialRod","planarBeam","flatPlate"}){
            const auto directory=QDir(root.canonicalFilePath()).filePath(family);QVERIFY2(!QFileInfo::exists(directory),qPrintable("Refusing existing PC11 family directory: "+directory));QVERIFY(QDir().mkpath(directory));const auto workspace=QDir(directory).filePath("workspace");QVERIFY(QDir().mkpath(workspace));session->clear();QString error;QVERIFY2(session->openProject(workspace,&error),qPrintable(error));session->addModel(family=="axialRod"?rods():family=="flatPlate"?plate():beam());session->setModelSelected(0,true);
            PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.findChild<QSpinBox*>("piezoSteps")->setValue(20000);const auto probe=RPiezoExperimentService::preview(session->getModel(0),widget.draftRecipe());QVERIFY2(probe.valid,qPrintable(probe.message));const auto package=family=="axialRod"?PiezoAxialBackendBridge::create(probe.candidate).exportPackage:PiezoNativeBackendBridge::create(probe.candidate).exportPackage;const auto omega=package.value("design").toObject().value("omega").toArray();QVERIFY(!omega.isEmpty());double omegaMax=0;QJsonArray frequenciesHz;
            for(const auto &value:omega){const double frequency=value.toDouble();QVERIFY(std::isfinite(frequency)&&frequency>0);omegaMax=std::max(omegaMax,frequency);frequenciesHz.append(frequency/(2*std::acos(-1.)));}
            const double dt=.12/omegaMax;QVERIFY(std::isfinite(dt)&&dt>=1e-12);widget.findChild<QDoubleSpinBox*>("piezoTimeStep")->setValue(dt);widget.findChild<QComboBox*>("piezoController")->setCurrentText("PI");QCoreApplication::processEvents();
            for(const auto *name:{"piezoControllerkp","piezoControllerki"}){auto *gain=widget.findChild<QTableWidget*>(name);QVERIFY(gain);for(int r=0;r<gain->rowCount();++r)for(int c=0;c<gain->columnCount();++c)gain->item(r,c)->setText("0");}
            auto *bias=widget.findChild<QTableWidget*>("piezoControllerbias");QVERIFY(bias);QVERIFY(bias->columnCount()>0);for(int c=0;c<bias->columnCount();++c)bias->item(0,c)->setText(".5");
            QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();QCOMPARE(applied.size(),1);const auto recipe=widget.draftRecipe();QCOMPARE(recipe.value("steps").toInt(),20000);QCOMPARE(recipe.value("dt").toDouble(),dt);QVERIFY2(widget.findChild<QPushButton*>("piezoRun")->isEnabled(),qPrintable(widget.findChild<QPushButton*>("piezoRun")->property("unavailableReason").toString()));
            const auto path=QDir(directory).filePath("source.rbm");ModelIO::save(session->getModel(0),path,false);Model reopened;ModelIO::open(reopened,path);const auto authoritative=RPiezoExperimentService::recipeFromModel(reopened);QCOMPARE(authoritative,recipe);QCOMPARE(reopened.getProblemSetup().getStructuralDynamicsSetup().getNSteps(),20000u);
            QFile source(path);QVERIFY(source.open(QIODevice::ReadOnly));const auto sourceBytes=source.readAll();const QString sourceSha=sha256(sourceBytes);const auto recipeBytes=QJsonDocument(recipe).toJson(QJsonDocument::Compact);const auto recipePath=QDir(directory).filePath("approved-recipe.json");QFile recipeFile(recipePath);QVERIFY(recipeFile.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(recipeFile.write(recipeBytes),qint64(recipeBytes.size()));recipeFile.close();
            const QString fingerprint=family=="axialRod"?reopened.getProblemSetup().getStructuralPiezoAxialSetup().computeSourceFingerprint(reopened):reopened.getProblemSetup().getStructuralPiezoLayeredSetup().computeSourceFingerprint(reopened);
            QJsonObject manifest{{"schema","range.piezo-pc11-approved-source/v1"},{"scope","approved native input only; accepted history and installed performance remain unmeasured"},{"family",family},{"sourcePath",path},{"sourceSha256",sourceSha},{"recipePath",recipePath},{"recipeSha256",sha256(recipeBytes)},{"sourceFingerprint",fingerprint},{"workspace",workspace},{"steps",20000},{"expectedAcceptedRecords",20001},{"dtS",dt},{"gridRule","dt*omegaMax=.12; explicit full-FE frequency probe"},{"probeFrequenciesHz",frequenciesHz},{"excitation","PI zero feedback gains with 0.5 V actuator bias; actual response unmeasured"},{"runInstructions","Open this workspace and source in installed fea.exe; select source; Piezo control → Run saved experiment → Confirm run. Wait for actual completion, then cold reopen native result for PC11 measurement."}};
            const auto manifestPath=QDir(directory).filePath("source-manifest.json");QFile output(manifestPath);QVERIFY(output.open(QIODevice::WriteOnly|QIODevice::NewOnly));const auto bytes=QJsonDocument(manifest).toJson(QJsonDocument::Indented);QCOMPARE(output.write(bytes),qint64(bytes.size()));output.close();sources.append(manifest);qInfo().noquote()<<"PC11_APPROVED_SOURCE"<<family<<path<<sourceSha;
        }
        QFile index(QDir(root.canonicalFilePath()).filePath("sources.json"));QVERIFY(index.open(QIODevice::WriteOnly|QIODevice::NewOnly));const auto bytes=QJsonDocument(QJsonObject{{"schema","range.piezo-pc11-source-index/v1"},{"qualification","SOURCE_PREPARATION_ONLY"},{"sources",sources}}).toJson(QJsonDocument::Indented);QCOMPARE(index.write(bytes),qint64(bytes.size()));
    }catch(const RError &error){QFAIL(qPrintable(error.getMessage()));}catch(const std::exception &error){QFAIL(error.what());}}
    void plotPaintSignalsTrackPresentedGenerationsOnly() {
        SeriesPlotWidget widget;widget.resize(640,480);auto *canvas=widget.findChild<QWidget*>("seriesPlotCanvas");QVERIFY(canvas);QSignalSpy painted(&widget,&SeriesPlotWidget::datasetPainted);
        const auto render=[&]{QImage image(canvas->size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);canvas->render(&image);};
        SeriesPlotWidget::Dataset data;data.xUnit="s";data.yUnit="V";SeriesPlotWidget::Trace trace;trace.id="named";trace.sourceId="source";trace.artifactSha256=QString(64,'a');trace.unit="V";trace.points={QPointF(0,1),QPointF(1,2)};trace.originalOrdinals={0,1};data.traces={trace};
        QVERIFY(widget.setDataset(data));const auto first=widget.datasetGeneration();QVERIFY(first>0);render();QCOMPARE(painted.size(),1);QCOMPARE(painted.first().first().toULongLong(),first);render();QCOMPARE(painted.size(),1);
        widget.setDisplayHeld(true);data.traces.first().points[1].setY(3);QVERIFY(widget.setDataset(data));QCOMPARE(widget.datasetGeneration(),first);render();QCOMPARE(painted.size(),1);widget.setDisplayHeld(false);QVERIFY(widget.datasetGeneration()>first);render();QCOMPARE(painted.size(),2);QCOMPARE(painted.last().first().toULongLong(),widget.datasetGeneration());
        QVERIFY(widget.setDataset({}));render();QCOMPARE(painted.size(),2);QVERIFY(widget.setDataset(data));render();QCOMPARE(painted.size(),3);
    }
    void performanceLoggingIsOptionalBoundedAndMarksFailure() {
        const auto oldWorkspace=qgetenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE"),oldEvents=qgetenv("RANGE_PIEZO_PERFORMANCE_EVENTS");struct Restore{QByteArray workspace,events;~Restore(){if(workspace.isNull())qunsetenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE");else qputenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE",workspace);if(events.isNull())qunsetenv("RANGE_PIEZO_PERFORMANCE_EVENTS");else qputenv("RANGE_PIEZO_PERFORMANCE_EVENTS",events);}}restore{oldWorkspace,oldEvents};
        QTemporaryDir directory;const auto events=directory.filePath("events.ndjson");qputenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE",directory.path().toUtf8());qputenv("RANGE_PIEZO_PERFORMANCE_EVENTS",events.toUtf8());PiezoWorkbenchWidget observed;QVERIFY(!observed.showAcceptedTimeSeriesFrom({}));QVERIFY(!observed.property("piezoMeasurementInvalid").toBool());QFile log(events);QVERIFY(log.open(QIODevice::ReadOnly));const auto event=QJsonDocument::fromJson(log.readLine()).object();QCOMPARE(event.value("event").toString(),QString("request_begin"));QVERIFY(!event.value("requestId").toString().isEmpty());QVERIFY(event.value("elapsedNanoseconds").toString().toLongLong()>=0);log.close();
        qputenv("RANGE_PIEZO_PERFORMANCE_EVENTS",directory.path().toUtf8());PiezoWorkbenchWidget invalid;QTest::ignoreMessage(QtCriticalMsg,QRegularExpression("^RANGE_PIEZO_MEASUREMENT_INVALID .*Performance evidence path is not a regular file\\.$"));QVERIFY(!invalid.showAcceptedTimeSeriesFrom({}));QVERIFY(invalid.property("piezoMeasurementInvalid").toBool());QVERIFY(!invalid.property("piezoMeasurementError").toString().isEmpty());
        qunsetenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE");qunsetenv("RANGE_PIEZO_PERFORMANCE_EVENTS");PiezoWorkbenchWidget ordinary;QVERIFY(!ordinary.showAcceptedTimeSeriesFrom({}));QVERIFY(!ordinary.property("piezoMeasurementInvalid").toBool());
    }
    void constructorAndRebuildNeverInsertOptionalJsonNulls_data() {
        QTest::addColumn<QString>("family");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");QTest::newRow("rod")<<QString("axialRod");
    }
    void constructorAndRebuildNeverInsertOptionalJsonNulls() {
        QFETCH(QString,family);static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(family=="axialRod"?rods():family=="flatPlate"?plate():beam());session->setModelSelected(0,true);
        const auto expected=RPiezoExperimentService::exampleRecipe(session->getModel(0));QVERIFY(!expected.contains("template"));const auto stamp=session->modelCommandStamp(0);
        PiezoWorkbenchWidget widget;widget.setSession(session);QVERIFY2(nullRecipePath(widget.draftRecipe()).isEmpty(),qPrintable(nullRecipePath(widget.draftRecipe())));QCOMPARE(widget.draftRecipe(),expected);
        widget.refreshContext();QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe(),expected);QCOMPARE(session->modelCommandStamp(0),stamp);
        QVERIFY(!session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);QVERIFY(!session->getModel(0).getProblemSetup().getStructuralPiezoAxialSetup().enabled);
        widget.findChild<QComboBox*>("piezoController")->setCurrentText("PID");QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe()["controller"].toObject()["kind"].toString(),QString("PID"));
        const auto edited=widget.draftRecipe();QVERIFY2(nullRecipePath(edited).isEmpty(),qPrintable(nullRecipePath(edited)));widget.refreshContext();QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe(),edited);QCOMPARE(session->modelCommandStamp(0),stamp);
    }
    void cleanup() {
        // Each submitted job belongs to the run test and is cancelled there.
        QTRY_VERIFY_WITH_TIMEOUT(!SolverManager::getInstance().hasLiveModelTasks(),30000);
        Application::instance()->getSession()->clear();
    }
    void wholePreviewUsesCurrentModelAndOneHistory() {
        static QTemporaryDir workspace;QVERIFY(workspace.isValid());auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        const auto stamp=session->modelCommandStamp(0);PiezoWorkbenchWidget widget;widget.setSession(session);
        QCOMPARE(widget.findChild<QTabWidget*>("piezoPages")->count(),6);
        QCOMPARE(widget.draftRecipe()["members"].toArray().size(),2);
        QCOMPARE(widget.draftRecipe()["members"].toArray().first().toObject()["physicalElementId"].toDouble(),501.);
        QVERIFY(!widget.isModal());permitLegacyReplacement(widget);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        QVERIFY(widget.findChild<QPushButton*>("piezoConfirm")->isEnabled());
        QCOMPARE(session->modelCommandStamp(0),stamp);
        QVERIFY(!session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
        QCOMPARE(session->getModel(0).getUndoStackSize(),0u);
        QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);widget.applyPreview();
        QCOMPARE(applied.size(),1);QVERIFY(session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
        QCOMPARE(session->getModel(0).getNElements(),2u);QCOMPARE(session->getModel(0).getElementPhysicalId(1),514u);
        QCOMPARE(session->getModel(0).getUndoStackSize(),1u);QVERIFY(session->modelCommandStamp(0).second>stamp.second);
        QVERIFY(!widget.hasCurrentPreview());QVERIFY(widget.findChild<QPushButton*>("piezoRun")->isEnabled());
    }
    void previewSummaryIsVisibleAndFocusedInCompactWorkflow() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        QScrollArea outer;outer.setWidgetResizable(true);auto *wrapper=new QWidget;auto *wrapperLayout=new QVBoxLayout(wrapper);wrapperLayout->setContentsMargins(0,0,0,0);
        auto *widget=new PiezoWorkbenchWidget(wrapper);wrapperLayout->addWidget(widget);outer.setWidget(wrapper);outer.resize(760,220);
        widget->setSession(session);permitLegacyReplacement(*widget);outer.show();QTest::qWait(50);
        auto *panel=widget->findChild<QWidget*>("piezoPreviewPanel");auto *changes=widget->findChild<QTableWidget*>("piezoChanges");auto *confirm=widget->findChild<QPushButton*>("piezoConfirm");auto *expandPreview=widget->findChild<QPushButton*>("piezoExpandPreview");QVERIFY(panel);QVERIFY(changes);QVERIFY(confirm);QVERIFY(expandPreview);
        widget->previewExperiment();QVERIFY2(widget->hasCurrentPreview(),qPrintable(status(*widget)));
        QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(changes));QVERIFY(confirm->isEnabled());QVERIFY(changes->isVisible());
        QVERIFY(panel->isVisible());QVERIFY(expandPreview->isVisible());QVERIFY(!expandPreview->isChecked());
        QVERIFY((expandPreview->focusPolicy()&Qt::TabFocus)!=Qt::NoFocus);QCOMPARE(expandPreview->accessibleName(),QStringLiteral("Expand experiment preview details"));
        QVERIFY(changes->height()<=120);QCOMPARE(widget->findChildren<QPushButton*>("piezoConfirm").size(),1);
        const auto panelGeometry=[&]{return QRect(panel->mapTo(outer.viewport(),QPoint()),panel->size());};
        const auto summaryGeometry=[&]{return QRect(changes->mapTo(outer.viewport(),QPoint()),changes->size());};
        const auto confirmGeometry=[&]{return QRect(confirm->mapTo(outer.viewport(),QPoint()),confirm->size());};
        QTRY_VERIFY_WITH_TIMEOUT(outer.viewport()->rect().contains(panelGeometry())&&outer.viewport()->rect().contains(summaryGeometry())&&outer.viewport()->rect().contains(confirmGeometry()),3000);
        QVERIFY2(outer.viewport()->rect().contains(summaryGeometry()),"Preview summary is clipped in compact workflow");
        QVERIFY2(outer.viewport()->rect().contains(confirmGeometry()),"Confirm action is clipped in compact workflow");
        QCOMPARE(changes->accessibleName(),QStringLiteral("Experiment preview summary"));QVERIFY(changes->rowCount()>0);
        changes->setRowCount(1);
        changes->setItem(0,0,new QTableWidgetItem(QStringLiteral("Solver warning")));
        changes->setItem(0,1,new QTableWidgetItem(QStringLiteral("Short warning.")));
        QTRY_VERIFY(!expandPreview->isVisible());
        changes->item(0,1)->setText(QStringLiteral("Long warning and material provenance details. ").repeated(140));
        QTRY_VERIFY(expandPreview->isVisible());QCOMPARE(changes->rowCount(),1);
        int naturalHeight=changes->horizontalHeader()->height()+2*changes->frameWidth();
        for(int row=0;row<changes->rowCount();++row)naturalHeight+=changes->rowHeight(row);
        QVERIFY(naturalHeight>120);QVERIFY(changes->height()<=120);
        const int compactHeight=changes->height();outer.resize(760,620);QTest::qWait(50);
        expandPreview->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(expandPreview));
        QTest::keyClick(expandPreview,Qt::Key_Space);QTRY_VERIFY(expandPreview->isChecked());QTRY_VERIFY(changes->height()>compactHeight);
        QCOMPARE(expandPreview->accessibleName(),QStringLiteral("Collapse experiment preview details"));QVERIFY(confirm->isEnabled());QVERIFY(confirm->isVisible());QVERIFY(changes->maximumHeight()<=360);
        QCOMPARE(widget->findChildren<QPushButton*>("piezoConfirm").size(),1);
        QTest::keyClick(expandPreview,Qt::Key_Space);QTRY_VERIFY(!expandPreview->isChecked());QCOMPARE(changes->maximumHeight(),120);QCOMPARE(changes->minimumHeight(),104);
    }
    void editorFocusAndScrollSurviveControllerRebuild() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);widget.resize(760,320);widget.show();QTest::qWait(50);
        auto *pages=widget.findChild<QTabWidget*>("piezoPages");QVERIFY(pages);QVERIFY(pages->count()>2);
        auto *controller=widget.findChild<QComboBox*>("piezoController");QVERIFY(controller);
        QWidget *controllerTab=controller;
        while(controllerTab&&pages->indexOf(controllerTab)<0)controllerTab=controllerTab->parentWidget();
        QVERIFY(controllerTab);pages->setCurrentIndex(pages->indexOf(controllerTab));controller->setCurrentText("PI");
        QTRY_COMPARE(widget.draftRecipe().value("controller").toObject().value("kind").toString(),QStringLiteral("PI"));
        QTRY_VERIFY(widget.findChild<QTableWidget*>("piezoControllerkp"));QCoreApplication::processEvents();
        pages=widget.findChild<QTabWidget*>("piezoPages");auto *gain=widget.findChild<QTableWidget*>("piezoControllerkp");QVERIFY(gain);
        QWidget *gainTab=gain;while(gainTab&&pages->indexOf(gainTab)<0)gainTab=gainTab->parentWidget();
        QVERIFY(gainTab);pages->setCurrentIndex(pages->indexOf(gainTab));QVERIFY(gain->isVisible());
        auto *page=qobject_cast<QScrollArea*>(gainTab);QVERIFY(page);QPointer<QTableWidget> retiredGain(gain);
        auto *scroll=page->verticalScrollBar();QVERIFY(scroll->maximum()>0);scroll->setValue(std::min(36,scroll->maximum()));
        gain->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(gain));
        const int savedScroll=scroll->value();QVERIFY(savedScroll>0);
        auto *preview=widget.findChild<QPushButton*>("piezoPreview");QVERIFY(preview);preview->setFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(preview));
        auto *state=widget.findChild<QComboBox*>("piezoStateSource");QVERIFY(state);state->setCurrentIndex(state->findData("sensor_only"));
        QTRY_VERIFY(retiredGain.isNull());
        auto *restoredGain=widget.findChild<QTableWidget*>("piezoControllerkp");QVERIFY(restoredGain);
        QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(restoredGain));
        pages=widget.findChild<QTabWidget*>("piezoPages");QWidget *restoredTab=restoredGain;
        while(restoredTab&&pages->indexOf(restoredTab)<0)restoredTab=restoredTab->parentWidget();
        QVERIFY(restoredTab);QCOMPARE(pages->currentWidget(),restoredTab);QVERIFY(restoredGain->isVisible());
        page=qobject_cast<QScrollArea*>(restoredTab);QVERIFY(page);
        QCOMPARE(page->verticalScrollBar()->value(),std::min(savedScroll,page->verticalScrollBar()->maximum()));
    }
    void setupEditorsExposeAccessibleFieldNames() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);
        auto *timeStep=widget.findChild<QDoubleSpinBox*>("piezoTimeStep");auto *controller=widget.findChild<QComboBox*>("piezoController");
        auto *state=widget.findChild<QComboBox*>("piezoStateSource");auto *damping=widget.findChild<QComboBox*>("piezoDamping");
        auto *supports=widget.findChild<QTableWidget*>("piezoSupports");auto *frame=widget.findChild<QTableWidget*>("piezoFrameex");
        QVERIFY(timeStep);QVERIFY(controller);QVERIFY(state);QVERIFY(damping);QVERIFY(supports);QVERIFY(frame);
        QCOMPARE(timeStep->accessibleName(),QStringLiteral("FE time step (s)"));
        QCOMPARE(controller->accessibleName(),QStringLiteral("Controller"));QCOMPARE(state->accessibleName(),QStringLiteral("State source"));
        QCOMPARE(damping->accessibleName(),QStringLiteral("Physical damping authority"));
        auto *node=qobject_cast<QComboBox*>(supports->cellWidget(0,0));QVERIFY(node);QCOMPARE(node->accessibleName(),QStringLiteral("Bound node"));
        QCOMPARE(frame->accessibleName(),QStringLiteral("Local X axis components"));
    }
    void draftAndSourceChangesRetireApproval() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.previewExperiment();
        QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        auto *thickness=widget.findChild<QDoubleSpinBox*>("piezoLayerThickness0_1");QVERIFY(thickness);thickness->setValue(.0004);
        QVERIFY(!widget.hasCurrentPreview());QVERIFY(!widget.findChild<QPushButton*>("piezoConfirm")->isEnabled());
        widget.applyPreview();QVERIFY(!session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        session->getModel(0).setDescription("External accepted edit");session->setModelChanged(0);
        QVERIFY(!widget.hasCurrentPreview());QVERIFY(!widget.findChild<QPushButton*>("piezoConfirm")->isEnabled());
        widget.applyPreview();QVERIFY(!session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
    }
    void actualPickedElementsStayBoundWhenViewportPicksChange() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        session->getPickList().addItem(PickItem(SessionEntityID(0,R_ENTITY_GROUP_LINE,0),1,1));
        PiezoWorkbenchWidget widget;widget.setSession(session);auto members=widget.draftRecipe()["members"].toArray();
        QCOMPARE(members[0].toObject()["layers"].toArray().size(),1);QCOMPARE(members[1].toObject()["layers"].toArray().size(),3);
        const auto draft=widget.draftRecipe();session->getPickList().clear();QCoreApplication::processEvents();
        QCOMPARE(widget.draftRecipe(),draft); // Physical selection was captured once into the experiment.
        widget.usePickedMembers();QCOMPARE(widget.draftRecipe(),draft);QVERIFY(status(widget).contains("No elements are currently picked"));
        permitLegacyReplacement(widget);widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();
        const auto &setup=session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup();
        QCOMPARE(setup.members[0].layers.size(),size_t(1));QCOMPARE(setup.members[1].physicalElementId,514u);QCOMPARE(setup.members[1].layers.size(),size_t(3));
    }
    void pc09AxialPickedDeviceReadoutPreviewAndConfirm() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(rods());session->setModelSelected(0,true);
        session->getPickList().addItem(PickItem(SessionEntityID(0,R_ENTITY_GROUP_LINE,1),1,1));
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        auto *usePicked=widget.findChild<QPushButton*>("piezoUsePickedMembers");QVERIFY(usePicked);QTest::mouseClick(usePicked,Qt::LeftButton);
        auto recipe=widget.draftRecipe();auto members=recipe.value("members").toArray();
        const auto picked=std::find_if(members.begin(),members.end(),[](const QJsonValue &value){return value.toObject().value("physicalElementId").toInt()==514;});
        QVERIFY(picked!=members.end());QCOMPARE(picked->toObject().value("kind").toString(),QString("active"));
        auto *readouts=widget.findChild<QTableWidget*>("piezoPorts");QVERIFY(readouts);QVERIFY(readouts->rowCount()>0);
        auto *role=qobject_cast<QComboBox*>(readouts->cellWidget(0,1));QVERIFY(role);const int sensorIndex=role->findData("sensor");QVERIFY(sensorIndex>=0);role->setCurrentIndex(sensorIndex);
        auto *readout=qobject_cast<QComboBox*>(readouts->cellWidget(0,2));QVERIFY(readout);
        const int amplifierIndex=readout->findData("idealChargeAmplifier");QVERIFY(amplifierIndex>=0);readout->setCurrentIndex(amplifierIndex);
        auto *feedbackCapacitance=qobject_cast<QDoubleSpinBox*>(readouts->cellWidget(0,3));QVERIFY(feedbackCapacitance);feedbackCapacitance->setValue(.01);
        recipe=widget.draftRecipe();members=recipe.value("members").toArray();
        auto active=std::find_if(members.begin(),members.end(),[](const QJsonValue &value){return value.toObject().value("physicalElementId").toInt()==514;});
        QVERIFY(active!=members.end());const auto activeMember=active->toObject();
        const auto materials=recipe.value("materials").toArray();const auto activeMaterial=std::find_if(materials.begin(),materials.end(),[&](const QJsonValue &value){return value.toObject().value("materialId")==activeMember.value("materialId");});
        QVERIFY(activeMaterial!=materials.end());QVERIFY(activeMaterial->toObject().value("piezoelectric").toBool());
        QCOMPARE(activeMember.value("materialId").toString(),QString("examplePiezo"));
        const auto hostMember=std::find_if(members.begin(),members.end(),[](const QJsonValue &value){return value.toObject().value("physicalElementId").toInt()==501;});
        QVERIFY(hostMember!=members.end());QCOMPARE(hostMember->toObject().value("kind").toString(),QString("passive"));
        const auto hostMaterial=std::find_if(materials.begin(),materials.end(),[&](const QJsonValue &value){return value.toObject().value("materialId")==hostMember->toObject().value("materialId");});
        QVERIFY(hostMaterial!=materials.end());QVERIFY(!hostMaterial->toObject().value("piezoelectric").toBool());
        const auto ports=recipe.value("ports").toArray();const QString portId=activeMember.value("portId").toString();
        const auto port=std::find_if(ports.begin(),ports.end(),[&](const QJsonValue &value){return value.toObject().value("portId").toString()==portId;});
        QVERIFY(port!=ports.end());QCOMPARE(port->toObject().value("role").toString(),QString("sensor"));
        QCOMPARE(port->toObject().value("readout").toString(),QString("idealChargeAmplifier"));
        QCOMPARE(port->toObject().value("Cf").toDouble(),.01);
        const auto beforeStamp=session->modelCommandStamp(0);const auto beforeUndo=session->getModel(0).getUndoStackSize();
        auto *preview=widget.findChild<QPushButton*>("piezoPreview");auto *confirm=widget.findChild<QPushButton*>("piezoConfirm");QVERIFY(preview);QVERIFY(confirm);
        QTest::mouseClick(preview,Qt::LeftButton);QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QVERIFY(confirm->isEnabled());
        QCOMPARE(session->modelCommandStamp(0),beforeStamp);QCOMPARE(session->getModel(0).getUndoStackSize(),beforeUndo);
        QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);QTest::mouseClick(confirm,Qt::LeftButton);QCOMPARE(applied.size(),1);
        const auto afterStamp=session->modelCommandStamp(0);QCOMPARE(afterStamp.first,beforeStamp.first);QCOMPARE(afterStamp.second,beforeStamp.second+1);QCOMPARE(session->getModel(0).getUndoStackSize(),beforeUndo+1);
        const auto accepted=RPiezoExperimentService::recipeFromModel(session->getModel(0));QCOMPARE(accepted,widget.draftRecipe());
        const auto savedMembers=accepted.value("members").toArray();const auto savedActive=std::find_if(savedMembers.begin(),savedMembers.end(),[](const QJsonValue &value){return value.toObject().value("physicalElementId").toInt()==514;});
        QVERIFY(savedActive!=savedMembers.end());QCOMPARE(savedActive->toObject().value("portId").toString(),portId);
        const auto savedPorts=accepted.value("ports").toArray();const auto savedPort=std::find_if(savedPorts.begin(),savedPorts.end(),[&](const QJsonValue &value){return value.toObject().value("portId").toString()==portId;});
        QVERIFY(savedPort!=savedPorts.end());QCOMPARE(savedPort->toObject().value("readout").toString(),QString("idealChargeAmplifier"));
        QCOMPARE(savedPort->toObject().value("Cf").toDouble(),.01);
    }
    void unsupportedPickedElementsGiveFeedbackWithoutChangingDraft() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));Model source=rods();RElement point(R_ELEMENT_POINT);point.setNodeId(0,0);source.addElement(point,true,0);source.setElementPhysicalId(source.getNElements()-1,777);
        session->addModel(source);session->setModelSelected(0,true);PiezoWorkbenchWidget widget;widget.setSession(session);
        const auto draft=widget.draftRecipe();session->getPickList().addItem(PickItem(SessionEntityID(0,R_ENTITY_GROUP_POINT,0),2,1));
        widget.usePickedMembers();QCOMPARE(widget.draftRecipe(),draft);QVERIFY(status(widget).contains("No supported TRUSS1 rods"));
    }
    void invalidTensorTextAndUnsupportedDofNeverApply() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        auto *elasticity=widget.findChild<QTableWidget*>("piezoMaterial0elasticityOrCompliance");QVERIFY(elasticity);
        const auto original=elasticity->item(0,0)->text();elasticity->item(0,0)->setText("nan");widget.previewExperiment();QVERIFY(!widget.hasCurrentPreview());
        elasticity->item(0,0)->setText(original);
        auto *supports=widget.findChild<QTableWidget*>("piezoSupports");QVERIFY(supports);
        auto *dof=qobject_cast<QComboBox*>(supports->cellWidget(0,1));QVERIFY(dof);dof->setCurrentText("UY");
        widget.previewExperiment();QVERIFY(!widget.hasCurrentPreview());QCOMPARE(session->getModel(0).getUndoStackSize(),0u);
        QVERIFY(!session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
    }
    void controllerDimensionsUseActualPlantAndElectrodeMeasurements() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        widget.findChild<QComboBox*>("piezoController")->setCurrentText("PID");QCoreApplication::processEvents();
        auto c=widget.draftRecipe()["controller"].toObject();QCOMPARE(c["kind"].toString(),QString("PID"));
        QVERIFY(c["selectedOutputs"].toArray().contains("sense"));QCOMPARE(c["kp"].toArray().size(),1);
        QCOMPARE(c["kp"].toArray().first().toArray().size(),c["selectedOutputs"].toArray().size());
        widget.findChild<QComboBox*>("piezoController")->setCurrentText("LQR");QCoreApplication::processEvents();c=widget.draftRecipe()["controller"].toObject();
        QCOMPARE(c["Q"].toArray().size(),12);QCOMPARE(c["R"].toArray().size(),1);
        widget.findChild<QComboBox*>("piezoStateSource")->setCurrentIndex(1);QCoreApplication::processEvents();c=widget.draftRecipe()["controller"].toObject();
        QCOMPARE(c["observer"].toObject()["processCovariance"].toArray().size(),12);
        QCOMPARE(c["observer"].toObject()["measurementCovariance"].toArray().size(),c["selectedOutputs"].toArray().size());
    }
    void savedReopenedExperimentKeepsEditableDeclarations_data() {
        QTest::addColumn<bool>("axial");QTest::newRow("layered-beam")<<false;QTest::newRow("axial-rods")<<true;
    }
    void savedReopenedExperimentKeepsEditableDeclarations() {
        QFETCH(bool,axial);
        static QTemporaryDir workspace;static QTemporaryDir nativeFiles;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(axial?rods():beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(3);widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);widget.applyPreview();QCOMPARE(applied.size(),1);
        QVERIFY(axial?session->getModel(0).getProblemSetup().getStructuralPiezoAxialSetup().enabled:session->getModel(0).getProblemSetup().getStructuralPiezoLayeredSetup().enabled);
        QCOMPARE(session->getModel(0).getUndoStackSize(),1u);
        const auto declaration=widget.draftRecipe();const auto path=nativeFiles.filePath("experiment.rbm");ModelIO::save(session->getModel(0),path,false);
        session->clear();Model reopened;ModelIO::open(reopened,path);session->addModel(reopened);session->setModelSelected(0,true);widget.refreshContext();
        QCOMPARE(widget.draftRecipe(),declaration);QVERIFY(widget.findChild<QPushButton*>("piezoRun")->isEnabled());
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(4);QVERIFY(!widget.findChild<QPushButton*>("piezoRun")->isEnabled());
    }
    void unchangedRunAndConfirmAreTwoNativeOperations_data() {
        QTest::addColumn<bool>("axial");QTest::newRow("layered-beam")<<false;QTest::newRow("axial-rods")<<true;
    }
    void unchangedRunAndConfirmAreTwoNativeOperations() {
        QFETCH(bool,axial);
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(axial?rods():beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.findChild<QSpinBox*>("piezoSteps")->setValue(2);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();
        const auto stamp=session->modelCommandStamp(0);const auto undo=session->getModel(0).getUndoStackSize();
        auto &manager=SolverManager::getInstance();const int before=manager.jobsQuery()["total"].toInt();
        QSignalSpy submitted(&widget,&PiezoWorkbenchWidget::runSubmitted);
        // First active operation: Run prepares actual native validation, without submission.
        widget.findChild<QPushButton*>("piezoRun")->click();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        QCOMPARE(manager.jobsQuery()["total"].toInt(),before);QCOMPARE(submitted.size(),0);
        const auto validation=manager.validateRun(0);auto *previewTable=widget.findChild<QTableWidget*>("piezoChanges");
        QVERIFY(!previewTable->isHidden());QVERIFY(previewTable->rowCount()>0);
        QVERIFY(!validation["warnings"].toArray().isEmpty()); // Real beam has the checker's no-volume warning.
        const auto savedRecipe=RPiezoExperimentService::recipeFromModel(session->getModel(0));
        const auto rowFor=[&](const QString &label){for(int row=0;row<previewTable->rowCount();++row)
            if(previewTable->item(row,0)&&previewTable->item(row,0)->text()==label)return row;return -1;};
        const auto valueFor=[&](int row){return row>=0&&previewTable->item(row,1)?previewTable->item(row,1)->text():QString();};
        const int savedModelRow=rowFor("Saved model"),savedSetupRow=rowFor("Saved FE setup"),savedControllerRow=rowFor("Saved controller");
        QVERIFY(savedModelRow>=0);QVERIFY(savedSetupRow>=0);QVERIFY(savedControllerRow>=0);
        const auto authoritativeStamp=session->modelCommandStamp(0);
        QCOMPARE(valueFor(savedModelRow),QString("%1 (revision %2)").arg(session->getModel(0).getName()).arg(authoritativeStamp.second));
        const double savedDt=savedRecipe.value("dt").toDouble();
        const int savedSteps=savedRecipe.value("steps").toInt();
        QCOMPARE(valueFor(savedSetupRow),QString("%1 FE steps; dt %2 s; total duration %3 s")
            .arg(savedSteps).arg(QString::number(savedDt,'g',17),QString::number(savedDt*savedSteps,'g',17)));
        QCOMPARE(valueFor(savedControllerRow),savedRecipe.value("controller").toObject().value("kind").toString());
        QStringList displayed;for(int row=0;row<previewTable->rowCount();++row)displayed.append(previewTable->item(row,1)->text());
        for(const auto &warning:validation["warnings"].toArray())QVERIFY(displayed.contains(warning.isString()?warning.toString():warning.toObject()["message"].toString()));
        int firstWarningRow=previewTable->rowCount();for(int row=0;row<previewTable->rowCount();++row)
            if(previewTable->item(row,0)&&previewTable->item(row,0)->text()=="Solver warning"){firstWarningRow=row;break;}
        QVERIFY(savedModelRow<firstWarningRow);QVERIFY(savedSetupRow<firstWarningRow);QVERIFY(savedControllerRow<firstWarningRow);
        QVERIFY(widget.findChild<QPushButton*>("piezoConfirm")->text().contains("warnings"));
        // Second active operation: one trusted confirmation submits exactly one job.
        widget.findChild<QPushButton*>("piezoConfirm")->click();QVERIFY2(submitted.size()==1,qPrintable(status(widget)));QCOMPARE(submitted.size(),1);
        const auto jobs=manager.jobsQuery();QCOMPARE(jobs["total"].toInt(),before+1);
        QCOMPARE(session->modelCommandStamp(0),stamp);QCOMPARE(session->getModel(0).getUndoStackSize(),undo);
        const auto records=jobs["jobs"].toArray();QVERIFY(!records.isEmpty());const QString jobId=records.last().toObject()["jobId"].toString();
        QVERIFY(!jobId.isEmpty());manager.cancelJob(jobId,&error);QTRY_VERIFY_WITH_TIMEOUT(!manager.hasLiveModelTasks(),30000);
        QCOMPARE(submitted.first().at(0).toString(),jobId);QVERIFY(!widget.bindCompletedNativeJob(jobId)); // Cancelled runs never bind a partial artifact.
        QVERIFY(status(widget).contains("completed successful result"));
        widget.applyPreview();QCOMPARE(manager.jobsQuery()["total"].toInt(),before+1);
    }
    void selectionRecoveryShowsModelTreeWithoutChangingGlobalSelection() {
        static QTemporaryDir workspace;QVERIFY(workspace.isValid());auto *session=Application::instance()->getSession();QString error;
        session->clear();QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));

        QMainWindow host;host.resize(430,560);
        auto *dock=new QDockWidget(QObject::tr("Models"),&host);dock->setObjectName("modelDock");
        auto *modelTabs=new QTabWidget(dock);
        auto *modelPage=new QScrollArea(modelTabs);modelPage->setObjectName("modelTabScroll");modelPage->setWidgetResizable(true);
        auto *modelTree=new ModelTree(Application::instance()->getActionList(),modelPage);modelPage->setWidget(modelTree);
        modelTabs->addTab(modelPage,QObject::tr("Model"));modelTabs->addTab(new QWidget(modelTabs),QObject::tr("Records"));
        dock->setWidget(modelTabs);host.addDockWidget(Qt::LeftDockWidgetArea,dock);
        auto *workbench=new PiezoWorkbenchWidget(&host);host.setCentralWidget(workbench);workbench->setSession(session);
        auto *source=workbench->findChild<QLabel*>("piezoSource");auto *choose=workbench->findChild<QPushButton*>("piezoChooseModel");
        QVERIFY(source);QVERIFY(choose);QVERIFY(source->wordWrap());QCOMPARE(choose->text(),QString("Choose model"));
        QCOMPARE(choose->accessibleName(),QString("Choose model"));QVERIFY(choose->accessibleDescription().contains("model tree"));
        host.show();host.raise();host.activateWindow();dock->hide();modelTabs->setCurrentIndex(1);QCoreApplication::processEvents();

        QVERIFY(source->text().contains("0 models selected"));QVERIFY(!choose->isHidden());
        choose->click();QTRY_VERIFY(dock->isVisible());QCOMPARE(modelTabs->currentWidget(),static_cast<QWidget*>(modelPage));
        QTRY_VERIFY(modelTree->hasFocus());QCOMPARE(session->getSelectedModelIDs().size(),0);

        session->addModel(beam());session->addModel(rods());
        for(const uint id:session->getSelectedModelIDs())session->setModelSelected(id,false);
        session->setModelSelected(0,true);session->setModelSelected(1,true);QCoreApplication::processEvents();workbench->refreshContext();
        const auto multipleSelection=session->getSelectedModelIDs();QCOMPARE(multipleSelection.size(),2);
        QVERIFY(source->text().contains("2 models selected"));QVERIFY(!choose->isHidden());
        dock->hide();modelTabs->setCurrentIndex(1);choose->click();QTRY_VERIFY(dock->isVisible());
        QCOMPARE(modelTabs->currentWidget(),static_cast<QWidget*>(modelPage));QTRY_VERIFY(modelTree->hasFocus());
        QCOMPARE(session->getSelectedModelIDs(),multipleSelection);

        session->setModelSelected(1,false);QCoreApplication::processEvents();workbench->refreshContext();
        QCOMPARE(session->getSelectedModelIDs().size(),1);QVERIFY(choose->isHidden());
        // Keep a recovery action available while the selected document is
        // temporarily unreadable, without changing the global selection.
        session->lock();workbench->refreshContext();
        const bool unavailableShown=source->text().contains("source document is not available");
        const bool recoveryShown=!choose->isHidden();
        session->unlock();
        QVERIFY(unavailableShown);QVERIFY(recoveryShown);
        QCOMPARE(session->getSelectedModelIDs().size(),1);
        workbench->refreshContext();QVERIFY(choose->isHidden());
    }
    void sensorOnlyRodObserverPreviewConfirmRunAndColdReopen() {try {
        QTemporaryDir artifacts;QVERIFY(artifacts.isValid());
        QTemporaryDir workspace;QVERIFY(workspace.isValid());auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));auto model=rods();
        auto declaration=RPiezoAxialExperimentService::exampleRecipe(model,{});declaration["dt"]=.004;declaration["steps"]=6;
        declaration["replaceLegacyConditions"]=true;
        model.getTimeSolver().setInputStartTime(1.304);
        // Explicit synthetic SI tensors keep this native GUI qualification at a modest frequency.
        auto materials=declaration["materials"].toArray();
        for(int i=0;i<materials.size();++i){auto material=materials[i].toObject();const bool piezo=material["piezoelectric"].toBool();
            const auto elastic=RStructuralPiezoContinuumMaterial::isotropic(100,.25,7);QJsonArray tensor,coupling,permittivity;
            for(double x:elastic.elasticity)tensor.append(x);for(int j=0;j<18;++j)coupling.append(piezo&&j==0?2.:0.);
            for(int j=0;j<9;++j)permittivity.append(piezo&&(j==0||j==4||j==8)?.5:0.);
            material["elasticityOrCompliance"]=tensor;material["coupling"]=coupling;material["permittivity"]=permittivity;material["density"]=7.;
            material["sourceId"]="independent-gui-rod-observer-example";material.remove("sourceSha256");
            material["sourceSha256"]=sha256(QJsonDocument(material).toJson(QJsonDocument::Compact));materials[i]=material;
        }declaration["materials"]=materials;
        declaration["initialDisplacements"]=QJsonArray{QJsonObject{{"physicalNodeId",112},{"values",QJsonArray{.02,0.,0.,0.,0.,0.}}},QJsonObject{{"physicalNodeId",123},{"values",QJsonArray{.04,0.,0.,0.,0.,0.}}}};
        declaration["damping"]=QJsonObject{{"kind","rayleigh"},{"rayleighAlpha",.06},{"rayleighBeta",.002}};
        declaration["excitation"]=QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId","node:123:X"},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","0.0003+0.002*t"}}}}}}}};
        const auto seed=RPiezoExperimentService::preview(model,declaration);QVERIFY2(seed.valid,qPrintable(seed.message));
        static_cast<RModel&>(model)=seed.candidate;session->addModel(model);session->setModelSelected(0,true);
        WorkflowWidget workflow;workflow.setSession(session);auto *embeddedPiezo=workflow.findChild<PiezoWorkbenchWidget*>();QVERIFY(embeddedPiezo);auto &widget=*embeddedPiezo;
        widget.findChild<QComboBox*>("piezoController")->setCurrentText("LQR");QCoreApplication::processEvents();
        auto *outputs=widget.findChild<QTableWidget*>("piezoControllerOutputs");QVERIFY(outputs);bool foundSensor=false;
        for(int row=0;row<outputs->rowCount();++row){const bool selected=outputs->item(row,1)->text()=="rod_514";foundSensor|=selected;outputs->item(row,0)->setCheckState(selected?Qt::Checked:Qt::Unchecked);}QVERIFY(foundSensor);
        auto *stateSource=widget.findChild<QComboBox*>("piezoStateSource");QVERIFY(stateSource);stateSource->setCurrentIndex(stateSource->findData("sensor_only"));QCoreApplication::processEvents();
        auto *interval=widget.findChild<QSpinBox*>("piezoControlEveryFESteps");QVERIFY(interval);interval->setValue(3);
        const auto controller=widget.draftRecipe()["controller"].toObject();QCOMPARE(controller["stateSource"].toString(),QString("sensor_only"));
        QCOMPARE(controller["selectedOutputs"].toArray(),QJsonArray({"rod_514"}));QCOMPARE(controller["observer"].toObject()["measurementCovariance"].toArray().size(),1);
        QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        widget.findChild<QPushButton*>("piezoConfirm")->click();QCOMPARE(applied.size(),1);QCOMPARE(session->getModel(0).getUndoStackSize(),1u);
        const auto saved=widget.draftRecipe();const auto sourcePath=workspace.filePath("observer-source.rbm");ModelIO::save(session->getModel(0),sourcePath,false);
        session->clear();Model reopenedSource;ModelIO::open(reopenedSource,sourcePath);session->addModel(reopenedSource);session->setModelSelected(0,true);widget.refreshContext();QCOMPARE(widget.draftRecipe(),saved);
        const RModel source=session->getModel(0);const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun reference(source,backend.callbacks);
        QCOMPARE(reference.acceptedRuntime().initialTime,source.getTimeSolver().getInputStartTime()-.004);
        QVERIFY(!backend.exportPackage["design"].toObject()["controller"].toObject()["observer"].toObject().isEmpty());
        QVERIFY(std::any_of(reference.acceptedRuntime().readouts.begin(),reference.acceptedRuntime().readouts.end(),[](const auto &x){return x.portId=="rod_514"&&!x.actionRecord&&x.deltaValue!=0.;}));
        while(reference.hasRemainingSteps()){auto prepared=reference.prepare(source);reference.validatePrepared(*prepared);reference.commit(*prepared);}
        const auto expected=RStructuralPiezoAxialRestartCodec::encode(reference.acceptedRuntime());
        auto &manager=SolverManager::getInstance();const int before=manager.jobsQuery()["total"].toInt();QSignalSpy submitted(&widget,&PiezoWorkbenchWidget::runSubmitted);
        widget.findChild<QPushButton*>("piezoRun")->click();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QCOMPARE(manager.jobsQuery()["total"].toInt(),before);QCOMPARE(submitted.size(),0);
        widget.findChild<QPushButton*>("piezoConfirm")->click();QVERIFY2(submitted.size()==1,qPrintable(status(widget)));QCOMPARE(manager.jobsQuery()["total"].toInt(),before+1);
        const auto jobs=manager.jobsQuery()["jobs"].toArray();QVERIFY(!jobs.isEmpty());const auto jobId=jobs.last().toObject()["jobId"].toString();QVERIFY(!jobId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(manager.jobQuery(jobId)["state"].toString()=="succeeded"||manager.jobQuery(jobId)["state"].toString()=="failed"||manager.jobQuery(jobId)["state"].toString()=="cancelled",120000);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.hasLiveModelTasks(),30000);const auto finished=manager.jobQuery(jobId);
        QVERIFY2(finished["state"].toString()=="succeeded",qPrintable(QString::fromUtf8(QJsonDocument(finished).toJson(QJsonDocument::Compact))));
        QVERIFY(finished["processStarted"].toBool());QVERIFY(finished["processExited"].toBool());QCOMPARE(finished["processExitCode"].toInt(),0);
        QVERIFY(finished["resultArtifactVerified"].toBool());QVERIFY(finished["resultSourceBindingVerified"].toBool());QVERIFY(finished["resultManifestVerified"].toBool());
        QCOMPARE(submitted.size(),1);QCOMPARE(submitted.first().at(0).toString(),jobId);
        const auto resultPath=finished["resultArtifactRef"].toString();QVERIFY(QFileInfo::exists(resultPath));
        auto *acceptedPlot=widget.findChild<SeriesPlotWidget*>("piezoAcceptedPlot");QVERIFY(acceptedPlot);
        // WorkflowWidget remains hidden here: its timer must process the exact
        // runSubmitted ID by direct jobQuery, independently of selector pages.
        QTRY_VERIFY2_WITH_TIMEOUT(!acceptedPlot->dataset().traces.isEmpty(),
            qPrintable(QStringLiteral("status=%1; pending=%2; bindFailures=%3; jobStamp=%4; currentDoc=%5; attachRecord=%6")
                .arg(status(widget))
                .arg(workflow.pendingPiezoResultJobs.contains(jobId))
                .arg(workflow.pendingPiezoResultFailures.value(jobId,-1))
                .arg(QString::fromUtf8(QJsonDocument(finished["sourceRuntimeStamp"].toObject()).toJson(QJsonDocument::Compact)))
                .arg(QString::fromUtf8(QJsonDocument(widget.currentDocument()).toJson(QJsonDocument::Compact)))
                .arg(QString::fromUtf8(QJsonDocument(widget.appliedNativeResults.value(QFileInfo(resultPath).canonicalFilePath())).toJson(QJsonDocument::Compact)))),5000);
        const auto boundGeneration=acceptedPlot->datasetGeneration();QVERIFY(widget.bindCompletedNativeJob(jobId));QCOMPARE(acceptedPlot->datasetGeneration(),boundGeneration);
        // The hidden workflow must retain an exact successful job ID while its
        // source snapshot is temporarily locked, then bind on the next poll.
        widget.clearAcceptedSeries(QStringLiteral("Exercise a transient source lock"));
        widget.runSubmitted(jobId);
        session->lock();
        workflow.refreshNativeJobs();
        const bool keptBusyJob=workflow.pendingPiezoResultJobs.contains(jobId);
        const bool displayStillBlank=acceptedPlot->dataset().traces.isEmpty();
        session->unlock();
        QVERIFY(keptBusyJob);QVERIFY(displayStillBlank);
        workflow.refreshNativeJobs();
        QVERIFY(!workflow.pendingPiezoResultJobs.contains(jobId));
        QVERIFY(!acceptedPlot->dataset().traces.isEmpty());
        const auto csvPath=artifacts.filePath("bound-result.csv");QVERIFY2(widget.exportCurrentAcceptedTimeSeriesTo(csvPath),qPrintable(status(widget)));
        QFile csv(csvPath);QVERIFY(csv.open(QIODevice::ReadOnly));const auto csvBytes=csv.readAll();
        const auto csvMetadata=QJsonDocument::fromJson(csvBytes.left(csvBytes.indexOf('\n')).mid(2)).object();
        QCOMPARE(csvMetadata["sourceFingerprint"].toString(),reference.acceptedRuntime().sourceFingerprint);
        QVERIFY(!widget.bindCompletedNativeJob(jobId+QStringLiteral("-foreign")));
        QVERIFY(!acceptedPlot->dataset().traces.isEmpty());
        const auto tracePath=widget.acceptedTraceProof.tracePath;QVERIFY(QFileInfo::exists(tracePath));
        const QFileInfo originalTraceInfo(tracePath);const auto originalTraceTime=originalTraceInfo.lastModified();QVERIFY(originalTraceTime.isValid());
        QFile trace(tracePath);QVERIFY(trace.open(QIODevice::ReadWrite));const auto originalTrace=trace.readAll();QVERIFY(!originalTrace.isEmpty());
        QByteArray changedTrace=originalTrace;changedTrace[0]=changedTrace[0]=='{'?'[':'{';
        QVERIFY(trace.seek(0));QCOMPARE(trace.write(changedTrace),qint64(changedTrace.size()));QVERIFY(trace.flush());
        QVERIFY(trace.setFileTime(originalTraceTime,QFileDevice::FileModificationTime));trace.close();
        QCOMPARE(QFileInfo(tracePath).size(),originalTraceInfo.size());QCOMPARE(QFileInfo(tracePath).lastModified(),originalTraceTime);
        QTRY_VERIFY_WITH_TIMEOUT(acceptedPlot->dataset().traces.isEmpty(),5000);
        const auto staleTraceCsv=artifacts.filePath("stale-trace.csv");
        QVERIFY(!widget.exportCurrentAcceptedTimeSeriesTo(staleTraceCsv));QVERIFY(!QFileInfo::exists(staleTraceCsv));
        QVERIFY(trace.open(QIODevice::WriteOnly));QCOMPARE(trace.write(originalTrace),qint64(originalTrace.size()));
        QVERIFY(trace.flush());QVERIFY(trace.setFileTime(originalTraceTime,QFileDevice::FileModificationTime));trace.close();
        QVERIFY2(widget.bindCompletedNativeJob(jobId),qPrintable(status(widget)));
        QVERIFY(!acceptedPlot->dataset().traces.isEmpty());
        const auto registryPath=finished["nativeJobRecordRef"].toString();QVERIFY(QFileInfo::exists(registryPath));
        const QFileInfo originalRegistryInfo(registryPath);const auto originalRegistryTime=originalRegistryInfo.lastModified();QVERIFY(originalRegistryTime.isValid());
        QFile registry(registryPath);QVERIFY(registry.open(QIODevice::ReadWrite));const auto originalRegistry=registry.readAll();QVERIFY(!originalRegistry.isEmpty());
        QCOMPARE(qint64(originalRegistry.size()),originalRegistryInfo.size());QVERIFY(registry.seek(originalRegistry.size()));QVERIFY(registry.write("post-bind registry tamper\n")>0);
        QVERIFY(registry.flush());QVERIFY(registry.resize(originalRegistry.size()));
        QVERIFY(registry.setFileTime(originalRegistryTime,QFileDevice::FileModificationTime));registry.close();
        QCOMPARE(QFileInfo(registryPath).size(),originalRegistryInfo.size());QCOMPARE(QFileInfo(registryPath).lastModified(),originalRegistryTime);
        QTRY_VERIFY_WITH_TIMEOUT(acceptedPlot->dataset().traces.isEmpty(),5000);
        QVERIFY2(widget.bindCompletedNativeJob(jobId),qPrintable(status(widget)));
        QVERIFY(!acceptedPlot->dataset().traces.isEmpty());
        const auto manifestPath=finished["resultManifestRef"].toString();QVERIFY(QFileInfo::exists(manifestPath));
        const QFileInfo originalManifestInfo(manifestPath);const auto originalManifestTime=originalManifestInfo.lastModified();
        QFile manifest(manifestPath);QVERIFY(manifest.open(QIODevice::ReadWrite));const auto originalManifest=manifest.readAll();QVERIFY(!originalManifest.isEmpty());
        QVERIFY(originalManifestTime.isValid());QCOMPARE(qint64(originalManifest.size()),originalManifestInfo.size());QVERIFY(manifest.seek(originalManifest.size()));QVERIFY(manifest.write("post-bind manifest tamper\n")>0);
        QVERIFY(manifest.flush());QVERIFY(manifest.resize(originalManifest.size()));
        QVERIFY(manifest.setFileTime(originalManifestTime,QFileDevice::FileModificationTime));manifest.close();
        QCOMPARE(QFileInfo(manifestPath).size(),originalManifestInfo.size());QCOMPARE(QFileInfo(manifestPath).lastModified(),originalManifestTime);
        // The file watcher must revoke on the observed write even though the
        // test restored the bytes and metadata before the next 500 ms check.
        QTRY_VERIFY_WITH_TIMEOUT(acceptedPlot->dataset().traces.isEmpty(),5000);
        QVERIFY2(widget.bindCompletedNativeJob(jobId),qPrintable(status(widget)));
        QVERIFY(!acceptedPlot->dataset().traces.isEmpty());
        // Equal-size/equal-mtime tamper evades the fast display snapshot. Export
        // must still perform full native certification before creating a CSV.
        QVERIFY(manifest.open(QIODevice::ReadWrite));QByteArray stealthTamper=originalManifest;stealthTamper[0]=stealthTamper[0]=='{'?'[':'{';
        QVERIFY(manifest.seek(0));QCOMPARE(manifest.write(stealthTamper),qint64(stealthTamper.size()));
        QVERIFY(manifest.resize(stealthTamper.size()));QVERIFY(manifest.flush());
        QVERIFY(manifest.setFileTime(originalManifestTime,QFileDevice::FileModificationTime));manifest.close();
        QCOMPARE(QFileInfo(manifestPath).size(),originalManifestInfo.size());QCOMPARE(QFileInfo(manifestPath).lastModified(),originalManifestTime);
        const auto revokedCsv=artifacts.filePath("revoked-result.csv");
        QVERIFY(!widget.exportCurrentAcceptedTimeSeriesTo(revokedCsv));QVERIFY(!QFileInfo::exists(revokedCsv));
        QVERIFY(acceptedPlot->dataset().traces.isEmpty());
        session->clear();
        Model reopenedResult;ModelIO::open(reopenedResult,resultPath);QCOMPARE(RPiezoExperimentService::recipeFromModel(reopenedResult),saved);
        reopenedResult.getProblemSetup().setRestart(true);const auto resumedBackend=PiezoAxialBackendBridge::create(reopenedResult);RStructuralPiezoAxialNativeRun resumed(reopenedResult,resumedBackend.callbacks);
        QVERIFY(!resumed.hasRemainingSteps());QCOMPARE(resumed.completedSteps(),6u);QCOMPARE(resumed.acceptedRuntime().controlTick,2u);
        QCOMPARE(RStructuralPiezoAxialRestartCodec::encode(resumed.acceptedRuntime()),expected);QCOMPARE(resumed.acceptedRuntime().opaqueControllerState,reference.acceptedRuntime().opaqueControllerState);
        QCOMPARE(resumed.acceptedState().displacements,reference.acceptedState().displacements);QCOMPARE(resumed.acceptedState().velocities,reference.acceptedState().velocities);QCOMPARE(resumed.acceptedState().accelerations,reference.acceptedState().accelerations);
        session->addModel(reopenedResult);session->setModelSelected(0,true);widget.refreshContext();QCOMPARE(widget.draftRecipe(),saved);
        QVERIFY(!widget.bindCompletedNativeJob(jobId)); // The exact old job ID is rejected against the newly loaded source identity.
        QVERIFY(!widget.bindCompletedNativeJob(jobId)); // The previous job is stale after the widget changes source documents.
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }
    void templateEditsUpdateDefaultsAndPreserveCustomLayers() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));Model empty;empty.setName("Empty template source");empty.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));session->addModel(empty);session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);
        const auto sourceLayers=[&]{const QJsonObject snapshot=widget.draftRecipe();const QJsonArray members=snapshot.value("members").toArray();return members.first().toObject().value("layers").toArray();};
        auto *thickness=widget.findChild<QDoubleSpinBox*>("piezoTemplatethickness");QVERIFY(thickness);thickness->setValue(.02);
        auto layers=sourceLayers();QCOMPARE(layers.at(0).toObject().value("lowerZ").toDouble(),-.01);QCOMPARE(layers.at(0).toObject().value("upperZ").toDouble(),.01);QCOMPARE(layers.at(1).toObject().value("lowerZ").toDouble(),.01);QCOMPARE(session->getModel(0).getNNodes(),0u);
        qInfo("PIEZO_TEMPLATE_STAGE before first queued rebuild");QCoreApplication::processEvents();qInfo("PIEZO_TEMPLATE_STAGE first queued rebuild completed");
        auto *layerTable=widget.findChild<QTableWidget*>("piezoLayers");QVERIFY(layerTable);QVERIFY(!layerTable->isColumnHidden(4));QVERIFY(layerTable->rowCount()>1);auto *patchWidth=qobject_cast<QDoubleSpinBox*>(layerTable->cellWidget(1,4));QVERIFY(patchWidth);QVERIFY(patchWidth->isEnabled());patchWidth->setValue(.12);
        const QJsonValue custom=sourceLayers().at(1);auto *length=widget.findChild<QDoubleSpinBox*>("piezoTemplatelength");QVERIFY(length);length->setValue(2.);QCOMPARE(sourceLayers().at(1),custom);
        qInfo("PIEZO_TEMPLATE_STAGE before length queued rebuild");QCoreApplication::processEvents();qInfo("PIEZO_TEMPLATE_STAGE length queued rebuild completed");
        auto *width=widget.findChild<QDoubleSpinBox*>("piezoTemplatewidth");QVERIFY(width);width->setValue(.2);layers=sourceLayers();QCOMPARE(layers.at(0).toObject().value("width").toDouble(),.2);QCOMPARE(layers.at(1),custom);
        qInfo("PIEZO_TEMPLATE_STAGE before width queued rebuild");QCoreApplication::processEvents();qInfo("PIEZO_TEMPLATE_STAGE width queued rebuild completed");
        auto *kind=widget.findChild<QComboBox*>("piezoTemplateKind");QVERIFY(kind);kind->setCurrentText("flatPlate");const QJsonObject plateDraft=widget.draftRecipe();QCOMPARE(plateDraft.value("family").toString(),QString("flatPlate"));
        const QJsonArray plateMembers=plateDraft.value("members").toArray();for(const QJsonValue &member:plateMembers){const QJsonArray plateLayers=member.toObject().value("layers").toArray();for(const QJsonValue &layer:plateLayers)QCOMPARE(layer.toObject().value("width").toDouble(),1.);}
        QCoreApplication::processEvents();layerTable=widget.findChild<QTableWidget*>("piezoLayers");QVERIFY(layerTable);QVERIFY(layerTable->isColumnHidden(4));
        for(int row=0;row<layerTable->rowCount();++row)QVERIFY(!layerTable->cellWidget(row,4));
        width=widget.findChild<QDoubleSpinBox*>("piezoTemplatewidth");QVERIFY(width);width->setValue(.3);QCoreApplication::processEvents();
        QCOMPARE(widget.draftRecipe()["template"].toObject()["width"].toDouble(),.3);
        for(const auto &member:widget.draftRecipe()["members"].toArray())for(const auto &layer:member.toObject()["layers"].toArray())QCOMPARE(layer.toObject()["width"].toDouble(),1.);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QCOMPARE(session->getModel(0).getNNodes(),0u);
    }
    void realModalClustersDriveSelectionAndDamping() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        widget.computeModes();QCoreApplication::processEvents();
        auto *modeTable=widget.findChild<QTableWidget*>("piezoModes");QVERIFY(modeTable);QVERIFY(modeTable->item(0,2)->text().toDouble()>0);
        widget.findChild<QComboBox*>("piezoDamping")->setCurrentText("perCluster");QCoreApplication::processEvents();
        modeTable=widget.findChild<QTableWidget*>("piezoModes");
        auto damping=widget.draftRecipe()["damping"].toObject();auto *clusters=widget.findChild<QTableWidget*>("piezoDampingClusters");QVERIFY(clusters);
        QCOMPARE(damping["ratios"].toArray().size(),clusters->rowCount());QCOMPARE(damping["clusterEnds"].toArray().last().toInt(),modeTable->rowCount());
        widget.findChild<QDoubleSpinBox*>("piezoClusterRatio0")->setValue(.025);
        QCOMPARE(widget.draftRecipe()["damping"].toObject()["ratios"].toArray().first().toDouble(),.025);
        widget.findChild<QCheckBox*>("piezoAllModes")->setChecked(false);modeTable=widget.findChild<QTableWidget*>("piezoModes");modeTable->item(0,0)->setCheckState(Qt::Unchecked);
        const auto selected=widget.draftRecipe()["design"].toObject()["modeIndices"].toArray();const int end=damping["clusterEnds"].toArray().first().toInt();
        for(int i=0;i<end;++i)QVERIFY(!selected.contains(i));
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        widget.findChild<QDoubleSpinBox*>("piezoLayerThickness0_1")->setValue(.0008);
        QVERIFY(!widget.hasCurrentPreview());widget.previewExperiment();QVERIFY(!widget.hasCurrentPreview());QVERIFY(status(widget).contains("clusters"));
    }
    void exportsUseBoundSourceAndCertifiedAcceptedSamples() {
        static QTemporaryDir workspace;static QTemporaryDir artifacts;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        QVERIFY(!widget.exportMatrixPackageTo(artifacts.filePath("unapplied.json")));
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(3);widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();
        const auto stamp=session->modelCommandStamp(0);const auto history=session->getModel(0).getUndoStackSize();
        const auto packagePath=artifacts.filePath("operators.json");QVERIFY2(widget.exportMatrixPackageTo(packagePath),qPrintable(status(widget)));
        QFile packageFile(packagePath);QVERIFY(packageFile.open(QIODevice::ReadOnly));const auto package=QJsonDocument::fromJson(packageFile.readAll()).object();
        QCOMPARE(package["nodes"].toArray().size(),3);QVERIFY(package.contains("M"));QVERIFY(package.contains("G"));QVERIFY(package.contains("design"));
        const RModel source=session->getModel(0);const auto backend=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);QStringList records;
        for(int i=0;i<3;++i){if(i){auto prepared=run.prepare(source);run.validatePrepared(*prepared);run.commit(*prepared);}auto accepted=run.acceptedModel(source);auto path=artifacts.filePath(QString("accepted-%1.rbm").arg(i));records.append(accepted.nativeWritePaths(path,false).first());accepted.write(path,false);}
        const auto csvPath=artifacts.filePath("selected.csv");QVERIFY2(widget.exportAcceptedTimeSeriesTo({records[0],records[2]},csvPath),qPrintable(status(widget)));
        QFile csv(csvPath);QVERIFY(csv.open(QIODevice::ReadOnly));auto bytes=csv.readAll();const auto meta=QJsonDocument::fromJson(bytes.left(bytes.indexOf('\n')).mid(2)).object();
        QCOMPARE(meta["recordCount"].toInt(),2);QVERIFY(!meta["continuousBetweenSelectedRecords"].toBool());QVERIFY(status(widget).contains("gaps"));
        QCOMPARE(session->modelCommandStamp(0),stamp);QCOMPARE(session->getModel(0).getUndoStackSize(),history);
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(4);QVERIFY(!widget.exportMatrixPackageTo(packagePath));
        QFile unchanged(packagePath);QVERIFY(unchanged.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(unchanged.readAll()).object(),package);
    }
    void axialPicksEditorRolesAndSourceInvalidation() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(rods());session->setModelSelected(0,true);
        session->getPickList().addItem(PickItem(SessionEntityID(0,R_ENTITY_GROUP_LINE,0),1,1));
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        QCOMPARE(widget.draftRecipe()["family"].toString(),QString("axialRod"));auto members=widget.draftRecipe()["members"].toArray();
        QCOMPARE(members[0].toObject()["kind"].toString(),QString("passive"));QCOMPARE(members[1].toObject()["kind"].toString(),QString("active"));
        QVERIFY(widget.findChild<QTableWidget*>("piezoAxialMembers"));QVERIFY(!widget.findChild<QTableWidget*>("piezoLayers"));
        auto *supports=widget.findChild<QTableWidget*>("piezoSupports");QCOMPARE(qobject_cast<QComboBox*>(supports->cellWidget(0,1))->count(),3);
        const auto captured=widget.draftRecipe();session->getPickList().clear();QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe(),captured);
        auto *ports=widget.findChild<QTableWidget*>("piezoPorts");ports->selectRow(0);widget.findChild<QPushButton*>("piezoRemovePort")->click();QCOMPARE(widget.draftRecipe()["ports"].toArray().size(),1);QVERIFY(status(widget).contains("bound"));
        // Passive/cable changes preserve the selected material and actual physical binding.
        widget.findChild<QComboBox*>("piezoAxialKind0")->setCurrentText("cable");QCoreApplication::processEvents();
        QCOMPARE(widget.draftRecipe()["members"].toArray()[0].toObject()["initialTension"].toDouble(),0.);
        widget.findChild<QComboBox*>("piezoAxialKind0")->setCurrentText("passive");QCoreApplication::processEvents();
        widget.findChild<QDoubleSpinBox*>("piezoAxialArea1")->setValue(.002);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        widget.findChild<QDoubleSpinBox*>("piezoAxialArea1")->setValue(.003);QVERIFY(!widget.hasCurrentPreview());
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();
        QVERIFY(session->getModel(0).getProblemSetup().getStructuralPiezoAxialSetup().enabled);QCOMPARE(session->getModel(0).getUndoStackSize(),1u);
        QCOMPARE(widget.draftRecipe()["members"].toArray()[1].toObject()["area"].toDouble(),.003);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        session->getModel(0).setDescription("Accepted source edit");session->setModelChanged(0);QVERIFY(!widget.hasCurrentPreview());
    }
    void memberRowsAndAutomaticGroupingExposePhysicalElementIds_data() {
        QTest::addColumn<QString>("family");QTest::newRow("rod")<<QString("axialRod");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");
    }
    void memberRowsAndAutomaticGroupingExposePhysicalElementIds() {try {
        QFETCH(QString,family);static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(family=="axialRod"?rods():family=="flatPlate"?plate():beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);const auto members=widget.draftRecipe().value("members").toArray();
        if(family=="axialRod"){
            auto *rows=widget.findChild<QTableWidget*>("piezoAxialMembers");QVERIFY(rows);QCOMPARE(rows->rowCount(),members.size());
            for(int i=0;i<members.size();++i){const auto id=QString::number(static_cast<qulonglong>(members[i].toObject().value("physicalElementId").toDouble()));
                QVERIFY2(rows->item(i,0)->text().contains(QString("(physical ID %1)").arg(id)),qPrintable(rows->item(i,0)->text()));}
            auto *area=widget.findChild<QDoubleSpinBox*>("piezoAxialArea1");QVERIFY(area);area->setValue(members.first().toObject().value("area").toDouble()*2.);
            widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
            auto *summary=widget.findChild<QTableWidget*>("piezoChanges");QVERIFY(summary);QStringList groupingRows;
            for(int row=0;row<summary->rowCount();++row)if(summary->item(row,0)->text()==QStringLiteral("Auto-separated member properties"))groupingRows.append(summary->item(row,1)->text());
            const QString groupingText=groupingRows.join(QStringLiteral(" | "));QVERIFY2(!groupingRows.isEmpty(),qPrintable(groupingText));
            QVERIFY(groupingText.contains(QStringLiteral("physical element IDs: 501")));QVERIFY(groupingText.contains(QStringLiteral("physical element IDs: 514")));
        }else{
            auto *rows=widget.findChild<QTableWidget*>("piezoLayers");QVERIFY(rows);QVERIFY(rows->rowCount()>0);
            bool firstId=false,secondId=false;for(int row=0;row<rows->rowCount();++row){const QString label=rows->item(row,0)->text();
                firstId|=label.contains(QStringLiteral("(physical ID 501)"));secondId|=label.contains(QStringLiteral("(physical ID 514)"));}
            QVERIFY(firstId);QVERIFY(secondId);
        }
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }
    void axialTemplateRebindsIdsAndPreservesCustomDeclarations() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));Model empty;empty.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));session->addModel(empty);session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);widget.findChild<QComboBox*>("piezoTemplateKind")->setCurrentText("axialTruss");QCoreApplication::processEvents();
        QCOMPARE(widget.draftRecipe()["family"].toString(),QString("axialRod"));
        auto *ports=widget.findChild<QTableWidget*>("piezoPorts");qobject_cast<QComboBox*>(ports->cellWidget(0,1))->setCurrentText("actuatorSensor");
        auto *area=widget.findChild<QDoubleSpinBox*>("piezoAxialArea0");QVERIFY(area);area->setValue(.004);
        const auto materials=widget.draftRecipe()["materials"].toArray();
        widget.findChild<QSpinBox*>("piezoTemplateElements")->setValue(5);QCoreApplication::processEvents();
        QCOMPARE(widget.draftRecipe()["members"].toArray().size(),5);QCOMPARE(widget.draftRecipe()["ports"].toArray().size(),5);
        QCOMPARE(widget.draftRecipe()["members"].toArray()[0].toObject()["area"].toDouble(),.004);
        QCOMPARE(widget.draftRecipe()["ports"].toArray()[0].toObject()["role"].toString(),QString("actuatorSensor"));QCOMPARE(widget.draftRecipe()["materials"].toArray(),materials);
        widget.findChild<QDoubleSpinBox*>("piezoTemplatethickness")->setValue(.02);QCoreApplication::processEvents();
        QCOMPARE(widget.draftRecipe()["members"].toArray()[1].toObject()["area"].toDouble(),.002);
        widget.computeModes();QCoreApplication::processEvents();auto *modes=widget.findChild<QTableWidget*>("piezoModes");QVERIFY(modes);QCOMPARE(modes->rowCount(),5);QVERIFY(modes->item(0,2)->text().toDouble()>0);
        widget.findChild<QComboBox*>("piezoDamping")->setCurrentText("uniformModal");QCoreApplication::processEvents();widget.findChild<QDoubleSpinBox*>("piezoDampingRatio")->setValue(.025);
        widget.findChild<QComboBox*>("piezoDamping")->setCurrentText("rayleigh");QCoreApplication::processEvents();QVERIFY(!widget.draftRecipe()["damping"].toObject().contains("uniformRatio"));
        widget.findChild<QComboBox*>("piezoDamping")->setCurrentText("uniformModal");QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe()["damping"].toObject()["uniformRatio"].toDouble(),.025);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QCOMPARE(session->getModel(0).getNNodes(),0u);widget.applyPreview();
        QCOMPARE(session->getModel(0).getNNodes(),6u);QCOMPARE(session->getModel(0).getUndoStackSize(),1u);
        QCOMPARE(session->getSelectedModelIDs(),QList<uint>{0});const auto authoritative=RPiezoExperimentService::recipeFromModel(session->getModel(0));QVERIFY2(!authoritative.isEmpty(),"Applied axial source could not reconstruct its authoritative recipe");QCOMPARE(widget.draftRecipe(),authoritative);
        auto *run=widget.findChild<QPushButton*>("piezoRun");QVERIFY2(run->isEnabled(),qPrintable(run->property("unavailableReason").toString()+" | "+status(widget)));
        QVERIFY(!widget.draftRecipe().contains("template")); // Saved generated mesh is authoritative.
    }
    void externalForceFilesAreFrozenAndRequireExplicitReload() {
        static QTemporaryDir workspace;QTemporaryDir files;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(rods());session->setModelSelected(0,true);
        const QByteArray code="def force(t):\n    return {'node:123:X': {'value': t, 'unit': 'N'}}\n";
        const auto sourcePath=files.filePath("force.py"),dependencyPath=files.filePath("gain.py");
        QFile source(sourcePath);QVERIFY(source.open(QIODevice::WriteOnly));QCOMPARE(source.write(code),qint64(code.size()));source.close();QFile dependency(dependencyPath);QVERIFY(dependency.open(QIODevice::WriteOnly));dependency.write("gain = 1\n");dependency.close();
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);QVERIFY2(widget.addExternalForceFunctionFrom(sourcePath,"force",{dependencyPath}),qPrintable(status(widget)));QCoreApplication::processEvents();
        auto function=widget.draftRecipe()["excitation"].toObject()["externalFunctions"].toArray().first().toObject();
        QCOMPARE(function["sourceSha256"].toString(),sha256(code));QVERIFY(QFileInfo(function["sourcePath"].toString()).isAbsolute());QCOMPARE(function["missingPolicy"].toString(),QString("error"));QVERIFY(!function["cacheExactTime"].toBool());
        for(const auto &key:{"pythonVersion","numpyVersion","scipyVersion"})QVERIFY(!function["environment"].toObject()[key].toString().isEmpty());
        auto channel=function["channels"].toArray().first().toObject();QCOMPARE(channel.size(),2);QCOMPARE(channel["channelId"].toString(),QString("node:123:X"));QCOMPARE(channel["unit"].toString(),QString("N"));
        auto *channels=widget.findChild<QTableWidget*>("piezoExternalChannels0");QVERIFY(channels);QCOMPARE(qobject_cast<QComboBox*>(channels->cellWidget(0,1))->count(),3);
        widget.findChild<QPushButton*>("piezoAddForce")->click();QCoreApplication::processEvents(); // Built-in and external forces share the real nodal channel.
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        QVERIFY(dependency.open(QIODevice::Append));dependency.write("# dependency revision\n");dependency.close();
        // The 500 ms validity path must revoke from cached metadata without a
        // full source/dependency SHA read or an explicit hasCurrentPreview call.
        QTRY_VERIFY_WITH_TIMEOUT(status(widget).contains("External force source or dependency changed"),3000);
        widget.applyPreview();QCOMPARE(session->getModel(0).getUndoStackSize(),0u);
        widget.previewExperiment();QVERIFY(!widget.hasCurrentPreview());QVERIFY(status(widget).contains("Reload"));QCOMPARE(widget.draftRecipe()["excitation"].toObject()["externalFunctions"].toArray().first().toObject(),function);
        widget.findChild<QPushButton*>("piezoExternalReload0")->click();QCoreApplication::processEvents();widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();QVERIFY(widget.findChild<QPushButton*>("piezoRun")->isEnabled());
        const auto saved=widget.draftRecipe();QVERIFY(source.open(QIODevice::Append));source.write("# source revision\n");source.close();
        widget.previewRun();QVERIFY(!widget.hasCurrentPreview());QVERIFY(status(widget).contains("Reload"));QCOMPARE(widget.draftRecipe(),saved);
        widget.findChild<QPushButton*>("piezoExternalReload0")->click();QCoreApplication::processEvents();QVERIFY(widget.draftRecipe()!=saved);QVERIFY(!widget.findChild<QPushButton*>("piezoRun")->isEnabled());
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
    }
    void scientificEditorsPreserveSmallMaterialAndElectrodeValues() {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));Model model=beam();auto recipe=RPiezoExperimentService::exampleRecipe(model);recipe["replaceLegacyConditions"]=true;
        auto materials=recipe["materials"].toArray();auto host=materials[0].toObject();host["form"]="strainCharge";
        QJsonArray compliance;for(int r=0;r<6;++r)for(int c=0;c<6;++c)compliance.append(r==c?(r<3?1./7e10:2.6/7e10):(r<3&&c<3?-.3/7e10:0.));host["elasticityOrCompliance"]=compliance;materials[0]=host;
        const double epsilon=1.2345678901234568e-12,capacitance=1.2345678901234568e-13;
        auto piezo=materials[1].toObject();auto permittivity=piezo["permittivity"].toArray();for(int i:{0,4,8})permittivity[i]=epsilon;piezo["permittivity"]=permittivity;materials[1]=piezo;recipe["materials"]=materials;
        auto ports=recipe["ports"].toArray();auto port=ports[1].toObject();port["readout"]="idealChargeAmplifier";port["Cf"]=capacitance;ports[1]=port;recipe["ports"]=ports;
        const auto preview=RPiezoExperimentService::preview(model,recipe);QVERIFY2(preview.valid,qPrintable(preview.message));static_cast<RModel&>(model)=preview.candidate;session->addModel(model);session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);const auto loaded=widget.draftRecipe();QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe(),loaded);
        auto *elasticity=widget.findChild<QTableWidget*>("piezoMaterial0elasticityOrCompliance"),*dielectric=widget.findChild<QTableWidget*>("piezoMaterial1permittivity");QVERIFY(elasticity);QVERIFY(dielectric);
        QCOMPARE(elasticity->item(0,0)->text().toDouble(),1./7e10);QCOMPARE(dielectric->item(0,0)->text().toDouble(),epsilon);
        auto *portTable=widget.findChild<QTableWidget*>("piezoPorts");auto *cf=qobject_cast<QDoubleSpinBox*>(portTable->cellWidget(1,3));QVERIFY(cf);QCOMPARE(cf->value(),capacitance);QCOMPARE(cf->text().toDouble(),capacitance);QVERIFY(cf->text().contains('e'));
        const double changed=9.876543210987654e-14;auto *edit=cf->findChild<QLineEdit*>();QVERIFY(edit);edit->setText(QString::number(changed,'g',17));cf->interpretText();
        QCOMPARE(cf->value(),changed);QCOMPARE(widget.draftRecipe()["ports"].toArray().at(1).toObject().value("Cf").toDouble(),changed);
        dielectric->item(0,0)->setText("2.345678901234567e-12");QCOMPARE(widget.draftRecipe()["materials"].toArray()[1].toObject()["permittivity"].toArray()[0].toDouble(),2.345678901234567e-12);
        widget.findChild<QComboBox*>("piezoController")->setCurrentText("PI");QCoreApplication::processEvents();
        portTable=widget.findChild<QTableWidget*>("piezoPorts");cf=qobject_cast<QDoubleSpinBox*>(portTable->cellWidget(1,3));QCOMPARE(cf->value(),changed);QCOMPARE(cf->text().toDouble(),changed);
        const double beforeStep=cf->value();cf->stepUp();QVERIFY(cf->value()>beforeStep);QVERIFY(cf->value()<beforeStep*1.2);
    }
    void forceDirectionsAndExternalTimeFunctionsAcrossFamilies_data() {
        QTest::addColumn<QString>("family");QTest::newRow("rod")<<QString("axialRod");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");
    }
    void forceDirectionsAndExternalTimeFunctionsAcrossFamilies() {
        QFETCH(QString,family);static QTemporaryDir workspace;QTemporaryDir files;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(family=="axialRod"?rods():family=="flatPlate"?plate():beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.findChild<QPushButton*>("piezoAddForce")->click();QCoreApplication::processEvents();
        auto *builtin=widget.findChild<QComboBox*>("piezoForceDirection0");QVERIFY(builtin);const QStringList expected=family=="axialRod"?QStringList{"X","Y","Z"}:family=="planarBeam"?QStringList{"X","Z","RY","local:EX","local:EZ","local:RY"}:QStringList{"X","Y","Z","RX","RY","local:EX","local:EY","local:EZ","local:RX","local:RY"};
        QCOMPARE(builtin->count(),expected.size());for(const auto &direction:expected)QVERIFY(builtin->findData(direction)>=0);
        QVERIFY(builtin->findData("local:RZ")<0);QVERIFY(builtin->findData("RZ")<0);if(family=="planarBeam"){QVERIFY(builtin->findData("Y")<0);QVERIFY(builtin->findData("local:RX")<0);}
        const auto channel=widget.draftRecipe()["excitation"].toObject()["channels"].toArray().first().toObject();
        const auto sourcePath=files.filePath("force.py"),dependencyPath=files.filePath("data.txt");QFile dependency(dependencyPath);QVERIFY(dependency.open(QIODevice::WriteOnly));dependency.write("declared data\n");dependency.close();
        const auto writeFunction=[&](const QString &name,const QString &unit){QFile source(sourcePath);if(!source.open(QIODevice::WriteOnly))return false;const auto bytes=QString("def force(t):\n    return {%1: {'value': t, 'unit': %2}}\n").arg(QString::fromUtf8(QJsonDocument(QJsonArray{name}).toJson(QJsonDocument::Compact)).mid(1).chopped(1),QString::fromUtf8(QJsonDocument(QJsonArray{unit}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)).toUtf8();return source.write(bytes)==bytes.size();};
        QVERIFY(writeFunction(channel["channelId"].toString(),"N"));QVERIFY2(widget.addExternalForceFunctionFrom(sourcePath,"force",{dependencyPath}),qPrintable(status(widget)));QCoreApplication::processEvents();
        auto *external=widget.findChild<QComboBox*>("piezoExternalForceDirection0_0");QVERIFY(external);QCOMPARE(external->count(),expected.size());for(const auto &direction:expected)QVERIFY(external->findData(direction)>=0);
        if(family!="axialRod"){
            external->setCurrentIndex(external->findData("local:RY"));QCOMPARE(external->currentText(),QString("Local RY"));const auto force=widget.draftRecipe()["excitation"].toObject()["externalFunctions"].toArray().first().toObject()["channels"].toArray().first().toObject();
            QVERIFY(force["channelId"].toString().endsWith(":local:RY"));QCOMPARE(force["unit"].toString(),QString("N m"));auto *table=widget.findChild<QTableWidget*>("piezoExternalChannels0");QCOMPARE(qobject_cast<QLabel*>(table->cellWidget(0,2))->text(),force["channelId"].toString());QCOMPARE(qobject_cast<QLabel*>(table->cellWidget(0,3))->text(),QString("N m"));
            QVERIFY(writeFunction(force["channelId"].toString(),"N m"));widget.findChild<QPushButton*>("piezoExternalReload0")->click();QCoreApplication::processEvents();
        }
        QVERIFY2(nullRecipePath(widget.draftRecipe()).isEmpty(),qPrintable(nullRecipePath(widget.draftRecipe())));widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);widget.applyPreview();QCOMPARE(applied.size(),1);
        const auto saved=widget.draftRecipe();const auto path=files.filePath("saved.rbm");ModelIO::save(session->getModel(0),path,false);session->clear();Model reopened;ModelIO::open(reopened,path);session->addModel(reopened);session->setModelSelected(0,true);widget.refreshContext();
        QCOMPARE(widget.draftRecipe(),saved);external=widget.findChild<QComboBox*>("piezoExternalForceDirection0_0");QVERIFY(external);QCOMPARE(external->currentData().toString(),family=="axialRod"?QString("X"):QString("local:RY"));QVERIFY(widget.findChild<QPushButton*>("piezoRun")->isEnabled());
    }
    void frameEditRetiresApprovalRunAndPlotWithoutReinterpretingGlobalMoment() { try {
        static QTemporaryDir workspace;QTemporaryDir artifacts;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(beam());session->setModelSelected(0,true);PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(3);widget.findChild<QPushButton*>("piezoAddForce")->click();QCoreApplication::processEvents();auto *direction=widget.findChild<QComboBox*>("piezoForceDirection0");QVERIFY(direction);direction->setCurrentIndex(direction->findData("RY"));
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();const auto oldFrame=widget.draftRecipe()["frame"].toObject();const auto oldChannel=widget.draftRecipe()["excitation"].toObject()["channels"].toArray().first();
        const auto records=acceptedRecords(session->getModel(0),artifacts.path(),false);QVERIFY2(widget.showAcceptedTimeSeriesFrom({records[1],records[3]}),qPrintable(status(widget)));auto *plot=widget.findChild<SeriesPlotWidget*>("piezoAcceptedPlot");QVERIFY(!plot->dataset().traces.isEmpty());widget.previewRun();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        const double h=1/std::sqrt(2.);auto *ey=widget.findChild<QTableWidget*>("piezoFrameey"),*ez=widget.findChild<QTableWidget*>("piezoFrameez");QVERIFY(ey);QVERIFY(ez);
        ey->item(0,1)->setText(QString::number(h,'g',17));ey->item(0,2)->setText(QString::number(h,'g',17));ez->item(0,1)->setText(QString::number(-h,'g',17));ez->item(0,2)->setText(QString::number(h,'g',17));
        QVERIFY(!widget.hasCurrentPreview());QVERIFY(plot->dataset().traces.isEmpty());QVERIFY(!widget.findChild<QPushButton*>("piezoRun")->isEnabled());QCOMPARE(widget.draftRecipe()["frame"].toObject()["origin"],oldFrame["origin"]);
        QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe()["excitation"].toObject()["channels"].toArray().first(),oldChannel);direction=widget.findChild<QComboBox*>("piezoForceDirection0");QVERIFY(direction);QCOMPARE(direction->currentData().toString(),QString("RY"));QVERIFY(direction->currentText().contains("Invalid"));QVERIFY(direction->property("invalidForceDirection").toBool());
        QVERIFY(direction->findData("Z")<0);QVERIFY(direction->findData("RX")<0);QVERIFY(direction->findData("RZ")<0);QVERIFY(direction->findData("local:RY")>=0);
        widget.previewExperiment();QVERIFY(!widget.hasCurrentPreview());QVERIFY(status(widget).contains("invalid"));direction->setCurrentIndex(direction->findData("local:RY"));
        const auto corrected=widget.draftRecipe()["excitation"].toObject()["channels"].toArray().first().toObject();QVERIFY(corrected["channelId"].toString().endsWith(":local:RY"));QCOMPARE(corrected["unit"].toString(),QString("N m"));QVERIFY(!direction->property("invalidForceDirection").toBool());
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }
    void computedIntegrationSuggestionPreservesCustomDraftUntilChosen() { try {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(rods());session->setModelSelected(0,true);PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        widget.findChild<QDoubleSpinBox*>("piezoTimeStep")->setValue(.0007);widget.findChild<QSpinBox*>("piezoSteps")->setValue(31);const auto original=widget.draftRecipe();const auto stamp=session->modelCommandStamp(0);
        widget.computeModes();QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe(),original);auto *use=widget.findChild<QPushButton*>("piezoApplySuggestedGrid");QVERIFY2(use,qPrintable(status(widget)));QVERIFY(use->isEnabled());
        const double dt=use->property("suggestedDtS").toDouble(),steps=use->property("suggestedSteps").toDouble();QVERIFY(dt>0);QVERIFY(steps>=1&&steps<=8192);auto *frequencies=widget.findChild<QTableWidget*>("piezoModes");QVERIFY(frequencies);double slowest=std::numeric_limits<double>::infinity(),fastest=0;
        for(int row=0;row<frequencies->rowCount();++row){const double hz=frequencies->item(row,2)->text().toDouble();slowest=std::min(slowest,hz);fastest=std::max(fastest,hz);}
        QVERIFY(std::abs(dt*fastest-1./20)<1e-10);QVERIFY(steps*dt*slowest>=3.-1e-9);QVERIFY(widget.findChild<QLabel*>("piezoGridSuggestion")->text().contains("8192"));
        use->click();QCoreApplication::processEvents();QCOMPARE(widget.draftRecipe()["dt"].toDouble(),dt);QCOMPARE(widget.draftRecipe()["steps"].toInt(),int(steps));QCOMPARE(widget.draftRecipe()["excitation"],original["excitation"]);QCOMPARE(widget.draftRecipe()["initialDisplacements"],original["initialDisplacements"]);QCOMPARE(widget.draftRecipe()["materials"],original["materials"]);
        QCOMPARE(session->modelCommandStamp(0),stamp);QVERIFY(!widget.hasCurrentPreview());QVERIFY(!widget.findChild<QPushButton*>("piezoRun")->isEnabled());
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }
    void compactNativeColumnsDriveTimeCsvAndSpectrum_data() {
        QTest::addColumn<QString>("family");QTest::newRow("rod")<<QString("axialRod");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");
    }
    void compactNativeColumnsDriveTimeCsvAndSpectrum() {try {
        QFETCH(QString,family);static QTemporaryDir workspace;QTemporaryDir artifacts;auto *session=Application::instance()->getSession();QString error;QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(family=="axialRod"?rods():family=="flatPlate"?plate():beam());session->setModelSelected(0,true);
        const auto oldWorkspace=qgetenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE"),oldEvents=qgetenv("RANGE_PIEZO_PERFORMANCE_EVENTS");
        struct RestoreEnvironment {QByteArray workspace,events;~RestoreEnvironment(){if(workspace.isNull())qunsetenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE");else qputenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE",workspace);if(events.isNull())qunsetenv("RANGE_PIEZO_PERFORMANCE_EVENTS");else qputenv("RANGE_PIEZO_PERFORMANCE_EVENTS",events);}} restoreEnvironment{oldWorkspace,oldEvents};
        const auto performanceEvents=artifacts.filePath("canvas-reveal.ndjson");qputenv("RANGE_PIEZO_PERFORMANCE_WORKSPACE",artifacts.path().toUtf8());qputenv("RANGE_PIEZO_PERFORMANCE_EVENTS",performanceEvents.toUtf8());
        QScrollArea outer;outer.setWidgetResizable(true);auto *wrapper=new QWidget;auto *wrapperLayout=new QVBoxLayout(wrapper);wrapperLayout->setContentsMargins(0,0,0,0);
        PiezoWorkbenchWidget widget(wrapper);wrapperLayout->addWidget(&widget);outer.setWidget(wrapper);outer.resize(800,450);
        widget.setSession(session);permitLegacyReplacement(widget);auto *changes=widget.findChild<QTableWidget*>("piezoChanges");QVERIFY(changes);QVERIFY(changes->isHidden());
        auto *open=widget.findChild<QPushButton*>("piezoShowAcceptedSeries");auto *port=widget.findChild<QComboBox*>("piezoResultPort");auto *quantity=widget.findChild<QComboBox*>("piezoResultQuantity");
        auto *reference=widget.findChild<QComboBox*>("piezoResultReference");auto *view=widget.findChild<QComboBox*>("piezoResultView");auto *window=widget.findChild<QComboBox*>("piezoResultWindow");auto *mean=widget.findChild<QCheckBox*>("piezoResultRemoveMean");
        auto *fieldHost=widget.findChild<QWidget*>("piezoResultFields");auto *fieldGrid=widget.findChild<QGridLayout*>("piezoResultFieldGrid");auto *spectralForm=widget.findChild<QFormLayout*>("piezoResultSpectralForm");
        QVERIFY(open);QVERIFY(port);QVERIFY(quantity);QVERIFY(reference);QVERIFY(view);QVERIFY(window);QVERIFY(mean);QVERIFY(fieldHost);QVERIFY(fieldGrid);QVERIFY(spectralForm);
        auto *windowLabel=qobject_cast<QLabel*>(spectralForm->labelForField(window));QVERIFY(windowLabel);QCOMPARE(windowLabel->buddy(),static_cast<QWidget*>(window));QCOMPARE(window->accessibleName(),windowLabel->text());
        QComboBox *fieldChoices[]{port,quantity,reference,view};
        const QString fullOpenLabel=QStringLiteral("Open native result / accepted records\nand show the selected view…");
        QCOMPARE(widget.findChildren<QPushButton*>("piezoShowAcceptedSeries").size(),1);QCOMPARE(open->text(),QStringLiteral("Open native result / accepted records…"));QVERIFY(!open->text().contains('\n'));QCOMPARE(open->accessibleName(),fullOpenLabel);QCOMPARE(open->toolTip(),fullOpenLabel);
        QCOMPARE(open->objectName(),QStringLiteral("piezoShowAcceptedSeries"));for(auto *choice:fieldChoices){QCOMPARE(choice->minimumContentsLength(),12);QCOMPARE(choice->sizePolicy().horizontalPolicy(),QSizePolicy::Expanding);}
        QCOMPARE(view->currentData().toString(),QStringLiteral("time"));QVERIFY(window->isHidden());QVERIFY(windowLabel->isHidden());QVERIFY(mean->isHidden());
        QWidget *fieldWidgets[]{widget.findChild<QWidget*>("piezoResultPortField"),widget.findChild<QWidget*>("piezoResultQuantityField"),widget.findChild<QWidget*>("piezoResultReferenceField"),widget.findChild<QWidget*>("piezoResultViewField")};
        QFormLayout *fieldForms[4]{};
        for(int i=0;i<4;++i){QVERIFY(fieldWidgets[i]);fieldForms[i]=qobject_cast<QFormLayout*>(fieldWidgets[i]->layout());QVERIFY(fieldForms[i]);QCOMPARE(fieldForms[i]->rowWrapPolicy(),QFormLayout::WrapLongRows);
            QCOMPARE(fieldChoices[i]->parentWidget(),fieldWidgets[i]);
            auto *fieldItem=fieldForms[i]->itemAt(0,QFormLayout::FieldRole);QVERIFY(fieldItem);QCOMPARE(fieldItem->widget(),static_cast<QWidget*>(fieldChoices[i]));
            auto *label=qobject_cast<QLabel*>(fieldForms[i]->labelForField(fieldChoices[i]));QVERIFY(label);QCOMPARE(label->buddy(),static_cast<QWidget*>(fieldChoices[i]));QCOMPARE(fieldChoices[i]->accessibleName(),label->text());}
        window->setCurrentIndex(window->findData("rectangular"));mean->setChecked(false);view->setCurrentIndex(view->findData("amplitude"));QVERIFY(!window->isHidden());QVERIFY(!windowLabel->isHidden());QVERIFY(!mean->isHidden());
        auto *focusPlot=widget.findChild<SeriesPlotWidget*>("piezoAcceptedPlot");QVERIFY(focusPlot);auto *hold=focusPlot->findChild<QCheckBox*>("seriesPlotHold");auto *fit=focusPlot->findChild<QPushButton*>("seriesPlotFit");auto *exportButton=focusPlot->findChild<QPushButton*>("seriesPlotExport");QVERIFY(hold);QVERIFY(fit);QVERIFY(exportButton);
        // Non-focusable layout wrappers belong to the Qt chain but are skipped by Tab.
        const auto nextTabPolicy=[](QWidget *from){auto *next=from->nextInFocusChain();for(int i=0;i<1000&&next!=from;++i,next=next->nextInFocusChain())if(next->focusPolicy()&Qt::TabFocus)return next;return static_cast<QWidget*>(nullptr);};
        QCOMPARE(nextTabPolicy(open),static_cast<QWidget*>(port));QCOMPARE(nextTabPolicy(port),static_cast<QWidget*>(quantity));QCOMPARE(nextTabPolicy(quantity),static_cast<QWidget*>(reference));
        QCOMPARE(nextTabPolicy(reference),static_cast<QWidget*>(view));QCOMPARE(nextTabPolicy(view),static_cast<QWidget*>(window));QCOMPARE(nextTabPolicy(window),static_cast<QWidget*>(mean));
        QCOMPARE(nextTabPolicy(mean),static_cast<QWidget*>(hold));QCOMPARE(nextTabPolicy(hold),static_cast<QWidget*>(fit));QCOMPARE(nextTabPolicy(fit),static_cast<QWidget*>(exportButton));
        view->setCurrentIndex(view->findData("time"));QVERIFY(window->isHidden());QVERIFY(windowLabel->isHidden());QVERIFY(mean->isHidden());QCOMPARE(window->currentData().toString(),QStringLiteral("rectangular"));QVERIFY(!mean->isChecked());
        view->setCurrentIndex(view->findData("psd"));QVERIFY(!window->isHidden());QVERIFY(!windowLabel->isHidden());QVERIFY(!mean->isHidden());QCOMPARE(window->currentData().toString(),QStringLiteral("rectangular"));QVERIFY(!mean->isChecked());
        window->setCurrentIndex(window->findData("hann"));mean->setChecked(true);view->setCurrentIndex(view->findData("time"));QVERIFY(window->isHidden());QVERIFY(windowLabel->isHidden());QVERIFY(mean->isHidden());
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(5);widget.findChild<QComboBox*>("piezoController")->setCurrentText("PI");QCoreApplication::processEvents();widget.findChild<QTableWidget*>("piezoControllerbias")->item(0,0)->setText(".5");widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QVERIFY(!changes->isHidden());QVERIFY(changes->rowCount()>0);widget.applyPreview();QVERIFY(changes->isHidden());
        const auto sourcePath=artifacts.filePath("source.rbm");ModelIO::save(session->getModel(0),sourcePath,false);session->clear();Model reopened;ModelIO::open(reopened,sourcePath);session->addModel(reopened);session->setModelSelected(0,true);widget.refreshContext();
        port=widget.findChild<QComboBox*>("piezoResultPort");quantity=widget.findChild<QComboBox*>("piezoResultQuantity");reference=widget.findChild<QComboBox*>("piezoResultReference");
        view=widget.findChild<QComboBox*>("piezoResultView");window=widget.findChild<QComboBox*>("piezoResultWindow");mean=widget.findChild<QCheckBox*>("piezoResultRemoveMean");fieldHost=widget.findChild<QWidget*>("piezoResultFields");
        fieldGrid=widget.findChild<QGridLayout*>("piezoResultFieldGrid");
        QVERIFY(port);QVERIFY(quantity);QVERIFY(reference);QVERIFY(view);QVERIFY(window);QVERIFY(mean);QVERIFY(fieldHost);QVERIFY(fieldGrid);
        fieldWidgets[0]=widget.findChild<QWidget*>("piezoResultPortField");fieldWidgets[1]=widget.findChild<QWidget*>("piezoResultQuantityField");fieldWidgets[2]=widget.findChild<QWidget*>("piezoResultReferenceField");fieldWidgets[3]=widget.findChild<QWidget*>("piezoResultViewField");
        fieldChoices[0]=port;fieldChoices[1]=quantity;fieldChoices[2]=reference;fieldChoices[3]=view;
        for(int i=0;i<4;++i){QVERIFY(fieldWidgets[i]);fieldForms[i]=qobject_cast<QFormLayout*>(fieldWidgets[i]->layout());QVERIFY(fieldForms[i]);
            QCOMPARE(fieldChoices[i]->parentWidget(),fieldWidgets[i]);
            auto *fieldItem=fieldForms[i]->itemAt(0,QFormLayout::FieldRole);QVERIFY(fieldItem);QCOMPARE(fieldItem->widget(),static_cast<QWidget*>(fieldChoices[i]));}
        const auto proof=acceptedTrace(session->getModel(0),artifacts.path(),family=="axialRod");const auto read=PiezoResultSeries::readAcceptedTrace(proof);QVERIFY2(read.valid,qPrintable(read.message));QCOMPARE(read.dataset->times.size(),6);const auto stamp=session->modelCommandStamp(0);
        auto *pages=widget.findChild<QTabWidget*>("piezoPages");QVERIFY(pages);auto *resultPage=qobject_cast<QScrollArea*>(pages->widget(pages->count()-1));QVERIFY(resultPage);pages->setCurrentWidget(resultPage);outer.resize(800,1200);outer.show();QTest::qWait(50);
        auto *plot=widget.findChild<SeriesPlotWidget*>("piezoAcceptedPlot");QVERIFY(plot);QSignalSpy painted(plot,&SeriesPlotWidget::datasetPainted);
        QVERIFY2(widget.showAcceptedTimeSeriesFromTrace(proof),qPrintable(status(widget)));QVERIFY(!plot->dataset().traces.isEmpty());
        auto *canvas=plot->findChild<QWidget*>("seriesPlotCanvas");QVERIFY(canvas);QCOMPARE(canvas->minimumSize(),QSize(320,240));
        // Read once in a tall window, then shrink and re-render the already
        // certified dataset through the real view controls. This must not start
        // a new measured request or retain the previous allocated canvas height.
        const auto tallGeneration=plot->datasetGeneration();
        QTRY_VERIFY_WITH_TIMEOUT(!painted.isEmpty(),3000);
        outer.resize(800,450);QTest::qWait(50);QCOMPARE(plot->datasetGeneration(),tallGeneration);
        auto *revealView=widget.findChild<QComboBox*>("piezoResultView");QVERIFY(revealView);
        revealView->setCurrentIndex(revealView->findData("amplitude"));revealView->setCurrentIndex(revealView->findData("time"));
        const auto generation=plot->datasetGeneration();const auto range=plot->visibleRange();
        const auto canvasGeometry=[&]{
            const auto rectangle=[](const QRect &r){return QString("[%1,%2 %3x%4]").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());};
            QStringList details{QString("canvas visible=%1 rect=%2 min=%3x%4; tabs height=%5 min=%6 selected=%7 result=%8")
                .arg(canvas->isVisible()).arg(rectangle(canvas->rect())).arg(canvas->minimumWidth()).arg(canvas->minimumHeight())
                .arg(pages->height()).arg(pages->minimumHeight()).arg(pages->currentIndex()).arg(pages->indexOf(resultPage))};
            int index=0;for(QWidget *parent=canvas->parentWidget();parent;parent=parent->parentWidget())if(auto *scroll=qobject_cast<QScrollArea*>(parent)){
                const auto *h=scroll->horizontalScrollBar(),*v=scroll->verticalScrollBar();const auto *content=scroll->widget();
                details.append(QString("scroll%1 canvas=%2 viewport=%3 scrollSize=%4 min=%5x%6 content=%7 contentMin=%8x%9 h=%10/%11..%12 v=%13/%14..%15")
                    .arg(index++).arg(rectangle(QRect(canvas->mapTo(scroll->viewport(),QPoint()),canvas->size()))).arg(rectangle(scroll->viewport()->rect()))
                    .arg(rectangle(scroll->rect())).arg(scroll->minimumWidth()).arg(scroll->minimumHeight()).arg(rectangle(content->rect()))
                    .arg(content->minimumWidth()).arg(content->minimumHeight()).arg(h->value()).arg(h->minimum()).arg(h->maximum())
                    .arg(v->value()).arg(v->minimum()).arg(v->maximum()));
            }
            // Bounded diagnostics identify the actual controls or layout that
            // impose a content minimum if the geometric gate still fails.
            auto children=resultPage->widget()->findChildren<QWidget*>();
            const auto minimumWidth=[](QWidget *child){return std::max(child->minimumWidth(),child->minimumSizeHint().width());};
            std::sort(children.begin(),children.end(),[&](QWidget *a,QWidget *b){return minimumWidth(a)>minimumWidth(b);});
            for(int i=0;i<std::min(16,int(children.size()));++i){auto *child=children[i];QString text;
                if(auto *label=qobject_cast<QLabel*>(child))text=label->text();else if(auto *button=qobject_cast<QPushButton*>(child))text=button->text();else if(auto *choice=qobject_cast<QComboBox*>(child))text=choice->currentText();
                details.append(QString("minimum%1 %2/%3 min=%4 hint=%5 layoutMin=%6 text=%7").arg(i).arg(child->metaObject()->className()).arg(child->objectName())
                    .arg(child->minimumWidth()).arg(child->minimumSizeHint().width()).arg(child->layout()?child->layout()->minimumSize().width():-1).arg(text.left(100).replace('\n',' ')));
            }
            return details.join("\n");
        };
        const auto fullCanvasVisible=[&]{int scrollCount=0;if(!canvas->isVisible())return false;for(QWidget *parent=canvas->parentWidget();parent;parent=parent->parentWidget())if(auto *scroll=qobject_cast<QScrollArea*>(parent)){++scrollCount;if(!scroll->viewport()->rect().contains(QRect(canvas->mapTo(scroll->viewport(),QPoint()),canvas->size())))return false;}return scrollCount>=2;};
        QTRY_VERIFY2_WITH_TIMEOUT(fullCanvasVisible(),qPrintable(canvasGeometry()),3000);
        QTRY_VERIFY_WITH_TIMEOUT(!painted.isEmpty()&&painted.last().first().toULongLong()==generation,3000);
        QCOMPARE(outer.size(),QSize(800,450));QVERIFY(outer.minimumSizeHint().height()<=450);QVERIFY(resultPage->viewport()->height()>=canvas->height());QCOMPARE(plot->datasetGeneration(),generation);
        QCOMPARE(plot->visibleRange().xMinimum,range.xMinimum);QCOMPARE(plot->visibleRange().xMaximum,range.xMaximum);QCOMPARE(plot->visibleRange().yMinimum,range.yMinimum);QCOMPARE(plot->visibleRange().yMaximum,range.yMaximum);
        auto *coverage=widget.findChild<QLabel*>("piezoResultCoverage");QVERIFY(coverage);QVERIFY(coverage->geometry().top()>=plot->geometry().bottom());
        QCOMPARE(port->toolTip(),port->currentText());QCOMPARE(quantity->toolTip(),quantity->currentText());QCOMPARE(reference->toolTip(),reference->currentText());QCOMPARE(view->toolTip(),view->currentText());QCOMPARE(window->toolTip(),window->currentText());
        const int wideThreshold=fieldHost->property("piezoResultWideThreshold").toInt();QVERIFY(wideThreshold>0);QCOMPARE(fieldGrid->count(),4);
        const auto fieldPosition=[&](QWidget *field){for(int i=0;i<fieldGrid->count();++i)if(fieldGrid->itemAt(i)->widget()==field){int row=0,column=0,rowSpan=0,columnSpan=0;fieldGrid->getItemPosition(i,&row,&column,&rowSpan,&columnSpan);return QPoint(row,column);}return QPoint(-1,-1);};
        const auto fieldsFitWithoutOverlap=[&]{
            const QRect hostRect=fieldHost->contentsRect();
            for(int i=0;i<4;++i){const QRect fieldRect(fieldWidgets[i]->mapTo(fieldHost,QPoint()),fieldWidgets[i]->size());if(!hostRect.contains(fieldRect))return false;
                auto *label=qobject_cast<QLabel*>(fieldForms[i]->labelForField(fieldChoices[i]));if(!label)return false;
                const QRect labelRect(label->mapTo(fieldHost,QPoint()),label->size()),choiceRect(fieldChoices[i]->mapTo(fieldHost,QPoint()),fieldChoices[i]->size());
                if(!hostRect.contains(labelRect)||!hostRect.contains(choiceRect)||labelRect.intersects(choiceRect))return false;
                for(int j=0;j<i;++j){const QRect previous(fieldWidgets[j]->mapTo(fieldHost,QPoint()),fieldWidgets[j]->size());if(fieldRect.intersects(previous))return false;}
            }
            return true;
        };
        outer.resize(1000,450);QTest::qWait(50);QCOMPARE(outer.size(),QSize(1000,450));QVERIFY(fieldHost->contentsRect().width()>=wideThreshold);QCOMPARE(fieldGrid->count(),4);
        QCOMPARE(fieldPosition(fieldWidgets[0]),QPoint(0,0));QCOMPARE(fieldPosition(fieldWidgets[1]),QPoint(0,1));
        QCOMPARE(fieldPosition(fieldWidgets[2]),QPoint(1,0));QCOMPARE(fieldPosition(fieldWidgets[3]),QPoint(1,1));QVERIFY(fieldsFitWithoutOverlap());
        const auto stableGeneration=plot->datasetGeneration();const auto stableRange=plot->visibleRange();const auto stableRecipe=widget.draftRecipe();const auto stableStamp=session->modelCommandStamp(0);const auto stableUndo=session->getModel(0).getUndoStackSize();
        outer.resize(600,450);QTest::qWait(50);QCOMPARE(outer.size(),QSize(600,450));QVERIFY(fieldHost->contentsRect().width()<wideThreshold);QCOMPARE(fieldGrid->count(),4);
        for(int i=0;i<4;++i)QCOMPARE(fieldPosition(fieldWidgets[i]),QPoint(i,0));QVERIFY(fieldsFitWithoutOverlap());
        int fieldLeft=0,fieldTop=0,fieldRight=0,fieldBottom=0;fieldGrid->getContentsMargins(&fieldLeft,&fieldTop,&fieldRight,&fieldBottom);
        const int usableFieldWidth=fieldHost->contentsRect().width()-fieldLeft-fieldRight;
        for(auto *field:fieldWidgets)QVERIFY2(std::abs(field->width()-usableFieldWidth)<=1,"Narrow fields must use the full available width without an empty second column");
        QCOMPARE(plot->datasetGeneration(),stableGeneration);QCOMPARE(plot->visibleRange().xMinimum,stableRange.xMinimum);QCOMPARE(plot->visibleRange().xMaximum,stableRange.xMaximum);
        QCOMPARE(plot->visibleRange().yMinimum,stableRange.yMinimum);QCOMPARE(plot->visibleRange().yMaximum,stableRange.yMaximum);QCOMPARE(widget.draftRecipe(),stableRecipe);
        QCOMPARE(session->modelCommandStamp(0),stableStamp);QCOMPARE(session->getModel(0).getUndoStackSize(),stableUndo);
        outer.resize(1000,450);QTest::qWait(50);QCOMPARE(outer.size(),QSize(1000,450));QVERIFY(fieldHost->contentsRect().width()>=wideThreshold);QCOMPARE(fieldGrid->count(),4);
        QCOMPARE(fieldPosition(fieldWidgets[0]),QPoint(0,0));QCOMPARE(fieldPosition(fieldWidgets[1]),QPoint(0,1));
        QCOMPARE(fieldPosition(fieldWidgets[2]),QPoint(1,0));QCOMPARE(fieldPosition(fieldWidgets[3]),QPoint(1,1));QVERIFY(fieldsFitWithoutOverlap());
        QCOMPARE(plot->datasetGeneration(),stableGeneration);QCOMPARE(plot->visibleRange().xMinimum,stableRange.xMinimum);QCOMPARE(plot->visibleRange().xMaximum,stableRange.xMaximum);
        QCOMPARE(plot->visibleRange().yMinimum,stableRange.yMinimum);QCOMPARE(plot->visibleRange().yMaximum,stableRange.yMaximum);QCOMPARE(widget.draftRecipe(),stableRecipe);
        QCOMPARE(session->modelCommandStamp(0),stableStamp);QCOMPARE(session->getModel(0).getUndoStackSize(),stableUndo);
        outer.resize(800,450);QTest::qWait(50);QCOMPARE(outer.size(),QSize(800,450));QCOMPARE(plot->datasetGeneration(),stableGeneration);
        // The performance request measures the first accepted dataset and its
        // actual paint. Later view changes produce new display generations but
        // do not start another request; select the latest matching event for
        // the captured request generation, then check the final view generation
        // separately above.
        const auto measuredGeneration=QString::number(tallGeneration);
        QFile performanceLog(performanceEvents);QVERIFY(performanceLog.open(QIODevice::ReadOnly));QJsonObject begin,ready,paint;int requestCount=0;
        while(!performanceLog.atEnd()){
            const auto event=QJsonDocument::fromJson(performanceLog.readLine()).object();
            if(event.value("event").toString()=="request_begin"){begin=event;++requestCount;}
            else if(event.value("event").toString()=="dataset_ready"){
                QCOMPARE(event.value("requestId"),begin.value("requestId"));
                if(event.value("datasetGeneration").toString()!=measuredGeneration)continue;
                if(!ready.isEmpty())QVERIFY(event.value("elapsedNanoseconds").toString().toLongLong()>=ready.value("elapsedNanoseconds").toString().toLongLong());
                ready=event;
            }else if(event.value("event").toString()=="canvas_painted"){
                QCOMPARE(event.value("requestId"),begin.value("requestId"));
                if(event.value("datasetGeneration").toString()==measuredGeneration)paint=event;
            }
        }
        QCOMPARE(requestCount,1);QVERIFY(!ready.isEmpty());QVERIFY(!paint.isEmpty());QCOMPARE(read.dataset->modelFamily,family);QCOMPARE(ready.value("modelFamily").toString(),family);QCOMPARE(paint.value("modelFamily").toString(),family);QCOMPARE(begin.value("requestId"),ready.value("requestId"));QCOMPARE(ready.value("requestId"),paint.value("requestId"));QCOMPARE(ready.value("datasetGeneration").toString(),measuredGeneration);QCOMPARE(paint.value("datasetGeneration"),ready.value("datasetGeneration"));QVERIFY(paint.value("elapsedNanoseconds").toString().toLongLong()>=ready.value("elapsedNanoseconds").toString().toLongLong());QVERIFY(!widget.property("piezoMeasurementInvalid").toBool());
        const auto trace=plot->dataset().traces.first();const auto channel=std::find_if(read.dataset->channels.begin(),read.dataset->channels.end(),[&](const auto &s){return s.channelId==trace.id;});QVERIFY(channel!=read.dataset->channels.end());const auto &column=read.dataset->columns[channel-read.dataset->channels.begin()];QCOMPARE(trace.points.size(),6);QCOMPARE(trace.artifactSha256,read.dataset->artifactSha256);QCOMPARE(trace.sourceId,read.dataset->sourceFingerprint);for(int i=0;i<6;++i){QCOMPARE(trace.points[i],QPointF(read.dataset->times[i],column[i]));QCOMPARE(trace.originalOrdinals[i],quint64(i));}
        // Exercise actual keyboard traversal once the certified controls are enabled.
        // refreshContext rebuilt these controls when the saved model was reopened.
        open=widget.findChild<QPushButton*>("piezoShowAcceptedSeries");hold=plot->findChild<QCheckBox*>("seriesPlotHold");fit=plot->findChild<QPushButton*>("seriesPlotFit");exportButton=plot->findChild<QPushButton*>("seriesPlotExport");QVERIFY(open);QVERIFY(hold);QVERIFY(fit);QVERIFY(exportButton);
        view->setCurrentIndex(view->findData("amplitude"));outer.activateWindow();open->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(open));
        QVERIFY(!exportButton->isEnabled());const QVector<QWidget*> keyboardTargets{port,quantity,reference,view,window,mean,hold,fit};
        for(auto *target:keyboardTargets){QVERIFY2(target->isEnabled(),qPrintable(target->objectName()));QVERIFY2(target->isVisible(),qPrintable(target->objectName()));QTest::keyClick(QApplication::focusWidget(),Qt::Key_Tab);QTRY_COMPARE(QApplication::focusWidget(),target);}QTest::keyClick(fit,Qt::Key_Tab);QTRY_VERIFY(QApplication::focusWidget()&&QApplication::focusWidget()!=exportButton&&QApplication::focusWidget()!=fit);
        view->setCurrentIndex(view->findData("time"));
        QVERIFY(std::any_of(column.begin(),column.end(),[](double x){return x!=0.;}));view->setCurrentIndex(view->findData("amplitude"));const auto spectrum=PiezoSpectrum::calculate(*read.dataset,trace.id,{});QVERIFY2(spectrum.valid,qPrintable(spectrum.message));QCOMPARE(plot->dataset().traces.first().points.size(),4);for(int k=0;k<4;++k)QCOMPARE(plot->dataset().traces.first().points[k],QPointF(spectrum.spectrum.frequenciesHz[k],spectrum.spectrum.amplitude[k]));view->setCurrentIndex(view->findData("psd"));QCOMPARE(plot->dataset().yUnit,spectrum.spectrum.psdUnit);
        const auto csv=artifacts.filePath("compact.csv");QVERIFY2(widget.exportAcceptedTimeSeriesFromTrace(proof,csv),qPrintable(status(widget)));QFile exported(csv);QVERIFY(exported.open(QIODevice::ReadOnly));const auto bytes=exported.readAll();QVERIFY(bytes.contains("range.piezo-accepted-trace-timeseries/v1"));QVERIFY(bytes.contains(read.dataset->sourceFingerprint.toUtf8()));QVERIFY(bytes.contains(trace.id.toUtf8()));QCOMPARE(session->modelCommandStamp(0),stamp);        const auto csvMetadata=QJsonDocument::fromJson(bytes.split('\n').first().mid(2)).object();QCOMPARE(csvMetadata.value("modelFamily").toString(),family);
        performanceLog.close();QVERIFY(performanceLog.open(QIODevice::ReadOnly));QJsonObject csvEvent;while(!performanceLog.atEnd()){const auto event=QJsonDocument::fromJson(performanceLog.readLine()).object();if(event.value("event").toString()=="csv_committed")csvEvent=event;}QVERIFY(!csvEvent.isEmpty());QCOMPARE(csvEvent.value("modelFamily").toString(),family);QCOMPARE(csvEvent.value("family").toString(),read.dataset->family);
        auto bad=proof;bad.expectedContextSha256=QByteArray(32,'x');QVERIFY(!widget.showAcceptedTimeSeriesFromTrace(bad));QVERIFY(plot->dataset().traces.isEmpty());bad=proof;bad.cursor.stateSha32=QByteArray(32,'y');QVERIFY(!widget.showAcceptedTimeSeriesFromTrace(bad));bad=proof;bad.expectedSourceFingerprint=QString(64,'a');QVERIFY(!widget.showAcceptedTimeSeriesFromTrace(bad));
        QFile pending(artifacts.filePath("pending-publication.json"));QVERIFY(pending.open(QIODevice::WriteOnly));pending.write("{}");pending.close();QVERIFY(!widget.showAcceptedTimeSeriesFromTrace(proof));QVERIFY(status(widget).contains("PENDING"));QVERIFY(!widget.exportAcceptedTimeSeriesFromTrace(proof,csv));QFile unchanged(csv);QVERIFY(unchanged.open(QIODevice::ReadOnly));QCOMPARE(unchanged.readAll(),bytes);QVERIFY(pending.remove());
        // Production discovery refuses a loose trace with no registered authority;
        // it never silently falls back to sparse raw MODEL records.
        QVERIFY(!widget.showAcceptedTimeSeriesFrom({proof.baseModelPath}));QVERIFY(status(widget).contains("registered"));
        view->setCurrentIndex(view->findData("time"));QVERIFY2(widget.showAcceptedTimeSeriesFromTrace(proof),qPrintable(status(widget)));widget.findChild<QSpinBox*>("piezoSteps")->setValue(6);QVERIFY(plot->dataset().traces.isEmpty());
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}}
    void acceptedNamedSpectraUseSavedNativePortsAndRejectHoles_data() {
        QTest::addColumn<QString>("family");QTest::newRow("rod")<<QString("axialRod");QTest::newRow("beam")<<QString("planarBeam");QTest::newRow("plate")<<QString("flatPlate");
    }
    void acceptedNamedSpectraUseSavedNativePortsAndRejectHoles() { try {
        QFETCH(QString,family);static QTemporaryDir workspace;QTemporaryDir artifacts;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(family=="axialRod"?rods():family=="flatPlate"?plate():beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.findChild<QSpinBox*>("piezoSteps")->setValue(5);widget.findChild<QComboBox*>("piezoController")->setCurrentText("PI");QCoreApplication::processEvents();
        auto *bias=widget.findChild<QTableWidget*>("piezoControllerbias");QVERIFY(bias);bias->item(0,0)->setText(".5");widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();
        const auto saved=widget.draftRecipe();const auto sourcePath=artifacts.filePath("source.rbm");ModelIO::save(session->getModel(0),sourcePath,false);session->clear();Model reopened;ModelIO::open(reopened,sourcePath);session->addModel(reopened);session->setModelSelected(0,true);widget.refreshContext();QCOMPARE(widget.draftRecipe(),saved);
        const RModel source=session->getModel(0);const auto paths=acceptedRecords(source,artifacts.path(),family=="axialRod",5);const auto certified=PiezoResultSeries::readAcceptedRecords(paths);QVERIFY2(certified.valid,qPrintable(certified.message));
        const auto stamp=session->modelCommandStamp(0);const auto undo=quint64(session->getModel(0).getUndoStackSize());
        QVERIFY2(widget.showAcceptedTimeSeriesFrom(paths),qPrintable(status(widget)));auto *plot=widget.findChild<SeriesPlotWidget*>("piezoAcceptedPlot");auto *view=widget.findChild<QComboBox*>("piezoResultView"),*window=widget.findChild<QComboBox*>("piezoResultWindow");auto *mean=widget.findChild<QCheckBox*>("piezoResultRemoveMean");QVERIFY(plot);QVERIFY(view);QVERIFY(window);QVERIFY(mean);
        window->setCurrentIndex(window->findData("rectangular"));mean->setChecked(false);view->setCurrentIndex(view->findData("amplitude"));
        const auto check=[&](bool psd){QVERIFY(!plot->dataset().traces.isEmpty());const auto &trace=plot->dataset().traces.first();const auto expected=PiezoSpectrum::calculate(*certified.dataset,trace.id,{window->currentData().toString()=="hann"?PiezoSpectrum::Window::PeriodicHann:PiezoSpectrum::Window::Rectangular,mean->isChecked()});QVERIFY2(expected.valid,qPrintable(expected.message));
            QCOMPARE(plot->dataset().viewKind,SeriesPlotWidget::ViewKind::Spectrum);QCOMPARE(plot->dataset().xUnit,QString("Hz"));QCOMPARE(trace.sourceId,certified.dataset->sourceFingerprint);QCOMPARE(trace.artifactSha256,certified.dataset->artifactSha256);QCOMPARE(trace.points.size(),4);QCOMPARE(trace.originalOrdinals,QVector<quint64>({0,1,2,3}));
            QCOMPARE(plot->dataset().yUnit,psd?expected.spectrum.psdUnit:expected.spectrum.amplitudeUnit);for(int k=0;k<trace.points.size();++k)QCOMPARE(trace.points[k],QPointF(expected.spectrum.frequenciesHz[k],psd?expected.spectrum.psd[k]:expected.spectrum.amplitude[k]));
        };
        check(false);const auto trace=plot->dataset().traces.first();QVector<double> values;
        for(const auto &frame:certified.dataset->frames){auto signal=std::find_if(frame->channels.begin(),frame->channels.end(),[&](const auto &s){return s.channelId==trace.id;});QVERIFY(signal!=frame->channels.end());values.append(signal->value);}
        QVERIFY(std::any_of(values.begin(),values.end(),[](double value){return value!=0.;}));
        for(int k=0;k<trace.points.size();++k){std::complex<double> bin;for(int j=0;j<values.size();++j){const double angle=-2*std::acos(-1.)*j*k/values.size();bin+=values[j]*std::complex<double>(std::cos(angle),std::sin(angle));}
            const double expected=std::abs(bin)/values.size()*(k>0&&k!=values.size()/2?2.:1.);QVERIFY(std::abs(trace.points[k].y()-expected)<=1e-10*std::max(1e-30,std::abs(expected))+1e-25);
        }
        view->setCurrentIndex(view->findData("psd"));check(true);window->setCurrentIndex(window->findData("hann"));mean->setChecked(true);check(true);
        auto *reference=widget.findChild<QComboBox*>("piezoResultReference");reference->setCurrentIndex(reference->findData("delta"));check(true);QVERIFY(plot->dataset().traces.first().id.endsWith(":delta"));
        auto *quantity=widget.findChild<QComboBox*>("piezoResultQuantity");quantity->setCurrentIndex(quantity->findData("electrodeVoltage"));check(true);QCOMPARE(plot->dataset().yUnit,QString("(V)^2/Hz"));QVERIFY(plot->dataset().traces.first().label.contains("action record"));
        QVERIFY(widget.findChild<QLabel*>("piezoResultCoverage")->text().contains("Nyquist"));QVERIFY(widget.findChild<QLabel*>("piezoResultCoverage")->text().contains("FFT bin"));QVERIFY(widget.findChild<QLabel*>("piezoResultCoverage")->text().contains("Sampled coverage"));
        QCOMPARE(session->modelCommandStamp(0),stamp);QCOMPARE(quint64(session->getModel(0).getUndoStackSize()),undo);
        view->setCurrentIndex(view->findData("time"));const QStringList holes{paths[0],paths[1],paths[3],paths[4],paths[5]};QVERIFY2(widget.showAcceptedTimeSeriesFrom(holes),qPrintable(status(widget)));view->setCurrentIndex(view->findData("amplitude"));QVERIFY(plot->dataset().traces.isEmpty());QVERIFY(status(widget).contains("gaps"));view->setCurrentIndex(view->findData("time"));QVERIFY(!plot->dataset().traces.isEmpty());
        auto other=source;other.getNode(1).setX(1.01);const auto wrong=artifacts.filePath("different-source.rbm");other.write(wrong,false);QVERIFY(!widget.showAcceptedTimeSeriesFrom({other.nativeWritePaths(wrong,false).first()}));QVERIFY(plot->dataset().traces.isEmpty());
        QVERIFY2(widget.showAcceptedTimeSeriesFrom(paths),qPrintable(status(widget)));view->setCurrentIndex(view->findData("amplitude"));QVERIFY(!plot->dataset().traces.isEmpty());
        QFile changed(paths.last());QVERIFY(changed.open(QIODevice::Append));changed.write("changed");changed.close();mean->setChecked(!mean->isChecked());QVERIFY(plot->dataset().traces.isEmpty());QVERIFY(widget.findChild<QLabel*>("piezoResultCoverage")->text().contains("changed"));
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }
    void acceptedNamedTimeSeriesUsesCertifiedStateAndRejectsOtherSources_data() {
        QTest::addColumn<bool>("axial");QTest::newRow("layered-beam")<<false;QTest::newRow("axial-rods")<<true;
    }
    void acceptedNamedTimeSeriesUsesCertifiedStateAndRejectsOtherSources() { try {
        QFETCH(bool,axial);static QTemporaryDir workspace;QTemporaryDir artifacts;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));session->addModel(axial?rods():beam());session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);widget.findChild<QSpinBox*>("piezoSteps")->setValue(3);
        widget.findChild<QComboBox*>("piezoController")->setCurrentText("PI");QCoreApplication::processEvents();auto *bias=widget.findChild<QTableWidget*>("piezoControllerbias");QVERIFY(bias);bias->item(0,0)->setText(".5");
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));widget.applyPreview();
        const RModel source=session->getModel(0);const auto paths=acceptedRecords(source,artifacts.path(),axial);const QStringList selected{paths[3],paths[1]};
        const auto certified=PiezoResultSeries::readAcceptedRecords(selected);QVERIFY2(certified.valid,qPrintable(certified.message));
        const auto stamp=session->modelCommandStamp(0);const auto undo=session->getModel(0).getUndoStackSize();
        QVERIFY2(widget.showAcceptedTimeSeriesFrom(selected),qPrintable(status(widget)));auto *plot=widget.findChild<SeriesPlotWidget*>("piezoAcceptedPlot");QVERIFY(plot);QCOMPARE(plot->dataset().traces.size(),1);
        auto *port=widget.findChild<QComboBox*>("piezoResultPort"),*quantity=widget.findChild<QComboBox*>("piezoResultQuantity"),*reference=widget.findChild<QComboBox*>("piezoResultReference");QVERIFY(port);QVERIFY(quantity);QVERIFY(reference);
        const auto compareTrace=[&]{const auto &trace=plot->dataset().traces.first();QCOMPARE(trace.sourceId,certified.dataset->sourceFingerprint);QCOMPARE(trace.artifactSha256,certified.dataset->artifactSha256);
            QCOMPARE(trace.points.size(),2);QCOMPARE(trace.originalOrdinals,QVector<quint64>({0,1}));QCOMPARE(plot->dataset().yUnit,trace.unit);
            for(int i=0;i<2;++i){const auto &frame=*certified.dataset->frames[i];const auto signal=std::find_if(frame.channels.begin(),frame.channels.end(),[&](const auto &s){return s.channelId==trace.id;});QVERIFY(signal!=frame.channels.end());QCOMPARE(trace.points[i],QPointF(frame.timeS,signal->value));}
        };
        compareTrace();QVERIFY(widget.findChild<QLabel*>("piezoResultCoverage")->text().contains("gaps"));QVERIFY(widget.findChild<QLabel*>("piezoResultCoverage")->text().contains("no interpolation"));
        reference->setCurrentIndex(reference->findData("delta"));compareTrace();QVERIFY(plot->dataset().traces.first().id.endsWith(":delta"));
        quantity->setCurrentIndex(quantity->findData("electrodeVoltage"));compareTrace();QCOMPARE(plot->dataset().yUnit,QString("V"));QVERIFY(plot->dataset().traces.first().label.contains("action record"));
        port->setCurrentIndex(1);quantity->setCurrentIndex(quantity->findData("positiveElectrodeCharge"));compareTrace();QCOMPARE(plot->dataset().yUnit,QString("C"));
        QCOMPARE(session->modelCommandStamp(0),stamp);QCOMPARE(session->getModel(0).getUndoStackSize(),undo);
        QVERIFY(!widget.showAcceptedTimeSeriesFrom({paths[1],paths[1]}));QVERIFY(plot->dataset().traces.isEmpty());
        QVERIFY2(widget.showAcceptedTimeSeriesFrom(selected),qPrintable(status(widget)));auto other=source;other.getNode(1).setX(1.01);const auto wrong=artifacts.filePath("wrong-source.rbm");other.write(wrong,false);
        QVERIFY(!widget.showAcceptedTimeSeriesFrom({other.nativeWritePaths(wrong,false).first()}));QVERIFY(plot->dataset().traces.isEmpty());
        QVERIFY2(widget.showAcceptedTimeSeriesFrom(selected),qPrintable(status(widget)));widget.findChild<QSpinBox*>("piezoSteps")->setValue(4);QVERIFY(plot->dataset().traces.isEmpty());QVERIFY(!widget.findChild<QPushButton*>("piezoShowAcceptedSeries")->isEnabled());
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }

    void completeRecipeDiffShowsConcreteContainersIdentityChangesAndSchemaUnits() {try {
        PiezoWorkbenchWidget widget;widget.previewCommand="piezo.experiment.preview";
        const QJsonObject drive{{"portId","drive"},{"role","actuator"},{"readout","actuatorCharge"},{"initialValue",12.5}};
        const QJsonObject sense{{"portId","sense"},{"role","sensor"},{"readout","openCircuitVoltage"},{"initialValue",3.e-6}};
        const QJsonObject retired{{"portId","retired"},{"role","sensor"},{"readout","openCircuitVoltage"},{"initialValue",1.e-6}};
        const QJsonObject added{{"portId","added"},{"role","sensor"},{"readout","openCircuitVoltage"},{"initialValue",2.e-6}};
        const QJsonObject boundaryActuator{{"portId","boundary"},{"role","actuator"},{"readout","actuatorCharge"},{"initialValue",1.0}};
        widget.savedRecipe=QJsonObject{
            {"ports",QJsonArray{drive,retired,boundaryActuator,sense}},
            {"controller",QJsonObject{{"kp",1.0},{"observer",QJsonObject{{"L",QJsonArray{QJsonArray{1.,2.},QJsonArray{3.,4.}}}}},
                {"voltageLower",QJsonArray{-100.}},{"voltageUpper",QJsonArray{100.}},{"deltaVoltageLimits",QJsonArray{10.}}}},
            {"frame",QJsonObject{{"origin",QJsonArray{0.,0.,0.}},{"ex",QJsonArray{1.,0.,0.}},
                {"ey",QJsonArray{0.,1.,0.}},{"ez",QJsonArray{0.,0.,1.}}}},
            {"template",QJsonObject{{"length",1.0},{"width",.1},{"thickness",.01}}},
            {"members",QJsonArray{QJsonObject{{"physicalElementId",504},{"kind","cable"},{"initialTension",5.0}}}}
        };
        const QJsonObject changedSense{{"portId","sense"},{"role","sensor"},{"readout","openCircuitVoltage"},{"initialValue",4.e-6}};
        const QJsonObject changedDrive{{"portId","drive"},{"role","actuator"},{"readout","actuatorCharge"},{"initialValue",13.5}};
        const QJsonObject boundarySensor{{"portId","boundary"},{"role","sensor"},{"readout","openCircuitVoltage"},{"initialValue",1.0}};
        const QJsonObject changedMember{{"physicalElementId",504},{"kind","cable"},{"initialTension",8.0}};
        const QJsonObject proposed{
            {"ports",QJsonArray{changedSense,added,boundarySensor,changedDrive}},
            {"controller",QJsonObject{{"kp",QJsonArray{QJsonValue(QJsonArray{100.})}},
                {"observer",QJsonObject{{"L",QJsonArray{QJsonArray{1.,7.},QJsonArray{3.,4.}}}}},
                {"voltageLower",QJsonArray{-80.}},{"voltageUpper",QJsonArray{80.}},{"deltaVoltageLimits",QJsonArray{8.}}}},
            {"frame",QJsonObject{{"origin",QJsonArray{.1,0.,0.}},{"ex",QJsonArray{0.,1.,0.}},
                {"ey",QJsonArray{0.,0.,1.}},{"ez",QJsonArray{1.,0.,0.}}}},
            {"template",QJsonObject{{"length",1.5},{"width",.1},{"thickness",.01}}},
            {"members",QJsonArray{changedMember}},
            {"futureRoot",QJsonObject{{"experimentalLimit",7.0}}}
        };
        widget.showPreview(QJsonObject{{"normalizedArguments",proposed}});
        auto *changes=widget.findChild<QTableWidget*>("piezoChanges");QVERIFY(changes);
        QStringList rows;for(int row=0;row<changes->rowCount();++row)for(int column=0;column<changes->columnCount();++column)
            if(changes->item(row,column))rows.append(changes->item(row,column)->text());
        const QString summary=rows.join('\n');
        QVERIFY(summary.contains("Before: 1 [unit depends on bound input/output quantities; recipe has no per-entry unit]"));
        QVERIFY2(summary.contains("Proposed: [[100"),qPrintable(summary));QVERIFY(summary.contains("100 [unit depends on bound input/output quantities; recipe has no per-entry unit]"));
        QVERIFY(summary.contains("Before: 2 [unit not declared by recipe schema]"));QVERIFY(summary.contains("Proposed: 7 [unit not declared by recipe schema]"));
        QVERIFY(summary.contains("12.5 [V]"));QVERIFY(summary.contains("13.5 [V]"));
        QVERIFY(summary.contains("3e-06 [C]"));QVERIFY(summary.contains("4e-06 [C]"));
        QVERIFY(summary.contains("Electrical port retired"));QVERIFY(summary.contains("Electrical port added"));QVERIFY(summary.contains("Ports / Order"));
        QVERIFY(summary.contains("Before: Electrical port drive, Electrical port retired, Electrical port boundary, Electrical port sense"));
        QVERIFY(summary.contains("Proposed: Electrical port sense, Electrical port added, Electrical port boundary, Electrical port drive"));
        QVERIFY(summary.contains("Before: Not configured"));QVERIFY(summary.contains("Proposed: Not configured"));
        bool boundaryUnitChange=false;
        for(int row=0;row<changes->rowCount();++row)if(changes->item(row,0)&&changes->item(row,0)->text().endsWith(
            "Electrical port boundary / Initial electrical boundary value")){
            const auto values=changes->item(row,1)->text();
            boundaryUnitChange=values.contains("Before: 1 [V]")&&values.contains("Proposed: 1 [C]");
        }
        QVERIFY2(boundaryUnitChange,qPrintable(summary));
        QVERIFY(summary.contains("1 [m]"));QVERIFY(summary.contains("1.5 [m]"));QVERIFY(summary.contains("5 [N]"));QVERIFY(summary.contains("8 [N]"));
        QVERIFY(summary.contains("Frame origin"));QVERIFY(summary.contains("0.1 [m]"));QVERIFY(summary.contains("[dimensionless]"));
        QVERIFY(summary.contains("-100 [V]"));QVERIFY(summary.contains("-80 [V]"));QVERIFY(summary.contains("Actuator voltage change limit"));
        QVERIFY(summary.contains("Page: Accepted results"));QVERIFY(summary.contains("Page: Unclassified recipe fields"));
        QVERIFY(summary.contains("Schema mapping required"));QVERIFY(summary.contains("Experimental Limit"));
        widget.approvedPreview=QJsonObject{{"previewId","missing-recipe"}};widget.showPreview(QJsonObject{});
        QVERIFY(!widget.findChild<QPushButton*>("piezoConfirm")->isEnabled());
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }

    void completeRecipePreviewShowsAdvancedDiffAndRejectsStaleApproval() {try {
        static QTemporaryDir workspace;auto *session=Application::instance()->getSession();QString error;
        QVERIFY2(session->openProject(workspace.path(),&error),qPrintable(error));
        Model source=beam();auto baselineRecipe=RPiezoExperimentService::exampleRecipe(source);baselineRecipe["replaceLegacyConditions"]=true;
        baselineRecipe["initialDisplacements"]=QJsonArray{
            QJsonObject{{"physicalNodeId",101},{"values",QJsonArray{0.,0.,0.,0.,0.,0.}}},
            QJsonObject{{"physicalNodeId",123},{"values",QJsonArray{0.,0.,0.,0.,0.,0.}}}};
        const auto configured=RPiezoExperimentService::preview(source,baselineRecipe);
        QVERIFY2(configured.valid,qPrintable(configured.message));static_cast<RModel&>(source)=configured.candidate;
        session->addModel(source);session->setModelSelected(0,true);
        PiezoWorkbenchWidget widget;widget.setSession(session);permitLegacyReplacement(widget);
        const auto savedBefore=RPiezoExperimentService::recipeFromModel(session->getModel(0));
        QVERIFY(!savedBefore.isEmpty());
        auto *readout=widget.findChild<QComboBox*>("piezoEditor_ports_1_readout");
        auto *capacitance=widget.findChild<QDoubleSpinBox*>("piezoEditor_ports_1_Cf");
        auto *supportValue=widget.findChild<QDoubleSpinBox*>("piezoEditor_supports_0_value");
        auto *fixedInitial=widget.findChild<QDoubleSpinBox*>("piezoEditor_initialDisplacements_0_values_0");
        auto *initial=widget.findChild<QDoubleSpinBox*>("piezoEditor_initialDisplacements_1_values_2");
        auto *layerThickness=widget.findChild<QDoubleSpinBox*>("piezoLayerThickness0_0");
        QVERIFY(readout);QVERIFY(capacitance);QVERIFY(supportValue);QVERIFY(fixedInitial);QVERIFY(initial);QVERIFY(layerThickness);
        readout->setCurrentIndex(readout->findData("idealChargeAmplifier"));
        capacitance->setValue(3.3e-9);supportValue->setValue(1.e-5);fixedInitial->setValue(1.e-5);initial->setValue(2.e-5);layerThickness->setValue(6.e-4);
        auto *damping=widget.findChild<QComboBox*>("piezoDamping");QVERIFY(damping);damping->setCurrentText("rayleigh");QCoreApplication::processEvents();
        auto *alpha=widget.findChild<QDoubleSpinBox*>("piezoEditor_damping_rayleighAlpha");
        auto *beta=widget.findChild<QDoubleSpinBox*>("piezoEditor_damping_rayleighBeta");QVERIFY(alpha);QVERIFY(beta);
        alpha->setValue(.06);beta->setValue(.002);
        widget.findChild<QPushButton*>("piezoAddForce")->click();QCoreApplication::processEvents();
        auto *direction=widget.findChild<QComboBox*>("piezoForceDirection0");QVERIFY(direction);direction->setCurrentIndex(direction->findData("RY"));
        auto *forceValue=widget.findChild<QDoubleSpinBox*>("piezoEditor_excitation_channels_0_terms_0_value");QVERIFY(forceValue);forceValue->setValue(4.5);
        const auto beforeStamp=session->modelCommandStamp(0);const auto beforeUndo=session->getModel(0).getUndoStackSize();
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));
        auto *changes=widget.findChild<QTableWidget*>("piezoChanges");auto *confirm=widget.findChild<QPushButton*>("piezoConfirm");QVERIFY(changes);QVERIFY(confirm);
        QStringList rendered;
        for(int row=0;row<changes->rowCount();++row)for(int column=0;column<changes->columnCount();++column)
            if(changes->item(row,column))rendered.append(changes->item(row,column)->text());
        const QString summary=rendered.join('\n');
        QVERIFY(summary.contains("Complete recipe fields changed"));QVERIFY(summary.contains("Before:"));QVERIFY(summary.contains("Proposed:"));
        QVERIFY(summary.contains("Page: Source / devices / materials"));QVERIFY(summary.contains("Page: Sensors / electrodes / supports"));
        QVERIFY(summary.contains("Page: Modes / damping / export"));QVERIFY(summary.contains("Page: Controllers / state source"));
        QVERIFY(summary.contains("Page: Excitation / initial state"));QVERIFY(summary.contains("Page: Accepted results"));
        QVERIFY(summary.contains("physical element ID 501"));QVERIFY(summary.contains("Feedback capacitance"));QVERIFY(summary.contains("[F]"));
        QVERIFY(summary.contains("Ideal charge amplifier"));
        QVERIFY(summary.contains("Support at physical node ID"));QVERIFY(summary.contains("Initial displacements"));QVERIFY(summary.contains("Rayleigh alpha"));
        QVERIFY(summary.contains("0.06 [1/s]"));QVERIFY(summary.contains("0.002 [s]"));
        QVERIFY(summary.contains("Force channel node:123:RY"));QVERIFY(summary.contains("4.5 [N m]"));
        QCOMPARE(session->modelCommandStamp(0),beforeStamp);QCOMPARE(session->getModel(0).getUndoStackSize(),beforeUndo);
        QSignalSpy applied(&widget,&PiezoWorkbenchWidget::experimentApplied);
        widget.approvedPreview.remove("normalizedArguments");widget.applyPreview();
        QVERIFY(!widget.hasCurrentPreview());QVERIFY(!confirm->isEnabled());
        QCOMPARE(applied.size(),0);QCOMPARE(session->modelCommandStamp(0),beforeStamp);QCOMPARE(session->getModel(0).getUndoStackSize(),beforeUndo);
        QCOMPARE(RPiezoExperimentService::recipeFromModel(session->getModel(0)),savedBefore);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QVERIFY(confirm->isEnabled());
        widget.findChild<QSpinBox*>("piezoSteps")->setValue(widget.draftRecipe().value("steps").toInt()+1);
        QVERIFY(!widget.hasCurrentPreview());QVERIFY(!confirm->isEnabled());widget.applyPreview();
        QCOMPARE(session->modelCommandStamp(0),beforeStamp);QCOMPARE(session->getModel(0).getUndoStackSize(),beforeUndo);
        QCOMPARE(RPiezoExperimentService::recipeFromModel(session->getModel(0)),savedBefore);
        widget.previewExperiment();QVERIFY2(widget.hasCurrentPreview(),qPrintable(status(widget)));QVERIFY(confirm->isEnabled());
        widget.applyPreview();QCOMPARE(applied.size(),1);
        QCOMPARE(RPiezoExperimentService::recipeFromModel(session->getModel(0)),widget.draftRecipe());
        QCOMPARE(session->getModel(0).getUndoStackSize(),beforeUndo+1);
    }catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());} }

};

int runPiezoWorkbenchWidgetTests(int argc,char **argv) {
    TestPiezoWorkbenchWidget test;return QTest::qExec(&test,argc,argv);
}
#include "tst_piezo_workbench_widget.moc"
