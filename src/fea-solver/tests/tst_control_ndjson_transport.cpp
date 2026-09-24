#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <limits>

#include "control_ndjson_transport.h"

namespace
{
QString fixturePath()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("control_ndjson_fixture.exe"));
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("control_ndjson_fixture"));
#endif
}

ControlTransportConfig config(const QString &mode = QStringLiteral("normal"))
{
    ControlTransportConfig value;
    value.program = fixturePath();
    value.arguments = {mode};
    value.controllerId = QStringLiteral("fixture-controller");
    value.controllerRevision = QString(64,QLatin1Char('a'));
    value.modelSnapshotSha256 = QString(64,QLatin1Char('b'));
    value.deterministicSeed = 17;
    value.sensorUnits.insert(QStringLiteral("tip-displacement"),QStringLiteral("m"));
    value.actuatorIds = {QStringLiteral("tip-force")};
    value.timeoutMs = 1000;
    return value;
}

QVector<ControlObservation> observations(double value = 0.25)
{
    return {{QStringLiteral("tip-displacement"),value,QStringLiteral("m")}};
}
}

class TestControlNdjsonTransport : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsPureStepsAndKeepsAcceptedStateWithCaller();
    void rejectsLocalInvalidDataWithoutWritingToProcess();
    void rejectsInvalidHandshake();
    void rejectsRemoteFaults_data();
    void rejectsRemoteFaults();
    void timesOutAndClosesProcess();
    void keepsStderrSeparateFromProtocol();
};

void TestControlNdjsonTransport::roundTripsPureStepsAndKeepsAcceptedStateWithCaller()
{
    QVERIFY(QFileInfo::exists(fixturePath()));
    ControlNdjsonTransport transport(config());
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    QVERIFY(transport.isReady());
    ControlStepReply first;
    const QJsonObject accepted{{QStringLiteral("count"),4},{QStringLiteral("bias"),0.1}};
    QVERIFY2(transport.compute(1,0.0,0.01,observations(),accepted,first,&error),qPrintable(error));
    QCOMPARE(first.commands.size(),1);
    QCOMPARE(first.commands[0].actuatorId,QStringLiteral("tip-force"));
    QCOMPARE(first.commands[0].force,-0.4);
    QCOMPARE(first.nextControllerState.value(QStringLiteral("count")).toInt(),5);
    ControlStepReply retry;
    QVERIFY2(transport.compute(1,0.0,0.01,observations(),accepted,retry,&error),qPrintable(error));
    QCOMPARE(retry.commands[0].force,first.commands[0].force);
    QCOMPARE(retry.nextControllerState,first.nextControllerState);
    ControlStepReply acceptedNext;
    QVERIFY2(transport.compute(2,0.01,0.01,observations(),first.nextControllerState,
                               acceptedNext,&error),qPrintable(error));
    QCOMPARE(acceptedNext.nextControllerState.value(QStringLiteral("count")).toInt(),6);
}

void TestControlNdjsonTransport::rejectsLocalInvalidDataWithoutWritingToProcess()
{
    ControlNdjsonTransport transport(config());
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    ControlStepReply reply;
    QVERIFY(!transport.compute(1,0.0,-0.01,observations(),{},reply,&error));
    QVERIFY(transport.isReady());
    QVERIFY(!transport.compute(1,0.0,0.01,observations(std::numeric_limits<double>::infinity()),{},reply,&error));
    QVERIFY(transport.isReady());
    QVERIFY(!transport.compute(1,0.0,0.01,
        {{QStringLiteral("unknown"),0.0,QStringLiteral("m")}}, {}, reply,&error));
    QVERIFY(transport.isReady());
    QVERIFY(!transport.compute(1,0.0,0.01,observations(),
        {{QStringLiteral("x"),QJsonValue(std::numeric_limits<double>::infinity())}},reply,&error));
    QVERIFY(transport.isReady());
    QVERIFY2(transport.compute(1,0.0,0.01,observations(),{},reply,&error),qPrintable(error));
}

void TestControlNdjsonTransport::rejectsInvalidHandshake()
{
    ControlNdjsonTransport transport(config(QStringLiteral("bad-handshake")));
    QString error;
    QVERIFY(!transport.start(&error));
    QVERIFY(!transport.isReady());
    QVERIFY(error.contains(QStringLiteral("pure")));
    ControlNdjsonTransport wrongIdentity(config(QStringLiteral("bad-identity")));
    QVERIFY(!wrongIdentity.start(&error));
    QVERIFY(!wrongIdentity.isReady());
    QVERIFY(error.contains(QStringLiteral("handshake")));
}

void TestControlNdjsonTransport::rejectsRemoteFaults_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("handshakeFault");
    QTest::newRow("unknown actuator") << QStringLiteral("bad-id") << false;
    QTest::newRow("wrong force type") << QStringLiteral("bad-force") << false;
    QTest::newRow("duplicate actuator") << QStringLiteral("duplicate") << false;
    QTest::newRow("wrong step") << QStringLiteral("wrong-step") << false;
    QTest::newRow("wrong request") << QStringLiteral("wrong-request") << true;
    QTest::newRow("oversized reply") << QStringLiteral("oversize") << false;
    QTest::newRow("child exit") << QStringLiteral("exit") << false;
}

void TestControlNdjsonTransport::rejectsRemoteFaults()
{
    QFETCH(QString,mode);
    QFETCH(bool,handshakeFault);
    ControlTransportConfig setup = config(mode);
    if (mode == QStringLiteral("oversize")) setup.maxLineBytes = 1024;
    ControlNdjsonTransport transport(setup);
    QString error;
    if (handshakeFault)
    {
        QVERIFY(!transport.start(&error));
    }
    else
    {
        QVERIFY2(transport.start(&error),qPrintable(error));
        ControlStepReply reply;
        QVERIFY(!transport.compute(1,0.0,0.01,observations(),{},reply,&error));
    }
    QVERIFY(!error.isEmpty());
    QVERIFY(!transport.isReady());
}

void TestControlNdjsonTransport::timesOutAndClosesProcess()
{
    ControlTransportConfig setup = config(QStringLiteral("timeout"));
    setup.timeoutMs = 500;
    ControlNdjsonTransport transport(setup);
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    ControlStepReply reply;
    QVERIFY(!transport.compute(1,0.0,0.01,observations(),{},reply,&error));
    QVERIFY(!transport.isReady());
    QVERIFY(error.contains(QStringLiteral("timed out")));
}

void TestControlNdjsonTransport::keepsStderrSeparateFromProtocol()
{
    ControlNdjsonTransport transport(config(QStringLiteral("stderr")));
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    ControlStepReply reply;
    QVERIFY2(transport.compute(1,0.0,0.01,observations(),{},reply,&error),qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(transport.standardErrorTail().contains("controller diagnostic"),1000);
}

QTEST_GUILESS_MAIN(TestControlNdjsonTransport)
#include "tst_control_ndjson_transport.moc"
