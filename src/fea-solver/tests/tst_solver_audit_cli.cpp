#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#include "rstructural_control_conditioner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDataStream>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include "execution_event_journal.h"
#include "rml_model.h"
#include "rml_structural_dynamics_restart_data.h"
#include "rstructural_thermal_fe_adapter.h"
#include "rstructural_typed_control.h"
#include "rstructural_typed_fe_control.h"

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

QString sha256Bytes(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        bytes,QCryptographicHash::Sha256).toHex());
}

bool isLowerHexSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value)
    {
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
              (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

QJsonObject nativeCheckpointContext(const QString &modelPath,double dtS)
{
    const QString inputModelHash = sha256(modelPath);
    return {{QStringLiteral("schema"),QStringLiteral("range.native-checkpoint-context/v1")},
            {QStringLiteral("sourceSha256"),inputModelHash},
            {QStringLiteral("modelSha256"),inputModelHash},
            {QStringLiteral("controlSha256"),sha256Bytes(
                 QByteArrayLiteral("range.native-checkpoint.no-control.v1"))},
            {QStringLiteral("runtimeSha256"),sha256(solverExecutablePath())},
            {QStringLiteral("packageSha256"),sha256Bytes(
                 QByteArrayLiteral("range-native-checkpoint-test-package-v1"))},
            {QStringLiteral("policy"),QStringLiteral("accepted-steps-or-sim-time-v1")},
            {QStringLiteral("maxAcceptedSteps"),10},
            {QStringLiteral("maxSimulationSeconds"),1.0},
            {QStringLiteral("dtS"),dtS}};
}

bool runNativeCheckpointCli(const QString &input,
                            const QString &checkpointDirectory,
                            const QJsonObject &context,
                            bool restart,
                            QString &diagnostic)
{
    QStringList arguments{
        QStringLiteral("--file=")+input,
        QStringLiteral("--native-checkpoint-dir=")+checkpointDirectory,
        QStringLiteral("--native-checkpoint-context-json=")+
            QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact))};
    if (restart) arguments.append(QStringLiteral("--restart"));
    QProcess process;
    process.start(solverExecutablePath(),arguments);
    const bool finished = process.waitForFinished(60000);
    diagnostic = QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError());
    if (!finished)
    {
        process.kill();
        process.waitForFinished();
        diagnostic += process.errorString();
    }
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QVector<QJsonObject> nativeCheckpointEnvelopes(const QString &directory)
{
    QVector<QJsonObject> envelopes;
    QDir dir(directory);
    for (const QString &name : dir.entryList({QStringLiteral("envelope-*.json")},QDir::Files,QDir::Name))
    {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) return {};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) return {};
        envelopes.append(document.object());
    }
    return envelopes;
}

bool closePhysical(double actual,double expected,double characteristicScale,
                   double relativeTolerance = 1.0e-8)
{
    const double scale = std::max({characteristicScale,std::abs(actual),std::abs(expected)});
    return std::isfinite(actual) && std::isfinite(expected) && std::isfinite(scale) &&
           scale > 0.0 && std::abs(actual-expected) <= relativeTolerance*scale;
}

bool readJsonObject(const QString &path,QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return false;
    object = document.object();
    return true;
}

QVector<QJsonObject> nativeResultJournalRecords(const QString &path)
{
    QVector<QJsonObject> records;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return records;
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || (!bytes.isEmpty() && !bytes.endsWith('\n')))
        return {};
    for (const QByteArray &line : bytes.split('\n'))
    {
        if (line.isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) return {};
        records.append(document.object());
    }
    return records;
}

bool sendLocalCommand(QLocalSocket *socket,const QString &taskId,
                      const QString &command,bool fragmented = false)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return false;
    const QString message = taskId+QLatin1Char(':')+command;
    QByteArray frameBytes;
    QDataStream output(&frameBytes,QIODevice::WriteOnly);
    output.setVersion(QDataStream::Qt_6_8);
    output << quint32(message.size()) << message;
    if (output.status() != QDataStream::Ok) return false;
    if (fragmented && frameBytes.size() > 3)
    {
        if (socket->write(frameBytes.constData(),3) != 3 ||
            !socket->waitForBytesWritten(5000)) return false;
        QTest::qWait(20);
        if (socket->write(frameBytes.constData()+3,frameBytes.size()-3) !=
            frameBytes.size()-3) return false;
    }
    else if (socket->write(frameBytes) != frameBytes.size())
    {
        return false;
    }
    return socket->waitForBytesWritten(5000);
}

bool sendRepeatedResumeCommands(QLocalSocket *socket,const QString &taskId)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return false;
    QByteArray frames;
    QDataStream output(&frames,QIODevice::WriteOnly);
    output.setVersion(QDataStream::Qt_6_8);
    for (int index=0;index<2;index++)
    {
        const QString message = taskId+QStringLiteral(":RESUME");
        output << quint32(message.size()) << message;
    }
    if (output.status() != QDataStream::Ok ||
        socket->write(frames) != frames.size()) return false;
    return socket->waitForBytesWritten(5000);
}

bool sendMalformedLocalCommandFrame(QLocalSocket *socket)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState) return false;
    // The outer character count is six while the serialized QString is one
    // UTF-16 character. The parser must reject this frame without resuming.
    const QByteArray malformed = QByteArray::fromHex("00000006000000020041");
    if (socket->write(malformed) != malformed.size()) return false;
    return socket->waitForBytesWritten(5000);
}

QString typedControlPython()
{
    QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty()) python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    return python;
}

QString typedControlArguments(
    const QString &mode,
    const QString &commandsJson = QStringLiteral("{\"length\":1.98,\"prestress\":8.0,\"anchor\":0.05}"))
{
    const QJsonArray arguments{
        QStringLiteral(RANGE_TYPED_CONTROL_EXAMPLE_PATH),
        mode,
        commandsJson};
    return QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));
}

RModel makeTypedCableNativeModel()
{
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
    return model;
}

void writeTypedCableSeed(const QString &path)
{
    using Quantity = RStructuralTypedControlQuantity;
    RModel model = makeTypedCableNativeModel();
    std::vector<RStructuralTypedControlChannel> catalog;
    for (const auto &definition : {
             std::pair<QString,Quantity>{QStringLiteral("length"),Quantity::CableReferenceLength},
             std::pair<QString,Quantity>{QStringLiteral("prestress"),Quantity::CableInitialTension},
             std::pair<QString,Quantity>{QStringLiteral("anchor"),Quantity::CableAnchorDisplacement}})
    {
        RStructuralTypedControlChannel channel;
        channel.channelId = definition.first;
        channel.quantity = definition.second;
        channel.initialCommand = definition.first == QStringLiteral("length") ? 2.0 :
                                definition.first == QStringLiteral("prestress") ? 5.0 : 0.0;
        catalog.push_back(channel);
    }
    RStructuralTypedControlSession session(17,0.1,std::move(catalog),
                                           QStringLiteral("python-typed-v2"));
    const std::vector<RStructuralTypedFEControlBinding> bindings{
        {QStringLiteral("length"),0,0,0},
        {QStringLiteral("prestress"),0,0,0},
        {QStringLiteral("anchor"),0,0,0}};
    auto driver = RStructuralTypedFEControlDriver::cable(model,session,bindings);
    driver->writeCheckpoint(path);
}

void setTypedBeamComponent(RCondition &condition,
                           RVariableType type,
                           bool enabled,
                           double value = 0.0)
{
    const uint position = condition.findComponentPosition(type);
    RConditionComponent &component = condition.getComponent(position);
    component.setEnabled(enabled);
    component.clear();
    if (enabled) component.add(0.0,value);
}

RBoundaryCondition typedBeamTranslation(const std::array<bool,3> &enabled)
{
    RBoundaryCondition condition(R_BOUNDARY_CONDITION_DISPLACEMENT);
    const RVariableType types[3] = {R_VARIABLE_DISPLACEMENT_X,
                                    R_VARIABLE_DISPLACEMENT_Y,
                                    R_VARIABLE_DISPLACEMENT_Z};
    for (uint axis=0;axis<3;axis++) setTypedBeamComponent(condition,types[axis],enabled[axis]);
    return condition;
}

RBoundaryCondition typedBeamRotation(const std::array<bool,3> &enabled)
{
    RBoundaryCondition condition(R_BOUNDARY_CONDITION_ROTATION);
    const RVariableType types[3] = {R_VARIABLE_ROTATION_X,
                                    R_VARIABLE_ROTATION_Y,
                                    R_VARIABLE_ROTATION_Z};
    for (uint axis=0;axis<3;axis++) setTypedBeamComponent(condition,types[axis],enabled[axis]);
    return condition;
}

constexpr double typedBeamLength = 1.0;
constexpr double typedBeamYoungModulus = 1000.0;
constexpr double typedBeamArea = 0.5;
constexpr double typedBeamIz = 0.02;
constexpr double typedPatchArea = 0.01;
constexpr double typedPatchThickness = 0.1;
constexpr double typedPatchOffset = 0.2;
constexpr double typedPatchYoungModulus = 800.0;
constexpr double typedPiezoCoupling = -0.05;
constexpr double typedPermittivity = 0.0015;
constexpr double typedVoltage = 100.0;

