#include "piezo_runtime_identity.h"
#include "piezo_axial_backend_bridge.h"
#include "piezo_native_backend_bridge.h"
#include <rml_model.h>
#include <rbl_error.h>
#include <rstructural_truss_native_run.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <stdexcept>

namespace {
bool layeredBackendRequired(const RModel &m)
{
    const auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();
    const auto d=s.damping.kind;
    return !s.excitationConfiguration.isEmpty() || !s.controllerConfiguration.isEmpty()
        || !s.designConfiguration.isEmpty() || d==RStructuralPiezoDampingKind::UniformModal
        || d==RStructuralPiezoDampingKind::PerMode || d==RStructuralPiezoDampingKind::PerCluster;
}
QByteArray hash(const QByteArray &bytes) {return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
QByteArray control(const QString &identity,bool proposes)
{
    if(proposes && identity.isEmpty())throw std::invalid_argument("Numerical backend callback has no compatibility identity");
    return hash(proposes?identity.toUtf8():QByteArrayLiteral("range.native-checkpoint.no-control.v1"));
}
template<class T> void vector(QDataStream &out,const std::vector<T> &values)
{
    out << quint64(values.size());for(const auto &value:values)out << value;
}
QByteArray legacyStateIdentity(const RStructuralDynamicsRestartData &s)
{
    QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_0);
    out << s.available << s.completedSteps << s.time << s.fixedTimeStep << s.meshNodeCount
        << s.meshElementCount << s.physicalDofCount << s.meshFingerprint << s.dofFingerprint << s.systemFingerprint;
    vector(out,s.q);vector(out,s.v);vector(out,s.a);vector(out,s.externalForce);
    out << s.cumulativeExternalWork << s.cumulativeDampingDissipation << s.initialMechanicalEnergy
        << s.supportResponseAvailable << s.supportFingerprint;
    vector(out,s.supportReactions);vector(out,s.supportPowers);vector(out,s.cumulativeSupportWork);
    out << s.controlAvailable << s.controlProtocolVersion << s.controlFingerprint;
    vector(out,s.controlChannelIds);vector(out,s.controlLastCommands);vector(out,s.controlPowers);
    vector(out,s.cumulativeControlWork);out << s.controlControllerState << s.controlDeterministicSeed;return hash(bytes);
}
QByteArray mixedStateIdentity(const RStructuralTrussDynamicsRestartData &s,const RStructuralNativeControlState &c)
{
    QByteArray bytes;QDataStream out(&bytes,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_0);
    out << s.available << s.completedSteps << s.runTargetSteps << s.initialTime << s.time << s.fixedTimeStep
        << s.nextCommandIndex << s.sourceFingerprint << s.systemIdentity;
    vector(out,s.orderedPhysicalNodeIds);vector(out,s.orderedMemberPhysicalIds);vector(out,s.orderedPortIds);
    vector(out,s.portVoltages);vector(out,s.portCharges);vector(out,s.portEnthalpies);
    vector(out,s.q);vector(out,s.v);vector(out,s.a);vector(out,s.reactions);vector(out,s.externalForce);
    vector(out,s.memberInputKinds);vector(out,s.memberInputValues);
    out << s.storedEnergy << s.kineticEnergy << s.initialTotalEnergy << s.cumulativeExternalWork
        << s.cumulativeSupportWork << s.cumulativeElectricalWork << s.cumulativeRestLengthWork << c.fingerprintBytes();
    return hash(bytes);
}
}

QByteArray rPiezoRuntimeControlSha256(const RModel &m) try
{
    const auto &p=m.getProblemSetup();
    if(p.getStructuralPiezoAxialSetup().enabled){const auto b=PiezoAxialBackendBridge::create(m);return control(b.callbacks.backendIdentity,bool(b.callbacks.propose));}
    if(p.getStructuralPiezoLayeredSetup().enabled && layeredBackendRequired(m)){
        const auto b=PiezoNativeBackendBridge::create(m);return control(b.callbacks.backendIdentity,bool(b.callbacks.propose));}
    return control({},false);
}

catch(const std::exception &error){throw RError(RError::Type::InvalidInput,R_ERROR_REF,"%s",error.what());}

