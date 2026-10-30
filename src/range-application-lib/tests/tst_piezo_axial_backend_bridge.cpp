#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QCryptographicHash>
#include <QDir>
#include <rml_model.h>
#include <rbl_error.h>
#include <rstructural_piezo_element_operators.h>
#include <rstructural_piezo_axial_operators.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include "piezo_axial_backend_bridge.h"
#include "piezo_numeric_transport.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>
#ifndef RANGE_PIEZO_BACKEND_PYTHON
#error "Axial bridge tests require the selected private Python executable"
#endif
#ifndef RANGE_PIEZO_BACKEND_MODULE_ROOT
#error "Axial bridge tests require the actual range_workflow module root"
#endif
namespace {
using Run=RStructuralPiezoAxialNativeRun;
using Codec=RStructuralPiezoAxialRestartCodec;
using Role=RStructuralPiezoPortRole;
using Mode=RStructuralPiezoReadoutMode;
using Bridge=PiezoAxialBackendBridge;
void component(RBoundaryCondition &bc,RVariableType type,double value,bool enabled=true)
{
    auto &c=bc.getComponent(bc.findComponentPosition(type)); c.setEnabled(enabled); c.clear(); if(enabled)c.add(0,value);
}
RBoundaryCondition support(uint axis,bool fixed,bool transverse=true)
{
    RBoundaryCondition bc(R_BOUNDARY_CONDITION_DISPLACEMENT);
    const RVariableType types[]={R_VARIABLE_DISPLACEMENT_X,R_VARIABLE_DISPLACEMENT_Y,R_VARIABLE_DISPLACEMENT_Z};
    for(uint a=0;a<3;++a) component(bc,types[a],0,fixed || (transverse && a!=axis));
    return bc;
}
RStructuralPiezoAxialMemberProperties material()
{
    auto m=RStructuralPiezoContinuumMaterial::isotropic(100,.25,7);
    m.piezoelectric=true; m.coupling[0]=2;
    m.permittivity[0]=m.permittivity[4]=m.permittivity[8]=.5;
    return rStructuralPiezoCompileAxialMember(m,1,1);
}
RModel fixture(uint axis=0,double q0=.04,bool transverse=true,bool explicitReference=true,
               double tension=1,double voltage=.7)
{
    RModel m; m.addNode(RNode(0,0,0)); m.addNode(axis==0?RNode(1,0,0):RNode(0,1,0));
    m.setNodePhysicalId(0,909); m.setNodePhysicalId(1,101);
    for(uint i=0;i<6;++i) {
        RElement e(R_ELEMENT_TRUSS1); e.setNodeId(0,0); e.setNodeId(1,1);
        m.addElement(e,true,i); m.setElementPhysicalId(i,901+i);
        RMaterial mat(RMaterial::Solid);
        RMaterialProperty young(RMaterialProperty::ModulusOfElasiticity); young.add(293.15,10); mat.add(young);
        RMaterialProperty density(RMaterialProperty::Density); density.add(293.15,1); mat.add(density);
        auto &line=m.getLine(i); line.setCrossArea(1); line.setMaterial(mat);
        if(transverse) line.addBoundaryCondition(support(axis,false));
    }
    for(uint node=0;node<2;++node) {
        RElement e(R_ELEMENT_POINT); e.setNodeId(0,node); m.addElement(e,true,node);
    }
    m.getPoint(0).addBoundaryCondition(support(axis,true));
    RInitialCondition initial(R_INITIAL_CONDITION_DISPLACEMENT);
    const RVariableType qs[]={R_VARIABLE_DISPLACEMENT_X,R_VARIABLE_DISPLACEMENT_Y,R_VARIABLE_DISPLACEMENT_Z};
    for(uint a=0;a<3;++a) {
        auto &c=initial.getComponent(initial.findComponentPosition(qs[a])); c.setEnabled(true); c.clear();
        c.add(0,a==axis?(explicitReference?.02:0):0);
    }
    m.getPoint(1).addInitialCondition(initial);
    const double charge=explicitReference?.03:0;
    const double force=140*q0+2*voltage-.4*(charge-.4*q0)/.5+std::max(0.0,tension+10*q0);
    RBoundaryCondition load(R_BOUNDARY_CONDITION_FORCE);
    const RVariableType fs[]={R_VARIABLE_FORCE_X,R_VARIABLE_FORCE_Y,R_VARIABLE_FORCE_Z};
    for(uint a=0;a<3;++a) component(load,fs[a],a==axis?force:0,a==axis);
    m.getPoint(1).addBoundaryCondition(load);
    for(uint i=0;i<m.getNElementGroups();++i) {
        auto *group=m.getElementGroupPtr(i);
        if(!group || !group->hasEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION)) continue;
        auto &g=group->getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        for(auto type:{R_VARIABLE_G_ACCELERATION_X,R_VARIABLE_G_ACCELERATION_Y,R_VARIABLE_G_ACCELERATION_Z}) {
            auto &c=g.getComponent(g.findComponentPosition(type)); c.setEnabled(true); c.clear(); c.add(0,0);
        }
    }
    m.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS)); m.getProblemSetup().setMassMatrixType(RMassMatrixType::Lumped);
    auto &d=m.getProblemSetup().getStructuralDynamicsSetup(); d.setEnabled(true); d.setTimeStep(.004); d.setNSteps(2); d.setOutputFrequency(1);
    auto &time=m.getTimeSolver(); time.setEnabled(true); time.setInputStartTime(.004); time.setInputTimeStepSize(.004);
    time.setInputNTimeSteps(2); time.setTimes({.004,.008});
    auto &truss=m.getProblemSetup().getStructuralTrussDynamicsSetup(); truss.enabled=true;
    truss.absoluteForceTolerance=1e-11; truss.relativeForceTolerance=1e-11; truss.linearRelativeTolerance=1e-12;
    truss.passiveMembers={{901,1}};
    const double coupling[]={2,-.4,.5,-.6}; const QString ids[]={"actuator","open","short","amplifier"};
    const auto property=material();
    for(uint i=0;i<4;++i) {
        RStructuralTrussActiveMember a; a.physicalElementId=902+i; a.portId=ids[i]; a.referenceLength=1;
        a.axialStiffness=i==0?property.k:10; a.coupling=coupling[i]; a.capacitance=.5;
        a.kind=i==1?RStructuralTrussElectricalKind::ConductorCharge:RStructuralTrussElectricalKind::Voltage;
        a.initialValue=i==0?voltage:(i==1?charge:0);
        if(i==0) { a.massSource=RStructuralTrussMassSource::FixedMemberMass; a.fixedMass=property.fixedMass; }
        truss.activeMembers.push_back(a);
    }
    RStructuralTrussCableMember cable; cable.physicalElementId=906; cable.initialReferenceLength=1; cable.initialTension=tension;
    truss.cableMembers.push_back(cable);
    auto &setup=m.getProblemSetup().getStructuralPiezoReadoutSetup();
    setup.ports={{"open",Role::Sensor,Mode::OpenCircuitVoltage},
                 {"amplifier",Role::Sensor,Mode::IdealChargeAmplifier,.25,-1,.012,1.3},
                 {"actuator",Role::ActuatorSensor,Mode::ActuatorCharge},
                 {"short",Role::Sensor,Mode::ShortCircuitCharge}};
    if(explicitReference) {
        setup.workpoint.available=true;
        RStructuralPiezoNodeWorkpoint node; node.physicalNodeId=101; node.q0[axis]=q0; node.f0[axis]=force;
        setup.workpoint.nodes={node,{909,{{0,0,0}},{{0,0,0}}}};
        setup.workpoint.ports={{"amplifier",0,.6*q0},{"short",0,-.5*q0},
                              {"open",(charge-.4*q0)/.5,charge},{"actuator",voltage,.5*voltage-2*q0}};
    }
    truss.bindToModel(m); return m;
}

