#include "runtime_control_bridge.h"
#include "runtime_control_json.h"
#include "native_execution_gate.h"
#include <rbl_application_state.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QSet>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
namespace RP = RStructuralControlRuntimeParameters;
using Gate = RuntimeParameterGate;
constexpr qsizetype maximumFrameBytes = 65536;
const QString schema = QStringLiteral("range.runtime-control/v1");
bool exact(const QJsonObject &o, const QStringList &keys)
{
    const auto actual=o.keys();
    return QSet<QString>(actual.begin(), actual.end()) == QSet<QString>(keys.begin(), keys.end());
}
bool uuid(const QString &s) { const QUuid u(s); return !u.isNull() && s.size() == 36 && u.toString(QUuid::WithoutBraces) == s; }
bool decimal(const QJsonValue &v, quint64 &n)
{
    if (!v.isString()) return false;
    bool ok = false; n = v.toString().toULongLong(&ok);
    return ok && QString::number(n) == v.toString();
}
QString errorName(Gate::Error e) {
    switch(e) {
    case Gate::Error::None:return "none"; case Gate::Error::InvalidIdentity:return "invalid-identity";
    case Gate::Error::InvalidProposalId:return "invalid-proposal-id"; case Gate::Error::InvalidParameters:return "invalid-parameters";
    case Gate::Error::RevisionMismatch:return "revision-mismatch"; case Gate::Error::ProposalConflict:return "proposal-conflict";
    case Gate::Error::QueueFull:return "queue-full"; case Gate::Error::HistoryFull:return "history-full";
    case Gate::Error::Shutdown:return "shutdown"; case Gate::Error::UnknownProposal:return "unknown-proposal";
    case Gate::Error::AlreadyFrozen:return "already-frozen"; case Gate::Error::StateMismatch:return "state-mismatch";
    case Gate::Error::Cancelled:return "cancelled"; case Gate::Error::RolledBack:return "rolled-back";
    }
    return "invalid";
}
QString statusName(Gate::Status s) {
    switch(s) { case Gate::Status::Queued:return "queued"; case Gate::Status::Frozen:return "frozen";
    case Gate::Status::Accepted:return "accepted"; case Gate::Status::Rejected:return "rejected"; }
    return "invalid";
}
bool parseParameters(const QJsonValue &v, Gate::Parameters &p) {
    if (!v.isObject()) return false; const auto o = v.toObject();
    if (!exact(o,{"revision","velocityGainNsm","actuatorLimits","fixedBaseControllerRevision","parameterSha256","runtimeControllerRevision"}) ||
        !decimal(o.value("revision"),p.revision) || !o.value("velocityGainNsm").isDouble() || !o.value("actuatorLimits").isArray() ||
        !o.value("fixedBaseControllerRevision").isString() || !o.value("parameterSha256").isString() || !o.value("runtimeControllerRevision").isString()) return false;
    p.velocityGainNsm=o.value("velocityGainNsm").toDouble(); p.fixedBaseControllerRevision=o.value("fixedBaseControllerRevision").toString();
    p.parameterSha256=o.value("parameterSha256").toString(); p.runtimeControllerRevision=o.value("runtimeControllerRevision").toString();
    const auto a=o.value("actuatorLimits").toArray(); if (a.isEmpty() || a.size()>qsizetype(RP::maximumActuatorCount)) return false;
    for (const auto &v:a) {
        if (!v.isObject()) return false; const auto l=v.toObject();
        if (!exact(l,{"actuatorId","minN","maxN","rateNPerS"}) || !l.value("actuatorId").isString() ||
            !l.value("minN").isDouble() || !l.value("maxN").isDouble() || !l.value("rateNPerS").isDouble()) return false;
        p.actuatorLimits.push_back({l.value("actuatorId").toString(),l.value("minN").toDouble(),l.value("maxN").toDouble(),l.value("rateNPerS").toDouble()});
    }
    return true;
}
}