RPiezoRuntimeCertificate rPiezoRuntimeCertify(const RModel &m,bool acceptedModel) try
{
    RModel source(m);auto &p=source.getProblemSetup();const auto &d=p.getStructuralDynamicsSetup();
    if(acceptedModel)p.setRestart(true);
    RPiezoRuntimeCertificate c;c.fixedTimeStep=d.getTimeStep();c.canonicalTarget=true;
    if(p.getStructuralPiezoAxialSetup().enabled){
        const auto b=PiezoAxialBackendBridge::create(source);const RStructuralPiezoAxialNativeRun run(source,b.callbacks);
        const auto &s=run.acceptedRuntime();c.family="axial";c.sourceFingerprint=s.sourceFingerprint;
        c.completedSteps=run.completedSteps();c.targetSteps=run.targetSteps();c.time=s.physical.time;c.initialTime=s.initialTime;
        c.available=p.getRestart();c.stateIdentity=hash(RStructuralPiezoAxialRestartCodec::encode(s));
    }else if(p.getStructuralPiezoLayeredSetup().enabled){
        RStructuralPiezoLayeredRuntimeCallbacks callbacks;
        if(layeredBackendRequired(source))callbacks=PiezoNativeBackendBridge::create(source).callbacks;
        const RStructuralPiezoLayeredNativeRun run(source,callbacks);const auto &s=run.acceptedRuntime();
        c.family="layered";c.sourceFingerprint=s.sourceFingerprint;c.completedSteps=run.completedSteps();
        c.targetSteps=run.targetSteps();c.time=s.physical.integrator.state.time;c.initialTime=s.initialTime;
        c.available=p.getRestart();c.stateIdentity=hash(RStructuralPiezoLayeredRestartCodec::encode(s));
    }else if(p.getStructuralTrussDynamicsSetup().enabled){
        const RStructuralTrussNativeRun run(source);const RModel sealed=run.acceptedModel(source);
        const auto &s=sealed.getProblemSetup().getStructuralTrussDynamicsRestartData();
        c.family="mixed";c.sourceFingerprint=run.adapter().sourceFingerprint();c.completedSteps=run.completedSteps();
        c.targetSteps=run.targetSteps();c.time=run.acceptedState().time;c.initialTime=run.adapter().initialTime();
        c.available=p.getRestart();c.stateIdentity=mixedStateIdentity(s,run.acceptedControlState());
    }else{
        if(p.getStructuralPiezoAxialSetup().hasNonDefaultData() || p.getStructuralPiezoAxialRestart().hasNonDefaultData()
            || p.getStructuralPiezoLayeredSetup().hasNonDefaultData() || p.getStructuralPiezoLayeredRestart().available)
            throw std::invalid_argument("Disabled experiment retains unsupported declared or accepted runtime data");
        if(source.getProblemTaskTree().getProblemTypeMask()!=R_PROBLEM_STRESS || !d.getEnabled()
            || d.getMethod()!=RStructuralTimeIntegrationMethod::Newmark || p.getStructuralControlSetup().enabled
            || p.getStructuralCableSetup().enabled || p.getStructuralPiezoSetup().enabled || p.getStructuralThermalControlSetup().enabled)
            throw std::invalid_argument("Accepted checkpoints require native mixed, axial, layered or uncontrolled Newmark dynamics");
        const auto &s=p.getStructuralDynamicsRestartData();if(s.available)s.validate();
        if(acceptedModel && !s.available)throw std::invalid_argument("Checkpoint has no accepted Newmark state");
        if(s.available && (s.fixedTimeStep!=d.getTimeStep() || s.meshNodeCount!=source.getNNodes()
            || s.meshElementCount!=source.getNElements()
            || s.meshFingerprint!=RStructuralDynamicsRestartData::computeMeshFingerprint(source)))
            throw std::invalid_argument("Accepted Newmark state differs from its actual source mesh or fixed time step");
        c.family="newmark";c.canonicalTarget=false;c.available=s.available;c.completedSteps=s.available?s.completedSteps:0;
        c.time=s.available?s.time:0;c.initialTime=c.time;c.targetSteps=c.completedSteps+d.getNSteps();
        c.sourceFingerprint=s.meshFingerprint+":"+s.dofFingerprint+":"+s.systemFingerprint;
        c.stateIdentity=legacyStateIdentity(s);
    }
    return c;
}
catch(const std::exception &error){throw RError(RError::Type::InvalidInput,R_ERROR_REF,"%s",error.what());}

bool rPiezoRuntimeSameAcceptedState(const RPiezoRuntimeCertificate &a,const RPiezoRuntimeCertificate &b)
{
    return a.available==b.available && a.family==b.family && a.sourceFingerprint==b.sourceFingerprint
        && a.completedSteps==b.completedSteps && a.time==b.time && a.fixedTimeStep==b.fixedTimeStep
        && a.stateIdentity==b.stateIdentity;
}