QByteArray json(const QJsonObject &o){return QJsonDocument(o).toJson(QJsonDocument::Compact);}
QJsonObject parse(const QByteArray &b){return QJsonDocument::fromJson(b).object();}
QJsonArray rowMatrix(const QJsonArray &row){QJsonArray out;out.append(row);return out;}
double entry(const QJsonValue &v,size_t i,size_t j){return v.toArray()[int(i)].toArray()[int(j)].toDouble();}
void close(double a,double b,double tol=1e-9){if(!std::isfinite(a) || std::abs(a-b)>tol*std::max({1.,std::abs(a),std::abs(b)}))throw std::runtime_error("Axial bridge numeric mismatch");}
bool rejects(const std::function<void()> &f){try{f();}catch(...){return true;}return false;}
void closeVectors(const std::vector<double> &a,const std::vector<double> &b,double tol=1e-9){if(a.size()!=b.size())throw std::runtime_error("Axial bridge vector dimension mismatch");for(size_t i=0;i<a.size();++i)close(a[i],b[i],tol);}
Bridge::Result bridge(const RModel &m){return Bridge::create(m,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT));}
void bindRecipe(RModel &m,QJsonObject recipe){auto &s=m.getProblemSetup().getStructuralPiezoAxialSetup();s.enabled=true;s.recipeConfiguration=json(recipe);s.bindToModel(m);}
RModel scene(const QString &kind="none",uint N=3,uint steps=6,bool multiple=false){
    auto m=fixture();auto &d=m.getProblemSetup().getStructuralDynamicsSetup();d.setNSteps(steps);auto &t=m.getTimeSolver();t.setInputNTimeSteps(steps);std::vector<double> times;for(uint i=1;i<=steps;++i)times.push_back(i*.004);t.setTimes(times);
    if(multiple){auto &ports=m.getProblemSetup().getStructuralPiezoReadoutSetup().ports;for(auto &p:ports)if(p.portId=="short"){p.role=Role::ActuatorSensor;p.readout=Mode::ActuatorCharge;}}
    m.getProblemSetup().getStructuralTrussDynamicsSetup().bindToModel(m);
    QJsonObject controller{{"kind",kind},{"controlEveryFESteps",int(N)}};
    if(kind!="none"){controller["voltageLower"]=QJsonArray{-1.};controller["voltageUpper"]=QJsonArray{1.};controller["deltaVoltageLimits"]=QJsonArray{.04};}
    if(kind=="PI"){controller["kp"]=.3;controller["ki"]=.02;controller["selectedOutputs"]=QJsonArray{"tip:q"};}
    if(kind=="LQR" || kind=="MPC"){controller["Q"]=QJsonArray{QJsonArray{2.,0.},QJsonArray{0.,1.}};controller["R"]=rowMatrix(QJsonArray{.5});}
    if(kind=="MPC"){controller["Qf"]=controller["Q"];controller["horizon"]=3;}
    QJsonArray outputs;for(const auto &q:{QString("displacement"),QString("velocity"),QString("acceleration")})outputs.append(QJsonObject{{"id",q=="displacement"?"tip:q":q=="velocity"?"tip:v":"tip:a"},{"physicalNodeId",101},{"dof","X"},{"quantity",q}});
    QJsonObject recipe{{"family","axialRod"},{"dt",.004},{"steps",int(steps)},
        {"design",QJsonObject{{"outputs",outputs}}},{"controller",controller},{"damping",QJsonObject{{"kind","rayleigh"},{"rayleighAlpha",.06},{"rayleighBeta",.002}}},
        {"excitation",QJsonObject{{"channels",QJsonArray{QJsonObject{{"channelId","node:101:X"},{"unit","N"},{"terms",QJsonArray{QJsonObject{{"kind","expression"},{"expression","0.03+0.2*t"}}}}}}}}}};
    bindRecipe(m,recipe);return m;
}
QJsonObject axialWaveformTerm(const QString &kind){
    if(kind=="constant")return {{"kind",kind},{"value",.17}};
    if(kind=="expression")return {{"kind",kind},{"expression","0.03+0.2*t"}};
    if(kind=="sine")return {{"kind",kind},{"amplitude",.11},{"frequencyHz",25.},{"phaseRad",.3}};
    if(kind=="multisine")return {{"kind",kind},{"components",QJsonArray{QJsonObject{{"amplitude",.06},{"frequencyHz",20.},{"phaseRad",.15}},QJsonObject{{"amplitude",-.025},{"frequencyHz",65.},{"phaseRad",.45}}}}};
    if(kind=="chirp")return {{"kind",kind},{"amplitude",.13},{"startTime",.004},{"duration",.02},{"startFrequencyHz",15.},{"endFrequencyHz",75.},{"phaseRad",.2}};
    if(kind=="smoothPulse")return {{"kind",kind},{"amplitude",.2},{"startTime",.004},{"riseTime",.008},{"holdTime",.004},{"fallTime",.008}};
    if(kind=="table")return {{"kind",kind},{"times",QJsonArray{0.,.008,.016,.024,.032}},{"values",QJsonArray{-.03,.12,-.05,.2,.01}},{"interpolation","linear"},{"outOfRange","hold"}};
    throw std::runtime_error("Unknown axial waveform test case");
}
double independentAxialWaveform(const QString &kind,double t){
    const double pi=std::acos(-1.0);
    if(kind=="constant")return .17;
    if(kind=="expression")return .03+.2*t;
    if(kind=="sine")return .11*std::sin(2*pi*25.*t+.3);
    if(kind=="multisine")return .06*std::sin(2*pi*20.*t+.15)-.025*std::sin(2*pi*65.*t+.45);
    if(kind=="chirp"){
        const double tau=t-.004,duration=.02;
        return tau<0. || tau>duration?0.:.13*std::sin(.2+2*pi*(15.*tau+.5*(75.-15.)*tau*tau/duration));
    }
    if(kind=="smoothPulse"){
        const double tau=t-.004,rise=.008,hold=.004,fall=.008,total=rise+hold+fall;
        const auto smooth=[](double x){return x*x*x*(10.+x*(-15.+6.*x));};
        if(tau<=0. || tau>=total)return 0.;
        if(tau<rise)return .2*smooth(tau/rise);
        if(tau<=rise+hold)return .2;
        return .2*smooth((total-tau)/fall);
    }
    if(kind=="table"){
        const double xs[]={0.,.008,.016,.024,.032},ys[]={-.03,.12,-.05,.2,.01};
        if(t<=xs[0])return ys[0];if(t>=xs[4])return ys[4];
        for(size_t i=0;i<4;++i)if(t<=xs[i+1]){const double ratio=(t-xs[i])/(xs[i+1]-xs[i]);return (1.-ratio)*ys[i]+ratio*ys[i+1];}
    }
    throw std::runtime_error("Unknown independent axial waveform test case");
}
RModel classicalControllerScene(const QString &kind){
    auto m=scene("none",3,4);auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);auto controller=recipe["controller"].toObject();
    const bool velocity=kind=="velocity_feedback";const QString output=velocity?"tip:v":"tip:q";
    controller["kind"]=kind;controller["selectedOutputs"]=QJsonArray{output};controller["reference"]=QJsonArray{velocity?.05:0.};controller["bias"]=QJsonArray{0.};
    controller["voltageLower"]=QJsonArray{-1.};controller["voltageUpper"]=QJsonArray{1.};controller["deltaVoltageLimits"]=QJsonArray{.5};
    if(kind=="velocity_feedback")controller["gain"]=rowMatrix(QJsonArray{.3});
    else if(kind=="PI"){controller["kp"]=rowMatrix(QJsonArray{.4});controller["ki"]=rowMatrix(QJsonArray{4.});}
    else if(kind=="PD"){controller["kp"]=rowMatrix(QJsonArray{.4});controller["kd"]=rowMatrix(QJsonArray{.002});controller["derivativeFilterAlpha"]=.5;}
    else if(kind=="PID"){controller["kp"]=rowMatrix(QJsonArray{.4});controller["ki"]=rowMatrix(QJsonArray{4.});controller["kd"]=rowMatrix(QJsonArray{.002});controller["derivativeFilterAlpha"]=.5;}
    else if(kind=="discrete_state_space"){
        controller["Ac"]=rowMatrix(QJsonArray{.5});controller["Bc"]=rowMatrix(QJsonArray{.8});controller["Cc"]=rowMatrix(QJsonArray{.2});controller["Dc"]=rowMatrix(QJsonArray{.4});
        controller["initialState"]=QJsonArray{.2};controller["samplingPeriod"]=.012;controller["inputUnits"]=QJsonArray{"m"};controller["outputUnits"]=QJsonArray{"V"};
    }
    recipe["controller"]=controller;bindRecipe(m,recipe);return m;
}
// Independent scalar recurrences consume committed FE state; they never call the backend proposal callback.
struct ClassicalControllerOracle
{
    QString kind;
    double reference=0,baseVoltage=.7,gain=.3,kp=.4,ki=4,kd=.002,alpha=.5;
    double Ac=.5,Bc=.8,Cc=.2,Dc=.4,initialControllerState=.2;
    double integral=0,previousError=0,filteredDerivative=0,controllerState=0,controllerStateFromInitial=.2;
    double lastError=0,lastIntegralContribution=0,lastDerivativeContribution=0,lastStateContribution=0,lastAcContribution=0;
    explicit ClassicalControllerOracle(QString controllerKind):kind(std::move(controllerKind)),reference(kind=="velocity_feedback"?.05:0.),controllerState(kind=="discrete_state_space"?initialControllerState:0.),controllerStateFromInitial(kind=="discrete_state_space"?initialControllerState:0.){}
    double propose(double measured,double samplingPeriod){
        const double error=reference-measured;lastError=error;double delta=0;
        if(kind=="velocity_feedback")delta=gain*error;
        else if(kind=="discrete_state_space"){
            const double oldState=controllerState;lastStateContribution=Cc*oldState;lastAcContribution=Cc*controllerStateFromInitial;
            delta=lastStateContribution+Dc*error;controllerState=Ac*oldState+Bc*error;controllerStateFromInitial=Ac*controllerStateFromInitial;
        }else{
            delta=kp*error;
            if(kind=="PI" || kind=="PID"){lastIntegralContribution=ki*integral;delta+=lastIntegralContribution;integral+=samplingPeriod*error;}
            if(kind=="PD" || kind=="PID"){
                const double derivative=(error-previousError)/samplingPeriod;
                filteredDerivative=alpha*filteredDerivative+(1-alpha)*derivative;
                lastDerivativeContribution=kd*filteredDerivative;delta+=lastDerivativeContribution;
            }
        }
        previousError=error;return baseVoltage+delta;
    }
};
double classicalMeasurement(const Run &run,const QJsonObject &exportPackage,const QString &kind){
    if(kind=="velocity_feedback")return run.acceptedState().velocities[3];
    return run.acceptedState().displacements[3]-exportPackage["q0"].toArray()[3].toDouble();
}
void verifyClassicalAction(const RStructuralPiezoAxialRunState &state,double request,double previousApplied){
    if(state.actions.size()!=1 || state.envelopes.size()!=1)throw std::runtime_error("Classical FE test actuator state has wrong dimension");
    const auto &action=state.actions[0];const auto &envelope=state.envelopes[0];
    const double amplitude=std::clamp(request,envelope.minimum,envelope.maximum);
    const double applied=std::clamp(amplitude,previousApplied-envelope.maximumRate*state.dt,previousApplied+envelope.maximumRate*state.dt);
    close(action.requested,request);close(action.amplitudeLimited,amplitude);close(action.previousApplied,previousApplied);close(action.applied,applied);
    close(state.heldRequested[0],request);close(state.lastApplied[0],applied);
    if(action.amplitudeClipped!=(amplitude!=request) || action.rateClipped!=(applied!=amplitude))throw std::runtime_error("Classical FE action limit flags disagree with numerical clamp");
}
void verifyClassicalOpaque(const RStructuralPiezoAxialRunState &state,const ClassicalControllerOracle &oracle,double expectedRequest){
    const auto opaque=parse(state.opaqueControllerState);const auto first=[&](const char *key){const auto values=opaque.value(key).toArray();return values.isEmpty()?0.:values[0].toDouble();};
    close(first("integral"),oracle.integral);close(first("previousError"),oracle.previousError);close(first("filteredDerivative"),oracle.filteredDerivative);
    close(first("controllerState"),oracle.controllerState);close(first("heldDelta"),expectedRequest-oracle.baseVoltage);close(first("previousApplied"),state.lastApplied[0]);
    if(opaque.value("phase").toString()!="ready" || opaque.value("feStep").toDouble()!=state.completedFE || opaque.value("controlTick").toDouble()!=state.controlTick)
        throw std::runtime_error("Classical FE controller opaque counters differ from accepted boundary");
}
void step(Run &run,const RModel &source){auto p=run.prepare(source);run.validatePrepared(*p);run.commit(*p);}
QJsonArray numbers(const double *begin,size_t n){QJsonArray out;for(size_t i=0;i<n;++i)out.append(begin[i]);return out;}
QJsonObject declaredMaterial(QString id,double young,double rho,double coupling,bool piezo){const auto base=RStructuralPiezoContinuumMaterial::isotropic(young,.25,rho);std::array<double,18> g{};std::array<double,9> epsilon{};if(piezo){g[0]=coupling;epsilon[0]=epsilon[4]=epsilon[8]=.5;}
    return {{"materialId",id},{"form","stressCharge"},{"elasticityOrCompliance",numbers(base.elasticity.data(),36)},{"coupling",numbers(g.data(),18)},{"permittivity",numbers(epsilon.data(),9)},{"density",rho},{"piezoelectric",piezo},{"sourceKind","example"},{"sourceId","independent-test-example"},{"sourceSha256",QString(64,'a')}};
}
RModel continuumScene(){
    auto m=fixture(0,.04,true,true,0);auto &native=m.getProblemSetup().getStructuralTrussDynamicsSetup();for(size_t i=1;i<native.activeMembers.size();++i){native.activeMembers[i].massSource=RStructuralTrussMassSource::FixedMemberMass;native.activeMembers[i].fixedMass=1;}
    native.bindToModel(m);QJsonArray materials,members;materials.append(declaredMaterial("passive",10,1,0,false));
    const double coupling[]={2,-.4,.5,-.6};const QString ports[]={"actuator","open","short","amplifier"};
    members.append(QJsonObject{{"physicalElementId",901},{"kind","passive"},{"materialId","passive"},{"area",1}});
    for(uint i=0;i<4;++i){const auto id=QString("piezo%1").arg(i);materials.append(declaredMaterial(id,i==0?100:10,i==0?7:1,coupling[i],true));members.append(QJsonObject{{"physicalElementId",int(902+i)},{"kind","active"},{"materialId",id},{"area",1},{"portId",ports[i]},{"polarity",1}});}
    members.append(QJsonObject{{"physicalElementId",906},{"kind","cable"},{"materialId","passive"},{"area",1}});
    bindRecipe(m,{{"family","axialRod"},{"materials",materials},{"members",members},{"damping",QJsonObject{{"kind","none"}}}});return m;
}
}
#define REPORT_EXCEPTIONS catch(const RError &e){QFAIL(qPrintable(e.getMessage()));}catch(const std::exception &e){QFAIL(e.what());}
class TestPiezoAxialBackendBridge:public QObject {
    Q_OBJECT
private slots:
    void actualOperatorsReadoutsAndAcceleration();
    void multipleVoltageOrder();
    void pureSensingNoActuator();
    void sensorOnlyHiddenState();
    void sensorOnlyObserverEveryDiskPhase_data();
    void sensorOnlyObserverEveryDiskPhase();
    void classicalControllersAcceptedFE_data();
    void classicalControllersAcceptedFE();
    void realDampingAndOptimalControllers_data();
    void realDampingAndOptimalControllers();
    void resealedFutureHeldTarget();
    void everyDiskPhase_data();
    void everyDiskPhase();
    void materialEquivalenceAndNegativeCertificates();
    void importedSupportCrossTermsAndModalC();
    void unsupportedUnitsClockAndInitialVoltage();
    void directlyBoundSourceFrame();
    void builtInWaveformsReachAcceptedAxialFE_data();
    void builtInWaveformsReachAcceptedAxialFE();
    void frozenPackageAndOwnerThread();
    void externalFunctionSharesPhysicalForceColumn();
};
void TestPiezoAxialBackendBridge::actualOperatorsReadoutsAndAcceleration() try {
    const auto m=scene();const auto b=bridge(m);const auto &e=b.exportPackage;const auto op=RStructuralPiezoAxialOperators::compile(m);
    QCOMPARE(e.value("sourceFingerprint").toString(),m.getProblemSetup().getStructuralPiezoAxialSetup().computeSourceFingerprint(m));QCOMPARE(e.value("materialQualification").toString(),QString("effective-native-scalar-source-no-continuum-provenance"));
    QCOMPARE(e.value("outputs").toArray().size(),7);QCOMPARE(e.value("inputs").toArray().size(),1);QCOMPARE(e.value("dofs").toArray().size(),1);
    close(entry(e["K"],0,0),op.free.K[0]);close(entry(e["M"],0,0),op.free.M[0]);close(entry(e["fullK"],3,3),entry(e["fullKmaterial"],3,3)+entry(e["fullKgeometric"],3,3)+entry(e["fullKfixedCharge"],3,3));
    Run run(m,b.callbacks);QCOMPARE(run.system().physicalDamping,b.callbacks.authoritativeFullC);QVERIFY(run.system().physicalDamping[3*6+3]>0);close(run.acceptedState().appliedInputs.externalForces[3],run.adapter().initialCompleteInputs().externalForces[3]+.03);
    size_t acceleration=0;const auto outputs=e["outputs"].toArray();for(int i=0;i<outputs.size();++i)if(outputs[i].toArray()[0].toString()=="tip:a")acceleration=size_t(i);
    close(entry(e["Dforce"],acceleration,0),1/op.free.M[0]);close(entry(e["physicalForceDirectFeedthrough"],acceleration,3),1/op.free.M[0]);
    const auto &physical=run.acceptedState();auto input=physical.appliedInputs;input.externalForces[3]+=.001;const RStructuralTrussDynamics driver(run.system());const auto perturbed=driver.initialize(physical.time,physical.displacements,physical.velocities,input);close((perturbed.accelerations[3]-physical.accelerations[3])/.001,entry(e["Dforce"],acceleration,0),1e-7);
    auto q=physical.displacements;q[3]+=1e-7;const auto moved=driver.initialize(physical.time,q,physical.velocities,physical.appliedInputs);RStructuralPiezoNativeReadout readout(run.adapter(),run.adapter().piezoReadoutSetup(),run.system());const auto left=readout.observe(physical),right=readout.observe(moved);
    for(size_t row=0;row<op.outputs.size();++row){const auto &id=op.outputs[row].portId;const auto l=std::find_if(left.begin(),left.end(),[&](const auto &x){return x.portId==id;}),r=std::find_if(right.begin(),right.end(),[&](const auto &x){return x.portId==id;});close((r->deltaValue-l->deltaValue)/1e-7,op.free.Lq[row],1e-6);}
    QVERIFY(e["privateRuntime"].toObject()["python"].isString());QVERIFY(e["privateRuntime"].toObject()["numpy"].isString());QVERIFY(e["privateRuntime"].toObject()["scipy"].isString());
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::multipleVoltageOrder() try {
    const auto m=scene("none",2,6,true);const auto b=bridge(m);const auto inputs=b.exportPackage["inputs"].toArray();QCOMPARE(inputs.size(),2);const auto op=RStructuralPiezoAxialOperators::compile(m);
    for(int j=0;j<inputs.size();++j)QCOMPARE(inputs[j].toArray()[0].toString(),op.inputs[size_t(j)].portId);
    Run run(m,b.callbacks);step(run,m);QCOMPARE(run.acceptedRuntime().actions.size(),size_t(2));for(size_t i=0;i<2;++i)QCOMPARE(run.acceptedRuntime().actions[i].portId,op.inputs[i].portId);
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::pureSensingNoActuator() try {
    auto m=scene();auto &native=m.getProblemSetup().getStructuralTrussDynamicsSetup();native.activeMembers[0].initialValue=0;
    auto &readout=m.getProblemSetup().getStructuralPiezoReadoutSetup();for(auto &p:readout.ports)if(p.portId=="actuator"){p.role=Role::Sensor;p.readout=Mode::ShortCircuitCharge;}
    for(auto &p:readout.workpoint.ports)if(p.portId=="actuator"){p.V0=0;p.Q0=-2*.04;}
    for(auto &node:readout.workpoint.nodes)if(node.physicalNodeId==101)node.f0[0]-=1.4;
    auto &load=m.getPoint(1).getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);auto &fx=load.getComponent(load.findComponentPosition(R_VARIABLE_FORCE_X));const double force=readout.workpoint.nodes[0].f0[0];fx.clear();fx.add(0,force);native.bindToModel(m);
    const auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);bindRecipe(m,recipe);const auto b=bridge(m);QVERIFY(b.exportPackage["inputs"].toArray().isEmpty());QVERIFY(b.exportPackage["B"].toArray()[0].toArray().isEmpty());Run run(m,b.callbacks);while(run.hasRemainingSteps())step(run,m);QVERIFY(run.acceptedRuntime().lastApplied.empty());QVERIFY(run.acceptedRuntime().actions.empty());QCOMPARE(run.completedSteps(),6u);
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::sensorOnlyHiddenState() try {
    auto m=scene("PI");auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);auto c=recipe["controller"].toObject();c["stateSource"]="sensor_only";c["selectedOutputs"]=QJsonArray{"open"};recipe["controller"]=c;bindRecipe(m,recipe);const auto b=bridge(m);Run run(m,b.callbacks);
    const auto &s=run.acceptedRuntime();const auto expected=b.callbacks.propose(s.physical,s.readouts,0,0,s.opaqueControllerState);auto hidden=s.physical;hidden.displacements.clear();hidden.velocities.assign(19,1e100);hidden.accelerations.clear();const auto actual=b.callbacks.propose(hidden,s.readouts,0,0,s.opaqueControllerState);
    QCOMPARE(actual.requestedVoltages,expected.requestedVoltages);QCOMPARE(actual.nextOpaqueState,expected.nextOpaqueState);QCOMPARE(actual.lastFrameBytes,expected.lastFrameBytes);
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::sensorOnlyObserverEveryDiskPhase_data(){
    QTest::addColumn<QString>("kind");QTest::addColumn<QString>("extension");QTest::addColumn<uint>("N");
    for(const auto &kind:{QString("LQR"),QString("MPC")})for(const auto &ext:{QString("ram"),QString("rbm")})for(uint N:{2u,3u})
        QTest::newRow(qPrintable(QString("%1-%2-N%3").arg(kind,ext).arg(N)))<<kind<<ext<<N;
}
void TestPiezoAxialBackendBridge::sensorOnlyObserverEveryDiskPhase() try {
    QFETCH(QString,kind);QFETCH(QString,extension);QFETCH(uint,N);auto source=scene(kind,N);
    auto &time=source.getTimeSolver();time.setInputStartTime(1.304);
    const double initialTime=time.getInputStartTime()-.004;std::vector<double> times;
    for(uint i=1;i<=6;++i)times.push_back(initialTime+double(i)*.004);time.setTimes(times);
    // Native axial f0 must equal its frozen background. Declare the independent
    // .07 N load offset as known excitation to preserve that certified equilibrium.
    const double background=source.getProblemSetup().getStructuralPiezoReadoutSetup().workpoint.nodes[0].f0[0];
    QVERIFY(background!=0.);source.getProblemSetup().getStructuralTrussDynamicsSetup().bindToModel(source);
    auto recipe=parse(source.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);
    auto excitation=recipe["excitation"].toObject();auto channels=excitation["channels"].toArray();auto channel=channels[0].toObject();
    auto terms=channel["terms"].toArray();terms.append(QJsonObject{{"kind","constant"},{"value",.07}});
    channel["terms"]=terms;channels[0]=channel;excitation["channels"]=channels;recipe["excitation"]=excitation;
    auto controller=recipe["controller"].toObject();controller["stateSource"]="sensor_only";
    controller["selectedOutputs"]=QJsonArray{"open","short","amplifier"};
    controller["observer"]=QJsonObject{
        {"processCovariance",QJsonArray{QJsonArray{.1,0.},QJsonArray{0.,.1}}},
        {"measurementCovariance",QJsonArray{QJsonArray{.01,0.,0.},QJsonArray{0.,.01,0.},QJsonArray{0.,0.,.01}}},
        {"initialPrior",QJsonArray{0.,0.}}};
    recipe["controller"]=controller;bindRecipe(source,recipe);const auto actual=bridge(source);
    QVERIFY(!actual.exportPackage["design"].toObject()["controller"].toObject()["observer"].toObject().isEmpty());
    QCOMPARE(actual.exportPackage["backgroundReducedOffset"].toArray()[0].toDouble(),0.);
        QJsonArray expectedDisturbanceInputs;expectedDisturbanceInputs.append(QJsonArray{"node:101:X","N"});
        QCOMPARE(actual.exportPackage["disturbanceInputs"].toArray(),expectedDisturbanceInputs);
    Run continuous(source,actual.callbacks);QCOMPARE(continuous.acceptedRuntime().initialTime,initialTime);
    QVERIFY(continuous.acceptedState().displacements[3]!=actual.exportPackage["q0"].toArray()[3].toDouble());
    close(actual.callbacks.perturbationForce(initialTime)[3],.07+.03+.2*initialTime);
    close(continuous.acceptedState().appliedInputs.externalForces[3],background+.07+.03+.2*initialTime);
    std::vector<RStructuralPiezoAxialRunState> boundaries;
    for(;;){
        const auto state=continuous.acceptedRuntime();boundaries.push_back(state);
        if(continuous.hasRemainingSteps() && state.controlPhase==0){
            // These samples come from the actual native accepted FE state, not a synthetic plant.
            for(const auto &id:{QString("open"),QString("short"),QString("amplifier")}){
                const auto sample=std::find_if(state.readouts.begin(),state.readouts.end(),[&](const auto &x){return x.portId==id;});
                QVERIFY(sample!=state.readouts.end());QVERIFY(!sample->actionRecord);
            }
            const auto expected=actual.callbacks.propose(state.physical,state.readouts,state.completedFE,state.controlTick,state.opaqueControllerState);
            auto hidden=state.physical;hidden.displacements.clear();hidden.velocities.assign(19,1e100);hidden.accelerations.clear();
            const auto isolated=actual.callbacks.propose(hidden,state.readouts,state.completedFE,state.controlTick,state.opaqueControllerState);
            QCOMPARE(isolated.requestedVoltages,expected.requestedVoltages);QCOMPARE(isolated.nextOpaqueState,expected.nextOpaqueState);QCOMPARE(isolated.lastFrameBytes,expected.lastFrameBytes);
            const auto innovation=parse(expected.lastFrameBytes)["diagnostics"].toObject()["innovation"].toArray();QCOMPARE(innovation.size(),3);
            QVERIFY(std::any_of(innovation.begin(),innovation.end(),[](const auto &x){return x.toDouble()!=0.;}));
        }
        if(!continuous.hasRemainingSteps())break;step(continuous,source);
    }
    QVERIFY(parse(boundaries.back().opaqueControllerState)["observerPrior"]!=parse(boundaries.front().opaqueControllerState)["observerPrior"]);
    if(kind=="MPC")QVERIFY(parse(boundaries.back().opaqueControllerState)["warmStart"]!=parse(boundaries.front().opaqueControllerState)["warmStart"]);
    QVERIFY(continuous.acceptedState().cumulativeDampingDissipation>0);QTemporaryDir directory;QVERIFY(directory.isValid());
    // Includes every held-control phase and the final accepted boundary in both native formats.
    for(uint split=0;split<=6;++split){
        const auto fresh=bridge(source);Run first(source,fresh.callbacks);for(uint i=0;i<split;++i)step(first,source);
        QCOMPARE(Codec::encode(first.acceptedRuntime()),Codec::encode(boundaries[split]));
        auto accepted=first.acceptedModel(source);const auto path=directory.filePath(QString("observer-%1.%2").arg(split).arg(extension));
        const auto written=accepted.nativeWritePaths(path,false).first();accepted.write(path,false);QVERIFY(QFile::exists(written));
        RModel disk;disk.read(written);disk.getProblemSetup().setRestart(true);const auto resumedBackend=bridge(disk);Run resumed(disk,resumedBackend.callbacks);
        for(uint ordinal=split;ordinal<=6;++ordinal){
            const auto &state=resumed.acceptedRuntime(),&reference=boundaries[ordinal];
            QCOMPARE(Codec::encode(state),Codec::encode(reference)); // Complete binary physical/controller history.
            QCOMPARE(state.opaqueControllerState,reference.opaqueControllerState);QCOMPARE(state.lastFrameBytes,reference.lastFrameBytes);
            QCOMPARE(state.initialTime,initialTime);QCOMPARE(state.dt,.004);QCOMPARE(state.completedFE,ordinal);
            QCOMPARE(state.controlPhase,ordinal%N);QCOMPARE(state.controlTick,(ordinal+N-1)/N);
            QCOMPARE(state.physical.time,initialTime+double(ordinal)*.004);
            QCOMPARE(state.physical.displacements,reference.physical.displacements);QCOMPARE(state.physical.velocities,reference.physical.velocities);
            QCOMPARE(state.physical.accelerations,reference.physical.accelerations);QCOMPARE(state.physical.reactions,reference.physical.reactions);
            QCOMPARE(state.physical.appliedInputs.externalForces,reference.physical.appliedInputs.externalForces);
            QCOMPARE(state.physical.appliedInputs.externalForces[3],background+resumedBackend.callbacks.perturbationForce(state.physical.time)[3]);
            QCOMPARE(state.initialPhysicalReference.displacements,boundaries[0].physical.displacements);
            const auto opaque=parse(state.opaqueControllerState);QCOMPARE(opaque["observerPrior"].toArray().size(),2);
            QCOMPARE(opaque["warmStart"].toArray().size(),kind=="MPC"?3:0);QCOMPARE(opaque["previousApplied"].toArray()[0].toDouble(),state.lastApplied[0]);
            if(ordinal<6)step(resumed,disk);
        }
    }
    if(kind=="MPC" && extension=="ram" && N==2){
        // Inject one failure after the real provider has computed postFE for a
        // control boundary, then retry from the unchanged accepted MODEL.
        std::function<bool(const QJsonValue &)> containsNonzero;
        containsNonzero=[&](const QJsonValue &value){
            if(value.isDouble())return value.toDouble()!=0.;
            if(!value.isArray())return false;
            for(const auto &item:value.toArray())if(containsNonzero(item))return true;
            return false;
        };
        const auto changedNonzeroArray=[&](const QJsonValue &before,const QJsonValue &candidate){
            if(!before.isArray() || !candidate.isArray() || before==candidate)return false;
            return containsNonzero(candidate);
        };
        uint failedFE=0;
        for(uint ordinal=1;ordinal<=6 && failedFE==0;++ordinal){
            const auto before=parse(boundaries[ordinal-1].opaqueControllerState);
            const auto candidate=parse(boundaries[ordinal].opaqueControllerState);
            bool allChanged=true;
            for(const char *field:{"observerPrior","warmStart","previousApplied"})
                allChanged=allChanged && changedNonzeroArray(before.value(field),candidate.value(field));
            if(allChanged)failedFE=ordinal;
        }
        QVERIFY(failedFE>0);
        auto faultCallbacks=actual.callbacks;
        const auto providerPropose=actual.callbacks.propose;
        QByteArray failedFrameBytes;
        faultCallbacks.propose=[&](const auto &physical,const auto &samples,uint completed,uint tick,const QByteArray &opaque){
            auto candidate=providerPropose(physical,samples,completed,tick,opaque);
            failedFrameBytes=candidate.lastFrameBytes;
            return candidate;
        };
        const auto providerPostFE=actual.callbacks.postFE;
        bool faultArmed=false;uint injectedFailures=0;
        RStructuralTrussDynamicsState failedPhysical;
        std::vector<RStructuralPiezoNativeReadoutSample> failedReadouts;
        std::vector<double> failedApplied;
        QByteArray failedProviderOpaque;
        uint failedTick=0,failedCompleted=0;
        faultCallbacks.postFE=[&,providerPostFE](const auto &physical,const auto &samples,uint completed,uint tick,const auto &applied,const QByteArray &opaque){
            const auto candidate=providerPostFE(physical,samples,completed,tick,applied,opaque);
            if(faultArmed && completed==failedFE){
                faultArmed=false;++injectedFailures;failedPhysical=physical;failedReadouts=samples;
                failedApplied=applied;failedProviderOpaque=candidate;failedTick=tick;failedCompleted=completed;
                throw std::runtime_error("PC08-A03 injected true-provider postFE failure");
            }
            return candidate;
        };
        Run retryable(source,faultCallbacks);while(retryable.completedSteps()+1<failedFE)step(retryable,source);
        QCOMPARE(Codec::encode(retryable.acceptedRuntime()),Codec::encode(boundaries[failedFE-1]));
        const auto acceptedBefore=Codec::encode(retryable.acceptedRuntime());
        const auto acceptedModelBytes=[&](const QString &name){
            auto model=retryable.acceptedModel(source);const auto path=directory.filePath(name+".ram");
            const auto written=model.nativeWritePaths(path,false).first();model.write(path,false);
            QFile file(written);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read serialized accepted axial MODEL");
            return file.readAll();
        };
        const auto modelBefore=acceptedModelBytes("pc08-retry-before");
        const auto beforeOpaque=parse(retryable.acceptedRuntime().opaqueControllerState);
        faultArmed=true;bool exactInjectedException=false;
        try{(void)retryable.prepare(source);}catch(const std::exception &e){
            exactInjectedException=QString::fromUtf8(e.what())=="PC08-A03 injected true-provider postFE failure";
        }
        QVERIFY(exactInjectedException);QCOMPARE(injectedFailures,1u);QVERIFY(!faultArmed);
        QCOMPARE(failedCompleted,failedFE);QCOMPARE(failedTick,boundaries[failedFE].controlTick);
        QCOMPARE(failedReadouts.size(),boundaries[failedFE].readouts.size());QCOMPARE(failedApplied,boundaries[failedFE].lastApplied);
        QCOMPARE(failedPhysical.displacements,boundaries[failedFE].physical.displacements);
        QCOMPARE(failedPhysical.velocities,boundaries[failedFE].physical.velocities);
        QCOMPARE(failedPhysical.accelerations,boundaries[failedFE].physical.accelerations);
        QCOMPARE(failedPhysical.reactions,boundaries[failedFE].physical.reactions);
        QCOMPARE(failedProviderOpaque,boundaries[failedFE].opaqueControllerState);
        QCOMPARE(failedFrameBytes,boundaries[failedFE].lastFrameBytes);
        const auto failedOpaque=parse(failedProviderOpaque);
        for(const char *field:{"observerPrior","warmStart","previousApplied"})
            QVERIFY2(changedNonzeroArray(beforeOpaque.value(field),failedOpaque.value(field)),field);
        QCOMPARE(retryable.completedSteps(),failedFE-1);QCOMPARE(Codec::encode(retryable.acceptedRuntime()),acceptedBefore);
        QCOMPARE(acceptedModelBytes("pc08-retry-after-fault"),modelBefore);

        // The provider has no accepted-step side effect: the identical request
        // returns the same full native/controller/frame candidate on retry.
        const auto retried=retryable.prepare(source);QCOMPARE(Codec::encode(retried->runtime()),Codec::encode(boundaries[failedFE]));
        QCOMPARE(retried->runtime().opaqueControllerState,failedProviderOpaque);
        QCOMPARE(retried->runtime().lastFrameBytes,failedFrameBytes);
        QCOMPARE(retried->result().displacements,boundaries[failedFE].physical.displacements);
        QCOMPARE(retried->result().velocities,boundaries[failedFE].physical.velocities);
        QCOMPARE(retried->result().accelerations,boundaries[failedFE].physical.accelerations);
        retryable.validatePrepared(*retried);retryable.commit(*retried);
        QCOMPARE(Codec::encode(retryable.acceptedRuntime()),Codec::encode(boundaries[failedFE]));
        while(retryable.hasRemainingSteps())step(retryable,source);
        QCOMPARE(Codec::encode(retryable.acceptedRuntime()),Codec::encode(boundaries.back()));
        QCOMPARE(retryable.acceptedRuntime().lastFrameBytes,boundaries.back().lastFrameBytes);
    }
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::classicalControllersAcceptedFE_data(){
    QTest::addColumn<QString>("kind");QTest::addColumn<QString>("stateField");
    QTest::newRow("velocity-feedback")<<QString("velocity_feedback")<<QString("previousError");
    QTest::newRow("PI")<<QString("PI")<<QString("integral");
    QTest::newRow("PD")<<QString("PD")<<QString("filteredDerivative");
    QTest::newRow("PID")<<QString("PID")<<QString("integral");
    QTest::newRow("discrete-state-space")<<QString("discrete_state_space")<<QString("controllerState");
}
void TestPiezoAxialBackendBridge::classicalControllersAcceptedFE() try {
    QFETCH(QString,kind);QFETCH(QString,stateField);const auto m=classicalControllerScene(kind);const auto b=bridge(m);Run run(m,b.callbacks);ClassicalControllerOracle oracle(kind);
    oracle.baseVoltage=run.acceptedRuntime().lastApplied[0];const double samplingPeriod=run.acceptedRuntime().nominalControlTs;close(samplingPeriod,.012);
    const double initialTip=run.acceptedState().displacements[3],firstRequest=oracle.propose(classicalMeasurement(run,b.exportPackage,kind),samplingPeriod);
    const auto initialRuntime=Codec::encode(run.acceptedRuntime());auto prepared=run.prepare(m);QCOMPARE(Codec::encode(run.acceptedRuntime()),initialRuntime);
    auto retry=run.prepare(m);QCOMPARE(Codec::encode(prepared->runtime()),Codec::encode(retry->runtime()));run.validatePrepared(*retry);run.commit(*retry);

    const auto first=run.acceptedRuntime();QCOMPARE(first.completedFE,1u);QCOMPARE(first.controlTick,1u);QCOMPARE(first.controlPhase,1u);
    QVERIFY(std::abs(first.physical.displacements[3]-initialTip)>1e-14);verifyClassicalAction(first,firstRequest,oracle.baseVoltage);verifyClassicalOpaque(first,oracle,firstRequest);
    QVERIFY(std::abs(firstRequest)>1e-9);QVERIFY(std::abs(first.actions[0].applied)>1e-9);QVERIFY(!first.actions[0].amplitudeClipped);QVERIFY(!first.actions[0].rateClipped);
    const auto firstOpaque=parse(first.opaqueControllerState);const auto evolved=firstOpaque.value(stateField).toArray();QVERIFY(!evolved.isEmpty());
    bool stateAdvanced=false;for(const auto &value:evolved)stateAdvanced=stateAdvanced || std::abs(value.toDouble())>1e-12;QVERIFY(stateAdvanced);

    double previousApplied=first.lastApplied[0];
    for(uint completed=2;completed<=3;++completed){
        step(run,m);const auto &accepted=run.acceptedRuntime();QCOMPARE(accepted.completedFE,completed);QCOMPARE(accepted.controlTick,1u);
        verifyClassicalAction(accepted,firstRequest,previousApplied);previousApplied=accepted.lastApplied[0];
        const auto opaque=parse(accepted.opaqueControllerState);
        for(const char *key:{"integral","previousError","filteredDerivative","controllerState","heldDelta"})
            if(opaque.value(key)!=firstOpaque.value(key))throw std::runtime_error("Controller hidden state advanced between FE control ticks");
    }

    const double secondRequest=oracle.propose(classicalMeasurement(run,b.exportPackage,kind),samplingPeriod);
    double requestWithoutIntegral=secondRequest,requestWithoutDerivative=secondRequest,requestWithoutState=secondRequest,requestWithoutAc=secondRequest;
    if(kind=="PI" || kind=="PID")QVERIFY(std::abs(oracle.lastIntegralContribution)>1e-5);
    if(kind=="PD" || kind=="PID")QVERIFY(std::abs(oracle.lastDerivativeContribution)>1e-5);
    if(kind=="discrete_state_space"){
        QVERIFY(std::abs(oracle.lastStateContribution)>1e-3);QVERIFY(std::abs(oracle.lastAcContribution)>1e-4);
        requestWithoutState-=oracle.lastStateContribution;requestWithoutAc-=oracle.lastAcContribution;
    }
    if(kind=="PI" || kind=="PID")requestWithoutIntegral-=oracle.lastIntegralContribution;
    if(kind=="PD" || kind=="PID")requestWithoutDerivative-=oracle.lastDerivativeContribution;
    const auto beforeSecond=Codec::encode(run.acceptedRuntime());auto preparedSecond=run.prepare(m);QCOMPARE(Codec::encode(run.acceptedRuntime()),beforeSecond);
    auto retrySecond=run.prepare(m);QCOMPARE(Codec::encode(preparedSecond->runtime()),Codec::encode(retrySecond->runtime()));run.validatePrepared(*retrySecond);run.commit(*retrySecond);
    const auto second=run.acceptedRuntime();QCOMPARE(second.completedFE,4u);QCOMPARE(second.controlTick,2u);QCOMPARE(second.controlPhase,1u);
    verifyClassicalAction(second,secondRequest,previousApplied);verifyClassicalOpaque(second,oracle,secondRequest);
    QVERIFY(std::abs(secondRequest)>1e-9);QVERIFY(!second.actions[0].amplitudeClipped);QVERIFY(!second.actions[0].rateClipped);
    if(kind=="PI" || kind=="PID")QVERIFY(std::abs(second.actions[0].requested-requestWithoutIntegral)>1e-5);
    if(kind=="PD" || kind=="PID")QVERIFY(std::abs(second.actions[0].requested-requestWithoutDerivative)>1e-5);
    if(kind=="discrete_state_space"){QVERIFY(std::abs(second.actions[0].requested-requestWithoutState)>1e-3);QVERIFY(std::abs(second.actions[0].requested-requestWithoutAc)>1e-4);}
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::realDampingAndOptimalControllers_data(){QTest::addColumn<QString>("kind");for(const auto &kind:{QString("PI"),QString("LQR"),QString("MPC")})QTest::newRow(qPrintable(kind))<<kind;}
void TestPiezoAxialBackendBridge::realDampingAndOptimalControllers() try {
    QFETCH(QString,kind);const auto m=scene(kind);const auto b=bridge(m);Run run(m,b.callbacks);const auto undamped=run.adapter().driver().systemIdentity();QVERIFY(run.acceptedState().systemIdentity!=undamped);
    while(run.hasRemainingSteps())step(run,m);QCOMPARE(run.acceptedRuntime().controlTick,2u);QVERIFY(run.acceptedState().cumulativeDampingDissipation>0);QCOMPARE(parse(run.acceptedRuntime().opaqueControllerState)["feStep"].toInt(),6);const auto previous=parse(run.acceptedRuntime().opaqueControllerState)["previousApplied"].toArray();QCOMPARE(previous.size(),1);QCOMPARE(previous[0].toDouble(),run.acceptedRuntime().lastApplied[0]);
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::resealedFutureHeldTarget() try {
    auto m=scene("PI");auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);auto controller=recipe["controller"].toObject();controller["bias"]=QJsonArray{.9};recipe["controller"]=controller;bindRecipe(m,recipe);const auto b=bridge(m);Run run(m,b.callbacks);const double previousApplied=run.acceptedRuntime().lastApplied[0];const double measured=run.acceptedState().displacements[3]-b.exportPackage["q0"].toArray()[3].toDouble();step(run,m);
    const auto first=run.acceptedRuntime();const double requested=previousApplied+.9+.3*(0.-measured);
    verifyClassicalAction(first,requested,previousApplied);QVERIFY(first.actions[0].requested>first.envelopes[0].maximum);QVERIFY(first.actions[0].amplitudeClipped);QVERIFY(first.actions[0].rateClipped);close(first.actions[0].amplitudeLimited,1.);close(first.actions[0].applied,previousApplied+first.envelopes[0].maximumRate*first.dt);
    auto tampered=first;tampered.heldRequested[0]-=.01;tampered.actions[0].requested=tampered.heldRequested[0];QVERIFY(tampered.heldRequested[0]>tampered.envelopes[0].maximum);
    auto restart=run.acceptedModel(m);restart.getProblemSetup().getStructuralPiezoAxialRestart().runtimeState=Codec::encode(tampered);restart.getProblemSetup().setRestart(true);const auto restoredBridge=bridge(restart);QVERIFY(rejects([&]{Run invalid(restart,restoredBridge.callbacks);}));
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::everyDiskPhase_data(){QTest::addColumn<QString>("extension");QTest::addColumn<uint>("N");for(const auto &ext:{QString("ram"),QString("rbm")})for(uint N:{2u,3u})QTest::newRow(qPrintable(ext+QString::number(N)))<<ext<<N;}
void TestPiezoAxialBackendBridge::everyDiskPhase() try {
    QFETCH(QString,extension);QFETCH(uint,N);const auto m=scene("PI",N);const auto b=bridge(m);Run baseline(m,b.callbacks);while(baseline.hasRemainingSteps())step(baseline,m);const auto expected=Codec::encode(baseline.acceptedRuntime());QTemporaryDir dir;QVERIFY(dir.isValid());
    for(uint split=0;split<=6;++split){Run first(m,b.callbacks);for(uint i=0;i<split;++i)step(first,m);auto model=first.acceptedModel(m);const auto path=dir.filePath(QString("split-%1.%2").arg(split).arg(extension));const auto written=model.nativeWritePaths(path,false).first();model.write(path,false);RModel disk;disk.read(written);disk.getProblemSetup().setRestart(true);const auto restoredBridge=bridge(disk);Run restored(disk,restoredBridge.callbacks);QCOMPARE(Codec::encode(restored.acceptedRuntime()),Codec::encode(first.acceptedRuntime()));while(restored.hasRemainingSteps())step(restored,disk);QCOMPARE(Codec::encode(restored.acceptedRuntime()),expected);
        auto tampered=restored.acceptedRuntime();auto opaque=parse(tampered.opaqueControllerState);opaque["previousApplied"]=QJsonArray{tampered.lastApplied[0]+.01};tampered.opaqueControllerState=json(opaque);QVERIFY(rejects([&]{restoredBridge.callbacks.validateAcceptedBoundary(tampered);}));
    }
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::materialEquivalenceAndNegativeCertificates() try {
    const auto m=continuumScene();const auto b=bridge(m);QCOMPARE(b.exportPackage["materialQualification"].toString(),QString("declared-continuum-tensors-certified-to-native-effective-laws-and-fixed-mass"));QCOMPARE(b.exportPackage["materials"].toArray().size(),5);
    for(uint which=0;which<5;++which){auto bad=m;auto recipe=parse(bad.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);if(which<3){auto members=recipe["members"].toArray();auto member=members[1].toObject();if(which==0)member["area"]=1.1;if(which==1)member["polarity"]=-1;if(which==2)member["portId"]="wrong";members[1]=member;recipe["members"]=members;}else{auto materials=recipe["materials"].toArray();auto material=materials[1].toObject();if(which==3)material["density"]=7.1;else{auto g=material["coupling"].toArray();g[0]=2.1;material["coupling"]=g;}materials[1]=material;recipe["materials"]=materials;}bindRecipe(bad,recipe);QVERIFY(rejects([&]{bridge(bad);}));}
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::importedSupportCrossTermsAndModalC() try {
    auto m=scene();auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);QJsonArray rows;for(uint i=0;i<6;++i){QJsonArray row;for(uint j=0;j<6;++j)row.append(i==j?.2:((i==0 && j==3)||(i==3 && j==0))?.05:0);rows.append(row);}recipe["damping"]=QJsonObject{{"kind","imported"},{"imported_C",rows}};bindRecipe(m,recipe);auto b=bridge(m);QCOMPARE(entry(b.exportPackage["fullC"],0,3),.05);QCOMPARE(entry(b.exportPackage["C"],0,0),.2);Run imported(m,b.callbacks);QCOMPARE(imported.system().physicalDamping[3],.05);
    for(const auto &kind:{QString("uniformModal"),QString("perMode"),QString("perCluster")}){QJsonObject damping{{"kind",kind}};if(kind=="uniformModal")damping["uniformRatio"]=.03;else damping["ratios"]=QJsonArray{.03};if(kind=="perCluster")damping["clusterEnds"]=QJsonArray{1};recipe["damping"]=damping;bindRecipe(m,recipe);b=bridge(m);const double mass=entry(b.exportPackage["M"],0,0),stiffness=entry(b.exportPackage["K"],0,0);close(entry(b.exportPackage["C"],0,0),.06*std::sqrt(mass*stiffness));Run modal(m,b.callbacks);QVERIFY(modal.system().physicalDamping[3*6+3]>0);}
    rows[0]=QJsonArray{-1.,0.,0.,0.,0.,0.};recipe["damping"]=QJsonObject{{"kind","imported"},{"imported_C",rows}};bindRecipe(m,recipe);QVERIFY(rejects([&]{bridge(m);}));
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::unsupportedUnitsClockAndInitialVoltage() try {
    for(uint which=0;which<6;++which){auto m=scene();auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);
        if(which<2){auto design=recipe["design"].toObject();auto outputs=design["outputs"].toArray();auto o=outputs[0].toObject();if(which==0)o["dof"]="RX";else o["unit"]="mm";outputs[0]=o;design["outputs"]=outputs;recipe["design"]=design;}
        if(which==2)recipe["dt"]=.003;if(which==3)recipe["steps"]=7;
        if(which==4){auto controller=recipe["controller"].toObject();controller["kind"]="discrete_state_space";controller["Ac"]=rowMatrix(QJsonArray{.5});controller["Bc"]=rowMatrix(QJsonArray{1.});controller["Cc"]=rowMatrix(QJsonArray{1.});controller["Dc"]=rowMatrix(QJsonArray{0.});controller["samplingPeriod"]=.013;controller["selectedOutputs"]=QJsonArray{"tip:q"};controller["inputUnits"]=QJsonArray{"m"};controller["outputUnits"]=QJsonArray{"V"};recipe["controller"]=controller;}
        if(which==5){auto &native=m.getProblemSetup().getStructuralTrussDynamicsSetup();native.activeMembers[0].initialValue=.71;native.bindToModel(m);}
        bindRecipe(m,recipe);QVERIFY(rejects([&]{bridge(m);}));
    }
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::directlyBoundSourceFrame() try {
    auto m=scene();auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);
    QVERIFY(!recipe.contains("frame"));const auto legacy=bridge(m);
    const QJsonObject identity{{"origin",QJsonArray{0,0,0}},{"ex",QJsonArray{1,0,0}},{"ey",QJsonArray{0,1,0}},{"ez",QJsonArray{0,0,1}}};
    recipe["frame"]=identity;bindRecipe(m,recipe);const auto explicitIdentity=bridge(m);
    QCOMPARE(explicitIdentity.exportPackage["sourceFrame"].toObject()["frame"].toObject(),identity);
    QCOMPARE(explicitIdentity.exportPackage["M"],legacy.exportPackage["M"]);
    QCOMPARE(explicitIdentity.exportPackage["K"],legacy.exportPackage["K"]);
    for(uint which=0;which<6;++which){auto bad=m;auto declaration=recipe;auto frame=identity;
        if(which==0)frame["origin"]=QJsonArray{.1,0,0};
        if(which==1){frame["ex"]=QJsonArray{0,1,0};frame["ey"]=QJsonArray{-1,0,0};}
        if(which==2)frame["ex"]=QJsonArray{1,0};
        if(which==3)frame["ey"]=QJsonArray{0,"1",0};
        if(which==4)frame.remove("ez");
        if(which==5)frame["userDirection"]=QJsonArray{1,0,0};
        declaration["frame"]=frame;bindRecipe(bad,declaration);
        // Unusable runtime paths prove the source guard precedes provider setup.
        // A transport/path error would not satisfy this specific diagnostic.
        bool frameRejected=false;try{Bridge::create(bad,QString(),QString());}catch(const std::exception &e){frameRejected=QString::fromUtf8(e.what()).contains("Axial recipe frame must be exact global identity");}
        QVERIFY2(frameRejected,qPrintable(QString("Directly bound invalid frame case %1 reached provider setup").arg(which)));
    }
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::builtInWaveformsReachAcceptedAxialFE_data(){
    QTest::addColumn<QString>("kind");
    for(const auto &kind:{QString("constant"),QString("expression"),QString("sine"),QString("multisine"),QString("chirp"),QString("smoothPulse"),QString("table")})
        QTest::newRow(qPrintable(kind))<<kind;
}
void TestPiezoAxialBackendBridge::builtInWaveformsReachAcceptedAxialFE() try {
    QFETCH(QString,kind);
    auto m=scene("PI",3,6);auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);
    auto excitation=recipe["excitation"].toObject();auto channels=excitation["channels"].toArray();QCOMPARE(channels.size(),1);
    auto channel=channels[0].toObject();channel["terms"]=QJsonArray{axialWaveformTerm(kind)};channels[0]=channel;excitation["channels"]=channels;recipe["excitation"]=excitation;bindRecipe(m,recipe);

    const auto b=bridge(m);Run run(m,b.callbacks);const auto baseForce=run.adapter().initialCompleteInputs().externalForces;
    const auto initial=run.acceptedRuntime().initialTime;const auto dt=run.acceptedRuntime().dt;
    const auto expectedForce=[&](double t){std::vector<double> values(baseForce.size(),0.);values[3]=independentAxialWaveform(kind,t);return values;};
    const RStructuralTrussDynamics directDriver(run.system());std::vector<double> heldRequested,heldApplied;bool sourceChangeRejected=false;
    for(uint ordinal=1;ordinal<=6;++ordinal){
        const double t0=initial+double(ordinal-1)*dt,t1=initial+double(ordinal)*dt;
        const auto force0=b.callbacks.perturbationForce(t0),force1=b.callbacks.perturbationForce(t1);
        closeVectors(force0,expectedForce(t0),2e-11);closeVectors(force1,expectedForce(t1),2e-11);

        const auto before=run.acceptedState();
        auto prepared=run.prepare(m);const auto &candidate=prepared->result();const auto &runtime=prepared->runtime();
        if((ordinal-1)%3==0){heldRequested=runtime.heldRequested;heldApplied=runtime.lastApplied;}
        else{closeVectors(runtime.heldRequested,heldRequested);closeVectors(runtime.lastApplied,heldApplied);}
        QCOMPARE(runtime.controlTick,(ordinal+2)/3);

        auto directInputs=before.appliedInputs;directInputs.memberInputs=candidate.appliedInputs.memberInputs;
        directInputs.externalForces=baseForce;directInputs.externalForces[3]+=independentAxialWaveform(kind,t0);
        directInputs.rightExternalForces=baseForce;directInputs.rightExternalForces[3]+=independentAxialWaveform(kind,t1);
        const auto direct=directDriver.trialStepOnGrid(before,directInputs,initial,ordinal,dt,run.adapter().options());
        closeVectors(candidate.displacements,direct.candidate.displacements);closeVectors(candidate.velocities,direct.candidate.velocities);
        closeVectors(candidate.accelerations,direct.candidate.accelerations);
        closeVectors(candidate.appliedInputs.externalForces,direct.candidate.appliedInputs.externalForces);
        closeVectors(candidate.appliedInputs.externalForces,directInputs.rightExternalForces);
        QVERIFY(candidate.appliedInputs.rightExternalForces.empty());

        double independentExternalWork=0.;const double h=direct.candidate.time-before.time;
        for(size_t i=0;i<before.displacements.size();++i)
            independentExternalWork+=.5*(directInputs.externalForces[i]+directInputs.rightExternalForces[i])*(direct.candidate.displacements[i]-before.displacements[i]);
        close(direct.stepWork.externalWork,independentExternalWork,1e-8);
        const auto &damping=run.system().physicalDamping;const size_t ndof=before.velocities.size();double independentDamping=0.;
        for(size_t i=0;i<ndof;++i)for(size_t j=0;j<ndof;++j)
            independentDamping+=.25*h*(before.velocities[i]+direct.candidate.velocities[i])*damping[i*ndof+j]*(before.velocities[j]+direct.candidate.velocities[j]);
        close(direct.stepWork.dampingDissipation,independentDamping,1e-8);
        QVERIFY(std::abs(direct.energyBalanceError)<1e-8);
        run.validatePrepared(*prepared);run.commit(*prepared);
        const auto &accepted=run.acceptedState();
        closeVectors(accepted.displacements,direct.candidate.displacements);closeVectors(accepted.velocities,direct.candidate.velocities);
        closeVectors(accepted.accelerations,direct.candidate.accelerations);
        close(accepted.storedEnergy,direct.candidate.storedEnergy);close(accepted.kineticEnergy,direct.candidate.kineticEnergy);
        close(accepted.cumulativeExternalWork-before.cumulativeExternalWork,direct.stepWork.externalWork,1e-8);
        close(accepted.cumulativeDampingDissipation-before.cumulativeDampingDissipation,direct.stepWork.dampingDissipation,1e-8);
        close(accepted.cumulativeExternalWork,direct.candidate.cumulativeExternalWork);
        close(accepted.cumulativeDampingDissipation,direct.candidate.cumulativeDampingDissipation);
        QVERIFY(accepted.cumulativeDampingDissipation>0.);
        if(ordinal==3){
            const auto acceptedBytes=Codec::encode(run.acceptedRuntime());auto changed=m;
            auto changedRecipe=parse(changed.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);
            auto controller=changedRecipe["controller"].toObject();controller["kp"]=.31;changedRecipe["controller"]=controller;bindRecipe(changed,changedRecipe);
            QVERIFY(rejects([&]{run.prepare(changed);}));QCOMPARE(Codec::encode(run.acceptedRuntime()),acceptedBytes);sourceChangeRejected=true;
        }
    }
    QCOMPARE(run.completedSteps(),6u);QVERIFY(sourceChangeRejected);
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::externalFunctionSharesPhysicalForceColumn() try {
    auto m=scene();const auto initial=bridge(m);const auto runtime=initial.exportPackage["privateRuntime"].toObject();QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("source_force.py");const QByteArray code="def force(t):\n    return {'node:101:X': {'value': 0.02+0.01*t, 'unit': 'N'}}\n";QFile source(path);QVERIFY(source.open(QIODevice::WriteOnly));QCOMPARE(source.write(code),qint64(code.size()));source.close();
    auto recipe=parse(m.getProblemSetup().getStructuralPiezoAxialSetup().recipeConfiguration);auto excitation=recipe["excitation"].toObject();excitation["externalFunctions"]=QJsonArray{QJsonObject{{"sourcePath",path},{"functionName","force"},{"sourceSha256",QString::fromLatin1(QCryptographicHash::hash(code,QCryptographicHash::Sha256).toHex())},{"dependencyFiles",QJsonArray{}},{"environment",QJsonObject{{"pythonVersion",runtime["python"]},{"numpyVersion",runtime["numpy"]},{"scipyVersion",runtime["scipy"]}}},{"channels",QJsonArray{QJsonObject{{"channelId","node:101:X"},{"unit","N"}}}},{"missingPolicy","error"},{"cacheExactTime",false}}};recipe["excitation"]=excitation;bindRecipe(m,recipe);const auto b=bridge(m);QCOMPARE(b.exportPackage["disturbanceInputs"].toArray().size(),1);close(b.callbacks.perturbationForce(.02)[3],.05+.21*.02);Run run(m,b.callbacks);step(run,m);
    QVERIFY(source.open(QIODevice::Append));source.write("\n# changed source\n");source.close();QVERIFY(rejects([&]{b.callbacks.perturbationForce(.024);}));
} REPORT_EXCEPTIONS
void TestPiezoAxialBackendBridge::frozenPackageAndOwnerThread() try {
    QTemporaryDir dir;QVERIFY(dir.isValid());const auto package=dir.filePath("range_workflow");QVERIFY(QDir().mkpath(package));const QDir original(QDir(QString::fromUtf8(RANGE_PIEZO_BACKEND_MODULE_ROOT)).filePath("range_workflow"));for(const auto &name:original.entryList({"*.py"},QDir::Files))QVERIFY(QFile::copy(original.filePath(name),QDir(package).filePath(name)));
    auto m=scene();const auto b=Bridge::create(m,QString::fromUtf8(RANGE_PIEZO_BACKEND_PYTHON),dir.path());Run run(m,b.callbacks);bool ownerRejected=false;std::thread worker([&]{ownerRejected=rejects([&]{b.callbacks.perturbationForce(.004);});});worker.join();QVERIFY(ownerRejected);QFile stale(QDir(package).filePath("piezo_native_backend.py"));
#ifdef Q_OS_WIN
    const auto frozenIdentity=b.callbacks.backendIdentity;
    QVERIFY(!stale.open(QIODevice::Append));
    QVERIFY(!rejects([&]{b.callbacks.perturbationForce(.004);}));
    QCOMPARE(b.callbacks.backendIdentity,frozenIdentity);
#else
    QVERIFY(stale.open(QIODevice::Append));stale.write("\n# stale source\n");stale.close();QVERIFY(rejects([&]{b.callbacks.perturbationForce(.004);}));
#endif
    const auto runtime=PiezoNumericTransport::defaultRuntime();QVERIFY(!runtime.first.isEmpty() && !runtime.second.isEmpty());
} REPORT_EXCEPTIONS
QTEST_GUILESS_MAIN(TestPiezoAxialBackendBridge)
#include "tst_piezo_axial_backend_bridge.moc"