RuntimeControlBridge::RuntimeControlBridge(Config config, std::shared_ptr<NativeExecutionGate> execution, QObject *parent)
    : QObject(parent), config_(std::move(config)), execution_(std::move(execution))
{
    socket_.setReadBufferSize(maximumFrameBytes*2);
    QObject::connect(&socket_, &QTcpSocket::readyRead, this, [this]{ if (ready_) readFrames(); });
    QObject::connect(&socket_, &QTcpSocket::disconnected, this, [this]{ disconnected(); });
}

RuntimeControlBridge::~RuntimeControlBridge()
{
    stopping_=true;
    QObject::disconnect(&socket_,nullptr,this,nullptr);
    socket_.abort();
    if (auto gate=parameterGate()) gate->shutdown();
}

std::optional<RuntimeControlBridge::Config> RuntimeControlBridge::environmentConfig()
{
    const QByteArray port=qgetenv("RANGE_RUNTIME_CONTROL_PORT"), token=qgetenv("RANGE_RUNTIME_CONTROL_TOKEN"),
        run=qgetenv("RANGE_RUNTIME_CONTROL_RUN_ID"), gain=qgetenv("RANGE_RUNTIME_CONTROL_GAIN_NSM"),
        job=qgetenv("RANGE_RUNTIME_CONTROL_JOB_ID");
    if (port.isEmpty() && token.isEmpty() && run.isEmpty() && gain.isEmpty() && job.isEmpty()) return {};
    bool portOk=false,gainOk=false; const uint p=port.toUInt(&portOk); const double g=gain.toDouble(&gainOk);
    bool tokenOk=token.size()==64; for(char c:token) tokenOk = tokenOk && ((c>='0'&&c<='9')||(c>='a'&&c<='f'));
    if (!portOk || p==0 || p>65535 || QByteArray::number(p)!=port || !tokenOk || run.isEmpty() || run.size()>128 ||
        job.isEmpty() || job.size()>128 || !gainOk || !std::isfinite(g)) throw std::invalid_argument("Invalid runtime control environment opt-in");
    return Config{quint16(p),token,QString::fromUtf8(run),g,5000,{},QString::fromUtf8(job)};
}

