#include <QtTest>
#include <piezo_certification_batch_cache.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rstructural_piezo_layered_restart_codec.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>

namespace {
template<class Operation> bool rejects(Operation operation){try{operation();}catch(const std::exception &){return true;}return false;}
QByteArray bytes(double value){return QByteArray(reinterpret_cast<const char *>(&value),sizeof(value));}
QString hash(const QByteArray &value){return QString::fromLatin1(QCryptographicHash::hash(value,QCryptographicHash::Sha256).toHex());}
QJsonObject identity()
{
    return {{"sourceFingerprint",QString(64,'a')},{"configurationIdentity",QString(64,'b')},
        {"backend",QJsonObject{{"sha256",QString(64,'e')},{"version","fake-certification-test"}}},
        {"excitationSourceHash",QString(64,'c')},{"excitationLoadedCodeHash",QString(64,'d')},
        {"externalConnections",QJsonObject{}}};
}
double timeFromBits(const QString &hex)
{
    const auto raw=QByteArray::fromHex(hex.toLatin1());quint64 word=0;
    for(uint i=0;i<8;++i)word|=quint64(uchar(raw[int(i)]))<<(8*i);double value=0;std::memcpy(&value,&word,8);return value;
}

// These complete codec fixtures exercise only cache admission and byte binding.
// The controlled RPC does not certify private Python semantics or FE physics.
template<bool Axial> struct Fixture
{
    using State=std::conditional_t<Axial,RStructuralPiezoAxialRunState,RStructuralPiezoLayeredRunState>;
    using Codec=std::conditional_t<Axial,RStructuralPiezoAxialRestartCodec,RStructuralPiezoLayeredRestartCodec>;
    using Callbacks=std::conditional_t<Axial,RStructuralPiezoAxialRuntimeCallbacks,RStructuralPiezoLayeredRuntimeCallbacks>;
    QJsonObject frozen=identity(),lastRequest;
    uint calls=0,originalForces=0,originalOpaque=0,originalBoundary=0;
    bool sourceFailure=false;
    std::function<void(QJsonObject &)> tamper;
    std::unique_ptr<PiezoCertificationBatchCache> cache;
    Callbacks original,wrapped;
    QJsonObject deferred;bool rpcPending=false;uint aborted=0;
    Fixture(bool split=false)
    {
        original.sourceFingerprint=frozen.value("sourceFingerprint").toString();original.controllerIdentity=frozen.value("configurationIdentity").toString();
        original.forcingIdentity=frozen.value("excitationSourceHash").toString()+":"+frozen.value("excitationLoadedCodeHash").toString();
        original.backendIdentity="original-native-backend";original.controlEveryFESteps=3;original.authoritativeFullC={1.,2.,3.};
        original.initialOpaqueControllerState="unchanged initial opaque";original.envelopes={{-2.,4.,5.}};
        if constexpr(Axial){
            original.recipeConfiguration="unaltered recipe";original.initialActuatorVoltages={.5};
            original.propose=[](const auto &,const auto &,uint,uint,const QByteArray &){return RStructuralPiezoAxialControllerProposal{{.75},"original proposal","original frame"};};
            original.postFE=[](const auto &,const auto &,uint,uint,const auto &,const QByteArray &){return QByteArray("original postFE");};
        }else {
            original.excitationConfiguration="unaltered excitation";original.controllerConfiguration="unaltered controller";original.designConfiguration="unaltered design";
            original.propose=[](const auto &,uint,uint,const QByteArray &){return RStructuralPiezoLayeredControllerProposal{{.75},"original proposal","original frame"};};
            original.postFE=[](const auto &,uint,uint,const auto &,const QByteArray &){return QByteArray("original postFE");};
        }
        original.perturbationForce=[this](double){++originalForces;return std::vector<double>(3,7.);};
        original.validateOpaque=[this](const QByteArray &,uint,uint,uint){++originalOpaque;};
        original.validateAcceptedBoundary=[this](const State &){++originalBoundary;};
        PiezoCertificationBatchCache::Pipeline pipeline;
        if(split)pipeline={
            [this](const QJsonObject &request){if(rpcPending)throw std::runtime_error("duplicate fake pending request");lastRequest=request;rpcPending=true;},
            [this]{if(!rpcPending)throw std::runtime_error("missing fake pending request");auto answer=rpc(lastRequest);rpcPending=false;return answer;},
            [this]{++aborted;rpcPending=false;}};
        cache=std::make_unique<PiezoCertificationBatchCache>([this](const QJsonObject &request){return rpc(request);},frozen,Axial?"axial":"layered",-0.,.125,8,3,3,std::move(pipeline));
        if constexpr(Axial)wrapped=cache->wrapAxial(original);else wrapped=cache->wrapLayered(original);
    }
    State state(uint step) const
    {
        State result;result.sourceFingerprint=original.sourceFingerprint;result.backendIdentity=original.backendIdentity;
        result.controllerIdentity=original.controllerIdentity;result.forcingIdentity=original.forcingIdentity;
        result.configurationIdentity=QByteArray(32,'n');result.initialTime=-0.;result.dt=.125;result.nominalControlTs=.375;
        result.targetSteps=8;result.completedFE=step;result.controlEveryFESteps=3;result.controlTick=(step+2)/3;result.controlPhase=step%3;
        result.lastApplied={-0.};result.heldRequested={.25};
        const QJsonObject opaque{{"sourceFingerprint",result.sourceFingerprint},{"configurationIdentity",result.controllerIdentity},
            {"feStep",double(step)},{"controlTick",double(result.controlTick)},{"phase","ready"},
            {"previousApplied",QJsonArray{-0.}},{"heldDelta",QJsonArray{.25}}};
        result.opaqueControllerState=QJsonDocument(opaque).toJson(QJsonDocument::Compact);
        if constexpr(Axial)result.physical.time=step?double(step)*.125:-0.;
        else result.physical.integrator.state.time=step?double(step)*.125:-0.;
        // Round-trip through the existing production codec, rather than a
        // replacement parser or abbreviated cache-specific state format.
        return Codec::decode(Codec::encode(result));
    }
    QJsonObject rpc(const QJsonObject &request)
    {
        ++calls;lastRequest=request;if(sourceFailure)throw std::runtime_error("controlled source revision failure");
        QJsonArray records,forces;
        for(const auto value:request.value("records").toArray()){
            const auto entry=value.toObject();records.append(QJsonObject{{"recordId",entry.value("recordId")},{"valid",true},
                {"configurationIdentity",frozen.value("configurationIdentity")},{"feStep",entry.value("feStep")},
                {"controlTick",entry.value("controlTick")},{"phase","ready"}});
        }
        for(const auto value:request.value("times").toArray()){
            const auto entry=value.toObject();const double step=entry.value("feStep").toDouble();
            forces.append(QJsonObject{{"feStep",entry.value("feStep")},{"timeBits",entry.value("timeBits")},
                {"time",timeFromBits(entry.value("timeBits").toString())},{"forces",QJsonArray{step+.25,-0.,-.125*step}},
                {"sourceHash",frozen.value("excitationSourceHash")},{"loadedCodeHash",frozen.value("excitationLoadedCodeHash")}});
        }
        QJsonObject answer{{"sourceFingerprint",frozen.value("sourceFingerprint")},{"configurationIdentity",frozen.value("configurationIdentity")},
            {"backend",frozen.value("backend")},{"records",records},{"forces",forces}};if(tamper)tamper(answer);return answer;
    }
    void prepare(std::initializer_list<uint> steps)
    {
        std::vector<QByteArray> payloads;for(uint step:steps)payloads.push_back(Codec::encode(state(step)));cache->prepare(payloads);
    }
    void validate(const State &s){wrapped.validateOpaque(s.opaqueControllerState,s.completedFE,s.controlTick,s.controlPhase);wrapped.validateAcceptedBoundary(s);}
};

template<bool Axial> void admissionAndExactBinding()
{
    Fixture<Axial> f;const auto s=f.state(4);
    QCOMPARE(f.wrapped.sourceFingerprint,f.original.sourceFingerprint);QCOMPARE(f.wrapped.backendIdentity,f.original.backendIdentity);
    QCOMPARE(f.wrapped.forcingIdentity,f.original.forcingIdentity);QCOMPARE(f.wrapped.controllerIdentity,f.original.controllerIdentity);
    QCOMPARE(f.wrapped.authoritativeFullC,f.original.authoritativeFullC);QCOMPARE(f.wrapped.initialOpaqueControllerState,f.original.initialOpaqueControllerState);
    QCOMPARE(f.wrapped.controlEveryFESteps,f.original.controlEveryFESteps);QCOMPARE(f.wrapped.envelopes.size(),f.original.envelopes.size());
    if constexpr(Axial){QCOMPARE(f.wrapped.recipeConfiguration,f.original.recipeConfiguration);QCOMPARE(f.wrapped.initialActuatorVoltages,f.original.initialActuatorVoltages);}
    else {QCOMPARE(f.wrapped.excitationConfiguration,f.original.excitationConfiguration);QCOMPARE(f.wrapped.controllerConfiguration,f.original.controllerConfiguration);QCOMPARE(f.wrapped.designConfiguration,f.original.designConfiguration);}
    f.validate(s);QCOMPARE(f.wrapped.perturbationForce(-0.),std::vector<double>(3,7.));
    QCOMPARE(f.originalOpaque,1u);QCOMPARE(f.originalBoundary,1u);QCOMPARE(f.originalForces,1u);
    f.prepare({2,4});QCOMPARE(f.calls,1u);QCOMPARE(f.lastRequest.keys(),QStringList({"op","opaqueState","records","times"}));
    QVERIFY(f.lastRequest.value("opaqueState").isObject());
    const auto records=f.lastRequest.value("records").toArray();QCOMPARE(records.size(),2);
    QCOMPARE(records[1].toObject().value("recordId").toString(),hash(Fixture<Axial>::Codec::encode(s)));
    QCOMPARE(records[1].toObject().value("appliedVoltages").toArray().size(),1);
    QCOMPARE(records[1].toObject().value("heldVoltages").toArray()[0].toDouble(),.25);
    const auto times=f.lastRequest.value("times").toArray();QCOMPARE(times.size(),3);
    QCOMPARE(times[0].toObject().value("timeBits").toString(),QString("0000000000000080"));
    f.validate(s);f.validate(s);QCOMPARE(f.calls,1u);
    if constexpr(Axial){
        QCOMPARE(f.wrapped.propose(s.physical,s.readouts,4,2,s.opaqueControllerState).nextOpaqueState,QByteArray("original proposal"));
        QCOMPARE(f.wrapped.postFE(s.physical,s.readouts,4,2,s.lastApplied,s.opaqueControllerState),QByteArray("original postFE"));
    }else {
        QCOMPARE(f.wrapped.propose(s.physical,4,2,s.opaqueControllerState).nextOpaqueState,QByteArray("original proposal"));
        QCOMPARE(f.wrapped.postFE(s.physical,4,2,s.lastApplied,s.opaqueControllerState),QByteArray("original postFE"));
    }
    // Force components retain the existing numeric JSON contract. Qt stores
    // integral zero as +0; binary64 preservation applies to codec and clock keys.
    const auto force=f.wrapped.perturbationForce(.5);QCOMPARE(force[0],4.25);QCOMPARE(bytes(force[1]),bytes(0.));QCOMPARE(force[2],-.5);
    QVERIFY(rejects([&]{f.wrapped.perturbationForce(0.);})); // Signed zero is an exact cache key.
    QVERIFY(rejects([&]{f.wrapped.perturbationForce(std::nextafter(.5,1.));}));
    QVERIFY(rejects([&]{f.wrapped.perturbationForce(.375);}));
    QVERIFY(rejects([&]{f.wrapped.validateOpaque(s.opaqueControllerState,4,1,1);}));
    QVERIFY(rejects([&]{f.wrapped.validateOpaque(s.opaqueControllerState,4,2,0);}));
    QVERIFY(rejects([&]{f.wrapped.validateOpaque(s.opaqueControllerState+" ",4,2,1);}));
    auto bad=s;bad.lastApplied[0]=0.;QVERIFY(rejects([&]{f.wrapped.validateAcceptedBoundary(bad);}));
    bad=s;bad.heldRequested[0]=.5;QVERIFY(rejects([&]{f.wrapped.validateAcceptedBoundary(bad);}));
    bad=s;bad.configurationIdentity[0]='z';QVERIFY(rejects([&]{f.wrapped.validateAcceptedBoundary(bad);}));
    bad=s;bad.opaqueControllerState+=" ";QVERIFY(rejects([&]{f.wrapped.validateAcceptedBoundary(bad);}));
    QVERIFY(rejects([&]{f.validate(f.state(1));}));
    QCOMPARE(f.originalOpaque,1u);QCOMPARE(f.originalBoundary,1u);QCOMPARE(f.originalForces,1u);
    f.cache->recheck();QCOMPARE(f.calls,2u);QVERIFY(f.lastRequest.value("records").toArray().isEmpty());QVERIFY(f.lastRequest.value("times").toArray().isEmpty());
    f.validate(s);QCOMPARE(f.wrapped.perturbationForce(.5),force); // Successful source check preserves proofs.
}

void mutateEntry(QJsonObject &answer,const QString &table,const QString &key,const QJsonValue &value)
{
    auto entries=answer.value(table).toArray();auto entry=entries[0].toObject();entry[key]=value;entries[0]=entry;answer[table]=entries;
}
template<bool Axial> void malformedResponsesAreAtomic()
{
    using Mutation=std::function<void(QJsonObject &)>;
    const std::vector<Mutation> mutations={
        [](auto &a){a["extra"]=true;},[](auto &a){a.remove("backend");},[](auto &a){a["sourceFingerprint"]=QString(64,'f');},
        [](auto &a){a["configurationIdentity"]=QString(64,'f');},[](auto &a){a["backend"]=QJsonObject{{"sha256",QString(64,'e')},{"version","changed"}};},
        [](auto &a){a["records"]=QJsonArray{};},[](auto &a){auto v=a["records"].toArray();v.append(v[0]);a["records"]=v;},
        [](auto &a){auto v=a["records"].toArray();const auto first=v[0];v[0]=v[1];v[1]=first;a["records"]=v;},
        [](auto &a){mutateEntry(a,"records","valid",false);},[](auto &a){mutateEntry(a,"records","valid",1);},
        [](auto &a){mutateEntry(a,"records","recordId",QString(64,'F'));},[](auto &a){mutateEntry(a,"records","configurationIdentity",QString(64,'f'));},
        [](auto &a){mutateEntry(a,"records","controlTick",42);},[](auto &a){mutateEntry(a,"records","feStep",2.5);},
        [](auto &a){mutateEntry(a,"records","phase","after-proposal");},[](auto &a){mutateEntry(a,"records","extra",0);},
        [](auto &a){a["forces"]=QJsonArray{};},[](auto &a){mutateEntry(a,"forces","timeBits","0000000000000000");},
        [](auto &a){mutateEntry(a,"forces","time",1.);},[](auto &a){mutateEntry(a,"forces","feStep",4);},
        [](auto &a){mutateEntry(a,"forces","sourceHash",QString(64,'f'));},[](auto &a){mutateEntry(a,"forces","loadedCodeHash",QString(64,'f'));},
        [](auto &a){mutateEntry(a,"forces","forces",QJsonArray{1.,2.});},[](auto &a){mutateEntry(a,"forces","forces",QJsonArray{1.,"two",3.});},
        [](auto &a){mutateEntry(a,"forces","forces",QJsonArray{1.,QJsonValue(),3.});},[](auto &a){mutateEntry(a,"forces","extra",0);}
    };
    for(const auto &mutate:mutations){
        Fixture<Axial> f(true);f.prepare({1});f.validate(f.state(1));f.tamper=mutate;
        f.cache->beginPrepare({Fixture<Axial>::Codec::encode(f.state(2)),Fixture<Axial>::Codec::encode(f.state(4))});
        f.validate(f.state(1));QVERIFY(rejects([&]{f.validate(f.state(2));}));
        QVERIFY(rejects([&]{f.cache->finishPrepare();}));QVERIFY(!f.rpcPending);QCOMPARE(f.aborted,1u);
        QVERIFY(rejects([&]{f.validate(f.state(1));}));QVERIFY(rejects([&]{f.validate(f.state(2));}));
        QVERIFY(rejects([&]{f.wrapped.perturbationForce(.125);}));QVERIFY(rejects([&]{f.wrapped.perturbationForce(.25);}));
        QCOMPARE(f.originalForces,0u);QCOMPARE(f.originalOpaque,0u);QCOMPARE(f.originalBoundary,0u);
        f.tamper={};f.prepare({4});f.validate(f.state(4));QCOMPARE(f.wrapped.perturbationForce(.5)[0],4.25);
    }
}

template<bool Axial> void invalidInputsAndSourceFailure()
{
    Fixture<Axial> f;f.prepare({2});
    auto invalid=f.state(4);invalid.sourceFingerprint=QString(64,'f');
    QVERIFY(rejects([&]{f.cache->prepare({Fixture<Axial>::Codec::encode(invalid)});}));QCOMPARE(f.calls,1u);
    QVERIFY(rejects([&]{f.validate(f.state(2));}));
    f.prepare({2});invalid=f.state(4);invalid.dt=.25;
    QVERIFY(rejects([&]{f.cache->prepare({Fixture<Axial>::Codec::encode(invalid)});}));
    f.prepare({2});invalid=f.state(4);invalid.controlTick=1;
    QVERIFY(rejects([&]{f.cache->prepare({Fixture<Axial>::Codec::encode(invalid)});}));
    f.prepare({2});invalid=f.state(4);auto opaque=QJsonDocument::fromJson(invalid.opaqueControllerState).object();opaque["configurationIdentity"]=QString(64,'f');invalid.opaqueControllerState=QJsonDocument(opaque).toJson(QJsonDocument::Compact);
    QVERIFY(rejects([&]{f.cache->prepare({Fixture<Axial>::Codec::encode(invalid)});}));
    f.prepare({2});invalid=f.state(4);invalid.configurationIdentity[0]='z';
    QVERIFY(rejects([&]{f.cache->prepare({Fixture<Axial>::Codec::encode(f.state(2)),Fixture<Axial>::Codec::encode(invalid)});}));
    f.prepare({2});auto corrupted=Fixture<Axial>::Codec::encode(f.state(4));corrupted[0]=char(corrupted[0]^1);
    QVERIFY(rejects([&]{f.cache->prepare({corrupted});}));
    QVERIFY(rejects([&]{f.cache->prepare({});}));QVERIFY(rejects([&]{f.prepare({2,2});}));
    std::vector<QByteArray> oversized(129,Fixture<Axial>::Codec::encode(f.state(2)));QVERIFY(rejects([&]{f.cache->prepare(oversized);}));
    const auto callsBeforeBound=f.calls;
    QVERIFY(rejects([&]{f.cache->prepare({QByteArray(16*1024*1024+1,'x')});}));
    auto large=f.state(2);large.lastFrameBytes=QByteArray(9*1024*1024,'x');const auto firstLarge=Fixture<Axial>::Codec::encode(large);
    large=f.state(4);large.lastFrameBytes=QByteArray(9*1024*1024,'x');const auto secondLarge=Fixture<Axial>::Codec::encode(large);
    QVERIFY(rejects([&]{f.cache->prepare({firstLarge,secondLarge});}));QCOMPARE(f.calls,callsBeforeBound);
    f.prepare({2});f.sourceFailure=true;QVERIFY(rejects([&]{f.cache->recheck();}));
    QVERIFY(rejects([&]{f.validate(f.state(2));}));QVERIFY(rejects([&]{f.wrapped.perturbationForce(.25);}));
    f.sourceFailure=false;f.prepare({2});f.sourceFailure=true;QVERIFY(rejects([&]{f.prepare({4});}));
    QVERIFY(rejects([&]{f.validate(f.state(2));}));
    // A failed first prepare also forbids the original validation fallback.
    Fixture<Axial> first;first.sourceFailure=true;QVERIFY(rejects([&]{first.prepare({4});}));
    QVERIFY(rejects([&]{first.validate(first.state(4));}));QVERIFY(rejects([&]{first.wrapped.perturbationForce(.5);}));
    QCOMPARE(first.originalOpaque,0u);QCOMPARE(first.originalBoundary,0u);QCOMPARE(first.originalForces,0u);
}
template<bool Axial> void twoBanksAndPendingFailure()
{
    using Codec=typename Fixture<Axial>::Codec;Fixture<Axial> f(true);f.prepare({1});
    f.cache->beginPrepare({Codec::encode(f.state(2))});QVERIFY(f.rpcPending);
    f.validate(f.state(1));QCOMPARE(f.wrapped.perturbationForce(.125)[0],1.25);
    QVERIFY(rejects([&]{f.validate(f.state(2));}));QVERIFY(rejects([&]{f.wrapped.perturbationForce(.25);}));
    f.cache->finishPrepare();QVERIFY(!f.rpcPending);f.validate(f.state(2));
    QVERIFY(rejects([&]{f.validate(f.state(1));}));
    f.cache->beginPrepare({Codec::encode(f.state(4))});f.sourceFailure=true;
    QVERIFY(rejects([&]{f.cache->finishPrepare();}));QVERIFY(!f.rpcPending);QVERIFY(rejects([&]{f.validate(f.state(2));}));
    f.sourceFailure=false;f.prepare({1});
    QVERIFY(rejects([&]{f.cache->beginPrepare({QByteArray(16*1024*1024+1,'x')});}));
    QVERIFY(!f.rpcPending);QVERIFY(rejects([&]{f.validate(f.state(1));}));
    f.prepare({1});f.cache->beginPrepare({Codec::encode(f.state(2))});
    QVERIFY(rejects([&]{f.cache->beginPrepare({Codec::encode(f.state(4))});}));QVERIFY(!f.rpcPending);
    f.prepare({1});f.cache->beginPrepare({Codec::encode(f.state(2))});f.cache.reset();QVERIFY(!f.rpcPending);
    QVERIFY(rejects([&]{f.validate(f.state(1));}));
}
template<bool Axial> void ownerThreadAndLifetime()
{
    Fixture<Axial> f;bool rejected=false;
    std::thread before([&]{rejected=rejects([&]{f.wrapped.perturbationForce(.5);});});before.join();QVERIFY(rejected);QCOMPARE(f.originalForces,0u);
    f.prepare({4});
    std::thread foreign([&]{
        rejected=rejects([&]{f.prepare({2});}) && rejects([&]{f.cache->recheck();})
            && rejects([&]{f.validate(f.state(4));}) && rejects([&]{f.wrapped.perturbationForce(.5);});
    });foreign.join();QVERIFY(rejected);QCOMPARE(f.calls,1u);f.validate(f.state(4));
    f.cache.reset();f.validate(f.state(4));QCOMPARE(f.wrapped.perturbationForce(.5)[0],4.25); // Callbacks retain cache ownership.
}
}

