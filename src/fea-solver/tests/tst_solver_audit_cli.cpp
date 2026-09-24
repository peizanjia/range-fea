#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "execution_event_journal.h"
#include "rml_model.h"

namespace
{
QString sha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

RModel makeModel()
{
    RModel model;
    model.setNNodes(2);
    model.getNode(0).set(0.0,0.0,0.0);
    model.getNode(1).set(2.0,0.0,0.0);
    RElement bar(R_ELEMENT_TRUSS1);
    bar.setNodeId(0,0);
    bar.setNodeId(1,1);
    model.addElement(bar,true,0);

    RMaterial material(RMaterial::Solid);
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15,2.0e11);
    material.add(modulus);
    RMaterialProperty poisson(RMaterialProperty::PoissonRatio);
    poisson.add(293.15,0.0);
    material.add(poisson);
    RMaterialProperty density(RMaterialProperty::Density);
    density.add(293.15,8000.0);
    material.add(density);
    RMaterialProperty expansion(RMaterialProperty::ThermalExpansionCoefficient);
    expansion.add(293.15,0.0);
    material.add(expansion);
    RLine &line = model.getLine(0);
    line.setCrossArea(0.01);
    line.setMaterial(material);
    RBoundaryCondition transverse(R_BOUNDARY_CONDITION_DISPLACEMENT);
    transverse.getComponent(transverse.findComponentPosition(R_VARIABLE_DISPLACEMENT_X))
              .setEnabled(false);
    transverse.getComponent(transverse.findComponentPosition(R_VARIABLE_DISPLACEMENT_Y))
              .add(0.0,0.0);
    transverse.getComponent(transverse.findComponentPosition(R_VARIABLE_DISPLACEMENT_Z))
              .add(0.0,0.0);
    line.addBoundaryCondition(transverse);

    RElement root(R_ELEMENT_POINT);
    root.setNodeId(0,0);
    model.addElement(root,true,0);
    RBoundaryCondition fixed(R_BOUNDARY_CONDITION_DISPLACEMENT);
    for (RVariableType direction : {R_VARIABLE_DISPLACEMENT_X,
                                    R_VARIABLE_DISPLACEMENT_Y,
                                    R_VARIABLE_DISPLACEMENT_Z})
    {
        fixed.getComponent(fixed.findComponentPosition(direction)).add(0.0,0.0);
    }
    model.getPoint(0).addBoundaryCondition(fixed);

    RElement tip(R_ELEMENT_POINT);
    tip.setNodeId(0,1);
    model.addElement(tip,true,1);
    RInitialCondition initial(R_INITIAL_CONDITION_DISPLACEMENT);
    initial.getComponent(initial.findComponentPosition(R_VARIABLE_DISPLACEMENT_X))
           .add(0.0,1.0e-5);
    model.getPoint(1).addInitialCondition(initial);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setOutputFrequency(0);
    RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
    dynamics.setTimeStep(1.0e-5);
    dynamics.setNSteps(2);
    dynamics.setOutputFrequency(1);
    return model;
}

QString singleJournal(const QDir &directory)
{
    const QStringList paths = directory.entryList({QStringLiteral("*.events.jsonl")},QDir::Files);
    return paths.size() == 1 ? directory.filePath(paths.front()) : QString();
}

QString solverExecutablePath()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fea-solver.exe"));
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fea-solver"));
#endif
}

QString controlFixturePath()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("control_ndjson_fixture.exe"));
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("control_ndjson_fixture"));
#endif
}

void configureControl(RModel &model)
{
    auto &control = model.getProblemSetup().getStructuralControlSetup();
    control.enabled = true;
    control.controllerId = QStringLiteral("fixture-controller");
    control.deterministicSeed = 17;
    RStructuralActuator actuator;
    actuator.id = QStringLiteral("tip-force");
    actuator.target.nodeId = 1;
    actuator.variable = R_VARIABLE_FORCE_X;
    actuator.unit = RVariable::getUnits(R_VARIABLE_FORCE_X);
    actuator.minimum = -100.0;
    actuator.maximum = 100.0;
    model.getProblemSetup().getStructuralInteractionSetup().getActuators().push_back(actuator);
    RStructuralSensor sensor;
    sensor.id = QStringLiteral("tip-displacement");
    sensor.target.nodeId = 1;
    sensor.quantity = RStructuralSensorQuantity::Displacement;
    sensor.direction = RR3Vector(1.0,0.0,0.0);
    sensor.unit = RStructuralInteractionSetup::getSensorUnit(sensor.quantity);
    model.getProblemSetup().getStructuralInteractionSetup().getSensors().push_back(sensor);
}
}

