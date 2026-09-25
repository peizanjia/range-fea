#include <QtTest>

#include <algorithm>
#include <cmath>
#include "rstructural_control_conditioner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "execution_event_journal.h"
#include "rml_model.h"
#include "rstructural_thermal_fe_adapter.h"

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

void configureControl(RModel &model);

RModel makeThermalControlModel(bool unbalancedAxialLoad = false,
                              bool enableForceController = false)
{
    RModel model;
    model.setNNodes(2);
    model.getNode(0).set(0.0,0.0,0.0);
    model.getNode(1).set(1.0,0.0,0.0);
    RElement rod(R_ELEMENT_TRUSS1);
    rod.setNodeId(0,0);
    rod.setNodeId(1,1);
    model.addElement(rod,true,0);

    RMaterial material(RMaterial::Solid);
    const auto addProperty = [&material](RMaterialProperty::Type type,double value)
    {
        RMaterialProperty property(type);
        property.add(293.15,value);
        material.add(property);
    };
    addProperty(RMaterialProperty::ModulusOfElasiticity,10.0);
    addProperty(RMaterialProperty::PoissonRatio,0.0);
    addProperty(RMaterialProperty::ThermalExpansionCoefficient,0.01);
    addProperty(RMaterialProperty::ThermalConductivity,1.0);
    addProperty(RMaterialProperty::HeatCapacity,3.0);
    addProperty(RMaterialProperty::Density,1.0);
    addProperty(RMaterialProperty::Emissivity,0.0);
    RLine &line = model.getLine(0);
    line.setCrossArea(1.0);
    line.setMaterial(material);

    for (uint node=0;node<2;node++)
    {
        RElement point(R_ELEMENT_POINT);
        point.setNodeId(0,node);
        model.addElement(point,true,node);
        RBoundaryCondition support(R_BOUNDARY_CONDITION_DISPLACEMENT);
        RConditionComponent &axial = support.getComponent(
            support.findComponentPosition(R_VARIABLE_DISPLACEMENT_X));
        if (node == 0 && !unbalancedAxialLoad)
            axial.add(0.0,0.0);
        else
            axial.setEnabled(false);
        support.getComponent(support.findComponentPosition(R_VARIABLE_DISPLACEMENT_Y))
               .add(0.0,0.0);
        support.getComponent(support.findComponentPosition(R_VARIABLE_DISPLACEMENT_Z))
               .add(0.0,0.0);
        model.getPoint(node).addBoundaryCondition(support);
    }
    RBoundaryCondition anchorTemperature(R_BOUNDARY_CONDITION_TEMPERATURE);
    anchorTemperature.getComponent(anchorTemperature.findComponentPosition(R_VARIABLE_TEMPERATURE))
                     .add(0.0,293.15);
    model.getPoint(0).addBoundaryCondition(anchorTemperature);
    if (unbalancedAxialLoad)
    {
        RBoundaryCondition force(R_BOUNDARY_CONDITION_FORCE);
        force.getComponent(force.findComponentPosition(R_VARIABLE_FORCE_X)).add(0.0,1.0);
        model.getPoint(1).addBoundaryCondition(force);
    }

    RProblemTaskItem sequence;
    sequence.addChild(RProblemTaskItem(R_PROBLEM_HEAT));
    sequence.addChild(RProblemTaskItem(R_PROBLEM_STRESS));
    model.setProblemTaskTree(sequence);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setSolverCvgValue(1.0e-12);

    auto &catalog = model.getProblemSetup().getStructuralInteractionSetup();
    RStructuralActuator actuator;
    actuator.id = QStringLiteral("heater-fe");
    actuator.type = RStructuralActuatorType::HeatSource;
    actuator.target.scope = RStructuralTargetScope::Node;
    actuator.target.nodeId = 1;
    actuator.variable = R_VARIABLE_HEAT;
    actuator.unit = QStringLiteral("W");
    actuator.minimum = 0.0;
    actuator.maximum = 2.0;
    catalog.getActuators().push_back(actuator);
    RStructuralSensor sensor;
    sensor.id = QStringLiteral("sensor-fe");
    sensor.target.scope = RStructuralTargetScope::Node;
    sensor.target.nodeId = 1;
    sensor.quantity = RStructuralSensorQuantity::Temperature;
    sensor.aggregation = RStructuralSensorAggregation::Point;
    sensor.unit = QStringLiteral("K");
    catalog.getSensors().push_back(sensor);

    if (enableForceController)
        configureControl(model);

    auto &thermal = model.getProblemSetup().getStructuralThermalControlSetup();
    thermal.enabled = true;
    thermal.thermalSubstepSeconds = 1.0;
    thermal.thermalSubstepsPerMechanicalMacrostep = 1;
    thermal.mechanicalMacrostepsPerControllerSample = 1;
    const QString nodeId = RStructuralThermalFEAdapter::nodeId(model,1);
    RStructuralThermalControlChannelSetup channel;
    channel.actuatorId = actuator.id;
    channel.heaterNodeId = nodeId;
    channel.temperatureSensorId = sensor.id;
    channel.temperatureSensorNodeId = nodeId;
    channel.turnOnBelowKelvin = 400.0;
    channel.turnOffAtOrAboveKelvin = 401.0;
    channel.onPowerWatts = 2.0;
    channel.maximumPowerWatts = 2.0;
    thermal.channels = {channel};
    thermal.sparseObservationNodeIds = {nodeId};
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
    void thermalCliRestartMatchesUninterrupted();
    void thermalCliRejectsCorruptCheckpoint();
    void thermalFailedMacrostepPreservesCheckpoint();
    void thermalCliRejectsEnabledForceController();
    void thermalCliRejectsConcurrentCheckpointWriter();
    void thermalCliRejectsOversizedMacrostepRequest();
    void thermalCliDetectsCheckpointChangeBeforePublish();
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

void TestSolverAuditCli::thermalCliRestartMatchesUninterrupted()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QDir root(directory.path());
    for (const QString &name : {QStringLiteral("full"),QStringLiteral("split-initial"),
                                QStringLiteral("split-resume")})
        QVERIFY(root.mkdir(name));

    const QString fullInput = root.filePath(QStringLiteral("full/input.ram"));
    const QString fullCheckpoint = root.filePath(QStringLiteral("full/state.tfec"));
    const QString firstInput = root.filePath(QStringLiteral("split-initial/input.ram"));
    const QString firstCheckpoint = root.filePath(QStringLiteral("split-initial/state.tfec"));
    const QString resumeCheckpoint = root.filePath(QStringLiteral("split-resume/state.tfec"));
    makeThermalControlModel().write(fullInput);
    makeThermalControlModel().write(firstInput);

    QString diagnostic;
    auto run = [&](const QString &input,const QString &checkpoint,uint steps)
    {
        QProcess process;
        process.setWorkingDirectory(QFileInfo(checkpoint).absolutePath());
        process.start(solverExecutablePath(),
            {QStringLiteral("--file=")+input,
             QStringLiteral("--thermal-steps=")+QString::number(steps),
             QStringLiteral("--thermal-checkpoint=")+checkpoint});
        const bool finished = process.waitForFinished(60000);
        diagnostic = QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError());
        if (!finished)
        {
            process.kill();
            process.waitForFinished();
            diagnostic += process.errorString();
        }
        return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };

    QVERIFY2(run(fullInput,fullCheckpoint,3),qPrintable(diagnostic));
    QVERIFY(!QFileInfo::exists(fullCheckpoint+QStringLiteral(".lock")));
    QVERIFY2(run(firstInput,firstCheckpoint,1),qPrintable(diagnostic));
    QVERIFY(!QFileInfo::exists(firstCheckpoint+QStringLiteral(".lock")));
    QVERIFY(QFile::copy(firstCheckpoint,resumeCheckpoint));
    QVERIFY2(run(resumeCheckpoint,resumeCheckpoint,2),qPrintable(diagnostic));
    QVERIFY(!QFileInfo::exists(resumeCheckpoint+QStringLiteral(".lock")));
    const QString fullJournalPath = singleJournal(QDir(root.filePath("full")));
    const QString firstJournalPath = singleJournal(QDir(root.filePath("split-initial")));
    const QString resumeJournalPath = singleJournal(QDir(root.filePath("split-resume")));
    QVERIFY(!fullJournalPath.isEmpty() && !firstJournalPath.isEmpty() && !resumeJournalPath.isEmpty());
    QVERIFY(!QFileInfo::exists(fullJournalPath+QStringLiteral(".lock")));
    QVERIFY(!QFileInfo::exists(firstJournalPath+QStringLiteral(".lock")));
    QVERIFY(!QFileInfo::exists(resumeJournalPath+QStringLiteral(".lock")));

    RModel fullModel,resumedModel;
    RStructuralThermalControlState fullState,resumedState;
    RStructuralThermalFEAdapter::readConfiguredCheckpoint(fullCheckpoint,fullModel,fullState);
    RStructuralThermalFEAdapter::readConfiguredCheckpoint(resumeCheckpoint,resumedModel,resumedState);
    const auto fullScheduler = RStructuralThermalFEAdapter::schedulerFromModel(fullModel);
    const auto resumedScheduler = RStructuralThermalFEAdapter::schedulerFromModel(resumedModel);
    QCOMPARE(fullState.completedMechanicalMacrosteps,quint64(3));
    QCOMPARE(resumedState.completedMechanicalMacrosteps,quint64(3));
    QCOMPARE(fullState.completedControllerSamples,quint64(3));
    QCOMPARE(resumedState.completedControllerSamples,quint64(3));
    QCOMPARE(fullState.acceptedTimeSeconds,3.0);
    QCOMPARE(resumedState.acceptedTimeSeconds,3.0);
    QCOMPARE(fullScheduler.serialize(fullState),resumedScheduler.serialize(resumedState));

    const uint temperature = fullModel.findVariable(R_VARIABLE_TEMPERATURE);
    const uint displacement = fullModel.findVariable(R_VARIABLE_DISPLACEMENT);
    const uint resumedTemperature = resumedModel.findVariable(R_VARIABLE_TEMPERATURE);
    const uint resumedDisplacement = resumedModel.findVariable(R_VARIABLE_DISPLACEMENT);
    QVERIFY(temperature != RConstants::eod && displacement != RConstants::eod);
    QVERIFY(resumedTemperature != RConstants::eod && resumedDisplacement != RConstants::eod);
    for (uint node=0;node<fullModel.getNNodes();node++)
    {
        QCOMPARE(fullModel.getVariable(temperature).getValue(0,node),
                 resumedModel.getVariable(resumedTemperature).getValue(0,node));
        QCOMPARE(fullModel.getVariable(displacement).getValue(0,node),
                 resumedModel.getVariable(resumedDisplacement).getValue(0,node));
    }

    const auto fullJournal = ExecutionEventJournal::read(fullJournalPath);
    const auto firstJournal = ExecutionEventJournal::read(firstJournalPath);
    const auto resumeJournal = ExecutionEventJournal::read(resumeJournalPath);
    QVERIFY(fullJournal.error.isEmpty() && firstJournal.error.isEmpty() && resumeJournal.error.isEmpty());
    auto acceptedCount = [](const ExecutionEventReadResult &journal)
    {
        return int(std::count_if(journal.events.begin(),journal.events.end(),[](const auto &event) {
            return event.type == QStringLiteral("ThermalMacrostepAccepted");
        }));
    };
    QCOMPARE(acceptedCount(fullJournal),3);
    QCOMPARE(acceptedCount(firstJournal),1);
    QCOMPARE(acceptedCount(resumeJournal),2);
    QCOMPARE(fullJournal.events.back().payload.value(QStringLiteral("thermalCheckpointPath")).toString(),
             QFileInfo(fullCheckpoint).absoluteFilePath());
    QCOMPARE(fullJournal.events.back().payload.value(QStringLiteral("acceptedTimeSeconds")).toDouble(),3.0);
}