RModel makeTypedPiezoBeamModel()
{
    RModel model;
    model.setNNodes(2);
    model.getNode(0).set(0.0,0.0,0.0);
    model.getNode(1).set(typedBeamLength,0.0,0.0);
    RElement beam(R_ELEMENT_BEAM1);
    beam.setNodeId(0,0);
    beam.setNodeId(1,1);
    model.addElement(beam,true,0);

    RLine &line = model.getLine(0);
    line.setCrossArea(typedBeamArea);
    line.setSecondMomentY(typedBeamIz);
    line.setSecondMomentZ(typedBeamIz);
    line.setTorsionalConstant(typedBeamIz);
    line.setLocalYDirection(RR3Vector(0.0,1.0,0.0));
    REnvironmentCondition &gravity =
        line.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
    for (RVariableType type : {R_VARIABLE_G_ACCELERATION_X,
                               R_VARIABLE_G_ACCELERATION_Y,
                               R_VARIABLE_G_ACCELERATION_Z})
        setTypedBeamComponent(gravity,type,true,0.0);

    RMaterial material(RMaterial::Solid);
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15,typedBeamYoungModulus);
    material.add(modulus);
    RMaterialProperty density(RMaterialProperty::Density);
    density.add(293.15,1.0);
    material.add(density);
    line.setMaterial(material);

    for (uint node=0;node<2;node++)
    {
        RElement point(R_ELEMENT_POINT);
        point.setNodeId(0,node);
        model.addElement(point,true,node);
        if (node==0)
        {
            model.getPoint(node).addBoundaryCondition(typedBeamTranslation({{true,true,true}}));
            model.getPoint(node).addBoundaryCondition(typedBeamRotation({{true,true,true}}));
        }
        else
        {
            model.getPoint(node).addBoundaryCondition(typedBeamTranslation({{false,false,true}}));
            model.getPoint(node).addBoundaryCondition(typedBeamRotation({{true,true,false}}));
        }
    }
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    RMatrixSolverConf &gmres = model.getMatrixSolverConf(RMatrixSolverConf::GMRES);
    gmres.setNInnerIterations(80);
    gmres.setNOuterIterations(20);
    gmres.setSolverCvgValue(1.0e-12);
    return model;
}

QJsonObject typedCableBootstrapManifest(const QString &fault = {})
{
    QJsonObject length{{"id","length"},{"quantity","cable_reference_length"},
                       {"element",0},{"initial",2.0},{"minimum",0.1},{"maximum",3.0}};
    QJsonObject tension{{"id","tension"},{"quantity","cable_initial_tension"},
                        {"element",0},{"initial",5.0},{"minimum",0.0},{"maximum",100.0}};
    QJsonObject anchor{{"id","anchor"},{"quantity","cable_anchor_displacement"},
                       {"node",0},{"axis",0},{"initial",0.0},{"minimum",-0.1},{"maximum",0.1}};
    if (fault==QStringLiteral("fractional-element")) length[QStringLiteral("element")]=0.5;
    if (fault==QStringLiteral("missing-initial")) length.remove(QStringLiteral("initial"));
    if (fault==QStringLiteral("misspelled-limit")) length[QStringLiteral("maxium")]=1.99;
    return {{"version",1},{"mode","cable"},{"seed","17"},{"dt",0.1},
            {"controller","python-typed-v2"},{"channels",QJsonArray{length,tension,anchor}}};
}

QJsonObject typedPiezoBootstrapManifest()
{
    const QJsonObject channel{{"id","patch"},{"quantity","voltage"},{"element",0},
                              {"initial",0.0},{"minimum",-100.0},{"maximum",100.0},
                              {"rate",1000.0}};
    const QJsonObject patch{{"element",0},{"area",typedPatchArea},
                            {"thickness",typedPatchThickness},{"offset",typedPatchOffset},
                            {"modulus",typedPatchYoungModulus},{"coupling",typedPiezoCoupling},
                            {"permittivity",typedPermittivity},{"initial",0.0}};
    return {{"version",1},{"mode","piezo_beam"},{"seed","17"},{"dt",0.1},
            {"controller","python-typed-v2"},{"electricMode","voltage"},
            {"channels",QJsonArray{channel}},{"patches",QJsonArray{patch}}};
}

bool writeJsonObject(const QString &path,const QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly|QIODevice::Truncate)) return false;
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.write(bytes)==bytes.size();
}

bool runTypedControlCli(const QString &input,
                        const QString &checkpoint,
                        const QString &steps,
                        const QString &program,
                        const QString &mode,
                        QString &diagnostic)
{
    QProcess process;
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,
         QStringLiteral("--typed-steps=")+steps,
         QStringLiteral("--typed-checkpoint=")+checkpoint,
         QStringLiteral("--typed-controller-id=python-typed-v2"),
         QStringLiteral("--control-program=")+program,
         QStringLiteral("--control-args-json=")+typedControlArguments(mode)});
    const bool finished = process.waitForFinished(60000);
    diagnostic = QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError());
    if (!finished)
    {
        process.kill();
        process.waitForFinished();
        diagnostic += process.errorString();
    }
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool runTypedBootstrapCli(const QString &input,
                          const QString &configuration,
                          const QString &checkpoint,
                          const QString &steps,
                          const QString &program,
                          const QString &mode,
                          const QString &commandsJson,
                          QString &diagnostic)
{
    QProcess process;
    process.start(solverExecutablePath(),
        {QStringLiteral("--file=")+input,
         QStringLiteral("--typed-config=")+configuration,
         QStringLiteral("--typed-steps=")+steps,
         QStringLiteral("--typed-checkpoint=")+checkpoint,
         QStringLiteral("--typed-controller-id=python-typed-v2"),
         QStringLiteral("--control-program=")+program,
         QStringLiteral("--control-args-json=")+typedControlArguments(mode,commandsJson)});
    const bool finished = process.waitForFinished(60000);
    diagnostic = QString::fromUtf8(process.readAllStandardOutput()+process.readAllStandardError());
    if (!finished)
    {
        process.kill();
        process.waitForFinished();
        diagnostic += process.errorString();
    }
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QVector<QJsonObject> typedPreparedSteps(const ExecutionEventReadResult &journal)
{
    QVector<QJsonObject> steps;
    for (const auto &event : journal.events)
        if (event.type == QStringLiteral("TypedStepPrepared")) steps.append(event.payload);
    return steps;
}

bool closeTypedValue(double actual,double expected)
{
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual-expected) <= 1.0e-11*(1.0+std::max(std::abs(actual),std::abs(expected)));
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
    void nativeAcceptedCheckpointsFollowPolicyAndIgnoreOutputFrequency();
    void nativeAcceptedCheckpointRejectsCorruptionAndDiskFailure();
    void nativeExecutionGatePausesAtAcceptedBoundaryAndResumesExactly();
    void nativeExecutionGateStopCancelsAtAcceptedBoundary();
    void nativeExecutionGateHostDisconnectCancelsPausedRun();
    void nativeExecutionGateRefusesExternalStateReplacement();
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
    void typedCableCliRestartMatchesUninterrupted();
    void typedCableCliInvalidUnitPreservesCheckpoint();
    void typedCableCliRejectsLockedCheckpoint();
    void typedCableCliRejectsOversizedStepCount();
    void typedCableBootstrapUsesNativeModelAndPythonController();
    void typedCableBootstrapRejectsMalformedManifest_data();
    void typedCableBootstrapRejectsMalformedManifest();
    void typedPiezoBootstrapUsesNativeBeamAndPythonController();
    void staticCablePersistsCertifiedResult_data();
    void staticCablePersistsCertifiedResult();
    void staticPiezoPersistsElectrodeCharge_data();
    void staticPiezoPersistsElectrodeCharge();
    void nodalPiezoPersistsPotentialAndCharge_data();
    void nodalPiezoPersistsPotentialAndCharge();
};

