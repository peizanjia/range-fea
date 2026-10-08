#include "import_model.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include <rbl_error.h>
#include <rml_boundary_condition.h>
#include <rml_element_descriptor.h>
#include <rml_file.h>
#include <rml_file_header.h>
#include <rml_file_io.h>
#include <rml_initial_condition.h>
#include <rml_model.h>
#include <rml_problem_task_item.h>
#include <rml_structural_control_setup.h>
#include <rml_structural_dynamics_restart_data.h>
#include <rml_structural_interaction.h>

namespace RangeNativeImport
{
namespace
{
constexpr double kGeometryTolerance = 1.0e-9;
constexpr double kSourceForceMinimum = -1.0e6;
constexpr double kSourceForceMaximum = 1.0e6;

struct Diagnostic
{
    QString code;
    QString message;
    QString path;
};

class Diagnostics
{
public:
    void add(const QString &code, const QString &path, const QString &message)
    {
        this->items.push_back({code, message, path});
    }

    bool empty() const { return this->items.empty(); }

    QJsonArray toJson() const
    {
        QJsonArray result;
        for (const auto &item : this->items)
        {
            result.append(QJsonObject{{"code", item.code}, {"severity", "error"},
                                      {"message", item.message}, {"path", item.path}});
        }
        return result;
    }

private:
    std::vector<Diagnostic> items;
};

QByteArray fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        throw std::runtime_error("Cannot read the source model for SHA-256.");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
    {
        throw std::runtime_error("Cannot hash the source model.");
    }
    return hash.result().toHex();
}

QJsonObject envelope(const QString &status,
                     const QString &sourceSha,
                     const QJsonArray &diagnostics)
{
    return {{"apiVersion", "range.native-import/v1"}, {"status", status},
            {"sourceSha256", sourceSha}, {"diagnostics", diagnostics}};
}

QJsonArray vectorJson(const std::array<double, 3> &values)
{
    return {values[0], values[1], values[2]};
}

QJsonValue jsonPhysicalId(uint value)
{
    return QJsonValue(qint64(value));
}

QString dofName(RDofType dof)
{
    switch (dof)
    {
        case RDofType::UX: return QStringLiteral("UX");
        case RDofType::UY: return QStringLiteral("UY");
        case RDofType::UZ: return QStringLiteral("UZ");
        case RDofType::RX: return QStringLiteral("RX");
        case RDofType::RY: return QStringLiteral("RY");
        case RDofType::RZ: return QStringLiteral("RZ");
        default: return QStringLiteral("UNKNOWN");
    }
}

QString dofUnit(RDofType dof)
{
    return dof == RDofType::RX || dof == RDofType::RY || dof == RDofType::RZ
        ? QStringLiteral("rad") : QStringLiteral("m");
}

QJsonArray dofDirection(RDofType dof)
{
    int component = 0;
    switch (dof)
    {
        case RDofType::UX:
        case RDofType::RX: component = 0; break;
        case RDofType::UY:
        case RDofType::RY: component = 1; break;
        case RDofType::UZ:
        case RDofType::RZ: component = 2; break;
        default: return {};
    }
    QJsonArray result{0.0, 0.0, 0.0};
    result[component] = 1.0;
    return result;
}

std::optional<RDofType> displacementDof(RVariableType type)
{
    switch (type)
    {
        case R_VARIABLE_DISPLACEMENT_X: return RDofType::UX;
        case R_VARIABLE_DISPLACEMENT_Y: return RDofType::UY;
        case R_VARIABLE_DISPLACEMENT_Z: return RDofType::UZ;
        case R_VARIABLE_ROTATION_X: return RDofType::RX;
        case R_VARIABLE_ROTATION_Y: return RDofType::RY;
        case R_VARIABLE_ROTATION_Z: return RDofType::RZ;
        default: return std::nullopt;
    }
}

std::optional<int> axisIndex(RVariableType type, RVariableType x, RVariableType y, RVariableType z);

std::optional<RDofType> initialDof(RInitialConditionType conditionType, RVariableType variableType)
{
    if (conditionType == R_INITIAL_CONDITION_DISPLACEMENT)
    {
        const auto axis = axisIndex(variableType, R_VARIABLE_DISPLACEMENT_X,
                                    R_VARIABLE_DISPLACEMENT_Y, R_VARIABLE_DISPLACEMENT_Z);
        if (axis) return RDofType(int(RDofType::UX) + *axis);
    }
    else if (conditionType == R_INITIAL_CONDITION_VELOCITY)
    {
        const auto axis = axisIndex(variableType, R_VARIABLE_VELOCITY_X,
                                    R_VARIABLE_VELOCITY_Y, R_VARIABLE_VELOCITY_Z);
        if (axis) return RDofType(int(RDofType::UX) + *axis);
    }
    else if (conditionType == R_INITIAL_CONDITION_ROTATION)
    {
        const auto axis = axisIndex(variableType, R_VARIABLE_ROTATION_X,
                                    R_VARIABLE_ROTATION_Y, R_VARIABLE_ROTATION_Z);
        if (axis) return RDofType(int(RDofType::RX) + *axis);
    }
    else if (conditionType == R_INITIAL_CONDITION_ANGULAR_VELOCITY)
    {
        const auto axis = axisIndex(variableType, R_VARIABLE_ANGULAR_VELOCITY_X,
                                    R_VARIABLE_ANGULAR_VELOCITY_Y, R_VARIABLE_ANGULAR_VELOCITY_Z);
        if (axis) return RDofType(int(RDofType::RX) + *axis);
    }
    return std::nullopt;
}

enum class InitialField
{
    Displacement,
    Velocity,
    Rotation,
    AngularVelocity
};

InitialField initialField(RInitialConditionType type)
{
    switch (type)
    {
        case R_INITIAL_CONDITION_DISPLACEMENT: return InitialField::Displacement;
        case R_INITIAL_CONDITION_VELOCITY: return InitialField::Velocity;
        case R_INITIAL_CONDITION_ROTATION: return InitialField::Rotation;
        case R_INITIAL_CONDITION_ANGULAR_VELOCITY: return InitialField::AngularVelocity;
        default: return InitialField::Displacement;
    }
}

using InitialDofValues = std::map<uint, std::map<std::pair<InitialField, RDofType>, double>>;

std::optional<int> axisIndex(RVariableType type, RVariableType x, RVariableType y, RVariableType z)
{
    if (type == x) return 0;
    if (type == y) return 1;
    if (type == z) return 2;
    return std::nullopt;
}

bool near(double left, double right, double scale = 1.0)
{
    return std::abs(left - right) <= kGeometryTolerance * std::max({1.0, scale, std::abs(left), std::abs(right)});
}

bool readConstantAtZero(const RConditionComponent &component,
                        const QString &path,
                        Diagnostics &diagnostics,
                        double &value)
{
    if (component.size() != 1)
    {
        diagnostics.add("UNSUPPORTED_TIME_DEPENDENT_CONDITION", path,
                        "WF-03 accepts only one constant condition value at time zero.");
        return false;
    }
    if (component.getKey(0) != 0.0)
    {
        diagnostics.add("UNSUPPORTED_CONDITION_TIME", path,
                        "The condition must be declared at global time zero.");
        return false;
    }
    value = component.getValue(0);
    if (!std::isfinite(value))
    {
        diagnostics.add("NONFINITE_CONDITION_VALUE", path,
                        "Condition values must be finite.");
        return false;
    }
    return true;
}

std::vector<uint> groupNodePositions(const RModel &model,
                                    const RElementGroup &group,
                                    const QString &path,
                                    Diagnostics &diagnostics)
{
    std::set<uint> unique;
    for (uint i = 0; i < group.size(); ++i)
    {
        const uint elementId = group.get(i);
        if (elementId >= model.getNElements())
        {
            diagnostics.add("INVALID_GROUP_ELEMENT_REFERENCE", path,
                            "An entity group references an element outside the model.");
            continue;
        }
        const RElement &element = model.getElement(elementId);
        for (unsigned int nodeIndex = 0; nodeIndex < element.size(); ++nodeIndex)
        {
            const uint node = element.getNodeId(nodeIndex);
            if (node >= model.getNNodes())
            {
                diagnostics.add("INVALID_ELEMENT_NODE_REFERENCE", path,
                                "An element references a node outside the model.");
                continue;
            }
            unique.insert(node);
        }
    }
    return {unique.begin(), unique.end()};
}

void addSupport(std::map<uint, std::map<RDofType, double>> &supports,
                uint node,
                RDofType dof,
                double value,
                const QString &path,
                Diagnostics &diagnostics)
{
    auto &nodeSupports = supports[node];
    const auto inserted = nodeSupports.emplace(dof, value);
    if (!inserted.second && inserted.first->second != value)
    {
        diagnostics.add("CONFLICTING_BOUNDARY_CONDITION", path,
                        "The same physical DOF has conflicting boundary values.");
    }
}

void validateBoundaryCondition(const RBoundaryCondition &condition,
                               const std::vector<uint> &nodes,
                               const QString &path,
                               std::map<uint, std::map<RDofType, double>> &supports,
                               Diagnostics &diagnostics)
{
    const auto type = condition.getType();
    if (type == R_BOUNDARY_CONDITION_DISPLACEMENT || type == R_BOUNDARY_CONDITION_ROTATION)
    {
        for (uint componentIndex = 0; componentIndex < condition.size(); ++componentIndex)
        {
            const auto &component = condition.getComponent(componentIndex);
            if (!component.getEnabled()) continue;
            const auto dof = displacementDof(component.getType());
            const QString componentPath = path + QStringLiteral("/components/") + QString::number(componentIndex);
            if (!dof || ((type == R_BOUNDARY_CONDITION_DISPLACEMENT) !=
                         (*dof == RDofType::UX || *dof == RDofType::UY || *dof == RDofType::UZ)))
            {
                diagnostics.add("UNSUPPORTED_BOUNDARY_COMPONENT", componentPath,
                                "Boundary component is not a global translation or rotation axis.");
                continue;
            }
            double value = 0.0;
            if (!readConstantAtZero(component, componentPath, diagnostics, value)) continue;
            for (uint node : nodes)
            {
                addSupport(supports, node, *dof, value,
                           QStringLiteral("/supports/nodes/") + QString::number(node) + "/" + dofName(*dof),
                           diagnostics);
                if (value != 0.0)
                {
                    diagnostics.add("NONZERO_SUPPORT", componentPath,
                                    "WF-03 accepts only explicitly declared zero supports.");
                }
            }
        }
        return;
    }

    if (type == R_BOUNDARY_CONDITION_FORCE)
    {
        for (uint componentIndex = 0; componentIndex < condition.size(); ++componentIndex)
        {
            const auto &component = condition.getComponent(componentIndex);
            if (!component.getEnabled()) continue;
            const QString componentPath = path + QStringLiteral("/components/") + QString::number(componentIndex);
            double value = 0.0;
            if (!readConstantAtZero(component, componentPath, diagnostics, value)) continue;
            if (!axisIndex(component.getType(), R_VARIABLE_FORCE_X, R_VARIABLE_FORCE_Y, R_VARIABLE_FORCE_Z))
            {
                diagnostics.add("UNSUPPORTED_FORCE_COMPONENT", componentPath,
                                "Only global XYZ nodal force components can be checked by this source profile.");
                continue;
            }
            if (value != 0.0)
            {
                diagnostics.add("NONZERO_EXTERNAL_FORCE", componentPath,
                                "The first WF-03 source profile does not accept nonzero external forces.");
            }
        }
        return;
    }

    diagnostics.add("UNSUPPORTED_BOUNDARY_CONDITION", path,
                    QStringLiteral("Boundary condition '") + RBoundaryCondition::getId(type) +
                    QStringLiteral("' is outside the WF-03 support matrix."));
}

void validateInitialCondition(const RInitialCondition &condition,
                              const std::vector<uint> &nodes,
                              const QString &path,
                              bool beam,
                              InitialDofValues &initials,
                              Diagnostics &diagnostics)
{
    const auto type = condition.getType();
    const bool translation = type == R_INITIAL_CONDITION_DISPLACEMENT ||
                             type == R_INITIAL_CONDITION_VELOCITY;
    const bool rotation = type == R_INITIAL_CONDITION_ROTATION ||
                          type == R_INITIAL_CONDITION_ANGULAR_VELOCITY;
    if (!translation && !rotation)
    {
        diagnostics.add("UNSUPPORTED_INITIAL_CONDITION", path,
                        QStringLiteral("Initial condition '") + RInitialCondition::getId(type) +
                        QStringLiteral("' is outside the WF-03 support matrix."));
        return;
    }

    for (uint componentIndex = 0; componentIndex < condition.size(); ++componentIndex)
    {
        const auto &component = condition.getComponent(componentIndex);
        if (!component.getEnabled()) continue;
        const QString componentPath = path + QStringLiteral("/components/") + QString::number(componentIndex);
        const auto dof = initialDof(type, component.getType());
        if (!dof)
        {
            diagnostics.add("UNSUPPORTED_INITIAL_COMPONENT", componentPath,
                            "Initial component is not a global translation or rotation axis.");
            continue;
        }
        double value = 0.0;
        if (!readConstantAtZero(component, componentPath, diagnostics, value)) continue;
        if (!beam && (*dof == RDofType::RX || *dof == RDofType::RY || *dof == RDofType::RZ))
        {
            if (value != 0.0)
            {
                diagnostics.add("INITIAL_CONDITION_WITHOUT_PHYSICAL_DOF", componentPath,
                                "The accepted rod topology has no rotational physical DOF.");
            }
            continue;
        }
        for (uint node : nodes)
        {
            auto &nodeInitials = initials[node];
            const auto inserted = nodeInitials.emplace(std::make_pair(initialField(type), *dof), value);
            if (!inserted.second && inserted.first->second != value)
            {
                diagnostics.add("CONFLICTING_INITIAL_CONDITION",
                                QStringLiteral("/nodes/") + QString::number(node) + "/" + dofName(*dof),
                                "The same physical DOF has conflicting initial values.");
            }
        }
    }
}

void validateEnvironment(const RElementGroup &group,
                         const QString &path,
                         bool rod,
                         bool pointMarkersOnly,
                         Diagnostics &diagnostics)
{
    for (uint i = 0; i < group.getNEnvironmentConditions(); ++i)
    {
        const auto &condition = group.getEnvironmentCondition(i);
        const QString conditionPath = path + QStringLiteral("/environmentConditions/") + QString::number(i);
        if (condition.getType() == R_ENVIRONMENT_CONDITION_TEMPERATURE)
        {
            for (uint componentIndex = 0; componentIndex < condition.size(); ++componentIndex)
            {
                const auto &component = condition.getComponent(componentIndex);
                if (!component.getEnabled()) continue;
                const QString componentPath = conditionPath + QStringLiteral("/components/") + QString::number(componentIndex);
                double value = 0.0;
                if (!readConstantAtZero(component, componentPath, diagnostics, value)) continue;
                if (component.getType() != R_VARIABLE_TEMPERATURE)
                    diagnostics.add("UNSUPPORTED_ENVIRONMENT_COMPONENT", componentPath,
                                    QStringLiteral("Environment condition '") + REnvironmentCondition::getId(condition.getType()) +
                                    QStringLiteral("' has unsupported component '") + RVariable::getId(component.getType()) +
                                    QStringLiteral("'."));
                else if (value != RVariable::getInitValue(R_VARIABLE_TEMPERATURE))
                    diagnostics.add("UNSUPPORTED_ENVIRONMENT_CONDITION", componentPath,
                                    QStringLiteral("Environment condition '") + REnvironmentCondition::getId(condition.getType()) +
                                    QStringLiteral("' component '") + RVariable::getId(component.getType()) +
                                    QStringLiteral("' is a nondefault thermal load; only native default reference temperature metadata is accepted."));
            }
            continue;
        }
        if (condition.getType() != R_ENVIRONMENT_CONDITION_G_ACCELERATION)
        {
            diagnostics.add("UNSUPPORTED_ENVIRONMENT_CONDITION", conditionPath,
                            QStringLiteral("Environment condition '") + REnvironmentCondition::getId(condition.getType()) +
                            QStringLiteral("' is outside the WF-03 Stress-only support matrix."));
            continue;
        }
        for (uint componentIndex = 0; componentIndex < condition.size(); ++componentIndex)
        {
            const auto &component = condition.getComponent(componentIndex);
            if (!component.getEnabled()) continue;
            const QString componentPath = conditionPath + QStringLiteral("/components/") + QString::number(componentIndex);
            double value = 0.0;
            if (!readConstantAtZero(component, componentPath, diagnostics, value)) continue;
            const auto axis = axisIndex(component.getType(), R_VARIABLE_G_ACCELERATION_X,
                                        R_VARIABLE_G_ACCELERATION_Y, R_VARIABLE_G_ACCELERATION_Z);
            if (!axis)
            {
                diagnostics.add("UNSUPPORTED_ENVIRONMENT_COMPONENT", componentPath,
                                QStringLiteral("Environment condition '") + REnvironmentCondition::getId(condition.getType()) +
                                QStringLiteral("' has unsupported component '") + RVariable::getId(component.getType()) +
                                QStringLiteral("'; gravity must be expressed in global XYZ."));
                continue;
            }

            // Gravity is a real body load only when it has a component in a
            // free direction of the accepted source profile. The other
            // components act only on explicitly held equations. POINT groups
            // carry no mass because nonzero point volume is rejected above.
            const bool projectsToFreeDof = !pointMarkersOnly &&
                                           (rod ? *axis == 0 : *axis == 1);
            if (value != 0.0 && projectsToFreeDof)
            {
                diagnostics.add("NONZERO_ENVIRONMENT_LOAD", componentPath,
                                QStringLiteral("Environment condition '") + REnvironmentCondition::getId(condition.getType()) +
                                QStringLiteral("' component '") + RVariable::getId(component.getType()) +
                                QStringLiteral("' applies a nonzero load to a free global DOF."));
            }
        }
    }
}

void validateInitialConditionsOnGroup(const RElementGroup &group,
                                      const std::vector<uint> &nodes,
                                      const QString &path,
                                      bool beam,
                                      InitialDofValues &initials,
                                      Diagnostics &diagnostics)
{
    for (uint i = 0; i < group.getNInitialConditions(); ++i)
    {
        const auto &condition = group.getInitialCondition(i);
        validateInitialCondition(condition, nodes,
                                 path + QStringLiteral("/initialConditions/") + QString::number(i),
                                 beam, initials, diagnostics);
    }
}

void validateBoundaryConditionsOnGroup(const RElementGroup &group,
                                       const std::vector<uint> &nodes,
                                       const QString &path,
                                       std::map<uint, std::map<RDofType, double>> &supports,
                                       Diagnostics &diagnostics)
{
    for (uint i = 0; i < group.getNBoundaryConditions(); ++i)
    {
        const auto &condition = group.getBoundaryCondition(i);
        validateBoundaryCondition(condition, nodes,
                                 path + QStringLiteral("/boundaryConditions/") + QString::number(i),
                                 supports, diagnostics);
    }
}

bool constantMaterialProperty(const RMaterial &material,
                              RMaterialProperty::Type type,
                              const QString &path,
                              Diagnostics &diagnostics,
                              double &value)
{
    const uint position = material.findPosition(type);
    if (position >= material.size())
    {
        diagnostics.add("MISSING_MATERIAL_PROPERTY", path,
                        QStringLiteral("Required material property '") + RMaterialProperty::getId(type) +
                        QStringLiteral("' is missing."));
        return false;
    }
    const auto &property = material.get(position);
    if (property.size() != 1)
    {
        diagnostics.add("UNSUPPORTED_MATERIAL_TABLE", path,
                        "WF-03 accepts constant material values and rejects temperature tables.");
        return false;
    }
    const double key = property.getKey(0);
    value = property.getValue(0);
    if (!std::isfinite(key) || !std::isfinite(value))
    {
        diagnostics.add("NONFINITE_MATERIAL_PROPERTY", path,
                        "Material property keys and values must be finite.");
        return false;
    }
    return true;
}

QJsonObject materialAndSection(const RLine &line,
                               bool beam,
                               const QString &path,
                               Diagnostics &diagnostics)
{
    const RMaterial &material = line.getMaterial();
    if (material.getState() != RMaterial::Solid)
    {
        diagnostics.add("UNSUPPORTED_MATERIAL_STATE", path + QStringLiteral("/material"),
                        "WF-03 requires a solid material.");
    }

    double young = 0.0, density = 0.0, poisson = 0.0;
    constantMaterialProperty(material, RMaterialProperty::ModulusOfElasiticity,
                             path + QStringLiteral("/material/youngModulusPa"), diagnostics, young);
    constantMaterialProperty(material, RMaterialProperty::Density,
                             path + QStringLiteral("/material/densityKgM3"), diagnostics, density);
    constantMaterialProperty(material, RMaterialProperty::PoissonRatio,
                             path + QStringLiteral("/material/poissonRatio"), diagnostics, poisson);
    if (std::isfinite(young) && young <= 0.0)
        diagnostics.add("INVALID_MATERIAL_PROPERTY", path + QStringLiteral("/material/youngModulusPa"),
                        "Young's modulus must be positive.");
    if (std::isfinite(density) && density <= 0.0)
        diagnostics.add("INVALID_MATERIAL_PROPERTY", path + QStringLiteral("/material/densityKgM3"),
                        "Density must be positive.");
    if (std::isfinite(poisson) && !(poisson > -1.0 && poisson < 0.5))
        diagnostics.add("INVALID_MATERIAL_PROPERTY", path + QStringLiteral("/material/poissonRatio"),
                        "Poisson ratio must lie strictly between -1 and 0.5.");

    for (uint i = 0; i < material.size(); ++i)
    {
        const auto type = material.get(i).getType();
        if (type == RMaterialProperty::ModulusOfElasiticity ||
            type == RMaterialProperty::Density || type == RMaterialProperty::PoissonRatio)
        {
            continue;
        }
        if (type == RMaterialProperty::ThermalExpansionCoefficient)
        {
            double value = 0.0;
            const QString alphaPath = path + QStringLiteral("/material/thermalExpansionCoefficient");
            if (material.get(i).size() != 1)
            {
                diagnostics.add("UNSUPPORTED_MATERIAL_TABLE", alphaPath,
                                "WF-03 rejects temperature-dependent material tables.");
            }
            else
            {
                value = material.get(i).getValue(0);
                if (!std::isfinite(value) || value != 0.0)
                    diagnostics.add("UNSUPPORTED_MATERIAL_PROPERTY", alphaPath,
                                    "Thermal expansion must be absent or explicitly zero.");
            }
            continue;
        }
        diagnostics.add("UNSUPPORTED_MATERIAL_PROPERTY",
                        path + QStringLiteral("/material/") + RMaterialProperty::getId(type),
                        "Material property is outside the WF-03 stress-only source profile.");
    }

    const double area = line.getCrossArea();
    const double iy = line.getSecondMomentY();
    const double iz = line.getSecondMomentZ();
    const double torsion = line.getTorsionalConstant();
    const double wy = line.getSectionModulusY();
    const double wz = line.getSectionModulusZ();
    const double wt = line.getTorsionalSectionModulus();
    const auto checkSection = [&](double value, const QString &name) {
        if (!std::isfinite(value) || value <= 0.0)
            diagnostics.add("INVALID_SECTION_PROPERTY", path + QStringLiteral("/section/") + name,
                            "Section values required by WF-03 must be finite and positive.");
    };
    checkSection(area, QStringLiteral("areaM2"));
    if (beam)
    {
        checkSection(iy, QStringLiteral("iyM4"));
        checkSection(iz, QStringLiteral("izM4"));
        checkSection(torsion, QStringLiteral("torsionM4"));
        // Native beam stress recovery executes even for zero-motion sources.
        checkSection(wy, QStringLiteral("wyM3"));
        checkSection(wz, QStringLiteral("wzM3"));
        checkSection(wt, QStringLiteral("wtM3"));
    }

    QJsonObject section{{"areaM2", area}, {"iyM4", QJsonValue(QJsonValue::Null)},
                        {"izM4", QJsonValue(QJsonValue::Null)},
                        {"torsionM4", QJsonValue(QJsonValue::Null)},
                        {"wyM3", QJsonValue(QJsonValue::Null)},
                        {"wzM3", QJsonValue(QJsonValue::Null)},
                        {"wtM3", QJsonValue(QJsonValue::Null)}};
    if (beam)
    {
        section.insert("iyM4", iy);
        section.insert("izM4", iz);
        section.insert("torsionM4", torsion);
        section.insert("wyM3", wy);
        section.insert("wzM3", wz);
        section.insert("wtM3", wt);
    }
    return {{"material", QJsonObject{{"youngModulusPa", young}, {"densityKgM3", density},
                                      {"poissonRatio", poisson}}},
            {"section", section}};
}

QJsonObject dynamicsJson(const RModel &model)
{
    const auto &setup = model.getProblemSetup();
    const auto &dynamics = setup.getStructuralDynamicsSetup();
    const auto &restart = setup.getStructuralDynamicsRestartData();
    QString method;
    switch (dynamics.getMethod())
    {
        case RStructuralTimeIntegrationMethod::Newmark: method = QStringLiteral("newmark"); break;
        case RStructuralTimeIntegrationMethod::GeneralizedAlpha: method = QStringLiteral("generalized-alpha"); break;
        case RStructuralTimeIntegrationMethod::CentralDifference: method = QStringLiteral("central-difference"); break;
    }
    QJsonArray ratios;
    for (double ratio : dynamics.getModalDampingRatios()) ratios.append(ratio);
    return {{"enabled", dynamics.getEnabled()}, {"method", method},
            {"massMatrix", setup.getMassMatrixType() == RMassMatrixType::Consistent
                               ? QStringLiteral("consistent") : QStringLiteral("lumped")},
            {"timeStepSeconds", dynamics.getTimeStep()}, {"steps", int(dynamics.getNSteps())},
            {"outputFrequency", int(dynamics.getOutputFrequency())},
            {"newmarkGamma", dynamics.getNewmarkGamma()}, {"newmarkBeta", dynamics.getNewmarkBeta()},
            {"rayleighAlpha", dynamics.getRayleighAlpha()}, {"rayleighBeta", dynamics.getRayleighBeta()},
            {"rhoInfinity", dynamics.getRhoInfinity()}, {"modalDampingRatios", ratios},
            {"legacyTimeSolverEnabled", model.getTimeSolver().getEnabled()},
            {"restartAvailable", restart.available}};
}

void validateStoredSettings(const RModel &model, Diagnostics &diagnostics)
{
    const auto &task = model.getProblemTaskTree();
    if (task.getProblemTypeMask() != R_PROBLEM_STRESS)
        diagnostics.add("UNSUPPORTED_ANALYSIS_TASK", "/analysis/problemTask",
                        "The source must declare Stress only.");

    const auto &problem = model.getProblemSetup();
    const auto &dynamics = problem.getStructuralDynamicsSetup();
    const auto &restart = problem.getStructuralDynamicsRestartData();
    try
    {
        dynamics.validate();
    }
    catch (const RError &error)
    {
        diagnostics.add("INVALID_DYNAMICS_SETUP", "/dynamics", error.getMessage());
    }
    catch (const std::exception &error)
    {
        diagnostics.add("INVALID_DYNAMICS_SETUP", "/dynamics", QString::fromUtf8(error.what()));
    }
    if (!dynamics.getEnabled())
        diagnostics.add("DYNAMICS_NOT_ENABLED", "/dynamics/enabled",
                        "The source must contain an enabled structural dynamics setup.");
    if (dynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark)
        diagnostics.add("UNSUPPORTED_TIME_INTEGRATOR", "/dynamics/method",
                        "WF-03 supports Newmark integration only.");
    if (problem.getMassMatrixType() != RMassMatrixType::Consistent)
        diagnostics.add("UNSUPPORTED_MASS_MATRIX", "/dynamics/massMatrix",
                        "WF-03 requires a consistent mass matrix.");
    if (dynamics.getNewmarkGamma() != 0.5)
        diagnostics.add("UNSUPPORTED_NEWMARK_PARAMETER", "/dynamics/newmarkGamma",
                        "WF-03 requires Newmark gamma = 0.5.");
    if (dynamics.getNewmarkBeta() != 0.25)
        diagnostics.add("UNSUPPORTED_NEWMARK_PARAMETER", "/dynamics/newmarkBeta",
                        "WF-03 requires Newmark beta = 0.25.");
    if (dynamics.getRayleighAlpha() != 0.0)
        diagnostics.add("UNSUPPORTED_RAYLEIGH_DAMPING", "/dynamics/rayleighAlpha",
                        "WF-03 requires zero Rayleigh alpha damping.");
    if (dynamics.getRayleighBeta() != 0.0)
        diagnostics.add("UNSUPPORTED_RAYLEIGH_DAMPING", "/dynamics/rayleighBeta",
                        "WF-03 requires zero Rayleigh beta damping.");
    if (!dynamics.getModalDampingRatios().empty())
        diagnostics.add("UNSUPPORTED_MODAL_DAMPING", "/dynamics/modalDampingRatios",
                        "WF-03 requires an empty modal damping ratio list.");
    if (!std::isfinite(dynamics.getTimeStep()) || dynamics.getTimeStep() <= 0.0 ||
        !std::isfinite(dynamics.getNewmarkGamma()) || !std::isfinite(dynamics.getNewmarkBeta()) ||
        !std::isfinite(dynamics.getRayleighAlpha()) || !std::isfinite(dynamics.getRayleighBeta()) ||
        !std::isfinite(dynamics.getRhoInfinity()) ||
        dynamics.getNSteps() == 0 || dynamics.getOutputFrequency() == 0)
        diagnostics.add("INVALID_DYNAMICS_SETUP", "/dynamics",
                        "Time step, step count, and output frequency must be positive and finite.");
    if (model.getTimeSolver().getEnabled())
        diagnostics.add("LEGACY_DYNAMICS_ENABLED", "/analysis/legacyTimeSolver",
                        "WF-03 does not accept legacy-only time-solver setup.");
    if (problem.getRestart() || restart.available)
        diagnostics.add("ACCEPTED_RESTART_PRESENT", "/dynamics/restart",
                        "An accepted restart cannot be used as a fresh WF-03 source.");

    // Fresh native stress solves recover these result vectors before applying
    // explicit initial conditions. Reject them rather than describe missing ICs
    // as zero while silently retaining a previous solve's state.
    for (uint index = 0; index < model.getNVariables(); ++index)
    {
        const auto type = model.getVariable(index).getType();
        if (type == R_VARIABLE_DISPLACEMENT || type == R_VARIABLE_ROTATION ||
            type == R_VARIABLE_VELOCITY || type == R_VARIABLE_ANGULAR_VELOCITY)
            diagnostics.add("SAVED_INITIAL_STATE_PRESENT",
                            QStringLiteral("/results/variables/") + QString::number(index),
                            "Saved displacement, rotation, velocity or angular velocity cannot be used as a fresh WF-03 source.");
    }

    const auto &cable = problem.getStructuralCableSetup();
    if (cable.enabled || !cable.members.empty())
        diagnostics.add("UNSUPPORTED_CABLE_PHYSICS", "/physics/cable",
                        "Cable form finding is outside the WF-03 source profile.");
    const auto &piezo = problem.getStructuralPiezoSetup();
    if (piezo.enabled || !piezo.ports.empty() || !piezo.nodalBoundaryConditions.empty())
        diagnostics.add("UNSUPPORTED_PIEZO_PHYSICS", "/physics/piezo",
                        "Piezoelectric physics is outside the WF-03 source profile.");
    const auto &thermal = problem.getStructuralThermalControlSetup();
    if (thermal.enabled || !thermal.channels.empty() || !thermal.sparseObservationNodeIds.empty())
        diagnostics.add("UNSUPPORTED_THERMAL_PHYSICS", "/physics/thermalControl",
                        "Thermal-control physics is outside the WF-03 source profile.");

    const auto &interaction = problem.getStructuralInteractionSetup();
    if (!interaction.getLinearConstraints().empty())
        diagnostics.add("UNSUPPORTED_MPC", "/interactions/linearConstraints",
                        "WF-03 does not accept multi-point constraints.");
    if (!interaction.getCouplings().empty())
        diagnostics.add("UNSUPPORTED_COUPLING", "/interactions/couplings",
                        "WF-03 does not accept structural couplings.");
    if (!interaction.getPrescribedMotions().empty())
        diagnostics.add("UNSUPPORTED_PRESCRIBED_MOTION", "/interactions/prescribedMotions",
                        "WF-03 does not accept prescribed motion.");

    try
    {
        problem.getStructuralControlSetup().validate();
    }
    catch (const RError &error)
    {
        diagnostics.add("INVALID_SOURCE_CONTROL_METADATA", "/originalControl", error.getMessage());
    }
    catch (const std::exception &error)
    {
        diagnostics.add("INVALID_SOURCE_CONTROL_METADATA", "/originalControl",
                        QString::fromUtf8(error.what()));
    }
}

bool readRawModel(const QString &path, RModel &model, QString &error)
{
    const QFileInfo info(path);
    const bool ascii = info.suffix() == RModel::getDefaultFileExtension(false);
    if (!ascii && info.suffix() != RModel::getDefaultFileExtension(true))
    {
        error = QStringLiteral("Expected a raw .ram or .rbm MODEL file.");
        return false;
    }

    try
    {
        RFile file(path, ascii ? RFile::ASCII : RFile::BINARY);
        if (!file.open(ascii ? QIODevice::ReadOnly | QIODevice::Text : QIODevice::ReadOnly))
        {
            error = QStringLiteral("Cannot open the native model header.");
            return false;
        }
        RFileHeader header;
        if (ascii) RFileIO::readAscii(file, header); else RFileIO::readBinary(file, header);
        if (header.getType() != R_FILE_TYPE_MODEL)
        {
            error = QStringLiteral("Input is not a raw MODEL file; native LINK files are not accepted.");
            return false;
        }
        model.read(path);
        return true;
    }
    catch (const RError &exception)
    {
        error = exception.getMessage();
    }
    catch (const std::exception &exception)
    {
        error = QString::fromUtf8(exception.what());
    }
    return false;
}

QJsonObject describeModel(const RModel &model,
                          const QString &sourceSha,
                          Diagnostics &diagnostics)
{
    validateStoredSettings(model, diagnostics);

    const bool rod = model.getNNodes() == 5 && model.getNElements(R_ELEMENT_TRUSS1) == 4 &&
                     model.getNElements(R_ELEMENT_BEAM1) == 0 &&
                     model.getNElements(R_ELEMENT_POINT) == 6 && model.getNElements() == 10;
    const bool beam = model.getNNodes() == 2 && model.getNElements(R_ELEMENT_BEAM1) == 1 &&
                      model.getNElements(R_ELEMENT_TRUSS1) == 0 &&
                      model.getNElements(R_ELEMENT_POINT) == 2 && model.getNElements() == 3;
    if (!rod && !beam)
    {
        diagnostics.add("UNSUPPORTED_TOPOLOGY", "/topology",
                        "Expected the five-node four-TRUSS1 rod with six POINT elements or the two-node BEAM1 cantilever with two POINT elements.");
        return {};
    }

    if (model.getNSurfaces() != 0 || model.getNVolumes() != 0)
        diagnostics.add("UNSUPPORTED_TOPOLOGY", "/topology",
                        "WF-03 accepts line elements and point markers only.");

    if (model.getNLines() == 0 || model.getNPoints() == 0)
        diagnostics.add("INVALID_ENTITY_GROUPS", "/topology/groups",
                        "The accepted topology requires nonempty line and point groups.");

    const uint nodeCount = model.getNNodes();
    std::vector<uint> nodeOrder(nodeCount);
    for (uint i = 0; i < nodeCount; ++i) nodeOrder[i] = i;
    for (uint i = 0; i < nodeCount; ++i)
    {
        const auto &node = model.getNode(i);
        if (!std::isfinite(node.getX()) || !std::isfinite(node.getY()) || !std::isfinite(node.getZ()))
            diagnostics.add("NONFINITE_NODE_COORDINATE", QStringLiteral("/nodes/") + QString::number(i) + "/coordinates",
                            "Global node coordinates must be finite.");
        if (model.getNodePhysicalId(i) == 0)
            diagnostics.add("MISSING_PHYSICAL_NODE_ID", QStringLiteral("/nodes/") + QString::number(i) + "/physicalId",
                            "Every node must have a nonzero stable physical identifier.");
    }
    if (!diagnostics.empty()) return {};
    std::sort(nodeOrder.begin(), nodeOrder.end(), [&](uint left, uint right) {
        return model.getNode(left).getX() < model.getNode(right).getX();
    });

    const auto &first = model.getNode(nodeOrder.front());
    const double lengthScale = std::max(1.0, std::abs(model.getNode(nodeOrder.back()).getX() - first.getX()));
    for (uint position : nodeOrder)
    {
        const auto &node = model.getNode(position);
        if (!near(node.getY(), first.getY(), lengthScale) || !near(node.getZ(), first.getZ(), lengthScale))
            diagnostics.add("UNSUPPORTED_GEOMETRY", QStringLiteral("/nodes/") + QString::number(position) + "/coordinates",
                            "The accepted line must be straight and parallel to global X.");
    }
    for (uint i = 1; i < nodeOrder.size(); ++i)
    {
        if (!(model.getNode(nodeOrder[i]).getX() > model.getNode(nodeOrder[i - 1]).getX()))
            diagnostics.add("INVALID_GEOMETRY_LENGTH", QStringLiteral("/nodes/") + QString::number(nodeOrder[i]) + "/coordinates/0",
                            "Adjacent physical nodes must have finite, positive global-X spacing.");
    }

    std::vector<int> lineGroupForElement(model.getNElements(), -1);
    std::vector<int> pointGroupForElement(model.getNElements(), -1);
    for (uint groupIndex = 0; groupIndex < model.getNLines(); ++groupIndex)
    {
        const RLine &group = model.getLine(groupIndex);
        if (group.empty())
            diagnostics.add("EMPTY_ELEMENT_GROUP", QStringLiteral("/lines/") + QString::number(groupIndex),
                            "Every line group must contain at least one supported element.");
        for (uint i = 0; i < group.size(); ++i)
        {
            const uint elementId = group.get(i);
            if (elementId >= model.getNElements())
            {
                diagnostics.add("INVALID_GROUP_ELEMENT_REFERENCE", QStringLiteral("/lines/") + QString::number(groupIndex),
                                "Line group references an element outside the model.");
                continue;
            }
            if (lineGroupForElement[elementId] != -1 || pointGroupForElement[elementId] != -1)
                diagnostics.add("DUPLICATE_ELEMENT_GROUP_REFERENCE", QStringLiteral("/elements/") + QString::number(elementId),
                                "Each element must belong to exactly one element group.");
            lineGroupForElement[elementId] = int(groupIndex);
        }
    }
    for (uint groupIndex = 0; groupIndex < model.getNPoints(); ++groupIndex)
    {
        const RPoint &group = model.getPoint(groupIndex);
        if (group.empty())
            diagnostics.add("EMPTY_ELEMENT_GROUP", QStringLiteral("/points/") + QString::number(groupIndex),
                            "Every point group must contain at least one POINT element.");
        if (!std::isfinite(group.getVolume()) || group.getVolume() != 0.0)
            diagnostics.add("UNSUPPORTED_POINT_MASS", QStringLiteral("/points/") + QString::number(groupIndex) + "/volume",
                            "WF-03 does not accept point masses or nonzero point volumes.");
        for (uint i = 0; i < group.size(); ++i)
        {
            const uint elementId = group.get(i);
            if (elementId >= model.getNElements())
            {
                diagnostics.add("INVALID_GROUP_ELEMENT_REFERENCE", QStringLiteral("/points/") + QString::number(groupIndex),
                                "Point group references an element outside the model.");
                continue;
            }
            if (lineGroupForElement[elementId] != -1 || pointGroupForElement[elementId] != -1)
                diagnostics.add("DUPLICATE_ELEMENT_GROUP_REFERENCE", QStringLiteral("/elements/") + QString::number(elementId),
                                "Each element must belong to exactly one element group.");
            pointGroupForElement[elementId] = int(groupIndex);
        }
    }

    for (uint elementId = 0; elementId < model.getNElements(); ++elementId)
    {
        const auto type = model.getElement(elementId).getType();
        if (type == R_ELEMENT_POINT)
        {
            if (model.getElement(elementId).size() != 1)
                diagnostics.add("INVALID_POINT_ELEMENT", QStringLiteral("/elements/") + QString::number(elementId),
                                "Each POINT marker must reference exactly one node.");
            if (pointGroupForElement[elementId] < 0 || lineGroupForElement[elementId] >= 0)
                diagnostics.add("UNASSIGNED_ELEMENT_GROUP", QStringLiteral("/elements/") + QString::number(elementId),
                                "Every POINT element must belong to exactly one point group.");
        }
        else
        {
            if (lineGroupForElement[elementId] < 0 || pointGroupForElement[elementId] >= 0)
                diagnostics.add("UNASSIGNED_ELEMENT_GROUP", QStringLiteral("/elements/") + QString::number(elementId),
                                "Every structural element must belong to exactly one line group.");
        }
    }

    if (beam && model.getNElements(R_ELEMENT_BEAM1) == 1)
    {
        const uint beamElement = [&]() {
            for (uint elementId = 0; elementId < model.getNElements(); ++elementId)
                if (model.getElement(elementId).getType() == R_ELEMENT_BEAM1) return elementId;
            return uint(0);
        }();
        const int lineGroupIndex = lineGroupForElement[beamElement];
        if (lineGroupIndex < 0)
        {
            diagnostics.add("UNASSIGNED_ELEMENT_GROUP", "/elements/0",
                            "The beam must belong to one line group with a verifiable local axis.");
        }
        else
        {
            const RR3Vector localY = model.getLine(uint(lineGroupIndex)).getLocalYDirection();
            const double norm = std::sqrt(localY[0] * localY[0] + localY[1] * localY[1] + localY[2] * localY[2]);
            if (!std::isfinite(norm) || !near(localY[0], 0.0) || !near(localY[2], 0.0) ||
                !(localY[1] > 0.0) || !near(norm, 1.0))
                diagnostics.add("UNSUPPORTED_LOCAL_AXES", "/topology/localYDirection",
                                "The beam local Y axis must be a unit vector aligned with global positive Y.");
        }
    }

    std::vector<uint> orderedStructuralElements;
    std::map<std::pair<uint, uint>, uint> elementByAdjacentNodes;
    for (uint elementId = 0; elementId < model.getNElements(); ++elementId)
    {
        const RElement &element = model.getElement(elementId);
        if (element.getType() == R_ELEMENT_POINT) continue;
        if (element.size() != 2)
        {
            diagnostics.add("UNSUPPORTED_ELEMENT_ORDER", QStringLiteral("/elements/") + QString::number(elementId),
                            "WF-03 supports two-node TRUSS1 or BEAM1 elements only.");
            continue;
        }
        const uint nodeA = element.getNodeId(0);
        const uint nodeB = element.getNodeId(1);
        if (nodeA >= nodeCount || nodeB >= nodeCount || nodeA == nodeB)
        {
            diagnostics.add("INVALID_ELEMENT_CONNECTIVITY", QStringLiteral("/elements/") + QString::number(elementId),
                            "Structural element connectivity must reference two distinct model nodes.");
            continue;
        }
        const uint posA = uint(std::find(nodeOrder.begin(), nodeOrder.end(), nodeA) - nodeOrder.begin());
        const uint posB = uint(std::find(nodeOrder.begin(), nodeOrder.end(), nodeB) - nodeOrder.begin());
        const std::pair<uint, uint> key{std::min(posA, posB), std::max(posA, posB)};
        if (!elementByAdjacentNodes.emplace(key, elementId).second)
            diagnostics.add("DUPLICATE_STRUCTURAL_CONNECTIVITY", QStringLiteral("/elements/") + QString::number(elementId),
                            "The accepted chain cannot contain duplicate structural elements.");
    }
    for (uint i = 0; i + 1 < nodeOrder.size(); ++i)
    {
        const auto found = elementByAdjacentNodes.find({i, i + 1});
        if (found == elementByAdjacentNodes.end())
        {
            diagnostics.add("UNSUPPORTED_CONNECTIVITY", "/topology/connectivity",
                            "Structural elements must form one connected chain between adjacent global-X nodes.");
        }
        else
        {
            orderedStructuralElements.push_back(found->second);
        }
    }
    if (elementByAdjacentNodes.size() != orderedStructuralElements.size())
        diagnostics.add("UNSUPPORTED_CONNECTIVITY", "/topology/connectivity",
                        "Structural connectivity contains an extra, disconnected, or nonadjacent element.");

    std::map<uint, std::map<RDofType, double>> actualSupports;
    InitialDofValues actualInitials;
    for (uint groupIndex = 0; groupIndex < model.getNLines(); ++groupIndex)
    {
        const auto &group = model.getLine(groupIndex);
        const QString path = QStringLiteral("/lines/") + QString::number(groupIndex);
        const auto nodes = groupNodePositions(model, group, path, diagnostics);
        validateBoundaryConditionsOnGroup(group, nodes, path, actualSupports, diagnostics);
        validateInitialConditionsOnGroup(group, nodes, path, beam, actualInitials, diagnostics);
        validateEnvironment(group, path, rod, false, diagnostics);
    }
    for (uint groupIndex = 0; groupIndex < model.getNPoints(); ++groupIndex)
    {
        const auto &group = model.getPoint(groupIndex);
        const QString path = QStringLiteral("/points/") + QString::number(groupIndex);
        const auto nodes = groupNodePositions(model, group, path, diagnostics);
        validateBoundaryConditionsOnGroup(group, nodes, path, actualSupports, diagnostics);
        validateInitialConditionsOnGroup(group, nodes, path, beam, actualInitials, diagnostics);
        validateEnvironment(group, path, rod, true, diagnostics);
    }

    std::vector<std::set<RDofType>> physicalDofs(nodeCount);
    const auto &descriptor = RElementDescriptor::get(beam ? R_ELEMENT_BEAM1 : R_ELEMENT_TRUSS1);
    for (uint position : nodeOrder)
        for (RDofType dof : descriptor.getDofsPerNode()) physicalDofs[position].insert(dof);

    std::map<uint, std::set<RDofType>> expectedSupports;
    if (rod)
    {
        for (uint order = 0; order < nodeOrder.size(); ++order)
        {
            expectedSupports[nodeOrder[order]].insert(RDofType::UY);
            expectedSupports[nodeOrder[order]].insert(RDofType::UZ);
        }
        expectedSupports[nodeOrder.front()].insert(RDofType::UX);
    }
    else
    {
        for (RDofType dof : physicalDofs[nodeOrder.front()])
            expectedSupports[nodeOrder.front()].insert(dof);
        for (RDofType dof : {RDofType::UX, RDofType::UZ, RDofType::RX, RDofType::RY})
            expectedSupports[nodeOrder.back()].insert(dof);
    }

    for (const auto &[node, nodeSupports] : actualSupports)
    {
        for (const auto &[dof, value] : nodeSupports)
        {
            if (!physicalDofs[node].contains(dof))
                diagnostics.add("SUPPORT_WITHOUT_PHYSICAL_DOF",
                                QStringLiteral("/supports/nodes/") + QString::number(node) + "/" + dofName(dof),
                                "The source constrains a DOF not supported by its element formulation.");
            if (!expectedSupports[node].contains(dof))
                diagnostics.add("UNSUPPORTED_SUPPORT_PATTERN",
                                QStringLiteral("/supports/nodes/") + QString::number(node) + "/" + dofName(dof),
                                "Boundary support pattern differs from the accepted WF-03 profile.");
            if (value != 0.0)
                diagnostics.add("NONZERO_SUPPORT",
                                QStringLiteral("/supports/nodes/") + QString::number(node) + "/" + dofName(dof),
                                "WF-03 accepts only zero-valued supports.");
        }
    }
    for (const auto &[node, expected] : expectedSupports)
    {
        for (RDofType dof : expected)
        {
            const auto foundNode = actualSupports.find(node);
            if (foundNode == actualSupports.end() || foundNode->second.find(dof) == foundNode->second.end())
                diagnostics.add("MISSING_REQUIRED_SUPPORT",
                                QStringLiteral("/supports/nodes/") + QString::number(node) + "/" + dofName(dof),
                                "A required zero support is not explicitly declared in the source model.");
        }
    }

    for (const auto &[node, nodeInitials] : actualInitials)
    {
        for (const auto &[fieldAndDof, value] : nodeInitials)
        {
            const auto dof = fieldAndDof.second;
            if (!physicalDofs[node].contains(dof))
                diagnostics.add("INITIAL_CONDITION_WITHOUT_PHYSICAL_DOF",
                                QStringLiteral("/nodes/") + QString::number(node) + "/" + dofName(dof),
                                "Initial data references a DOF not supported by the element formulation.");
            const auto supportNode = actualSupports.find(node);
            if (supportNode != actualSupports.end())
            {
                const auto support = supportNode->second.find(dof);
                if (support != supportNode->second.end() && support->second != value)
                    diagnostics.add("INITIAL_SUPPORT_CONFLICT",
                                    QStringLiteral("/nodes/") + QString::number(node) + "/" + dofName(dof),
                                    "Initial value conflicts with the source's prescribed support value.");
            }
        }
    }

    if (!diagnostics.empty()) return {};

    std::vector<std::array<double, 3>> initialU(nodeCount), initialV(nodeCount), initialR(nodeCount), initialW(nodeCount);
    for (auto &values : initialU) values.fill(0.0);
    for (auto &values : initialV) values.fill(0.0);
    for (auto &values : initialR) values.fill(0.0);
    for (auto &values : initialW) values.fill(0.0);
    for (const auto &[node, values] : actualInitials)
    {
        for (const auto &[fieldAndDof, value] : values)
        {
            std::array<double, 3> *target = nullptr;
            const auto field = fieldAndDof.first;
            const auto dof = fieldAndDof.second;
            if (field == InitialField::Displacement)
            {
                if (dof == RDofType::UX) target = &initialU[node];
                else if (dof == RDofType::UY) target = &initialU[node];
                else if (dof == RDofType::UZ) target = &initialU[node];
            }
            else if (field == InitialField::Velocity)
            {
                if (dof == RDofType::UX) target = &initialV[node];
                else if (dof == RDofType::UY) target = &initialV[node];
                else if (dof == RDofType::UZ) target = &initialV[node];
            }
            else if (field == InitialField::Rotation)
            {
                if (dof == RDofType::RX) target = &initialR[node];
                else if (dof == RDofType::RY) target = &initialR[node];
                else if (dof == RDofType::RZ) target = &initialR[node];
            }
            else if (field == InitialField::AngularVelocity)
            {
                if (dof == RDofType::RX) target = &initialW[node];
                else if (dof == RDofType::RY) target = &initialW[node];
                else if (dof == RDofType::RZ) target = &initialW[node];
            }
            if (!target) continue;
            const int axis = dof == RDofType::UX || dof == RDofType::RX ? 0
                             : dof == RDofType::UY || dof == RDofType::RY ? 1 : 2;
            (*target)[axis] = value;
        }
    }

    QJsonArray nodesJson;
    for (uint position = 0; position < nodeOrder.size(); ++position)
    {
        const uint nodeIndex = nodeOrder[position];
        const auto &node = model.getNode(nodeIndex);
        nodesJson.append(QJsonObject{
            {"position", int(nodeIndex)}, {"physicalId", jsonPhysicalId(model.getNodePhysicalId(nodeIndex))},
            {"coordinates", QJsonArray{node.getX(), node.getY(), node.getZ()}},
            {"initialDisplacementM", vectorJson(initialU[nodeIndex])},
            {"initialVelocityMS", vectorJson(initialV[nodeIndex])},
            {"initialRotationRad", vectorJson(initialR[nodeIndex])},
            {"initialAngularVelocityRadS", vectorJson(initialW[nodeIndex])}});
    }

    QJsonArray elementsJson;
    for (uint elementId : orderedStructuralElements)
    {
        const auto &element = model.getElement(elementId);
        const uint lineGroupIndex = uint(lineGroupForElement[elementId]);
        const auto actual = materialAndSection(model.getLine(lineGroupIndex), beam,
                                               QStringLiteral("/elements/") + QString::number(element.getPhysicalId()),
                                               diagnostics);
        elementsJson.append(QJsonObject{
            {"type", beam ? QStringLiteral("beam1") : QStringLiteral("truss1")},
            {"physicalId", jsonPhysicalId(element.getPhysicalId())},
            {"nodePhysicalIds", QJsonArray{jsonPhysicalId(model.getNodePhysicalId(element.getNodeId(0))),
                                            jsonPhysicalId(model.getNodePhysicalId(element.getNodeId(1)))}},
            {"material", actual.value("material")}, {"section", actual.value("section")}});
    }

    QJsonArray supportsJson;
    QJsonArray freeDofsJson;
    const std::vector<RDofType> dofOrder{RDofType::UX, RDofType::UY, RDofType::UZ,
                                         RDofType::RX, RDofType::RY, RDofType::RZ};
    for (uint position = 0; position < nodeOrder.size(); ++position)
    {
        const uint node = nodeOrder[position];
        for (RDofType dof : dofOrder)
        {
            if (!physicalDofs[node].contains(dof)) continue;
            if (expectedSupports[node].contains(dof))
            {
                supportsJson.append(QJsonObject{
                    {"nodePhysicalId", jsonPhysicalId(model.getNodePhysicalId(node))},
                    {"physicalDof", dofName(dof)}, {"globalDirection", dofDirection(dof)},
                    {"value", 0.0}, {"unit", dofUnit(dof)}});
            }
            else
            {
                freeDofsJson.append(QJsonObject{
                    {"nodePhysicalId", jsonPhysicalId(model.getNodePhysicalId(node))},
                    {"physicalDof", dofName(dof)}, {"globalDirection", dofDirection(dof)},
                    {"unit", dofUnit(dof)}});
            }
        }
    }

    QJsonArray pointElementsJson;
    for (uint elementId = 0; elementId < model.getNElements(); ++elementId)
    {
        const auto &element = model.getElement(elementId);
        if (element.getType() != R_ELEMENT_POINT) continue;
        pointElementsJson.append(QJsonObject{
            {"physicalId", jsonPhysicalId(element.getPhysicalId())},
            {"nodePhysicalId", jsonPhysicalId(model.getNodePhysicalId(element.getNodeId(0)))}});
    }

    const auto &control = model.getProblemSetup().getStructuralControlSetup();
    const uint tip = nodeOrder.back();
    const RDofType portDof = rod ? RDofType::UX : RDofType::UY;
    const QJsonObject ports{
        {"force", QJsonObject{{"id", "source-tip-force"},
                              {"nodePhysicalId", jsonPhysicalId(model.getNodePhysicalId(tip))},
                              {"globalDirection", dofDirection(portDof)}, {"unit", "N"},
                              {"minimum", kSourceForceMinimum}, {"maximum", kSourceForceMaximum},
                              {"kind", "nodal_force"}}},
        {"velocity", QJsonObject{{"id", "source-tip-velocity"},
                                 {"nodePhysicalId", jsonPhysicalId(model.getNodePhysicalId(tip))},
                                 {"globalDirection", dofDirection(portDof)}, {"unit", "m/s"},
                                 {"kind", "velocity"}}}};

    if (!diagnostics.empty()) return {};
    QJsonObject result = envelope(QStringLiteral("supported"), sourceSha, {});
    result.insert("modelKind", rod ? QStringLiteral("four-rod") : QStringLiteral("cantilever-beam"));
    result.insert("nodes", nodesJson);
    result.insert("elements", elementsJson);
    result.insert("pointElements", pointElementsJson);
    result.insert("supports", supportsJson);
    result.insert("freeDofs", freeDofsJson);
    result.insert("ports", ports);
    result.insert("dynamics", dynamicsJson(model));
    result.insert("originalControl", QJsonObject{
        {"enabled", control.enabled}, {"controlProtocolVersion", int(control.controlProtocolVersion)},
        {"controllerId", control.controllerId},
        {"deterministicSeed", QString::number(control.deterministicSeed)}});
    return result;
}
} // namespace

