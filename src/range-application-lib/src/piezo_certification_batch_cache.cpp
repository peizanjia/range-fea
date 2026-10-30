#include "piezo_certification_batch_cache.h"
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <rbl_hash_sha256.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QThread>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
QString hash(const QByteArray &bytes){return QString::fromLatin1(RHashSha256::hash(bytes).toHex());}
bool hex(const QJsonValue &value,int length)
{
    if(!value.isString() || value.toString().size()!=length)return false;
    for(QChar c:value.toString())if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return false;
    return true;
}
QString bits(double value)
{
    static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559,"IEEE binary64 required");
    quint64 word=0;std::memcpy(&word,&value,8);QByteArray bytes;
    for(uint i=0;i<8;++i)bytes.append(char((word>>(8*i))&255));
    return QString::fromLatin1(bytes.toHex());
}
void keys(const QJsonObject &object,std::initializer_list<const char *> expected,
          std::initializer_list<const char *> optional={})
{
    std::set<QString> admitted;
    for(const auto key:expected)admitted.insert(QLatin1String(key));
    for(const auto key:optional)admitted.insert(QLatin1String(key));
    bool invalid=object.size()<int(expected.size()) || object.size()>int(admitted.size());
    for(auto it=object.begin();it!=object.end();++it)if(!admitted.count(it.key()))invalid=true;
    if(invalid){
        QStringList actual;for(auto it=object.begin();it!=object.end();++it)actual.append(it.key());
        QStringList wanted;for(const auto key:expected)wanted.append(QLatin1String(key));
        for(const auto key:optional)wanted.append(QStringLiteral("optional:")+QLatin1String(key));
        throw std::invalid_argument(QString("Certification object fields differ; expected [%1], received [%2]")
            .arg(wanted.join(", "),actual.join(", ")).toStdString());
    }
    for(const auto key:expected)require(object.contains(QLatin1String(key)),"Certification response field missing");
}
bool integer(const QJsonValue &value,uint expected){return value.isDouble() && value.toDouble()==double(expected);}
QJsonObject opaqueObject(const QByteArray &bytes)
{
    QJsonParseError error;const auto document=QJsonDocument::fromJson(bytes,&error);
    require(error.error==QJsonParseError::NoError && document.isObject(),"Certification opaque state is not JSON");
    return document.object();
}
QJsonArray array(const std::vector<double> &values)
{
    QJsonArray result;for(double value:values){require(std::isfinite(value),"Certification voltage is nonfinite");result.append(value);}return result;
}
}

struct PiezoCertificationBatchCache::Impl
{
    struct Record {QString id;QByteArray codec,opaque;uint step=0,tick=0,phase=0;QJsonObject request;};
    Qt::HANDLE owner=QThread::currentThreadId();
    Request request;
    Pipeline pipeline;
    bool pendingBank=false;
    std::vector<Record> pendingCandidates;
    std::map<uint,QString> pendingTimes;
    QJsonObject immediateResponse;
    QJsonObject identity;
    QString family,source,configuration,forceSource,forceCode,backendIdentity,forcingIdentity,controllerIdentity;
    double initialTime,dt;
    uint target,N;
    size_t forceSize;
    bool activated=false,bound=false;
    std::map<QString,Record> records;
    std::map<QString,std::vector<double>> forces;

