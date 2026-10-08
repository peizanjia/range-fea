// Creates independent raw native MODEL inputs for WF-03 describe preflight.
#include <QCoreApplication>
#include <QFileInfo>

#include <stdexcept>

#include <rml_boundary_condition.h>
#include <rml_initial_condition.h>
#include <rml_line.h>
#include <rml_material.h>
#include <rml_model.h>
#include <rml_problem_task_item.h>
#include <rml_structural_interaction.h>
#include <rml_variable.h>

namespace
{
void setComponent(RCondition &condition, RVariableType type, bool enabled, double value = 0.0)
{
    const uint position = condition.findComponentPosition(type);
    if (position >= condition.size()) throw std::runtime_error("Fixture condition component is missing.");
    auto &component = condition.getComponent(position);
    component.setEnabled(enabled);
    component.clear();
    if (enabled) component.add(0.0, value);
}

RMaterial material(double young, double density, double poisson)
{
    RMaterial result(RMaterial::Solid);
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15, young);
    result.add(modulus);
    RMaterialProperty massDensity(RMaterialProperty::Density);
    massDensity.add(293.15, density);
    result.add(massDensity);
    RMaterialProperty poissonRatio(RMaterialProperty::PoissonRatio);
    poissonRatio.add(293.15, poisson);
    result.add(poissonRatio);
    RMaterialProperty expansion(RMaterialProperty::ThermalExpansionCoefficient);
    expansion.add(293.15, 0.0);
    result.add(expansion);
    return result;
}

void enableDynamics(RModel &model)
{
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    auto &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setTimeStep(0.0125);
    dynamics.setNSteps(120);
    dynamics.setOutputFrequency(12);
    model.getProblemSetup().setMassMatrixType(RMassMatrixType::Consistent);
}

RModel makeRod(bool rotated, bool nonzeroSupport, bool force, bool freeAxisGravity)
{
    RModel model;
    model.setNNodes(5);
    const double x[5] = {0.0, 0.4, 1.15, 2.35, 4.7};
    const uint nodeIds[5] = {4001, 87, 625, 1009, 51000};
    for (uint i = 0; i < 5; ++i)
    {
        model.getNode(i).set(rotated ? 0.0 : x[i], rotated ? x[i] : 2.5, -0.75);
        model.setNodePhysicalId(i, nodeIds[i]);
    }
    for (uint i = 0; i < 4; ++i)
    {
        RElement truss(R_ELEMENT_TRUSS1);
        truss.setNodeId(0, i);
        truss.setNodeId(1, i + 1);
        model.addElement(truss, true, 0);
        model.setElementPhysicalId(i, 8101 + 53 * i);
    }
    RLine &line = model.getLine(0);
    line.setMaterial(material(3.2e7, 840.0, 0.29));
    line.setCrossArea(0.0065);
    if (freeAxisGravity)
    {
        auto &gravity = line.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        setComponent(gravity, R_VARIABLE_G_ACCELERATION_X, true, 1.0);
    }
    RBoundaryCondition transverse(R_BOUNDARY_CONDITION_DISPLACEMENT);
    setComponent(transverse, R_VARIABLE_DISPLACEMENT_X, false);
    setComponent(transverse, R_VARIABLE_DISPLACEMENT_Y, true, 0.0);
    setComponent(transverse, R_VARIABLE_DISPLACEMENT_Z, true, 0.0);
    line.addBoundaryCondition(transverse);

    const uint pointNodes[6] = {0, 0, 1, 2, 3, 4};
    for (uint i = 0; i < 6; ++i)
    {
        RElement point(R_ELEMENT_POINT);
        point.setNodeId(0, pointNodes[i]);
        model.addElement(point, true, i);
        model.setElementPhysicalId(4 + i, 9101 + 47 * i);
    }
    RBoundaryCondition root(R_BOUNDARY_CONDITION_DISPLACEMENT);
    setComponent(root, R_VARIABLE_DISPLACEMENT_X, true, nonzeroSupport ? 0.002 : 0.0);
    setComponent(root, R_VARIABLE_DISPLACEMENT_Y, true, 0.0);
    setComponent(root, R_VARIABLE_DISPLACEMENT_Z, true, 0.0);
    model.getPoint(0).addBoundaryCondition(root);

    RInitialCondition displacement(R_INITIAL_CONDITION_DISPLACEMENT);
    setComponent(displacement, R_VARIABLE_DISPLACEMENT_X, true, 0.0017);
    setComponent(displacement, R_VARIABLE_DISPLACEMENT_Y, false);
    setComponent(displacement, R_VARIABLE_DISPLACEMENT_Z, false);
    model.getPoint(2).addInitialCondition(displacement);
    RInitialCondition velocity(R_INITIAL_CONDITION_VELOCITY);
    setComponent(velocity, R_VARIABLE_VELOCITY_X, true, -0.023);
    setComponent(velocity, R_VARIABLE_VELOCITY_Y, false);
    setComponent(velocity, R_VARIABLE_VELOCITY_Z, false);
    model.getPoint(3).addInitialCondition(velocity);

    if (force)
    {
        RBoundaryCondition load(R_BOUNDARY_CONDITION_FORCE);
        setComponent(load, R_VARIABLE_FORCE_X, true, 17.0);
        setComponent(load, R_VARIABLE_FORCE_Y, false);
        setComponent(load, R_VARIABLE_FORCE_Z, false);
        model.getPoint(5).addBoundaryCondition(load);
    }
    enableDynamics(model);
    return model;
}