void TestSolverAuditCli::thermalCliRejectsCorruptCheckpoint()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("setup")));
    const QString input = directory.filePath(QStringLiteral("setup/input.ram"));
    const QString validCheckpoint = directory.filePath(QStringLiteral("setup/valid.tfec"));
    makeThermalControlModel().write(input);

    QProcess create;
    create.setWorkingDirectory(directory.filePath(QStringLiteral("setup")));
    create.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,QStringLiteral("--thermal-steps=1"),
         QStringLiteral("--thermal-checkpoint=")+validCheckpoint});
    QVERIFY2(create.waitForFinished(60000),qPrintable(create.errorString()));
    QCOMPARE(create.exitStatus(),QProcess::NormalExit);
    QVERIFY2(create.exitCode() == 0,
             qPrintable(QString::fromUtf8(create.readAllStandardOutput()+create.readAllStandardError())));

    const QString corruptCheckpoint = directory.filePath(QStringLiteral("corrupt.tfec"));
    QVERIFY(QFile::copy(validCheckpoint,corruptCheckpoint));
    QFile corrupt(corruptCheckpoint);
    QVERIFY(corrupt.open(QIODevice::ReadWrite));
    QByteArray bytes = corrupt.readAll();
    QVERIFY(bytes.size() > 8);
    bytes[bytes.size()-1] = char(bytes.back() ^ 0x5a);
    corrupt.close();
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write(bytes),qint64(bytes.size()));
    corrupt.close();
    const QString corruptHash = sha256(corruptCheckpoint);

    const QString output = directory.filePath(QStringLiteral("preserve.tfec"));
    QFile sentinel(output);
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    const QByteArray sentinelContents("existing thermal checkpoint stays intact");
    QCOMPARE(sentinel.write(sentinelContents),qint64(sentinelContents.size()));
    sentinel.close();
    const QString outputHash = sha256(output);

    QProcess resume;
    resume.setWorkingDirectory(directory.path());
    resume.start(solverExecutablePath(),
        {QStringLiteral("--file=")+corruptCheckpoint,QStringLiteral("--thermal-steps=1"),
         QStringLiteral("--thermal-checkpoint=")+output});
    QVERIFY2(resume.waitForFinished(60000),qPrintable(resume.errorString()));
    QCOMPARE(resume.exitStatus(),QProcess::NormalExit);
    QVERIFY2(resume.exitCode() != 0,
             qPrintable(QString::fromUtf8(resume.readAllStandardOutput()+resume.readAllStandardError())));
    QCOMPARE(sha256(corruptCheckpoint),corruptHash);
    QCOMPARE(sha256(output),outputHash);

    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QVERIFY(!journal.events.empty());
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFailed"));
    QVERIFY(journal.events.back().payload.value(QStringLiteral("resultsMayAlreadyExist")).toBool());
    QVERIFY(journal.events.back().payload.value(QStringLiteral("thermalCheckpointPreserved")).toBool());
}

