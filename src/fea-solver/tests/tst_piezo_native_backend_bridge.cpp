#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <rml_model.h>
#include <rbl_error.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rparallel_execution_context.h>
#include <rpiezo_experiment_service.h>
#include <omp.h>
#include "piezo_native_backend_bridge.h"
#include "piezo_certification_batch_cache.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

#ifndef RANGE_PIEZO_BACKEND_PYTHON
#error "Bridge tests require the explicitly selected private Python executable"
#endif
#ifndef RANGE_PIEZO_BACKEND_MODULE_ROOT
#error "Bridge tests require the actual range_workflow Python module root"
#endif
namespace {
using Run=RStructuralPiezoLayeredNativeRun;
using Codec=RStructuralPiezoLayeredRestartCodec;
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


QByteArray json(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject parse(const QByteArray &b){return QJsonDocument::fromJson(b).object();}
// A single QJsonArray inside braces selects QJsonArray's copy constructor.
QJsonArray rowMatrix(const QJsonArray &row){QJsonArray matrix;matrix.append(row);return matrix;}
QByteArray realBits(const QJsonArray &values){QByteArray bytes;for(const auto &entry:values){const double value=entry.toDouble();bytes.append(reinterpret_cast<const char *>(&value),sizeof(value));}return bytes;}
class DiagnosticTimer{
    const char *name;QElapsedTimer timer;
public:
    explicit DiagnosticTimer(const char *label):name(label){timer.start();qInfo()<<name<<"started";}
    ~DiagnosticTimer(){qInfo()<<name<<"elapsedMs"<<timer.elapsed();}
};
PiezoNativeBackendBridge::Result bridge(const RModel &m){return PiezoNativeBackendBridge::create(m,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));}
RModel experiment(bool plate=false,const QString &kind="PI",uint N=3){
    auto m=scene(plate);auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();
    QJsonObject controller{{"kind",kind},{"controlEveryFESteps",int(N)},{"voltageLower",QJsonArray{-.5}},{"voltageUpper",QJsonArray{.5}},{"deltaVoltageLimits",QJsonArray{.06}}};
    if(kind=="PI"){controller["kp"]=.4;controller["ki"]=.02;controller["selectedOutputs"]=QJsonArray{"tip:q"};controller["bias"]=QJsonArray{.3};}
    if(kind=="LQR" || kind=="MPC"){controller["Q"]=QJsonArray{QJsonArray{2.,0.},QJsonArray{0.,1.}};controller["R"]=rowMatrix(QJsonArray{.5});}
    if(kind=="MPC"){controller["Qf"]=controller["Q"];controller["horizon"]=3;}
    s.controllerConfiguration=json(controller);
    s.designConfiguration=json({{"modeIndices",QJsonArray{0}},{"outputs",QJsonArray{
        QJsonObject{{"id","tip:q"},{"physicalNodeId",123},{"dof","Z"},{"quantity","displacement"}},
        QJsonObject{{"id","tip:v"},{"physicalNodeId",123},{"dof","Z"},{"quantity","velocity"}},
        QJsonObject{{"id","tip:a"},{"physicalNodeId",123},{"dof","Z"},{"quantity","acceleration"}}}}});
    s.excitationConfiguration=json({{"channels",QJsonArray{QJsonObject{{"channelId","node:123:Z"},{"unit","N"},
        {"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","0.01*t"}}}}}}}});
    s.bindToModel(m);return m;
}
RModel eccentricLayeredBeam(){
    RModel m;m.addNode(RNode(0,0,0));m.addNode(RNode(1,0,0));
    m.setNodePhysicalId(0,101);m.setNodePhysicalId(1,123);
    RElement beam(R_ELEMENT_BEAM1);beam.setNodeId(0,0);beam.setNodeId(1,1);m.addElement(beam,true,0);m.setElementPhysicalId(0,501);
    auto &line=m.getLine(0);line.removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);line.removeEnvironmentCondition(R_ENVIRONMENT_CONDITION_TEMPERATURE);
    m.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));m.getProblemSetup().setMassMatrixType(RMassMatrixType::Consistent);
    auto &dynamics=m.getProblemSetup().getStructuralDynamicsSetup();dynamics.setEnabled(true);dynamics.setTimeStep(.01);dynamics.setNSteps(2);m.getTimeSolver().setInputStartTime(.01);
    auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();setup.enabled=true;
    const auto native=RStructuralPiezoContinuumMaterial::isotropic(30,.2,7);
    RStructuralPiezoLayeredMaterial host;host.materialId="host";host.elasticityOrCompliance=native.elasticity;host.density=7;host.sourceId="independent-eccentric-beam-example";host.sourceSha256=QString(64,'b');
    auto active=host;active.materialId="piezo";active.piezoelectric=true;active.coupling[12]=2;active.coupling[13]=2;active.permittivity={{4,0,0,0,4,0,0,0,4}};setup.materials={host,active};
    RStructuralPiezoLayeredPort drive;drive.readout.portId="drive";drive.readout.role=RStructuralPiezoPortRole::ActuatorSensor;drive.readout.readout=RStructuralPiezoReadoutMode::ActuatorCharge;drive.initialValue=.12;
    RStructuralPiezoLayeredPort sense;sense.readout.portId="sense";sense.readout.role=RStructuralPiezoPortRole::Sensor;sense.readout.readout=RStructuralPiezoReadoutMode::OpenCircuitVoltage;sense.initialValue=.03;setup.ports={drive,sense};
    RStructuralPiezoLayeredModelLayer layer;layer.materialId="host";layer.lowerZ=.1;layer.upperZ=.3;
    auto driven=layer;driven.materialId="piezo";driven.lowerZ=.3;driven.upperZ=.4;driven.portId="drive";
    auto sensed=driven;sensed.lowerZ=.4;sensed.upperZ=.45;sensed.portId="sense";setup.members={{501,{layer,driven,sensed}}};
    using D=RStructuralPiezoPhysicalDof;setup.supports={{101,D::UX,0},{101,D::UZ,0},{101,D::RY,0}};
    setup.excitationConfiguration=json({{"channels",QJsonArray{QJsonObject{{"channelId","node:123:X"},{"unit","N"},
        {"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",.4}}}}}}}});
    setup.controllerConfiguration=json({{"kind","PI"},{"controlEveryFESteps",1},{"voltageLower",QJsonArray{-.5}},
        {"voltageUpper",QJsonArray{.5}},{"deltaVoltageLimits",QJsonArray{.06}},{"kp",.4},{"ki",.02},
        {"selectedOutputs",QJsonArray{"tip:q"}},{"bias",QJsonArray{.3}}});
    setup.designConfiguration=json({{"modeIndices",QJsonArray{0}},{"outputs",QJsonArray{
        QJsonObject{{"id","tip:q"},{"physicalNodeId",123},{"dof","X"},{"quantity","displacement"}},
        QJsonObject{{"id","tip:v"},{"physicalNodeId",123},{"dof","X"},{"quantity","velocity"}},
        QJsonObject{{"id","tip:a"},{"physicalNodeId",123},{"dof","X"},{"quantity","acceleration"}}}}});
    setup.damping.kind=RStructuralPiezoDampingKind::Rayleigh;setup.damping.rayleighAlpha=.06;setup.damping.rayleighBeta=.005;
    setup.bindToModel(m);return m;
}
bool rejects(const std::function<void()> &f){try{f();}catch(...){return true;}return false;}
RModel step(Run &run,const RModel &source){auto p=run.prepare(source);run.validatePrepared(*p);RModel model(p->candidateModel());run.commit(*p);return model;}
double entry(const QJsonValue &matrix,size_t i,size_t j){
    if(!matrix.isArray())throw std::runtime_error("Expected a JSON matrix array");
    const auto rows=matrix.toArray();if(i>=size_t(rows.size()))throw std::out_of_range("JSON matrix row is outside its array");
    const auto rowValue=rows.at(int(i));if(!rowValue.isArray())throw std::runtime_error("Expected a JSON matrix row array");
    const auto row=rowValue.toArray();if(j>=size_t(row.size()))throw std::out_of_range("JSON matrix column is outside its row");
    const auto value=row.at(int(j));if(!value.isDouble())throw std::runtime_error("Expected a numeric JSON matrix entry");return value.toDouble();
}
void close(double a,double b){if(!std::isfinite(a)||!std::isfinite(b)||std::abs(a-b)>1e-10*std::max({1.,std::abs(a),std::abs(b)}))throw std::runtime_error("Analytic native bridge operator mismatch");}
RModel arbitraryRotatedPlate(double angle)
{
    RModel empty;empty.getTimeSolver().setInputStartTime(.001);auto recipe=RPiezoExperimentService::exampleRecipe(empty,"flatPlate");
    auto source=RPiezoExperimentService::templateCandidate(empty,recipe["template"].toObject());recipe.remove("template");recipe["replaceLegacyConditions"]=true;recipe["steps"]=3;
    const uint tip=uint(recipe["design"].toObject()["outputs"].toArray()[0].toObject()["physicalNodeId"].toDouble());
    recipe["design"]=QJsonObject{{"outputs",QJsonArray{QJsonObject{{"id","globalAcceleration"},{"physicalNodeId",double(tip)},{"dof","Z"},{"quantity","acceleration"},{"unit","m/s^2"}}}}};
    QJsonArray channels;for(const auto &axis:{"local:EZ","local:RY"})channels.append(QJsonObject{{"channelId",QString("node:%1:%2").arg(tip).arg(axis)},{"unit",QString(axis).endsWith("RY")?"N m":"N"},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",.03}}}}});recipe["excitation"]=QJsonObject{{"channels",channels}};
    std::set<uint> fixed;for(const auto &entry:recipe["supports"].toArray())fixed.insert(uint(entry.toObject()["physicalNodeId"].toDouble()));QJsonArray supports;
    for(uint node:fixed)for(const auto &axis:{"UX","UY","UZ","RX","RY","RZ"})supports.append(QJsonObject{{"physicalNodeId",double(node)},{"dof",axis},{"value",0.}});recipe["supports"]=supports;
    const double c=std::cos(angle),s=std::sin(angle),length=std::sqrt(14.),u[]={1/length,2/length,3/length};double rotation[3][3];
    for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)rotation[i][j]=(i==j?c:0)+(1-c)*u[i]*u[j];rotation[0][1]-=s*u[2];rotation[0][2]+=s*u[1];rotation[1][0]+=s*u[2];rotation[1][2]-=s*u[0];rotation[2][0]-=s*u[1];rotation[2][1]+=s*u[0];
    const double origin[]={.3,-.2,.4};for(uint i=0;i<source.getNNodes();++i){auto &node=source.getNode(i);const double old[]={node.getX(),node.getY(),node.getZ()};double point[3];for(size_t j=0;j<3;++j){point[j]=origin[j];for(size_t k=0;k<3;++k)point[j]+=rotation[j][k]*old[k];}node.setX(point[0]);node.setY(point[1]);node.setZ(point[2]);}
    QJsonObject frame{{"origin",QJsonArray{origin[0],origin[1],origin[2]}}};const char *axes[]={"ex","ey","ez"};for(size_t j=0;j<3;++j)frame[axes[j]]=QJsonArray{rotation[0][j],rotation[1][j],rotation[2][j]};recipe["frame"]=frame;
    const auto candidate=RPiezoExperimentService::preview(source,recipe);if(!candidate.valid)throw std::invalid_argument(qPrintable(candidate.message));return candidate.candidate;
}
// Independent scaled partial-pivot elimination (not the bridge's Cholesky).
std::vector<double> independentMassSolve(const std::vector<double> &mass,const std::vector<double> &rhs)
{
    const size_t n=rhs.size();std::vector<double> a(n*n),b(n),scale(n);for(size_t i=0;i<n;++i)scale[i]=std::sqrt(mass[i*n+i]);
    for(size_t i=0;i<n;++i){b[i]=rhs[i]/scale[i];for(size_t j=0;j<n;++j)a[i*n+j]=mass[i*n+j]/scale[i]/scale[j];}
    for(size_t k=0;k<n;++k){size_t pivot=k;for(size_t i=k+1;i<n;++i)if(std::abs(a[i*n+k])>std::abs(a[pivot*n+k]))pivot=i;
        if(!std::isfinite(a[pivot*n+k])||a[pivot*n+k]==0)throw std::runtime_error("Independent original-M solve failed");
        if(pivot!=k){for(size_t j=0;j<n;++j)std::swap(a[pivot*n+j],a[k*n+j]);std::swap(b[pivot],b[k]);}
        for(size_t i=k+1;i<n;++i){const double multiplier=a[i*n+k]/a[k*n+k];for(size_t j=k+1;j<n;++j)a[i*n+j]-=multiplier*a[k*n+j];b[i]-=multiplier*b[k];}
    }
    std::vector<double> result(n);for(size_t i=n;i-->0;){double value=b[i];for(size_t j=i+1;j<n;++j)value-=a[i*n+j]*result[j];result[i]=value/a[i*n+i];}for(size_t i=0;i<n;++i)result[i]/=scale[i];return result;
}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoNativeBackendBridge:public QObject{
    Q_OBJECT
private slots:
    void actualOperatorsAndPureProposal_data();void actualOperatorsAndPureProposal();
    void everyPhaseRestartAndAppliedValidation();
    void modalSingleAuthority();
    void allElectricalReadouts_data();void allElectricalReadouts();
    void sensorOnlyCannotReadHiddenFEState();
    void rotatedScalarReadoutAndForceAdmission();
    void rotatedPlateAccelerationUsesOriginalMass_data();void rotatedPlateAccelerationUsesOriginalMass();
    void invalidAccelerationSourceIsStillRejected();
    void realControllerKinds_data();void realControllerKinds();
    void forceAwareObserverAndMPCRestart_data();void forceAwareObserverAndMPCRestart();
    void jointFailureKeepsControllerCandidateRetryable_data();void jointFailureKeepsControllerCandidateRetryable();
    void configuredForceColumnsAndAffineBias_data();void configuredForceColumnsAndAffineBias();
    void defaultRuntimeFactory();
    void deterministicCompilationRestoresHost_data();void deterministicCompilationRestoresHost();
    void unsupportedSourceConfigurations_data();void unsupportedSourceConfigurations();
    void frozenBackendModuleStaleness();
    void explicitExecutablePaths_data();void explicitExecutablePaths();
    void externalSampledTransaction_data();void externalSampledTransaction();
    void eccentricBeamBridgePreservesCoupledMassAndNewmark();
    void eccentricBeamExplicitWorkpointTangentPortsAndAcceptedReadout();
};
void TestPiezoNativeBackendBridge::actualOperatorsAndPureProposal_data(){QTest::addColumn<bool>("plate");QTest::newRow("beam")<<false;QTest::newRow("plate")<<true;}
void TestPiezoNativeBackendBridge::actualOperatorsAndPureProposal() try{
    QFETCH(bool,plate);const auto m=experiment(plate);const auto result=bridge(m);RStructuralPiezoLayeredModelAdapter adapter(m,m.getProblemSetup().getStructuralPiezoLayeredSetup());
    const auto &d=adapter.data();const auto &op=adapter.mesh().operators;const auto exported=result.exportPackage;
    QCOMPARE(exported.value("sourceFingerprint").toString(),d.sourceFingerprint);QCOMPARE(exported.value("nodes").toArray().size(),int(m.getNNodes()));
    QCOMPARE(result.callbacks.excitationConfiguration,m.getProblemSetup().getStructuralPiezoLayeredSetup().excitationConfiguration);
    QCOMPARE(result.callbacks.controllerConfiguration,m.getProblemSetup().getStructuralPiezoLayeredSetup().controllerConfiguration);
    QCOMPARE(result.callbacks.designConfiguration,m.getProblemSetup().getStructuralPiezoLayeredSetup().designConfiguration);
    QCOMPARE(exported.value("members").toArray().size(),int(m.getNElements()));QCOMPARE(exported.value("outputs").toArray().size(),5);
    for(size_t i=0;i<d.freeDofs;++i)for(size_t j=0;j<d.freeDofs;++j){QCOMPARE(entry(exported["M"],i,j),d.mass[i*d.freeDofs+j]);QCOMPARE(entry(exported["K"],i,j),d.stiffness[i*d.freeDofs+j]);QCOMPARE(entry(exported["C"],i,j),d.damping[i*d.freeDofs+j]);}
    const size_t drive=size_t(std::find(op.portIds.begin(),op.portIds.end(),QString("drive"))-op.portIds.begin());
    const size_t sense=size_t(std::find(op.portIds.begin(),op.portIds.end(),QString("sense"))-op.portIds.begin());
    for(size_t j=0;j<d.freeDofs;++j){double gd=0,gs=0;for(size_t i=0;i<d.fullDofs;++i){gd+=op.coupling[i*2+drive]*d.T[i*d.freeDofs+j];gs+=op.coupling[i*2+sense]*d.T[i*d.freeDofs+j];}close(entry(exported["Lq"],0,j),-gd);close(entry(exported["Lq"],1,j),gs/op.capacitance[sense*2+sense]);}
    close(entry(exported["D"],0,0),op.capacitance[drive*2+drive]);QCOMPARE(entry(exported["D"],1,0),0.);
    const auto forces=result.callbacks.perturbationForce(.2);QCOMPARE(forces.size(),size_t(6*m.getNNodes()));close(forces[14],.002);
    Run run(m,result.callbacks);const auto physical=run.acceptedState();const auto opaque=result.callbacks.initialOpaqueControllerState;
    const auto a=result.callbacks.propose(physical,0,0,opaque),b=result.callbacks.propose(physical,0,0,opaque);
    QCOMPARE(a.requestedVoltages,b.requestedVoltages);QCOMPARE(a.nextOpaqueState,b.nextOpaqueState);QCOMPARE(opaque,result.callbacks.initialOpaqueControllerState);
    // Initial q is a true increment about the separately exported native q0.
    const auto stateObject=parse(a.nextOpaqueState);const auto previousErrorValue=stateObject.value("previousError");QVERIFY(previousErrorValue.isArray());
    const auto previousError=previousErrorValue.toArray();QCOMPARE(previousError.size(),1);QVERIFY(previousError[0].isDouble());QCOMPARE(previousError.at(0).toDouble(),0.);
    QCOMPARE(result.callbacks.envelopes.size(),size_t(1));close(result.callbacks.envelopes[0].maximumRate,.06/(3*.01));
    const auto prepared=run.prepare(m);close(prepared->runtime().lastApplied[0],.14);QCOMPARE(prepared->runtime().controlTick,1u);
    const auto directFeedthroughValue=exported.value("physicalForceDirectFeedthrough");QVERIFY(directFeedthroughValue.isArray());const auto outputs=directFeedthroughValue.toArray();QCOMPARE(outputs.size(),5);QVERIFY(std::abs(entry(directFeedthroughValue,4,14))>0);
    QCOMPARE(exported["controllerModelScope"].toString(),QString("complete-known-forcing"));
    const auto disturbanceInputsValue=exported.value("disturbanceInputs");QVERIFY(disturbanceInputsValue.isArray());QCOMPARE(disturbanceInputsValue.toArray(),rowMatrix(QJsonArray{"node:123:Z","N"}));
    close(exported["initialTime"].toDouble(),m.getTimeSolver().getInputStartTime()-.01);
    const auto biasForceValue=exported.value("biasForce");QVERIFY(biasForceValue.isArray());const auto biasForce=biasForceValue.toArray();QCOMPARE(biasForce.size(),int(d.freeDofs));
    const auto bForceValue=exported.value("Bforce"),reducedForceMapValue=exported.value("reducedPhysicalForceMap");
    for(size_t i=0;i<d.freeDofs;++i){close(entry(bForceValue,i,0),entry(reducedForceMapValue,i,14));QVERIFY(biasForce[int(i)].isDouble());close(biasForce.at(int(i)).toDouble(),d.backgroundOffset[i]);}
    const auto designValue=exported.value("design");QVERIFY(designValue.isObject());const auto design=designValue.toObject();const auto forceAwareValue=design.value("forceAware");QVERIFY(forceAwareValue.isObject());
    const auto forceAware=forceAwareValue.toObject();QVERIFY(forceAware.value("allConfiguredForcesMapped").isBool());QVERIFY(forceAware.value("allConfiguredForcesMapped").toBool());
    const auto feValue=forceAware.value("FE"),romValue=forceAware.value("ROM");QVERIFY(feValue.isObject());QVERIFY(romValue.isObject());
    const auto fe=feValue.toObject(),rom=romValue.toObject();const auto feContinuousValue=fe.value("continuous"),romContinuousValue=rom.value("continuous");
    QVERIFY(feContinuousValue.isObject());QVERIFY(romContinuousValue.isObject());const auto feContinuous=feContinuousValue.toObject(),romContinuous=romContinuousValue.toObject();
    const auto feD=feContinuous.value("D"),romD=romContinuous.value("D");QVERIFY(feD.isArray());QVERIFY(romD.isArray());
    QCOMPARE(feD.toArray().size(),5);QCOMPARE(romD.toArray().size(),5);
    for(size_t i=0;i<5;++i){close(entry(exported["Dforce"],i,0),entry(exported["physicalForceDirectFeedthrough"],i,14));close(entry(feD,i,1),entry(exported["Dforce"],i,0));close(entry(romD,i,1),entry(exported["Dforce"],i,0));}
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::everyPhaseRestartAndAppliedValidation() try{
    const auto source=experiment();auto full=bridge(source);Run continuous(source,full.callbacks);while(continuous.hasRemainingSteps())step(continuous,source);const auto expected=Codec::encode(continuous.acceptedRuntime());QTemporaryDir dir;
    for(uint split=0;split<=8;++split){auto first=bridge(source);Run run(source,first.callbacks);for(uint i=0;i<split;++i)step(run,source);auto model=run.acceptedModel(source);
        const auto path=model.nativeWritePaths(dir.filePath(QString("split-%1.rbm").arg(split)),false).first();model.write(path,false);RModel disk;disk.read(path);disk.getProblemSetup().setRestart(true);
        auto fresh=bridge(disk);Run resumed(disk,fresh.callbacks);while(resumed.hasRemainingSteps())step(resumed,disk);QCOMPARE(Codec::encode(resumed.acceptedRuntime()),expected);
    }
    auto result=bridge(source);Run run(source,result.callbacks);auto accepted=run.acceptedRuntime();auto opaque=parse(accepted.opaqueControllerState);opaque["previousApplied"]=QJsonArray{.13};accepted.opaqueControllerState=json(opaque);
    QVERIFY(rejects([&]{result.callbacks.validateAcceptedBoundary(accepted);}));
    accepted=run.acceptedRuntime();opaque=parse(accepted.opaqueControllerState);opaque["configurationIdentity"]=QString(64,'a');accepted.opaqueControllerState=json(opaque);QVERIFY(rejects([&]{result.callbacks.validateAcceptedBoundary(accepted);}));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::allElectricalReadouts_data(){QTest::addColumn<int>("mode");for(auto mode:{RStructuralPiezoReadoutMode::OpenCircuitVoltage,RStructuralPiezoReadoutMode::ShortCircuitCharge,RStructuralPiezoReadoutMode::IdealChargeAmplifier})QTest::newRow(qPrintable(QString::number(int(mode))))<<int(mode);}
void TestPiezoNativeBackendBridge::allElectricalReadouts() try{
    QFETCH(int,mode);auto m=experiment();auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();auto &sense=setup.ports[1];sense.readout.readout=RStructuralPiezoReadoutMode(mode);
    if(mode!=int(RStructuralPiezoReadoutMode::OpenCircuitVoltage))sense.initialValue=0;
    if(mode==int(RStructuralPiezoReadoutMode::IdealChargeAmplifier)){sense.readout.Cf=.5;sense.readout.amplifierPolarity=1;sense.readout.Qref=.03;sense.readout.VoutBias=.2;}setup.bindToModel(m);
    // Binding replaces the setup's port storage; reacquire metadata afterward.
    const auto &boundSetup=m.getProblemSetup().getStructuralPiezoLayeredSetup();const auto &readout=boundSetup.ports[1].readout;
    const auto result=bridge(m);RStructuralPiezoLayeredModelAdapter adapter(m,boundSetup);const auto &d=adapter.data();std::vector<double> q=d.q0,increment(d.freeDofs),voltage=d.actuatorVoltages0;
    for(size_t j=0;j<increment.size();++j){increment[j]=.001*double(j+1);for(size_t i=0;i<q.size();++i)q[i]+=d.T[i*d.freeDofs+j]*increment[j];}voltage[0]+=.01;
    const auto native=adapter.electrical().evaluate(q,voltage);const auto found=std::find_if(native.ports.begin(),native.ports.end(),[](const auto &port){return port.portId=="sense";});QVERIFY(found!=native.ports.end());const auto &port=*found;
    const auto outputs=result.exportPackage["outputs"].toArray();size_t outputRow=0;while(outputRow<size_t(outputs.size()) && outputs[int(outputRow)].toArray()[0].toString()!="sense")++outputRow;QVERIFY(outputRow<size_t(outputs.size()));
    const double expected=mode==int(RStructuralPiezoReadoutMode::OpenCircuitVoltage)?port.deltaVoltage:mode==int(RStructuralPiezoReadoutMode::ShortCircuitCharge)?port.deltaCharge:-double(readout.amplifierPolarity)*port.deltaAmplifierVoltage;
    double analytic=entry(result.exportPackage["D"],outputRow,0)*.01;for(size_t j=0;j<increment.size();++j)analytic+=entry(result.exportPackage["Lq"],outputRow,j)*increment[j];
    const QString diagnostic=QString("mode=%1 port=%2 outputRow=%3 actualOperator=%4 expectedNative=%5 deltaCharge=%6 deltaAmplifierVoltage=%7 Cf=%8 polarity=%9 Qref=%10 VoutBias=%11")
        .arg(mode).arg(port.portId).arg(qulonglong(outputRow)).arg(analytic,0,'g',17).arg(expected,0,'g',17).arg(port.deltaCharge,0,'g',17).arg(port.deltaAmplifierVoltage,0,'g',17)
        .arg(readout.Cf,0,'g',17).arg(readout.amplifierPolarity).arg(readout.Qref,0,'g',17).arg(readout.VoutBias,0,'g',17);
    if(mode==int(RStructuralPiezoReadoutMode::IdealChargeAmplifier)){
        const double fromCharge=double(readout.amplifierPolarity)*port.deltaCharge/readout.Cf;
        QVERIFY2(std::abs(fromCharge-expected)<=1e-10*std::max({1.,std::abs(fromCharge),std::abs(expected)}),qPrintable("Native amplifier increment disagrees with charge/Cf: "+diagnostic));
    }
    QVERIFY2(std::isfinite(analytic) && std::isfinite(expected) && std::abs(analytic-expected)<=1e-10*std::max({1.,std::abs(analytic),std::abs(expected)}),qPrintable(diagnostic));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::sensorOnlyCannotReadHiddenFEState() try{
    auto m=experiment(false,"LQR");auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();auto controller=parse(setup.controllerConfiguration);
    controller["stateSource"]="sensor_only";controller["selectedOutputs"]=QJsonArray{"sense"};controller["observer"]=QJsonObject{{"processCovariance",QJsonArray{QJsonArray{.1,0.},QJsonArray{0.,.1}}},{"measurementCovariance",rowMatrix(QJsonArray{.01})}};setup.controllerConfiguration=json(controller);setup.bindToModel(m);
    const auto result=bridge(m);Run run(m,result.callbacks);auto accepted=run.acceptedState();const auto opaque=result.callbacks.initialOpaqueControllerState;
    const auto actual=result.callbacks.propose(accepted,0,0,opaque);accepted.integrator.state.q[0]+=999.;accepted.integrator.state.v[0]-=888.;accepted.physicalDisplacements[14]+=777.;
    const auto hiddenChanged=result.callbacks.propose(accepted,0,0,opaque);QCOMPARE(actual.requestedVoltages,hiddenChanged.requestedVoltages);QCOMPARE(actual.nextOpaqueState,hiddenChanged.nextOpaqueState);
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::rotatedScalarReadoutAndForceAdmission() try{
    auto m=experiment();const double c=std::sqrt(.5);for(uint i=0;i<m.getNNodes();++i){const double x=m.getNode(i).getX();m.getNode(i).setX(c*x);m.getNode(i).setY(c*x);}
    auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();setup.frame.ex={{c,c,0}};setup.frame.ey={{-c,c,0}};
    for(auto *fields:{&setup.backgroundForces,&setup.initialVelocities})for(auto &field:*fields){const double x=field.values[0];field.values[0]=c*x;field.values[1]=c*x;}
    auto design=parse(setup.designConfiguration);auto outputs=design["outputs"].toArray();outputs.append(QJsonObject{{"id","tip:y"},{"physicalNodeId",123},{"dof","Y"},{"quantity","displacement"},{"unit","m"}});design["outputs"]=outputs;setup.designConfiguration=json(design);setup.bindToModel(m);
    const auto result=bridge(m);RStructuralPiezoLayeredModelAdapter adapter(m,setup);const auto &d=adapter.data();bool nonzero=false;
    for(size_t j=0;j<d.freeDofs;++j){double expected=0;for(size_t i=0;i<d.fullDofs;++i)expected+=adapter.mesh().physicalTransform[13*d.fullDofs+i]*d.T[i*d.freeDofs+j];close(entry(result.exportPackage["Lq"],5,j),expected);nonzero=nonzero||expected!=0;}QVERIFY(nonzero);
    for(const auto &entry:result.exportPackage["forceCatalog"].toArray())QVERIFY(entry.toObject()["channelId"].toString()!="node:123:X");
    setup.excitationConfiguration=json({{"channels",QJsonArray{QJsonObject{{"channelId","node:123:X"},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",1.}}}}}}}});setup.bindToModel(m);QVERIFY(rejects([&]{bridge(m);}));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::rotatedPlateAccelerationUsesOriginalMass_data(){QTest::addColumn<double>("angle");QTest::newRow("generic-three-axis-rotation")<<.71;QTest::newRow("near-aligned-coupled-rows")<<1e-7;}
void TestPiezoNativeBackendBridge::rotatedPlateAccelerationUsesOriginalMass() try{
    QFETCH(double,angle);const auto source=arbitraryRotatedPlate(angle);const auto actual=bridge(source);const auto &exported=actual.exportPackage;
    RStructuralPiezoLayeredModelAdapter adapter(source,source.getProblemSetup().getStructuralPiezoLayeredSetup());const auto &data=adapter.data();const size_t n=data.freeDofs;
    for(size_t i=0;i<n;++i)for(size_t j=0;j<n;++j)QCOMPARE(entry(exported["M"],i,j),data.mass[i*n+j]);
    bool tinyCoupling=false;for(size_t i=0;i<n;++i)for(size_t j=0;j<i;++j){const double term=std::abs(data.mass[i*n+j]),scale=std::sqrt(data.mass[i*n+i])*std::sqrt(data.mass[j*n+j]);if(term>0&&term<1e-12*scale)tinyCoupling=true;}QVERIFY(tinyCoupling);
    const size_t ny=size_t(exported["outputs"].toArray().size()),nf=size_t(exported["disturbanceInputs"].toArray().size());QCOMPARE(nf,size_t(2));
    for(size_t column=0;column<nf;++column){std::vector<double> force(n);for(size_t i=0;i<n;++i)force[i]=entry(exported["Bforce"],i,column);const auto acceleration=independentMassSolve(data.mass,force);
        for(size_t row=0;row<ny;++row){double expected=0;for(size_t i=0;i<n;++i)expected+=entry(exported["La"],row,i)*acceleration[i];const double got=entry(exported["Dforce"],row,column);QVERIFY(std::isfinite(got));
            QVERIFY2(std::abs(got-expected)<=1e-10*std::max({std::abs(got),std::abs(expected),1e-20}),qPrintable(QString("Original-M acceleration derivative row=%1 column=%2 actual=%3 independent=%4").arg(qulonglong(row)).arg(qulonglong(column)).arg(got,0,'g',17).arg(expected,0,'g',17)));
        }
    }
    // Independently perturb the actual physical force at fixed q/v/voltage.
    // This tests the output derivative, rather than comparing two exports.
    const auto force0=actual.callbacks.perturbationForce(0);auto force1=force0;const double h=1e-5;for(size_t i=0;i<force1.size();++i)force1[i]+=h*entry(exported["physicalForceDirections"],i,0);
    RStructuralPiezoLayeredDynamics dynamics0(adapter,RMatrixSolverConf(RMatrixSolverConf::GMRES),0,[force0](double){return force0;},"acceleration-fd0"),dynamics1(adapter,RMatrixSolverConf(RMatrixSolverConf::GMRES),0,[force1](double){return force1;},"acceleration-fd1");
    const auto state0=dynamics0.initialize(),state1=dynamics1.initialize();const auto output=exported["outputs"].toArray().last().toArray();QCOMPARE(output.first().toString(),QString("globalAcceleration"));
    size_t physical=0;for(const auto &node:exported["nodes"].toArray()){const auto metadata=node.toObject();if(metadata["physicalNodeId"].toDouble()==parse(source.getProblemSetup().getStructuralPiezoLayeredSetup().designConfiguration)["outputs"].toArray()[0].toObject()["physicalNodeId"].toDouble())physical=size_t(metadata["physicalOffset"].toDouble())+2;}
    const double derivative=(state1.physicalAccelerations[physical]-state0.physicalAccelerations[physical])/h,expected=entry(exported["Dforce"],ny-1,0);QVERIFY(std::isfinite(derivative));QVERIFY(std::abs(derivative-expected)<=2e-6*std::max({std::abs(derivative),std::abs(expected),1e-20}));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::invalidAccelerationSourceIsStillRejected() try{
    auto model=experiment(true);auto &setup=model.getProblemSetup().getStructuralPiezoLayeredSetup();
    QVERIFY(rejects([&]{setup.materials[0].density=std::numeric_limits<double>::quiet_NaN();setup.bindToModel(model);bridge(model);}));
    model=experiment(true);auto &unsupported=model.getProblemSetup().getStructuralPiezoLayeredSetup();auto design=parse(unsupported.designConfiguration);design["outputs"]=QJsonArray{QJsonObject{{"id","drillingAcceleration"},{"physicalNodeId",123},{"dof","RZ"},{"quantity","acceleration"}}};unsupported.designConfiguration=json(design);unsupported.bindToModel(model);QVERIFY(rejects([&]{bridge(model);}));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::modalSingleAuthority() try{
    auto m=experiment();auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();s.damping={};s.damping.kind=RStructuralPiezoDampingKind::UniformModal;s.damping.uniformRatio=.025;s.bindToModel(m);
    const auto result=bridge(m);QVERIFY(!result.callbacks.authoritativeFullC.empty());RStructuralPiezoLayeredModelAdapter adapter(m,s,result.callbacks.authoritativeFullC);
    QCOMPARE(result.exportPackage.value("dampingAuthority").toString(),adapter.data().dampingAuthority);for(size_t i=0;i<adapter.data().freeDofs;++i)for(size_t j=0;j<adapter.data().freeDofs;++j)QCOMPARE(entry(result.exportPackage["C"],i,j),adapter.data().damping[i*adapter.data().freeDofs+j]);
    Run run(m,result.callbacks);step(run,m);QCOMPARE(run.completedSteps(),1u);
    auto configuration=parse(s.designConfiguration);configuration["damping"]=QJsonObject{{"kind","uniform"},{"zeta",.05}};s.designConfiguration=json(configuration);s.bindToModel(m);QVERIFY(rejects([&]{bridge(m);}));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::realControllerKinds_data(){QTest::addColumn<QString>("kind");QTest::newRow("LQR")<<QString("LQR");QTest::newRow("MPC")<<QString("MPC");}
void TestPiezoNativeBackendBridge::realControllerKinds() try{
    QFETCH(QString,kind);const auto source=experiment(false,kind);auto result=bridge(source);Run run(source,result.callbacks);while(run.hasRemainingSteps())step(run,source);QCOMPARE(run.completedSteps(),8u);
    const auto state=parse(run.acceptedRuntime().opaqueControllerState);QCOMPARE(state["feStep"].toDouble(),8.);QCOMPARE(state["controlTick"].toDouble(),3.);
    QVERIFY(run.acceptedState().freeForceResidual<1e-7);const auto certificate=result.exportPackage["design"].toObject()["controller"].toObject();
    if(kind=="LQR"){QVERIFY(certificate["riccatiRelativeResidual"].toDouble()<1e-8);QVERIFY(certificate["K"].isArray());}
    else {QVERIFY(!state["warmStart"].toArray().isEmpty());const auto frame=parse(run.acceptedRuntime().lastFrameBytes);QVERIFY(frame["diagnostics"].toObject()["mpc"].isObject());}
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::forceAwareObserverAndMPCRestart_data(){
    QTest::addColumn<bool>("plate");QTest::addColumn<QString>("kind");QTest::addColumn<uint>("N");
    for(bool plate:{false,true})for(const auto &kind:{QString("LQR"),QString("MPC")})for(uint N:{2u,3u})
        QTest::newRow(qPrintable(QString("%1-%2-N%3").arg(plate?"plate":"beam").arg(kind).arg(N)))<<plate<<kind<<N;
}
void TestPiezoNativeBackendBridge::forceAwareObserverAndMPCRestart() try{
    QFETCH(bool,plate);QFETCH(QString,kind);QFETCH(uint,N);auto source=experiment(plate,kind,N);auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    source.getTimeSolver().setInputStartTime(1.31);auto controller=parse(setup.controllerConfiguration);
    controller["stateSource"]="sensor_only";controller["selectedOutputs"]=QJsonArray{"sense","tip:a"};
    controller["observer"]=QJsonObject{{"processCovariance",QJsonArray{QJsonArray{.1,0.},QJsonArray{0.,.1}}},{"measurementCovariance",QJsonArray{QJsonArray{.01,0.},QJsonArray{0.,.01}}}};
    setup.controllerConfiguration=json(controller);setup.bindToModel(source);auto complete=bridge(source);Run continuous(source,complete.callbacks);
    close(complete.exportPackage["initialTime"].toDouble(),1.3);auto physical=continuous.acceptedState();const auto boundary=continuous.acceptedRuntime();
    while(continuous.hasRemainingSteps())step(continuous,source);const auto expected=Codec::encode(continuous.acceptedRuntime());
    QVERIFY(continuous.acceptedState().freeForceResidual<1e-7);QTemporaryDir dir;QVERIFY(dir.isValid());
    // Every control phase, including a checkpoint after the final FE step.
    for(uint split=0;split<=8;++split){auto fresh=bridge(source);Run first(source,fresh.callbacks);for(uint i=0;i<split;++i)step(first,source);
        auto model=first.acceptedModel(source);const auto path=model.nativeWritePaths(dir.filePath(QString("forced-%1.rbm").arg(split)),false).first();model.write(path,false);
        RModel disk;disk.read(path);disk.getProblemSetup().setRestart(true);auto resumedBridge=bridge(disk);Run resumed(disk,resumedBridge.callbacks);
        while(resumed.hasRemainingSteps())step(resumed,disk);QCOMPARE(Codec::encode(resumed.acceptedRuntime()),expected);
    }
    const auto a=complete.callbacks.propose(physical,boundary.completedFE,boundary.controlTick,boundary.opaqueControllerState);
    physical.integrator.state.q[0]+=99.;physical.integrator.state.v[0]-=88.;physical.physicalDisplacements[14]+=77.;
    const auto b=complete.callbacks.propose(physical,boundary.completedFE,boundary.controlTick,boundary.opaqueControllerState);
    QCOMPARE(a.requestedVoltages,b.requestedVoltages);QCOMPARE(a.nextOpaqueState,b.nextOpaqueState);
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::jointFailureKeepsControllerCandidateRetryable_data(){
    QTest::addColumn<QString>("kind");QTest::newRow("configured-PI")<<QString("PI");QTest::newRow("sensor-only-MPC-observer")<<QString("MPC");
}
void TestPiezoNativeBackendBridge::jointFailureKeepsControllerCandidateRetryable() try{
    QFETCH(QString,kind);auto source=experiment(false,kind,1);auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    if(kind=="PI"){
        auto controller=parse(setup.controllerConfiguration);controller["reference"]=QJsonArray{.1};setup.controllerConfiguration=json(controller);setup.bindToModel(source);
    }else{
        auto controller=parse(setup.controllerConfiguration);controller["stateSource"]="sensor_only";controller["selectedOutputs"]=QJsonArray{"sense"};
        controller["observer"]=QJsonObject{{"processCovariance",QJsonArray{QJsonArray{.1,0.},QJsonArray{0.,.1}}},{"measurementCovariance",rowMatrix(QJsonArray{.01})}};
        setup.controllerConfiguration=json(controller);setup.bindToModel(source);
    }
    auto result=bridge(source);const auto originalPostFE=result.callbacks.postFE;bool failOnce=true;
    result.callbacks.postFE=[&](const auto &state,uint completed,uint tick,const auto &applied,const QByteArray &old){
        const auto next=originalPostFE(state,completed,tick,applied,old);if(failOnce){failOnce=false;throw std::runtime_error("injected postFE failure after native candidate mutation");}return next;
    };
    Run run(source,result.callbacks);const auto acceptedBytes=Codec::encode(run.acceptedRuntime());const auto acceptedState=run.acceptedRuntime();
    QVERIFY(rejects([&]{run.prepare(source);}));QCOMPARE(Codec::encode(run.acceptedRuntime()),acceptedBytes);QCOMPARE(run.completedSteps(),0u);
    auto first=run.prepare(source);auto second=run.prepare(source);const auto firstBytes=Codec::encode(first->runtime());
    QCOMPARE(Codec::encode(second->runtime()),firstBytes);QCOMPARE(Codec::encode(run.acceptedRuntime()),acceptedBytes);QCOMPARE(run.completedSteps(),0u);
    const auto oldOpaque=parse(acceptedState.opaqueControllerState),nextOpaque=parse(first->runtime().opaqueControllerState);
    if(kind=="PI")QVERIFY(oldOpaque["integral"]!=nextOpaque["integral"]);
    else {QVERIFY(oldOpaque["observerPrior"]!=nextOpaque["observerPrior"]);QVERIFY(!nextOpaque["warmStart"].toArray().isEmpty());QVERIFY(oldOpaque["warmStart"]!=nextOpaque["warmStart"]);}
    QCOMPARE(first->runtime().actions.size(),size_t(1));QCOMPARE(first->runtime().actions[0].previousApplied,acceptedState.lastApplied[0]);
    QCOMPARE(nextOpaque["previousApplied"].toArray().size(),int(first->runtime().actions.size()));
    QCOMPARE(nextOpaque["previousApplied"].toArray()[0].toDouble(),first->runtime().actions[0].applied);
    QCOMPARE(first->runtime().actions[0].applied,first->runtime().lastApplied[0]);
    run.validatePrepared(*second);result.callbacks.validateAcceptedBoundary(second->runtime());
    QCOMPARE(Codec::encode(run.acceptedRuntime()),acceptedBytes);run.commit(*second);QCOMPARE(run.completedSteps(),1u);
    QCOMPARE(Codec::encode(run.acceptedRuntime()),firstBytes);QCOMPARE(Codec::encode(run.acceptedRuntime()),Codec::encode(second->runtime()));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::configuredForceColumnsAndAffineBias_data(){QTest::addColumn<bool>("plate");QTest::newRow("beam")<<false;QTest::newRow("plate")<<true;}
void TestPiezoNativeBackendBridge::configuredForceColumnsAndAffineBias() try{
    QFETCH(bool,plate);auto source=experiment(plate);auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    // Freeze the original equilibrium, then apply a different background load.
    RStructuralPiezoLayeredModelAdapter reference(source,setup);const auto q0=reference.mesh().reconstructPhysical(reference.data().q0),f0=reference.mesh().reconstructPhysical(reference.data().referenceForce);
    setup.workpoint.available=true;for(size_t i=0;i<reference.mesh().nodeIds.size();++i){RStructuralPiezoLayeredNodeWorkpoint node;node.physicalNodeId=reference.mesh().nodeIds[i];for(size_t c=0;c<6;++c){node.q0[c]=q0[6*i+c];node.f0[c]=f0[6*i+c];}setup.workpoint.nodes.push_back(node);}
    for(const auto &port:reference.electrical().referenceResponse().ports)setup.workpoint.ports.push_back({port.portId,port.voltage,port.charge});
    setup.backgroundForces[0].values[2]-=.07;
    setup.excitationConfiguration=json({{"channels",QJsonArray{
        QJsonObject{{"channelId","node:123:RY"},{"unit","N m"},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",.002}}}}},
        QJsonObject{{"channelId","node:123:Z"},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","0.01*t"}}}}}}}});setup.bindToModel(source);
    const auto actual=bridge(source);const auto exported=actual.exportPackage;RStructuralPiezoLayeredModelAdapter adapter(source,setup);const auto &d=adapter.data();
    QCOMPARE(exported["disturbanceInputs"].toArray(),QJsonArray({QJsonArray{"node:123:RY","N m"},QJsonArray{"node:123:Z","N"}}));
    size_t declarations=0;for(const auto &value:exported["forceCatalog"].toArray()){const auto binding=value.toObject();if(!binding.contains("disturbanceColumn"))continue;++declarations;
        const size_t column=size_t(binding["disturbanceColumn"].toDouble()),physical=size_t(binding["physicalIndex"].toDouble());
        for(size_t i=0;i<d.freeDofs;++i)close(entry(exported["Bforce"],i,column),entry(exported["reducedPhysicalForceMap"],i,physical));
        for(size_t i=0;i<5;++i)close(entry(exported["Dforce"],i,column),entry(exported["physicalForceDirectFeedthrough"],i,physical));}
    QCOMPARE(declarations,size_t(2));double bias=0;for(size_t i=0;i<d.freeDofs;++i){close(exported["biasForce"].toArray()[int(i)].toDouble(),d.backgroundOffset[i]);bias=std::hypot(bias,d.backgroundOffset[i]);}QVERIFY(bias>.001);
    const auto forceFE=exported["design"].toObject()["forceAware"].toObject()["FE"].toObject();const auto affine=forceFE["affineDerivative"].toArray();QCOMPARE(affine.size(),int(2*d.freeDofs));
    for(size_t i=0;i<d.freeDofs;++i){QCOMPARE(affine[int(i)].toDouble(),0.);double rhs=0;for(size_t j=0;j<d.freeDofs;++j)rhs+=d.mass[i*d.freeDofs+j]*affine[int(d.freeDofs+j)].toDouble();close(rhs,d.backgroundOffset[i]);}
    for(size_t i=0;i<5;++i){double output=0;for(size_t j=0;j<d.freeDofs;++j)output+=entry(exported["La"],i,j)*affine[int(d.freeDofs+j)].toDouble();close(forceFE["outputBias"].toArray()[int(i)].toDouble(),output);}
    Run run(source,actual.callbacks);step(run,source);QVERIFY(run.acceptedState().freeForceResidual<1e-7);
    auto channels=parse(setup.excitationConfiguration)["channels"].toArray();channels.append(channels[0]);setup.excitationConfiguration=json({{"channels",channels}});setup.bindToModel(source);QVERIFY(rejects([&]{bridge(source);}));
    channels.removeLast();auto wrong=channels[0].toObject();wrong["unit"]="N";channels[0]=wrong;setup.excitationConfiguration=json({{"channels",channels}});setup.bindToModel(source);QVERIFY(rejects([&]{bridge(source);}));
    setup.excitationConfiguration.clear();setup.bindToModel(source);const auto free=bridge(source).exportPackage;
    QCOMPARE(free["disturbanceInputs"].toArray().size(),0);for(const auto &row:free["Bforce"].toArray())QCOMPARE(row.toArray().size(),0);
    QCOMPARE(free["controllerModelScope"].toString(),QString("complete-known-forcing"));
    for(const auto &binding:free["forceCatalog"].toArray())QVERIFY(!binding.toObject().contains("disturbanceColumn"));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::defaultRuntimeFactory() try{
    const auto model=experiment();const auto result=PiezoNativeBackendBridge::create(model);
    QCOMPARE(result.callbacks.sourceFingerprint,model.getProblemSetup().getStructuralPiezoLayeredSetup().computeSourceFingerprint(model));
    QCOMPARE(result.exportPackage["controllerModelScope"].toString(),QString("complete-known-forcing"));Run run(model,result.callbacks);step(run,model);
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::deterministicCompilationRestoresHost_data(){
    QTest::addColumn<bool>("plate");QTest::addColumn<bool>("modal");
    for(bool plate:{false,true})for(bool modal:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(plate?"plate":"beam").arg(modal?"modal-C":"Rayleigh")))<<plate<<modal;
}
void TestPiezoNativeBackendBridge::deterministicCompilationRestoresHost() try{
    QFETCH(bool,plate);QFETCH(bool,modal);const int originalThreads=omp_get_max_threads(),originalDynamic=omp_get_dynamic();
    {
        RParallelExecutionConfig hostPolicy;hostPolicy.setMode(RParallelExecutionMode::FixedThreads);hostPolicy.setRequestedThreads(4);hostPolicy.setDeterministicReduction(false);
        const RParallelExecutionContext host(hostPolicy);const int hostThreads=omp_get_max_threads(),hostDynamic=omp_get_dynamic();
        auto model=experiment(plate);auto &setup=model.getProblemSetup().getStructuralPiezoLayeredSetup();if(modal){setup.damping={};setup.damping.kind=RStructuralPiezoDampingKind::UniformModal;setup.damping.uniformRatio=.025;setup.bindToModel(model);}
        const auto first=bridge(model);QCOMPARE(omp_get_max_threads(),hostThreads);QCOMPARE(omp_get_dynamic(),hostDynamic);
        const auto second=bridge(model);QCOMPARE(omp_get_max_threads(),hostThreads);QCOMPARE(omp_get_dynamic(),hostDynamic);
        QCOMPARE(realBits(first.exportPackage["q0"].toArray()),realBits(second.exportPackage["q0"].toArray()));
        QCOMPARE(json(first.exportPackage),json(second.exportPackage));QCOMPARE(first.callbacks.backendIdentity,second.callbacks.backendIdentity);
        QCOMPARE(first.callbacks.authoritativeFullC,second.callbacks.authoritativeFullC);
        setup.materials[0].density+=1;QVERIFY(rejects([&]{bridge(model);}));QCOMPARE(omp_get_max_threads(),hostThreads);QCOMPARE(omp_get_dynamic(),hostDynamic);
    }
    QCOMPARE(omp_get_max_threads(),originalThreads);QCOMPARE(omp_get_dynamic(),originalDynamic);
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::unsupportedSourceConfigurations_data(){
    QTest::addColumn<int>("kind");QTest::newRow("stale-source")<<0;QTest::newRow("unsupported-axis")<<1;QTest::newRow("external-executable")<<2;QTest::newRow("partial-envelope")<<3;
}
void TestPiezoNativeBackendBridge::unsupportedSourceConfigurations() try{
    QFETCH(int,kind);DiagnosticTimer elapsed(QTest::currentDataTag());auto m=experiment();auto &setup=m.getProblemSetup().getStructuralPiezoLayeredSetup();
    if(kind==0)setup.materials[0].density+=1;
    if(kind==1){auto design=parse(setup.designConfiguration);design["outputs"]=QJsonArray{QJsonObject{{"id","bad"},{"physicalNodeId",123},{"dof","RX"},{"quantity","displacement"}}};setup.designConfiguration=json(design);setup.bindToModel(m);}
    if(kind==2){setup.excitationConfiguration=json({{"external","arbitrary-program"}});setup.bindToModel(m);}
    if(kind==3){auto controller=parse(setup.controllerConfiguration);controller.remove("deltaVoltageLimits");setup.controllerConfiguration=json(controller);setup.bindToModel(m);}
    QVERIFY(rejects([&]{bridge(m);}));
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::frozenBackendModuleStaleness() try{
    DiagnosticTimer elapsed("frozen-module");
    // Freeze and recheck an independent module copy, never mutate working source.
    QTemporaryDir copy;const QString package=copy.filePath("range_workflow");QVERIFY(QDir().mkpath(package));
    const QDir originalPackage(QDir(QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT)).filePath("range_workflow"));
    for(const auto &name:originalPackage.entryList({"*.py"},QDir::Files))QVERIFY(QFile::copy(originalPackage.filePath(name),QDir(package).filePath(name)));
    qInfo()<<"frozen-module copy complete";const auto m=experiment();auto copied=PiezoNativeBackendBridge::create(m,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),copy.path());qInfo()<<"frozen-module provider initialized";
    QFile changed(QDir(package).filePath("piezo_native_backend.py"));
#ifdef Q_OS_WIN
    const auto frozenIdentity=copied.callbacks.backendIdentity;
    QVERIFY(!changed.open(QIODevice::Append));
    QVERIFY(!rejects([&]{copied.callbacks.perturbationForce(.2);}));
    QCOMPARE(copied.callbacks.backendIdentity,frozenIdentity);
#else
    QVERIFY(changed.open(QIODevice::Append));changed.write("\n# changed frozen module\n");changed.close();QVERIFY(rejects([&]{copied.callbacks.perturbationForce(.2);}));
#endif
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::explicitExecutablePaths_data(){QTest::addColumn<bool>("present");QTest::newRow("missing-executable")<<false;QTest::newRow("invalid-executable")<<true;}
void TestPiezoNativeBackendBridge::explicitExecutablePaths() try{
    QFETCH(bool,present);DiagnosticTimer elapsed(QTest::currentDataTag());QTemporaryDir copy;QVERIFY(copy.isValid());const auto path=copy.filePath("rejected-python.exe");
    if(present){QFile invalid(path);QVERIFY(invalid.open(QIODevice::WriteOnly));invalid.write("invalid frozen executable\n");invalid.close();}
    const auto m=experiment();QElapsedTimer rejection;rejection.start();
#ifdef Q_OS_WIN
    QString diagnostic;
    try{PiezoNativeBackendBridge::create(m,path,QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));}
    catch(const std::exception &error){diagnostic=QString::fromUtf8(error.what());}
    QVERIFY2(!diagnostic.isEmpty(),"Invalid executable was admitted");
    if(present)QVERIFY2(diagnostic.contains("requires a native Windows executable"),qPrintable(diagnostic));
    QVERIFY2(rejection.elapsed()<10000,qPrintable(QString("Executable rejection took %1 ms").arg(rejection.elapsed())));
#else
    QVERIFY(rejects([&]{PiezoNativeBackendBridge::create(m,path,QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));}));
#endif
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::eccentricBeamBridgePreservesCoupledMassAndNewmark() try{
    const auto source=eccentricLayeredBeam();const auto result=bridge(source);RStructuralPiezoLayeredModelAdapter adapter(source,source.getProblemSetup().getStructuralPiezoLayeredSetup());
    const auto &data=adapter.data();const auto &mesh=adapter.mesh();const auto &operators=mesh.operators;const auto &exported=result.exportPackage;
    const size_t full=operators.dofs,free=data.freeDofs;QCOMPARE(full,size_t(6));QCOMPARE(free,size_t(3));
    QCOMPARE(exported["sourceFingerprint"].toString(),data.sourceFingerprint);QCOMPARE(exported["sourceFrame"].toObject()["family"].toString(),QString("planarBeam"));
    const auto nodes=exported["nodes"].toArray();QCOMPARE(nodes.size(),2);QCOMPARE(nodes[0].toObject()["physicalNodeId"].toDouble(),101.);QCOMPARE(nodes[0].toObject()["physicalOffset"].toDouble(),0.);
    QCOMPARE(nodes[1].toObject()["physicalNodeId"].toDouble(),123.);QCOMPARE(nodes[1].toObject()["physicalOffset"].toDouble(),6.);
    const auto nodeUnits=nodes[1].toObject()["units"].toArray();QCOMPARE(nodeUnits,QJsonArray({"m","m","m","rad","rad","rad"}));
    const auto members=exported["members"].toArray();QCOMPARE(members.size(),1);QCOMPARE(members[0].toObject()["physicalElementId"].toDouble(),501.);
    QCOMPARE(members[0].toObject()["physicalNodeIds"].toArray(),QJsonArray({101.,123.}));
    QCOMPARE(exported["materials"].toArray().size(),2);QCOMPARE(exported["materials"].toArray()[0].toObject()["sourceId"].toString(),QString("independent-eccentric-beam-example"));
    QCOMPARE(exported["T"].toArray().size(),int(full));QCOMPARE(exported["P"].toArray().size(),int(6*mesh.nodeIds.size()));
    for(size_t i=0;i<full;++i)for(size_t j=0;j<free;++j)QCOMPARE(entry(exported["T"],i,j),data.T[i*free+j]);
    for(size_t i=0;i<6*mesh.nodeIds.size();++i)for(size_t j=0;j<full;++j)QCOMPARE(entry(exported["P"],i,j),mesh.physicalTransform[i*full+j]);
    const size_t fixedFull[3]={0,1,2};for(size_t row:fixedFull)for(size_t j=0;j<free;++j)QCOMPARE(data.T[row*free+j],0.);
    const size_t tipUx=3,tipRy=5;size_t uxColumn=free,ryColumn=free;
    for(size_t j=0;j<free;++j){if(std::abs(data.T[tipUx*free+j])>.5)uxColumn=j;if(std::abs(data.T[tipRy*free+j])>.5)ryColumn=j;}
    QVERIFY(uxColumn<free);QVERIFY(ryColumn<free);QVERIFY(uxColumn!=ryColumn);
    QCOMPARE(entry(exported["P"],6,tipUx),1.);QCOMPARE(entry(exported["P"],10,tipRy),1.);
    QCOMPARE(exported["dofs"].toArray().size(),int(free));QCOMPARE(exported["dofs"].toArray()[int(uxColumn)].toArray(),QJsonArray({QString("free:%1").arg(uxColumn),"m"}));
    QCOMPARE(exported["dofs"].toArray()[int(ryColumn)].toArray(),QJsonArray({QString("free:%1").arg(ryColumn),"rad"}));
    QVERIFY2(std::abs(operators.mass[tipUx*full+tipRy])>1e-8,"The eccentric section must produce a visible physical translation/rotation mass cross term");
    QVERIFY(std::abs(operators.mass[tipUx*full+tipRy])/std::sqrt(operators.mass[tipUx*full+tipUx]*operators.mass[tipRy*full+tipRy])>1e-3);
    for(size_t i=0;i<full;++i)for(size_t j=0;j<full;++j)QCOMPARE(entry(exported["fullM"],i,j),operators.mass[i*full+j]);
    for(size_t i=0;i<free;++i)for(size_t j=0;j<free;++j){
        double constrained=0;for(size_t a=0;a<full;++a)for(size_t b=0;b<full;++b)constrained+=data.T[a*free+i]*operators.mass[a*full+b]*data.T[b*free+j];
        QCOMPARE(entry(exported["M"],i,j),data.mass[i*free+j]);close(data.mass[i*free+j],constrained);
    }
    const double reducedCross=data.mass[uxColumn*free+ryColumn];
    QVERIFY2(std::abs(reducedCross)>1e-8,"The translation/rotation cross term must survive the cantilever constraints");
    QVERIFY(std::abs(reducedCross)/std::sqrt(data.mass[uxColumn*free+uxColumn]*data.mass[ryColumn*free+ryColumn])>1e-3);
    QCOMPARE(exported["disturbanceInputs"].toArray(),rowMatrix(QJsonArray{"node:123:X","N"}));
    const auto forceCatalog=exported["forceCatalog"].toArray();bool forceMapped=false;
    for(const auto &value:forceCatalog){const auto binding=value.toObject();if(binding["channelId"].toString()!="node:123:X")continue;
        QCOMPARE(binding["physicalNodeId"].toDouble(),123.);QCOMPARE(binding["dof"].toString(),QString("X"));QCOMPARE(binding["unit"].toString(),QString("N"));
        QCOMPARE(binding["physicalIndex"].toDouble(),6.);QCOMPARE(binding["coordinateSystem"].toString(),QString("global"));
        QCOMPARE(binding["physicalDirection"].toArray(),QJsonArray({1.,0.,0.,0.,0.,0.}));forceMapped=true;
    }
    QVERIFY(forceMapped);QCOMPARE(exported["inputs"].toArray()[0].toArray(),QJsonArray({"drive","V"}));
    for(size_t i=0;i<free;++i)close(entry(exported["Bforce"],i,0),entry(exported["reducedPhysicalForceMap"],i,6));
    qInfo()<<"eccentricBeam mass cross full/reduced/normalized"<<operators.mass[tipUx*full+tipRy]<<reducedCross
           <<std::abs(reducedCross)/std::sqrt(data.mass[uxColumn*free+uxColumn]*data.mass[ryColumn*free+ryColumn]);

    Run run(source,result.callbacks);const auto left=run.acceptedState();const auto &runData=run.adapter().data();const size_t n=runData.freeDofs;
    const auto prepared=run.prepare(source);run.validatePrepared(*prepared);const auto &applied=prepared->result().heldActuatorVoltages;
    const double h=source.getProblemSetup().getStructuralDynamicsSetup().getTimeStep(),leftTime=left.integrator.state.time,rightTime=leftTime+h;
    auto reducedForce=[&](double time){
        auto physical=result.callbacks.perturbationForce(time);const auto projected=mesh.projectPhysical(physical,RStructuralPiezoPhysicalDomain::ForceAndMoment);
        std::vector<double> value(runData.backgroundOffset);for(size_t i=0;i<runData.fullDofs;++i)for(size_t j=0;j<n;++j)value[j]+=runData.T[i*n+j]*projected[i];
        for(size_t i=0;i<n;++i)for(size_t j=0;j<applied.size();++j)value[i]+=runData.input[i*applied.size()+j]*(applied[j]-runData.actuatorVoltages0[j]);
        return value;
    };
    auto leftRhs=reducedForce(leftTime),rightRhs=reducedForce(rightTime);
    auto matVec=[](const std::vector<double> &matrix,const std::vector<double> &x,size_t size){std::vector<double> value(size);for(size_t i=0;i<size;++i)for(size_t j=0;j<size;++j)value[i]+=matrix[i*size+j]*x[j];return value;};
    auto initialEq=leftRhs,cv=matVec(runData.damping,left.integrator.state.v,n),kq=matVec(runData.stiffness,left.integrator.state.q,n);
    for(size_t i=0;i<n;++i)initialEq[i]-=cv[i]+kq[i];
    const auto leftAcceleration=independentMassSolve(runData.mass,initialEq);
    std::vector<double> predictorQ=left.integrator.state.q,predictorV=left.integrator.state.v,effective=runData.mass;
    for(size_t i=0;i<n;++i){predictorQ[i]+=h*left.integrator.state.v[i]+.25*h*h*leftAcceleration[i];predictorV[i]+=.5*h*leftAcceleration[i];}
    for(size_t i=0;i<n*n;++i)effective[i]+=.5*h*runData.damping[i]+.25*h*h*runData.stiffness[i];
    auto independentRhs=rightRhs;cv=matVec(runData.damping,predictorV,n);kq=matVec(runData.stiffness,predictorQ,n);
    for(size_t i=0;i<n;++i)independentRhs[i]-=cv[i]+kq[i];
    const auto newmarkAcceleration=independentMassSolve(effective,independentRhs);
    for(size_t i=0;i<n;++i){predictorQ[i]+=.25*h*h*newmarkAcceleration[i];predictorV[i]+=.5*h*newmarkAcceleration[i];}
    auto reducedEquilibrium=rightRhs;cv=matVec(runData.damping,predictorV,n);kq=matVec(runData.stiffness,predictorQ,n);
    for(size_t i=0;i<n;++i)reducedEquilibrium[i]-=cv[i]+kq[i];
    const auto massRecoveredAcceleration=independentMassSolve(runData.mass,reducedEquilibrium);
    run.commit(*prepared);const auto &accepted=run.acceptedState();
    for(size_t i=0;i<n;++i){
        QVERIFY(std::abs(accepted.integrator.state.q[i]-predictorQ[i])<=2e-8*std::max({1.,std::abs(accepted.integrator.state.q[i]),std::abs(predictorQ[i])}));
        QVERIFY(std::abs(accepted.integrator.state.v[i]-predictorV[i])<=2e-8*std::max({1.,std::abs(accepted.integrator.state.v[i]),std::abs(predictorV[i])}));
        QVERIFY(std::abs(accepted.integrator.state.a[i]-massRecoveredAcceleration[i])<=2e-8*std::max({1.,std::abs(accepted.integrator.state.a[i]),std::abs(massRecoveredAcceleration[i])}));
        QVERIFY(std::abs(accepted.integrator.state.a[i]-newmarkAcceleration[i])<=2e-8*std::max({1.,std::abs(accepted.integrator.state.a[i]),std::abs(newmarkAcceleration[i])}));
    }
    QVERIFY(run.completedSteps()==1);QVERIFY(accepted.freeForceResidual<1e-7);
    QVERIFY(std::abs(massRecoveredAcceleration[ryColumn])>1e-4);
    QVERIFY(std::abs(reducedCross*massRecoveredAcceleration[ryColumn])>1e-5);
    qInfo()<<"eccentricBeam first accepted Newmark step, translational/rotational acceleration"<<massRecoveredAcceleration[uxColumn]<<massRecoveredAcceleration[ryColumn];
} REPORT_EXCEPTIONS
void TestPiezoNativeBackendBridge::eccentricBeamExplicitWorkpointTangentPortsAndAcceptedReadout() try{
    auto source=eccentricLayeredBeam();auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    using D=RStructuralPiezoPhysicalDof;setup.supports={{101,D::UX,.001},{101,D::UZ,0},{101,D::RY,0}};
    setup.ports[0].initialValue=.22;setup.backgroundForces.clear();setup.workpoint={};setup.bindToModel(source);

    // Build a nonzero absolute FE witness, then declare both its fixed-source
    // load and explicit workpoint. The support reaction below is a physical
    // prescribed-DOF load; it changes the full residual without polluting the
    // free equilibrium.
    RStructuralPiezoLayeredModelAdapter seed(source,setup);const auto &mesh=seed.mesh();const auto &op=mesh.operators;
    QCOMPARE(op.dofs,size_t(6));QCOMPARE(mesh.nodeIds,std::vector<uint>({101,123}));
    std::vector<double> physicalQ(6*mesh.nodeIds.size(),0.0);physicalQ[0]=.001;
    const size_t tipOffset=6;physicalQ[tipOffset]=.013;physicalQ[tipOffset+2]=.004;physicalQ[tipOffset+4]=.008;
    const auto q0=mesh.projectPhysical(physicalQ,RStructuralPiezoPhysicalDomain::Displacement);
    const auto voltage0=std::vector<double>{.22};const auto reference=seed.electrical().evaluate(q0,voltage0);
    auto physicalF=mesh.reconstructPhysical(reference.internalForce);physicalF[0]+=.75;
    physicalQ=mesh.reconstructPhysical(q0);
    setup.workpoint.available=true;setup.workpoint.nodes.clear();setup.workpoint.ports.clear();setup.backgroundForces.clear();
    for(size_t i=0;i<mesh.nodeIds.size();++i){
        RStructuralPiezoLayeredNodeWorkpoint workpoint;workpoint.physicalNodeId=mesh.nodeIds[i];
        RStructuralPiezoLayeredNodeField background;background.physicalNodeId=mesh.nodeIds[i];
        for(size_t c=0;c<6;++c){workpoint.q0[c]=physicalQ[6*i+c];workpoint.f0[c]=physicalF[6*i+c];background.values[c]=physicalF[6*i+c];}
        setup.workpoint.nodes.push_back(workpoint);setup.backgroundForces.push_back(background);
    }
    for(const auto &port:reference.ports)setup.workpoint.ports.push_back({port.portId,port.voltage,port.charge});
    setup.bindToModel(source);const auto &boundSetup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    const auto exportedResult=bridge(source);const auto &exported=exportedResult.exportPackage;
    RStructuralPiezoLayeredModelAdapter adapter(source,boundSetup);const auto &data=adapter.data();const auto &actualMesh=adapter.mesh();const auto &actualOp=actualMesh.operators;
    const size_t n=actualOp.dofs,np=actualOp.portIds.size(),free=data.freeDofs;QCOMPARE(n,size_t(6));QCOMPARE(free,size_t(3));
    QVERIFY(boundSetup.workpoint.available);QCOMPARE(boundSetup.workpoint.nodes.size(),size_t(2));QCOMPARE(boundSetup.workpoint.ports.size(),np);
    QVERIFY(std::any_of(data.q0.begin(),data.q0.end(),[](double x){return std::abs(x)>1e-8;}));
    QVERIFY(std::any_of(data.actuatorVoltages0.begin(),data.actuatorVoltages0.end(),[](double x){return std::abs(x)>1e-8;}));
    QVERIFY(std::any_of(data.referenceForce.begin(),data.referenceForce.end(),[](double x){return std::abs(x)>1e-8;}));
    const auto exportedQ0=exported["Q0"].toArray();
    QVERIFY(std::any_of(exportedQ0.cbegin(),exportedQ0.cend(),[](const QJsonValue &x){return x.isDouble()&&std::abs(x.toDouble())>1e-12;}));
    QCOMPARE(exported["q0"].toArray().size(),int(n));QCOMPARE(exported["V0"].toArray().size(),int(np));
    QCOMPARE(exported["Q0"].toArray().size(),int(np));QCOMPARE(exported["f0"].toArray().size(),int(n));
    for(size_t i=0;i<n;++i){close(exported["q0"].toArray()[int(i)].toDouble(),data.q0[i]);close(exported["f0"].toArray()[int(i)].toDouble(),data.referenceForce[i]);}

    // Scoped export consistency: these arrays serialize the adapter's complete
    // mechanical/electrically condensed operators in stable FE source order.
    for(size_t i=0;i<n;++i)for(size_t j=0;j<n;++j){
        QCOMPARE(entry(exported["fullKE"],i,j),actualOp.stiffness[i*n+j]);
        QCOMPARE(entry(exported["fullK"],i,j),adapter.electrical().condensed().stiffness[i*n+j]);
    }
    QVERIFY(entry(exported["fullK"],3,3)>0);
    bool condensedSensorTangent=false;
    for(size_t i=0;i<n*n;++i)condensedSensorTangent=condensedSensorTangent||
        std::abs(adapter.electrical().condensed().stiffness[i]-actualOp.stiffness[i])>1e-9;
    QVERIFY(condensedSensorTangent);
    const auto exportedG=exported["G"].toArray(),exportedCe=exported["Ce"].toArray();
    for(size_t i=0;i<n;++i)for(size_t j=0;j<np;++j)QCOMPARE(entry(exported["G"],i,j),actualOp.coupling[i*np+j]);
    for(size_t i=0;i<np;++i)for(size_t j=0;j<np;++j)QCOMPARE(entry(exported["Ce"],i,j),actualOp.capacitance[i*np+j]);
    QVERIFY(!exportedG.isEmpty());QVERIFY(!exportedCe.isEmpty());

    // Independently re-evaluate the exported full tangent by differentiating
    // the native absolute FE force response column by column.
    const auto tangent=exported["fullK"];
    for(size_t column=0;column<n;++column){
        const double h=1e-7;auto plus=q0,minus=q0;plus[column]+=h;minus[column]-=h;
        const auto fPlus=adapter.electrical().evaluate(plus,data.actuatorVoltages0).internalForce;
        const auto fMinus=adapter.electrical().evaluate(minus,data.actuatorVoltages0).internalForce;
        double error=0,scale=0;
        for(size_t row=0;row<n;++row){const double derivative=(fPlus[row]-fMinus[row])/(2*h),expected=entry(tangent,row,column);
            error=std::hypot(error,derivative-expected);scale=std::hypot(scale,expected);}
        QVERIFY2(scale>1e-8,qPrintable(QString("Full FE tangent column %1 is vacuous").arg(qulonglong(column))));
        QVERIFY2(error<=2e-6*scale,qPrintable(QString("Centered force-tangent error in column %1 is %2 / %3").arg(qulonglong(column)).arg(error,0,'g',12).arg(scale,0,'g',12)));
    }

    // Recompute the reference mechanical and electrical equations from K^E,
    // G, Ce, the explicit workpoint and f0, without consulting exported residuals.
    std::vector<double> referenceVoltage(np),referenceCharge(np),referenceForce(n);
    for(size_t port=0;port<np;++port){
        const auto namedIt=std::find_if(boundSetup.workpoint.ports.begin(),boundSetup.workpoint.ports.end(),[&](const auto &v){return v.portId==actualOp.portIds[port];});
        QVERIFY(namedIt!=boundSetup.workpoint.ports.end());const auto &named=*namedIt;
        referenceVoltage[port]=named.V0;referenceCharge[port]=named.Q0;
    }
    for(size_t i=0;i<n;++i){
        for(size_t j=0;j<n;++j)referenceForce[i]+=actualOp.stiffness[i*n+j]*data.q0[j];
        for(size_t port=0;port<np;++port)referenceForce[i]+=actualOp.coupling[i*np+port]*referenceVoltage[port];
    }
    std::vector<double> physicalReferenceForce(6*actualMesh.nodeIds.size(),0.0);
    for(size_t i=0;i<actualMesh.nodeIds.size();++i){
        const auto nodeIt=std::find_if(boundSetup.workpoint.nodes.begin(),boundSetup.workpoint.nodes.end(),[&](const auto &v){return v.physicalNodeId==actualMesh.nodeIds[i];});
        QVERIFY(nodeIt!=boundSetup.workpoint.nodes.end());const auto &node=*nodeIt;
        std::copy(node.f0.begin(),node.f0.end(),physicalReferenceForce.begin()+6*i);
    }
    const auto projectedF0=actualMesh.projectPhysical(physicalReferenceForce,RStructuralPiezoPhysicalDomain::ForceAndMoment);
    std::vector<double> fullResidual(n);for(size_t i=0;i<n;++i)fullResidual[i]=referenceForce[i]-projectedF0[i];
    for(size_t reduced=0;reduced<free;++reduced){double residual=0,scale=0;
        for(size_t i=0;i<n;++i){residual+=data.T[i*free+reduced]*fullResidual[i];scale+=std::abs(data.T[i*free+reduced])*(std::abs(referenceForce[i])+std::abs(projectedF0[i]));}
        QVERIFY2(std::abs(residual)<=2e-10*std::max(scale,1e-12),qPrintable(QString("Explicit workpoint free equilibrium failed at DOF %1: residual=%2 scale=%3").arg(qulonglong(reduced)).arg(residual,0,'g',12).arg(scale,0,'g',12)));
    }
    QVERIFY(std::abs(fullResidual[0]+.75)<=1e-9);QVERIFY(data.forceResidual<1e-10);QVERIFY(data.electricResidual<1e-10);
    close(exported["forceResidual"].toDouble(),data.forceResidual);close(exported["electricResidual"].toDouble(),data.electricResidual);
    for(size_t port=0;port<np;++port){double q=0,scale=std::abs(referenceCharge[port]);
        for(size_t other=0;other<np;++other){const double term=actualOp.capacitance[port*np+other]*referenceVoltage[other];q+=term;scale+=std::abs(term);}
        for(size_t i=0;i<n;++i){const double term=actualOp.coupling[i*np+port]*data.q0[i];q-=term;scale+=std::abs(term);}
        QVERIFY2(std::abs(q-referenceCharge[port])<=2e-10*std::max(scale,1e-18),qPrintable(QString("Named port %1 charge equation failed").arg(actualOp.portIds[port])));
    }

    // The nonzero affine support must survive reduction and return as physical p.
    QVERIFY(std::abs(data.p[0]-.001)<1e-12);QCOMPARE(exported["p"].toArray().size(),int(n));
    for(size_t i=0;i<n;++i)close(exported["p"].toArray()[int(i)].toDouble(),data.p[i]);
    const auto physicalP=actualMesh.reconstructPhysical(data.p);QVERIFY(std::abs(physicalP[0]-.001)<1e-12);

    const auto ports=exported["ports"].toArray();QCOMPARE(ports.size(),int(np));
    auto portMetadata=[&](const QString &id){for(const auto &value:ports)if(value.toObject()["portId"].toString()==id)return value.toObject();throw std::runtime_error("Named port metadata missing");};
    const auto drive=portMetadata("drive"),sense=portMetadata("sense");
    QCOMPARE(drive["role"].toInt(),int(RStructuralPiezoPortRole::ActuatorSensor));QCOMPARE(drive["readout"].toInt(),int(RStructuralPiezoReadoutMode::ActuatorCharge));QCOMPARE(drive["unit"].toString(),QString("C"));
    QCOMPARE(sense["role"].toInt(),int(RStructuralPiezoPortRole::Sensor));QCOMPARE(sense["readout"].toInt(),int(RStructuralPiezoReadoutMode::OpenCircuitVoltage));QCOMPARE(sense["unit"].toString(),QString("V"));
    auto namedRow=[](const QJsonArray &rows,const QString &id){int found=-1;for(int i=0;i<rows.size();++i){const auto row=rows[i].toArray();if(!row.isEmpty()&&row[0].toString()==id){if(found>=0)throw std::runtime_error("Duplicate named port row");found=i;}}return found;};
    const auto inputs=exported["inputs"].toArray(),outputs=exported["outputs"].toArray();
    const int driveInput=namedRow(inputs,"drive"),driveOutput=namedRow(outputs,"drive"),senseOutput=namedRow(outputs,"sense");
    QVERIFY(driveInput>=0&&driveOutput>=0&&senseOutput>=0);QCOMPARE(inputs[driveInput].toArray()[1].toString(),QString("V"));
    QCOMPARE(outputs[driveOutput].toArray()[1].toString(),QString("C"));QCOMPARE(outputs[senseOutput].toArray()[1].toString(),QString("V"));
    for(size_t port=0;port<np;++port){
        const QString id=actualOp.portIds[port];const auto named=portMetadata(id);
        close(exported["V0"].toArray()[int(port)].toDouble(),referenceVoltage[port]);
        close(exported["Q0"].toArray()[int(port)].toDouble(),referenceCharge[port]);
        QCOMPARE(named["unit"].toString(),id=="drive"?QString("C"):QString("V"));
    }

    Run run(source,exportedResult.callbacks);auto prepared=run.prepare(source);run.validatePrepared(*prepared);
    QVERIFY(prepared->result().integrator.state.time>run.acceptedState().integrator.state.time);
    run.commit(*prepared);QCOMPARE(run.completedSteps(),1u);
    const auto &accepted=run.acceptedState();QVERIFY(accepted.freeForceResidual<1e-7);
    const auto acceptedQ=actualMesh.projectPhysical(accepted.physicalDisplacements,RStructuralPiezoPhysicalDomain::Displacement);
    const auto acceptedElectrical=adapter.electrical().evaluate(acceptedQ,accepted.heldActuatorVoltages);
    const auto nativeSense=std::find_if(acceptedElectrical.ports.begin(),acceptedElectrical.ports.end(),[](const auto &p){return p.portId=="sense";});
    const auto acceptedSense=std::find_if(accepted.readouts.begin(),accepted.readouts.end(),[](const auto &p){return p.portId=="sense";});
    QVERIFY(nativeSense!=acceptedElectrical.ports.end());QVERIFY(acceptedSense!=accepted.readouts.end());
    QCOMPARE(acceptedSense->mode,RStructuralPiezoReadoutMode::OpenCircuitVoltage);QCOMPARE(acceptedSense->unit,QString("V"));QVERIFY(!acceptedSense->actionRecord);
    close(acceptedSense->value,nativeSense->voltage);close(acceptedSense->deltaValue,nativeSense->deltaVoltage);
    QVERIFY(std::abs(acceptedSense->deltaValue)>1e-12);
    qInfo()<<"explicit workpoint p0/free residual/sensor deltaV"<<data.p[0]<<data.forceResidual<<acceptedSense->deltaValue;
} REPORT_EXCEPTIONS

void TestPiezoNativeBackendBridge::externalSampledTransaction_data(){
    QTest::addColumn<uint>("controlStride");QTest::addColumn<uint>("forceStride");QTest::addColumn<bool>("sensorOnly");
    QTest::newRow("every-FE")<<1u<<1u<<true;
    QTest::newRow("independent-multirate")<<3u<<2u<<true;
    QTest::newRow("explicit-full-FE")<<1u<<1u<<false;
}
void TestPiezoNativeBackendBridge::externalSampledTransaction() try{
    QFETCH(uint,controlStride);QFETCH(uint,forceStride);QFETCH(bool,sensorOnly);QTemporaryDir directory;QVERIFY(directory.isValid());
    const QString scriptPath=directory.filePath("pure_endpoint.py"),callsPath=scriptPath+".calls";
    QFile script(scriptPath);QVERIFY(script.open(QIODevice::WriteOnly));
    const QByteArray program=QString("import sys\nsys.path.insert(0, %1)\n").arg(QString::fromUtf8(QJsonDocument(QJsonArray{QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT)}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)).toUtf8()+R"PY(from pathlib import Path
import json
from range_workflow.piezo_external_io import serve
ports=[]
def initialize(request):
    ports.extend(request['ports'])
    return {'excitation': {'n':0}, 'controller': {'n':0}}
def evaluate(request):
    with open(__file__+'.calls','a',encoding='utf-8') as f:
        f.write(json.dumps(request)+'\n')
    outputs=[]
    for p in ports:
        if p['role']==request['role'] and p['direction']=='output':
            value=.02*(request['state']['n']+1) if request['role']=='excitation' else .12-.05*request['inputs'][0]['value']
            outputs.append({'id':p['id'],'value':value,'unit':p['unit'],'reference':p['reference']})
    return outputs, {'n':request['state']['n']+1}
serve(sys.stdin.buffer,sys.stdout.buffer,algorithm={'id':'native-test','version':'1'},initialize=initialize,evaluate=evaluate)
)PY";
    QCOMPARE(script.write(program),qint64(program.size()));script.close();
    auto source=experiment(false,"PI",controlStride);auto &setup=source.getProblemSetup().getStructuralPiezoLayeredSetup();
    const QJsonObject connection{{"transport","stdio"},{"executable",QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON)},
        {"args",QJsonArray{"-u",scriptPath}},{"cwd",directory.path()},{"algorithm",QJsonObject{{"id","native-test"},{"version","1"}}},
        {"requestTimeout",3},{"handshakeTimeout",3}};
    auto controller=parse(setup.controllerConfiguration);for(const auto *key:{"kp","ki","bias"})controller.remove(key);
    controller["kind"]="external";controller["stateSource"]=sensorOnly?"sensor_only":"full_fe";controller["selectedOutputs"]=QJsonArray{"tip:a"};
    controller["external"]=QJsonObject{{"connection",connection},{"initialState",QJsonObject{{"n",0}}},{"seed",7}};
    setup.controllerConfiguration=json(controller);setup.excitationConfiguration=json({{"channels",QJsonArray{}},{"externalSources",QJsonArray{QJsonObject{
        {"connection",connection},{"channelIds",QJsonArray{"node:123:Z"}},{"sampleEveryFESteps",int(forceStride)},{"initialState",QJsonObject{{"n",0}}},{"seed",7}}}}});
    setup.bindToModel(source);auto result=bridge(source);QVERIFY(!QFileInfo::exists(callsPath));
    const auto originalPostFE=result.callbacks.postFE;bool failOnce=true;
    result.callbacks.postFE=[&](const auto &physical,uint completed,uint tick,const auto &applied,const QByteArray &old){
        auto candidate=originalPostFE(physical,completed,tick,applied,old);
        if(failOnce){failOnce=false;throw std::runtime_error("injected external postFE candidate failure");}return candidate;
    };
    Run run(source,result.callbacks);QCOMPARE(run.acceptedState().perturbationForces[14],.02);
    const auto initial=Codec::encode(run.acceptedRuntime());std::vector<QByteArray> history{initial};QVERIFY(rejects([&]{run.prepare(source);}));QCOMPARE(Codec::encode(run.acceptedRuntime()),initial);
    for(uint i=0;i<4;++i){
        const double previousAcceleration=run.acceptedState().physicalAccelerations[14];
        const auto before=Codec::encode(run.acceptedRuntime());auto first=run.prepare(source);auto second=run.prepare(source);
        QCOMPARE(Codec::encode(first->runtime()),Codec::encode(second->runtime()));QCOMPARE(Codec::encode(run.acceptedRuntime()),before);
        QCOMPARE(first->result().perturbationForces[14],.02*double(i/forceStride+1));
        QFile calls(callsPath);QVERIFY(calls.open(QIODevice::ReadOnly));const auto lines=calls.readAll().trimmed().split('\n');
        if(i%controlStride==0){QJsonObject latest;for(const auto &line:lines){const auto request=parse(line);if(request["role"].toString()=="controller")latest=request;}
            QVERIFY(!latest.isEmpty());close(latest["inputs"].toArray()[0].toObject()["value"].toDouble(),previousAcceleration);
        }
        run.validatePrepared(*second);run.commit(*second);
        const auto accepted=run.acceptedRuntime();QCOMPARE(parse(accepted.opaqueControllerState)["excitationStates"].toArray()[0].toObject()["n"].toInt(),int(i/forceStride+1));
        const auto saved=run.acceptedModel(source);const auto countBefore=lines.size();history.push_back(Codec::encode(accepted));
        PiezoCertificationBatchCache cache(result.certificationBatchRequest,result.backendInitializationIdentity,"layered",accepted.initialTime,accepted.dt,accepted.targetSteps,controlStride,18);
        const auto certifiedCallbacks=cache.wrapLayered(result.callbacks);cache.prepare(history);
        for(uint boundary=0;boundary<=i+1;++boundary){
            const double time=boundary?accepted.initialTime+double(boundary)*accepted.dt:accepted.initialTime;
            QCOMPARE(certifiedCallbacks.perturbationForce(time)[14],.02*double((boundary?boundary-1:0)/forceStride+1));
        }
        auto resumedSource=saved;resumedSource.getProblemSetup().setRestart(true);auto resumedBridge=bridge(resumedSource);Run resumed(resumedSource,resumedBridge.callbacks);
        QCOMPARE(Codec::encode(resumed.acceptedRuntime()),Codec::encode(accepted));
        QFile replayCalls(callsPath);QVERIFY(replayCalls.open(QIODevice::ReadOnly));QCOMPARE(replayCalls.readAll().trimmed().split('\n').size(),countBefore);
        run.certifyAcceptedRuntime(accepted);run.certifyAcceptedRuntime(run.acceptedModel(source));
    }
} REPORT_EXCEPTIONS

QTEST_GUILESS_MAIN(TestPiezoNativeBackendBridge)
#include "tst_piezo_native_backend_bridge.moc"