void TestSolverAuditCli::nativeAcceptedCheckpointsFollowPolicyAndIgnoreOutputFrequency()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    RModel base = makeModel();
    auto &setup = base.getProblemSetup().getStructuralDynamicsSetup();
    setup.setTimeStep(1.0e-5);
    setup.setNSteps(25);
    setup.setOutputFrequency(1);
    const QString denseInput = directory.filePath(QStringLiteral("dense.rbm"));
    const QString sparseInput = directory.filePath(QStringLiteral("sparse.rbm"));
    QCOMPARE(base.write(denseInput,false),denseInput);
    QVERIFY(QFile::copy(denseInput,sparseInput));
    const QString denseDirectory = directory.filePath(QStringLiteral("dense-checkpoints"));
    const QString sparseDirectory = directory.filePath(QStringLiteral("sparse-checkpoints"));
    const QJsonObject denseContext = nativeCheckpointContext(denseInput,1.0e-5);
    QString diagnostic;
    QVERIFY2(runNativeCheckpointCli(denseInput,denseDirectory,denseContext,false,diagnostic),
             qPrintable(diagnostic));

    RModel sparseBase;
    sparseBase.read(sparseInput);
    sparseBase.getProblemSetup().getStructuralDynamicsSetup().setOutputFrequency(7);
    QCOMPARE(sparseBase.write(sparseInput,false),sparseInput);
    const QJsonObject adjustedSparseContext = nativeCheckpointContext(sparseInput,1.0e-5);
    QVERIFY2(runNativeCheckpointCli(sparseInput,sparseDirectory,adjustedSparseContext,false,diagnostic),
             qPrintable(diagnostic));

    auto denseEnvelopes = nativeCheckpointEnvelopes(denseDirectory);
    auto sparseEnvelopes = nativeCheckpointEnvelopes(sparseDirectory);
    auto sequenceList = [](const QVector<QJsonObject> &envelopes)
    {
        QVector<quint64> values;
        for (const QJsonObject &envelope : envelopes)
            values.append(quint64(envelope.value(QStringLiteral("acceptedSeq")).toDouble()));
        return values;
    };
    const QVector<quint64> expectedSteps{10,20,25};
    QCOMPARE(sequenceList(denseEnvelopes),expectedSteps);
    QCOMPARE(sequenceList(sparseEnvelopes),expectedSteps);

    auto statesByStep = [](const QVector<QJsonObject> &envelopes,const QString &checkpointDirectory)
    {
        QMap<quint64,RStructuralDynamicsRestartData> states;
        for (const QJsonObject &envelope : envelopes)
        {
            const quint64 step = quint64(envelope.value(QStringLiteral("acceptedSeq")).toDouble());
            RModel checkpoint;
            checkpoint.read(QDir(checkpointDirectory).filePath(
                envelope.value(QStringLiteral("modelFile")).toString()));
            states.insert(step,checkpoint.getProblemSetup().getStructuralDynamicsRestartData());
        }
        return states;
    };
    const auto denseStates = statesByStep(denseEnvelopes,denseDirectory);
    const auto sparseStates = statesByStep(sparseEnvelopes,sparseDirectory);
    const double omega = std::sqrt(3.0*2.0e11/(8000.0*2.0*2.0));
    const double qScale = 1.0e-5;
    const double vScale = qScale*omega;
    const double aScale = qScale*omega*omega;
    const double energyScale = 0.5*(2.0e11*0.01/2.0)*qScale*qScale;
    for (quint64 step : expectedSteps)
    {
        const auto &dense = denseStates[step];
        const auto &sparse = sparseStates[step];
        QCOMPARE(dense.completedSteps,uint(step));
        QCOMPARE(sparse.completedSteps,dense.completedSteps);
        QCOMPARE(sparse.systemFingerprint,dense.systemFingerprint);
        QCOMPARE(sparse.controlControllerState,dense.controlControllerState);
        QCOMPARE(sparse.cumulativeControlWork,dense.cumulativeControlWork);
        QCOMPARE(sparse.q.size(),dense.q.size());
        QCOMPARE(sparse.v.size(),dense.v.size());
        QCOMPARE(sparse.a.size(),dense.a.size());
        for (std::size_t i=0;i<dense.q.size();++i)
        {
            QVERIFY(closePhysical(sparse.q[i],dense.q[i],qScale));
            QVERIFY(closePhysical(sparse.v[i],dense.v[i],vScale));
            QVERIFY(closePhysical(sparse.a[i],dense.a[i],aScale));
        }
        QVERIFY(closePhysical(sparse.cumulativeExternalWork,
                              dense.cumulativeExternalWork,energyScale));
        QVERIFY(closePhysical(sparse.cumulativeDampingDissipation,
                              dense.cumulativeDampingDissipation,energyScale));
    }

    RModel timeBound = makeModel();
    auto &timeSetup = timeBound.getProblemSetup().getStructuralDynamicsSetup();
    timeSetup.setTimeStep(0.25);
    timeSetup.setNSteps(10);
    timeSetup.setOutputFrequency(7);
    const QString timeInput = directory.filePath(QStringLiteral("time-bound.rbm"));
    QCOMPARE(timeBound.write(timeInput,false),timeInput);
    const QString timeCheckpointDirectory = directory.filePath(QStringLiteral("time-bound-checkpoints"));
    QVERIFY2(runNativeCheckpointCli(timeInput,timeCheckpointDirectory,
             nativeCheckpointContext(timeInput,0.25),false,diagnostic),qPrintable(diagnostic));
    const QVector<QJsonObject> timeEnvelopes = nativeCheckpointEnvelopes(timeCheckpointDirectory);
    QCOMPARE(sequenceList(timeEnvelopes),QVector<quint64>({4,8,10}));
}

void TestSolverAuditCli::nativeAcceptedCheckpointRejectsCorruptionAndDiskFailure()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = makeModel();
    auto &setup = model.getProblemSetup().getStructuralDynamicsSetup();
    setup.setTimeStep(1.0e-5);
    setup.setNSteps(10);
    setup.setOutputFrequency(10);
    const QString input = directory.filePath(QStringLiteral("accepted-input.rbm"));
    QCOMPARE(model.write(input,false),input);
    const QByteArray originalInput = [&]()
    {
        QFile file(input);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        return file.readAll();
    }();
    QVERIFY(!originalInput.isEmpty());
    const QString checkpointDirectory = directory.filePath(QStringLiteral("accepted-checkpoints"));
    const QJsonObject context = nativeCheckpointContext(input,1.0e-5);
    QString diagnostic;
    QVERIFY2(runNativeCheckpointCli(input,checkpointDirectory,context,false,diagnostic),
             qPrintable(diagnostic));

    QFile firstPointer(QDir(checkpointDirectory).filePath(QStringLiteral("latest.json")));
    QVERIFY(firstPointer.open(QIODevice::ReadOnly));
    const QJsonObject firstPointerObject =
        QJsonDocument::fromJson(firstPointer.readAll()).object();
    firstPointer.close();
    const QJsonObject firstEnvelope = QJsonDocument::fromJson(
        [&]()
        {
            QFile file(QDir(checkpointDirectory).filePath(
                firstPointerObject.value(QStringLiteral("envelopeFile")).toString()));
            if (!file.open(QIODevice::ReadOnly)) return QByteArray();
            return file.readAll();
        }()).object();
    const QString firstModelPath = QDir(checkpointDirectory).filePath(
        firstEnvelope.value(QStringLiteral("modelFile")).toString());
    const QString validResumeInput = directory.filePath(QStringLiteral("valid-resume-input.rbm"));
    QVERIFY(QFile::copy(firstModelPath,validResumeInput));
    QVERIFY2(runNativeCheckpointCli(validResumeInput,checkpointDirectory,context,true,diagnostic),
             qPrintable(diagnostic));
    const QVector<QJsonObject> beforeCorruption = nativeCheckpointEnvelopes(checkpointDirectory);
    QVector<quint64> beforeCorruptionSteps;
    for (const QJsonObject &envelope : beforeCorruption)
        beforeCorruptionSteps.append(quint64(envelope.value(QStringLiteral("acceptedSeq")).toDouble()));
    QCOMPARE(beforeCorruptionSteps,QVector<quint64>({10,20}));
    QFile lastValidPointer(QDir(checkpointDirectory).filePath(QStringLiteral("lastvalid.json")));
    QVERIFY(lastValidPointer.open(QIODevice::ReadOnly));
    const QJsonObject lastValidObject = QJsonDocument::fromJson(lastValidPointer.readAll()).object();
    lastValidPointer.close();
    QFile lastValidEnvelopeFile(QDir(checkpointDirectory).filePath(
        lastValidObject.value(QStringLiteral("envelopeFile")).toString()));
    QVERIFY(lastValidEnvelopeFile.open(QIODevice::ReadOnly));
    const QJsonObject lastValidEnvelope =
        QJsonDocument::fromJson(lastValidEnvelopeFile.readAll()).object();
    QCOMPARE(lastValidEnvelope.value(QStringLiteral("acceptedSeq")).toInt(),10);
    lastValidEnvelopeFile.close();

    QFile pointer(QDir(checkpointDirectory).filePath(QStringLiteral("latest.json")));
    QVERIFY(pointer.open(QIODevice::ReadOnly));
    const QByteArray pointerBytes = pointer.readAll();
    pointer.close();
    const QJsonDocument pointerDocument = QJsonDocument::fromJson(pointerBytes);
    QVERIFY(pointerDocument.isObject());
    const QString envelopeName = pointerDocument.object()
        .value(QStringLiteral("envelopeFile")).toString();
    const QString envelopePath = QDir(checkpointDirectory).filePath(envelopeName);
    QFile envelope(envelopePath);
    QVERIFY(envelope.open(QIODevice::ReadOnly));
    QByteArray envelopeBytes = envelope.readAll();
    envelope.close();
    QVERIFY(!envelopeBytes.isEmpty());
    envelopeBytes[envelopeBytes.size()-1] = char(envelopeBytes.back() ^ 0x5a);
    QVERIFY(envelope.open(QIODevice::WriteOnly|QIODevice::Truncate));
    QCOMPARE(envelope.write(envelopeBytes),qint64(envelopeBytes.size()));
    envelope.close();

    // The tampered envelope may no longer parse; retrieve the immutable MODEL
    // name from the already written original envelope in its unmodified bytes.
    QByteArray restoredEnvelopeBytes = envelopeBytes;
    restoredEnvelopeBytes[restoredEnvelopeBytes.size()-1] = char(
        restoredEnvelopeBytes.back() ^ 0x5a);
    const QString resumeModelPath = QDir(checkpointDirectory).filePath(
        QJsonDocument::fromJson(restoredEnvelopeBytes).object()
            .value(QStringLiteral("modelFile")).toString());
    QVERIFY(QFile::exists(resumeModelPath));
    const QString corruptResumeInput = directory.filePath(QStringLiteral("corrupt-resume-input.rbm"));
    QVERIFY(QFile::copy(resumeModelPath,corruptResumeInput));

    QVERIFY(!runNativeCheckpointCli(corruptResumeInput,checkpointDirectory,context,true,diagnostic));
    QVERIFY2(diagnostic.contains(QStringLiteral("checkpoint"),Qt::CaseInsensitive),
             qPrintable(diagnostic));
    // A valid older MODEL cannot authorize silent rollback past damaged latest.
    const QString olderResumeInput = directory.filePath(QStringLiteral("older-resume-input.rbm"));
    QVERIFY(QFile::copy(firstModelPath,olderResumeInput));
    const QString olderHash = sha256(olderResumeInput);
    QVERIFY(!runNativeCheckpointCli(olderResumeInput,checkpointDirectory,context,true,diagnostic));
    QCOMPARE(sha256(olderResumeInput),olderHash);
    QFile pointerAfter(QDir(checkpointDirectory).filePath(QStringLiteral("latest.json")));
    QVERIFY(pointerAfter.open(QIODevice::ReadOnly));
    QCOMPARE(pointerAfter.readAll(),pointerBytes);
    pointerAfter.close();

    const QString blockerPath = directory.filePath(QStringLiteral("checkpoint-path-is-file"));
    QFile blocker(blockerPath);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    const QByteArray blockerBytes("keep this file intact");
    QCOMPARE(blocker.write(blockerBytes),qint64(blockerBytes.size()));
    blocker.close();
    const QString blockerHash = sha256(blockerPath);
    const QString diskFailureInput = directory.filePath(QStringLiteral("disk-failure-input.rbm"));
    QFile pristine(diskFailureInput);
    QVERIFY(pristine.open(QIODevice::WriteOnly));
    QCOMPARE(pristine.write(originalInput),qint64(originalInput.size()));
    pristine.close();
    QVERIFY(!runNativeCheckpointCli(diskFailureInput,blockerPath,
             nativeCheckpointContext(diskFailureInput,1.0e-5),false,diagnostic));
    QCOMPARE(sha256(blockerPath),blockerHash);
}