RModel makeBeam(bool badSupport, bool freeAxisGravity = false)
{
    RModel model;
    model.setNNodes(2);
    model.getNode(0).set(0.0, 1.25, -0.5);
    model.getNode(1).set(3.3, 1.25, -0.5);
    model.setNodePhysicalId(0, 4093);
    model.setNodePhysicalId(1, 117);
    RElement beam(R_ELEMENT_BEAM1);
    beam.setNodeId(0, 0);
    beam.setNodeId(1, 1);
    model.addElement(beam, true, 0);
    model.setElementPhysicalId(0, 7007);
    RLine &line = model.getLine(0);
    line.setMaterial(material(5.1e9, 3210.0, 0.23));
    line.setCrossArea(0.007);
    line.setSecondMomentY(0.0011);
    line.setSecondMomentZ(0.0015);
    line.setTorsionalConstant(0.0009);
    line.setLocalYDirection(RR3Vector(0.0, 1.0, 0.0));
    if (freeAxisGravity)
    {
        auto &gravity = line.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
        setComponent(gravity, R_VARIABLE_G_ACCELERATION_Y, true, 1.0);
    }

    for (uint i = 0; i < 2; ++i)
    {
        RElement point(R_ELEMENT_POINT);
        point.setNodeId(0, i);
        model.addElement(point, true, i);
        model.setElementPhysicalId(1 + i, 7011 + 2 * i);
    }

    RBoundaryCondition root(R_BOUNDARY_CONDITION_DISPLACEMENT);
    setComponent(root, R_VARIABLE_DISPLACEMENT_X, true, 0.0);
    setComponent(root, R_VARIABLE_DISPLACEMENT_Y, true, 0.0);
    setComponent(root, R_VARIABLE_DISPLACEMENT_Z, true, 0.0);
    model.getPoint(0).addBoundaryCondition(root);
    RBoundaryCondition rootRotation(R_BOUNDARY_CONDITION_ROTATION);
    setComponent(rootRotation, R_VARIABLE_ROTATION_X, true, 0.0);
    setComponent(rootRotation, R_VARIABLE_ROTATION_Y, true, 0.0);
    setComponent(rootRotation, R_VARIABLE_ROTATION_Z, true, 0.0);
    model.getPoint(0).addBoundaryCondition(rootRotation);

    RBoundaryCondition tipTranslation(R_BOUNDARY_CONDITION_DISPLACEMENT);
    setComponent(tipTranslation, R_VARIABLE_DISPLACEMENT_X, true, 0.0);
    setComponent(tipTranslation, R_VARIABLE_DISPLACEMENT_Y, false);
    setComponent(tipTranslation, R_VARIABLE_DISPLACEMENT_Z, true, 0.0);
    model.getPoint(1).addBoundaryCondition(tipTranslation);
    RBoundaryCondition tipRotation(R_BOUNDARY_CONDITION_ROTATION);
    setComponent(tipRotation, R_VARIABLE_ROTATION_X, true, 0.0);
    setComponent(tipRotation, R_VARIABLE_ROTATION_Y, true, 0.0);
    setComponent(tipRotation, R_VARIABLE_ROTATION_Z, false);
    model.getPoint(1).addBoundaryCondition(tipRotation);
    if (badSupport)
    {
        RBoundaryCondition bad(R_BOUNDARY_CONDITION_DISPLACEMENT);
        setComponent(bad, R_VARIABLE_DISPLACEMENT_Y, true, 0.0);
        setComponent(bad, R_VARIABLE_DISPLACEMENT_X, false);
        setComponent(bad, R_VARIABLE_DISPLACEMENT_Z, false);
        model.getPoint(1).addBoundaryCondition(bad);
    }

    RInitialCondition displacement(R_INITIAL_CONDITION_DISPLACEMENT);
    setComponent(displacement, R_VARIABLE_DISPLACEMENT_X, false);
    setComponent(displacement, R_VARIABLE_DISPLACEMENT_Y, true, 0.0042);
    setComponent(displacement, R_VARIABLE_DISPLACEMENT_Z, false);
    model.getPoint(1).addInitialCondition(displacement);
    RInitialCondition velocity(R_INITIAL_CONDITION_VELOCITY);
    setComponent(velocity, R_VARIABLE_VELOCITY_X, false);
    setComponent(velocity, R_VARIABLE_VELOCITY_Y, true, -0.031);
    setComponent(velocity, R_VARIABLE_VELOCITY_Z, false);
    model.getPoint(1).addInitialCondition(velocity);
    RInitialCondition rotation(R_INITIAL_CONDITION_ROTATION);
    setComponent(rotation, R_VARIABLE_ROTATION_X, false);
    setComponent(rotation, R_VARIABLE_ROTATION_Y, false);
    setComponent(rotation, R_VARIABLE_ROTATION_Z, true, 0.014);
    model.getPoint(1).addInitialCondition(rotation);
    RInitialCondition angularVelocity(R_INITIAL_CONDITION_ANGULAR_VELOCITY);
    setComponent(angularVelocity, R_VARIABLE_ANGULAR_VELOCITY_X, false);
    setComponent(angularVelocity, R_VARIABLE_ANGULAR_VELOCITY_Y, false);
    setComponent(angularVelocity, R_VARIABLE_ANGULAR_VELOCITY_Z, true, 0.63);
    model.getPoint(1).addInitialCondition(angularVelocity);

    enableDynamics(model);
    return model;
}