    Impl(Request rpc,const QJsonObject &initialized,const QString &kind,double start,double step,uint end,uint every,size_t size,Pipeline channel)
        :request(std::move(rpc)),pipeline(std::move(channel)),identity(initialized),family(kind),initialTime(start),dt(step),target(end),N(every),forceSize(size)
    {
        require((!pipeline.begin && !pipeline.finish && !pipeline.abort) || (pipeline.begin && pipeline.finish && pipeline.abort),"Incomplete certification pipeline");
        require(bool(request) && (family=="axial" || family=="layered"),"Invalid certification cache provider/family");
        require(std::isfinite(initialTime) && std::isfinite(dt) && dt>0 && target>0 && N>0 && forceSize>0,"Invalid certification cache clock/dimensions");
        keys(identity,{"sourceFingerprint","configurationIdentity","backend","excitationSourceHash","excitationLoadedCodeHash"},{"externalConnections"});
        if(identity.contains("externalConnections"))require(identity.value("externalConnections").isObject(),"Invalid frozen external connection identities");
        for(const auto key:{"sourceFingerprint","configurationIdentity","excitationSourceHash","excitationLoadedCodeHash"})require(hex(identity.value(key),64),"Invalid frozen certification identity hash");
        require(identity.value("backend").isObject() && !identity.value("backend").toObject().isEmpty(),"Invalid frozen certification backend identity");
        source=identity.value("sourceFingerprint").toString();configuration=identity.value("configurationIdentity").toString();
        forceSource=identity.value("excitationSourceHash").toString();forceCode=identity.value("excitationLoadedCodeHash").toString();
    }
    void guard() const {require(QThread::currentThreadId()==owner,"Certification cache must remain on its owner thread");}
    void clear(){records.clear();forces.clear();}
    void discardPending(){
        guard();const bool abort=pendingBank;pendingBank=false;
        pendingCandidates.clear();pendingTimes.clear();immediateResponse={};
        if(abort && pipeline.abort)pipeline.abort();
    }
    void fail() noexcept {
        // Never invoke a QObject-backed abort on a foreign thread.
        if(QThread::currentThreadId()!=owner)std::terminate();
        clear();try{discardPending();}catch(...){}
    }
    double time(uint step) const
    {
        require(step<=target,"Certification FE step exceeds frozen target");
        const double value=step?initialTime+double(step)*dt:initialTime;
        require(std::isfinite(value),"Certification original clock overflow");
        if(step){const double previous=step==1?initialTime:initialTime+double(step-1)*dt;require(value>previous,"Certification original clock interval collapsed");}
        return value;
    }
    template<class Callbacks> void bind(const Callbacks &callbacks)
    {
        guard();require(!activated,"Certification callbacks must be wrapped before preparation");
        require(callbacks.sourceFingerprint==source && callbacks.controllerIdentity==configuration
            && callbacks.forcingIdentity==forceSource+":"+forceCode && callbacks.controlEveryFESteps==N
            && bool(callbacks.perturbationForce) && bool(callbacks.validateOpaque) && bool(callbacks.validateAcceptedBoundary),"Certification callbacks differ from initialized backend");
        require(!callbacks.backendIdentity.isEmpty(),"Certification callback backend identity missing");
        if(bound)require(backendIdentity==callbacks.backendIdentity,"Certification callback backend identity changed");
        backendIdentity=callbacks.backendIdentity;forcingIdentity=callbacks.forcingIdentity;controllerIdentity=callbacks.controllerIdentity;bound=true;
    }
    template<class State> Record record(const State &state,const QByteArray &codec,double actualTime) const
    {
        require(state.sourceFingerprint==source && state.controllerIdentity==controllerIdentity
            && state.forcingIdentity==forcingIdentity && state.backendIdentity==backendIdentity,"Certification runtime source/backend identity changed");
        require(state.configurationIdentity.size()==32,"Certification native configuration identity missing");
        require(state.targetSteps==target && state.controlEveryFESteps==N && state.completedFE<=target
            && state.controlPhase==state.completedFE%N && state.controlTick==uint((quint64(state.completedFE)+N-1)/N)
            && bits(state.initialTime)==bits(initialTime) && bits(state.dt)==bits(dt)
            && bits(state.nominalControlTs)==bits(double(N)*dt) && bits(actualTime)==bits(time(state.completedFE)),"Certification runtime original clock changed");
        const auto opaque=opaqueObject(state.opaqueControllerState);
        require(opaque.value("sourceFingerprint").isString() && opaque.value("sourceFingerprint").toString()==source
            && opaque.value("configurationIdentity").isString() && opaque.value("configurationIdentity").toString()==configuration
            && integer(opaque.value("feStep"),state.completedFE) && integer(opaque.value("controlTick"),state.controlTick)
            && opaque.value("phase").isString() && opaque.value("phase").toString()=="ready","Certification opaque identity/counters changed");
        require(state.heldRequested.size()==state.lastApplied.size(),"Certification held/applied voltage dimensions changed");
        Record result;result.id=hash(codec);result.codec=codec;result.opaque=state.opaqueControllerState;
        result.step=state.completedFE;result.tick=state.controlTick;result.phase=state.controlPhase;
        result.request={{"recordId",result.id},{"opaqueState",opaque},{"feStep",double(result.step)},
            {"controlTick",double(result.tick)},{"phase","ready"},{"appliedVoltages",array(state.lastApplied)},{"heldVoltages",array(state.heldRequested)}};
        return result;
    }
    void responseIdentity(const QJsonObject &response) const
    {
        keys(response,{"sourceFingerprint","configurationIdentity","backend","records","forces"});
        require(response.value("sourceFingerprint").isString() && response.value("sourceFingerprint").toString()==source
            && response.value("configurationIdentity").isString() && response.value("configurationIdentity").toString()==configuration
            && response.value("backend").isObject() && response.value("backend").toObject()==identity.value("backend").toObject(),"Certification backend/source identity changed");
        require(response.value("records").isArray() && response.value("forces").isArray(),"Certification response arrays missing");
    }
    void validateOpaque(const QByteArray &opaque,uint step,uint tick,uint phase) const
    {
        guard();for(const auto &entry:records){const auto &r=entry.second;if(r.step==step && r.tick==tick && r.phase==phase && r.opaque==opaque)return;}
        throw std::invalid_argument("Certification opaque boundary is absent from admitted batch");
    }
    void validateBoundary(const QByteArray &codec) const
    {
        guard();const auto found=records.find(hash(codec));require(found!=records.end() && found->second.codec==codec,"Certification complete boundary is absent from admitted batch");
    }
    std::vector<double> force(double value) const
    {
        guard();require(std::isfinite(value),"Certification force time is nonfinite");const auto found=forces.find(bits(value));
        require(found!=forces.end(),"Certification exact force time is absent from admitted batch");return found->second;
    }
};

