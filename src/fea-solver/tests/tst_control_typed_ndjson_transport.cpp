#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <limits>

#include "control_typed_ndjson_transport.h"

namespace
{
struct PythonCommand
{
    QString program;
    QStringList prefixArguments;
};

PythonCommand pythonCommand()
{
    for (const QString &name : {QStringLiteral("python"),QStringLiteral("python3")})
    {
        const QString executable = QStandardPaths::findExecutable(name);
        if (!executable.isEmpty()) return {executable,{}};
    }
#ifdef Q_OS_WIN
    const QString launcher = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!launcher.isEmpty()) return {launcher,{QStringLiteral("-3")}};
#endif
    return {};
}

QString typedControllerScript()
{
#ifdef RANGE_TYPED_CONTROL_EXAMPLE_PATH
    return QStringLiteral(RANGE_TYPED_CONTROL_EXAMPLE_PATH);
#else
    return QDir(QFileInfo(QString::fromLocal8Bit(__FILE__)).absolutePath())
        .filePath(QStringLiteral("../examples/control_typed_ndjson_python.py"));
#endif
}

ControlTypedTransportConfig config(const PythonCommand &python,
                                   const QString &mode = QStringLiteral("normal"))
{
    ControlTypedTransportConfig value;
    value.program = python.program;
    value.arguments = python.prefixArguments;
    value.arguments << typedControllerScript() << mode
                    << QStringLiteral("{\"force-z\":-0.4,\"beam-voltage\":12.5}");
    value.controllerIdentity = QStringLiteral("python-fixture/revision-2");
    value.deterministicSeed = 17;
    value.fixedDt = 0.01;
    value.configurationSha256 = QByteArray(32,char(0x5a));
    RStructuralTypedControlChannel force;
    force.channelId = QStringLiteral("force-z");
    force.quantity = RStructuralTypedControlQuantity::Force;
    force.minimum = -10.0;
    force.maximum = 10.0;
    force.rateLimit = 100.0;
    force.initialCommand = 0.0;
    RStructuralTypedControlChannel voltage;
    voltage.channelId = QStringLiteral("beam-voltage");
    voltage.quantity = RStructuralTypedControlQuantity::Voltage;
    voltage.minimum = -100.0;
    voltage.maximum = 100.0;
    voltage.initialCommand = 0.0;
    value.channels = {force,voltage};
    value.sensorUnits.insert(QStringLiteral("tip-displacement"),QStringLiteral("m"));
    value.timeoutMs = 1000;
    value.maxLineBytes = 1024*1024;
    return value;
}

RStructuralControlObservation observation(double time = 0.0,
                                          const QByteArray &state = QByteArrayLiteral("4"))
{
    RStructuralControlObservation value;
    value.acceptedTime = time;
    value.nextTime = time+0.01;
    value.deterministicSeed = 17;
    value.sensors = {{QStringLiteral("tip-displacement"),0.25,QStringLiteral("m")}};
    value.priorControllerState = state;
    return value;
}

bool startTransport(ControlTypedNdjsonTransport &transport, QString &error)
{
    if (!QFileInfo::exists(typedControllerScript()))
    {
        error = QStringLiteral("Python typed controller example is missing");
        return false;
    }
    return transport.start(&error);
}
}

class TestControlTypedNdjsonTransport : public QObject
{
    Q_OBJECT

private slots:
    void pythonRoundTripsTypedPureStepsAndOpaqueState();
    void rejectsLocalInvalidObservationsWithoutClosingProcess();
    void rejectsInvalidHandshakes_data();
    void rejectsInvalidHandshakes();
    void rejectsRemoteTypedFaults_data();
    void rejectsRemoteTypedFaults();
    void timesOutAndClosesTypedProcess();
    void keepsTypedControllerStderrSeparate();
};

void TestControlTypedNdjsonTransport::pythonRoundTripsTypedPureStepsAndOpaqueState()
{
    const PythonCommand python = pythonCommand();
    if (python.program.isEmpty()) QSKIP("Python 3 was not found on PATH");
    QVERIFY2(QFileInfo::exists(typedControllerScript()),qPrintable(typedControllerScript()));
    ControlTypedNdjsonTransport transport(config(python));
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    QVERIFY(transport.isReady());

    RStructuralTypedControlDecision first;
    const auto accepted = observation(0.0,QByteArrayLiteral("4"));
    QVERIFY2(transport.compute(1,accepted,first,&error),qPrintable(error));
    QCOMPARE(first.requestedCommands.size(),std::size_t(2));
    QCOMPARE(first.requestedCommands[0].channelId,QStringLiteral("force-z"));
    QVERIFY(first.requestedCommands[0].quantity == RStructuralTypedControlQuantity::Force);
    QCOMPARE(first.requestedCommands[0].value,-0.4);
    QCOMPARE(first.requestedCommands[1].channelId,QStringLiteral("beam-voltage"));
    QVERIFY(first.requestedCommands[1].quantity == RStructuralTypedControlQuantity::Voltage);
    QCOMPARE(first.requestedCommands[1].value,12.5);
    QCOMPARE(first.nextControllerState,QByteArrayLiteral("5"));

    RStructuralTypedControlDecision retry;
    QVERIFY2(transport.compute(1,accepted,retry,&error),qPrintable(error));
    QCOMPARE(retry.requestedCommands[0].value,first.requestedCommands[0].value);
    QCOMPARE(retry.nextControllerState,first.nextControllerState);
    RStructuralTypedControlDecision next;
    QVERIFY2(transport.compute(2,observation(0.01,first.nextControllerState),next,&error),qPrintable(error));
    QCOMPARE(next.nextControllerState,QByteArrayLiteral("6"));
}