QJsonObject describe(const QString &inputPath, const QString &expectedSha256)
{
    Diagnostics diagnostics;
    const QFileInfo info(inputPath);
    if (!info.isFile() || info.isSymLink())
    {
        diagnostics.add("INVALID_INPUT_FILE", "/input",
                        "Input must be a regular raw native model file, not a symbolic link.");
        return envelope(QStringLiteral("rejected"), {}, diagnostics.toJson());
    }
    const QString canonicalPath = info.canonicalFilePath();
    if (canonicalPath.isEmpty())
    {
        diagnostics.add("INVALID_INPUT_FILE", "/input", "Input path cannot be resolved.");
        return envelope(QStringLiteral("rejected"), {}, diagnostics.toJson());
    }
    if (!QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(expectedSha256).hasMatch())
    {
        diagnostics.add("INVALID_EXPECTED_SHA256", "/expectedSha256",
                        "Expected SHA-256 must contain exactly 64 hexadecimal characters.");
        return envelope(QStringLiteral("rejected"), {}, diagnostics.toJson());
    }

    QByteArray before;
    try
    {
        before = fileSha256(canonicalPath);
    }
    catch (const std::exception &error)
    {
        diagnostics.add("INPUT_READ_FAILED", "/input", QString::fromUtf8(error.what()));
        return envelope(QStringLiteral("rejected"), {}, diagnostics.toJson());
    }
    const QString sourceSha = QString::fromLatin1(before);
    if (sourceSha.compare(expectedSha256, Qt::CaseInsensitive) != 0)
    {
        diagnostics.add("SOURCE_SHA256_MISMATCH", "/expectedSha256",
                        "Input bytes do not match the caller's expected SHA-256.");
        return envelope(QStringLiteral("rejected"), sourceSha, diagnostics.toJson());
    }

    RModel model;
    QString readError;
    const bool readOk = readRawModel(canonicalPath, model, readError);
    QJsonObject description;
    if (readOk)
    {
        try
        {
            description = describeModel(model, sourceSha, diagnostics);
        }
        catch (const RError &error)
        {
            diagnostics.add("MODEL_DESCRIPTION_FAILED", "/input", error.getMessage());
        }
        catch (const std::exception &error)
        {
            diagnostics.add("MODEL_DESCRIPTION_FAILED", "/input", QString::fromUtf8(error.what()));
        }
    }
    else
    {
        diagnostics.add("INVALID_RAW_MODEL", "/input", readError);
    }

    QByteArray after;
    try
    {
        after = fileSha256(canonicalPath);
    }
    catch (const std::exception &error)
    {
        diagnostics.add("INPUT_READ_FAILED", "/input", QString::fromUtf8(error.what()));
        return envelope(QStringLiteral("rejected"), sourceSha, diagnostics.toJson());
    }
    if (after != before)
    {
        diagnostics.add("SOURCE_CHANGED_DURING_PREFLIGHT", "/input",
                        "Input SHA-256 changed while the model was being described.");
    }
    if (!diagnostics.empty())
    {
        QJsonObject rejected = envelope(QStringLiteral("rejected"), sourceSha, diagnostics.toJson());
        return rejected;
    }
    description.insert("sourceSha256", sourceSha);
    return description;
}

