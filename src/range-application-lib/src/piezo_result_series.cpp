#include "piezo_result_series.h"
#include "piezo_axial_backend_bridge.h"
#include "piezo_native_backend_bridge.h"
#include "piezo_certification_batch_cache.h"
#include <rml_model.h>
#include <rml_file_io.h>
#include <rbl_error.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rbl_hash_sha256.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <QUrl>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
QString digest(const QByteArray &bytes){return QString::fromLatin1(RHashSha256::hash(bytes).toHex());}
QString fileHash(const QString &path)
{
    QFile f(path);require(f.open(QIODevice::ReadOnly),"Native result record cannot be read");
    RHashSha256 h;require(h.addData(&f) && f.error()==QFileDevice::NoError,"Native result record cannot be hashed");
    return QString::fromLatin1(h.result().toHex());
}
bool linkedComponent(QString path)
{
    while(!path.isEmpty()){if(QFileInfo(path).isSymLink())return true;const auto parent=QFileInfo(path).absolutePath();if(parent==path)break;path=parent;}return false;
}
bool layeredBackend(const RModel &m)
{
    const auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();const auto d=s.damping.kind;
    return !s.excitationConfiguration.isEmpty() || !s.controllerConfiguration.isEmpty() || !s.designConfiguration.isEmpty()
        || d==RStructuralPiezoDampingKind::UniformModal || d==RStructuralPiezoDampingKind::PerMode || d==RStructuralPiezoDampingKind::PerCluster;
}
QString kind(const RModel &m)
{
    const auto &p=m.getProblemSetup();const bool axial=p.getStructuralPiezoAxialSetup().enabled;
    auto o=QJsonDocument::fromJson(axial?p.getStructuralPiezoAxialSetup().recipeConfiguration:p.getStructuralPiezoLayeredSetup().controllerConfiguration).object();
    if(axial)o=o.value(QStringLiteral("controller")).toObject();return o.value(QStringLiteral("kind")).toString(QStringLiteral("none"));
}
QString layeredModelFamily(RStructuralPiezoLayeredModelFamily family)
{
    switch(family){
    case RStructuralPiezoLayeredModelFamily::PlanarBeam:return QStringLiteral("planarBeam");
    case RStructuralPiezoLayeredModelFamily::FlatPlate:return QStringLiteral("flatPlate");
    case RStructuralPiezoLayeredModelFamily::Solid3D:return QStringLiteral("solid3D");
    }
    throw std::invalid_argument("Unsupported validated layered model family");
}
QString portKey(const QString &id){return QString::fromLatin1(QUrl::toPercentEncoding(id));}
QString binary64(double value){return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(&value),sizeof(value)).toHex());}
template<class Callbacks> QString configurationWitness(const QByteArray &setup,const Callbacks &cb)
{
    QByteArray fullC;for(double value:cb.authoritativeFullC)fullC.append(reinterpret_cast<const char *>(&value),sizeof(value));
    return QString("setupSha256=%1 initialOpaqueSha256=%2 fullCBitsSha256=%3 proposal=%4 postFE=%5 opaqueValidator=%6 boundaryValidator=%7 envelopeCount=%8")
        .arg(digest(setup),digest(cb.initialOpaqueControllerState),digest(fullC)).arg(bool(cb.propose)).arg(bool(cb.postFE))
        .arg(bool(cb.validateOpaque)).arg(bool(cb.validateAcceptedBoundary)).arg(cb.envelopes.size());
}
template<class State> void runtimeIdentity(const State &actual,const State &expected,const QString &family,const QString &configurationInputs)
{
    if(actual.sourceFingerprint==expected.sourceFingerprint && actual.backendIdentity==expected.backendIdentity &&
       actual.configurationIdentity==expected.configurationIdentity && actual.forcingIdentity==expected.forcingIdentity &&
       actual.controllerIdentity==expected.controllerIdentity && actual.targetSteps==expected.targetSteps &&
       actual.controlEveryFESteps==expected.controlEveryFESteps && std::memcmp(&actual.initialTime,&expected.initialTime,sizeof(double))==0 &&
       std::memcmp(&actual.dt,&expected.dt,sizeof(double))==0 && std::memcmp(&actual.nominalControlTs,&expected.nominalControlTs,sizeof(double))==0)return;
    QStringList differences;
    const auto text=[&](const QString &field,const QString &a,const QString &e){if(a!=e)differences.append(field+" expected="+e+" actual="+a);};
    text("sourceFingerprint",actual.sourceFingerprint,expected.sourceFingerprint);text("backendIdentity",actual.backendIdentity,expected.backendIdentity);
    text("configurationIdentity",QString::fromLatin1(actual.configurationIdentity.toHex()),QString::fromLatin1(expected.configurationIdentity.toHex()));
    text("forcingIdentity",actual.forcingIdentity,expected.forcingIdentity);text("controllerIdentity",actual.controllerIdentity,expected.controllerIdentity);
    text("targetSteps",QString::number(actual.targetSteps),QString::number(expected.targetSteps));
    text("controlEveryFESteps",QString::number(actual.controlEveryFESteps),QString::number(expected.controlEveryFESteps));
    text("initialTimeBits",binary64(actual.initialTime),binary64(expected.initialTime));text("dtBits",binary64(actual.dt),binary64(expected.dt));
    text("nominalControlTsBits",binary64(actual.nominalControlTs),binary64(expected.nominalControlTs));
    if(actual.configurationIdentity!=expected.configurationIdentity){differences.append("frozen configuration inputs: "+configurationInputs);
        if(actual.completedFE==0)differences.append("received zero-step opaqueSha256="+digest(actual.opaqueControllerState));}
    if(!differences.isEmpty())throw std::invalid_argument((family+" accepted runtime identity mismatch: "+differences.join("; ")).toStdString());
}
void signal(PiezoResultSeries::Frame &frame,const QString &id,const QString &quantity,const QString &axis,
            const QString &unit,const QString &target,double value,const QString &port={},uint node=0,bool action=false)
{
    require(std::isfinite(value) && !unit.isEmpty(),"Certified signal value or SI unit is invalid");
    frame.channels.append({id,quantity,axis,unit,target,port,node,action,value});
}
void readoutSignals(PiezoResultSeries::Frame &f)
{
    std::set<QString> ports;
    for(const auto &r:f.readouts){
        require(!r.portId.isEmpty() && ports.insert(r.portId).second,"Accepted named readout table has duplicate or empty ports");
        require((r.unit=="V" || r.unit=="C") && r.mode>=0 && r.mode<=4,"Accepted readout mode or SI unit is invalid");
        const auto base=QStringLiteral("piezo:port:")+portKey(r.portId)+":";
        signal(f,base+"readout:absolute","piezoReadout","absolute",r.unit,r.actionRecord?"accepted-piezo-action-record":"accepted-named-piezo-port",r.value,r.portId,0,r.actionRecord);
        signal(f,base+"readout:delta","piezoReadout","delta",r.unit,r.actionRecord?"accepted-piezo-action-record":"accepted-named-piezo-port",r.deltaValue,r.portId,0,r.actionRecord);
        signal(f,base+"voltage:absolute","electrodeVoltage","absolute","V",(r.mode==0 || r.mode==4)?"accepted-piezo-action-record":"accepted-named-piezo-port",r.voltage,r.portId,0,r.mode==0 || r.mode==4);
        signal(f,base+"voltage:delta","electrodeVoltage","delta","V",(r.mode==0 || r.mode==4)?"accepted-piezo-action-record":"accepted-named-piezo-port",r.deltaVoltage,r.portId,0,r.mode==0 || r.mode==4);
        signal(f,base+"charge:absolute","positiveElectrodeCharge","absolute","C","accepted-named-piezo-port",r.charge,r.portId);
        signal(f,base+"charge:delta","positiveElectrodeCharge","delta","C","accepted-named-piezo-port",r.deltaCharge,r.portId);
        if(r.amplifierAvailable){
            signal(f,base+"amplifierVoltage:absolute","idealAmplifierVoltage","absolute","V","accepted-named-piezo-port",r.amplifierVoltage,r.portId);
            signal(f,base+"amplifierVoltage:delta","idealAmplifierVoltage","delta","V","accepted-named-piezo-port",r.deltaAmplifierVoltage,r.portId);
        }
    }
    for(const auto &a:f.actions){
        const auto base=QStringLiteral("piezo:action:")+portKey(a.portId)+":";
        signal(f,base+"heldRequested","voltageAction","heldRequested","V","accepted-piezo-action",a.heldRequested,a.portId,0,true);
        signal(f,base+"requested","voltageAction","requested","V","accepted-piezo-action",a.requested,a.portId,0,true);
        signal(f,base+"amplitudeLimited","voltageAction","amplitudeLimited","V","accepted-piezo-action",a.amplitudeLimited,a.portId,0,true);
        signal(f,base+"previousApplied","voltageAction","previousApplied","V","accepted-piezo-action",a.previousApplied,a.portId,0,true);
        signal(f,base+"applied","voltageAction","applied","V","accepted-piezo-action",a.applied,a.portId,0,true);
    }
    signal(f,"piezo:electricalWork","electricalWork","scalar","J","accepted-piezo-electrical-ledger",f.electricalWorkJ);
}
template<class Actions> void actions(PiezoResultSeries::Frame &f,const std::vector<QString> &ids,
                                    const std::vector<double> &requested,const std::vector<double> &applied,const Actions &records)
{
    require(ids.size()==requested.size() && ids.size()==applied.size() && (records.empty() || records.size()==ids.size()),"Accepted voltage action table is incomplete");
    for(size_t i=0;i<ids.size();++i){PiezoResultSeries::Action a;a.portId=ids[i];a.heldRequested=requested[i];a.applied=applied[i];a.requested=requested[i];a.amplitudeLimited=applied[i];a.previousApplied=applied[i];
        if(!records.empty()){const auto &r=records[i];require(r.portId==ids[i],"Accepted voltage action port order changed");a.intervalPresent=true;a.requested=r.requested;a.amplitudeLimited=r.amplitudeLimited;a.previousApplied=r.previousApplied;a.applied=r.applied;a.amplitudeClipped=r.amplitudeClipped;a.rateClipped=r.rateClipped;}
        f.actions.append(a);
    }
}
void physical(PiezoResultSeries::Frame &f,const std::vector<uint> &ids,const std::vector<double> &q,
              const std::vector<double> &v,const std::vector<double> &a,uint stride,const std::vector<double> &P={},uint columns=0)
{
    require(q.size()==stride*ids.size() && v.size()==q.size() && a.size()==q.size(),"Accepted physical result shape differs from its actual FE map");
    require(P.empty() || (columns>0 && P.size()==q.size()*columns),"Actual supported physical projection shape is invalid");
    const char *axes[]={"X","Y","Z"};const std::vector<double> *values[]={&q,&v,&a};
    const char *translation[]={"displacement","velocity","acceleration"},*rotation[]={"rotation","angularVelocity","angularAcceleration"};
    const char *tUnits[]={"m","m/s","m/s^2"},*rUnits[]={"rad","rad/s","rad/s^2"};
    for(size_t n=0;n<ids.size();++n)for(uint component=0;component<stride;++component){
        bool supported=P.empty();if(!P.empty())for(uint j=0;j<columns;++j)supported=supported || P[(stride*n+component)*columns+j]!=0;
        if(!supported)continue;
        for(uint k=0;k<3;++k){const QString quantity=component<3?translation[k]:rotation[k];
            signal(f,QString("node:%1:%2:%3").arg(ids[n]).arg(quantity,axes[component%3]),quantity,axes[component%3],component<3?tUnits[k]:rUnits[k],
                   "physical-component-of-supported-native-kinematics",(*values[k])[stride*n+component],{},ids[n]);
        }
    }
}
}