void TestControlTypedNdjsonTransport::rejectsLocalInvalidObservationsWithoutClosingProcess()
{
    const PythonCommand python = pythonCommand();
    if (python.program.isEmpty()) QSKIP("Python 3 was not found on PATH");
    ControlTypedNdjsonTransport transport(config(python));
    QString error;
    QVERIFY2(startTransport(transport,error),qPrintable(error));
    RStructuralTypedControlDecision decision;

    auto invalid = observation();
    invalid.deterministicSeed++;
    QVERIFY(!transport.compute(1,invalid,decision,&error));
    QVERIFY(transport.isReady());
    invalid = observation();
    invalid.nextTime += 0.01;
    QVERIFY(!transport.compute(1,invalid,decision,&error));
    QVERIFY(transport.isReady());
    invalid = observation();
    invalid.sensors[0].unit = QStringLiteral("mm");
    QVERIFY(!transport.compute(1,invalid,decision,&error));
    QVERIFY(transport.isReady());
    invalid = observation();
    invalid.sensors[0].value = std::numeric_limits<double>::infinity();
    QVERIFY(!transport.compute(1,invalid,decision,&error));
    QVERIFY(transport.isReady());
    invalid = observation();
    invalid.priorControllerState = QByteArray(1024*1024+1,'s');
    QVERIFY(!transport.compute(1,invalid,decision,&error));
    QVERIFY(transport.isReady());
    QVERIFY2(transport.compute(1,observation(),decision,&error),qPrintable(error));
}

void TestControlTypedNdjsonTransport::rejectsInvalidHandshakes_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("wrong identity") << QStringLiteral("bad-identity");
    QTest::newRow("wrong schema") << QStringLiteral("bad-schema");
    QTest::newRow("wrong channel unit") << QStringLiteral("bad-catalog");
}

void TestControlTypedNdjsonTransport::rejectsInvalidHandshakes()
{
    QFETCH(QString,mode);
    const PythonCommand python = pythonCommand();
    if (python.program.isEmpty()) QSKIP("Python 3 was not found on PATH");
    ControlTypedNdjsonTransport transport(config(python,mode));
    QString error;
    QVERIFY(!transport.start(&error));
    QVERIFY(!transport.isReady());
    QVERIFY(!error.isEmpty());
}

void TestControlTypedNdjsonTransport::rejectsRemoteTypedFaults_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("oversizedLine");
    QTest::newRow("unknown ID") << QStringLiteral("bad-id") << false;
    QTest::newRow("duplicate ID") << QStringLiteral("duplicate") << false;
    QTest::newRow("missing ID") << QStringLiteral("missing") << false;
    QTest::newRow("wrong quantity") << QStringLiteral("bad-quantity") << false;
    QTest::newRow("wrong unit") << QStringLiteral("invalid-unit") << false;
    QTest::newRow("non-finite value") << QStringLiteral("nonfinite") << false;
    QTest::newRow("wrong step") << QStringLiteral("wrong-step") << false;
    QTest::newRow("wrong identity") << QStringLiteral("wrong-identity") << false;
    QTest::newRow("bad base64 state") << QStringLiteral("bad-state") << false;
    QTest::newRow("oversized base64 state") << QStringLiteral("oversized-state") << false;
    QTest::newRow("oversized line") << QStringLiteral("oversized-line") << true;
    QTest::newRow("child exit") << QStringLiteral("exit") << false;
}

void TestControlTypedNdjsonTransport::rejectsRemoteTypedFaults()
{
    QFETCH(QString,mode);
    QFETCH(bool,oversizedLine);
    const PythonCommand python = pythonCommand();
    if (python.program.isEmpty()) QSKIP("Python 3 was not found on PATH");
    ControlTypedTransportConfig setup = config(python,mode);
    if (oversizedLine) setup.maxLineBytes = 1024;
    if (mode == QStringLiteral("oversized-state")) setup.maxLineBytes = 2*1024*1024;
    ControlTypedNdjsonTransport transport(std::move(setup));
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    RStructuralTypedControlDecision decision;
    QVERIFY(!transport.compute(1,observation(),decision,&error));
    QVERIFY(!transport.isReady());
    QVERIFY(!error.isEmpty());
}

void TestControlTypedNdjsonTransport::timesOutAndClosesTypedProcess()
{
    const PythonCommand python = pythonCommand();
    if (python.program.isEmpty()) QSKIP("Python 3 was not found on PATH");
    auto setup = config(python,QStringLiteral("timeout"));
    setup.timeoutMs = 400;
    ControlTypedNdjsonTransport transport(std::move(setup));
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    RStructuralTypedControlDecision decision;
    QVERIFY(!transport.compute(1,observation(),decision,&error));
    QVERIFY(!transport.isReady());
    QVERIFY(error.contains(QStringLiteral("timed out")));
}

void TestControlTypedNdjsonTransport::keepsTypedControllerStderrSeparate()
{
    const PythonCommand python = pythonCommand();
    if (python.program.isEmpty()) QSKIP("Python 3 was not found on PATH");
    ControlTypedNdjsonTransport transport(config(python,QStringLiteral("stderr")));
    QString error;
    QVERIFY2(transport.start(&error),qPrintable(error));
    RStructuralTypedControlDecision decision;
    QVERIFY2(transport.compute(1,observation(),decision,&error),qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(transport.standardErrorTail().contains("typed controller diagnostic"),1000);
}

QTEST_GUILESS_MAIN(TestControlTypedNdjsonTransport)

#include "tst_control_typed_ndjson_transport.moc"
