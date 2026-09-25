#include <QtTest>

#include <algorithm>
#include <cmath>
#include "rstructural_control_conditioner.h"

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

RModel makeStaticTrussModel()
{
    RModel model = makeModel();
    model.getProblemSetup().getStructuralDynamicsSetup().setEnabled(false);
    model.getPoint(1).removeAllInitialConditions();
    // Specialized static truss benchmarks explicitly disable gravity.
    for (uint group=0;group<model.getNElementGroups();group++)
    {
        auto &gravity = model.getElementGroupPtr(group)->getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        for (uint component=0;component<gravity.size();component++)
        {
            gravity.getComponent(component).clear();
            gravity.getComponent(component).add(0.0,0.0);
        }
    }
    RMaterial material(RMaterial::Solid);
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15,1000.0);
    material.add(modulus);
    model.getLine(0).setMaterial(material);
    model.getLine(0).setCrossArea(0.5);
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
    void pythonPidRestartMatchesUninterrupted();
    void staticCablePersistsCertifiedResult_data();
    void staticCablePersistsCertifiedResult();
    void staticPiezoPersistsElectrodeCharge_data();
    void staticPiezoPersistsElectrodeCharge();
    void nodalPiezoPersistsPotentialAndCharge_data();
    void nodalPiezoPersistsPotentialAndCharge();
};

void TestSolverAuditCli::staticCablePersistsCertifiedResult_data()
{
    QTest::addColumn<bool>("binary");
    QTest::newRow("ascii") << false;
    QTest::newRow("binary") << true;
}

void TestSolverAuditCli::staticCablePersistsCertifiedResult()
{
    QFETCH(bool,binary);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = makeStaticTrussModel();
    RBoundaryCondition force(R_BOUNDARY_CONDITION_FORCE);
    for (RVariableType type : {R_VARIABLE_FORCE_X,R_VARIABLE_FORCE_Y,R_VARIABLE_FORCE_Z})
    {
        auto &component = force.getComponent(force.findComponentPosition(type));
        component.clear();
        component.setEnabled(true);
        component.add(0.0,type == R_VARIABLE_FORCE_X ? 10.0 : 0.0);
    }
    model.getPoint(1).addBoundaryCondition(force);
    auto &cable = model.getProblemSetup().getStructuralCableSetup();
    cable.enabled = true;
    cable.members = {{0,2.0,5.0}};
    cable.bindToModel(model);
    const QString path = directory.filePath("cable."+RModel::getDefaultFileExtension(binary));
    model.write(path);
    QProcess process;
    process.start(solverExecutablePath(),{QStringLiteral("--file=")+path});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    const QByteArray diagnostics = process.readAllStandardError()+process.readAllStandardOutput();
    QVERIFY2(process.exitCode() == 0,diagnostics.constData());

    RModel result;
    result.read(path);
    QVERIFY(result.getProblemSetup().getStructuralCableSetup().enabled);
    result.getProblemSetup().getStructuralCableSetup().validate(result);
    const uint displacement = result.findVariable(R_VARIABLE_DISPLACEMENT);
    const uint internalForce = result.findVariable(R_VARIABLE_FORCE);
    const uint normalStress = result.findVariable(R_VARIABLE_STRESS_NORMAL);
    QVERIFY(displacement != RConstants::eod);
    QVERIFY(internalForce != RConstants::eod);
    QVERIFY(normalStress != RConstants::eod);
    // EA/L0=250 N/m; N0=5 N, F=10 N => u=0.02 m, stress=20 Pa.
    QVERIFY(std::abs(result.getVariable(displacement).getValue(0,1)-0.02) < 1e-10);
    QVERIFY(std::abs(result.getVariable(internalForce).getValue(0,1)-10.0) < 1e-8);
    QVERIFY(std::abs(result.getVariable(normalStress).getValue(0,0)-20.0) < 1e-8);
    QCOMPARE(result.getNode(1).getX(),2.0);
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QVERIFY(!journal.truncatedTail);
    QVERIFY(!journal.events.empty());
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFinished"));
}

void TestSolverAuditCli::staticPiezoPersistsElectrodeCharge_data()
{
    QTest::addColumn<bool>("binary");
    QTest::newRow("ascii") << false;
    QTest::newRow("binary") << true;
}