void TestSolverAuditCli::nativeExecutionGatePausesAtAcceptedBoundaryAndResumesExactly()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const double dtS = 1.0e-5;
    auto writeTwentyStepModel = [dtS](const QString &path)
    {
        RModel model = makeModel();
        RStructuralDynamicsSetup &setup =
            model.getProblemSetup().getStructuralDynamicsSetup();
        setup.setTimeStep(dtS);
        setup.setNSteps(20);
        setup.setOutputFrequency(1);
        return model.write(path,false) == path;
    };

    const QString resultDirectory = directory.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    const QString fullInput = directory.filePath(QStringLiteral("continuous.rbm"));
    const QString pausedInput = QDir(resultDirectory).filePath(QStringLiteral("paused.rbm"));
    QVERIFY(writeTwentyStepModel(fullInput));
    QVERIFY(writeTwentyStepModel(pausedInput));
    const QString fullCheckpointDirectory = directory.filePath(QStringLiteral("continuous-checkpoints"));
    const QString pausedCheckpointDirectory = directory.filePath(QStringLiteral("paused-checkpoints"));
    QVERIFY(QDir().mkpath(fullCheckpointDirectory));
    QVERIFY(QDir().mkpath(pausedCheckpointDirectory));
    QString diagnostic;
    QVERIFY2(runNativeCheckpointCli(fullInput,fullCheckpointDirectory,
             nativeCheckpointContext(fullInput,dtS),false,diagnostic),qPrintable(diagnostic));

    const QString eventDirectory = directory.filePath(QStringLiteral("events"));
    QVERIFY(QDir().mkpath(eventDirectory));
    const QString jobId = QStringLiteral("solver-audit-job-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString taskId = QStringLiteral("solver-audit-task-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString serverName = QStringLiteral("range-solver-audit-")+
        QUuid::createUuid().toString(QUuid::Id128);
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    QVERIFY(server.listen(serverName));

    const QString resultJournalPath = QDir(eventDirectory)
        .filePath(QStringLiteral("native-result-frames.jsonl"));
    const QString statePath = QDir(eventDirectory)
        .filePath(QStringLiteral("native-execution-state.json"));
    const QJsonObject context = nativeCheckpointContext(pausedInput,dtS);
    const QByteArray contextBytes = QJsonDocument(context).toJson(QJsonDocument::Compact);
    const QString initialSourceSha256 = context.value(QStringLiteral("sourceSha256")).toString();
    const QString initialModelSha256 = context.value(QStringLiteral("modelSha256")).toString();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("RANGE_NATIVE_JOB_ID"),jobId);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_DIRECTORY"),resultDirectory);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_JOURNAL_PATH"),resultJournalPath);

    QProcess process;
    const auto processCleanup = qScopeGuard([&process]
    {
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(5000);
        }
    });
    process.setProcessEnvironment(environment);
    process.start(solverExecutablePath(),{
        QStringLiteral("--file=")+pausedInput,
        QStringLiteral("--native-checkpoint-dir=")+pausedCheckpointDirectory,
        QStringLiteral("--native-checkpoint-context-json=")+QString::fromUtf8(contextBytes),
        QStringLiteral("--pause-at-accepted-seq=15"),
        QStringLiteral("--native-execution-state=")+statePath,
        QStringLiteral("--task-id=")+taskId,
        QStringLiteral("--task-server=")+serverName});
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(),10000);
    QLocalSocket *hostSocket = server.nextPendingConnection();
    QVERIFY(hostSocket);

    QJsonObject pausedState;
    QElapsedTimer pauseTimer;
    pauseTimer.start();
    while (pauseTimer.elapsed() < 15000)
    {
        readJsonObject(statePath,pausedState);
        if (pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"))
            break;
        if (process.state() == QProcess::NotRunning)
            break;
        QTest::qWait(10);
    }
    const QString beforeResumeDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QVERIFY2(pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"),
             qPrintable(beforeResumeDiagnostic));
    QVERIFY(process.state() != QProcess::NotRunning);
    QCOMPARE(pausedState.value(QStringLiteral("schema")).toString(),
             QStringLiteral("range.native-execution-state/v1"));
    QCOMPARE(pausedState.value(QStringLiteral("generation")).toInt(),1);
    QCOMPARE(pausedState.value(QStringLiteral("jobId")).toString(),jobId);
    QCOMPARE(pausedState.value(QStringLiteral("taskId")).toString(),taskId);
    QCOMPARE(pausedState.value(QStringLiteral("pauseAtAcceptedSeq")).toDouble(),15.0);
    QCOMPARE(pausedState.value(QStringLiteral("acceptedSeq")).toDouble(),15.0);
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAvailable")).toBool(),true);
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAcceptedSeq")).toDouble(),15.0);
    QCOMPARE(pausedState.value(QStringLiteral("sourceSha256")).toString(),initialSourceSha256);
    QCOMPARE(pausedState.value(QStringLiteral("modelSha256")).toString(),initialModelSha256);
    QCOMPARE(pausedState.value(QStringLiteral("contextSha256")).toString(),sha256Bytes(contextBytes));
    for (const QString &name : {QStringLiteral("sourceSha256"),QStringLiteral("modelSha256"),
                                QStringLiteral("controlSha256"),QStringLiteral("runtimeSha256"),
                                QStringLiteral("packageSha256")})
        QCOMPARE(pausedState.value(name).toString(),context.value(name).toString());

    const QString latestPointerPath = QDir(pausedCheckpointDirectory)
        .filePath(QStringLiteral("latest.json"));
    QJsonObject latestPointer;
    QVERIFY(readJsonObject(latestPointerPath,latestPointer));
    const QString envelopePath = QDir(pausedCheckpointDirectory).filePath(
        latestPointer.value(QStringLiteral("envelopeFile")).toString());
    QFile envelopeFile(envelopePath);
    QVERIFY(envelopeFile.open(QIODevice::ReadOnly));
    const QByteArray envelopeBytes = envelopeFile.readAll();
    const QJsonDocument envelopeDocument = QJsonDocument::fromJson(envelopeBytes);
    QVERIFY(envelopeDocument.isObject());
    const QJsonObject envelopeObject = envelopeDocument.object();
    QCOMPARE(pausedState.value(QStringLiteral("checkpointEnvelopeSha256")).toString(),
             sha256Bytes(envelopeBytes));
    QCOMPARE(pausedState.value(QStringLiteral("checkpointModelSha256")).toString(),
             envelopeObject.value(QStringLiteral("checkpointModelSha256")).toString());
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAcceptedSeq")).toDouble(),
             envelopeObject.value(QStringLiteral("acceptedSeq")).toDouble());
    QCOMPARE(pausedState.value(QStringLiteral("checkpointTimeS")).toDouble(),
             envelopeObject.value(QStringLiteral("timeS")).toDouble());

    const QVector<QJsonObject> recordsAtPause = nativeResultJournalRecords(resultJournalPath);
    QCOMPARE(recordsAtPause.size(),15);
    QCOMPARE(recordsAtPause.last().value(QStringLiteral("seq")).toInt(),15);
    QCOMPARE(qRound64(recordsAtPause.last().value(QStringLiteral("timeS")).toDouble()/dtS),15LL);
    QFile stateFile(statePath);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    const QByteArray pausedStateBytes = stateFile.readAll();
    stateFile.close();
    QVERIFY(sendLocalCommand(hostSocket,taskId+QStringLiteral("-wrong"),QStringLiteral("RESUME")));
    QVERIFY(sendMalformedLocalCommandFrame(hostSocket));
    QTest::qWait(50);
    QVERIFY(process.state() != QProcess::NotRunning);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    QCOMPARE(stateFile.readAll(),pausedStateBytes);
    stateFile.close();
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),15);

    QVERIFY(sendRepeatedResumeCommands(hostSocket,taskId));
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    const QString completedDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,qPrintable(completedDiagnostic));

    QJsonObject resumedState;
    QVERIFY(readJsonObject(statePath,resumedState));
    QCOMPARE(resumedState.value(QStringLiteral("state")).toString(),QStringLiteral("resumed"));
    QCOMPARE(resumedState.value(QStringLiteral("generation")).toInt(),2);
    QCOMPARE(resumedState.value(QStringLiteral("acceptedSeq")).toDouble(),15.0);
    const QVector<QJsonObject> completedRecords = nativeResultJournalRecords(resultJournalPath);
    QCOMPARE(completedRecords.size(),20);
    QCOMPARE(completedRecords.last().value(QStringLiteral("seq")).toInt(),20);

    RModel continuous, resumed;
    continuous.read(fullInput);
    resumed.read(pausedInput);
    const RStructuralDynamicsRestartData &expected =
        continuous.getProblemSetup().getStructuralDynamicsRestartData();
    const RStructuralDynamicsRestartData &actual =
        resumed.getProblemSetup().getStructuralDynamicsRestartData();
    QCOMPARE(actual.completedSteps,20u);
    QCOMPARE(actual.systemFingerprint,expected.systemFingerprint);
    QCOMPARE(actual.controlControllerState,expected.controlControllerState);
    QCOMPARE(actual.cumulativeControlWork,expected.cumulativeControlWork);
    QCOMPARE(actual.q.size(),expected.q.size());
    QCOMPARE(actual.v.size(),expected.v.size());
    QCOMPARE(actual.a.size(),expected.a.size());
    const double omega = std::sqrt(3.0*2.0e11/(8000.0*2.0*2.0));
    const double qScale = 1.0e-5;
    const double vScale = qScale*omega;
    const double aScale = qScale*omega*omega;
    const double energyScale = 0.5*(2.0e11*0.01/2.0)*qScale*qScale;
    for (std::size_t index=0;index<expected.q.size();++index)
    {
        QVERIFY(closePhysical(actual.q[index],expected.q[index],qScale));
        QVERIFY(closePhysical(actual.v[index],expected.v[index],vScale,1.0e-7));
        QVERIFY(closePhysical(actual.a[index],expected.a[index],aScale,1.0e-7));
    }
    QVERIFY(closePhysical(actual.cumulativeExternalWork,
                          expected.cumulativeExternalWork,energyScale));
    QVERIFY(closePhysical(actual.cumulativeDampingDissipation,
                          expected.cumulativeDampingDissipation,energyScale));
}

