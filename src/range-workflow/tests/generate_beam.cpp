// Reproduce the committed Stage 10 workflow benchmark using the native writer.
// Build explicitly: cmake --build <build> --target range-workflow-fixture.
#include <QCoreApplication>
#include <QFileInfo>
#include <rml_initial_condition.h>
#include "stage9_test_models.h"

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) return 2;
    const QString output = application.arguments().at(1);
    if (QFileInfo::exists(output)) return 2;
    using namespace Stage9TestModels;
    RModel model = makeFrameCantilever();
    model.setName("Stage 10 planar cantilever control benchmark");
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15, 2.0e7);
    model.getLine(0).getMaterial().add(modulus);
    model.getPoint(1).addBoundaryCondition(displacement({{true,false,true}}, {{0.,0.,0.}}));
    RBoundaryCondition rotation(R_BOUNDARY_CONDITION_ROTATION);
    setComponent(rotation, R_VARIABLE_ROTATION_X, true);
    setComponent(rotation, R_VARIABLE_ROTATION_Y, true);
    setComponent(rotation, R_VARIABLE_ROTATION_Z, false);
    model.getPoint(1).addBoundaryCondition(rotation);
    RInitialCondition initial(R_INITIAL_CONDITION_DISPLACEMENT);
    setComponent(initial, R_VARIABLE_DISPLACEMENT_X, false);
    setComponent(initial, R_VARIABLE_DISPLACEMENT_Y, true, 0.001);
    setComponent(initial, R_VARIABLE_DISPLACEMENT_Z, false);
    model.getPoint(1).addInitialCondition(initial);
    auto &interaction = model.getProblemSetup().getStructuralInteractionSetup();
    interaction.getActuators()[0].id = "tip-force-y";
    interaction.getActuators()[0].variable = R_VARIABLE_FORCE_Y;
    interaction.getActuators()[0].direction = RR3Vector(0.,1.,0.);
    const QString ids[] = {"tip-displacement-y", "tip-velocity-y", "tip-acceleration-y"};
    for (uint i = 0; i < 3; ++i)
    {
        interaction.getSensors()[i].id = ids[i];
        interaction.getSensors()[i].direction = RR3Vector(0.,1.,0.);
    }
    auto &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setTimeStep(0.005);
    dynamics.setNSteps(400);
    dynamics.setOutputFrequency(400);
    auto &control = model.getProblemSetup().getStructuralControlSetup();
    control.enabled = true;
    control.controllerId = "stage10-beam-velocity-feedback";
    control.deterministicSeed = 10;
    try { model.write(output, false); }
    catch (const RError &) { return 1; }
    return 0;
}