QJsonObject RuntimeControlBridge::identityJson(const Gate::Identity &i)
{
    return {{"jobId",i.jobId},{"runId",i.runId},{"taskId",i.taskId},{"modelSha256",i.modelSha256},{"baseControllerRevision",i.baseControllerRevision}};
}
QJsonObject RuntimeControlBridge::parametersJson(const Gate::Parameters &p)
{
    QJsonArray a; for(const auto &l:p.actuatorLimits) a.append(QJsonObject{{"actuatorId",l.actuatorId},{"minN",l.minN},{"maxN",l.maxN},{"rateNPerS",l.rateNPerS}});
    return {{"revision",QString::number(p.revision)},{"velocityGainNsm",p.velocityGainNsm},{"actuatorLimits",a},
            {"fixedBaseControllerRevision",p.fixedBaseControllerRevision},{"parameterSha256",p.parameterSha256},{"runtimeControllerRevision",p.runtimeControllerRevision}};
}
bool RuntimeControlBridge::strictJson(const QByteArray &b, QJsonObject &o)
{
    return RuntimeControlJson::parse(b,o);
}
std::shared_ptr<Gate> RuntimeControlBridge::parameterGate() const
{
    std::lock_guard<std::mutex> lock(mutex_); return gate_;
}
bool RuntimeControlBridge::initialize(Gate::Identity i, Gate::State initial, std::vector<Gate::Limit> limits, QString *error)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gate_) { if(error)*error="Runtime bridge already initialized"; return false; }
        gate_=std::make_shared<Gate>(i,std::move(initial),std::move(limits)); identity_=std::move(i);
    }
    bool ok=false; QString detail;
    auto action=[&]{ok=initializeOnMain(&detail);};
    if (QThread::currentThread()==thread()) action();
    else if (!QMetaObject::invokeMethod(this,action,Qt::BlockingQueuedConnection)) detail="Runtime bridge main loop unavailable";
    if(error)*error=detail; return ok;
}
QJsonObject RuntimeControlBridge::envelope(const QString &op,const QString &requestId) const
{
    return {{"schema",schema},{"op",op},{"requestId",requestId},{"identity",identityJson(identity_)}};
}
bool RuntimeControlBridge::initializeOnMain(QString *error)
{
    auto fail=[&]{ config_.token.fill('\0'); config_.token.clear(); disconnected(); socket_.abort(); if(error)*error="Runtime control handshake failed"; return false; };
    socket_.connectToHost(QHostAddress::LocalHost,config_.port);
    if (!socket_.waitForConnected(config_.timeoutMs)) return fail();
    const auto rid=QUuid::createUuid().toString(QUuid::WithoutBraces); auto hello=envelope("hello",rid);
    hello.insert("token",QString::fromLatin1(config_.token)); config_.token.fill('\0'); config_.token.clear();
    hello.insert("acceptedState",QJsonDocument::fromJson(RP::serialize(parameterGate()->acceptedState())).object());
    if (!send(hello)) return fail();
    QElapsedTimer timer; timer.start();
    while (!input_.contains('\n')) {
        input_+=socket_.readAll();
        if(input_.contains('\n')) break;
        if(input_.size()>maximumFrameBytes) return fail();
        const int remaining=config_.timeoutMs-int(timer.elapsed());
        if(remaining<=0 || !socket_.waitForReadyRead(remaining)) return fail();
    }
    const auto end=input_.indexOf('\n'); QJsonObject reply;
    if (end+1>maximumFrameBytes || !strictJson(input_.left(end),reply) || !exact(reply,{"schema","op","requestId","identity","ok"}) ||
        reply.value("schema")!=schema || reply.value("op")!="ready" || reply.value("requestId")!=rid ||
        reply.value("identity")!=identityJson(identity_) || reply.value("ok")!=true) return fail();
    input_.remove(0,end+1); ready_=true;
    if(!input_.isEmpty()) readFrames();
    return ready_;
}
bool RuntimeControlBridge::send(const QJsonObject &frame)
{
    const auto b=QJsonDocument(frame).toJson(QJsonDocument::Compact)+'\n';
    if(b.size()>maximumFrameBytes || socket_.state()!=QAbstractSocket::ConnectedState || socket_.bytesToWrite()>maximumFrameBytes*2) return false;
    if (socket_.write(b)!=b.size()) return false;
    socket_.flush();
    return socket_.state()==QAbstractSocket::ConnectedState;
}
QJsonObject RuntimeControlBridge::receiptJson(const Gate::Receipt &r) const
{
    QJsonObject o{{"proposalId",r.proposalId},{"parameterRevision",QString::number(r.parameterRevision)},
        {"parameterSha256",r.parameterSha256},{"runtimeControllerRevision",r.runtimeControllerRevision},
        {"status",statusName(r.status)},{"reason",errorName(r.reason)},{"provenance",r.restored?"restored":"currentAttempt"}};
    if(r.status==Gate::Status::Accepted && r.appliedSeq && r.acceptedTimeS) {
        o.insert("appliedSeq",QString::number(*r.appliedSeq)); o.insert("acceptedTimeS",*r.acceptedTimeS);
    }
    return o;
}
bool RuntimeControlBridge::publishOnMain(const Gate::Receipt &r)
{
    if(!ready_) return false;
    auto frame=envelope("receipt",QUuid::createUuid().toString(QUuid::WithoutBraces)); frame.insert("receipt",receiptJson(r));
    if(r.status==Gate::Status::Accepted) frame.insert("acceptedState",QJsonDocument::fromJson(RP::serialize(parameterGate()->acceptedState())).object());
    return send(frame);
}
bool RuntimeControlBridge::publish(const Gate::Receipt &r,QString *error)
{
    bool ok=false; auto action=[&]{ok=publishOnMain(r); if(!ok)disconnected();};
    if(QThread::currentThread()==thread()) action(); else QMetaObject::invokeMethod(this,action,Qt::BlockingQueuedConnection);
    if(!ok && error)*error="Cannot publish authoritative runtime parameter receipt";
    return ok;
}
void RuntimeControlBridge::readFrames()
{
    input_+=socket_.readAll();
    for(int count=0; count<128; ++count) {
        const auto end=input_.indexOf('\n');
        if(end<0) { if(input_.size()>maximumFrameBytes) {disconnected();socket_.abort();} return; }
        QJsonObject f; if(end+1>maximumFrameBytes || !strictJson(input_.left(end),f)) {disconnected();socket_.abort();return;}
        input_.remove(0,end+1); command(f); if(!ready_)return;
    }
    if(!input_.isEmpty()) {disconnected();socket_.abort();} // bounded command burst
}
void RuntimeControlBridge::command(const QJsonObject &f)
{
    const auto rid=f.value("requestId").toString(), op=f.value("op").toString();
    if(f.value("schema")!=schema || !uuid(rid) || f.value("identity")!=identityJson(identity_)) {disconnected();socket_.abort();return;}
    QStringList keys{"schema","op","requestId","identity"}; Gate::Result result; QString reason="invalid-command"; bool ok=false;
    const auto gate=parameterGate();
    if(op=="enqueue") {
        keys.append({"proposalId","expectedParameterRevision","parameters"}); Gate::Request r; r.identity=identity_;
        r.proposalId=f.value("proposalId").toString();
        if(exact(f,keys) && decimal(f.value("expectedParameterRevision"),r.expectedParameterRevision) && parseParameters(f.value("parameters"),r.candidateParameters)) {
            result=gate->enqueue(r); reason=errorName(result.error); ok=result.error==Gate::Error::None;
        }
    } else if(op=="query" || op=="cancel-proposal") {
        keys.append("proposalId");
        if(exact(f,keys) && uuid(f.value("proposalId").toString())) {
            if(op=="query") {result.receipt=gate->query(f.value("proposalId").toString());result.error=result.receipt?Gate::Error::None:Gate::Error::UnknownProposal;}
            else result=gate->cancel(f.value("proposalId").toString());
            reason=errorName(result.error);ok=result.error==Gate::Error::None;
        }
    } else if(exact(f,keys) && execution_) {
        if(op=="pause-next") ok=execution_->requestPauseNext();
        else if(op=="resume") ok=execution_->requestResume();
        else if(op=="cancel-run") {ok=execution_->requestCancel();gate->shutdown();RApplicationState::getInstance().setStateType(R_APPLICATION_STATE_STOP);}
        reason=ok?"none":"execution-state-mismatch";
    }
    auto reply=envelope("reply",rid);reply.insert("command",op);reply.insert("ok",ok);reply.insert("reason",reason);
    if(result.receipt) {
        reply.insert("receipt",receiptJson(*result.receipt));
        if(result.receipt->status==Gate::Status::Accepted) reply.insert("acceptedState",QJsonDocument::fromJson(RP::serialize(gate->acceptedState())).object());
    }
    if(!send(reply)) {disconnected();socket_.abort();}
}
void RuntimeControlBridge::disconnected()
{
    ready_=false; const auto gate=parameterGate(); if(gate)gate->shutdown();
    if(!stopping_) { if(execution_)execution_->requestCancel(); RApplicationState::getInstance().setStateType(R_APPLICATION_STATE_STOP); }
}
void RuntimeControlBridge::stop()
{
    auto action=[this]{stopping_=true;ready_=false;if(auto gate=parameterGate())gate->shutdown();socket_.disconnectFromHost();};
    if(QThread::currentThread()==thread()) action();else QMetaObject::invokeMethod(this,action,Qt::BlockingQueuedConnection);
}