PiezoCertificationBatchCache::PiezoCertificationBatchCache(Request request,const QJsonObject &identity,const QString &family,
    double initialTime,double dt,uint target,uint N,size_t forceSize,Pipeline pipeline)
    :impl(std::make_shared<Impl>(std::move(request),identity,family,initialTime,dt,target,N,forceSize,std::move(pipeline))){}
PiezoCertificationBatchCache::~PiezoCertificationBatchCache(){if(impl->pendingBank)impl->fail();}

RStructuralPiezoAxialRuntimeCallbacks PiezoCertificationBatchCache::wrapAxial(const RStructuralPiezoAxialRuntimeCallbacks &original) const
{
    impl->guard();require(impl->family=="axial","Certification cache family mismatch");impl->bind(original);
    auto callbacks=original;const auto cache=impl;
    callbacks.perturbationForce=[cache,fallback=original.perturbationForce](double time){cache->guard();return cache->activated?cache->force(time):fallback(time);};
    callbacks.validateOpaque=[cache,fallback=original.validateOpaque](const QByteArray &state,uint step,uint tick,uint phase){cache->guard();if(cache->activated)cache->validateOpaque(state,step,tick,phase);else fallback(state,step,tick,phase);};
    callbacks.validateAcceptedBoundary=[cache,fallback=original.validateAcceptedBoundary](const RStructuralPiezoAxialRunState &state){cache->guard();if(cache->activated)cache->validateBoundary(RStructuralPiezoAxialRestartCodec::encode(state));else fallback(state);};
    return callbacks;
}
RStructuralPiezoLayeredRuntimeCallbacks PiezoCertificationBatchCache::wrapLayered(const RStructuralPiezoLayeredRuntimeCallbacks &original) const
{
    impl->guard();require(impl->family=="layered","Certification cache family mismatch");impl->bind(original);
    auto callbacks=original;const auto cache=impl;
    callbacks.perturbationForce=[cache,fallback=original.perturbationForce](double time){cache->guard();return cache->activated?cache->force(time):fallback(time);};
    callbacks.validateOpaque=[cache,fallback=original.validateOpaque](const QByteArray &state,uint step,uint tick,uint phase){cache->guard();if(cache->activated)cache->validateOpaque(state,step,tick,phase);else fallback(state,step,tick,phase);};
    callbacks.validateAcceptedBoundary=[cache,fallback=original.validateAcceptedBoundary](const RStructuralPiezoLayeredRunState &state){cache->guard();if(cache->activated)cache->validateBoundary(RStructuralPiezoLayeredRestartCodec::encode(state));else fallback(state);};
    return callbacks;
}