struct PiezoResultSeries::Reader::Impl
{
    Qt::HANDLE owner=QThread::currentThreadId();
    QString family,modelFamily,sourceFingerprint,configurationIdentity,backendIdentity,backendControlSha256,controlKind,configurationInputs;
    mutable QByteArray lastCodecBytes;
    std::shared_ptr<const Frame> frame(const RStructuralPiezoAxialRunState &,const QByteArray &) const;
    std::shared_ptr<const Frame> frame(const RStructuralPiezoLayeredRunState &,const QByteArray &) const;
    RStructuralPiezoAxialRuntimeCallbacks axial;
    RStructuralPiezoLayeredRuntimeCallbacks layered;
    std::shared_ptr<PiezoCertificationBatchCache> certificationCache;
    std::unique_ptr<RStructuralPiezoAxialNativeRun> frozenAxial;
    std::unique_ptr<RStructuralPiezoLayeredNativeRun> frozenLayered;
    explicit Impl(const RModel &model,bool factoryRequired)
    {
        // Freeze the original source/initial state, then certify each accepted
        // payload using the pure facade API. Restart bytes never initialize or
        // choose the frozen provider. All typed source declarations remain.
        RModel m(model);m.getProblemSetup().setRestart(false);
        const auto &p=m.getProblemSetup();controlKind=kind(m);
        if(p.getStructuralPiezoAxialSetup().enabled){
            family="axial";const auto backend=PiezoAxialBackendBridge::create(m);axial=backend.callbacks;
            const auto &clock=p.getStructuralDynamicsSetup();certificationCache=std::make_shared<PiezoCertificationBatchCache>(backend.certificationBatchRequest,backend.backendInitializationIdentity,family,
                m.getTimeSolver().getInputStartTime()-clock.getTimeStep(),clock.getTimeStep(),clock.getNSteps(),axial.controlEveryFESteps,size_t(backend.exportPackage.value("nodes").toArray().size())*3,backend.certificationBatchPipeline);
            axial=certificationCache->wrapAxial(axial);frozenAxial=std::make_unique<RStructuralPiezoAxialNativeRun>(m,axial);
            modelFamily=QStringLiteral("axialRod");
            const auto &r=frozenAxial->acceptedRuntime();sourceFingerprint=r.sourceFingerprint;configurationIdentity=QString::fromLatin1(r.configurationIdentity.toHex());backendIdentity=r.backendIdentity;
            backendControlSha256=digest(axial.propose?axial.backendIdentity.toUtf8():QByteArrayLiteral("range.native-checkpoint.no-control.v1"));
            configurationInputs=configurationWitness(p.getStructuralPiezoAxialSetup().identityBytes(),axial);
        }else if(p.getStructuralPiezoLayeredSetup().enabled){
            family="layered";if(factoryRequired || layeredBackend(m)){const auto backend=PiezoNativeBackendBridge::create(m);layered=backend.callbacks;
                const auto &clock=p.getStructuralDynamicsSetup();certificationCache=std::make_shared<PiezoCertificationBatchCache>(backend.certificationBatchRequest,backend.backendInitializationIdentity,family,
                    m.getTimeSolver().getInputStartTime()-clock.getTimeStep(),clock.getTimeStep(),clock.getNSteps(),layered.controlEveryFESteps,size_t(backend.exportPackage.value("nodes").toArray().size())*6,backend.certificationBatchPipeline);
                layered=certificationCache->wrapLayered(layered);
            }
            frozenLayered=std::make_unique<RStructuralPiezoLayeredNativeRun>(m,layered);const auto &r=frozenLayered->acceptedRuntime();
            modelFamily=layeredModelFamily(p.getStructuralPiezoLayeredSetup().family);
            sourceFingerprint=r.sourceFingerprint;configurationIdentity=QString::fromLatin1(r.configurationIdentity.toHex());backendIdentity=r.backendIdentity;
            backendControlSha256=digest(layered.propose?layered.backendIdentity.toUtf8():QByteArrayLiteral("range.native-checkpoint.no-control.v1"));
            const RMatrixSolverConf solver(RMatrixSolverConf::GMRES);
            configurationInputs=configurationWitness(p.getStructuralPiezoLayeredSetup().identityBytes(),layered)+
                QString(" solverType=%1 inner=%2 outer=%3 toleranceBits=%4").arg(int(solver.getType())).arg(solver.getNInnerIterations())
                    .arg(solver.getNOuterIterations()).arg(binary64(solver.getSolverCvgValue()));
        }else throw std::invalid_argument("Named piezo result reader requires an enabled axial or layered source");
    }
};
PiezoResultSeries::Reader::Reader(const RModel &source,bool factoryRequired):impl(std::make_unique<Impl>(source,factoryRequired)){}
PiezoResultSeries::Reader::~Reader()=default;

