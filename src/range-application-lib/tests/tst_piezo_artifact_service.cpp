#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDir>
#include <QSaveFile>
#include <QCryptographicHash>
#include <rml_model.h>
#include <rbl_error.h>
#include <rpiezo_artifact_service.h>
#include <rpiezo_experiment_service.h>
#include <piezo_axial_backend_bridge.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_axial_operators.h>
#include <piezo_native_backend_bridge.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rstructural_piezo_layered_model_adapter.h>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <cstddef>
#include <vector>
#include <cstring>
namespace {
RModel scene(bool plate=false,bool configured=false) {
    RModel m;const double xy[4][2]={{0,0},{1,0},{1,1},{0,1}};
    for(uint i=0;i<(plate?4u:3u);++i){m.addNode(plate?RNode(xy[i][0],xy[i][1],0):RNode(double(i),0,0));m.setNodePhysicalId(i,101+11*i);}
    for(uint i=0;i<(plate?1u:2u);++i){RElement e(plate?R_ELEMENT_QUAD1:R_ELEMENT_BEAM1);for(uint j=0;j<(plate?4u:2u);++j)e.setNodeId(j,plate?j:i+j);m.addElement(e,true,0);m.setElementPhysicalId(i,501+13*i);}
    // This fixture declares its complete loading through the layered setup.
    // removeAllEnvironmentConditions() would reinstate these legacy defaults.
    for(uint i=0;i<(plate?m.getNSurfaces():m.getNLines());++i){
        RElementGroup &group=plate?static_cast<RElementGroup &>(m.getSurface(i)):static_cast<RElementGroup &>(m.getLine(i));
        group.removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        group.removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_TEMPERATURE);
    }
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));m.getProblemSetup().setMassMatrixType(RMassMatrixType::Consistent);
    auto &d=m.getProblemSetup().getStructuralDynamicsSetup();d.setEnabled(true);d.setTimeStep(.01);d.setNSteps(8);m.getTimeSolver().setInputStartTime(.01);
    auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();s.enabled=true;if(plate)s.family=RStructuralPiezoLayeredModelFamily::FlatPlate;
    const auto native=RStructuralPiezoContinuumMaterial::isotropic(30,.2,7);
    RStructuralPiezoLayeredMaterial h;h.materialId="host";h.elasticityOrCompliance=native.elasticity;h.density=7;h.sourceId="independent-example";h.sourceSha256=QString(64,'a');
    auto p=h;p.materialId="piezo";p.piezoelectric=true;p.coupling[12]=2;p.coupling[13]=2;p.permittivity={{4,0,0,0,4,0,0,0,4}};s.materials={h,p};
    RStructuralPiezoLayeredPort drive;drive.readout.portId="drive";drive.readout.role=RStructuralPiezoPortRole::ActuatorSensor;drive.readout.readout=RStructuralPiezoReadoutMode::ActuatorCharge;drive.initialValue=.12;
    RStructuralPiezoLayeredPort sense;sense.readout.portId="sense";sense.readout.role=RStructuralPiezoPortRole::Sensor;sense.readout.readout=RStructuralPiezoReadoutMode::OpenCircuitVoltage;sense.initialValue=.03;s.ports={drive,sense};
    for(uint i=0;i<(plate?1u:2u);++i){RStructuralPiezoLayeredModelLayer host;host.materialId="host";host.lowerZ=.1;host.upperZ=.3;auto active=host;active.materialId="piezo";active.lowerZ=.3;active.upperZ=.4;active.portId="drive";auto sensor=active;sensor.lowerZ=.4;sensor.upperZ=.45;sensor.portId="sense";s.members.push_back({501+13*i,{host,active,sensor}});}
    using D=RStructuralPiezoPhysicalDof;
    for(uint node:plate?std::vector<uint>{101,134}:std::vector<uint>{101})for(D dof:plate?std::vector<D>{D::UX,D::UY,D::UZ,D::RX,D::RY}:std::vector<D>{D::UX,D::UZ,D::RY})s.supports.push_back({node,dof,0});
    s.backgroundForces={{123,{{.2,0,-.1,0,0,0}}}};s.initialVelocities={{123,{{.04,0,.08,0,0,0}}}};
    s.damping.kind=RStructuralPiezoDampingKind::Rayleigh;s.damping.rayleighAlpha=.06;s.damping.rayleighBeta=.005;
    if(configured){s.excitationConfiguration="ramp/v1";s.controllerConfiguration="counter+observer/v1";s.designConfiguration="native-experiment/v1";}
    s.bindToModel(m);return m;
}