void TestSolverAuditCli::thermalFailedMacrostepPreservesCheckpoint()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("unbalanced.ram"));
    const QString checkpoint = directory.filePath(QStringLiteral("previous.tfec"));
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("work")));
    makeThermalControlModel(true).write(input);
    const QString inputHash = sha256(input);
    QFile sentinel(checkpoint);
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    const QByteArray sentinelContents("previous accepted checkpoint");
    QCOMPARE(sentinel.write(sentinelContents),qint64(sentinelContents.size()));
    sentinel.close();
    const QString checkpointHash = sha256(checkpoint);
    const auto nonJournalFiles = [](const QDir &dir)
    {
        QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot,QDir::Name);
        files.erase(std::remove_if(files.begin(),files.end(),[](const QString &name) {
            return name.endsWith(QStringLiteral(".events.jsonl"));
        }),files.end());
        return files;
    };
    const QStringList filesBefore = nonJournalFiles(QDir(directory.path()));

    QProcess process;
    process.setWorkingDirectory(directory.filePath(QStringLiteral("work")));
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,QStringLiteral("--thermal-steps=2"),
         QStringLiteral("--thermal-checkpoint=")+checkpoint});
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() != 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError())));
    QCOMPARE(sha256(input),inputHash);
    QCOMPARE(sha256(checkpoint),checkpointHash);
    const QStringList filesAfter = nonJournalFiles(QDir(directory.path()));
    QVERIFY2(filesAfter == filesBefore,
             qPrintable(QStringLiteral("Files before: [%1]; after: [%2]")
                            .arg(filesBefore.join(QStringLiteral(", ")))
                            .arg(filesAfter.join(QStringLiteral(", ")))));
    QVERIFY(singleJournal(QDir(directory.path())).size() > 0);
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFailed"));
    QCOMPARE(journal.events.size(),2);
    QVERIFY(journal.events.back().payload.value(QStringLiteral("resultsMayAlreadyExist")).toBool());
    QVERIFY(journal.events.back().payload.value(QStringLiteral("thermalCheckpointPreserved")).toBool());
    QVERIFY(std::none_of(journal.events.begin(),journal.events.end(),[](const auto &event) {
        return event.type == QStringLiteral("ThermalMacrostepAccepted");
    }));
    QCOMPARE(QDir(directory.path()).entryList({QStringLiteral("*.rbm")},QDir::Files).size(),0);
}

