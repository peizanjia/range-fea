#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QHostAddress>
#include <rbl_application_state.h>
#include "runtime_control_bridge.h"
#include "native_execution_gate.h"
#include "native_accepted_checkpoint.h"
#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <limits>

namespace {
namespace RP=RStructuralControlRuntimeParameters;
using Gate=RuntimeParameterGate;
Gate::Identity identity() {return {"job-1","run-1","task-1",QString(64,'a'),QString(64,'b')};}
std::vector<Gate::Limit> limits() {return {{"force-1",-100,100,10}};}
QString uuid(int i) {return QStringLiteral("00000000-0000-4000-8000-%1").arg(i,12,16,QLatin1Char('0'));}
QJsonObject command(const QString &op,int request=1) {
    return {{"schema","range.runtime-control/v1"},{"op",op},{"requestId",uuid(request)},{"identity",RuntimeControlBridge::identityJson(identity())}};
}
QJsonObject enqueue(const Gate::State &s,int proposal=1,int request=1) {
    auto f=command("enqueue",request); f.insert("proposalId",uuid(proposal));f.insert("expectedParameterRevision",QString::number(s.parameters.revision));
    f.insert("parameters",RuntimeControlBridge::parametersJson(RP::candidate(s,2+proposal,limits())));return f;
}
// A real loopback peer owns its sockets on a separate thread. Its finite reads
// let the test application's event loop continue delivering bridge callbacks.
class Host {
    std::mutex mutex; std::deque<QByteArray> outgoing; std::vector<QJsonObject> received;
    std::atomic<bool> stopping{false}, disconnectRequest{false}; std::thread worker;
public:
    quint16 port=0;
    Host() {
        std::promise<quint16> listening; auto ready=listening.get_future();
        worker=std::thread([this,&listening] {
            QTcpServer server;
            if(!server.listen(QHostAddress::LocalHost,0)) {listening.set_value(0);return;}
            listening.set_value(server.serverPort());
            while(!stopping && !server.hasPendingConnections())server.waitForNewConnection(10);
            if(stopping)return;
            std::unique_ptr<QTcpSocket> peer(server.nextPendingConnection()); QByteArray input;
            while(!stopping && peer->state()==QAbstractSocket::ConnectedState) {
                if(disconnectRequest) {peer->abort();return;}
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while(!outgoing.empty()) {peer->write(outgoing.front());outgoing.pop_front();}peer->flush();
                }
                peer->waitForReadyRead(10);input+=peer->readAll();
                while(input.contains('\n')) {
                    const auto end=input.indexOf('\n');const auto f=QJsonDocument::fromJson(input.left(end)).object();input.remove(0,end+1);
                    if(f.value("op")=="hello") {
                        auto response=f;response.remove("token");response.remove("acceptedState");response.insert("op","ready");response.insert("ok",true);
                        peer->write(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');peer->flush();
                    }
                    std::lock_guard<std::mutex> lock(mutex);received.push_back(f);
                }
            }
        });port=ready.get();
    }
    ~Host() {stopping=true;worker.join();}
    void send(const QJsonObject &f) {sendRaw(QJsonDocument(f).toJson(QJsonDocument::Compact)+'\n');}
    void sendRaw(QByteArray b) {std::lock_guard<std::mutex> lock(mutex);outgoing.push_back(std::move(b));}
    void disconnect() {disconnectRequest=true;}
    int count() {std::lock_guard<std::mutex> lock(mutex);return int(received.size());}
    QJsonObject at(int i) {std::lock_guard<std::mutex> lock(mutex);return received.at(std::size_t(i));}
};
std::shared_ptr<NativeExecutionGate> execution(const QString &directory) {
    NativeAcceptedCheckpointContext c;
    c.sourceSha256=QString(64,'a');c.modelSha256=QString(64,'a');c.controlSha256=QString(64,'b');
    c.runtimeSha256=QString(64,'c');c.packageSha256=QString(64,'d');c.dtS=0.1;
    c.policy="accepted-steps-or-sim-time-v1";c.maxAcceptedSteps=10;c.maxSimulationSeconds=1.0;
    NativeExecutionGate::Config config;config.stateFileName=directory+"/execution.json";config.jobId="job-1";config.taskId="task-1";
    config.checkpointContextJson=QJsonDocument(c.toJsonObject()).toJson(QJsonDocument::Compact);
    auto e=NativeExecutionGate::create(config); if(e)e->configureAcceptedRange(0,10,0);return e;
}
struct BoundaryWorker {
    std::shared_ptr<NativeExecutionGate> gate;
    std::atomic<bool> returned{false};
    std::thread thread;
    BoundaryWorker(std::shared_ptr<NativeExecutionGate> e,quint64 sequence,double time):gate(std::move(e)),
        thread([this,sequence,time]{gate->acceptedBoundary(sequence,time,{});returned=true;}) {}
    ~BoundaryWorker() {if(thread.joinable()){gate->requestCancel();thread.join();}}
};
}