void TestSolverAuditCli::staticPiezoPersistsElectrodeCharge()
{
    QFETCH(bool,binary);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = makeStaticTrussModel();
    auto &piezo = model.getProblemSetup().getStructuralPiezoSetup();
    piezo.enabled = true;
    piezo.ports = {{QStringLiteral("rod-terminal"),0,2.0,4.0,1.0}};
    piezo.bindToModel(model);
    const QString path = directory.filePath("piezo."+RModel::getDefaultFileExtension(binary));
    model.write(path);
    QProcess process;
    process.start(solverExecutablePath(),{QStringLiteral("--file=")+path});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    const QByteArray diagnostics = process.readAllStandardError()+process.readAllStandardOutput();
    QVERIFY2(process.exitCode() == 0,diagnostics.constData());
    RModel result;
    result.read(path);
    QVERIFY(result.getProblemSetup().getStructuralPiezoSetup().enabled);
    result.getProblemSetup().getStructuralPiezoSetup().validate(result);
    const uint displacement = result.findVariable(R_VARIABLE_DISPLACEMENT);
    const uint force = result.findVariable(R_VARIABLE_FORCE);
    const uint charge = result.findVariable(R_VARIABLE_ELECTRIC_CHARGE);
    QVERIFY(displacement != RConstants::eod);
    QVERIFY(force != RConstants::eod);
    QVERIFY(charge != RConstants::eod);
    // u=-eV/c=-0.002 m; qR=(eps*A/L + (e*A/L)^2/(c*A/L))*V=1.001 C.
    QVERIFY(std::abs(result.getVariable(displacement).getValue(0,1)+0.002) < 1e-10);
    QVERIFY(std::abs(result.getVariable(force).getValue(0,1)) < 1e-8);
    QVERIFY(std::abs(result.getVariable(charge).getValue(0,0)-1.001) < 1e-8);
    QCOMPARE(result.getVariable(charge).getApplyType(),R_VARIABLE_APPLY_ELEMENT);
    QCOMPARE(result.getVariable(charge).getValue(0,1),0.0);
    QCOMPARE(result.getVariable(charge).getValue(0,2),0.0);
    QCOMPARE(result.getNode(1).getX(),2.0);
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QVERIFY(!journal.truncatedTail);
    QVERIFY(!journal.events.empty());
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFinished"));
}

void TestSolverAuditCli::nodalPiezoPersistsPotentialAndCharge_data()
{
    QTest::addColumn<bool>("binary");
    QTest::newRow("ascii") << false;
    QTest::newRow("binary") << true;
}