void TestSolverAuditCli::thermalCliRejectsEnabledForceController()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("ambiguous.ram"));
    const QString checkpoint = directory.filePath(QStringLiteral("unused.tfec"));
    makeThermalControlModel(false,true).write(input);
    const QString inputHash = sha256(input);

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,QStringLiteral("--thermal-steps=1"),
         QStringLiteral("--thermal-checkpoint=")+checkpoint});
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() != 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError())));
    QCOMPARE(sha256(input),inputHash);
    QVERIFY(!QFile::exists(checkpoint));
    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFailed"));
    QVERIFY(journal.events.back().payload.value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("structural force controller")));
}

void TestSolverAuditCli::thermalCliRejectsConcurrentCheckpointWriter()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("input.ram"));
    const QString checkpoint = directory.filePath(QStringLiteral("state.tfec"));
    makeThermalControlModel().write(input);

    QLockFile heldLock(QFileInfo(checkpoint).absoluteFilePath()+QStringLiteral(".lock"));
    heldLock.setStaleLockTime(0);
    QVERIFY(heldLock.tryLock(0));
    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,QStringLiteral("--thermal-steps=1"),
         QStringLiteral("--thermal-checkpoint=")+checkpoint});
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() != 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError())));
    QVERIFY(!QFile::exists(checkpoint));

    const auto journal = ExecutionEventJournal::read(singleJournal(QDir(directory.path())));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.size(),1);
    QCOMPARE(journal.events.front().type,QStringLiteral("RunFailed"));
    QVERIFY(journal.events.front().payload.value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("ThermalCheckpointLocked")));
    QVERIFY(!journal.events.front().payload.value(QStringLiteral("resultsMayAlreadyExist")).toBool());
}