QJsonObject prepareSource(const QString &inputPath,
                          const QString &expectedSha256,
                          const QString &outputPath,
                          uint steps,
                          uint outputFrequency,
                          double timeStep,
                          bool passive)
{
    QString sourceSha;
    QJsonArray diagnostics;
    const auto rejected = [&](const QString &code, const QString &path, const QString &message) {
        QJsonArray items = diagnostics;
        if (!code.isEmpty())
            items.append(QJsonObject{{"code", code}, {"severity", "error"},
                                     {"message", message}, {"path", path}});
        return QJsonObject{{"apiVersion", "range.workflow.native/v1"},
                           {"status", "rejected"}, {"operation", "prepare-source"},
                           {"sourceSha256", sourceSha}, {"diagnostics", items}};
    };

    if (steps < 1 || steps > 5000)
        return rejected("INVALID_STEP_COUNT", "/steps", "steps must be an integer in 1..5000.");
    if (outputFrequency < 1 || outputFrequency > steps)
        return rejected("INVALID_OUTPUT_FREQUENCY", "/outputFrequency", "outputFrequency must be an integer in 1..steps.");
    if (!std::isfinite(timeStep) || timeStep <= 0.0 || timeStep > 1.0)
        return rejected("INVALID_TIME_STEP", "/dt", "dt must be in (0,1] seconds.");

    const QJsonObject sourceDescription = describe(inputPath, expectedSha256);
    sourceSha = sourceDescription.value("sourceSha256").toString();
    if (sourceDescription.value("status").toString() != QStringLiteral("supported"))
    {
        diagnostics = sourceDescription.value("diagnostics").toArray();
        return rejected({}, {}, {});
    }
    const QString modelKind = sourceDescription.value("modelKind").toString();
    if (modelKind != QStringLiteral("four-rod") && modelKind != QStringLiteral("cantilever-beam"))
        return rejected("INVALID_PREFLIGHT_RESULT", "/modelKind", "Preflight did not return an accepted WF-03 model kind.");

    const QFileInfo sourceInfo(inputPath);
    if (!sourceInfo.isFile() || sourceInfo.isSymLink() || sourceInfo.canonicalFilePath().isEmpty())
        return rejected("INVALID_INPUT_FILE", "/input", "Input must remain a regular raw native MODEL file.");
    const QString canonicalInput = sourceInfo.canonicalFilePath();

    QFileInfo targetInfo(outputPath);
    const QString outputParent = targetInfo.dir().canonicalPath();
    if (outputParent.isEmpty() || targetInfo.fileName().isEmpty() || targetInfo.isSymLink())
        return rejected("INVALID_OUTPUT_PATH", "/output", "Output must be a new file in an existing directory.");
    const QString output = QDir(outputParent).filePath(targetInfo.fileName());
    if (output.compare(canonicalInput, Qt::CaseInsensitive) == 0 || QFileInfo::exists(output))
        return rejected("OUTPUT_NOT_DISTINCT", "/output", "Output must be new and distinct from its source.");
    if (targetInfo.suffix() != QStringLiteral("ram") && targetInfo.suffix() != QStringLiteral("rbm"))
        return rejected("INVALID_OUTPUT_EXTENSION", "/output", "Output must use the .ram or .rbm extension.");

    QLockFile lock(output + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return rejected("OUTPUT_BUSY", "/output", "Output is busy.");
    QTemporaryDir temporary(QDir(outputParent).filePath(QStringLiteral(".range-workflow-XXXXXX")));
    if (!temporary.isValid())
        return rejected("STAGING_FAILED", "/output", "Cannot create a private staging directory.");

    QByteArray sourceHashBefore;
    try
    {
        sourceHashBefore = fileSha256(canonicalInput);
    }
    catch (const std::exception &error)
    {
        return rejected("INPUT_READ_FAILED", "/input", QString::fromUtf8(error.what()));
    }
    if (QString::fromLatin1(sourceHashBefore).compare(expectedSha256, Qt::CaseInsensitive) != 0)
        return rejected("SOURCE_SHA256_MISMATCH", "/expectedSha256", "Input changed after successful preflight.");

    RModel model;
    QString readError;
    if (!readRawModel(canonicalInput, model, readError))
        return rejected("INVALID_RAW_MODEL", "/input", readError);
    QByteArray sourceHashAfterRead;
    try
    {
        sourceHashAfterRead = fileSha256(canonicalInput);
    }
    catch (const std::exception &error)
    {
        return rejected("INPUT_READ_FAILED", "/input", QString::fromUtf8(error.what()));
    }
    if (sourceHashAfterRead != sourceHashBefore)
        return rejected("SOURCE_CHANGED_DURING_PREPARATION", "/input", "Input SHA-256 changed while copying the native model.");

    const QJsonObject forcePort = sourceDescription.value("ports").toObject().value("force").toObject();
    const QJsonArray direction = forcePort.value("globalDirection").toArray();
    const double physicalIdValue = forcePort.value("nodePhysicalId").toDouble(-1.0);
    if (direction.size() != 3 || !std::isfinite(physicalIdValue) || physicalIdValue <= 0.0 ||
        std::floor(physicalIdValue) != physicalIdValue || physicalIdValue > double(std::numeric_limits<uint>::max()))
        return rejected("INVALID_PREFLIGHT_PORT", "/ports/force", "Preflight returned an invalid physical tip port.");
    const uint tipPhysicalId = uint(physicalIdValue);
    const uint tipPosition = model.findNodePositionByPhysicalId(tipPhysicalId);
    if (tipPosition == RConstants::eod)
        return rejected("SOURCE_PORT_NODE_MISSING", "/ports/force/nodePhysicalId", "The source port node no longer exists.");
    if (model.getNodePhysicalId(tipPosition) != tipPhysicalId)
        return rejected("SOURCE_PORT_NODE_MISMATCH", "/ports/force/nodePhysicalId", "Physical ID resolution did not round-trip.");
    const RR3Vector directionVector(direction[0].toDouble(), direction[1].toDouble(), direction[2].toDouble());
    const RVariableType forceVariable = modelKind == QStringLiteral("four-rod")
        ? R_VARIABLE_FORCE_X : R_VARIABLE_FORCE_Y;
    if (RVariable::getUnits(forceVariable) != QStringLiteral("N"))
        return rejected("INVALID_FORCE_UNIT", "/ports/force/unit", "WF-03 force port must use newtons.");

    RStructuralInteractionSetup &interaction = model.getProblemSetup().getStructuralInteractionSetup();
    // Physical MPC/coupling/motion checks have already completed in describe().
    // Only the source's replaceable control catalogs are discarded here.
    interaction.getActuators().clear();
    interaction.getSensors().clear();

    RStructuralActuator actuator;
    actuator.id = QStringLiteral("source-tip-force");
    actuator.name = actuator.id;
    actuator.target.scope = RStructuralTargetScope::Node;
    // The current native interaction API stores node positions in nodeId;
    // the source physical ID is retained in the returned mapping evidence.
    actuator.target.nodeId = tipPosition;
    actuator.type = RStructuralActuatorType::NodalForce;
    actuator.variable = forceVariable;
    actuator.frame = RStructuralFrame::Global;
    actuator.direction = directionVector;
    actuator.unit = RVariable::getUnits(forceVariable);
    actuator.minimum = kSourceForceMinimum;
    actuator.maximum = kSourceForceMaximum;
    interaction.getActuators().push_back(actuator);

    const struct
    {
        const char *id;
        RStructuralSensorQuantity quantity;
    } sensorDefinitions[] = {
        {"source-tip-displacement", RStructuralSensorQuantity::Displacement},
        {"source-tip-velocity", RStructuralSensorQuantity::Velocity},
        {"source-tip-acceleration", RStructuralSensorQuantity::Acceleration}
    };
    for (const auto &definition : sensorDefinitions)
    {
        RStructuralSensor sensor;
        sensor.id = QString::fromLatin1(definition.id);
        sensor.name = sensor.id;
        sensor.target.scope = RStructuralTargetScope::Node;
        sensor.target.nodeId = tipPosition;
        sensor.quantity = definition.quantity;
        sensor.aggregation = RStructuralSensorAggregation::Point;
        sensor.frame = RStructuralFrame::Global;
        sensor.direction = directionVector;
        sensor.unit = RStructuralInteractionSetup::getSensorUnit(sensor.quantity);
        interaction.getSensors().push_back(sensor);
    }

    const quint64 sourceSeed = model.getProblemSetup().getStructuralControlSetup().deterministicSeed;
    if (passive)
    {
        interaction.getActuators().clear();
        interaction.getSensors().clear();
        model.getProblemSetup().setStructuralControlSetup(RStructuralControlSetup());
    }
    else
    {
        RStructuralControlSetup control;
        control.enabled = true;
        control.controlProtocolVersion = 1;
        control.controllerId = QStringLiteral("stage9-velocity-feedback");
        control.deterministicSeed = sourceSeed;
        control.channelConditioning.clear();
        try
        {
            control.validate();
            model.getProblemSetup().setStructuralControlSetup(control);
        }
        catch (const RError &error)
        {
            return rejected("INVALID_PREPARED_CONTROL", "/originalControl", error.getMessage());
        }
    }

    auto &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setTimeStep(timeStep);
    dynamics.setNSteps(steps);
    dynamics.setOutputFrequency(outputFrequency);
    try
    {
        dynamics.validate();
    }
    catch (const RError &error)
    {
        return rejected("INVALID_PREPARED_DYNAMICS", "/dynamics", error.getMessage());
    }

    const auto validation = interaction.validate(model);
    if (validation.hasErrors())
    {
        for (const auto &item : validation.getDiagnostics())
        {
            if (item.severity == RStructuralDiagnosticSeverity::Error)
                diagnostics.append(QJsonObject{{"code", QStringLiteral("PREPARED_INTERACTION_") + item.code},
                                               {"severity", "error"}, {"message", item.message},
                                               {"path", QStringLiteral("/interactions/") + item.objectId}});
        }
        return rejected({}, {}, {});
    }

    const QString stagedPath = model.write(temporary.filePath(QStringLiteral("model.") + targetInfo.suffix()), false);
    QByteArray preparedHash;
    try
    {
        preparedHash = fileSha256(stagedPath);
    }
    catch (const std::exception &error)
    {
        return rejected("STAGED_MODEL_READ_FAILED", "/output", QString::fromUtf8(error.what()));
    }

    RModel preparedModel;
    if (!readRawModel(stagedPath, preparedModel, readError))
        return rejected("STAGED_MODEL_INVALID", "/output", readError);
    const QJsonObject preparedDescription = describe(stagedPath, QString::fromLatin1(preparedHash));
    if (preparedDescription.value("status").toString() != QStringLiteral("supported"))
    {
        diagnostics = preparedDescription.value("diagnostics").toArray();
        for (qsizetype index = 0; index < diagnostics.size(); ++index)
        {
            if (diagnostics.at(index).isObject())
            {
                auto item = diagnostics.at(index).toObject();
                item.insert("path", QStringLiteral("/prepared") + item.value("path").toString());
                diagnostics.replace(index, item);
            }
        }
        return rejected({}, {}, {});
    }

    const auto &preparedInteraction = preparedModel.getProblemSetup().getStructuralInteractionSetup();
    QJsonArray actuatorJson;
    for (const auto &saved : preparedInteraction.getActuators())
    {
        if (saved.minimum != forcePort.value("minimum").toDouble() ||
            saved.maximum != forcePort.value("maximum").toDouble())
            return rejected("PREPARED_FORCE_BOUNDS_MISMATCH", "/mapping/actuators",
                            "Saved actuator bounds differ from the preflight force port bounds.");
        actuatorJson.append(QJsonObject{
            {"id", saved.id}, {"kind", "nodal_force"},
            {"targetNodePosition", int(saved.target.nodeId)},
            {"targetNodePhysicalId", jsonPhysicalId(preparedModel.getNodePhysicalId(saved.target.nodeId))},
            {"variable", RVariable::getId(saved.variable)},
            {"globalDirection", QJsonArray{saved.direction[0], saved.direction[1], saved.direction[2]}},
            {"unit", saved.unit}, {"minimum", saved.minimum}, {"maximum", saved.maximum}});
    }
    QJsonArray sensorJson;
    for (const auto &saved : preparedInteraction.getSensors())
    {
        QString quantity;
        switch (saved.quantity)
        {
            case RStructuralSensorQuantity::Displacement: quantity = QStringLiteral("displacement"); break;
            case RStructuralSensorQuantity::Velocity: quantity = QStringLiteral("velocity"); break;
            case RStructuralSensorQuantity::Acceleration: quantity = QStringLiteral("acceleration"); break;
            default: quantity = QStringLiteral("unsupported"); break;
        }
        sensorJson.append(QJsonObject{
            {"id", saved.id}, {"quantity", quantity},
            {"targetNodePosition", int(saved.target.nodeId)},
            {"targetNodePhysicalId", jsonPhysicalId(preparedModel.getNodePhysicalId(saved.target.nodeId))},
            {"globalDirection", QJsonArray{saved.direction[0], saved.direction[1], saved.direction[2]}},
            {"unit", saved.unit}});
    }
    const auto &preparedControl = preparedModel.getProblemSetup().getStructuralControlSetup();
    const QJsonObject mapping{{"actuators", actuatorJson}, {"sensors", sensorJson},
                              {"controller", QJsonObject{{"enabled", preparedControl.enabled},
                                  {"controlProtocolVersion", int(preparedControl.controlProtocolVersion)},
                                  {"controllerId", preparedControl.controllerId},
                                  {"deterministicSeed", QString::number(preparedControl.deterministicSeed)},
                                  {"conditioningCount", int(preparedControl.channelConditioning.size())}}}};

    QFile staged(stagedPath);
    if (!staged.open(QIODevice::ReadOnly))
        return rejected("STAGED_MODEL_READ_FAILED", "/output", "Cannot read staged native model.");
    QSaveFile published(output);
    if (!published.open(QIODevice::WriteOnly))
        return rejected("OUTPUT_WRITE_FAILED", "/output", "Cannot stage output file.");
    while (!staged.atEnd())
    {
        const QByteArray data = staged.read(1024 * 1024);
        if (data.isEmpty() || published.write(data) != data.size())
            return rejected("OUTPUT_WRITE_FAILED", "/output", "Cannot copy staged model to output.");
    }

    QByteArray finalSourceHash;
    try
    {
        finalSourceHash = fileSha256(canonicalInput);
    }
    catch (const std::exception &error)
    {
        return rejected("INPUT_READ_FAILED", "/input", QString::fromUtf8(error.what()));
    }
    if (finalSourceHash != sourceHashBefore)
        return rejected("SOURCE_CHANGED_DURING_PREPARATION", "/input", "Input SHA-256 changed before publication.");
    if (QFileInfo::exists(output))
        return rejected("OUTPUT_NOT_DISTINCT", "/output", "Output appeared before publication.");
    if (!published.commit())
        return rejected("OUTPUT_PUBLISH_FAILED", "/output", "Cannot publish prepared native model.");

    return {{"apiVersion", "range.workflow.native/v1"}, {"status", "published"},
            {"operation", "prepare-source"}, {"output", output},
            {"sourcePath", canonicalInput}, {"sourceFiles", QJsonArray{QJsonObject{
                {"path", canonicalInput}, {"sha256", sourceSha}}}}, {"resume", false},
            {"sourceSha256", sourceSha}, {"preparedSha256", QString::fromLatin1(preparedHash)},
            {"sha256", QString::fromLatin1(preparedHash)}, {"modelKind", modelKind},
            {"steps", int(steps)}, {"outputFrequency", int(outputFrequency)},
            {"dt", timeStep}, {"passive", passive}, {"mapping", mapping},
            {"diagnostics", QJsonArray{}}};
}
} // namespace RangeNativeImport