std::shared_ptr<const PiezoResultSeries::Frame> PiezoResultSeries::Reader::Impl::frame(const RStructuralPiezoAxialRunState &r,const QByteArray &bytes) const
{
    auto f=std::make_shared<Frame>();
    f->family=family;f->modelFamily=modelFamily;f->sourceFingerprint=sourceFingerprint;f->backendIdentity=backendIdentity;
    f->backendControlSha256=backendControlSha256;f->configurationIdentity=configurationIdentity;f->controlKind=controlKind;
    const auto &run=*frozenAxial;const auto &s=r.physical;
        require(r.sourceFingerprint==f->sourceFingerprint && r.backendIdentity==f->backendIdentity && QString::fromLatin1(r.configurationIdentity.toHex())==f->configurationIdentity,"Accepted axial source, backend or runtime configuration changed");
        f->completedFE=r.completedFE;f->targetSteps=r.targetSteps;f->controlTick=r.controlTick;f->controlPhase=r.controlPhase;f->timeS=s.time;f->dtS=r.dt;f->initialTimeS=r.initialTime;
        f->stateSha256=digest(bytes);f->electricalWorkJ=s.cumulativeElectricalWork;
        f->electricalWorkSource="actual accepted mixed-truss positive-electrode electrical work; not mechanical actuator-force work";
        for(const auto &p:r.readouts){Readout out;out.portId=p.portId;out.unit=p.unit;out.mode=int(p.mode);out.actionRecord=p.actionRecord;out.value=p.value;out.deltaValue=p.deltaValue;
            out.voltage=p.voltage;out.deltaVoltage=p.deltaVoltage;out.charge=p.charge;out.deltaCharge=p.deltaCharge;
            out.amplifierAvailable=p.mode==RStructuralPiezoReadoutMode::IdealChargeAmplifier;if(out.amplifierAvailable){out.amplifierVoltage=p.value;out.deltaAmplifierVoltage=p.deltaValue;}f->readouts.append(out);}
        std::vector<QString> actuatorIds;for(const auto &id:run.adapter().maps().orderedPortIds){const auto &ports=run.adapter().piezoReadoutSetup().ports;
            const auto it=std::find_if(ports.begin(),ports.end(),[&](const auto &p){return p.portId==id;});require(it!=ports.end(),"Actual axial readout port metadata is missing");if(it->role!=RStructuralPiezoPortRole::Sensor)actuatorIds.push_back(id);}
        actions(*f,actuatorIds,r.heldRequested,r.lastApplied,r.actions);
        physical(*f,run.adapter().maps().orderedPhysicalNodeIds,s.displacements,s.velocities,s.accelerations,3);
    readoutSignals(*f);std::set<QString> ids;for(const auto &s:f->channels)require(ids.insert(s.channelId).second,"Certified native signal channel identity is duplicated");
    return f;
}