class RuntimeControlBridgeTest:public QObject {
    Q_OBJECT
private slots:
    void init() {RApplicationState::getInstance().setStateType(R_APPLICATION_STATE_RUN);}
    void strictParser() {
        QJsonObject o;
        QVERIFY(RuntimeControlBridge::strictJson("{\"x\":1,\"object\":{\"x\":2}}",o));
        for(const QByteArray &b:{QByteArray("{\"x\":1,\"x\":2}"),QByteArray("{\"x\":1,\"\\u0078\":2}"),
            QByteArray("{\"nested\":{\"x\":1,\"x\":2}}"),QByteArray("{\"x\":1e999}"),QByteArray("{\"x\":null}"),QByteArray("{} {}")})
            QVERIFY(!RuntimeControlBridge::strictJson(b,o));
        QVERIFY(!RuntimeControlBridge::strictJson(QByteArray("{\"x\":\"")+char(0xff)+"\"}",o));
    }
    void loopbackCorrelationAndTwoCommits() {
        Host host;QVERIFY(host.port);
        RuntimeControlBridge bridge({host.port,QByteArray(64,'c'),"run-1",1,1000},{});
        auto s=RP::fresh(identity().baseControllerRevision,1,limits());QVERIFY(bridge.initialize(identity(),s,limits()));
        QTRY_COMPARE(host.count(),1);QCOMPARE(host.at(0).value("acceptedState").toObject(),QJsonDocument::fromJson(RP::serialize(s)).object());
        for(int i=1;i<=2;++i) {
            host.send(enqueue(s,i,i));QTRY_COMPARE(host.count(),2*i);
            const auto reply=host.at(2*i-1);QCOMPARE(reply.value("requestId").toString(),uuid(i));QCOMPARE(reply.value("ok"),QJsonValue(true));
            auto receipt=reply.value("receipt").toObject();QCOMPARE(receipt.value("status").toString(),QString("queued"));QVERIFY(!receipt.contains("appliedSeq"));
            const auto frozen=bridge.parameterGate()->freezeCandidate(s);QVERIFY(frozen);
            s=RP::acceptedChange(s,frozen->proposalId,frozen->parameters,quint64(i),double(i)/10);
            const auto result=bridge.parameterGate()->onCommitted(s);QVERIFY(result.receipt);QVERIFY(bridge.publish(*result.receipt));
            QTRY_COMPARE(host.count(),2*i+1);const auto accepted=host.at(2*i);
            QCOMPARE(accepted.value("receipt").toObject().value("appliedSeq").toString(),QString::number(i));
            QCOMPARE(accepted.value("acceptedState").toObject(),QJsonDocument::fromJson(RP::serialize(s)).object());
        }
        bridge.stop();
    }
    void unknownKeysRejectWithoutQueueAndIdentityDisconnect() {
        Host host;auto s=RP::fresh(identity().baseControllerRevision,1,limits());
        RuntimeControlBridge bridge({host.port,QByteArray(64,'c'),"run-1",1,1000},{});QVERIFY(bridge.initialize(identity(),s,limits()));
        auto f=enqueue(s);f.insert("unknown",true);host.send(f);QTRY_COMPARE(host.count(),2);QCOMPARE(host.at(1).value("ok"),QJsonValue(false));
        QVERIFY(!bridge.parameterGate()->query(uuid(1)));
        f=enqueue(s);auto wrong=identity();wrong.runId="wrong";f.insert("identity",RuntimeControlBridge::identityJson(wrong));host.send(f);
        QTRY_COMPARE(RApplicationState::getInstance().getStateType(),R_APPLICATION_STATE_STOP);
        QCOMPARE(bridge.parameterGate()->enqueue({identity(),uuid(2),0,RP::candidate(s,2,limits())}).error,Gate::Error::Shutdown);
    }
    void duplicateFrameDisconnects() {
        Host host;auto s=RP::fresh(identity().baseControllerRevision,1,limits());
        RuntimeControlBridge bridge({host.port,QByteArray(64,'c'),"run-1",1,1000},{});QVERIFY(bridge.initialize(identity(),s,limits()));
        auto b=QJsonDocument(command("query")).toJson(QJsonDocument::Compact);b.chop(1);b+=" ,\"op\":\"resume\"}\n";host.sendRaw(b);
        QTRY_COMPARE(RApplicationState::getInstance().getStateType(),R_APPLICATION_STATE_STOP);
    }
    void restoredReceiptAndQueuedDisconnect() {
        Host host;auto s=RP::fresh(identity().baseControllerRevision,1,limits());
        s=RP::acceptedChange(s,uuid(1),RP::candidate(s,2,limits()),1,0.1);
        RuntimeControlBridge bridge({host.port,QByteArray(64,'c'),"run-1",1,1000},{});QVERIFY(bridge.initialize(identity(),s,limits()));
        auto f=command("query");f.insert("proposalId",uuid(1));host.send(f);QTRY_COMPARE(host.count(),2);
        QCOMPARE(host.at(1).value("receipt").toObject().value("provenance").toString(),QString("restored"));
        host.send(enqueue(s,2,2));QTRY_COMPARE(host.count(),3);host.disconnect();
        QTRY_COMPARE(bridge.parameterGate()->query(uuid(2))->reason,Gate::Error::Shutdown);
        QCOMPARE(RP::serialize(bridge.parameterGate()->acceptedState()),RP::serialize(s));
    }
    void pauseResumeAndDisconnectWake() {
        QTemporaryDir directory;QVERIFY(directory.isValid());auto e=execution(directory.path());QVERIFY(e);
        Host host;auto s=RP::fresh(identity().baseControllerRevision,1,limits());
        RuntimeControlBridge bridge({host.port,QByteArray(64,'c'),"run-1",1,1000},e);QVERIFY(bridge.initialize(identity(),s,limits()));
        host.send(command("pause-next",1));QTRY_COMPARE(host.count(),2);QVERIFY(e->hasPauseAt(1));
        BoundaryWorker solver(e,1,0.1);
        QTRY_VERIFY(QFileInfo::exists(directory.path()+"/execution.json"));QVERIFY(!solver.returned);
        host.send(command("resume",2));QTRY_VERIFY(solver.returned);solver.thread.join();QTRY_COMPARE(host.count(),3);
        host.send(command("pause-next",3));QTRY_COMPARE(host.count(),4);QVERIFY(e->hasPauseAt(2));
        BoundaryWorker paused(e,2,0.2);
        QTest::qWait(50);QVERIFY(!paused.returned);host.disconnect();QTRY_VERIFY(paused.returned);paused.thread.join();QVERIFY(e->isCancelled());
    }
    void executionRejectsInvalidBoundary_data() {
        QTest::addColumn<int>("kind");QTest::newRow("nonfinite-time")<<0;QTest::newRow("same-time")<<1;QTest::newRow("sequence-gap")<<2;
    }
    void executionRejectsInvalidBoundary() {
        QFETCH(int,kind);QTemporaryDir directory;QVERIFY(directory.isValid());auto e=execution(directory.path());QVERIFY(e);
        QString error;
        QVERIFY(!e->acceptedBoundary(kind==2?2:1,kind==0?std::numeric_limits<double>::infinity():(kind==1?0:0.1),{},&error));
        QVERIFY(!error.isEmpty());QVERIFY(e->isCancelled());
    }
};
QTEST_GUILESS_MAIN(RuntimeControlBridgeTest)
#include "tst_runtime_control_bridge.moc"