void TestSolverAuditCli::thermalCliRejectsOversizedMacrostepRequest()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("input.ram"));
    const QString checkpoint = directory.filePath(QStringLiteral("unused.tfec"));
    makeThermalControlModel().write(input);
    const QString inputHash = sha256(input);

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,QStringLiteral("--thermal-steps=1001"),
         QStringLiteral("--thermal-checkpoint=")+checkpoint});
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() != 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError())));
    QCOMPARE(sha256(input),inputHash);
    QVERIFY(!QFile::exists(checkpoint));
    QVERIFY(singleJournal(QDir(directory.path())).isEmpty());
}

void TestSolverAuditCli::thermalCliDetectsCheckpointChangeBeforePublish()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("setup")));
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("run")));
    const QString input = directory.filePath(QStringLiteral("setup/input.ram"));
    const QString seedCheckpoint = directory.filePath(QStringLiteral("setup/seed.tfec"));
    makeThermalControlModel().write(input);

    QProcess seed;
    seed.setWorkingDirectory(directory.filePath(QStringLiteral("setup")));
    seed.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,QStringLiteral("--thermal-steps=1"),
         QStringLiteral("--thermal-checkpoint=")+seedCheckpoint});
    QVERIFY2(seed.waitForFinished(60000),qPrintable(seed.errorString()));
    QCOMPARE(seed.exitStatus(),QProcess::NormalExit);
    QVERIFY2(seed.exitCode() == 0,
             qPrintable(QString::fromUtf8(seed.readAllStandardOutput()+seed.readAllStandardError())));

    const QString target = directory.filePath(QStringLiteral("run/state.tfec"));
    QVERIFY(QFile::copy(seedCheckpoint,target));
    QProcess process;
    process.setWorkingDirectory(directory.filePath(QStringLiteral("run")));
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+target,QStringLiteral("--thermal-steps=1000"),
         QStringLiteral("--thermal-checkpoint=")+target});

    QElapsedTimer timer;
    timer.start();
    QString journalPath;
    bool runStarted = false;
    while (timer.elapsed() < 10000 && process.state() != QProcess::NotRunning)
    {
        journalPath = singleJournal(QDir(directory.filePath(QStringLiteral("run"))));
        if (!journalPath.isEmpty())
        {
            const auto journal = ExecutionEventJournal::read(journalPath);
            runStarted = std::any_of(journal.events.begin(),journal.events.end(),[](const auto &event) {
                return event.type == QStringLiteral("SolverRunStarted");
            });
            if (runStarted) break;
        }
        QTest::qWait(1);
    }
    QVERIFY2(runStarted,"Thermal CLI did not reach the macrostep loop.");
    QVERIFY2(process.state() != QProcess::NotRunning,
             "Thermal CLI finished before the in-flight checkpoint mutation could be tested.");

    QFile externalChange(target);
    QVERIFY(externalChange.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray externalBytes("external writer replaced checkpoint during thermal computation");
    QCOMPARE(externalChange.write(externalBytes),qint64(externalBytes.size()));
    externalChange.close();
    const QString externalHash = sha256(target);

    QVERIFY2(process.waitForFinished(120000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() != 0,
             qPrintable(QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError())));
    QCOMPARE(sha256(target),externalHash);

    const auto journal = ExecutionEventJournal::read(journalPath);
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFailed"));
    QVERIFY(journal.events.back().payload.value(QStringLiteral("message")).toString()
                .contains(QStringLiteral("ThermalCheckpointConflict")));
    QVERIFY(journal.events.back().payload.value(QStringLiteral("resultsMayAlreadyExist")).toBool());
    QVERIFY(!journal.events.back().payload.value(QStringLiteral("thermalCheckpointPreserved")).toBool());
    QVERIFY(!journal.events.back().payload.value(QStringLiteral("thermalCheckpointCommitted")).toBool());
    QVERIFY(std::none_of(journal.events.begin(),journal.events.end(),[](const auto &event) {
        return event.type == QStringLiteral("ThermalMacrostepAccepted");
    }));
}


int main(int argc,char *argv[])
{
    QCoreApplication app(argc,argv);
    TestSolverAuditCli test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_solver_audit_cli.moc"