class TestSolverAuditCli : public QObject
{
    Q_OBJECT

private slots:
    void successKeepsLinkAndTargetHashesDistinct();
    void restartFailureRecordsStartedThenFailed();
    void malformedExternalControlOptionsFailBeforeRun();
    void externalControllerCommitsTwoStepsWithJournal();
    void enabledControllerWithoutProgramFailsClosed();
    void externalControllerTimeoutDoesNotAcceptStep();
    void pythonPidControllerRunsWhenInterpreterAvailable();
};

void TestSolverAuditCli::successKeepsLinkAndTargetHashesDistinct()
{
    const QString executable = solverExecutablePath();
    QVERIFY2(QFile::exists(executable),"The fea-solver executable is not beside the test.");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("input.ram"));
    RModel model = makeModel();
    model.write(input);
    const QString originalLinkHash = sha256(input);
    QVERIFY(!originalLinkHash.isEmpty());

    QProcess process;
    process.start(executable,{QStringLiteral("--file=") + input});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,process.readAllStandardError().constData());

    const QString journalPath = singleJournal(QDir(directory.path()));
    QVERIFY2(!journalPath.isEmpty(),"Expected exactly one UUID-named event journal.");
    const ExecutionEventReadResult journal = ExecutionEventJournal::read(journalPath);
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QVERIFY(!journal.truncatedTail);
    QCOMPARE(journal.events.size(),3);
    QCOMPARE(journal.events[0].type,QStringLiteral("SolverRunStarted"));
    QCOMPARE(journal.events[1].type,QStringLiteral("RunComputed"));
    QCOMPARE(journal.events[2].type,QStringLiteral("RunFinished"));
    for (int i=0;i<journal.events.size();i++)
    {
        QCOMPARE(journal.events[i].runId,journal.events[0].runId);
        QCOMPARE(journal.events[i].sequence,quint64(i+1));
    }
    QVERIFY(journalPath.contains(journal.events[0].runId.toString(QUuid::WithoutBraces)));

    const QJsonObject started = journal.events[0].payload;
    QCOMPARE(started.value(QStringLiteral("inputArgumentPath")).toString(),
             QFileInfo(input).absoluteFilePath());
    QCOMPARE(started.value(QStringLiteral("inputArgumentSha256")).toString(),originalLinkHash);
    const QString inputTarget = started.value(QStringLiteral("inputModelTargetPath")).toString();
    QVERIFY(inputTarget != QFileInfo(input).absoluteFilePath());
    QVERIFY(QFile::exists(inputTarget));
    QCOMPARE(started.value(QStringLiteral("inputModelTargetSha256")).toString(),sha256(inputTarget));
    const QJsonObject report = started.value(QStringLiteral("parallelReport")).toObject();
    QVERIFY(report.value(QStringLiteral("actualThreads")).toInt() >= 1);
    QVERIFY(report.value(QStringLiteral("actualThreadsMeaning")).toString().contains(
                QStringLiteral("scope probe")));
    QVERIFY(report.contains(QStringLiteral("requestedThreads")));
    QVERIFY(report.contains(QStringLiteral("appliedThreads")));
    QVERIFY(report.contains(QStringLiteral("availableThreads")));
    QVERIFY(report.contains(QStringLiteral("backend")));
    QVERIFY(report.contains(QStringLiteral("fallback")));
    QVERIFY(report.contains(QStringLiteral("diagnostic")));
    QVERIFY(!started.value(QStringLiteral("cliOverride")).toObject()
                .value(QStringLiteral("provided")).toBool());

    const QJsonObject finished = journal.events[2].payload;
    const QString resultLink = finished.value(QStringLiteral("resultLinkPath")).toString();
    const QString resultTarget = finished.value(QStringLiteral("resultModelTargetPath")).toString();
    QCOMPARE(resultLink,QFileInfo(input).absoluteFilePath());
    QVERIFY(resultTarget != resultLink);
    QVERIFY(QFile::exists(resultTarget));
    QCOMPARE(finished.value(QStringLiteral("resultLinkSha256")).toString(),sha256(resultLink));
    QCOMPARE(finished.value(QStringLiteral("resultModelTargetSha256")).toString(),sha256(resultTarget));
}