std::shared_ptr<const PiezoResultSeries::Frame> PiezoResultSeries::Reader::Impl::frame(const RStructuralPiezoLayeredRunState &r,const QByteArray &bytes) const
{
    auto f=std::make_shared<Frame>();
    f->family=family;f->modelFamily=modelFamily;f->sourceFingerprint=sourceFingerprint;f->backendIdentity=backendIdentity;
    f->backendControlSha256=backendControlSha256;f->configurationIdentity=configurationIdentity;f->controlKind=controlKind;
    const auto &run=*frozenLayered;const auto &s=r.physical;
        require(r.sourceFingerprint==f->sourceFingerprint && r.backendIdentity==f->backendIdentity && QString::fromLatin1(r.configurationIdentity.toHex())==f->configurationIdentity,"Accepted layered source, backend or runtime configuration changed");
        f->completedFE=r.completedFE;f->targetSteps=r.targetSteps;f->controlTick=r.controlTick;f->controlPhase=r.controlPhase;f->timeS=s.integrator.state.time;f->dtS=r.dt;f->initialTimeS=r.initialTime;
        f->stateSha256=digest(bytes);f->electricalWorkJ=s.electricalWork;
        f->electricalWorkSource="actual accepted layered voltage/electrode-charge electrical work; not mechanical actuator-force work";
        for(const auto &p:s.readouts){Readout out;out.portId=p.portId;out.unit=p.unit;out.mode=int(p.mode);out.actionRecord=p.actionRecord;out.value=p.value;out.deltaValue=p.deltaValue;
            const auto it=std::find_if(s.electrical.ports.begin(),s.electrical.ports.end(),[&](const auto &e){return e.portId==p.portId;});require(it!=s.electrical.ports.end(),"Actual layered electrode response is missing");
            out.voltage=it->voltage;out.deltaVoltage=it->deltaVoltage;out.charge=it->charge;out.deltaCharge=it->deltaCharge;
            out.amplifierAvailable=it->role==RStructuralPiezoElectricalRole::IdealChargeAmplifier;out.amplifierVoltage=it->amplifierVoltage;out.deltaAmplifierVoltage=it->deltaAmplifierVoltage;f->readouts.append(out);}
        actions(*f,run.adapter().electrical().condensed().actuatorPortIds,r.heldRequested,r.lastApplied,r.actions);
        physical(*f,run.adapter().mesh().nodeIds,s.physicalDisplacements,s.physicalVelocities,s.physicalAccelerations,6,run.adapter().mesh().physicalTransform,run.adapter().data().fullDofs);
    readoutSignals(*f);std::set<QString> ids;for(const auto &s:f->channels)require(ids.insert(s.channelId).second,"Certified native signal channel identity is duplicated");
    return f;
}

std::shared_ptr<const PiezoResultSeries::Frame> PiezoResultSeries::Reader::certifyAcceptedModel(const RModel &model) const
{
    require(QThread::currentThreadId()==impl->owner,"Piezo result certification must remain on its factory owner thread");
    if(impl->family=="axial"){const auto &restart=model.getProblemSetup().getStructuralPiezoAxialRestart();restart.validate();require(restart.available,"Accepted MODEL has no axial runtime");
        runtimeIdentity(RStructuralPiezoAxialRestartCodec::decode(restart.runtimeState),impl->frozenAxial->acceptedRuntime(),impl->family,impl->configurationInputs);
        const auto state=impl->frozenAxial->certifyAcceptedRuntime(model);const auto bytes=RStructuralPiezoAxialRestartCodec::encode(state);auto frame=impl->frame(state,bytes);impl->lastCodecBytes=bytes;return frame;}
    const auto &restart=model.getProblemSetup().getStructuralPiezoLayeredRestart();restart.validate();require(restart.available,"Accepted MODEL has no layered runtime");
    runtimeIdentity(RStructuralPiezoLayeredRestartCodec::decode(restart.stateBytes),impl->frozenLayered->acceptedRuntime(),impl->family,impl->configurationInputs);
    const auto state=impl->frozenLayered->certifyAcceptedRuntime(model);const auto bytes=RStructuralPiezoLayeredRestartCodec::encode(state);auto frame=impl->frame(state,bytes);impl->lastCodecBytes=bytes;return frame;
}
std::shared_ptr<const PiezoResultSeries::Frame> PiezoResultSeries::Reader::certifyAcceptedRuntime(const QByteArray &bytes) const
{
    require(QThread::currentThreadId()==impl->owner,"Piezo result certification must remain on its factory owner thread");
    require(!bytes.isEmpty()&&bytes.size()<=PiezoAcceptedTrace::MaxPayloadBytes,"Accepted runtime codec exceeds its trace bound");
    if(impl->family=="axial"){const auto state=RStructuralPiezoAxialRestartCodec::decode(bytes);runtimeIdentity(state,impl->frozenAxial->acceptedRuntime(),impl->family,impl->configurationInputs);
        impl->frozenAxial->certifyAcceptedRuntime(state);auto frame=impl->frame(state,bytes);impl->lastCodecBytes=bytes;return frame;}
    const auto state=RStructuralPiezoLayeredRestartCodec::decode(bytes);runtimeIdentity(state,impl->frozenLayered->acceptedRuntime(),impl->family,impl->configurationInputs);
    impl->frozenLayered->certifyAcceptedRuntime(state);auto frame=impl->frame(state,bytes);impl->lastCodecBytes=bytes;return frame;
}
void PiezoResultSeries::Reader::prepareAcceptedRuntimeBatch(const std::vector<QByteArray> &bytes) const
{
    require(QThread::currentThreadId()==impl->owner,"Piezo result preparation must remain on its factory owner thread");
    if(impl->certificationCache){impl->certificationCache->prepare(bytes);return;}
    require(!bytes.empty()&&bytes.size()<=128,"Accepted runtime preparation requires 1..128 records");
    for(const auto &record:bytes)require(!record.isEmpty()&&record.size()<=PiezoAcceptedTrace::MaxPayloadBytes,"Accepted runtime preparation codec exceeds its trace bound");
}
void PiezoResultSeries::Reader::beginAcceptedRuntimeBatch(const std::vector<QByteArray> &bytes) const
{
    require(QThread::currentThreadId()==impl->owner,"Piezo batch begin must remain on its owner thread");
    require(bool(impl->certificationCache),"Piezo pipelining requires the formal provider");
    impl->certificationCache->beginPrepare(bytes);
}
void PiezoResultSeries::Reader::finishAcceptedRuntimeBatch() const
{
    require(QThread::currentThreadId()==impl->owner,"Piezo batch finish must remain on its owner thread");
    require(bool(impl->certificationCache),"Piezo pipelining requires the formal provider");
    impl->certificationCache->finishPrepare();
}
void PiezoResultSeries::Reader::recheckSources() const
{
    require(!impl->lastCodecBytes.isEmpty(),"No accepted runtime was certified before final source recheck");
    if(impl->certificationCache)impl->certificationCache->recheck();
    const QByteArray bytes=impl->lastCodecBytes;certifyAcceptedRuntime(bytes);
}