QByteArray read(const QString &path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("read failed");return f.readAll();}
void write(const QString &path,const QByteArray &bytes){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size())throw std::runtime_error("write failed");}
QStringList records(const RModel &source,const QString &dir,bool provider=false){
    RStructuralPiezoLayeredRuntimeCallbacks cb;if(provider)cb=PiezoNativeBackendBridge::create(source).callbacks;
    RStructuralPiezoLayeredNativeRun run(source,cb);QStringList result;
    for(uint i=0;i<4;++i){if(i){auto p=run.prepare(source);run.validatePrepared(*p);run.commit(*p);}const auto path=QDir(dir).filePath(QString("accepted%1.%2").arg(i).arg(i%2?"ram":"rbm"));auto m=run.acceptedModel(source);result.append(m.nativeWritePaths(path,false).first());m.write(path,false);}return result;
}
PiezoResultSeries::TraceProof traceRecords(const QString &family,const QString &directory,bool factoryNone=false)
{
    RModel source;
    if(family=="axialRod"){auto recipe=RPiezoExperimentService::exampleRecipe(RModel{},family);recipe["replaceLegacyConditions"]=true;recipe["steps"]=4;recipe["controller"]=QJsonObject{{"kind","PI"},{"kp",.3},{"ki",.02},{"selectedOutputs",QJsonArray{"tip_displacement"}},{"bias",QJsonArray{.3}}};const auto candidate=RPiezoExperimentService::preview(RModel{},recipe);if(!candidate.valid)throw std::runtime_error(qPrintable(candidate.message));source=candidate.candidate;}
    else{source=scene(family=="flatPlate");auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
        if(!factoryNone)setup.controllerConfiguration=QJsonDocument(QJsonObject{{"kind","PI"},{"kp",.4},{"ki",.02},{"selectedOutputs",QJsonArray{"sense"}},{"bias",QJsonArray{.3}},{"controlEveryFESteps",3}}).toJson(QJsonDocument::Compact);
        setup.bindToModel(source);}
    PiezoResultSeries::TraceProof proof;
    const auto store=[&](auto &run,auto encode){auto model=run.acceptedModel(source);const auto basePath=QDir(directory).filePath("trace-base.rbm");model.write(basePath,false);proof.baseModelPath=model.nativeWritePaths(basePath,false).first();
        const auto &state=run.acceptedRuntime();PiezoAcceptedTrace::Header header;header.family=family=="axialRod"?"axial":"layered";header.sourceFingerprint=state.sourceFingerprint;header.configurationIdentity=QString::fromLatin1(state.configurationIdentity.toHex());header.backendIdentity=state.backendIdentity;header.baseSeq=0;header.targetSteps=state.targetSteps;header.initialTime=state.initialTime;header.dt=state.dt;
        header.baseModelFile=QFileInfo(proof.baseModelPath).fileName();header.baseModelSha256=QCryptographicHash::hash(read(proof.baseModelPath),QCryptographicHash::Sha256);header.contextSha256=QCryptographicHash::hash("actual-stream-csv-context",QCryptographicHash::Sha256);header.targetTimeGridSha256=PiezoAcceptedTrace::targetTimeGridSha256(header.initialTime,header.dt,header.targetSteps);
        proof.tracePath=QDir(directory).filePath("csv.trace");QFile trace(proof.tracePath);if(!trace.open(QIODevice::ReadWrite|QIODevice::Truncate))throw std::runtime_error("CSV trace fixture write failed");header=PiezoAcceptedTrace::writeHeader(trace,header);auto cursor=PiezoAcceptedTrace::initialCursor(header);
        for(uint sequence=0;sequence<4;++sequence){if(sequence){auto prepared=run.prepare(source);run.validatePrepared(*prepared);run.commit(*prepared);}cursor=PiezoAcceptedTrace::append(trace,header,cursor,sequence,sequence?header.initialTime+sequence*header.dt:header.initialTime,encode(run.acceptedRuntime()));}
        proof.baseModelSha256=header.baseModelSha256;proof.expectedHeaderSha256=header.headerSha32;proof.expectedContextSha256=header.contextSha256;proof.expectedSourceFingerprint=header.sourceFingerprint;proof.cursor=cursor;
    };
    if(family=="axialRod"){const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun run(source,backend.callbacks);store(run,[](const auto &state){return RStructuralPiezoAxialRestartCodec::encode(state);});}
    else{const auto backend=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,backend.callbacks);store(run,[](const auto &state){return RStructuralPiezoLayeredRestartCodec::encode(state);});}return proof;
}
QJsonObject metadata(const QByteArray &csv){return QJsonDocument::fromJson(csv.left(csv.indexOf('\n')).mid(2)).object();}
std::vector<std::vector<QByteArray>> csvRecords(const QByteArray &payload){
    std::vector<std::vector<QByteArray>> result;std::vector<QByteArray> record;QByteArray field;
    bool quoted=false,closedQuote=false;
    for(qsizetype i=0;i<payload.size();++i){
        const char c=payload[i];
        if(quoted){
            if(c=='"'){
                if(i+1<payload.size() && payload[i+1]=='"'){field+='"';++i;}
                else{quoted=false;closedQuote=true;}
            }else field+=c;
        }else if(c==','){
            record.push_back(field);field.clear();closedQuote=false;
        }else if(c=='\n' || c=='\r'){
            if(c=='\r' && i+1<payload.size() && payload[i+1]=='\n')++i;
            record.push_back(field);result.push_back(record);record.clear();field.clear();closedQuote=false;
        }else if(c=='"' && field.isEmpty() && !closedQuote){
            quoted=true;
        }else{
            if(c=='"' || closedQuote)throw std::runtime_error("malformed CSV field");
            field+=c;
        }
    }
    if(quoted)throw std::runtime_error("unterminated CSV quote");
    if(!record.empty() || !field.isEmpty() || closedQuote){record.push_back(field);result.push_back(record);}
    return result;
}
QByteArray column(const QByteArray &csv,const QByteArray &name,int row=0){
    const auto metadataEnd=csv.indexOf('\n');if(metadataEnd<0)throw std::runtime_error("missing CSV metadata");
    const auto parsed=csvRecords(csv.mid(metadataEnd+1));
    if(parsed.empty() || row<0 || std::size_t(row)+1>=parsed.size())throw std::runtime_error("missing CSV row");
    const auto &header=parsed.front();const auto found=std::find(header.begin(),header.end(),name);
    if(found==header.end())throw std::runtime_error("missing CSV column");
    const auto &values=parsed[std::size_t(row)+1];
    if(values.size()!=header.size())throw std::runtime_error("CSV column count mismatch");
    return values[std::size_t(found-header.begin())];
}
using Dense=std::vector<double>;
Dense jsonMatrix(const QJsonValue &value,size_t rows,size_t columns){
    if(!value.isArray() || size_t(value.toArray().size())!=rows)throw std::runtime_error("JSON matrix row count differs");
    Dense result;result.reserve(rows*columns);
    for(const auto &row:value.toArray()){
        if(!row.isArray() || size_t(row.toArray().size())!=columns)throw std::runtime_error("JSON matrix column count differs");
        for(const auto &entry:row.toArray()){if(!entry.isDouble() || !std::isfinite(entry.toDouble()))throw std::runtime_error("JSON matrix value is invalid");result.push_back(entry.toDouble());}
    }
    return result;
}
bool close(double actual,double expected,double relative=2e-9,double absolute=2e-12){
    return std::isfinite(actual) && std::isfinite(expected) && std::abs(actual-expected)<=std::max(absolute,relative*std::max(std::abs(actual),std::abs(expected)));
}
void checkMatrix(const QJsonObject &object,const char *key,const Dense &expected,size_t rows,size_t columns){
    const auto actual=jsonMatrix(object.value(QLatin1String(key)),rows,columns);
    if(actual.size()!=expected.size())throw std::runtime_error(std::string(key)+" matrix size differs from native FE operator");
    for(size_t i=0;i<actual.size();++i)if(!close(actual[i],expected[i]))throw std::runtime_error(std::string(key)+" matrix differs from native FE operator at flat entry "+std::to_string(i));
}
void checkVector(const QJsonObject &object,const char *key,const Dense &expected){
    const auto value=object.value(QLatin1String(key));
    if(!value.isArray() || size_t(value.toArray().size())!=expected.size())throw std::runtime_error(std::string(key)+" vector size differs from native FE operator");
    const auto actual=value.toArray();for(size_t i=0;i<expected.size();++i)if(!actual[int(i)].isDouble() || !close(actual[int(i)].toDouble(),expected[i]))throw std::runtime_error(std::string(key)+" vector differs from native FE operator at entry "+std::to_string(i));
}
void checkRomMatrix(const QJsonValue &value,const Dense &expected,size_t rows,size_t columns,const char *name,double relative=2e-9,double absolute=2e-12){
    const auto actual=jsonMatrix(value,rows,columns);for(size_t i=0;i<actual.size();++i)if(!close(actual[i],expected[i],relative,absolute))throw std::runtime_error(QStringLiteral("%1 differs at entry %2: exported=%3, oracle=%4").arg(QString::fromUtf8(name)).arg(qulonglong(i)).arg(actual[i],0,'g',17).arg(expected[i],0,'g',17).toStdString());
}
Dense multiply(const Dense &a,size_t rows,size_t inner,const Dense &b,size_t columns){
    if(a.size()!=rows*inner || b.size()!=inner*columns)throw std::runtime_error("Dense product dimensions differ");
    Dense result(rows*columns,0);for(size_t i=0;i<rows;++i)for(size_t k=0;k<inner;++k)for(size_t j=0;j<columns;++j)result[i*columns+j]+=a[i*inner+k]*b[k*columns+j];return result;
}
Dense transposeMultiply(const Dense &a,size_t rows,size_t columns,const Dense &b,size_t rightColumns){
    if(a.size()!=rows*columns || b.size()!=rows*rightColumns)throw std::runtime_error("Dense transpose product dimensions differ");
    Dense result(columns*rightColumns,0);for(size_t i=0;i<columns;++i)for(size_t k=0;k<rows;++k)for(size_t j=0;j<rightColumns;++j)result[i*rightColumns+j]+=a[k*columns+i]*b[k*rightColumns+j];return result;
}
Dense solve(const Dense &a,size_t n,const Dense &rhs,size_t columns){
    if(a.size()!=n*n || rhs.size()!=n*columns)throw std::runtime_error("Dense solve dimensions differ");
    Dense left=a,result=rhs;
    for(size_t pivot=0;pivot<n;++pivot){size_t best=pivot;for(size_t row=pivot+1;row<n;++row)if(std::abs(left[row*n+pivot])>std::abs(left[best*n+pivot]))best=row;
        if(left[best*n+pivot]==0 || !std::isfinite(left[best*n+pivot]))throw std::runtime_error("Native FE operator is singular in ROM oracle");
        if(best!=pivot){for(size_t column=0;column<n;++column)std::swap(left[pivot*n+column],left[best*n+column]);for(size_t column=0;column<columns;++column)std::swap(result[pivot*columns+column],result[best*columns+column]);}
        const double diagonal=left[pivot*n+pivot];for(size_t column=pivot;column<n;++column)left[pivot*n+column]/=diagonal;for(size_t column=0;column<columns;++column)result[pivot*columns+column]/=diagonal;
        for(size_t row=0;row<n;++row){if(row==pivot)continue;const double factor=left[row*n+pivot];for(size_t column=pivot;column<n;++column)left[row*n+column]-=factor*left[pivot*n+column];for(size_t column=0;column<columns;++column)result[row*columns+column]-=factor*result[pivot*columns+column];}
    }
    return result;
}
Dense horizontal(const Dense &a,size_t rows,size_t leftColumns,const Dense &b,size_t rightColumns){
    if(a.size()!=rows*leftColumns || b.size()!=rows*rightColumns)throw std::runtime_error("Dense horizontal concatenation dimensions differ");
    Dense result(rows*(leftColumns+rightColumns));for(size_t row=0;row<rows;++row){std::copy_n(a.begin()+std::ptrdiff_t(row*leftColumns),leftColumns,result.begin()+std::ptrdiff_t(row*(leftColumns+rightColumns)));std::copy_n(b.begin()+std::ptrdiff_t(row*rightColumns),rightColumns,result.begin()+std::ptrdiff_t(row*(leftColumns+rightColumns)+leftColumns));}return result;
}
void verifySingleModeRom(const QJsonObject &package,size_t dofs,size_t actuatorInputs,size_t outputs,double samplingPeriod){
    constexpr size_t modes=1;const auto mass=jsonMatrix(package.value("M"),dofs,dofs),stiffness=jsonMatrix(package.value("K"),dofs,dofs),damping=jsonMatrix(package.value("C"),dofs,dofs);
    const auto actuatorB=jsonMatrix(package.value("B"),dofs,actuatorInputs),forceB=jsonMatrix(package.value("Bforce"),dofs,size_t(package.value("disturbanceInputs").toArray().size()));
    const size_t forceInputs=size_t(package.value("disturbanceInputs").toArray().size()),inputs=actuatorInputs+forceInputs;
    const auto allB=horizontal(actuatorB,dofs,actuatorInputs,forceB,forceInputs);
    const auto lq=jsonMatrix(package.value("Lq"),outputs,dofs),lv=jsonMatrix(package.value("Lv"),outputs,dofs),la=jsonMatrix(package.value("La"),outputs,dofs),direct=jsonMatrix(package.value("D"),outputs,actuatorInputs),forceDirect=jsonMatrix(package.value("Dforce"),outputs,forceInputs);
    const auto design=package.value("design").toObject();const auto selected=design.value("selectedModes").toArray();
    if(selected.size()!=int(modes) || selected[0].toInt(-1)!=0)throw std::runtime_error("Export did not use the configured first selected mode");
    const auto phi=jsonMatrix(design.value("Phi"),dofs,modes);const auto massModal=transposeMultiply(phi,dofs,modes,multiply(mass,dofs,dofs,phi,modes),modes);
    const auto stiffnessModal=transposeMultiply(phi,dofs,modes,multiply(stiffness,dofs,dofs,phi,modes),modes);
    const auto dampingModal=transposeMultiply(phi,dofs,modes,multiply(damping,dofs,dofs,phi,modes),modes);
    if(!close(massModal[0],1.,2e-8,2e-10))throw std::runtime_error("Selected mode is not mass-normalized against actual native FE M");
    const auto modalB=transposeMultiply(phi,dofs,modes,allB,inputs);const auto frequencies=design.value("omega").toArray();if(frequencies.size()!=int(modes))throw std::runtime_error("Selected modal frequency vector shape differs");
    const double modalMass=massModal[0],omega=std::sqrt(stiffnessModal[0]/modalMass),alpha=dampingModal[0]/(2*modalMass);
    if(!(omega>0) || !close(frequencies[0].toDouble(),omega,2e-8,1e-11))throw std::runtime_error("Selected modal frequency differs from native FE generalized eigenproblem");
    const double square=omega*omega-alpha*alpha;if(!(square>0))throw std::runtime_error("Selected mode is not underdamped for the independent closed-form ZOH oracle");
    const double effectiveInputScale=1/modalMass;Dense continuousA{0,1,-omega*omega,-2*alpha},continuousB(2*inputs,0);
    for(size_t input=0;input<inputs;++input)continuousB[inputs+input]=modalB[input]*effectiveInputScale;
    const double wd=std::sqrt(square),decay=std::exp(-alpha*samplingPeriod),c=std::cos(wd*samplingPeriod),s=std::sin(wd*samplingPeriod);
    Dense discreteA{decay*(c+alpha*s/wd),decay*s/wd,-decay*omega*omega*s/wd,decay*(c-alpha*s/wd)},discreteB(2*inputs,0);
    for(size_t input=0;input<inputs;++input){const double accelerationGain=modalB[input]/modalMass/omega/omega;discreteB[input]=accelerationGain*(1-discreteA[0]);discreteB[inputs+input]=-accelerationGain*discreteA[2];}
    const auto solveK=solve(mass,dofs,stiffness,dofs),solveC=solve(mass,dofs,damping,dofs),solveB=solve(mass,dofs,allB,inputs);
    auto outputQFull=lq,outputVFull=lv;const auto accelerationFromQ=multiply(la,outputs,dofs,solveK,dofs),accelerationFromV=multiply(la,outputs,dofs,solveC,dofs);
    for(size_t i=0;i<outputQFull.size();++i){outputQFull[i]-=accelerationFromQ[i];outputVFull[i]-=accelerationFromV[i];}
    const auto outputQ=multiply(outputQFull,outputs,dofs,phi,modes),outputV=multiply(outputVFull,outputs,dofs,phi,modes);
    Dense expectedD=horizontal(direct,outputs,actuatorInputs,forceDirect,forceInputs);
    for(size_t row=0;row<outputs;++row)for(size_t input=0;input<actuatorInputs;++input)for(size_t k=0;k<dofs;++k)expectedD[row*inputs+input]+=la[row*dofs+k]*solveB[k*inputs+input];
    const auto rom=design.value("forceAware").toObject().value("ROM").toObject();const auto continuous=rom.value("continuous").toObject(),discrete=rom.value("discrete").toObject();
    Dense expectedCy(outputs*2);for(size_t row=0;row<outputs;++row){expectedCy[row*2]=outputQ[row];expectedCy[row*2+1]=outputV[row];}
    checkRomMatrix(continuous.value("A"),continuousA,2,2,"continuous ROM A");checkRomMatrix(continuous.value("B"),continuousB,2,inputs,"continuous ROM B");checkRomMatrix(continuous.value("Cy"),expectedCy,outputs,2,"continuous ROM Cy");checkRomMatrix(continuous.value("D"),expectedD,outputs,inputs,"continuous ROM D");
    checkRomMatrix(discrete.value("A"),discreteA,2,2,"discrete ROM A from scalar exact ZOH");checkRomMatrix(discrete.value("B"),discreteB,2,inputs,"discrete ROM B from scalar exact ZOH");checkRomMatrix(discrete.value("Cy"),expectedCy,outputs,2,"discrete ROM Cy");checkRomMatrix(discrete.value("D"),expectedD,outputs,inputs,"discrete ROM D");
}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoArtifactService:public QObject{
    Q_OBJECT
private slots:
    void package_data();void package();void records_data();void records();void rejectionKeepsDestination();void configuredRecords();
    void nativeDestinationsPreserved();void canonicalNativeDestinationsPreserved();
    void expectedSourcePreservesDestination();
    void axialArtifacts();
    void actualMatrixPackageFEAndRom_data();void actualMatrixPackageFEAndRom();
    void csvQuotedFields();
    void compactTraceCSVStreamsActualSignals_data();void compactTraceCSVStreamsActualSignals();
    void compactTraceCSVRejectsTamperAndKeepsDestination();
    void nativeFactoryTargetSurvivesBaselineRoundTrip_data();void nativeFactoryTargetSurvivesBaselineRoundTrip();
};
void TestPiezoArtifactService::package_data(){QTest::addColumn<bool>("plate");QTest::newRow("beam")<<false;QTest::newRow("plate")<<true;}
void TestPiezoArtifactService::package() try{
    QFETCH(bool,plate);const auto m=scene(plate);QTemporaryDir dir;QVERIFY(dir.isValid());QString error;RPiezoArtifactService::Report report;
    const auto path=dir.filePath("package.json");QVERIFY2(RPiezoArtifactService::writeMatrixPackage(m,path,&error,&report),qPrintable(error));const auto actual=QJsonDocument::fromJson(read(path)).object();const auto expected=PiezoNativeBackendBridge::create(m).exportPackage;
    QCOMPARE(report.modelFamily,QString(plate?"flatPlate":"planarBeam"));
    for(auto it=expected.begin();it!=expected.end();++it)QCOMPARE(actual.value(it.key()),it.value());
    RStructuralPiezoLayeredModelAdapter adapter(m,m.getProblemSetup().getStructuralPiezoLayeredSetup());QCOMPARE(actual["M"].toArray()[0].toArray()[0].toDouble(),adapter.data().mass[0]);QCOMPARE(actual["G"].toArray()[0].toArray()[0].toDouble(),adapter.mesh().operators.coupling[0]);QCOMPARE(actual["nodes"].toArray().size(),int(m.getNNodes()));QCOMPARE(actual["units"].toObject()["physicalRotation"].toString(),QString("rad"));QCOMPARE(report.sourceFingerprint,adapter.data().sourceFingerprint);QVERIFY(!actual["sourceConfigurationSha256"].toString().isEmpty());
    auto changed=m;changed.getProblemSetup().getStructuralPiezoLayeredSetup().materials[0].density+=1;QVERIFY(!RPiezoArtifactService::writeMatrixPackage(changed,path,&error));QCOMPARE(QJsonDocument::fromJson(read(path)).object(),actual);
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::csvQuotedFields(){
    const QByteArray csv="# {}\r\n\"plain\",\"port,\"\"quoted\"\"\",\"multiline\"\r\n1,\"2,3\",\"first\nsecond\"\r\n";
    QCOMPARE(column(csv,"plain"),QByteArray("1"));
    QCOMPARE(column(csv,"port,\"quoted\""),QByteArray("2,3"));
    QCOMPARE(column(csv,"multiline"),QByteArray("first\nsecond"));
}
void TestPiezoArtifactService::compactTraceCSVStreamsActualSignals_data(){QTest::addColumn<QString>("family");QTest::addColumn<bool>("factoryNone");
    for(const auto &family:{"axialRod","planarBeam","flatPlate"})QTest::newRow(family)<<QString(family)<<false;
    QTest::newRow("planarBeam-empty-config-formal-factory")<<QString("planarBeam")<<true;
    QTest::newRow("flatPlate-empty-config-formal-factory")<<QString("flatPlate")<<true;}
void TestPiezoArtifactService::compactTraceCSVStreamsActualSignals() try{
    QFETCH(QString,family);QFETCH(bool,factoryNone);QTemporaryDir directory;QVERIFY(directory.isValid());const auto proof=traceRecords(family,directory.path(),factoryNone);const auto numeric=PiezoResultSeries::readAcceptedTrace(proof);QVERIFY2(numeric.valid,qPrintable(numeric.message));const auto csvPath=directory.filePath("trace.csv");QString error;RPiezoArtifactService::Report receipt;
    QVERIFY2(RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,csvPath,&error,&receipt),qPrintable(error));const auto csv=read(csvPath);const auto meta=metadata(csv);QCOMPARE(receipt.recordCount,4u);QVERIFY(receipt.continuousFE);QCOMPARE(receipt.modelFamily,numeric.dataset->modelFamily);QCOMPARE(meta["family"].toString(),numeric.dataset->family);QCOMPARE(meta["modelFamily"].toString(),numeric.dataset->modelFamily);QCOMPARE(meta["schema"].toString(),QString("range.piezo-accepted-trace-timeseries/v1"));QCOMPARE(meta["artifactSha256"].toString(),numeric.dataset->artifactSha256);QCOMPARE(meta["recordCount"].toInt(),4);QVERIFY(meta["sampledCoverage"].toBool());QCOMPARE(meta["channels"].toArray().size(),numeric.dataset->channels.size());
    QCOMPARE(meta["csvPayloadSha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(csv.mid(csv.indexOf('\n')+1),QCryptographicHash::Sha256).toHex()));
    for(int row=0;row<4;++row){QCOMPARE(column(csv,"completedFE [1]",row).toUInt(),numeric.dataset->completedFE[row]);QCOMPARE(column(csv,"time [s]",row).toDouble(),numeric.dataset->times[row]);
        for(qsizetype c=0;c<numeric.dataset->channels.size();++c){const auto &channel=numeric.dataset->channels[c];const auto field=column(csv,(channel.channelId+" ["+channel.unit+"]").toUtf8(),row);const double decoded=field.toDouble();QCOMPARE(decoded,numeric.dataset->columns[c][row]);QCOMPARE(std::signbit(decoded),std::signbit(numeric.dataset->columns[c][row]));}
    }
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::compactTraceCSVRejectsTamperAndKeepsDestination() try{
    QTemporaryDir directory;const auto proof=traceRecords("planarBeam",directory.path());const auto target=directory.filePath("sentinel.csv");write(target,"sentinel");QString error;
    bool checked=false;RPiezoArtifactService::Report prior;prior.recordCount=77;
    QVERIFY(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,target,&error,&prior,[&]{
        checked=true;throw std::runtime_error("Queued authority changed before CSV commit");}));
    QVERIFY(checked);QVERIFY(error.contains("Queued authority"));QCOMPARE(read(target),QByteArray("sentinel"));QCOMPARE(prior.recordCount,77u);
    auto bad=proof;bad.expectedSourceFingerprint=QString(64,'f');QVERIFY(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(bad,target,&error));QCOMPARE(read(target),QByteArray("sentinel"));bad=proof;bad.expectedContextSha256=QByteArray(32,'f');QVERIFY(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(bad,target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    QFile tail(proof.tracePath);QVERIFY(tail.open(QIODevice::Append));tail.write("uncommitted-tail");tail.close();QVERIFY(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    const auto originalBase=read(proof.baseModelPath);QVERIFY(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,proof.baseModelPath,&error));QCOMPARE(read(proof.baseModelPath),originalBase);
    const auto originalTrace=read(proof.tracePath);QVERIFY(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,proof.tracePath,&error));QCOMPARE(read(proof.tracePath),originalTrace);
    QFile trim(proof.tracePath);QVERIFY(trim.open(QIODevice::ReadWrite));QVERIFY(trim.resize(proof.cursor.byteEnd));trim.close();
    // Stage a CSV, then change the source in the completion callback. The final
    // source checks must reject, so the caller cannot publish the QSaveFile.
    QSaveFile staged(target);QVERIFY(staged.open(QIODevice::WriteOnly));const auto changed=PiezoResultSeries::streamAcceptedTrace(proof,{},[&](const auto &frame,const auto &){staged.write(QByteArray::number(frame.timeS,'g',17)+'\n');},[&](const auto &){QFile base(proof.baseModelPath);if(!base.open(QIODevice::Append))throw std::runtime_error("Atomic CSV source-change fixture failed");base.write("source changed during staging");});QVERIFY(!changed.valid);staged.cancelWriting();QCOMPARE(read(target),QByteArray("sentinel"));
    write(proof.baseModelPath,originalBase);QVERIFY2(RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,target,&error),qPrintable(error));QVERIFY(read(target)!=QByteArray("sentinel"));
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::records_data(){
    QTest::addColumn<bool>("plate");QTest::addColumn<bool>("quotedPorts");
    QTest::newRow("beam")<<false<<false;QTest::newRow("plate")<<true<<false;
    QTest::newRow("beam-quoted-ports")<<false<<true;QTest::newRow("plate-quoted-ports")<<true<<true;
}
void TestPiezoArtifactService::records() try{
    QFETCH(bool,plate);QFETCH(bool,quotedPorts);auto m=scene(plate);
    if(quotedPorts){
        auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();
        const QString drive="drive,main",sense="sense,probe";
        setup.ports[0].readout.portId=drive;setup.ports[1].readout.portId=sense;
        for(auto &member:setup.members)for(auto &layer:member.layers){if(layer.portId=="drive")layer.portId=drive;else if(layer.portId=="sense")layer.portId=sense;}
        setup.bindToModel(m);
    }
    QTemporaryDir dir;const auto paths=::records(m,dir.path());const auto csvPath=dir.filePath("accepted.csv");QString error;RPiezoArtifactService::Report report;QStringList reversed=paths;std::reverse(reversed.begin(),reversed.end());QVERIFY2(RPiezoArtifactService::writeAcceptedTimeSeries(reversed,csvPath,&error,&report),qPrintable(error));
    const auto bytes=read(csvPath);const auto meta=metadata(bytes);QCOMPARE(report.recordCount,4u);QVERIFY(report.continuousFE);QCOMPARE(report.modelFamily,QString(plate?"flatPlate":"planarBeam"));QCOMPARE(meta["modelFamily"].toString(),report.modelFamily);QCOMPARE(meta["family"].toString(),report.modelFamily);QCOMPARE(meta["records"].toArray().size(),4);QCOMPARE(column(bytes,"completedFE [1]",3).toUInt(),3u);
    QCOMPARE(meta["csvPayloadSha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(bytes.mid(bytes.indexOf('\n')+1),QCryptographicHash::Sha256).toHex()));
    QCOMPARE(meta["records"].toArray()[0].toObject()["sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(read(paths[0]),QCryptographicHash::Sha256).toHex()));
    RModel disk;disk.read(paths[3]);disk.getProblemSetup().setRestart(true);RStructuralPiezoLayeredNativeRun restored(disk);const auto &s=restored.acceptedState();QCOMPARE(column(bytes,"node:123:Z:q.absolute [m]",3).toDouble(),s.physicalDisplacements[14]);QCOMPARE(column(bytes,"energy:balanceResidual [J]",3).toDouble(),s.energyBalanceResidual);
    for(const auto &r:s.readouts)QCOMPARE(column(bytes,("readout:"+r.portId+":absolute ["+r.unit+"]").toUtf8(),3).toDouble(),r.value);
    QVERIFY2(RPiezoArtifactService::writeAcceptedTimeSeries({paths[0],paths[3]},csvPath,&error,&report),qPrintable(error));QVERIFY(!report.continuousFE);QCOMPARE(report.qualification,QString("sampled-certified-accepted-native-records"));
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::rejectionKeepsDestination() try{
    const auto m=scene();QTemporaryDir dir;const auto paths=::records(m,dir.path());const auto target=dir.filePath("sentinel.csv");write(target,"sentinel");QString error;
    QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1],paths[1]},target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    const auto duplicate=dir.filePath("duplicate.ram");QVERIFY(QFile::copy(paths[1],duplicate));QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1],duplicate},target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1]},paths[1],&error));
    const auto link=dir.filePath("latest.rbm");RModel::writeLink(link,paths[2]);QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({link},target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    RModel changed;changed.read(paths[2]);changed.getProblemSetup().getStructuralPiezoLayeredSetup().materials[0].density+=1;const auto changedPath=dir.filePath("changed.rbm");changed.write(changedPath,false);QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1],changedPath},target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    const auto missing=dir.filePath("source.rbm");m.write(missing,false);QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({missing},target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
    RModel corrupted;corrupted.read(paths[1]);auto state=RStructuralPiezoLayeredRestartCodec::decode(corrupted.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes);state.physical.physicalDisplacements[14]+=1;corrupted.getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes=RStructuralPiezoLayeredRestartCodec::encode(state);const auto corruptedPath=dir.filePath("corrupted.rbm");corrupted.write(corruptedPath,false);QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({corruptedPath},target,&error));QCOMPARE(read(target),QByteArray("sentinel"));
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::configuredRecords() try{
    auto m=scene();auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();setup.controllerConfiguration=QJsonDocument(QJsonObject{{"kind","PI"},{"kp",.4},{"ki",.02},{"selectedOutputs",QJsonArray{"sense"}},{"bias",QJsonArray{.3}},{"controlEveryFESteps",3},{"voltageLower",QJsonArray{-.5}},{"voltageUpper",QJsonArray{.5}},{"deltaVoltageLimits",QJsonArray{.06}}}).toJson(QJsonDocument::Compact);setup.bindToModel(m);
    QTemporaryDir dir;const auto paths=::records(m,dir.path(),true);QString error;RPiezoArtifactService::Report report;QVERIFY2(RPiezoArtifactService::writeAcceptedTimeSeries(paths,dir.filePath("controlled.csv"),&error,&report),qPrintable(error));QCOMPARE(report.recordCount,4u);
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::nativeFactoryTargetSurvivesBaselineRoundTrip_data()
{
    QTest::addColumn<bool>("plate");QTest::addColumn<QString>("suffix");
    for(bool plate:{false,true})for(const auto &suffix:{"rbm","ram"})QTest::newRow(qPrintable(QString("%1-%2").arg(plate?"plate":"beam",suffix)))<<plate<<QString(suffix);
}
void TestPiezoArtifactService::nativeFactoryTargetSurvivesBaselineRoundTrip() try{
    QFETCH(bool,plate);QFETCH(QString,suffix);auto source=scene(plate);
    auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();setup.controllerConfiguration=QJsonDocument(QJsonObject{{"kind","PI"},{"kp",.4},{"ki",.02},
        {"selectedOutputs",QJsonArray{"sense"}},{"bias",QJsonArray{.3}},{"controlEveryFESteps",3}}).toJson(QJsonDocument::Compact);setup.bindToModel(source);
    source.getTimeSolver().setInputNTimeSteps(1);
    const uint legacyInputCount=source.getTimeSolver().getInputNTimeSteps(),nativeTarget=source.getProblemSetup().getStructuralDynamicsSetup().getNSteps();
    QVERIFY(legacyInputCount<nativeTarget);const auto before=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredNativeRun run(source,before.callbacks);
    const auto baselineBytes=RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime());auto baseline=run.acceptedModel(source);
    QCOMPARE(baseline.getTimeSolver().getInputNTimeSteps(),nativeTarget);QTemporaryDir dir;QVERIFY(dir.isValid());
    const auto requested=dir.filePath("baseline."+suffix),saved=baseline.nativeWritePaths(requested,false).first();baseline.write(requested,false);
    RModel persisted;persisted.read(saved);persisted.getProblemSetup().setRestart(true);const auto after=PiezoNativeBackendBridge::create(persisted);
    QCOMPARE(after.callbacks.sourceFingerprint,before.callbacks.sourceFingerprint);QCOMPARE(after.callbacks.backendIdentity,before.callbacks.backendIdentity);
    QCOMPARE(after.callbacks.controllerIdentity,before.callbacks.controllerIdentity);QCOMPARE(after.callbacks.forcingIdentity,before.callbacks.forcingIdentity);
    QCOMPARE(after.callbacks.initialOpaqueControllerState,before.callbacks.initialOpaqueControllerState);QCOMPARE(after.backendInitializationIdentity,before.backendInitializationIdentity);
    // Compare actual factory exports, including every numerical matrix/readout
    // catalog/private runtime witness, rather than expected constant hashes.
    QCOMPARE(after.exportPackage,before.exportPackage);RStructuralPiezoLayeredNativeRun restored(persisted,after.callbacks);
    QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(restored.acceptedRuntime()),baselineBytes);
    const auto array=[](const std::vector<double> &values){QJsonArray a;for(double value:values)a.append(value);return a;};
    const auto timeBits=[](double time){quint64 bits=0;std::memcpy(&bits,&time,sizeof(bits));QByteArray b(8,'\0');for(uint i=0;i<8;++i)b[i]=char((bits>>(8*i))&255);return QString::fromLatin1(b.toHex());};
    for(uint sequence=1;sequence<=nativeTarget;++sequence){auto prepared=run.prepare(source);run.validatePrepared(*prepared);run.commit(*prepared);
        auto resumed=restored.prepare(persisted);restored.validatePrepared(*resumed);restored.commit(*resumed);
        const auto codec=RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime());QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(restored.acceptedRuntime()),codec);
        const auto &state=run.acceptedRuntime();const QJsonObject record{{"recordId",QString::fromLatin1(QCryptographicHash::hash(codec,QCryptographicHash::Sha256).toHex())},
            {"opaqueState",QJsonDocument::fromJson(state.opaqueControllerState).object()},{"feStep",double(state.completedFE)},{"controlTick",double(state.controlTick)},
            {"phase","ready"},{"appliedVoltages",array(state.lastApplied)},{"heldVoltages",array(state.heldRequested)}};
        const auto certified=before.certificationBatchRequest({{"op","certificationBatch"},{"records",QJsonArray{record}},
            {"times",QJsonArray{QJsonObject{{"feStep",double(state.completedFE)},{"timeBits",timeBits(state.physical.integrator.state.time)}}}}});
        QCOMPARE(certified["configurationIdentity"].toString(),before.callbacks.controllerIdentity);
        const auto certificates=certified["records"].toArray();QCOMPARE(certificates.size(),1);
        QVERIFY(certificates.first().toObject()["valid"].toBool());QCOMPARE(state.completedFE,sequence);
    }
    const auto finalBytes=RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime());QCOMPARE(run.completedSteps(),nativeTarget);QVERIFY(run.completedSteps()>legacyInputCount);
    QString rejection;try{before.certificationBatchRequest({{"op","certificationBatch"},{"records",QJsonArray{}},
        {"times",QJsonArray{QJsonObject{{"feStep",double(nativeTarget+1)},{"timeBits",timeBits(run.acceptedRuntime().initialTime+double(nativeTarget+1)*run.acceptedRuntime().dt)}}}}});}
    catch(const RError &error){rejection=error.getMessage();}catch(const std::exception &error){rejection=QString::fromUtf8(error.what());}
    QVERIFY2(rejection.contains("exceeds frozen original targetSteps"),qPrintable(rejection));
    QCOMPARE(RStructuralPiezoLayeredRestartCodec::encode(run.acceptedRuntime()),finalBytes);
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::nativeDestinationsPreserved() try{
    const auto m=scene();QTemporaryDir dir;QVERIFY(dir.isValid());QString error;
    const auto sourcePath=dir.filePath("originalsource.rbm");m.write(sourcePath,false);const auto originalSource=read(sourcePath);
    QVERIFY(!RPiezoArtifactService::writeMatrixPackage(m,sourcePath,&error));QVERIFY(!error.isEmpty());QCOMPARE(read(sourcePath),originalSource);
    const auto paths=::records(m,dir.path());
    for(const auto &extension:{QString("ram"),QString("rbm")}){
        const auto other=dir.filePath("nonselectedothernative."+extension);m.write(other,false);const auto original=read(other);
        QVERIFY(!paths.contains(other));QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1]},other,&error));QVERIFY(!error.isEmpty());QCOMPARE(read(other),original);
    }
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::expectedSourcePreservesDestination() try{
    const auto model=scene();QTemporaryDir dir;QVERIFY(dir.isValid());const auto paths=::records(model,dir.path());
    const auto target=dir.filePath("expected-source.csv");QFile existing(target);QVERIFY(existing.open(QIODevice::WriteOnly));existing.write("sentinel");existing.close();
    QString error;QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries(paths,target,&error,nullptr,QString(64,'f')));
    QVERIFY(error.contains("selected experiment source"));QCOMPARE(read(target),QByteArray("sentinel"));
    const auto source=model.getProblemSetup().getStructuralPiezoLayeredSetup().computeSourceFingerprint(model);
    QVERIFY2(RPiezoArtifactService::writeAcceptedTimeSeries(paths,target,&error,nullptr,source),qPrintable(error));
    QCOMPARE(metadata(read(target))["sourceFingerprint"].toString(),source);
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::canonicalNativeDestinationsPreserved() try{
    const auto m=scene();QTemporaryDir dir;QVERIFY(dir.isValid());const auto paths=::records(m,dir.path());
    const auto source=dir.filePath("originalsource.rbm");m.write(source,false);const auto other=dir.filePath("nonselectedothernative.ram");m.write(other,false);
    const auto matrixAlias=dir.filePath("matrix.json"),csvAlias=dir.filePath("series.csv");std::error_code linkError;
    const auto nativePath=[](const QString &path){
#ifdef Q_OS_WIN
        return std::filesystem::path(path.toStdWString());
#else
        return std::filesystem::path(path.toStdString());
#endif
    };
    std::filesystem::create_symlink(nativePath(source),nativePath(matrixAlias),linkError);
    if(linkError)QSKIP(qPrintable(QString("Canonical native-target check unavailable: real filesystem symbolic link creation failed: %1").arg(QString::fromStdString(linkError.message()))));
    std::filesystem::create_symlink(nativePath(other),nativePath(csvAlias),linkError);
    if(linkError)QSKIP(qPrintable(QString("Canonical CSV-target check unavailable: real filesystem symbolic link creation failed: %1").arg(QString::fromStdString(linkError.message()))));
    QVERIFY(QFileInfo(matrixAlias).isSymLink());QVERIFY(QFileInfo(csvAlias).isSymLink());QCOMPARE(QFileInfo(matrixAlias).canonicalFilePath(),QFileInfo(source).canonicalFilePath());QCOMPARE(QFileInfo(csvAlias).canonicalFilePath(),QFileInfo(other).canonicalFilePath());
    const auto originalSource=read(source),originalOther=read(other);QString error;
    QVERIFY(!RPiezoArtifactService::writeMatrixPackage(m,matrixAlias,&error));QVERIFY(!error.isEmpty());QCOMPARE(read(source),originalSource);QVERIFY(QFileInfo(matrixAlias).isSymLink());QCOMPARE(read(matrixAlias),originalSource);
    QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1]},csvAlias,&error));QVERIFY(!error.isEmpty());QCOMPARE(read(other),originalOther);QVERIFY(QFileInfo(csvAlias).isSymLink());QCOMPARE(read(csvAlias),originalOther);
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::axialArtifacts() try {
    auto recipe=RPiezoExperimentService::exampleRecipe(RModel{},"axialRod");recipe["replaceLegacyConditions"]=true;recipe["steps"]=8;
    auto preliminary=RPiezoExperimentService::preview(RModel{},recipe);QVERIFY2(preliminary.valid,qPrintable(preliminary.message));
    const uint node=preliminary.candidate.getNodePhysicalId(preliminary.candidate.getNNodes()-1);
    recipe["excitation"]=QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId",QString("node:%1:X").arg(node)},{"unit","N"},
        {"terms",QJsonArray{QJsonObject{{"kind","sine"},{"amplitude",1.},{"frequencyHz",2.},{"phaseRad",.2}}}}}}}};
    const auto compiled=RPiezoExperimentService::preview(RModel{},recipe);QVERIFY2(compiled.valid,qPrintable(compiled.message));const auto &source=compiled.candidate;
    const auto backend=PiezoAxialBackendBridge::create(source);RStructuralPiezoAxialNativeRun run(source,backend.callbacks);QTemporaryDir directory;QVERIFY(directory.isValid());QStringList paths;
    for(uint step=0;step<4;++step){if(step){auto prepared=run.prepare(source);run.validatePrepared(*prepared);run.commit(*prepared);}
        const auto path=directory.filePath(QString("axial%1.%2").arg(step).arg(step%2?"ram":"rbm"));auto accepted=run.acceptedModel(source);accepted.write(path,false);paths.append(path);}
    QString error;RPiezoArtifactService::Report report;const auto csv=directory.filePath("axial.csv");
    const auto identity=source.getProblemSetup().getStructuralPiezoAxialSetup().computeSourceFingerprint(source);
    QVERIFY2(RPiezoArtifactService::writeAcceptedTimeSeries(paths,csv,&error,&report,identity),qPrintable(error));QCOMPARE(report.recordCount,4u);QVERIFY(report.continuousFE);
    const auto bytes=read(csv);QCOMPARE(report.modelFamily,QString("axialRod"));QCOMPARE(metadata(bytes)["family"].toString(),QString("axialRod"));QCOMPARE(metadata(bytes)["modelFamily"].toString(),report.modelFamily);
    QCOMPARE(column(bytes,QString("node:%1:X:q.absolute [m]").arg(node).toUtf8(),3).toDouble(),run.acceptedState().displacements[3*run.adapter().maps().nodeByPhysicalId.at(node)]);
    for(const auto &p:run.acceptedRuntime().readouts)QCOMPARE(column(bytes,("readout:"+p.portId+":absolute ["+p.unit+"]").toUtf8(),3).toDouble(),p.value);
    const auto matrixPath=directory.filePath("axial-matrices.json");QVERIFY2(RPiezoArtifactService::writeMatrixPackage(source,matrixPath,&error,&report),qPrintable(error));QCOMPARE(report.modelFamily,QString("axialRod"));
    const auto matrices=QJsonDocument::fromJson(read(matrixPath)).object();for(const auto *key:{"M","K","C","B","G","Ce","fullM","fullK","fullC","T","q0","V0","Q0","f0"})QCOMPARE(matrices[key],backend.exportPackage[key]);
    const auto previous=bytes;QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries(paths,csv,&error,nullptr,QString(64,'f')));QCOMPARE(read(csv),previous);
    RModel altered;altered.read(paths[2]);auto state=RStructuralPiezoAxialRestartCodec::decode(altered.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState);
    state.physical.displacements[3]+=1.;altered.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState=RStructuralPiezoAxialRestartCodec::encode(state);const auto changed=directory.filePath("axial-tampered.rbm");altered.write(changed,false);
    QVERIFY(!RPiezoArtifactService::writeAcceptedTimeSeries({paths[1],changed},csv,&error));QCOMPARE(read(csv),previous);
} REPORT_EXCEPTIONS
void TestPiezoArtifactService::actualMatrixPackageFEAndRom_data(){
    QTest::addColumn<QString>("family");
    for(const auto *family:{"axialRod","planarBeam","flatPlate"})QTest::newRow(family)<<QString(family);
}
void TestPiezoArtifactService::actualMatrixPackageFEAndRom() try{
    QFETCH(QString,family);RModel source;QJsonObject bridgePackage;QString expectedFamily;QByteArray configurationIdentity;
    if(family=="axialRod"){
        auto recipe=RPiezoExperimentService::exampleRecipe(RModel{},family);recipe["replaceLegacyConditions"]=true;recipe["steps"]=8;
        const auto preliminary=RPiezoExperimentService::preview(RModel{},recipe);QVERIFY2(preliminary.valid,qPrintable(preliminary.message));
        const uint tip=preliminary.candidate.getNodePhysicalId(preliminary.candidate.getNNodes()-1);
        auto design=recipe.value("design").toObject();design["modeIndices"]=QJsonArray{0};recipe["design"]=design;
        recipe["excitation"]=QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId",QString("node:%1:X").arg(tip)},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",.75}}}}}}}};
        const auto compiled=RPiezoExperimentService::preview(RModel{},recipe);QVERIFY2(compiled.valid,qPrintable(compiled.message));source=compiled.candidate;
        const auto &setup=source.getProblemSetup().getStructuralPiezoAxialSetup();setup.validate(source);configurationIdentity=setup.identityBytes();expectedFamily="axialRod";
        bridgePackage=PiezoAxialBackendBridge::create(source).exportPackage;
    }else{
        source=scene(family=="flatPlate");auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
        setup.controllerConfiguration=QJsonDocument(QJsonObject{{"kind","none"}}).toJson(QJsonDocument::Compact);
        setup.designConfiguration=QJsonDocument(QJsonObject{{"modeIndices",QJsonArray{0}},{"outputs",QJsonArray{QJsonObject{{"id","tip_acceleration"},{"physicalNodeId",123},{"quantity","acceleration"},{"dof","Z"},{"unit","m/s^2"}}}}}).toJson(QJsonDocument::Compact);
        setup.excitationConfiguration=QJsonDocument(QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId","node:123:Z"},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",.75}}}}}}}}).toJson(QJsonDocument::Compact);
        setup.bindToModel(source);setup.validate(source);configurationIdentity=setup.identityBytes();expectedFamily=family;
        bridgePackage=PiezoNativeBackendBridge::create(source).exportPackage;
    }
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("actual-fe-rom.json");QString error;RPiezoArtifactService::Report report;
    QVERIFY2(RPiezoArtifactService::writeMatrixPackage(source,path,&error,&report),qPrintable(error));const auto package=QJsonDocument::fromJson(read(path)).object();
    QVERIFY(!package.isEmpty());QCOMPARE(report.modelFamily,expectedFamily);QCOMPARE(package.value("artifactQualification").toString(),QString("actual-source-bound-native-FE-operators"));
    const QString sourceFingerprint=family=="axialRod"?source.getProblemSetup().getStructuralPiezoAxialSetup().computeSourceFingerprint(source):source.getProblemSetup().getStructuralPiezoLayeredSetup().computeSourceFingerprint(source);
    QCOMPARE(report.sourceFingerprint,sourceFingerprint);QCOMPARE(package.value("sourceFingerprint").toString(),sourceFingerprint);QCOMPARE(bridgePackage.value("sourceFingerprint").toString(),sourceFingerprint);
    QCOMPARE(package.value("sourceConfigurationSha256").toString(),QString::fromLatin1(QCryptographicHash::hash(configurationIdentity,QCryptographicHash::Sha256).toHex()));
    for(auto it=bridgePackage.constBegin();it!=bridgePackage.constEnd();++it)QCOMPARE(package.value(it.key()),it.value());
    const auto configuration=package.value("configuration").toObject();QCOMPARE(configuration.value("design"),bridgePackage.value("configuration").toObject().value("design"));
    QCOMPARE(configuration.value("excitation"),bridgePackage.value("configuration").toObject().value("excitation"));
    QCOMPARE(configuration.value("controller"),bridgePackage.value("configuration").toObject().value("controller"));
    QCOMPARE(package.value("dofs"),bridgePackage.value("dofs"));QCOMPARE(package.value("inputs"),bridgePackage.value("inputs"));QCOMPARE(package.value("outputs"),bridgePackage.value("outputs"));
    QCOMPARE(package.value("disturbanceInputs"),bridgePackage.value("disturbanceInputs"));QCOMPARE(package.value("Bforce"),bridgePackage.value("Bforce"));QCOMPARE(package.value("Dforce"),bridgePackage.value("Dforce"));
    const auto pairsWithUnit=[](const QJsonValue &value,const QString &unit){if(!value.isArray())return false;const auto entries=value.toArray();if(entries.isEmpty())return false;for(const auto &entry:entries)if(!entry.isArray()||entry.toArray().size()!=2||!entry.toArray()[1].isString()||entry.toArray()[1].toString()!=unit)return false;return true;};
    QVERIFY(pairsWithUnit(package.value("inputs"),"V"));QVERIFY(pairsWithUnit(package.value("disturbanceInputs"),"N"));
    const auto outputPairs=package.value("outputs").toArray();bool chargeOutput=false,voltageOutput=false,accelerationOutput=false;
    for(const auto &entry:outputPairs){QVERIFY(entry.isArray());QCOMPARE(entry.toArray().size(),2);QVERIFY(entry.toArray()[0].isString());QVERIFY(entry.toArray()[1].isString());const auto id=entry.toArray()[0].toString(),unit=entry.toArray()[1].toString();chargeOutput=chargeOutput||unit=="C";voltageOutput=voltageOutput||unit=="V";accelerationOutput=accelerationOutput||id=="tip_acceleration"&&unit=="m/s^2";}
    QVERIFY(chargeOutput);QVERIFY(voltageOutput);QVERIFY(accelerationOutput);
    const auto units=package.value("units").toObject();QCOMPARE(units.value("system").toString(),QString("SI"));QCOMPARE(units.value("time").toString(),QString("s"));
    if(family=="axialRod"){
        const auto ops=RStructuralPiezoAxialOperators::compile(source);const size_t n=ops.full.dofs,r=ops.free.dofs,p=ops.full.ports,m=size_t(std::count_if(ops.inputs.begin(),ops.inputs.end(),[](const auto &x){return !x.portId.isEmpty();}));
        Dense actuatorB(r*m);size_t outputColumn=0;for(size_t columnIndex=0;columnIndex<ops.inputs.size();++columnIndex)if(!ops.inputs[columnIndex].portId.isEmpty()){for(size_t row=0;row<r;++row)actuatorB[row*m+outputColumn]=ops.free.B[row*ops.free.inputs+columnIndex];++outputColumn;}
        checkMatrix(package,"M",ops.free.M,r,r);checkMatrix(package,"K",ops.free.K,r,r);checkMatrix(package,"C",ops.free.C,r,r);checkMatrix(package,"B",actuatorB,r,m);
        checkMatrix(package,"fullM",ops.full.M,n,n);checkMatrix(package,"fullK",ops.full.K,n,n);checkMatrix(package,"fullC",ops.full.C,n,n);checkMatrix(package,"G",ops.full.G,n,p);checkMatrix(package,"Ce",ops.full.Ce,p,p);
        Dense transform(n*r,0);for(size_t j=0;j<r;++j)transform[ops.freeToFull[j]*r+j]=1.;checkMatrix(package,"T",transform,n,r);checkVector(package,"q0",ops.q0);checkVector(package,"V0",ops.V0);checkVector(package,"Q0",ops.Q0);checkVector(package,"f0",ops.f0);
        QCOMPARE(package.value("mappedCoordinateUnits").toArray().size(),int(n));QCOMPARE(package.value("mappedForceUnits").toArray().size(),int(n));
        for(const auto &unit:package.value("mappedCoordinateUnits").toArray())QCOMPARE(unit.toString(),QString("m"));for(const auto &unit:package.value("mappedForceUnits").toArray())QCOMPARE(unit.toString(),QString("N"));
    }else{
        const auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();const auto bridge=PiezoNativeBackendBridge::create(source);RStructuralPiezoLayeredModelAdapter adapter(source,setup,bridge.callbacks.authoritativeFullC);const auto &d=adapter.data();const auto &op=adapter.mesh().operators;const size_t n=d.fullDofs,r=d.freeDofs,p=op.portIds.size(),m=d.actuatorVoltages0.size();
        checkMatrix(package,"M",d.mass,r,r);checkMatrix(package,"K",d.stiffness,r,r);checkMatrix(package,"C",d.damping,r,r);checkMatrix(package,"B",d.input,r,m);
        checkMatrix(package,"fullM",op.mass,n,n);checkMatrix(package,"fullK",adapter.electrical().condensed().stiffness,n,n);checkMatrix(package,"fullKE",op.stiffness,n,n);checkMatrix(package,"fullC",d.fullDamping,n,n);checkMatrix(package,"G",op.coupling,n,p);checkMatrix(package,"Ce",op.capacitance,p,p);
        checkMatrix(package,"T",d.T,n,r);checkVector(package,"q0",d.q0);checkVector(package,"p",d.p);checkVector(package,"f0",d.referenceForce);
        const auto mappedCoordinates=package.value("mappedCoordinateUnits").toArray(),mappedForces=package.value("mappedForceUnits").toArray();QCOMPARE(mappedCoordinates.size(),int(d.generalizedCoordinateUnits.size()));QCOMPARE(mappedForces.size(),int(d.generalizedForceUnits.size()));
        for(size_t i=0;i<d.generalizedCoordinateUnits.size();++i){QCOMPARE(mappedCoordinates[int(i)].toString(),d.generalizedCoordinateUnits[i]);QCOMPARE(mappedForces[int(i)].toString(),d.generalizedForceUnits[i]);}
    }
    const auto outputPairs2=package.value("outputs").toArray();int accelerationRow=-1;for(int row=0;row<outputPairs2.size();++row)if(outputPairs2[row].toArray()[0].toString()=="tip_acceleration")accelerationRow=row;
    QVERIFY(accelerationRow>=0);const auto forceDirect=jsonMatrix(package.value("Dforce"),size_t(outputPairs2.size()),size_t(package.value("disturbanceInputs").toArray().size()));
    QVERIFY(std::abs(forceDirect[size_t(accelerationRow)*size_t(package.value("disturbanceInputs").toArray().size())])>0);
    const auto actuatorDirect=jsonMatrix(package.value("D"),size_t(outputPairs2.size()),size_t(package.value("inputs").toArray().size()));bool nonzeroElectricalDirect=false;
    for(int row=0;row<outputPairs2.size();++row)if(outputPairs2[row].toArray()[1].toString()=="C")for(size_t columnIndex=0;columnIndex<size_t(package.value("inputs").toArray().size());++columnIndex)nonzeroElectricalDirect=nonzeroElectricalDirect||std::abs(actuatorDirect[size_t(row)*size_t(package.value("inputs").toArray().size())+columnIndex])>0;
    QVERIFY(nonzeroElectricalDirect);
    const double samplingPeriod=package.value("design").toObject().value("samplingPeriod").toDouble();QVERIFY(samplingPeriod>0);
    if(family=="axialRod"){const auto dynamics=source.getProblemSetup().getStructuralDynamicsSetup();QVERIFY(close(samplingPeriod,dynamics.getTimeStep()*double(package.value("controlEveryFESteps").toInt(1))));}
    else{const auto dynamics=source.getProblemSetup().getStructuralDynamicsSetup();QVERIFY(close(samplingPeriod,dynamics.getTimeStep()*double(package.value("controlEveryFESteps").toInt(1))));}
    verifySingleModeRom(package,size_t(package.value("M").toArray().size()),size_t(package.value("inputs").toArray().size()),size_t(package.value("outputs").toArray().size()),samplingPeriod);
} REPORT_EXCEPTIONS
QTEST_GUILESS_MAIN(TestPiezoArtifactService)
#include "tst_piezo_artifact_service.moc"