void PiezoCertificationBatchCache::prepare(const std::vector<QByteArray> &payloads)
{beginPrepare(payloads);finishPrepare();}
void PiezoCertificationBatchCache::abortPending(){impl->guard();impl->fail();}
void PiezoCertificationBatchCache::beginPrepare(const std::vector<QByteArray> &payloads)
{
    impl->guard();impl->activated=true;
    try{
        require(!impl->pendingBank,"Certification next bank already pending");
        require(impl->bound && !payloads.empty() && payloads.size()<=128,"Certification batch requires 1..128 bound records");
        size_t total=0;std::vector<Impl::Record> candidates;QJsonArray records,times;std::set<uint> steps;
        std::map<uint,QString> requestedTimes;requestedTimes.emplace(0,bits(impl->time(0)));
        QByteArray nativeConfiguration;
        for(const auto &bytes:payloads){
            require(!bytes.isEmpty() && size_t(bytes.size())<=16*1024*1024,"Certification restart payload exceeds bound");
            total+=size_t(bytes.size());require(total<=16*1024*1024,"Certification batch payloads exceed aggregate bound");
            Impl::Record candidate;QByteArray configuration;
            if(impl->family=="axial"){
                const auto state=RStructuralPiezoAxialRestartCodec::decode(bytes);require(RStructuralPiezoAxialRestartCodec::encode(state)==bytes,"Noncanonical axial certification codec");
                candidate=impl->record(state,bytes,state.physical.time);configuration=state.configurationIdentity;
            }else{
                const auto state=RStructuralPiezoLayeredRestartCodec::decode(bytes);require(RStructuralPiezoLayeredRestartCodec::encode(state)==bytes,"Noncanonical layered certification codec");
                candidate=impl->record(state,bytes,state.physical.integrator.state.time);configuration=state.configurationIdentity;
            }
            if(nativeConfiguration.isEmpty())nativeConfiguration=configuration;
            require(configuration==nativeConfiguration,"Certification native runtime configuration changed within batch");
            require(steps.insert(candidate.step).second,"Certification batch has duplicate FE boundaries");
            records.append(candidate.request);requestedTimes.emplace(candidate.step,bits(impl->time(candidate.step)));candidates.push_back(std::move(candidate));
        }
        require(requestedTimes.size()<=129,"Certification force request bound exceeded");
        for(const auto &entry:requestedTimes)times.append(QJsonObject{{"feStep",double(entry.first)},{"timeBits",entry.second}});
        QJsonObject message{{"op","certificationBatch"},{"records",records},{"times",times}};
        const auto latest=std::max_element(candidates.begin(),candidates.end(),[](const auto &a,const auto &b){return a.step<b.step;});
        if(latest!=candidates.end())message["opaqueState"]=latest->request.value("opaqueState");
        impl->pendingCandidates=std::move(candidates);impl->pendingTimes=std::move(requestedTimes);impl->pendingBank=true;
        if(impl->pipeline.begin)impl->pipeline.begin(message);
        else impl->immediateResponse=impl->request(message); // synchronous compatibility
    }catch(...){impl->fail();throw;}
}
void PiezoCertificationBatchCache::finishPrepare()
{
    impl->guard();
    try{
        require(impl->pendingBank,"Certification next bank is absent");
        const auto response=impl->pipeline.finish?impl->pipeline.finish():impl->immediateResponse;
        impl->responseIdentity(response);
        const auto &candidates=impl->pendingCandidates;const auto &requestedTimes=impl->pendingTimes;
        const auto proofs=response.value("records").toArray(),forceProofs=response.value("forces").toArray();
        require(size_t(proofs.size())==candidates.size() && size_t(forceProofs.size())==requestedTimes.size(),"Certification response is partial or oversized");
        std::map<QString,Impl::Record> admittedRecords;std::map<QString,std::vector<double>> admittedForces;
        for(size_t i=0;i<candidates.size();++i){
            require(proofs[int(i)].isObject(),"Certification record proof is not an object");const auto proof=proofs[int(i)].toObject();
            keys(proof,{"recordId","valid","configurationIdentity","feStep","controlTick","phase"});const auto &candidate=candidates[i];
            require(hex(proof.value("recordId"),64) && proof.value("recordId").toString()==candidate.id
                && proof.value("valid").isBool() && proof.value("valid").toBool()
                && proof.value("configurationIdentity").isString() && proof.value("configurationIdentity").toString()==impl->configuration
                && integer(proof.value("feStep"),candidate.step) && integer(proof.value("controlTick"),candidate.tick)
                && proof.value("phase").isString() && proof.value("phase").toString()=="ready","Certification record proof identity/order/counters mismatch");
            require(admittedRecords.emplace(candidate.id,candidate).second,"Certification record proof duplicated");
        }
        size_t index=0;for(const auto &entry:requestedTimes){
            require(forceProofs[int(index)].isObject(),"Certification force proof is not an object");const auto proof=forceProofs[int(index++)].toObject();
            keys(proof,{"feStep","timeBits","time","forces","sourceHash","loadedCodeHash"});
            require(integer(proof.value("feStep"),entry.first) && hex(proof.value("timeBits"),16) && proof.value("timeBits").toString()==entry.second
                && proof.value("time").isDouble() && std::isfinite(proof.value("time").toDouble()) && proof.value("time").toDouble()==impl->time(entry.first)
                && hex(proof.value("sourceHash"),64) && proof.value("sourceHash").toString()==impl->forceSource
                && hex(proof.value("loadedCodeHash"),64) && proof.value("loadedCodeHash").toString()==impl->forceCode
                && proof.value("forces").isArray(),"Certification force proof identity/time mismatch");
            const auto values=proof.value("forces").toArray();require(size_t(values.size())==impl->forceSize,"Certification force dimensions changed");
            std::vector<double> force;force.reserve(impl->forceSize);for(const auto &value:values){require(value.isDouble() && std::isfinite(value.toDouble()),"Certification force is nonfinite/nonnumeric");force.push_back(value.toDouble());}
            require(admittedForces.emplace(entry.second,std::move(force)).second,"Certification exact force time duplicated");
        }
        // The sole commit point: all response checks and transport post-guard
        // have succeeded, and the caller has finished certifying current.
        impl->records.swap(admittedRecords);impl->forces.swap(admittedForces);
        impl->pendingBank=false;impl->discardPending();
    }catch(...){impl->fail();throw;}
}
void PiezoCertificationBatchCache::recheck()
{
    impl->guard();
    try{
        require(!impl->pendingBank,"Certification source recheck cannot bypass pending bank");
        const auto response=impl->request({{"op","certificationBatch"},{"records",QJsonArray{}},{"times",QJsonArray{}}});impl->responseIdentity(response);
        require(response.value("records").toArray().isEmpty() && response.value("forces").toArray().isEmpty(),"Certification source recheck returned unsolicited proofs");
    }catch(...){impl->activated=true;impl->fail();throw;}
}