PiezoResultSeries::Result PiezoResultSeries::readAcceptedRecords(const QStringList &paths,const QString &expectedSource,const QStringList &expectedHashes)
{
    Result result;
    try {
        require(!paths.isEmpty() && paths.size()<=20001,"Select between 1 and 20001 explicit accepted native records");
        require(expectedHashes.isEmpty() || expectedHashes.size()==paths.size(),"Expected native record hashes must follow input path order");
        struct Input{QString path,hash;std::shared_ptr<const Frame> frame;};QVector<Input> inputs;std::set<QString> seen;
        std::unique_ptr<Reader> reader;
        for(qsizetype i=0;i<paths.size();++i){const QFileInfo info(paths[i]);
            require(info.isFile() && !linkedComponent(info.absoluteFilePath()) && info.size()>0 && info.size()<=256LL*1024*1024,"Native record is unavailable, linked, empty or exceeds its bound");
            require(info.suffix().compare("rbm",Qt::CaseInsensitive)==0 || info.suffix().compare("ram",Qt::CaseInsensitive)==0,"Named piezo records must be explicit RBM or RAM files");
            const auto path=info.canonicalFilePath();require(!path.isEmpty() && seen.insert(path.toCaseFolded()).second,"Duplicate or noncanonical native record path");
            Input in;in.path=path;in.hash=fileHash(path);require(expectedHashes.isEmpty() || expectedHashes[i]==in.hash,"Native record differs from its requested artifact hash");
            const bool ascii=info.suffix().compare("ram",Qt::CaseInsensitive)==0;
            RFile file(path,ascii?RFile::ASCII:RFile::BINARY);require(file.open(ascii?(QIODevice::ReadOnly|QIODevice::Text):QIODevice::ReadOnly),"Native record header cannot be read");RFileHeader header;
            if(info.suffix().compare("ram",Qt::CaseInsensitive)==0)RFileIO::readAscii(file,header);else RFileIO::readBinary(file,header);
            require(header.getType()==R_FILE_TYPE_MODEL,"Named piezo result reader does not admit LINK records");file.close();
            RModel model;model.read(path);require(fileHash(path)==in.hash,"Native record changed during decode");model.getProblemSetup().setRestart(true);
            if(!reader)reader=std::make_unique<Reader>(model);in.frame=reader->certifyAcceptedModel(model);
            require(expectedSource.isEmpty() || in.frame->sourceFingerprint==expectedSource,"Native result source differs from the selected experiment");
            require(fileHash(path)==in.hash,"Native record changed during semantic certification");inputs.append(std::move(in));
        }
        std::sort(inputs.begin(),inputs.end(),[](const auto &a,const auto &b){return a.frame->completedFE<b.frame->completedFE;});
        auto dataset=std::make_shared<Dataset>();const auto &first=*inputs.front().frame;
        dataset->family=first.family;dataset->modelFamily=first.modelFamily;dataset->sourceFingerprint=first.sourceFingerprint;dataset->configurationIdentity=first.configurationIdentity;dataset->backendIdentity=first.backendIdentity;
        QByteArray witness=first.sourceFingerprint.toUtf8()+'\n'+first.configurationIdentity.toUtf8()+'\n'+first.backendIdentity.toUtf8()+'\n';
        for(qsizetype i=0;i<inputs.size();++i){const auto &in=inputs[i];const auto &frame=*in.frame;
            if(i){const auto &previous=*inputs[i-1].frame;require(frame.family==first.family && frame.modelFamily==first.modelFamily,"Accepted native records changed their validated model family");
                require(frame.completedFE>previous.completedFE && frame.timeS>previous.timeS,"Duplicate accepted FE step or nonincreasing native time");
                if(frame.completedFE!=previous.completedFE+1)dataset->continuousBetweenSelectedRecords=false;}
            require(fileHash(in.path)==in.hash,"Native result record changed before immutable DTO publication");
            dataset->records.append({QFileInfo(in.path).fileName(),in.hash,frame.completedFE});dataset->frames.append(in.frame);
            witness+=QByteArray::number(frame.completedFE)+':'+in.hash.toLatin1()+'\n';
        }
        dataset->artifactSha256=digest(witness);
        reader->recheckSources();
        result.valid=true;result.code="CERTIFIED_ACCEPTED_NATIVE_RECORDS";result.dataset=dataset;
    } catch(const RError &error){result.code="INVALID_ACCEPTED_NATIVE_RECORD";result.message=error.getMessage();}
      catch(const std::exception &error){result.code="INVALID_ACCEPTED_NATIVE_RECORD";result.message=QString::fromUtf8(error.what());}
    return result;
}