void addSourceControlMapping(RModel &model, uint tipPosition)
{
    auto &interaction = model.getProblemSetup().getStructuralInteractionSetup();
    RStructuralActuator legacyActuator;
    legacyActuator.id = QStringLiteral("legacy-force");
    legacyActuator.name = legacyActuator.id;
    legacyActuator.target.nodeId = tipPosition;
    legacyActuator.variable = R_VARIABLE_FORCE_X;
    legacyActuator.direction = RR3Vector(1.0, 0.0, 0.0);
    legacyActuator.unit = QStringLiteral("N");
    interaction.getActuators().push_back(legacyActuator);
    RStructuralSensor legacySensor;
    legacySensor.id = QStringLiteral("legacy-displacement");
    legacySensor.name = legacySensor.id;
    legacySensor.target.nodeId = tipPosition;
    legacySensor.quantity = RStructuralSensorQuantity::Displacement;
    legacySensor.direction = RR3Vector(1.0, 0.0, 0.0);
    legacySensor.unit = QStringLiteral("m");
    interaction.getSensors().push_back(legacySensor);

    RStructuralControlSetup &control = model.getProblemSetup().getStructuralControlSetup();
    control.enabled = true;
    control.controlProtocolVersion = 1;
    control.controllerId = QStringLiteral("source-controller");
    control.deterministicSeed = 41;
    RStructuralControlChannelConditioning oldConditioning;
    oldConditioning.actuatorId = legacyActuator.id;
    oldConditioning.delaySteps = 2;
    oldConditioning.holdSteps = 3;
    oldConditioning.noiseAmplitude = 0.1;
    control.channelConditioning.push_back(oldConditioning);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList args = application.arguments();
    if (args.size() != 3) return 2;
    const QString kind = args.at(1);
    const QString output = args.at(2);
    if (QFileInfo::exists(output)) return 2;
    RModel model;
    if (kind == QStringLiteral("rod")) model = makeRod(false, false, false, false);
    else if (kind == QStringLiteral("rod-rotated")) model = makeRod(true, false, false, false);
    else if (kind == QStringLiteral("rod-support-conflict")) model = makeRod(false, true, false, false);
    else if (kind == QStringLiteral("rod-force")) model = makeRod(false, false, true, false);
    else if (kind == QStringLiteral("rod-gravity-x")) model = makeRod(false, false, false, true);
    else if (kind == QStringLiteral("rod-controlled"))
    {
        model = makeRod(false, false, false, false);
        addSourceControlMapping(model, 4);
    }
    else if (kind == QStringLiteral("beam")) model = makeBeam(false);
    else if (kind == QStringLiteral("beam-support-conflict")) model = makeBeam(true);
    else if (kind == QStringLiteral("beam-gravity-y")) model = makeBeam(false, true);
    else return 2;
    try
    {
        model.write(output, false);
    }
    catch (const RError &)
    {
        return 1;
    }
    return 0;
}