void TestSolverAuditCli::nativeExecutionGateStopCancelsAtAcceptedBoundary()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const double dtS = 1.0e-5;
    const QString resultDirectory = directory.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    RModel model = makeModel();
    auto &setup = model.getProblemSetup().getStructuralDynamicsSetup();
    setup.setTimeStep(dtS);
    setup.setNSteps(20);
    setup.setOutputFrequency(1);
    const QString input = QDir(resultDirectory).filePath(QStringLiteral("cancel-at-boundary.rbm"));
    QCOMPARE(model.write(input,false),input);
    const QString checkpointDirectory = directory.filePath(QStringLiteral("checkpoints"));
    const QString eventsDirectory = directory.filePath(QStringLiteral("events"));
    QVERIFY(QDir().mkpath(checkpointDirectory));
    QVERIFY(QDir().mkpath(eventsDirectory));
    const QString jobId = QStringLiteral("solver-audit-cancel-job-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString taskId = QStringLiteral("solver-audit-cancel-task-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString serverName = QStringLiteral("range-solver-cancel-")+
        QUuid::createUuid().toString(QUuid::Id128);
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    QVERIFY(server.listen(serverName));

    const QString resultJournalPath = QDir(eventsDirectory)
        .filePath(QStringLiteral("native-result-frames.jsonl"));
    const QString statePath = QDir(eventsDirectory)
        .filePath(QStringLiteral("native-execution-state.json"));
    const QByteArray contextBytes = QJsonDocument(
        nativeCheckpointContext(input,dtS)).toJson(QJsonDocument::Compact);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("RANGE_NATIVE_JOB_ID"),jobId);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_DIRECTORY"),resultDirectory);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_JOURNAL_PATH"),resultJournalPath);

    QProcess process;
    const auto processCleanup = qScopeGuard([&process]
    {
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(5000);
        }
    });
    process.setProcessEnvironment(environment);
    process.start(solverExecutablePath(),{
        QStringLiteral("--file=")+input,
        QStringLiteral("--native-checkpoint-dir=")+checkpointDirectory,
        QStringLiteral("--native-checkpoint-context-json=")+QString::fromUtf8(contextBytes),
        QStringLiteral("--pause-at-accepted-seq=10"),
        QStringLiteral("--native-execution-state=")+statePath,
        QStringLiteral("--task-id=")+taskId,
        QStringLiteral("--task-server=")+serverName});
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(),10000);
    QLocalSocket *hostSocket = server.nextPendingConnection();
    QVERIFY(hostSocket);

    QJsonObject pausedState;
    QElapsedTimer pauseTimer;
    pauseTimer.start();
    while (pauseTimer.elapsed() < 15000)
    {
        readJsonObject(statePath,pausedState);
        if (pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"))
            break;
        if (process.state() == QProcess::NotRunning)
            break;
        QTest::qWait(10);
    }
    const QString pauseDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QVERIFY2(pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"),
             qPrintable(pauseDiagnostic));
    QCOMPARE(pausedState.value(QStringLiteral("acceptedSeq")).toDouble(),10.0);
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAvailable")).toBool(),true);
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAcceptedSeq")).toDouble(),10.0);
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),10);

    QVERIFY(sendLocalCommand(hostSocket,taskId,QStringLiteral("STOP"),true));
    QVERIFY2(process.waitForFinished(60000),qPrintable(process.errorString()));
    const QString stoppedDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,qPrintable(stoppedDiagnostic));
    QJsonObject cancelledState;
    QVERIFY(readJsonObject(statePath,cancelledState));
    QCOMPARE(cancelledState.value(QStringLiteral("state")).toString(),QStringLiteral("cancelled"));
    QCOMPARE(cancelledState.value(QStringLiteral("generation")).toInt(),2);
    QCOMPARE(cancelledState.value(QStringLiteral("acceptedSeq")).toDouble(),10.0);
    QCOMPARE(cancelledState.value(QStringLiteral("checkpointAcceptedSeq")).toDouble(),10.0);
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),10);

    RModel stopped;
    stopped.read(input);
    QCOMPARE(stopped.getProblemSetup().getStructuralDynamicsRestartData().completedSteps,10u);
}