void TestSolverAuditCli::restartFailureRecordsStartedThenFailed()
{
    const QString executable = solverExecutablePath();
    QVERIFY2(QFile::exists(executable),"The fea-solver executable is not beside the test.");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("missing-checkpoint.ram"));
    RModel model = makeModel();
    model.write(input);
    const QString originalLinkHash = sha256(input);

    QProcess process;
    process.start(executable,{QStringLiteral("--file=") + input,QStringLiteral("--restart")});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY(process.exitCode() != 0);
    const QString journalPath = singleJournal(QDir(directory.path()));
    QVERIFY2(!journalPath.isEmpty(),"Failure should leave one readable audit journal.");
    const ExecutionEventReadResult journal = ExecutionEventJournal::read(journalPath);
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.size(),2);
    QCOMPARE(journal.events[0].type,QStringLiteral("SolverRunStarted"));
    QCOMPARE(journal.events[1].type,QStringLiteral("RunFailed"));
    QCOMPARE(journal.events[1].runId,journal.events[0].runId);
    QCOMPARE(journal.events[1].sequence,quint64(2));
    QCOMPARE(journal.events[1].payload.value(QStringLiteral("phase")).toString(),
             QStringLiteral("solver-run"));
    QVERIFY(journal.events[1].payload.value(QStringLiteral("resultsMayAlreadyExist")).toBool());
    QCOMPARE(sha256(input),originalLinkHash);
}

void TestSolverAuditCli::malformedExternalControlOptionsFailBeforeRun()
{
    const QString executable = solverExecutablePath();
    QVERIFY(QFile::exists(executable));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("input.ram"));
    RModel model = makeModel();
    model.write(input);
    const QString originalHash = sha256(input);
    QVERIFY(!originalHash.isEmpty());

    const QVector<QStringList> invalidOptions{
        {QStringLiteral("--control-args-json=[1]")},
        {QStringLiteral("--control-program=")+executable,
         QStringLiteral("--control-args-json=[1]")},
        {QStringLiteral("--control-timeout-ms=0")},
        {QStringLiteral("--control-timeout-ms=60001")},
        {QStringLiteral("--control-timeout-ms=5000")}
    };
    for (const QStringList &options : invalidOptions)
    {
        QProcess process;
        QStringList arguments{QStringLiteral("--file=")+input};
        arguments.append(options);
        process.start(executable,arguments);
        QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
        QCOMPARE(process.exitStatus(),QProcess::NormalExit);
        QVERIFY2(process.exitCode() != 0,qPrintable(options.join(QLatin1Char(' '))));
        QCOMPARE(sha256(input),originalHash);
        QVERIFY(singleJournal(QDir(directory.path())).isEmpty());
    }
}

void TestSolverAuditCli::externalControllerCommitsTwoStepsWithJournal()
{
    const QString executable = solverExecutablePath();
    const QString controller = controlFixturePath();
    QVERIFY(QFile::exists(executable));
    QVERIFY(QFile::exists(controller));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("controlled.ram"));
    RModel model = makeModel();
    configureControl(model);
    model.getProblemSetup().getStructuralDynamicsSetup().setOutputFrequency(2);
    model.write(input);

    QProcess process;
    process.start(executable,{QStringLiteral("--file=")+input,
                              QStringLiteral("--control-program=")+controller});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+
                                          process.readAllStandardError())));
    const QString path = singleJournal(QDir(directory.path()));
    QVERIFY(!path.isEmpty());
    const auto journal = ExecutionEventJournal::read(path);
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.size(),5);
    QCOMPARE(journal.events[0].type,QStringLiteral("SolverRunStarted"));
    for (int i=1;i<=2;i++)
    {
        const auto &event = journal.events[i];
        QCOMPARE(event.type,QStringLiteral("ControlStepAccepted"));
        QCOMPARE(event.payload.value(QStringLiteral("stepId")).toString(),QString::number(i));
        QCOMPARE(event.payload.value(QStringLiteral("controllerId")).toString(),
                 QStringLiteral("fixture-controller"));
        QVERIFY(event.payload.value(QStringLiteral("nextTimeS")).toDouble() >
                event.payload.value(QStringLiteral("acceptedTimeS")).toDouble());
        const QJsonArray channels = event.payload.value(QStringLiteral("appliedChannels")).toArray();
        QCOMPARE(channels.size(),1);
        QCOMPARE(event.payload.value(QStringLiteral("conditionedCommands")).toArray().size(),1);
        QCOMPARE(event.payload.value(QStringLiteral("resultArtifactPersisted")).toBool(),i == 2);
        QCOMPARE(event.payload.contains(QStringLiteral("resultModelTargetPath")),i == 2);
        QCOMPARE(channels[0].toObject().value(QStringLiteral("actuatorId")).toString(),
                 QStringLiteral("tip-force"));
        QVERIFY(channels[0].toObject().contains(QStringLiteral("cumulativeWorkJ")));
    }
    QCOMPARE(journal.events[3].type,QStringLiteral("RunComputed"));
    QCOMPARE(journal.events[4].type,QStringLiteral("RunFinished"));
}