void TestSolverAuditCli::nodalPiezoPersistsPotentialAndCharge()
{
    QFETCH(bool,binary);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = makeStaticTrussModel();
    RMaterial material(RMaterial::Solid);
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15,10.0);
    material.add(modulus);
    model.getLine(0).setMaterial(material);

    RBoundaryCondition force(R_BOUNDARY_CONDITION_FORCE);
    for (RVariableType type : {R_VARIABLE_FORCE_X,R_VARIABLE_FORCE_Y,R_VARIABLE_FORCE_Z})
    {
        auto &component = force.getComponent(force.findComponentPosition(type));
        component.clear();
        component.setEnabled(true);
        component.add(0.0,type == R_VARIABLE_FORCE_X ? 0.025 : 0.0);
    }
    model.getPoint(1).addBoundaryCondition(force);

    auto &piezo = model.getProblemSetup().getStructuralPiezoSetup();
    piezo.enabled = true;
    piezo.electricalMode = RStructuralPiezoElectricalMode::NodalPotential;
    piezo.ports = {{QStringLiteral("rod-network"),0,2.0,4.0,0.0}};
    piezo.nodalBoundaryConditions.push_back(
        {0,RStructuralPiezoBoundaryType::Potential,0.0});
    piezo.nodalBoundaryConditions.push_back(
        {1,RStructuralPiezoBoundaryType::ConductorCharge,0.0});
    piezo.bindToModel(model);

    const QString path = directory.filePath("nodal-piezo."+RModel::getDefaultFileExtension(binary));
    model.write(path);
    QProcess process;
    process.start(solverExecutablePath(),{QStringLiteral("--file=")+path});
    QVERIFY2(process.waitForFinished(30000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    const QByteArray diagnostics = process.readAllStandardError()+process.readAllStandardOutput();
    QVERIFY2(process.exitCode() == 0,diagnostics.constData());

    RModel result;
    result.read(path);
    QCOMPARE(int(result.getProblemSetup().getStructuralPiezoSetup().electricalMode),
             int(RStructuralPiezoElectricalMode::NodalPotential));
    result.getProblemSetup().getStructuralPiezoSetup().validate(result);
    const uint displacement = result.findVariable(R_VARIABLE_DISPLACEMENT);
    const uint potential = result.findVariable(R_VARIABLE_ELECTRIC_POTENTIAL);
    const uint charge = result.findVariable(R_VARIABLE_ELECTRIC_CHARGE);
    QVERIFY(displacement != RConstants::eod);
    QVERIFY(potential != RConstants::eod);
    QVERIFY(charge != RConstants::eod);
    QCOMPARE(result.getVariable(potential).getApplyType(),R_VARIABLE_APPLY_NODE);
    QCOMPARE(result.getVariable(charge).getApplyType(),R_VARIABLE_APPLY_NODE);
    const double forceScale = 0.025*2.0/0.5;
    const double determinant = 10.0*4.0+2.0*2.0;
    // The current ASCII result writer keeps eight significant digits for
    // variable values; binary preserves the tighter in-memory solution.
    const double persistedTolerance = binary ? 1.0e-10 : 1.0e-8;
    const double displacementValue = result.getVariable(displacement).getValue(0,1);
    QVERIFY2(std::abs(displacementValue-forceScale*4.0/determinant) < persistedTolerance,
             qPrintable(QString("CLI nodal piezo displacement %1 vs analytic %2")
                        .arg(displacementValue,0,'g',17)
                        .arg(forceScale*4.0/determinant,0,'g',17)));
    QVERIFY(std::abs(result.getVariable(potential).getValue(0,1)
                     -forceScale*2.0/determinant) < persistedTolerance);
    QVERIFY(std::abs(result.getVariable(charge).getValue(0,1)) < 1.0e-10);
}

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

void TestSolverAuditCli::pythonPidRestartMatchesUninterrupted()
{
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QDir root(directory.path());
    for (const QString &name : {QStringLiteral("full"),QStringLiteral("first"),QStringLiteral("resume")})
        QVERIFY(root.mkdir(name));
    const QString fullPath = root.filePath(QStringLiteral("full/input.ram"));
    const QString firstPath = root.filePath(QStringLiteral("first/input.ram"));
    const QString resumePath = root.filePath(QStringLiteral("resume/input.ram"));
    auto configured = [](uint steps)
    {
        RModel model = makeModel();
        configureControl(model);
        model.getProblemSetup().getStructuralDynamicsSetup().setNSteps(steps);
        RStructuralControlChannelConditioning conditioning;
        conditioning.actuatorId = QStringLiteral("tip-force");
        conditioning.delaySteps = 2;
        conditioning.holdSteps = 3;
        conditioning.noiseAmplitude = 0.01;
        model.getProblemSetup().getStructuralControlSetup().channelConditioning.push_back(conditioning);
        return model;
    };
    configured(20).write(fullPath);
    configured(8).write(firstPath);
    // Nonzero integral gain and a split inside a hold interval exercise both
    // external PID state and the solver's delay/hold/noise checkpoint envelope.
    const QJsonArray args{QStringLiteral(RANGE_PID_EXAMPLE_PATH),QStringLiteral("1000000"),
                          QStringLiteral("100000000"),QStringLiteral("100")};
    const QString encoded = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    QString error;
    auto run = [&](const QString &path,bool restart)
    {
        QStringList arguments{QStringLiteral("--file=")+path,
                              QStringLiteral("--control-program=")+python,
                              QStringLiteral("--control-args-json=")+encoded};
        if (restart) arguments.append(QStringLiteral("--restart"));
        QProcess process;
        process.start(solverExecutablePath(),arguments);
        const bool finished = process.waitForFinished(60000);
        error = QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError());
        if (!finished) { process.kill(); process.waitForFinished(); error += process.errorString(); }
        return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    QVERIFY2(run(fullPath,false),qPrintable(error));
    QVERIFY2(run(firstPath,false),qPrintable(error));
    RModel resumed;
    resumed.read(firstPath);
    QCOMPARE(resumed.getProblemSetup().getStructuralDynamicsRestartData().completedSteps,8u);
    resumed.getProblemSetup().getStructuralDynamicsSetup().setNSteps(12);
    resumed.write(resumePath);
    QVERIFY2(run(resumePath,true),qPrintable(error));
    RModel full,actual;
    full.read(fullPath);
    actual.read(resumePath);
    const auto &expectedState = full.getProblemSetup().getStructuralDynamicsRestartData();
    const auto &actualState = actual.getProblemSetup().getStructuralDynamicsRestartData();
    QVERIFY(expectedState.controlAvailable && actualState.controlAvailable);
    QCOMPARE(actualState.completedSteps,20u);
    QCOMPARE(actualState.controlFingerprint,expectedState.controlFingerprint);
    QCOMPARE(actualState.controlChannelIds,expectedState.controlChannelIds);
    auto near = [](double a,double b) { return std::abs(a-b) <= 1e-11*(1.0+std::max(std::abs(a),std::abs(b))); };
    auto vectorsNear = [&](const std::vector<double> &a,const std::vector<double> &b)
    {
        if (a.size()!=b.size()) return false;
        for (std::size_t i=0;i<a.size();++i) if (!near(a[i],b[i])) return false;
        return true;
    };
    QVERIFY(vectorsNear(actualState.q,expectedState.q));
    QVERIFY(vectorsNear(actualState.v,expectedState.v));
    QVERIFY(vectorsNear(actualState.a,expectedState.a));
    QVERIFY(vectorsNear(actualState.externalForce,expectedState.externalForce));
    QVERIFY(vectorsNear(actualState.controlLastCommands,expectedState.controlLastCommands));
    QVERIFY(vectorsNear(actualState.controlPowers,expectedState.controlPowers));
    QVERIFY(vectorsNear(actualState.cumulativeControlWork,expectedState.cumulativeControlWork));
    QVERIFY(near(actualState.cumulativeExternalWork,expectedState.cumulativeExternalWork));
    const auto expectedEnvelope = RStructuralControlStateEnvelope::deserialize(expectedState.controlControllerState);
    const auto actualEnvelope = RStructuralControlStateEnvelope::deserialize(actualState.controlControllerState);
    const auto expectedPid = QJsonDocument::fromJson(expectedEnvelope.controllerState).object();
    const auto actualPid = QJsonDocument::fromJson(actualEnvelope.controllerState).object();
    QVERIFY(!expectedPid.isEmpty() && !actualPid.isEmpty());
    QCOMPARE(actualPid.value("sampleCount").toInt(),20);
    QVERIFY(near(actualPid.value("integral").toDouble(),expectedPid.value("integral").toDouble()));
    QVERIFY(near(actualPid.value("previousError").toDouble(),expectedPid.value("previousError").toDouble()));
    const auto fullJournal = ExecutionEventJournal::read(singleJournal(QDir(root.filePath("full"))));
    const auto firstJournal = ExecutionEventJournal::read(singleJournal(QDir(root.filePath("first"))));
    const auto resumeJournal = ExecutionEventJournal::read(singleJournal(QDir(root.filePath("resume"))));
    QVERIFY(fullJournal.error.isEmpty() && firstJournal.error.isEmpty() && resumeJournal.error.isEmpty());
    auto steps = [](const ExecutionEventReadResult &journal)
    {
        QVector<QJsonObject> result;
        for (const auto &event : journal.events)
            if (event.type == QStringLiteral("ControlStepAccepted")) result.append(event.payload);
        return result;
    };
    const auto expected = steps(fullJournal);
    auto split = steps(firstJournal);
    split += steps(resumeJournal);
    QCOMPARE(expected.size(),20);
    QCOMPARE(split.size(),20);
    for (int i=0;i<20;i++)
    {
        QCOMPARE(split[i].value("stepId"),expected[i].value("stepId"));
        QVERIFY(near(split[i].value("nextTimeS").toDouble(),expected[i].value("nextTimeS").toDouble()));
        for (const QString &key : {QStringLiteral("requestedCommands"),QStringLiteral("conditionedCommands"),QStringLiteral("appliedChannels")})
        {
            const auto a = split[i].value(key).toArray();
            const auto b = expected[i].value(key).toArray();
            QCOMPARE(a.size(),1);
            QCOMPARE(b.size(),1);
            const auto ao = a[0].toObject();
            const auto bo = b[0].toObject();
            QCOMPARE(ao.keys(),bo.keys());
            for (const QString &field : bo.keys())
            {
                if (bo[field].isDouble()) QVERIFY(near(ao[field].toDouble(),bo[field].toDouble()));
                else QCOMPARE(ao[field],bo[field]);
            }
        }
    }
}


int main(int argc,char *argv[])
{
    QCoreApplication app(argc,argv);
    TestSolverAuditCli test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_solver_audit_cli.moc"