void TestSolverAuditCli::nativeExecutionGateHostDisconnectCancelsPausedRun()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const double dtS = 1.0e-5;
    const QString resultDirectory = directory.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    RModel model = makeModel();
    auto &setup = model.getProblemSetup().getStructuralDynamicsSetup();
    setup.setTimeStep(dtS);
    setup.setNSteps(20);
    setup.setOutputFrequency(1);
    const QString input = QDir(resultDirectory).filePath(QStringLiteral("host-disconnect.rbm"));
    QCOMPARE(model.write(input,false),input);
    const QString checkpointDirectory = directory.filePath(QStringLiteral("checkpoints"));
    const QString eventsDirectory = directory.filePath(QStringLiteral("events"));
    QVERIFY(QDir().mkpath(checkpointDirectory));
    QVERIFY(QDir().mkpath(eventsDirectory));
    const QString jobId = QStringLiteral("solver-audit-disconnect-job-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString taskId = QStringLiteral("solver-audit-disconnect-task-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString serverName = QStringLiteral("range-solver-disconnect-")+
        QUuid::createUuid().toString(QUuid::Id128);
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    QVERIFY(server.listen(serverName));

    const QString resultJournalPath = QDir(eventsDirectory)
        .filePath(QStringLiteral("native-result-frames.jsonl"));
    const QString statePath = QDir(eventsDirectory)
        .filePath(QStringLiteral("native-execution-state.json"));
    const QByteArray contextBytes = QJsonDocument(
        nativeCheckpointContext(input,dtS)).toJson(QJsonDocument::Compact);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("RANGE_NATIVE_JOB_ID"),jobId);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_DIRECTORY"),resultDirectory);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_JOURNAL_PATH"),resultJournalPath);

    QProcess process;
    const auto processCleanup = qScopeGuard([&process]
    {
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(5000);
        }
    });
    process.setProcessEnvironment(environment);
    process.start(solverExecutablePath(),{
        QStringLiteral("--file=")+input,
        QStringLiteral("--native-checkpoint-dir=")+checkpointDirectory,
        QStringLiteral("--native-checkpoint-context-json=")+QString::fromUtf8(contextBytes),
        QStringLiteral("--pause-at-accepted-seq=10"),
        QStringLiteral("--native-execution-state=")+statePath,
        QStringLiteral("--task-id=")+taskId,
        QStringLiteral("--task-server=")+serverName});
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(),10000);
    QLocalSocket *hostSocket = server.nextPendingConnection();
    QVERIFY(hostSocket);

    QJsonObject pausedState;
    QElapsedTimer pauseTimer;
    pauseTimer.start();
    while (pauseTimer.elapsed() < 15000)
    {
        readJsonObject(statePath,pausedState);
        if (pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"))
            break;
        if (process.state() == QProcess::NotRunning)
            break;
        QTest::qWait(10);
    }
    const QString pauseDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QVERIFY2(pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"),
             qPrintable(pauseDiagnostic));
    QCOMPARE(pausedState.value(QStringLiteral("acceptedSeq")).toDouble(),10.0);
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAcceptedSeq")).toDouble(),10.0);
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),10);

    hostSocket->abort();
    QVERIFY2(process.waitForFinished(15000),qPrintable(process.errorString()));
    const QString stoppedDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0,qPrintable(stoppedDiagnostic));
    QJsonObject cancelledState;
    QVERIFY(readJsonObject(statePath,cancelledState));
    QCOMPARE(cancelledState.value(QStringLiteral("state")).toString(),QStringLiteral("cancelled"));
    QCOMPARE(cancelledState.value(QStringLiteral("generation")).toInt(),2);
    QCOMPARE(cancelledState.value(QStringLiteral("acceptedSeq")).toDouble(),10.0);
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),10);

    RModel stopped;
    stopped.read(input);
    QCOMPARE(stopped.getProblemSetup().getStructuralDynamicsRestartData().completedSteps,10u);
}

void TestSolverAuditCli::nativeExecutionGateRefusesExternalStateReplacement()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const double dtS = 1.0e-5;
    const QString resultDirectory = directory.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    RModel model = makeModel();
    auto &setup = model.getProblemSetup().getStructuralDynamicsSetup();
    setup.setTimeStep(dtS);
    setup.setNSteps(20);
    setup.setOutputFrequency(1);
    const QString input = QDir(resultDirectory).filePath(QStringLiteral("external-state-change.rbm"));
    QCOMPARE(model.write(input,false),input);
    const QString checkpointDirectory = directory.filePath(QStringLiteral("checkpoints"));
    const QString eventsDirectory = directory.filePath(QStringLiteral("events"));
    QVERIFY(QDir().mkpath(checkpointDirectory));
    QVERIFY(QDir().mkpath(eventsDirectory));
    const QString jobId = QStringLiteral("solver-audit-external-state-job-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString taskId = QStringLiteral("solver-audit-external-state-task-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString serverName = QStringLiteral("range-solver-external-state-")+
        QUuid::createUuid().toString(QUuid::Id128);
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    QVERIFY(server.listen(serverName));

    const QString resultJournalPath = QDir(eventsDirectory)
        .filePath(QStringLiteral("native-result-frames.jsonl"));
    const QString statePath = QDir(eventsDirectory)
        .filePath(QStringLiteral("native-execution-state.json"));
    const QByteArray contextBytes = QJsonDocument(
        nativeCheckpointContext(input,dtS)).toJson(QJsonDocument::Compact);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("RANGE_NATIVE_JOB_ID"),jobId);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_DIRECTORY"),resultDirectory);
    environment.insert(QStringLiteral("RANGE_NATIVE_RESULT_JOURNAL_PATH"),resultJournalPath);

    QProcess process;
    const auto processCleanup = qScopeGuard([&process]
    {
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(5000);
        }
    });
    process.setProcessEnvironment(environment);
    process.start(solverExecutablePath(),{
        QStringLiteral("--file=")+input,
        QStringLiteral("--native-checkpoint-dir=")+checkpointDirectory,
        QStringLiteral("--native-checkpoint-context-json=")+QString::fromUtf8(contextBytes),
        QStringLiteral("--pause-at-accepted-seq=15"),
        QStringLiteral("--native-execution-state=")+statePath,
        QStringLiteral("--task-id=")+taskId,
        QStringLiteral("--task-server=")+serverName});
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(),10000);
    QLocalSocket *hostSocket = server.nextPendingConnection();
    QVERIFY(hostSocket);

    QJsonObject pausedState;
    QElapsedTimer pauseTimer;
    pauseTimer.start();
    while (pauseTimer.elapsed() < 15000)
    {
        readJsonObject(statePath,pausedState);
        if (pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"))
            break;
        if (process.state() == QProcess::NotRunning)
            break;
        QTest::qWait(10);
    }
    const QString pauseDiagnostic = QString::fromUtf8(
        process.readAllStandardOutput()+process.readAllStandardError());
    QVERIFY2(pausedState.value(QStringLiteral("state")).toString() == QStringLiteral("paused"),
             qPrintable(pauseDiagnostic));
    QCOMPARE(pausedState.value(QStringLiteral("acceptedSeq")).toDouble(),15.0);
    QCOMPARE(pausedState.value(QStringLiteral("checkpointAcceptedSeq")).toDouble(),15.0);
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),15);

    const auto checkpointsAtPause = nativeCheckpointEnvelopes(checkpointDirectory);
    QCOMPARE(checkpointsAtPause.size(),2);
    QCOMPARE(checkpointsAtPause.front().value(QStringLiteral("acceptedSeq")).toDouble(),10.0);
    QCOMPARE(checkpointsAtPause.back().value(QStringLiteral("acceptedSeq")).toDouble(),15.0);

    const QByteArray externalContents("external state replacement must survive");
    QFile stateFile(statePath);
    QVERIFY(stateFile.open(QIODevice::WriteOnly|QIODevice::Truncate));
    QCOMPARE(stateFile.write(externalContents),qint64(externalContents.size()));
    stateFile.close();
    QVERIFY(sendLocalCommand(hostSocket,taskId,QStringLiteral("RESUME")));
    QVERIFY2(process.waitForFinished(15000),qPrintable(process.errorString()));
    QCOMPARE(process.exitStatus(),QProcess::NormalExit);
    QVERIFY(process.exitCode() != 0);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    QCOMPARE(stateFile.readAll(),externalContents);
    stateFile.close();
    QCOMPARE(nativeResultJournalRecords(resultJournalPath).size(),15);
    QCOMPARE(nativeCheckpointEnvelopes(checkpointDirectory),checkpointsAtPause);
}

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