void TestSolverAuditCli::enabledControllerWithoutProgramFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("unbound.ram"));
    RModel model = makeModel();
    configureControl(model);
    model.write(input);
    const QString inputHash = sha256(input);
    QProcess process;
    process.start(solverExecutablePath(),{QStringLiteral("--file=")+input});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QVERIFY(process.exitCode() != 0);
    QCOMPARE(sha256(input),inputHash);
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.size(),1);
    QCOMPARE(journal.events[0].type,QStringLiteral("RunFailed"));
    QCOMPARE(journal.events[0].payload.value(QStringLiteral("phase")).toString(),
             QStringLiteral("control-setup"));
}

void TestSolverAuditCli::externalControllerTimeoutDoesNotAcceptStep()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("timeout.ram"));
    RModel model = makeModel();
    configureControl(model);
    model.write(input);
    const QString before = sha256(input);
    QProcess process;
    const QString arguments = QString::fromUtf8(
        QJsonDocument(QJsonArray{QStringLiteral("timeout")}).toJson(QJsonDocument::Compact));
    process.start(solverExecutablePath(),
                  {QStringLiteral("--file=")+input,
                   QStringLiteral("--control-program=")+controlFixturePath(),
                   QStringLiteral("--control-args-json=")+arguments,
                   QStringLiteral("--control-timeout-ms=100")});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QVERIFY(process.exitCode() != 0);
    QCOMPARE(sha256(input),before);
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.size(),2);
    QCOMPARE(journal.events[0].type,QStringLiteral("SolverRunStarted"));
    QCOMPARE(journal.events[1].type,QStringLiteral("RunFailed"));
}

void TestSolverAuditCli::pythonPidControllerRunsWhenInterpreterAvailable()
{
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QVERIFY(QFile::exists(QStringLiteral(RANGE_PID_EXAMPLE_PATH)));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("python-pid.ram"));
    RModel model = makeModel();
    configureControl(model);
    model.getProblemSetup().getStructuralDynamicsSetup().setNSteps(20);
    model.write(input);
    const QJsonArray controlArguments{QStringLiteral(RANGE_PID_EXAMPLE_PATH),
                                      QStringLiteral("10000000"),QStringLiteral("0"),
                                      QStringLiteral("5000")};
    const QString args = QString::fromUtf8(QJsonDocument(controlArguments)
                                               .toJson(QJsonDocument::Compact));
    QProcess process;
    process.start(solverExecutablePath(),
                  {QStringLiteral("--file=")+input,
                   QStringLiteral("--control-program=")+python,
                   QStringLiteral("--control-args-json=")+args});
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+
                                          process.readAllStandardError())));
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.size(),23);
    int nonzeroCommands = 0;
    for (int i=1;i<=20;i++)
    {
        QCOMPARE(journal.events[i].type,QStringLiteral("ControlStepAccepted"));
        const QJsonArray channels = journal.events[i].payload.value(QStringLiteral("appliedChannels"))
                                       .toArray();
        QCOMPARE(channels.size(),1);
        nonzeroCommands += channels[0].toObject().value(QStringLiteral("appliedForceN"))
                               .toDouble() != 0.0;
    }
    QVERIFY(nonzeroCommands > 0);
    QCOMPARE(journal.events.last().type,QStringLiteral("RunFinished"));
}

int main(int argc,char *argv[])
{
    QCoreApplication app(argc,argv);
    TestSolverAuditCli test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_solver_audit_cli.moc"