namespace {
bool sameBits(double a,double b){return std::memcmp(&a,&b,sizeof(double))==0;}
bool sameChannel(const PiezoResultSeries::Signal &a,const PiezoResultSeries::Signal &b)
{
    return a.channelId==b.channelId&&a.quantity==b.quantity&&a.axis==b.axis&&a.unit==b.unit&&a.nodeTarget==b.nodeTarget
        &&a.portId==b.portId&&a.physicalNodeId==b.physicalNodeId&&a.actionRecord==b.actionRecord;
}
QByteArray flatPrefixHash(QIODevice &device,qint64 end)
{
    require(end>=0&&end<=PiezoAcceptedTrace::MaxFileBytes&&device.seek(0),"Trace final prefix seek failed");RHashSha256 hash;
    qint64 remaining=end;while(remaining){const auto bytes=device.read(std::min<qint64>(remaining,64*1024));require(!bytes.isEmpty(),"Trace final prefix is truncated");hash.addData(bytes);remaining-=bytes.size();}return hash.result();
}
QString admittedTracePath(const QString &path,qint64 bound)
{
    const QFileInfo info(path);require(info.isFile()&&!linkedComponent(info.absoluteFilePath())&&info.size()>0&&info.size()<=bound,"Trace source is unavailable, linked, empty or exceeds its bound");
    const auto canonical=info.canonicalFilePath();require(!canonical.isEmpty(),"Trace source has no canonical path");return canonical;
}
bool hotpathProfileEnabled()
{
    return !qEnvironmentVariable("RANGE_PIEZO_PERFORMANCE_WORKSPACE").isEmpty()
        &&!qEnvironmentVariable("RANGE_PIEZO_PERFORMANCE_EVENTS").isEmpty();
}
void appendHotpathProfile(QJsonObject fields) noexcept
{
    try {
        const QString workspace=qEnvironmentVariable("RANGE_PIEZO_PERFORMANCE_WORKSPACE");
        const QString events=qEnvironmentVariable("RANGE_PIEZO_PERFORMANCE_EVENTS");
        if(workspace.isEmpty()||events.isEmpty()||!QDir::isAbsolutePath(workspace)||!QDir::isAbsolutePath(events))return;
        const QFileInfo root(workspace),target(events),parent(target.absolutePath());
        if(!root.isDir()||root.canonicalFilePath().isEmpty()||!parent.isDir()||parent.canonicalFilePath().isEmpty()
            ||target.isSymLink()||(target.exists()&&!target.isFile()))return;
        const auto relative=QDir(root.canonicalFilePath()).relativeFilePath(QDir(parent.canonicalFilePath()).filePath(target.fileName()));
        if(QDir::isAbsolutePath(relative)||relative==".."||relative.startsWith("../"))return;
        fields["schema"]="range.piezo-hotpath-profile/v1";fields["event"]="trace_processing_profile";
        const auto bytes=QJsonDocument(fields).toJson(QJsonDocument::Compact)+'\n';constexpr qint64 maximum=16LL*1024*1024;
        if(bytes.size()>64*1024||target.size()>maximum-bytes.size())return;
        QFile file(events);if(!file.open(QIODevice::WriteOnly|QIODevice::Append)||file.size()>maximum-bytes.size())return;
        if(file.write(bytes)==bytes.size())file.flush();
    }catch(...){}
}
QByteArray baseCodec(const RModel &model)
{
    if(model.getProblemSetup().getStructuralPiezoAxialSetup().enabled){const auto &restart=model.getProblemSetup().getStructuralPiezoAxialRestart();restart.validate();require(restart.available,"Trace base MODEL has no accepted axial runtime");return restart.runtimeState;}
    const auto &restart=model.getProblemSetup().getStructuralPiezoLayeredRestart();restart.validate();require(restart.available,"Trace base MODEL has no accepted layered runtime");return restart.stateBytes;
}
PiezoResultSeries::NumericResult processTrace(const PiezoResultSeries::TraceProof &proof,const QStringList &selectedIds,
                                           bool retainColumns,const PiezoResultSeries::FrameConsumer &consumer,
                                           const PiezoResultSeries::SummaryConsumer &stageCompletion={},
                                           bool allowUncommittedTail=false)
{
    using Series=PiezoResultSeries;using Trace=PiezoAcceptedTrace;Series::NumericResult result;
    const bool profileEnabled=hotpathProfileEnabled();QElapsedTimer totalTimer,phaseTimer,hashTimer,certificateTimer,recordTimer;
    if(profileEnabled)totalTimer.start();
    qint64 baseModelHashNanoseconds=0,traceHeaderNanoseconds=0,baseModelDecodeNanoseconds=0,prefixVerificationNanoseconds=0;
    qint64 readerProviderInitNanoseconds=0,initialRuntimeCertificationNanoseconds=0,batchReadNanoseconds=0,batchProviderNanoseconds=0;
    qint64 recordChecksNanoseconds=0,runtimeCertificationNanoseconds=0,finalPrefixSourceCheckNanoseconds=0;
    qint64 baseModelBytes=0;quint64 baseModelHashPasses=0,verifiedPrefixRecords=0,runtimeCertificateCount=0,batchCount=0;
    std::atomic<qint64> prefixVerificationThreadNanoseconds{0};
    try {
        require(proof.baseModelSha256.size()==32&&proof.expectedHeaderSha256.size()==32&&proof.expectedContextSha256.size()==32&&!proof.expectedSourceFingerprint.isEmpty(),"Trace admission requires explicit base, header, context and source authority");
        const auto basePath=admittedTracePath(proof.baseModelPath,256LL*1024*1024),tracePath=admittedTracePath(proof.tracePath,Trace::MaxFileBytes);
        require(basePath!=tracePath,"Trace cannot be its own base MODEL");
        baseModelBytes=QFileInfo(basePath).size();
        if(profileEnabled)hashTimer.start();
        require(QByteArray::fromHex(fileHash(basePath).toLatin1())==proof.baseModelSha256,"Trace base MODEL differs from its approved SHA-256");
        if(profileEnabled){baseModelHashNanoseconds+=hashTimer.nsecsElapsed();++baseModelHashPasses;}
        const auto suffix=QFileInfo(basePath).suffix().toLower();require(suffix=="ram"||suffix=="rbm","Trace base must be an explicit RAM or RBM MODEL");
        RFile nativeFile(basePath,suffix=="ram"?RFile::ASCII:RFile::BINARY);require(nativeFile.open(suffix=="ram"?(QIODevice::ReadOnly|QIODevice::Text):QIODevice::ReadOnly),"Trace base MODEL header cannot be read");RFileHeader nativeHeader;
        if(suffix=="ram")RFileIO::readAscii(nativeFile,nativeHeader);else RFileIO::readBinary(nativeFile,nativeHeader);require(nativeHeader.getType()==R_FILE_TYPE_MODEL,"Trace base does not admit LINK records");nativeFile.close();
        QFile trace(tracePath);require(trace.open(QIODevice::ReadOnly),"Accepted trace cannot be opened");
        if(profileEnabled)phaseTimer.start();
        const auto header=Trace::readHeader(trace);
        if(profileEnabled)traceHeaderNanoseconds=phaseTimer.nsecsElapsed();
        require(header.headerSha32==proof.expectedHeaderSha256&&header.contextSha256==proof.expectedContextSha256&&header.baseModelSha256==proof.baseModelSha256,"Trace header differs from approved base/header/context authority");
        require(header.sourceFingerprint==proof.expectedSourceFingerprint,"Trace source differs from the selected experiment");
        require(QFileInfo(QDir(QFileInfo(tracePath).absolutePath()).filePath(header.baseModelFile)).canonicalFilePath()==basePath,"Trace relative base MODEL differs from the explicit approved path");
        RModel model;if(profileEnabled)phaseTimer.start();model.read(basePath);
        if(profileEnabled)baseModelDecodeNanoseconds=phaseTimer.nsecsElapsed();
        if(profileEnabled)hashTimer.start();
        require(QByteArray::fromHex(fileHash(basePath).toLatin1())==proof.baseModelSha256,"Trace base changed during MODEL decode");
        if(profileEnabled){baseModelHashNanoseconds+=hashTimer.nsecsElapsed();++baseModelHashPasses;}
        model.getProblemSetup().setRestart(true);
        // The worker owns its device and touches no MODEL, provider or DTO. Its
        // future is destroyed before the reader, joining even if factory setup
        // throws. Neither path can admit runtimes until both have succeeded.
        std::unique_ptr<Series::Reader> readerOwner;
        auto prefixVerification=std::async(std::launch::async,[tracePath,cursor=proof.cursor,profileEnabled,allowUncommittedTail,&prefixVerificationThreadNanoseconds]{
            QElapsedTimer verifyTimer;if(profileEnabled)verifyTimer.start();
            try {
                QFile verificationTrace(tracePath);require(verificationTrace.open(QIODevice::ReadOnly),"Accepted trace verification cannot be opened");
                auto verified=Trace::verifyPrefix(verificationTrace,cursor,allowUncommittedTail);
                if(profileEnabled)prefixVerificationThreadNanoseconds.store(verifyTimer.nsecsElapsed(),std::memory_order_relaxed);
                return verified;
            }catch(...){if(profileEnabled)prefixVerificationThreadNanoseconds.store(verifyTimer.nsecsElapsed(),std::memory_order_relaxed);throw;}
        });
        if(profileEnabled)phaseTimer.start();readerOwner=std::make_unique<Series::Reader>(model,true);
        if(profileEnabled)readerProviderInitNanoseconds=phaseTimer.nsecsElapsed();
        const auto prefix=prefixVerification.get();
        if(profileEnabled){prefixVerificationNanoseconds=prefixVerificationThreadNanoseconds.load(std::memory_order_relaxed);verifiedPrefixRecords=prefix.records;}
        require(prefix.records>0,"Accepted trace contains no committed runtime records");
        require(prefix.header.headerSha32==header.headerSha32,"Trace header changed during provider initialization");
        auto &reader=*readerOwner;const QByteArray initialBytes=baseCodec(model);
        if(profileEnabled)phaseTimer.start();const auto first=reader.certifyAcceptedRuntime(initialBytes);
        if(profileEnabled)initialRuntimeCertificationNanoseconds=phaseTimer.nsecsElapsed();
        require(first->family==header.family&&first->sourceFingerprint==header.sourceFingerprint&&first->configurationIdentity==header.configurationIdentity&&first->backendIdentity==header.backendIdentity,"Trace header source/configuration/backend differs from the actual base MODEL");
        require(first->completedFE==header.baseSeq&&first->targetSteps==header.targetSteps&&sameBits(first->initialTimeS,header.initialTime)&&sameBits(first->dtS,header.dt),"Trace base sequence or original clock differs from its certified MODEL");
        auto dataset=std::make_shared<Series::NumericDataset>();dataset->family=header.family;dataset->modelFamily=first->modelFamily;dataset->sourceFingerprint=header.sourceFingerprint;dataset->configurationIdentity=header.configurationIdentity;dataset->backendIdentity=header.backendIdentity;
        dataset->baseSeq=header.baseSeq;dataset->targetSteps=header.targetSteps;dataset->initialTimeS=header.initialTime;dataset->dtS=header.dt;dataset->readouts=first->readouts;
        for(auto &readout:dataset->readouts){readout.value=readout.deltaValue=readout.voltage=readout.deltaVoltage=readout.charge=readout.deltaCharge=readout.amplifierVoltage=readout.deltaAmplifierVoltage=0.;}
        std::vector<qsizetype> selected;std::set<QString> ids;
        if(selectedIds.isEmpty()){for(qsizetype i=0;i<first->channels.size();++i)selected.push_back(i);}
        else for(const auto &id:selectedIds){require(ids.insert(id).second,"Duplicate selected trace channel");const auto found=std::find_if(first->channels.begin(),first->channels.end(),[&](const auto &signal){return signal.channelId==id;});require(found!=first->channels.end(),"Selected trace channel is absent from the actual base catalog");selected.push_back(found-first->channels.begin());}
        require(!selected.empty(),"Trace source provides no selected certified channels");
        require(!retainColumns||quint64(selected.size())<=64000000/prefix.records,"Selected trace numeric columns exceed the bounded memory budget");
        for(const auto index:selected){auto metadata=first->channels[index];metadata.value=0.;dataset->channels.append(metadata);if(retainColumns){dataset->columns.append(QVector<double>());dataset->columns.last().reserve(qsizetype(prefix.records));}}
        dataset->completedFE.reserve(qsizetype(prefix.records));dataset->times.reserve(qsizetype(prefix.records));
        auto cursor=Trace::initialCursor(header);quint64 count=0;
        struct Batch {std::vector<Trace::Record> records;std::vector<QByteArray> codecs;};
        const auto readBatch=[&]{Batch batch;qint64 batchBytes=0;
            while(batch.records.size()<128&&cursor.byteEnd<proof.cursor.byteEnd){auto record=Trace::readRecord(trace,header,cursor,proof.cursor.byteEnd);
                if(!batch.records.empty()&&batchBytes+record.payload.size()>4LL*1024*1024)break;
                cursor=record.cursor;batchBytes+=record.payload.size();batch.codecs.push_back(record.payload);batch.records.push_back(std::move(record));
            }return batch;
        };
        if(profileEnabled)phaseTimer.start();auto current=readBatch();
        if(profileEnabled)batchReadNanoseconds+=phaseTimer.nsecsElapsed();
        if(profileEnabled)phaseTimer.start();reader.prepareAcceptedRuntimeBatch(current.codecs);
        if(profileEnabled){batchProviderNanoseconds+=phaseTimer.nsecsElapsed();++batchCount;}
        while(!current.records.empty()){
            if(profileEnabled)phaseTimer.start();auto next=readBatch();
            if(profileEnabled)batchReadNanoseconds+=phaseTimer.nsecsElapsed();
            const bool pending=!next.records.empty();
            if(pending){if(profileEnabled)phaseTimer.start();reader.beginAcceptedRuntimeBatch(next.codecs);
                if(profileEnabled){batchProviderNanoseconds+=phaseTimer.nsecsElapsed();++batchCount;}}
            for(const auto &record:current.records){
                if(profileEnabled)certificateTimer.start();const auto frame=reader.certifyAcceptedRuntime(record.payload);
                if(profileEnabled){runtimeCertificationNanoseconds+=certificateTimer.nsecsElapsed();++runtimeCertificateCount;recordTimer.start();}
                require(frame->completedFE==record.sequence&&sameBits(frame->timeS,record.time)&&frame->stateSha256==QString::fromLatin1(record.cursor.stateSha32.toHex()),"Trace record differs from its complete certified native runtime");
                if(!count)require(record.payload==initialBytes,"Trace first runtime differs from its exact approved base MODEL state");
                require(record.sequence==header.baseSeq+count,"Accepted trace sequence is discontinuous from its base");
                require(frame->family==header.family&&frame->modelFamily==dataset->modelFamily&&frame->sourceFingerprint==header.sourceFingerprint&&frame->configurationIdentity==header.configurationIdentity&&frame->backendIdentity==header.backendIdentity
                    &&frame->targetSteps==header.targetSteps&&sameBits(frame->initialTimeS,header.initialTime)&&sameBits(frame->dtS,header.dt),"Trace certified source, backend or original clock changed");
                require(frame->channels.size()==first->channels.size()&&frame->readouts.size()==first->readouts.size(),"Trace certified channel or readout catalog changed");
                for(qsizetype i=0;i<frame->channels.size();++i)require(sameChannel(frame->channels[i],first->channels[i]),"Trace certified channel metadata changed");
                for(qsizetype i=0;i<frame->readouts.size();++i){const auto &a=frame->readouts[i],&b=first->readouts[i];require(a.portId==b.portId&&a.unit==b.unit&&a.mode==b.mode&&a.actionRecord==b.actionRecord&&a.amplifierAvailable==b.amplifierAvailable,"Trace readout mode or unit changed");}
                if(count)require(frame->timeS>dataset->times.last(),"Trace native time does not advance");
                dataset->completedFE.append(frame->completedFE);dataset->times.append(frame->timeS);
                if(retainColumns)for(size_t column=0;column<selected.size();++column)dataset->columns[qsizetype(column)].append(frame->channels[selected[column]].value);
                if(consumer)consumer(*frame,dataset->channels);++count;
                if(profileEnabled)recordChecksNanoseconds+=recordTimer.nsecsElapsed();
            }
            if(pending){if(profileEnabled)phaseTimer.start();reader.finishAcceptedRuntimeBatch();
                if(profileEnabled)batchProviderNanoseconds+=phaseTimer.nsecsElapsed();}
            current=std::move(next);
        }
        require(count==prefix.records&&cursor.byteEnd==proof.cursor.byteEnd&&cursor.recordSha32==proof.cursor.recordSha32&&cursor.stateSha32==proof.cursor.stateSha32&&cursor.sequence==proof.cursor.sequence&&cursor.lastRecordOffset==proof.cursor.lastRecordOffset,"Trace final cursor differs from approved committed authority");
        const QJsonObject witness{{"schema","range.piezo-accepted-numeric-trace/v1"},{"baseModelSha256",QString::fromLatin1(proof.baseModelSha256.toHex())},{"headerSha256",QString::fromLatin1(header.headerSha32.toHex())},{"contextSha256",QString::fromLatin1(header.contextSha256.toHex())},{"committedPrefixSha256",QString::fromLatin1(prefix.prefixSha32.toHex())},
            {"sourceFingerprint",header.sourceFingerprint},{"configurationIdentity",header.configurationIdentity},{"backendIdentity",header.backendIdentity},{"cursor",QJsonObject{{"byteEnd",double(cursor.byteEnd)},{"lastRecordOffset",double(cursor.lastRecordOffset)},{"sequence",double(cursor.sequence)},{"recordSha256",QString::fromLatin1(cursor.recordSha32.toHex())},{"stateSha256",QString::fromLatin1(cursor.stateSha32.toHex())}}}};
        dataset->artifactSha256=digest(QJsonDocument(witness).toJson(QJsonDocument::Compact));
        // Completion may fill a staged artifact, never publish it. Source and
        // provider checks follow ALL staging work, including a long CSV copy.
        if(stageCompletion)stageCompletion(*dataset);
        if(profileEnabled)phaseTimer.start();
        const bool finalBaseMatches=admittedTracePath(proof.baseModelPath,256LL*1024*1024)==basePath
            && [&](){
                if(profileEnabled)hashTimer.start();
                const bool matches=QByteArray::fromHex(fileHash(basePath).toLatin1())==proof.baseModelSha256;
                if(profileEnabled){baseModelHashNanoseconds+=hashTimer.nsecsElapsed();++baseModelHashPasses;}
                return matches;
            }();
        require(finalBaseMatches,"Trace base MODEL changed before numeric publication");
        require(admittedTracePath(proof.tracePath,Trace::MaxFileBytes)==tracePath,"Trace path changed before numeric publication");QFile finalTrace(tracePath);require(finalTrace.open(QIODevice::ReadOnly)&&finalTrace.size()>=proof.cursor.byteEnd&&(allowUncommittedTail||finalTrace.size()==proof.cursor.byteEnd),"Trace committed prefix length changed before publication");
        require(flatPrefixHash(finalTrace,proof.cursor.byteEnd)==prefix.prefixSha32&&flatPrefixHash(trace,proof.cursor.byteEnd)==prefix.prefixSha32&&finalTrace.size()==trace.size()&&(allowUncommittedTail||trace.size()==proof.cursor.byteEnd),"Trace committed prefix changed before numeric publication");
        reader.recheckSources();
        if(profileEnabled)finalPrefixSourceCheckNanoseconds=phaseTimer.nsecsElapsed();
        result.valid=true;result.code="CERTIFIED_ACCEPTED_NATIVE_TRACE";result.dataset=dataset;
    }catch(const RError &error){result.code="INVALID_ACCEPTED_NATIVE_TRACE";result.message=error.getMessage();}
     catch(const std::exception &error){result.code="INVALID_ACCEPTED_NATIVE_TRACE";result.message=QString::fromUtf8(error.what());}
    if(profileEnabled)appendHotpathProfile({{"totalNanoseconds",QString::number(totalTimer.nsecsElapsed())},
        {"baseModelHashNanoseconds",QString::number(baseModelHashNanoseconds)},{"baseModelHashPasses",QString::number(baseModelHashPasses)},
        {"baseModelBytes",QString::number(baseModelBytes)},{"traceHeaderNanoseconds",QString::number(traceHeaderNanoseconds)},
        {"baseModelDecodeNanoseconds",QString::number(baseModelDecodeNanoseconds)},
        {"verifyPrefixNanoseconds",QString::number(prefixVerificationNanoseconds)},{"verifiedPrefixRecords",QString::number(verifiedPrefixRecords)},
        {"committedTraceBytes",QString::number(proof.cursor.byteEnd)},{"readerProviderInitNanoseconds",QString::number(readerProviderInitNanoseconds)},
        {"initialRuntimeCertificationNanoseconds",QString::number(initialRuntimeCertificationNanoseconds)},
        {"batchReadNanoseconds",QString::number(batchReadNanoseconds)},{"batchProviderNanoseconds",QString::number(batchProviderNanoseconds)},
        {"batchCount",QString::number(batchCount)},{"recordChecksNanoseconds",QString::number(recordChecksNanoseconds)},
        {"runtimeCertificationNanoseconds",QString::number(runtimeCertificationNanoseconds)},
        {"runtimeCertificateCount",QString::number(runtimeCertificateCount)},
        {"finalPrefixSourceCheckNanoseconds",QString::number(finalPrefixSourceCheckNanoseconds)}});
    return result;
}
}
PiezoResultSeries::NumericResult PiezoResultSeries::readAcceptedTrace(const TraceProof &proof,const QStringList &channels)
{return processTrace(proof,channels,true,{}, {},false);}
PiezoResultSeries::NumericResult PiezoResultSeries::streamAcceptedTrace(const TraceProof &proof,const QStringList &channels,const FrameConsumer &consumer,const SummaryConsumer &stageCompletion,bool allowUncommittedTail)
{return processTrace(proof,channels,false,consumer,stageCompletion,allowUncommittedTail);}