void TestSolverAuditCli::typedCableCliRestartMatchesUninterrupted()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QVERIFY(QFile::exists(QStringLiteral(RANGE_TYPED_CONTROL_EXAMPLE_PATH)));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QDir root(directory.path());
    for (const QString &name : {QStringLiteral("full"),QStringLiteral("split"),
                                QStringLiteral("resume")})
        QVERIFY(root.mkdir(name));

    const QString fullSeed = root.filePath(QStringLiteral("full/seed.sfec"));
    const QString fullCheckpoint = root.filePath(QStringLiteral("full/out.sfec"));
    const QString splitSeed = root.filePath(QStringLiteral("split/seed.sfec"));
    const QString partialCheckpoint = root.filePath(QStringLiteral("split/first.sfec"));
    const QString finalCheckpoint = root.filePath(QStringLiteral("resume/final.sfec"));
    writeTypedCableSeed(fullSeed);
    writeTypedCableSeed(splitSeed);
    const QString fullSeedHash = sha256(fullSeed);
    const QString splitSeedHash = sha256(splitSeed);
    QVERIFY(!fullSeedHash.isEmpty() && !splitSeedHash.isEmpty());

    QString diagnostic;
    QVERIFY2(runTypedControlCli(fullSeed,fullCheckpoint,QStringLiteral("3"),python,
                                QStringLiteral("normal"),diagnostic),qPrintable(diagnostic));
    QVERIFY2(runTypedControlCli(splitSeed,partialCheckpoint,QStringLiteral("1"),python,
                                QStringLiteral("normal"),diagnostic),qPrintable(diagnostic));
    const QString partialHash = sha256(partialCheckpoint);
    QVERIFY(!partialHash.isEmpty());
    QVERIFY2(runTypedControlCli(partialCheckpoint,finalCheckpoint,QStringLiteral("2"),python,
                                QStringLiteral("normal"),diagnostic),qPrintable(diagnostic));
    QCOMPARE(sha256(fullSeed),fullSeedHash);
    QCOMPARE(sha256(splitSeed),splitSeedHash);
    QCOMPARE(sha256(partialCheckpoint),partialHash);

    auto uninterrupted = RStructuralTypedFEControlDriver::readCheckpoint(
        fullCheckpoint,QStringLiteral("python-typed-v2"));
    auto resumed = RStructuralTypedFEControlDriver::readCheckpoint(
        finalCheckpoint,QStringLiteral("python-typed-v2"));
    const auto &fullState = uninterrupted->state();
    const auto &resumedState = resumed->state();
    QCOMPARE(fullState.completedSteps,quint64(3));
    QCOMPARE(resumedState.completedSteps,quint64(3));
    QVERIFY(closeTypedValue(fullState.acceptedTime,0.3));
    QVERIFY(closeTypedValue(resumedState.acceptedTime,0.3));
    QVERIFY(closeTypedValue(fullState.acceptedTime,resumedState.acceptedTime));
    QCOMPARE(fullState.controllerState,QByteArrayLiteral("3"));
    QCOMPARE(resumedState.controllerState,fullState.controllerState);
    QCOMPARE(resumedState.lastAppliedValues.size(),fullState.lastAppliedValues.size());
    for (std::size_t i=0;i<fullState.lastAppliedValues.size();i++)
        QVERIFY(closeTypedValue(resumedState.lastAppliedValues[i],fullState.lastAppliedValues[i]));
    QCOMPARE(resumedState.cumulativeWorkJ.size(),fullState.cumulativeWorkJ.size());
    for (std::size_t i=0;i<fullState.cumulativeWorkJ.size();i++)
        QVERIFY(closeTypedValue(resumedState.cumulativeWorkJ[i],fullState.cumulativeWorkJ[i]));

    const auto tipDisplacement = [](const RStructuralTypedFEControlDriver &driver)
    {
        const RModel &model = driver.model();
        const uint variable = model.findVariable(R_VARIABLE_DISPLACEMENT);
        return variable == RConstants::eod ? std::numeric_limits<double>::quiet_NaN()
                                           : model.getVariable(variable).getValue(0,1);
    };
    const double fullTip = tipDisplacement(*uninterrupted);
    const double resumedTip = tipDisplacement(*resumed);
    QVERIFY(std::isfinite(fullTip) && std::isfinite(resumedTip));
    QVERIFY(closeTypedValue(fullTip,resumedTip));

    const auto fullJournal = ExecutionEventJournal::read(fullCheckpoint+QStringLiteral(".events.ndjson"));
    const auto firstJournal = ExecutionEventJournal::read(partialCheckpoint+QStringLiteral(".events.ndjson"));
    const auto resumeJournal = ExecutionEventJournal::read(finalCheckpoint+QStringLiteral(".events.ndjson"));
    QVERIFY2(fullJournal.error.isEmpty(),qPrintable(fullJournal.error));
    QVERIFY2(firstJournal.error.isEmpty(),qPrintable(firstJournal.error));
    QVERIFY2(resumeJournal.error.isEmpty(),qPrintable(resumeJournal.error));
    QVERIFY(!fullJournal.events.empty() && !firstJournal.events.empty() &&
            !resumeJournal.events.empty());
    QCOMPARE(fullJournal.events.back().type,QStringLiteral("RunFinished"));
    QCOMPARE(firstJournal.events.back().type,QStringLiteral("RunFinished"));
    QCOMPARE(resumeJournal.events.back().type,QStringLiteral("RunFinished"));
    const auto fullSteps = typedPreparedSteps(fullJournal);
    auto splitSteps = typedPreparedSteps(firstJournal);
    splitSteps += typedPreparedSteps(resumeJournal);
    QCOMPARE(fullSteps.size(),3);
    QCOMPARE(splitSteps.size(),3);
    for (int step=0;step<3;step++)
    {
        QCOMPARE(fullSteps[step].value(QStringLiteral("stepId")).toString(),QString::number(step+1));
        QCOMPARE(splitSteps[step].value(QStringLiteral("stepId")),
                 fullSteps[step].value(QStringLiteral("stepId")));
        QVERIFY(closeTypedValue(splitSteps[step].value(QStringLiteral("acceptedTime")).toDouble(),
                                fullSteps[step].value(QStringLiteral("acceptedTime")).toDouble()));
        const QJsonArray fullChannels = fullSteps[step].value(QStringLiteral("channels")).toArray();
        const QJsonArray splitChannels = splitSteps[step].value(QStringLiteral("channels")).toArray();
        QCOMPARE(fullChannels.size(),3);
        QCOMPARE(splitChannels.size(),3);
        for (int channel=0;channel<3;channel++)
        {
            const QJsonObject expected = fullChannels[channel].toObject();
            const QJsonObject actual = splitChannels[channel].toObject();
            QCOMPARE(actual.value(QStringLiteral("id")),expected.value(QStringLiteral("id")));
            QCOMPARE(actual.value(QStringLiteral("quantity")),expected.value(QStringLiteral("quantity")));
            QCOMPARE(actual.value(QStringLiteral("unit")),expected.value(QStringLiteral("unit")));
            for (const QString &field : {QStringLiteral("requested"),QStringLiteral("conditioned"),
                                         QStringLiteral("applied"),QStringLiteral("cumulativeWorkJ"),
                                         QStringLiteral("averagePowerW")})
                QVERIFY(closeTypedValue(actual.value(field).toDouble(),expected.value(field).toDouble()));
        }
    }
    for (const auto *journal : {&fullJournal,&firstJournal,&resumeJournal})
        QCOMPARE(int(std::count_if(journal->events.begin(),journal->events.end(),[](const auto &event) {
            return event.type == QStringLiteral("TypedCheckpointCommitted");
        })),1);
}

void TestSolverAuditCli::typedCableCliInvalidUnitPreservesCheckpoint()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString seed = directory.filePath(QStringLiteral("seed.sfec"));
    const QString checkpoint = directory.filePath(QStringLiteral("preserve.sfec"));
    writeTypedCableSeed(seed);
    QVERIFY(QFile::copy(seed,checkpoint));
    const QString seedHash = sha256(seed);
    const QString checkpointHash = sha256(checkpoint);
    QString diagnostic;
    QVERIFY(!runTypedControlCli(seed,checkpoint,QStringLiteral("1"),python,
                                QStringLiteral("invalid-unit"),diagnostic));
    QVERIFY(!diagnostic.isEmpty());
    QCOMPARE(sha256(seed),seedHash);
    QCOMPARE(sha256(checkpoint),checkpointHash);
    const auto journal = ExecutionEventJournal::read(checkpoint+QStringLiteral(".events.ndjson"));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QVERIFY(!journal.events.empty());
    QCOMPARE(journal.events.back().type,QStringLiteral("RunFinished"));
    QVERIFY(std::none_of(journal.events.begin(),journal.events.end(),[](const auto &event) {
        return event.type == QStringLiteral("TypedCheckpointCommitted");
    }));
}

void TestSolverAuditCli::typedCableCliRejectsLockedCheckpoint()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString seed = directory.filePath(QStringLiteral("seed.sfec"));
    const QString checkpoint = directory.filePath(QStringLiteral("locked.sfec"));
    writeTypedCableSeed(seed);
    const QString seedHash = sha256(seed);
    QLockFile heldLock(QFileInfo(checkpoint).absoluteFilePath()+QStringLiteral(".lock"));
    heldLock.setStaleLockTime(0);
    QVERIFY(heldLock.tryLock(0));
    QString diagnostic;
    QVERIFY(!runTypedControlCli(seed,checkpoint,QStringLiteral("1"),python,
                                QStringLiteral("normal"),diagnostic));
    QVERIFY(!diagnostic.isEmpty());
    QCOMPARE(sha256(seed),seedHash);
    QVERIFY(!QFile::exists(checkpoint));
    QVERIFY(!QFile::exists(checkpoint+QStringLiteral(".events.ndjson")));
}

void TestSolverAuditCli::typedCableCliRejectsOversizedStepCount()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString seed = directory.filePath(QStringLiteral("seed.sfec"));
    const QString checkpoint = directory.filePath(QStringLiteral("unused.sfec"));
    writeTypedCableSeed(seed);
    const QString seedHash = sha256(seed);
    QString diagnostic;
    QVERIFY(!runTypedControlCli(seed,checkpoint,QStringLiteral("1001"),python,
                                QStringLiteral("normal"),diagnostic));
    QVERIFY(!diagnostic.isEmpty());
    QCOMPARE(sha256(seed),seedHash);
    QVERIFY(!QFile::exists(checkpoint));
    QVERIFY(!QFile::exists(checkpoint+QStringLiteral(".events.ndjson")));
}