class TestPiezoCertificationBatchCache:public QObject
{
    Q_OBJECT
private slots:
    void admissionAndExactBinding_data(){QTest::addColumn<bool>("axial");QTest::newRow("axial")<<true;QTest::newRow("layered")<<false;}
    void admissionAndExactBinding(){QFETCH(bool,axial);if(axial)::admissionAndExactBinding<true>();else ::admissionAndExactBinding<false>();}
    void malformedResponsesAreAtomic_data(){admissionAndExactBinding_data();}
    void malformedResponsesAreAtomic(){QFETCH(bool,axial);if(axial)::malformedResponsesAreAtomic<true>();else ::malformedResponsesAreAtomic<false>();}
    void invalidInputsAndSourceFailure_data(){admissionAndExactBinding_data();}
    void invalidInputsAndSourceFailure(){QFETCH(bool,axial);if(axial)::invalidInputsAndSourceFailure<true>();else ::invalidInputsAndSourceFailure<false>();}
    void twoBanksAndPendingFailure_data(){admissionAndExactBinding_data();}
    void twoBanksAndPendingFailure(){QFETCH(bool,axial);if(axial)::twoBanksAndPendingFailure<true>();else ::twoBanksAndPendingFailure<false>();}
    void ownerThreadAndLifetime_data(){admissionAndExactBinding_data();}
    void ownerThreadAndLifetime(){QFETCH(bool,axial);if(axial)::ownerThreadAndLifetime<true>();else ::ownerThreadAndLifetime<false>();}
};
QTEST_APPLESS_MAIN(TestPiezoCertificationBatchCache)
#include "tst_piezo_certification_batch_cache.moc"