void TestSolverAuditCli::typedCableBootstrapUsesNativeModelAndPythonController()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QVERIFY(QFile::exists(QStringLiteral(RANGE_TYPED_CONTROL_EXAMPLE_PATH)));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString native = directory.filePath(QStringLiteral("cable.rbm"));
    const QString configuration = directory.filePath(QStringLiteral("cable.json"));
    const QString checkpoint = directory.filePath(QStringLiteral("accepted.sfec"));
    RModel model = makeTypedCableNativeModel();
    model.write(native,false);
    QVERIFY(writeJsonObject(configuration,typedCableBootstrapManifest()));
    const QString inputHash = sha256(native);
    const QString configurationHash = sha256(configuration);
    const QString commands = QStringLiteral("{\"length\":1.98,\"tension\":8.0,\"anchor\":0.05}");

    QString diagnostic;
    QVERIFY2(runTypedBootstrapCli(native,configuration,checkpoint,QStringLiteral("3"),python,
                                  QStringLiteral("normal"),commands,diagnostic),qPrintable(diagnostic));
    QCOMPARE(sha256(native),inputHash);
    QCOMPARE(sha256(configuration),configurationHash);
    auto accepted = RStructuralTypedFEControlDriver::readCheckpoint(
        checkpoint,QStringLiteral("python-typed-v2"));
    QCOMPARE(accepted->state().completedSteps,quint64(3));
    QVERIFY(closeTypedValue(accepted->state().acceptedTime,0.3));
    QCOMPARE(accepted->state().controllerState,QByteArrayLiteral("3"));
    const uint displacement = accepted->model().findVariable(R_VARIABLE_DISPLACEMENT);
    QVERIFY(displacement != RConstants::eod);
    const double tipDisplacement = accepted->model().getVariable(displacement).getValue(0,1);
    QVERIFY(std::abs(tipDisplacement-0.03792)<1.0e-10);

    const auto journal = ExecutionEventJournal::read(checkpoint+QStringLiteral(".events.ndjson"));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    QCOMPARE(typedPreparedSteps(journal).size(),3);
    QVERIFY(std::any_of(journal.events.begin(),journal.events.end(),[](const auto &event) {
        return event.type == QStringLiteral("TypedCheckpointCommitted");
    }));
}

void TestSolverAuditCli::typedCableBootstrapRejectsMalformedManifest_data()
{
    QTest::addColumn<QString>("fault");
    QTest::newRow("fractional member index") << QStringLiteral("fractional-element");
    QTest::newRow("missing initial command") << QStringLiteral("missing-initial");
    QTest::newRow("misspelled safety limit") << QStringLiteral("misspelled-limit");
}

void TestSolverAuditCli::typedCableBootstrapRejectsMalformedManifest()
{
    QFETCH(QString,fault);
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString native = directory.filePath(QStringLiteral("cable.rbm"));
    const QString configuration = directory.filePath(QStringLiteral("invalid.json"));
    const QString seed = directory.filePath(QStringLiteral("seed.sfec"));
    const QString checkpoint = directory.filePath(QStringLiteral("preserve.sfec"));
    makeTypedCableNativeModel().write(native,false);
    QVERIFY(writeJsonObject(configuration,typedCableBootstrapManifest(fault)));
    writeTypedCableSeed(seed);
    QVERIFY(QFile::copy(seed,checkpoint));
    const QString nativeHash = sha256(native);
    const QString checkpointHash = sha256(checkpoint);

    QString diagnostic;
    const QString commands = QStringLiteral("{\"length\":1.98,\"tension\":8.0,\"anchor\":0.05}");
    QVERIFY(!runTypedBootstrapCli(native,configuration,checkpoint,QStringLiteral("1"),python,
                                  QStringLiteral("normal"),commands,diagnostic));
    QVERIFY(!diagnostic.isEmpty());
    QCOMPARE(sha256(native),nativeHash);
    QCOMPARE(sha256(checkpoint),checkpointHash);
}

void TestSolverAuditCli::typedPiezoBootstrapUsesNativeBeamAndPythonController()
{
    QVERIFY(QFile::exists(solverExecutablePath()));
    const QString python = typedControlPython();
    if (python.isEmpty()) QSKIP("Python is not available on this test host");
    QVERIFY(QFile::exists(QStringLiteral(RANGE_TYPED_CONTROL_EXAMPLE_PATH)));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString native = directory.filePath(QStringLiteral("beam.rbm"));
    const QString configuration = directory.filePath(QStringLiteral("beam.json"));
    const QString checkpoint = directory.filePath(QStringLiteral("accepted.sfec"));
    RModel model = makeTypedPiezoBeamModel();
    model.write(native,false);
    QVERIFY(writeJsonObject(configuration,typedPiezoBootstrapManifest()));
    const QString inputHash = sha256(native);

    QString diagnostic;
    QVERIFY2(runTypedBootstrapCli(native,configuration,checkpoint,QStringLiteral("1"),python,
                                  QStringLiteral("normal"),QStringLiteral("{\"patch\":100.0}"),
                                  diagnostic),qPrintable(diagnostic));
    QCOMPARE(sha256(native),inputHash);
    auto accepted = RStructuralTypedFEControlDriver::readCheckpoint(
        checkpoint,QStringLiteral("python-typed-v2"));
    QCOMPARE(accepted->state().completedSteps,quint64(1));
    QCOMPARE(accepted->state().controllerState,QByteArrayLiteral("1"));

    const double axialStiffness = typedBeamYoungModulus*typedBeamArea+
                                  typedPatchYoungModulus*typedPatchArea;
    const double bendingStiffness = typedBeamYoungModulus*typedBeamIz+
        typedPatchYoungModulus*typedPatchArea*typedPatchOffset*typedPatchOffset*
            typedBeamYoungModulus*typedBeamArea/axialStiffness;
    const double piezoMoment = typedPiezoCoupling*typedPatchArea*typedPatchOffset*
        typedVoltage/typedPatchThickness*typedBeamYoungModulus*typedBeamArea/axialStiffness;
    const double curvature = piezoMoment/bendingStiffness;
    const double axialStrain =
        (typedPatchYoungModulus*typedPatchArea*typedPatchOffset*curvature-
         typedPiezoCoupling*typedPatchArea*typedVoltage/typedPatchThickness)/axialStiffness;
    const double patchStrain = axialStrain-typedPatchOffset*curvature;
    const double expectedCharge =
        (typedPermittivity*typedPatchArea*typedVoltage/
             (typedPatchThickness*typedPatchThickness)-
         typedPiezoCoupling*typedPatchArea*patchStrain/typedPatchThickness)*typedBeamLength;
    const double expectedTipY = 0.5*curvature*typedBeamLength*typedBeamLength;
    const uint displacement = accepted->model().findVariable(R_VARIABLE_DISPLACEMENT);
    const uint charge = accepted->model().findVariable(R_VARIABLE_ELECTRIC_CHARGE);
    QVERIFY(displacement != RConstants::eod && charge != RConstants::eod);
    const double tipY = accepted->model().getVariable(displacement).getValue(1,1);
    const double terminalCharge = accepted->model().getVariable(charge).getValue(0,0);
    QVERIFY(std::abs(tipY)>1.0e-8);
    QVERIFY(std::abs(tipY-expectedTipY)<1.0e-8*(1.0+std::abs(expectedTipY)));
    QVERIFY(std::abs(terminalCharge)>1.0e-8);
    QVERIFY(std::abs(terminalCharge-expectedCharge)<1.0e-8*(1.0+std::abs(expectedCharge)));
    const double expectedWork = 0.5*typedVoltage*expectedCharge;
    QCOMPARE(accepted->state().cumulativeWorkJ.size(),std::size_t(1));
    QVERIFY(std::abs(accepted->state().cumulativeWorkJ[0]-expectedWork)<
            1.0e-8*(1.0+std::abs(expectedWork)));
    const auto journal = ExecutionEventJournal::read(checkpoint+QStringLiteral(".events.ndjson"));
    QVERIFY2(journal.error.isEmpty(),qPrintable(journal.error));
    const auto steps = typedPreparedSteps(journal);
    QCOMPARE(steps.size(),1);
    const QJsonArray channels = steps[0].value(QStringLiteral("channels")).toArray();
    QCOMPARE(channels.size(),1);
    QVERIFY(std::abs(channels[0].toObject().value(QStringLiteral("cumulativeWorkJ")).toDouble()-expectedWork)<
            1.0e-8*(1.0+std::abs(expectedWork)));
}


int main(int argc,char *argv[])
{
    QCoreApplication app(argc,argv);
    TestSolverAuditCli test;
    return QTest::qExec(&test,argc,argv);
}
#include "tst_solver_audit_cli.moc"
