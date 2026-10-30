#include "rtrusted_mechanics_service.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <QVector>

#include <rmodel_command_service.h>
#include <rml_boundary_condition.h>
#include <rml_condition_component.h>
#include <rml_element_descriptor.h>
#include <rml_initial_condition.h>
#include <rml_material.h>
#include <rml_material_property.h>
#include <rml_modal_setup.h>
#include <rml_point.h>
#include <rml_problem_setup.h>
#include <rml_problem_task_item.h>
#include <rml_problem_type.h>
#include <rml_surface.h>
#include <rml_structural_dynamics_setup.h>
#include <rml_variable.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
enum class Dimension { Length, Area, Inertia, SectionModulus, Force, Pressure,
                       Density, Time, Frequency, Velocity, Rotation, AngularVelocity };

struct UnitScale { Dimension dimension; double scale; };

const QMap<QString, UnitScale> &unitScales()
{
    static const QMap<QString, UnitScale> scales = {
        {"m", {Dimension::Length, 1.0}}, {"mm", {Dimension::Length, 1.0e-3}},
        {"cm", {Dimension::Length, 1.0e-2}}, {"km", {Dimension::Length, 1.0e3}},
        {"in", {Dimension::Length, 0.0254}}, {"ft", {Dimension::Length, 0.3048}},
        {"m2", {Dimension::Area, 1.0}}, {"m^2", {Dimension::Area, 1.0}},
        {"mm2", {Dimension::Area, 1.0e-6}}, {"mm^2", {Dimension::Area, 1.0e-6}},
        {"cm2", {Dimension::Area, 1.0e-4}}, {"cm^2", {Dimension::Area, 1.0e-4}},
        {"m4", {Dimension::Inertia, 1.0}}, {"m^4", {Dimension::Inertia, 1.0}},
        {"mm4", {Dimension::Inertia, 1.0e-12}}, {"mm^4", {Dimension::Inertia, 1.0e-12}},
        {"m3", {Dimension::SectionModulus, 1.0}}, {"m^3", {Dimension::SectionModulus, 1.0}},
        {"mm3", {Dimension::SectionModulus, 1.0e-9}}, {"mm^3", {Dimension::SectionModulus, 1.0e-9}},
        {"N", {Dimension::Force, 1.0}}, {"kN", {Dimension::Force, 1.0e3}},
        {"Pa", {Dimension::Pressure, 1.0}}, {"kPa", {Dimension::Pressure, 1.0e3}},
        {"MPa", {Dimension::Pressure, 1.0e6}}, {"GPa", {Dimension::Pressure, 1.0e9}},
        {"kg/m3", {Dimension::Density, 1.0}}, {"kg/m^3", {Dimension::Density, 1.0}},
        {"g/cm3", {Dimension::Density, 1.0e3}}, {"g/cm^3", {Dimension::Density, 1.0e3}},
        {"s", {Dimension::Time, 1.0}}, {"ms", {Dimension::Time, 1.0e-3}},
        {"Hz", {Dimension::Frequency, 1.0}}, {"1/s", {Dimension::Frequency, 1.0}},
        {"s^-1", {Dimension::Frequency, 1.0}},
        {"m/s", {Dimension::Velocity, 1.0}}, {"mm/s", {Dimension::Velocity, 1.0e-3}},
        {"rad", {Dimension::Rotation, 1.0}}, {"deg", {Dimension::Rotation, std::acos(-1.0) / 180.0}},
        {"rad/s", {Dimension::AngularVelocity, 1.0}},
        {"deg/s", {Dimension::AngularVelocity, std::acos(-1.0) / 180.0}}
    };
    return scales;
}

void addIssue(QJsonArray &issues, const QString &code, const QString &path,
              const QString &message, const QString &severity = QStringLiteral("error"),
              const QJsonObject &evidence = {})
{
    issues.append(QJsonObject{{"code", code}, {"path", path},
                              {"severity", severity}, {"message", message}, {"evidence", evidence}});
}

QString dimensionName(Dimension dimension)
{
    switch (dimension) {
    case Dimension::Length: return QStringLiteral("length");
    case Dimension::Area: return QStringLiteral("area");
    case Dimension::Inertia: return QStringLiteral("area_moment_of_inertia");
    case Dimension::SectionModulus: return QStringLiteral("section_modulus");
    case Dimension::Force: return QStringLiteral("force");
    case Dimension::Pressure: return QStringLiteral("pressure");
    case Dimension::Density: return QStringLiteral("density");
    case Dimension::Time: return QStringLiteral("time");
    case Dimension::Frequency: return QStringLiteral("frequency");
    case Dimension::Velocity: return QStringLiteral("velocity");
    case Dimension::Rotation: return QStringLiteral("rotation");
    case Dimension::AngularVelocity: return QStringLiteral("angular_velocity");
    }
    return QStringLiteral("unknown");
}

void rejectUnknownFields(const QJsonObject &object, const QSet<QString> &allowed,
                        const QString &path, QJsonArray &issues)
{
    for (const QString &key : object.keys()) {
        if (!allowed.contains(key))
            addIssue(issues, QStringLiteral("SPEC_FIELD_UNSUPPORTED"), path + "." + key,
                     QStringLiteral("Field '%1' is outside the declared mechanics schema.").arg(key),
                     QStringLiteral("error"), QJsonObject{{"field", key}});
    }
}

bool quantity(const QJsonValue &value, Dimension expected, const QString &path,
              QJsonArray &issues, double &si)
{
    if (!value.isObject()) {
        addIssue(issues, QStringLiteral("SPEC_MISSING_QUANTITY"), path,
                 QStringLiteral("Expected an object with numeric value and explicit unit."),
                 QStringLiteral("error"), QJsonObject{{"expectedDimension", dimensionName(expected)}});
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!object.value("value").isDouble()) {
        addIssue(issues, QStringLiteral("SPEC_INVALID_QUANTITY"), path,
                 QStringLiteral("Quantity value must be numeric."), QStringLiteral("error"),
                 QJsonObject{{"expectedDimension", dimensionName(expected)}, {"observedValue", object.value("value")}});
        return false;
    }
    const double raw = object.value("value").toDouble();
    if (!std::isfinite(raw)) {
        addIssue(issues, QStringLiteral("SPEC_NONFINITE_QUANTITY"), path,
                 QStringLiteral("Quantity must be finite."), QStringLiteral("error"),
                 QJsonObject{{"expectedDimension", dimensionName(expected)}, {"observedValue", raw}});
        return false;
    }
    const QString unit = object.value("unit").toString();
    if (unit.isEmpty()) {
        addIssue(issues, QStringLiteral("UNIT_MISSING"), path,
                 QStringLiteral("An explicit unit is required."), QStringLiteral("error"),
                 QJsonObject{{"expectedDimension", dimensionName(expected)}, {"observedValue", raw}});
        return false;
    }
    const auto it = unitScales().constFind(unit);
    if (it == unitScales().cend()) {
        addIssue(issues, QStringLiteral("UNIT_UNSUPPORTED"), path,
                 QStringLiteral("Unit '%1' is not in the supported SI conversion table.").arg(unit),
                 QStringLiteral("error"), QJsonObject{{"expectedDimension", dimensionName(expected)},
                                                       {"observedUnit", unit}, {"observedValue", raw}});
        return false;
    }
    if (it->dimension != expected) {
        addIssue(issues, QStringLiteral("UNIT_DIMENSION_MISMATCH"), path,
                 QStringLiteral("Unit '%1' has the wrong physical dimension.").arg(unit),
                 QStringLiteral("error"), QJsonObject{{"expectedDimension", dimensionName(expected)},
                                                       {"observedDimension", dimensionName(it->dimension)},
                                                       {"observedUnit", unit}, {"observedValue", raw}});
        return false;
    }
    si = raw * it->scale;
    if (!std::isfinite(si)) {
        addIssue(issues, QStringLiteral("SPEC_NONFINITE_QUANTITY"), path,
                 QStringLiteral("SI-normalized quantity is not finite."), QStringLiteral("error"),
                 QJsonObject{{"expectedDimension", dimensionName(expected)}, {"observedUnit", unit},
                             {"observedValue", raw}, {"normalizedValue", si}});
        return false;
    }
    return true;
}

bool quantityMember(const QJsonObject &object, const QString &key, Dimension dimension,
                    const QString &path, QJsonArray &issues, QJsonObject &normalized,
                    bool strictlyPositive = false)
{
    double value = 0.0;
    const bool ok = quantity(object.value(key), dimension, path + "." + key, issues, value);
    if (!ok) return false;
    if (strictlyPositive && value <= 0.0) {
        addIssue(issues, QStringLiteral("SPEC_NONPOSITIVE_QUANTITY"), path + "." + key,
                 QStringLiteral("Quantity must be greater than zero."), QStringLiteral("error"),
                 QJsonObject{{"normalizedSIValue", value}});
        return false;
    }
    normalized.insert(key, value);
    return true;
}

bool normalizeSelector(const QJsonObject &raw, const QString &path, QJsonArray &issues,
                       QJsonObject &normalized)
{
    rejectUnknownFields(raw, {"kind", "x", "tolerance"}, path, issues);
    const QString kind = raw.value("kind").toString();
    static const QSet<QString> supported = {"fixedEnd", "freeEnd", "allNodes", "position"};
    if (!supported.contains(kind)) {
        addIssue(issues, QStringLiteral("SELECTOR_UNSUPPORTED"), path + ".kind",
                 QStringLiteral("Supported selectors are fixedEnd, freeEnd, allNodes, and position."));
        return false;
    }
    if (kind != QLatin1String("position") && (raw.contains("x") || raw.contains("tolerance"))) {
        addIssue(issues, QStringLiteral("SELECTOR_FIELD_NOT_APPLICABLE"), path,
                 QStringLiteral("Coordinate and tolerance fields apply only to position selectors."));
        return false;
    }
    normalized.insert("kind", kind);
    if (kind == QLatin1String("position")) {
        double x = 0.0;
        if (!quantity(raw.value("x"), Dimension::Length, path + ".x", issues, x)) return false;
        normalized.insert("x", x);
        double tolerance = 0.0;
        if (raw.contains("tolerance")) {
            if (!quantity(raw.value("tolerance"), Dimension::Length, path + ".tolerance", issues, tolerance)) return false;
        }
        if (raw.contains("tolerance")) {
            if (tolerance < 0.0) {
                addIssue(issues, QStringLiteral("SELECTOR_TOLERANCE_INVALID"), path + ".tolerance",
                         QStringLiteral("Selector tolerance must be nonnegative."));
                return false;
            }
            normalized.insert("tolerance", tolerance);
        }
    }
    return true;
}

QJsonArray dofArray(const QJsonValue &value, const QString &path, QJsonArray &issues)
{
    QJsonArray result;
    if (!value.isArray()) {
        addIssue(issues, QStringLiteral("SPEC_MISSING_DOFS"), path,
                 QStringLiteral("Support dofs must be an array."));
        return result;
    }
    static const QSet<QString> valid = {"ux", "uy", "uz", "rx", "ry", "rz"};
    QSet<QString> seen;
    for (const QJsonValue &entry : value.toArray()) {
        const QString dof = entry.toString();
        if (!valid.contains(dof)) {
            addIssue(issues, QStringLiteral("DOF_UNSUPPORTED"), path,
                     QStringLiteral("Unsupported degree of freedom '%1'.").arg(dof));
            continue;
        }
        if (seen.contains(dof)) {
            addIssue(issues, QStringLiteral("DOF_DUPLICATE"), path,
                     QStringLiteral("Degree of freedom '%1' is duplicated.").arg(dof));
            continue;
        }
        seen.insert(dof);
        result.append(dof);
    }
    return result;
}

QJsonArray normalizeVector(const QJsonValue &value, Dimension dimension,
                           const QString &path, QJsonArray &issues)
{
    QJsonArray result;
    if (!value.isArray() || value.toArray().size() != 3) {
        addIssue(issues, QStringLiteral("SPEC_INVALID_VECTOR"), path,
                 QStringLiteral("Expected exactly three explicitly-unitized components."));
        return result;
    }
    const QJsonArray input = value.toArray();
    for (int i = 0; i < 3; ++i) {
        double si = 0.0;
        if (quantity(input.at(i), dimension, path + QStringLiteral("[%1]").arg(i), issues, si))
            result.append(si);
    }
    return result;
}

RVariableType dofVariable(const QString &dof)
{
    if (dof == QLatin1String("ux")) return R_VARIABLE_DISPLACEMENT_X;
    if (dof == QLatin1String("uy")) return R_VARIABLE_DISPLACEMENT_Y;
    if (dof == QLatin1String("uz")) return R_VARIABLE_DISPLACEMENT_Z;
    if (dof == QLatin1String("rx")) return R_VARIABLE_ROTATION_X;
    if (dof == QLatin1String("ry")) return R_VARIABLE_ROTATION_Y;
    return R_VARIABLE_ROTATION_Z;
}

QString dofName(RVariableType type)
{
    switch (type) {
    case R_VARIABLE_DISPLACEMENT_X: return QStringLiteral("ux");
    case R_VARIABLE_DISPLACEMENT_Y: return QStringLiteral("uy");
    case R_VARIABLE_DISPLACEMENT_Z: return QStringLiteral("uz");
    case R_VARIABLE_ROTATION_X: return QStringLiteral("rx");
    case R_VARIABLE_ROTATION_Y: return QStringLiteral("ry");
    case R_VARIABLE_ROTATION_Z: return QStringLiteral("rz");
    default: return QStringLiteral("unknown");
    }
}

void addDisplacementCondition(RElementGroup &group, const QJsonArray &dofs)
{
    QSet<QString> active;
    for (const auto &entry : dofs) active.insert(entry.toString());
    RBoundaryCondition displacement(R_BOUNDARY_CONDITION_DISPLACEMENT);
    const QList<RVariableType> translations = {R_VARIABLE_DISPLACEMENT_X, R_VARIABLE_DISPLACEMENT_Y,
                                               R_VARIABLE_DISPLACEMENT_Z};
    for (RVariableType variable : translations) {
        RConditionComponent &component = displacement.getComponent(displacement.findComponentPosition(variable));
        if (active.contains(dofName(variable))) component.add(0.0, 0.0);
        else component.setEnabled(false);
    }
    group.addBoundaryCondition(displacement);

    const QList<RVariableType> rotations = {R_VARIABLE_ROTATION_X, R_VARIABLE_ROTATION_Y,
                                            R_VARIABLE_ROTATION_Z};
    const bool hasRotation = active.contains(QStringLiteral("rx"))
        || active.contains(QStringLiteral("ry")) || active.contains(QStringLiteral("rz"));
    if (hasRotation) {
        RBoundaryCondition rotation(R_BOUNDARY_CONDITION_ROTATION);
        for (RVariableType variable : rotations) {
            RConditionComponent &component = rotation.getComponent(rotation.findComponentPosition(variable));
            if (active.contains(dofName(variable))) component.add(0.0, 0.0);
            else component.setEnabled(false);
        }
        group.addBoundaryCondition(rotation);
    }
}

void addPointGroup(RModel &model, uint nodeIndex, const QString &name,
                   const RMaterial &material, uint &pointGroupIndex)
{
    RElement pointElement(R_ELEMENT_POINT);
    pointElement.setNodeId(0, nodeIndex);
    model.addElement(pointElement, true, pointGroupIndex);
    RPoint &point = model.getPoint(pointGroupIndex++);
    point.setName(name);
    point.setVolume(0.0);
    point.setMaterial(material);
}

QJsonObject canonicalObject(const QJsonObject &input)
{
    QStringList keys = input.keys();
    std::sort(keys.begin(), keys.end());
    QJsonObject result;
    for (const QString &key : keys) {
        const QJsonValue value = input.value(key);
        if (value.isObject()) result.insert(key, canonicalObject(value.toObject()));
        else if (value.isArray()) {
            QJsonArray array;
            for (const QJsonValue &entry : value.toArray())
                array.append(entry.isObject() ? QJsonValue(canonicalObject(entry.toObject())) : entry);
            result.insert(key, array);
        } else result.insert(key, value);
    }
    return result;
}

QJsonObject conditionSummary(const RCondition &condition)
{
    QJsonArray components;
    for (uint i = 0; i < condition.size(); ++i) {
        const RConditionComponent &component = condition.getComponent(i);
        QJsonArray samples;
        for (uint j = 0; j < component.size(); ++j) {
            const double time = component.getKey(j);
            const double value = component.getValue(j);
            samples.append(QJsonObject{{"time_s", std::isfinite(time) ? QJsonValue(time) : QJsonValue(QJsonValue::Null)},
                                       {"timeKeyFinite", std::isfinite(time)},
                                       {"value", std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null)},
                                       {"valueFinite", std::isfinite(value)}});
        }
        components.append(QJsonObject{{"variable", int(component.getType())},
                                      {"enabled", component.getEnabled()},
                                      {"unit", component.getUnits()}, {"samples", samples}});
    }
    return QJsonObject{{"name", condition.getName()}, {"components", components}};
}

QJsonArray boundarySummary(const RElementGroup &group)
{
    QJsonArray result;
    for (uint i = 0; i < group.getNBoundaryConditions(); ++i) {
        const RBoundaryCondition &condition = group.getBoundaryCondition(i);
        QJsonObject summary = conditionSummary(condition);
        summary.insert("type", int(condition.getType()));
        summary.insert("typeName", RBoundaryCondition::getName(condition.getType()));
        const RR3Vector &direction = condition.getLocalDirection();
        summary.insert("hasLocalDirection", condition.getHasLocalDirection());
        summary.insert("explicitLocalDirection", condition.getExplicitLocalDirection());
        summary.insert("localDirection", QJsonArray{direction[0], direction[1], direction[2]});
        result.append(summary);
    }
    return result;
}

QJsonArray initialConditionSummary(const RElementGroup &group)
{
    QJsonArray result;
    for (uint i = 0; i < group.getNInitialConditions(); ++i) {
        const RInitialCondition &condition = group.getInitialCondition(i);
        QJsonObject summary = conditionSummary(condition);
        summary.insert("type", int(condition.getType()));
        summary.insert("typeName", RInitialCondition::getName(condition.getType()));
        result.append(summary);
    }
    return result;
}

QJsonArray environmentConditionSummary(const RElementGroup &group)
{
    QJsonArray result;
    for (uint i = 0; i < group.getNEnvironmentConditions(); ++i) {
        const REnvironmentCondition &condition = group.getEnvironmentCondition(i);
        QJsonObject summary = conditionSummary(condition);
        summary.insert("type", int(condition.getType()));
        summary.insert("typeName", REnvironmentCondition::getName(condition.getType()));
        result.append(summary);
    }
    return result;
}

QJsonObject groupSummary(const RElementGroup &group)
{
    QJsonArray elements;
    for (uint i = 0; i < group.size(); ++i) elements.append(double(group.get(i)));
    return QJsonObject{{"physicalId", double(group.getPhysicalId())}, {"name", group.getName()},
                       {"elements", elements}, {"material", group.getMaterial().toJson()},
                       {"boundaryConditions", boundarySummary(group)},
                       {"environmentConditions", environmentConditionSummary(group)},
                       {"initialConditions", initialConditionSummary(group)}};
}

void disableEnvironmentConditions(RElementGroup &group)
{
    for (uint i = 0; i < group.getNEnvironmentConditions(); ++i) {
        REnvironmentCondition &condition = group.getEnvironmentCondition(i);
        for (uint j = 0; j < condition.size(); ++j)
            condition.getComponent(j).setEnabled(false);
    }
}

void addDiagnostic(QJsonArray &issues, const QString &code, const QString &severity,
                   const QString &message, const QJsonObject &evidence,
                   const QJsonArray &causeCandidates, const QJsonArray &allowedOperations,
                   bool requiresConfirmation, const QJsonArray &objects = {})
{
    issues.append(QJsonObject{{"code", code}, {"severity", severity}, {"message", message},
                              {"evidence", evidence}, {"objects", objects},
                              {"causeCandidates", causeCandidates},
                              {"allowedOperations", allowedOperations},
                              {"requiresUserConfirmation", requiresConfirmation}});
}

bool samePhysicalNumber(double actual, double expected)
{
    return std::isfinite(actual) && std::isfinite(expected)
        && std::abs(actual - expected) <= 1.0e-10 * std::max({std::abs(actual), std::abs(expected), 1.0e-30});
}

QString supportDofName(RVariableType type)
{
    switch (type) {
    case R_VARIABLE_DISPLACEMENT_X: return QStringLiteral("ux");
    case R_VARIABLE_DISPLACEMENT_Y: return QStringLiteral("uy");
    case R_VARIABLE_DISPLACEMENT_Z: return QStringLiteral("uz");
    case R_VARIABLE_ROTATION_X: return QStringLiteral("rx");
    case R_VARIABLE_ROTATION_Y: return QStringLiteral("ry");
    case R_VARIABLE_ROTATION_Z: return QStringLiteral("rz");
    default: return {};
    }
}

using NodeSupportMap = QMap<uint, QSet<QString>>;
using NodeSignatureMap = QMap<uint, QStringList>;

bool equivalentLoadNumber(double a, double b)
{
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    const double scale = std::max(std::abs(a), std::abs(b));
    return scale == 0.0 ? a == b
                        : std::abs(a - b) <= 64.0 * std::numeric_limits<double>::epsilon() * scale;
}

bool equivalentForceSignature(const QJsonObject &a, const QJsonObject &b)
{
    if (a.value("kind") != b.value("kind")) return false;
    if (a.value("componentMetadata") != b.value("componentMetadata")) return false;
    const QJsonArray ah = a.value("history").toArray(), bh = b.value("history").toArray();
    if (ah.size() != bh.size()) return false;
    for (int i = 0; i < ah.size(); ++i) {
        const QJsonObject ap = ah.at(i).toObject(), bp = bh.at(i).toObject();
        const QJsonValue at = ap.value("time"), bt = bp.value("time");
        if (!at.isDouble() || !bt.isDouble()
            || !equivalentLoadNumber(at.toDouble(), bt.toDouble())) return false;
        const QJsonArray ac = ap.value("components").toArray(), bc = bp.value("components").toArray();
        if (ac.size() != bc.size()) return false;
        for (int axis = 0; axis < ac.size(); ++axis) {
            const QJsonValue av = ac.at(axis), bv = bc.at(axis);
            if (!av.isDouble() || !bv.isDouble()
                || !equivalentLoadNumber(av.toDouble(), bv.toDouble())) return false;
        }
    }
    return true;
}

bool equivalentLoadMaps(const NodeSignatureMap &expected, const NodeSignatureMap &observed)
{
    if (expected.keys() != observed.keys()) return false;
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const auto actualIt = observed.constFind(it.key());
        if (actualIt == observed.cend() || actualIt.value().size() != it.value().size()) return false;
        QVector<bool> matched(actualIt.value().size(), false);
        for (const QString &encodedExpected : it.value()) {
            const auto expectedDocument = QJsonDocument::fromJson(encodedExpected.toUtf8());
            if (!expectedDocument.isObject()) return false;
            bool found = false;
            for (int index = 0; index < actualIt.value().size(); ++index) {
                if (matched[index]) continue;
                const auto actualDocument = QJsonDocument::fromJson(actualIt.value().at(index).toUtf8());
                if (actualDocument.isObject()
                    && equivalentForceSignature(expectedDocument.object(), actualDocument.object())) {
                    matched[index] = true;
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }
    return true;
}

QSet<uint> groupNodePositions(const RModel &model, const RElementGroup &group)
{
    QSet<uint> positions;
    for (uint i = 0; i < group.size(); ++i) {
        const uint elementIndex = group.get(i);
        if (elementIndex >= model.getNElements()) continue;
        const RElement &element = model.getElement(elementIndex);
        for (uint j = 0; j < element.size(); ++j)
            if (element.getNodeId(j) < model.getNNodes()) positions.insert(element.getNodeId(j));
    }
    return positions;
}

NodeSupportMap actualSupportMap(const RModel &model)
{
    NodeSupportMap constrained;
    auto applyGroup = [&](const RElementGroup &group) {
        const QSet<uint> nodePositions = groupNodePositions(model, group);
        for (uint i = 0; i < group.getNBoundaryConditions(); ++i) {
            const RBoundaryCondition &condition = group.getBoundaryCondition(i);
            if (condition.getType() != R_BOUNDARY_CONDITION_DISPLACEMENT
                && condition.getType() != R_BOUNDARY_CONDITION_ROTATION) continue;
            for (uint j = 0; j < condition.size(); ++j) {
                const RConditionComponent &component = condition.getComponent(j);
                const QString dof = supportDofName(component.getType());
                if (dof.isEmpty() || !component.getEnabled() || component.size() == 0) continue;
                for (uint node : nodePositions) constrained[node].insert(dof);
            }
        }
    };
    for (uint i = 0; i < model.getNLines(); ++i) applyGroup(model.getLine(i));
    for (uint i = 0; i < model.getNPoints(); ++i) applyGroup(model.getPoint(i));
    return constrained;
}

NodeSupportMap expectedSupportMap(const RModel &model, const QJsonObject &normalized,
                                  const QString &revision)
{
    NodeSupportMap expected;
    for (const QJsonValue &entry : normalized.value("supports").toArray()) {
        const QJsonObject support = entry.toObject();
        const QJsonObject binding = RTrustedMechanicsService::resolveSelector(
            model, support.value("selector").toObject(), revision, revision);
        if (!binding.value("valid").toBool()) continue;
        QSet<uint> nodes;
        for (const QJsonValue &value : binding.value("nodePositions").toArray())
            nodes.insert(uint(value.toInt()));
        for (const QString &dof : {QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                                   QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")}) {
            if (!support.value("dofs").toArray().contains(dof)) continue;
            for (uint node : nodes) expected[node].insert(dof);
        }
    }
    return expected;
}

QJsonObject supportMapJson(const NodeSupportMap &map)
{
    QJsonArray nodes;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        QStringList dofs = it.value().values();
        std::sort(dofs.begin(), dofs.end());
        QJsonArray dofArray;
        for (const QString &dof : dofs) dofArray.append(dof);
        nodes.append(QJsonObject{{"nodePosition", int(it.key())}, {"dofs", dofArray}});
    }
    return QJsonObject{{"nodes", nodes}};
}

QString canonicalJson(const QJsonValue &value)
{
    const QJsonObject wrapped = value.isObject() ? canonicalObject(value.toObject()) : QJsonObject{{"value", value}};
    return QString::fromUtf8(QJsonDocument(wrapped).toJson(QJsonDocument::Compact));
}

void appendSignature(NodeSignatureMap &map, uint node, const QJsonObject &signature)
{
    map[node].append(canonicalJson(signature));
}

QJsonObject nodeSignatureMapJson(NodeSignatureMap map)
{
    QJsonArray nodes;
    for (auto it = map.begin(); it != map.end(); ++it) {
        std::sort(it.value().begin(), it.value().end());
        QJsonArray signatures;
        for (const QString &encoded : it.value()) {
            const QJsonDocument document = QJsonDocument::fromJson(encoded.toUtf8());
            signatures.append(document.isObject() ? QJsonValue(document.object()) : QJsonValue(encoded));
        }
        nodes.append(QJsonObject{{"nodePosition", int(it.key())}, {"signatures", signatures}});
    }
    return QJsonObject{{"nodes", nodes}};
}

QString actualInitialType(RInitialConditionType type)
{
    switch (type) {
    case R_INITIAL_CONDITION_VELOCITY: return QStringLiteral("velocity");
    case R_INITIAL_CONDITION_ROTATION: return QStringLiteral("rotation");
    case R_INITIAL_CONDITION_ANGULAR_VELOCITY: return QStringLiteral("angularVelocity");
    case R_INITIAL_CONDITION_DISPLACEMENT: return QStringLiteral("displacement");
    default: return QStringLiteral("unsupported:%1").arg(int(type));
    }
}

QList<RVariableType> initialVariables(const QString &type)
{
    if (type == QLatin1String("velocity"))
        return {R_VARIABLE_VELOCITY_X, R_VARIABLE_VELOCITY_Y, R_VARIABLE_VELOCITY_Z};
    if (type == QLatin1String("rotation"))
        return {R_VARIABLE_ROTATION_X, R_VARIABLE_ROTATION_Y, R_VARIABLE_ROTATION_Z};
    if (type == QLatin1String("angularVelocity"))
        return {R_VARIABLE_ANGULAR_VELOCITY_X, R_VARIABLE_ANGULAR_VELOCITY_Y, R_VARIABLE_ANGULAR_VELOCITY_Z};
    return {R_VARIABLE_DISPLACEMENT_X, R_VARIABLE_DISPLACEMENT_Y, R_VARIABLE_DISPLACEMENT_Z};
}

QJsonObject boundaryConditionHistory(const RBoundaryCondition &condition)
{
    const QList<RVariableType> variables = {R_VARIABLE_FORCE_X, R_VARIABLE_FORCE_Y, R_VARIABLE_FORCE_Z};
    QJsonArray componentMetadata;
    const RConditionComponent *timeSource = nullptr;
    for (RVariableType variable : variables) {
        const unsigned int position = condition.findComponentPosition(variable);
        if (position == RConstants::eod) {
            componentMetadata.append(QJsonObject{{"variable", int(variable)}, {"missing", true}});
            continue;
        }
        const RConditionComponent &component = condition.getComponent(position);
        QJsonArray keys;
        for (uint sample = 0; sample < component.size(); ++sample)
            keys.append(component.getKey(sample));
        componentMetadata.append(QJsonObject{{"variable", int(component.getType())},
            {"enabled", component.getEnabled()}, {"unit", component.getUnits()},
            {"keyType", int(component.getKeyType())}, {"keyUnits", component.getKeyUnits()},
            {"timeKeys", keys}});
        if (variable == variables.first()) timeSource = &component;
    }

    QJsonArray history;
    if (timeSource) for (uint sample = 0; sample < timeSource->size(); ++sample) {
        QJsonArray components;
        for (RVariableType variable : variables) {
            const unsigned int position = condition.findComponentPosition(variable);
            if (position == RConstants::eod) {
                components.append(QJsonValue(QJsonValue::Null));
                continue;
            }
            const RConditionComponent &component = condition.getComponent(position);
            components.append(sample < component.size()
                ? QJsonValue(component.getValue(sample)) : QJsonValue(QJsonValue::Null));
        }
        history.append(QJsonObject{{"time", timeSource->getKey(sample)}, {"components", components}});
    }
    return QJsonObject{{"history", history}, {"components", componentMetadata}};
}

QJsonArray expectedForceComponentMetadata(const QJsonArray &history)
{
    const QList<RVariableType> variables = {R_VARIABLE_FORCE_X, R_VARIABLE_FORCE_Y, R_VARIABLE_FORCE_Z};
    QJsonArray timeKeys;
    for (const QJsonValue &sampleValue : history)
        timeKeys.append(sampleValue.toObject().value("time"));

    QJsonArray result;
    for (RVariableType variable : variables)
        result.append(QJsonObject{{"variable", int(variable)}, {"enabled", true},
            {"unit", RVariable::getUnits(variable)}, {"keyType", int(R_VARIABLE_TIME)},
            {"keyUnits", RVariable::getUnits(R_VARIABLE_TIME)}, {"timeKeys", timeKeys}});
    return result;
}

void diagnoseModelAgainstSpec(const RModel &model, const QJsonObject &normalized,
                              const QString &modelRevision, QJsonArray &issues)
{
    const QJsonObject geometry = normalized.value("geometry").toObject();
    const double length = geometry.value("length").toDouble();
    const int segments = geometry.value("segments").toInt();
    QJsonArray geometryMismatches;
    if (model.getNNodes() != uint(segments + 1))
        geometryMismatches.append(QJsonObject{{"property", "nodeCount"},
            {"expected", segments + 1}, {"observed", int(model.getNNodes())}});
    for (uint i = 0; i < model.getNNodes(); ++i) {
        const double expectedX = length * double(i) / double(std::max(1, segments));
        const RNode &node = model.getNode(i);
        if (!samePhysicalNumber(node.getX(), expectedX) || !samePhysicalNumber(node.getY(), 0.0)
            || !samePhysicalNumber(node.getZ(), 0.0))
            geometryMismatches.append(QJsonObject{{"property", QStringLiteral("node[%1].xyz").arg(i)},
                {"expected", QJsonArray{expectedX, 0.0, 0.0}},
                {"observed", QJsonArray{node.getX(), node.getY(), node.getZ()}}});
    }
    uint structuralElementCount = 0;
    for (uint i = 0; i < model.getNElements(); ++i)
        if (R_ELEMENT_TYPE_IS_LINE(model.getElement(i).getType())) ++structuralElementCount;
    if (structuralElementCount != uint(segments))
        geometryMismatches.append(QJsonObject{{"property", "elementCount"},
            {"expected", segments}, {"observed", int(structuralElementCount)}});
    if (model.getNLines() != 1)
        geometryMismatches.append(QJsonObject{{"property", "lineGroupCount"},
            {"expected", 1}, {"observed", int(model.getNLines())}});
    const QString modelType = normalized.value("modelType").toString();
    const RElementType expectedElementType = modelType == QLatin1String("axialRod")
        ? R_ELEMENT_TRUSS1 : R_ELEMENT_BEAM1;
    QMap<uint, uint> lineMembershipCounts;
    if (model.getNLines() == 1) {
        const RLine &line = model.getLine(0);
        for (uint i = 0; i < line.size(); ++i) {
            const uint elementPosition = line.get(i);
            ++lineMembershipCounts[elementPosition];
            if (elementPosition >= model.getNElements())
                geometryMismatches.append(QJsonObject{{"property", "lineGroupElementReference"},
                    {"elementPosition", int(elementPosition)}, {"elementCount", int(model.getNElements())}});
        }
    }
    QJsonArray unsupportedElements;
    uint structuralIndex = 0;
    for (uint i = 0; i < model.getNElements(); ++i) {
        const RElement &element = model.getElement(i);
        if (element.getType() == R_ELEMENT_POINT) continue;
        if (element.getType() != expectedElementType) {
            unsupportedElements.append(QJsonObject{{"elementPhysicalId", double(element.getPhysicalId())},
                {"elementPosition", int(i)}, {"expectedType", int(expectedElementType)},
                {"observedType", int(element.getType())}});
            continue;
        }
        if (element.size() != 2 || structuralIndex + 1 >= model.getNNodes()
            || element.getNodeId(0) != structuralIndex || element.getNodeId(1) != structuralIndex + 1)
            geometryMismatches.append(QJsonObject{{"property", QStringLiteral("element[%1].connectivity").arg(structuralIndex)},
                {"expected", QJsonArray{int(structuralIndex), int(structuralIndex + 1)}},
                {"observed", element.size() >= 2 ? QJsonArray{int(element.getNodeId(0)), int(element.getNodeId(1))} : QJsonArray{}}});
        const uint memberships = lineMembershipCounts.value(i, 0);
        if (memberships != 1)
            geometryMismatches.append(QJsonObject{{"property", QStringLiteral("element[%1].lineGroupMembership").arg(i)},
                {"elementPhysicalId", double(element.getPhysicalId())}, {"expectedOccurrences", 1},
                {"observedOccurrences", int(memberships)}});
        ++structuralIndex;
    }
    for (auto it = lineMembershipCounts.cbegin(); it != lineMembershipCounts.cend(); ++it) {
        if (it.key() >= model.getNElements()) continue;
        const RElement &element = model.getElement(it.key());
        if (element.getType() == R_ELEMENT_POINT || !R_ELEMENT_TYPE_IS_LINE(element.getType()))
            geometryMismatches.append(QJsonObject{{"property", "lineGroupContainsNonStructuralElement"},
                {"elementPosition", int(it.key())}, {"elementType", int(element.getType())},
                {"occurrences", int(it.value())}});
    }
    if (!unsupportedElements.isEmpty())
        addDiagnostic(issues, QStringLiteral("MODEL_ELEMENT_TYPE_UNSUPPORTED"), QStringLiteral("error"),
            QStringLiteral("Current rod/beam model contains element types outside the validated template."),
            QJsonObject{{"modelType", modelType}, {"unsupportedElements", unsupportedElements}},
            QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
            QJsonArray{"rebuildDetachedCandidate"}, true);
    if (!geometryMismatches.isEmpty())
        addDiagnostic(issues, QStringLiteral("MODEL_GEOMETRY_SPEC_MISMATCH"), QStringLiteral("error"),
            QStringLiteral("Current model geometry differs from the validated ProblemSpec."),
            QJsonObject{{"mismatches", geometryMismatches}}, QJsonArray{"candidate_modified_after_creation"},
            QJsonArray{"rebuildDetachedCandidate"}, true);

    QJsonArray unsupportedBoundaryConditions;
    auto inspectBoundaryConditionTypes = [&](const RElementGroup &group, const QString &groupKind) {
        for (uint i = 0; i < group.getNBoundaryConditions(); ++i) {
            const RBoundaryCondition &condition = group.getBoundaryCondition(i);
            const RBoundaryConditionType type = condition.getType();
            const bool structuralGroup = groupKind == QLatin1String("line")
                || groupKind == QLatin1String("point");
            const bool supportedDisplacement = structuralGroup
                && type == R_BOUNDARY_CONDITION_DISPLACEMENT;
            const bool supportedRotation = structuralGroup
                && modelType == QLatin1String("cantileverBeam")
                && type == R_BOUNDARY_CONDITION_ROTATION;
            const bool supportedPointForce = groupKind == QLatin1String("point")
                && type == R_BOUNDARY_CONDITION_FORCE;
            if (supportedDisplacement || supportedRotation || supportedPointForce) continue;
            unsupportedBoundaryConditions.append(QJsonObject{
                {"groupKind", groupKind}, {"groupPhysicalId", double(group.getPhysicalId())},
                {"groupName", group.getName()}, {"conditionType", int(type)},
                {"conditionName", RBoundaryCondition::getName(type)}});
        }
    };
    for (uint i = 0; i < model.getNLines(); ++i)
        inspectBoundaryConditionTypes(model.getLine(i), QStringLiteral("line"));
    for (uint i = 0; i < model.getNPoints(); ++i)
        inspectBoundaryConditionTypes(model.getPoint(i), QStringLiteral("point"));
    for (uint i = 0; i < model.getNSurfaces(); ++i)
        inspectBoundaryConditionTypes(model.getSurface(i), QStringLiteral("surface"));
    for (uint i = 0; i < model.getNVolumes(); ++i)
        inspectBoundaryConditionTypes(model.getVolume(i), QStringLiteral("volume"));
    if (!unsupportedBoundaryConditions.isEmpty())
        addDiagnostic(issues, QStringLiteral("MODEL_BOUNDARY_CONDITION_UNSUPPORTED"), QStringLiteral("error"),
            QStringLiteral("Current model contains boundary-condition types or placements outside the validated ProblemSpec."),
            QJsonObject{{"unsupportedConditions", unsupportedBoundaryConditions}},
            QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
            QJsonArray{"requestPhysicsClarification"}, true);

    QJsonArray unsupportedEnvironmentConditions;
    auto inspectEnvironmentConditions = [&](const RElementGroup &group, const QString &groupKind) {
        for (uint i = 0; i < group.getNEnvironmentConditions(); ++i) {
            const REnvironmentCondition &condition = group.getEnvironmentCondition(i);
            QJsonArray activeComponents;
            for (uint j = 0; j < condition.size(); ++j) {
                const RConditionComponent &component = condition.getComponent(j);
                if (!component.getEnabled()) continue;
                QJsonArray samples;
                bool hasActiveValue = component.size() == 0;
                for (uint k = 0; k < component.size(); ++k) {
                    const double time = component.getKey(k);
                    const double value = component.getValue(k);
                    const bool finiteTime = std::isfinite(time);
                    const bool finiteValue = std::isfinite(value);
                    if (value != 0.0 || !finiteValue || !finiteTime) hasActiveValue = true;
                    samples.append(QJsonObject{
                        {"time_s", finiteTime ? QJsonValue(time) : QJsonValue(QJsonValue::Null)},
                        {"timeKeyFinite", finiteTime},
                        {"value", finiteValue ? QJsonValue(value) : QJsonValue(QJsonValue::Null)},
                        {"valueFinite", finiteValue}});
                }
                if (hasActiveValue)
                    activeComponents.append(QJsonObject{{"variable", int(component.getType())},
                        {"unit", component.getUnits()}, {"sampleCount", int(component.size())},
                        {"samples", samples}});
            }
            if (activeComponents.isEmpty()) continue;
            unsupportedEnvironmentConditions.append(QJsonObject{
                {"groupKind", groupKind}, {"groupPhysicalId", double(group.getPhysicalId())},
                {"groupName", group.getName()}, {"conditionType", int(condition.getType())},
                {"conditionName", REnvironmentCondition::getName(condition.getType())},
                {"components", activeComponents}});
        }
    };
    for (uint i = 0; i < model.getNLines(); ++i)
        inspectEnvironmentConditions(model.getLine(i), QStringLiteral("line"));
    for (uint i = 0; i < model.getNPoints(); ++i)
        inspectEnvironmentConditions(model.getPoint(i), QStringLiteral("point"));
    for (uint i = 0; i < model.getNSurfaces(); ++i)
        inspectEnvironmentConditions(model.getSurface(i), QStringLiteral("surface"));
    for (uint i = 0; i < model.getNVolumes(); ++i)
        inspectEnvironmentConditions(model.getVolume(i), QStringLiteral("volume"));
    if (!unsupportedEnvironmentConditions.isEmpty())
        addDiagnostic(issues, QStringLiteral("MODEL_ENVIRONMENT_CONDITION_UNSUPPORTED"), QStringLiteral("error"),
            QStringLiteral("Current model contains active environment conditions outside the validated ProblemSpec."),
            QJsonObject{{"unsupportedConditions", unsupportedEnvironmentConditions}},
            QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
            QJsonArray{"requestPhysicsClarification"}, true);

    const NodeSupportMap expectedSupports = expectedSupportMap(model, normalized, modelRevision);
    const NodeSupportMap observedSupports = actualSupportMap(model);
    if (canonicalJson(supportMapJson(expectedSupports)) != canonicalJson(supportMapJson(observedSupports)))
        addDiagnostic(issues, QStringLiteral("MODEL_SUPPORT_SPEC_MISMATCH"), QStringLiteral("error"),
            QStringLiteral("Current model support locations or restrained directions differ from the validated ProblemSpec."),
            QJsonObject{{"expected", supportMapJson(expectedSupports)}, {"observed", supportMapJson(observedSupports)}},
            QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
            QJsonArray{"rebuildDetachedCandidate"}, true);

    bool nonzeroSupportValue = false;
    QJsonArray changedSupportValues;
    auto inspectSupportValues = [&](const RElementGroup &group) {
        for (uint i = 0; i < group.getNBoundaryConditions(); ++i) {
            const RBoundaryCondition &condition = group.getBoundaryCondition(i);
            if (condition.getType() != R_BOUNDARY_CONDITION_DISPLACEMENT
                && condition.getType() != R_BOUNDARY_CONDITION_ROTATION) continue;
            for (uint j = 0; j < condition.size(); ++j) {
                const RConditionComponent &component = condition.getComponent(j);
                if (!component.getEnabled()) continue;
                for (uint k = 0; k < component.size(); ++k) {
                    if (component.getValue(k) == 0.0) continue;
                    nonzeroSupportValue = true;
                    changedSupportValues.append(QJsonObject{{"groupPhysicalId", double(group.getPhysicalId())},
                        {"dof", supportDofName(component.getType())}, {"time_s", component.getKey(k)},
                        {"value", component.getValue(k)}});
                }
            }
        }
    };
    for (uint i = 0; i < model.getNLines(); ++i) inspectSupportValues(model.getLine(i));
    for (uint i = 0; i < model.getNPoints(); ++i) inspectSupportValues(model.getPoint(i));
    for (uint i = 0; i < model.getNSurfaces(); ++i) inspectSupportValues(model.getSurface(i));
    for (uint i = 0; i < model.getNVolumes(); ++i) inspectSupportValues(model.getVolume(i));
    if (nonzeroSupportValue)
        addDiagnostic(issues, QStringLiteral("MODEL_SUPPORT_VALUE_SPEC_MISMATCH"), QStringLiteral("error"),
            QStringLiteral("The current model contains nonzero prescribed support values, but ProblemSpec supports are zero-valued restraints."),
            QJsonObject{{"changedValues", changedSupportValues}}, QJsonArray{"candidate_modified_after_creation"},
            QJsonArray{"requestSupportClarification"}, true);

    NodeSignatureMap expectedLoads;
    for (const QJsonValue &entry : normalized.value("loads").toArray()) {
        const QJsonObject load = entry.toObject();
        const QJsonObject binding = RTrustedMechanicsService::resolveSelector(
            model, load.value("selector").toObject(), modelRevision, modelRevision);
        QJsonArray history = load.value("history").toArray();
        if (history.isEmpty()) history.append(QJsonObject{{"time", 0.0}, {"components", load.value("components")}});
        for (const QJsonValue &node : binding.value("nodePositions").toArray())
            appendSignature(expectedLoads, uint(node.toInt()), QJsonObject{{"kind", "pointForce"},
                {"history", history}, {"componentMetadata", expectedForceComponentMetadata(history)}});
    }
    NodeSignatureMap observedLoads;
    auto inspectLoads = [&](const RElementGroup &group) {
        const QSet<uint> nodes = groupNodePositions(model, group);
        for (uint i = 0; i < group.getNBoundaryConditions(); ++i) {
            const RBoundaryCondition &condition = group.getBoundaryCondition(i);
            if (condition.getType() != R_BOUNDARY_CONDITION_FORCE) continue;
            const QJsonObject signature = boundaryConditionHistory(condition);
            for (uint node : nodes)
                appendSignature(observedLoads, node, QJsonObject{{"kind", "pointForce"},
                    {"history", signature.value("history")},
                    {"componentMetadata", signature.value("components")}});
        }
    };
    for (uint i = 0; i < model.getNLines(); ++i) inspectLoads(model.getLine(i));
    for (uint i = 0; i < model.getNPoints(); ++i) inspectLoads(model.getPoint(i));
    for (uint i = 0; i < model.getNSurfaces(); ++i) inspectLoads(model.getSurface(i));
    for (uint i = 0; i < model.getNVolumes(); ++i) inspectLoads(model.getVolume(i));
    if (!equivalentLoadMaps(expectedLoads, observedLoads))
        addDiagnostic(issues, QStringLiteral("MODEL_LOAD_SPEC_MISMATCH"), QStringLiteral("error"),
            QStringLiteral("Current model point forces or force histories differ from the validated ProblemSpec."),
            QJsonObject{{"expected", nodeSignatureMapJson(expectedLoads)}, {"observed", nodeSignatureMapJson(observedLoads)}},
            QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
            QJsonArray{"requestLoadClarification"}, true);

    NodeSignatureMap expectedInitial;
    for (const QJsonValue &entry : normalized.value("initialConditions").toArray()) {
        const QJsonObject initial = entry.toObject();
        const QString type = initial.value("type").toString();
        const QJsonObject binding = RTrustedMechanicsService::resolveSelector(
            model, initial.value("selector").toObject(), modelRevision, modelRevision);
        const QJsonArray history{QJsonObject{{"time", 0.0}, {"components", initial.value("components")}}};
        for (const QJsonValue &node : binding.value("nodePositions").toArray())
            appendSignature(expectedInitial, uint(node.toInt()),
                QJsonObject{{"type", type}, {"history", history}});
    }
    NodeSignatureMap observedInitial;
    auto inspectInitial = [&](const RElementGroup &group) {
        const QSet<uint> nodes = groupNodePositions(model, group);
        for (uint i = 0; i < group.getNInitialConditions(); ++i) {
            const RInitialCondition &condition = group.getInitialCondition(i);
            const QString type = actualInitialType(condition.getType());
            const QList<RVariableType> variables = initialVariables(type);
            QList<const RConditionComponent *> components;
            bool validHistory = true;
            for (RVariableType variable : variables) {
                const unsigned int position = condition.findComponentPosition(variable);
                if (position == RConstants::eod || condition.getComponent(position).size() == 0) {
                    validHistory = false;
                    break;
                }
                components.append(&condition.getComponent(position));
            }
            QJsonArray history;
            if (validHistory) {
                const RConditionComponent &timeSource = *components.first();
                for (uint sample = 0; sample < timeSource.size(); ++sample) {
                    QJsonArray values;
                    for (const RConditionComponent *component : components) {
                        if (sample >= component->size() || !samePhysicalNumber(
                                component->getKey(sample), timeSource.getKey(sample))) {
                            validHistory = false;
                            break;
                        }
                        values.append(component->getValue(sample));
                    }
                    if (!validHistory) break;
                    history.append(QJsonObject{{"time", timeSource.getKey(sample)}, {"components", values}});
                }
            }
            const QJsonObject signature{{"type", type}, {"history", history}};
            for (uint node : nodes) appendSignature(observedInitial, node, signature);
        }
    };
    for (uint i = 0; i < model.getNLines(); ++i) inspectInitial(model.getLine(i));
    for (uint i = 0; i < model.getNPoints(); ++i) inspectInitial(model.getPoint(i));
    for (uint i = 0; i < model.getNSurfaces(); ++i) inspectInitial(model.getSurface(i));
    for (uint i = 0; i < model.getNVolumes(); ++i) inspectInitial(model.getVolume(i));
    if (canonicalJson(nodeSignatureMapJson(expectedInitial)) != canonicalJson(nodeSignatureMapJson(observedInitial)))
        addDiagnostic(issues, QStringLiteral("MODEL_INITIAL_CONDITION_SPEC_MISMATCH"), QStringLiteral("error"),
            QStringLiteral("Current model initial conditions differ from the validated ProblemSpec."),
            QJsonObject{{"expected", nodeSignatureMapJson(expectedInitial)},
                        {"observed", nodeSignatureMapJson(observedInitial)}},
            QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
            QJsonArray{"requestInitialConditionClarification"}, true);

    const QJsonObject expectedAnalysis = normalized.value("analysis").toObject();
    const RProblemTaskItem &task = model.getProblemTaskTree();
    const RProblemSetup &setup = model.getProblemSetup();
    const RStructuralDynamicsSetup &dynamics = setup.getStructuralDynamicsSetup();
    const RModalSetup &modal = setup.getModalSetup();
    QJsonArray analysisMismatches;
    const QString kind = expectedAnalysis.value("kind").toString();
    const int expectedProblem = kind == QLatin1String("modal") ? int(R_PROBLEM_STRESS_MODAL) : int(R_PROBLEM_STRESS);
    if (int(task.getProblemType()) != expectedProblem)
        analysisMismatches.append(QJsonObject{{"property", "problemType"}, {"expected", expectedProblem},
                                               {"observed", int(task.getProblemType())}});
    if (kind == QLatin1String("linearTransient")) {
        if (!dynamics.getEnabled()) analysisMismatches.append(QJsonObject{{"property", "dynamics.enabled"}, {"expected", true}, {"observed", false}});
        if (dynamics.getMethod() != RStructuralTimeIntegrationMethod::Newmark)
            analysisMismatches.append(QJsonObject{{"property", "dynamics.method"}, {"expected", "Newmark"}, {"observed", int(dynamics.getMethod())}});
        const QList<QPair<QString, QPair<double, double>>> values = {
            {QStringLiteral("timeStep"), {dynamics.getTimeStep(), expectedAnalysis.value("timeStep").toDouble()}},
            {QStringLiteral("rayleighAlpha"), {dynamics.getRayleighAlpha(), expectedAnalysis.value("rayleighAlpha").toDouble()}},
            {QStringLiteral("rayleighBeta"), {dynamics.getRayleighBeta(), expectedAnalysis.value("rayleighBeta").toDouble()}},
            {QStringLiteral("newmarkGamma"), {dynamics.getNewmarkGamma(), 0.5}},
            {QStringLiteral("newmarkBeta"), {dynamics.getNewmarkBeta(), 0.25}}
        };
        for (const auto &value : values)
            if (!samePhysicalNumber(value.second.first, value.second.second))
                analysisMismatches.append(QJsonObject{{"property", value.first}, {"expected", value.second.second},
                                                       {"observed", value.second.first}});
        if (dynamics.getNSteps() != uint(expectedAnalysis.value("steps").toInt()))
            analysisMismatches.append(QJsonObject{{"property", "steps"}, {"expected", expectedAnalysis.value("steps")},
                                                   {"observed", int(dynamics.getNSteps())}});
        if (dynamics.getOutputFrequency() != 1)
            analysisMismatches.append(QJsonObject{{"property", "outputFrequency"}, {"expected", 1},
                                                   {"observed", int(dynamics.getOutputFrequency())}});
    } else if (dynamics.getEnabled()) {
        analysisMismatches.append(QJsonObject{{"property", "dynamics.enabled"}, {"expected", false}, {"observed", true}});
    }
    if (kind == QLatin1String("modal")) {
        if (modal.getNModesToExtract() != uint(expectedAnalysis.value("modes").toInt()))
            analysisMismatches.append(QJsonObject{{"property", "modal.modes"},
                {"expected", expectedAnalysis.value("modes")}, {"observed", int(modal.getNModesToExtract())}});
        if (modal.getMethod() != R_MODAL_MULTIPLE_MODES)
            analysisMismatches.append(QJsonObject{{"property", "modal.method"},
                {"expected", int(R_MODAL_MULTIPLE_MODES)}, {"observed", int(modal.getMethod())}});
    }
    if (!analysisMismatches.isEmpty())
        addDiagnostic(issues, QStringLiteral("MODEL_ANALYSIS_SPEC_MISMATCH"), QStringLiteral("error"),
            QStringLiteral("Current model analysis settings differ from the validated ProblemSpec."),
            QJsonObject{{"mismatches", analysisMismatches}}, QJsonArray{"candidate_modified_after_creation"},
            QJsonArray{"rebuildDetachedCandidate"}, true);
}

QJsonObject actualSupportEvidence(const RModel &model, const QString &modelType)
{
    const NodeSupportMap constrained = actualSupportMap(model);
    QMap<uint, QSet<uint>> adjacency;
    QSet<uint> structuralNodes;
    for (uint i = 0; i < model.getNElements(); ++i) {
        const RElement &element = model.getElement(i);
        if (!R_ELEMENT_TYPE_IS_LINE(element.getType())) continue;
        for (uint j = 0; j < element.size(); ++j) {
            const uint node = element.getNodeId(j);
            if (node >= model.getNNodes()) continue;
            structuralNodes.insert(node);
            adjacency[node];
        }
        for (uint j = 0; j < element.size(); ++j)
            for (uint k = j + 1; k < element.size(); ++k) {
                const uint a = element.getNodeId(j), b = element.getNodeId(k);
                if (a >= model.getNNodes() || b >= model.getNNodes()) continue;
                adjacency[a].insert(b);
                adjacency[b].insert(a);
            }
    }
    QJsonArray missingDirections;
    QSet<QString> unconstrainedDirections;
    QJsonArray componentEvidence;
    bool anyUnderconstrained = false;
    QSet<uint> visited;
    QList<uint> orderedStructuralNodes = structuralNodes.values();
    std::sort(orderedStructuralNodes.begin(), orderedStructuralNodes.end());
    for (uint start : orderedStructuralNodes) {
        if (visited.contains(start)) continue;
        QSet<uint> component;
        QList<uint> pending{start};
        visited.insert(start);
        while (!pending.isEmpty()) {
            const uint node = pending.takeLast();
            component.insert(node);
            for (uint neighbor : adjacency.value(node)) {
                if (visited.contains(neighbor)) continue;
                visited.insert(neighbor);
                pending.append(neighbor);
            }
        }
        QJsonArray componentNodes;
        QList<uint> orderedNodes = component.values();
        std::sort(orderedNodes.begin(), orderedNodes.end());
        for (uint node : orderedNodes) componentNodes.append(int(node));
        if (modelType == QLatin1String("axialRod")) {
            double minX = std::numeric_limits<double>::infinity();
            for (uint node : component) minX = std::min(minX, model.getNode(node).getX());
            bool fixedAxialEnd = false;
            QSet<QString> componentMissing;
            for (uint node : component) {
                const QSet<QString> dofs = constrained.value(node);
                if (std::abs(model.getNode(node).getX() - minX) <= 1.0e-12 && dofs.contains("ux"))
                    fixedAxialEnd = true;
                if (!dofs.contains("uy")) componentMissing.insert(QStringLiteral("uy"));
                if (!dofs.contains("uz")) componentMissing.insert(QStringLiteral("uz"));
            }
            if (!fixedAxialEnd) componentMissing.insert(QStringLiteral("ux"));
            for (const QString &dof : {QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")})
                if (componentMissing.contains(dof)) unconstrainedDirections.insert(dof);
            QJsonArray componentMissingJson;
            for (const QString &dof : {QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")})
                if (componentMissing.contains(dof)) componentMissingJson.append(dof);
            anyUnderconstrained = anyUnderconstrained || !componentMissing.isEmpty();
            componentEvidence.append(QJsonObject{{"nodePositions", componentNodes},
                {"hasUnrestrainedRigidMode", !componentMissing.isEmpty()}, {"missingDirections", componentMissingJson}});
            continue;
        }

        double minX = std::numeric_limits<double>::infinity(), minY = minX, minZ = minX;
        double maxX = -minX, maxY = -minX, maxZ = -minX;
        for (uint node : component) {
            const RNode &point = model.getNode(node);
            minX = std::min(minX, point.getX()); maxX = std::max(maxX, point.getX());
            minY = std::min(minY, point.getY()); maxY = std::max(maxY, point.getY());
            minZ = std::min(minZ, point.getZ()); maxZ = std::max(maxZ, point.getZ());
        }
        const double originX = (minX + maxX) / 2.0;
        const double originY = (minY + maxY) / 2.0;
        const double originZ = (minZ + maxZ) / 2.0;
        const double characteristicLength = std::max({maxX - minX, maxY - minY, maxZ - minZ, 1.0e-12});
        std::vector<std::array<double, 6>> rows;
        const QStringList allDofs{"ux", "uy", "uz", "rx", "ry", "rz"};
        QSet<QString> componentPresent;
        for (uint node : component) {
            const RNode &point = model.getNode(node);
            const double x = (point.getX() - originX) / characteristicLength;
            const double y = (point.getY() - originY) / characteristicLength;
            const double z = (point.getZ() - originZ) / characteristicLength;
            for (const QString &dof : allDofs) {
                if (!constrained.value(node).contains(dof)) continue;
                componentPresent.insert(dof);
                std::array<double, 6> row{};
                if (dof == QLatin1String("ux")) row = {1.0, 0.0, 0.0, 0.0, z, -y};
                else if (dof == QLatin1String("uy")) row = {0.0, 1.0, 0.0, -z, 0.0, x};
                else if (dof == QLatin1String("uz")) row = {0.0, 0.0, 1.0, y, -x, 0.0};
                else if (dof == QLatin1String("rx")) row = {0.0, 0.0, 0.0, 1.0, 0.0, 0.0};
                else if (dof == QLatin1String("ry")) row = {0.0, 0.0, 0.0, 0.0, 1.0, 0.0};
                else if (dof == QLatin1String("rz")) row = {0.0, 0.0, 0.0, 0.0, 0.0, 1.0};
                rows.push_back(row);
            }
        }
        int rank = 0;
        for (int column = 0; column < 6 && rank < int(rows.size()); ++column) {
            int pivot = rank;
            for (int row = rank + 1; row < int(rows.size()); ++row)
                if (std::abs(rows[size_t(row)][size_t(column)]) > std::abs(rows[size_t(pivot)][size_t(column)])) pivot = row;
            if (std::abs(rows[size_t(pivot)][size_t(column)]) < 1.0e-10) continue;
            std::swap(rows[size_t(rank)], rows[size_t(pivot)]);
            const double divisor = rows[size_t(rank)][size_t(column)];
            for (int c = column; c < 6; ++c) rows[size_t(rank)][size_t(c)] /= divisor;
            for (int row = 0; row < int(rows.size()); ++row) {
                if (row == rank) continue;
                const double factor = rows[size_t(row)][size_t(column)];
                for (int c = column; c < 6; ++c) rows[size_t(row)][size_t(c)] -= factor * rows[size_t(rank)][size_t(c)];
            }
            ++rank;
        }
        QJsonArray componentMissingJson;
        for (const QString &dof : allDofs) {
            if (!componentPresent.contains(dof)) {
                unconstrainedDirections.insert(dof);
                componentMissingJson.append(dof);
            }
        }
        if (rank < 6 && componentMissingJson.isEmpty()) componentMissingJson.append(QStringLiteral("rigid-body-mode"));
        anyUnderconstrained = anyUnderconstrained || rank < 6;
        componentEvidence.append(QJsonObject{{"nodePositions", componentNodes},
            {"constraintRank", rank}, {"requiredConstraintRank", 6},
            {"hasUnrestrainedRigidMode", rank < 6}, {"missingDirections", componentMissingJson}});
    }
    if (modelType == QLatin1String("axialRod")) {
        QStringList ordered{"ux", "uy", "uz"};
        for (const QString &dof : ordered)
            if (unconstrainedDirections.contains(dof)) missingDirections.append(dof);
        return QJsonObject{{"modelType", modelType}, {"method", "axial-rod-direction-coverage"},
                           {"hasUnrestrainedRigidMode", anyUnderconstrained || structuralNodes.isEmpty()},
                           {"requiredDirections", QJsonArray{"ux_at_fixed_end", "uy_at_each_node", "uz_at_each_node"}},
                           {"missingDirections", missingDirections}, {"components", componentEvidence},
                           {"structuralComponentCount", componentEvidence.size()},
                           {"constrainedNodeCount", constrained.size()}, {"nodeCount", int(model.getNNodes())}};
    }
    QJsonArray orderedUnconstrained;
    for (const QString &dof : {QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                               QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz")})
        if (unconstrainedDirections.contains(dof)) orderedUnconstrained.append(dof);
    return QJsonObject{{"modelType", modelType}, {"method", "scaled-six-rigid-mode-constraint-rank"},
                       {"components", componentEvidence},
                       {"hasUnrestrainedRigidMode", anyUnderconstrained || structuralNodes.isEmpty()},
                       {"unconstrainedDirections", orderedUnconstrained},
                       {"missingDirections", anyUnderconstrained && unconstrainedDirections.isEmpty()
                            ? QJsonArray{"rigid-body-mode"} : QJsonArray(orderedUnconstrained)},
                       {"structuralComponentCount", componentEvidence.size()}};
}
}

RTrustedMechanicsService::ValidationResult RTrustedMechanicsService::validate(const QJsonObject &spec)
{
    ValidationResult result;
    QJsonArray issues;
    rejectUnknownFields(spec, {"schemaVersion", "modelType", "name", "geometry", "material", "section",
                               "analysis", "supports", "loads", "initialConditions", "observables", "precision"},
                       QStringLiteral("spec"), issues);
    if (spec.value("schemaVersion").toInt(-1) != SchemaVersion)
        addIssue(issues, QStringLiteral("SPEC_VERSION_UNSUPPORTED"), QStringLiteral("schemaVersion"),
                 QStringLiteral("Only schema version 1 is supported."));

    const auto arrayMember = [&issues](const QJsonObject &object, const QString &key,
                                       const QString &path) {
        const QJsonValue value = object.value(key);
        if (value.isUndefined()) return QJsonArray{};
        if (!value.isArray()) {
            addIssue(issues, QStringLiteral("SPEC_ARRAY_TYPE_INVALID"), path,
                     QStringLiteral("This field must be an array when it is provided."));
            return QJsonArray{};
        }
        return value.toArray();
    };

    const QString modelType = spec.value("modelType").toString();
    if (modelType != QLatin1String("axialRod") && modelType != QLatin1String("cantileverBeam"))
        addIssue(issues, QStringLiteral("MODEL_TYPE_UNSUPPORTED"), QStringLiteral("modelType"),
                 QStringLiteral("Only axialRod and cantileverBeam are supported."));

    const QJsonObject rawGeometry = spec.value("geometry").toObject();
    rejectUnknownFields(rawGeometry, {"length", "segments"}, QStringLiteral("geometry"), issues);
    QJsonObject geometry;
    quantityMember(rawGeometry, QStringLiteral("length"), Dimension::Length,
                   QStringLiteral("geometry"), issues, geometry, true);
    const QJsonValue segmentValue = rawGeometry.value("segments");
    const int segments = segmentValue.toInt(-1);
    if (!segmentValue.isDouble() || segments < 1 || segments > 10000)
        addIssue(issues, QStringLiteral("MESH_SEGMENT_COUNT_INVALID"), QStringLiteral("geometry.segments"),
                 QStringLiteral("segments must be an integer in [1,10000]."));
    geometry.insert("segments", segments);

    const QJsonObject rawMaterial = spec.value("material").toObject();
    rejectUnknownFields(rawMaterial, {"name", "source", "youngModulus", "poissonRatio", "density"},
                        QStringLiteral("material"), issues);
    QJsonObject material;
    material.insert("name", rawMaterial.value("name").toString().trimmed());
    material.insert("source", rawMaterial.value("source").toString().trimmed());
    if (material.value("name").toString().isEmpty())
        addIssue(issues, QStringLiteral("MATERIAL_NAME_MISSING"), QStringLiteral("material.name"),
                 QStringLiteral("A material name is required."));
    if (material.value("source").toString().isEmpty())
        addIssue(issues, QStringLiteral("MATERIAL_SOURCE_MISSING"), QStringLiteral("material.source"),
                 QStringLiteral("Record whether the material comes from a user, catalog, or reference."));
    quantityMember(rawMaterial, QStringLiteral("youngModulus"), Dimension::Pressure,
                   QStringLiteral("material"), issues, material, true);
    quantityMember(rawMaterial, QStringLiteral("density"), Dimension::Density,
                   QStringLiteral("material"), issues, material, true);
    const QJsonValue poissonValue = rawMaterial.value("poissonRatio");
    if (!poissonValue.isDouble() || !std::isfinite(poissonValue.toDouble())
        || poissonValue.toDouble() < 0.0 || poissonValue.toDouble() >= 0.5)
        addIssue(issues, QStringLiteral("MATERIAL_POISSON_INVALID"), QStringLiteral("material.poissonRatio"),
                 QStringLiteral("For this isotropic linear-elastic lane, poissonRatio must be in [0,0.5)."));
    material.insert("poissonRatio", poissonValue.toDouble());

    const QJsonObject rawSection = spec.value("section").toObject();
    QSet<QString> sectionFields{"area"};
    if (modelType == QLatin1String("cantileverBeam"))
        sectionFields.unite({"iy", "iz", "torsionalConstant", "wy", "wz", "wt", "localY"});
    rejectUnknownFields(rawSection, sectionFields, QStringLiteral("section"), issues);
    QJsonObject section;
    quantityMember(rawSection, QStringLiteral("area"), Dimension::Area,
                   QStringLiteral("section"), issues, section, true);
    if (modelType == QLatin1String("cantileverBeam")) {
        quantityMember(rawSection, QStringLiteral("iy"), Dimension::Inertia,
                       QStringLiteral("section"), issues, section, true);
        quantityMember(rawSection, QStringLiteral("iz"), Dimension::Inertia,
                       QStringLiteral("section"), issues, section, true);
        quantityMember(rawSection, QStringLiteral("torsionalConstant"), Dimension::Inertia,
                       QStringLiteral("section"), issues, section, true);
        quantityMember(rawSection, QStringLiteral("wy"), Dimension::SectionModulus,
                       QStringLiteral("section"), issues, section, true);
        quantityMember(rawSection, QStringLiteral("wz"), Dimension::SectionModulus,
                       QStringLiteral("section"), issues, section, true);
        quantityMember(rawSection, QStringLiteral("wt"), Dimension::SectionModulus,
                       QStringLiteral("section"), issues, section, true);
        const QJsonArray localY = arrayMember(rawSection, QStringLiteral("localY"),
                                              QStringLiteral("section.localY"));
        if (localY.size() != 3) {
            addIssue(issues, QStringLiteral("BEAM_LOCAL_AXIS_MISSING"), QStringLiteral("section.localY"),
                     QStringLiteral("Provide a three-component section local-Y direction."));
        } else {
            double norm = 0.0;
            for (const QJsonValue &entry : localY) {
                if (!entry.isDouble() || !std::isfinite(entry.toDouble())) {
                    addIssue(issues, QStringLiteral("BEAM_LOCAL_AXIS_INVALID"), QStringLiteral("section.localY"),
                             QStringLiteral("Local direction values must be finite numbers."));
                    break;
                }
                norm += entry.toDouble() * entry.toDouble();
            }
            if (std::abs(norm - 1.0) > 1.0e-8 || std::abs(localY.at(0).toDouble()) > 1.0e-8)
                addIssue(issues, QStringLiteral("BEAM_LOCAL_AXIS_INVALID"), QStringLiteral("section.localY"),
                         QStringLiteral("For an x-axis beam, localY must be unit length and perpendicular to the beam axis."));
            section.insert("localY", localY);
        }
    }

    const QJsonObject rawAnalysis = spec.value("analysis").toObject();
    QJsonObject analysis;
    const QString analysisKind = rawAnalysis.value("kind").toString();
    QSet<QString> analysisFields{"kind"};
    if (analysisKind == QLatin1String("modal")) analysisFields.insert("modes");
    if (analysisKind == QLatin1String("linearTransient")) analysisFields.unite({"timeStep", "steps", "damping"});
    rejectUnknownFields(rawAnalysis, analysisFields, QStringLiteral("analysis"), issues);
    static const QSet<QString> analysisKinds = {"static", "modal", "linearTransient"};
    if (!analysisKinds.contains(analysisKind))
        addIssue(issues, QStringLiteral("ANALYSIS_TYPE_UNSUPPORTED"), QStringLiteral("analysis.kind"),
                 QStringLiteral("Supported analysis kinds are static, modal, and linearTransient."));
    analysis.insert("kind", analysisKind);
    if (analysisKind == QLatin1String("modal")) {
        const int modes = rawAnalysis.value("modes").toInt(-1);
        if (modes < 1 || modes > 100)
            addIssue(issues, QStringLiteral("ANALYSIS_MODAL_COUNT_INVALID"), QStringLiteral("analysis.modes"),
                     QStringLiteral("modes must be in [1,100]."));
        analysis.insert("modes", modes);
    }
    if (analysisKind == QLatin1String("linearTransient")) {
        double timeStep = 0.0;
        const bool hasTimeStep = quantity(rawAnalysis.value("timeStep"), Dimension::Time,
                                          QStringLiteral("analysis.timeStep"), issues, timeStep);
        const int steps = rawAnalysis.value("steps").toInt(-1);
        if (hasTimeStep && timeStep <= 0.0)
            addIssue(issues, QStringLiteral("ANALYSIS_TIME_STEP_INVALID"), QStringLiteral("analysis.timeStep"),
                     QStringLiteral("timeStep must be positive."));
        if (steps < 1 || steps > 1000000)
            addIssue(issues, QStringLiteral("ANALYSIS_STEP_COUNT_INVALID"), QStringLiteral("analysis.steps"),
                     QStringLiteral("steps must be in [1,1000000]."));
        analysis.insert("timeStep", timeStep);
        analysis.insert("steps", steps);
        const double duration = timeStep * steps;
        if (!std::isfinite(duration) || duration <= 0.0)
            addIssue(issues, QStringLiteral("ANALYSIS_DURATION_INVALID"), QStringLiteral("analysis"),
                     QStringLiteral("timeStep multiplied by steps must remain a finite positive duration."));
        analysis.insert("duration", duration);

        const QJsonObject rawDamping = rawAnalysis.value("damping").toObject();
        rejectUnknownFields(rawDamping, {"rayleighAlpha", "rayleighBeta"},
                            QStringLiteral("analysis.damping"), issues);
        double rayleighAlpha = 0.0, rayleighBeta = 0.0;
        const bool alphaValid = quantity(rawDamping.value("rayleighAlpha"), Dimension::Frequency,
                                         QStringLiteral("analysis.damping.rayleighAlpha"), issues, rayleighAlpha);
        const bool betaValid = quantity(rawDamping.value("rayleighBeta"), Dimension::Time,
                                        QStringLiteral("analysis.damping.rayleighBeta"), issues, rayleighBeta);
        if (alphaValid && rayleighAlpha < 0.0)
            addIssue(issues, QStringLiteral("ANALYSIS_DAMPING_INVALID"), QStringLiteral("analysis.damping.rayleighAlpha"),
                     QStringLiteral("Rayleigh mass damping coefficient must be nonnegative."));
        if (betaValid && rayleighBeta < 0.0)
            addIssue(issues, QStringLiteral("ANALYSIS_DAMPING_INVALID"), QStringLiteral("analysis.damping.rayleighBeta"),
                     QStringLiteral("Rayleigh stiffness damping coefficient must be nonnegative."));
        analysis.insert("rayleighAlpha", rayleighAlpha);
        analysis.insert("rayleighBeta", rayleighBeta);
    }

    QJsonArray supports;
    const QJsonArray rawSupports = arrayMember(spec, QStringLiteral("supports"), QStringLiteral("supports"));
    if (rawSupports.isEmpty())
        addIssue(issues, QStringLiteral("SUPPORTS_MISSING"), QStringLiteral("supports"),
                 QStringLiteral("At least one explicit support definition is required."));
    QSet<QString> fixedDofs;
    QSet<QString> allNodeDofs;
    for (int i = 0; i < rawSupports.size(); ++i) {
        const QJsonObject raw = rawSupports.at(i).toObject();
        rejectUnknownFields(raw, {"selector", "dofs"}, QStringLiteral("supports[%1]").arg(i), issues);
        QJsonObject selector;
        normalizeSelector(raw.value("selector").toObject(), QStringLiteral("supports[%1].selector").arg(i), issues, selector);
        QJsonArray dofs = dofArray(raw.value("dofs"), QStringLiteral("supports[%1].dofs").arg(i), issues);
        const QString selectorKind = selector.value("kind").toString();
        bool selectsFixedEnd = selectorKind == QLatin1String("fixedEnd");
        if (selectorKind == QLatin1String("position")) {
            const double x = selector.value("x").toDouble(std::numeric_limits<double>::quiet_NaN());
            const double tolerance = selector.contains("tolerance")
                ? selector.value("tolerance").toDouble()
                : std::max(1.0e-12, 1.0e-9 * geometry.value("length").toDouble());
            const double segmentLength = geometry.value("length").toDouble() / double(std::max(1, segments));
            const double nearestNode = std::round(x / segmentLength);
            selectsFixedEnd = std::isfinite(x) && std::abs(x) <= tolerance
                && std::abs(nearestNode) <= 1.0e-12;
        }
        if (selectsFixedEnd) for (const auto &entry : dofs) fixedDofs.insert(entry.toString());
        if (selectorKind == QLatin1String("allNodes")) for (const auto &entry : dofs) allNodeDofs.insert(entry.toString());
        if (modelType == QLatin1String("axialRod")) {
            for (const auto &entry : dofs) {
                if (entry.toString() == QLatin1String("rx") || entry.toString() == QLatin1String("ry")
                    || entry.toString() == QLatin1String("rz"))
                    addIssue(issues, QStringLiteral("ROD_ROTATIONAL_SUPPORT_UNSUPPORTED"),
                             QStringLiteral("supports[%1].dofs").arg(i),
                             QStringLiteral("TRUSS1 axial rods have no rotational degrees of freedom."));
            }
        }
        supports.append(QJsonObject{{"selector", selector}, {"dofs", dofs}});
    }
    if (modelType == QLatin1String("axialRod")) {
        if (!fixedDofs.contains("ux")) addIssue(issues, QStringLiteral("SUPPORT_MISSING_DOF"), QStringLiteral("supports"),
            QStringLiteral("The fixed end must restrain ux."), QStringLiteral("error"), QJsonObject{{"missingDirection", "ux"}, {"requiredAt", "fixedEnd"}});
        if (!allNodeDofs.contains("uy")) addIssue(issues, QStringLiteral("SUPPORT_MISSING_DOF"), QStringLiteral("supports"),
            QStringLiteral("The rod line must restrain transverse uy at every node."), QStringLiteral("error"), QJsonObject{{"missingDirection", "uy"}, {"requiredAt", "allNodes"}});
        if (!allNodeDofs.contains("uz")) addIssue(issues, QStringLiteral("SUPPORT_MISSING_DOF"), QStringLiteral("supports"),
            QStringLiteral("The rod line must restrain transverse uz at every node."), QStringLiteral("error"), QJsonObject{{"missingDirection", "uz"}, {"requiredAt", "allNodes"}});
    } else if (modelType == QLatin1String("cantileverBeam")) {
        for (const QString &dof : {"ux", "uy", "uz", "rx", "ry", "rz"})
            if (!fixedDofs.contains(dof))
                addIssue(issues, QStringLiteral("SUPPORT_MISSING_DOF"), QStringLiteral("supports"),
                         QStringLiteral("The cantilever fixed end must restrain %1.").arg(dof),
                         QStringLiteral("error"), QJsonObject{{"missingDirection", dof}, {"requiredAt", "fixedEnd"}});
    }

    QJsonArray loads;
    const QJsonArray rawLoads = arrayMember(spec, QStringLiteral("loads"), QStringLiteral("loads"));
    if (analysisKind == QLatin1String("static") && rawLoads.isEmpty())
        addIssue(issues, QStringLiteral("LOADS_MISSING"), QStringLiteral("loads"),
                 QStringLiteral("Static analysis requires an explicit load definition."));
    if (analysisKind == QLatin1String("modal") && !rawLoads.isEmpty())
        addIssue(issues, QStringLiteral("LOADS_NOT_APPLICABLE_TO_MODAL"), QStringLiteral("loads"),
                 QStringLiteral("This unprestressed modal lane does not apply static or transient loads."));
    for (int i = 0; i < rawLoads.size(); ++i) {
        const QJsonObject raw = rawLoads.at(i).toObject();
        rejectUnknownFields(raw, {"kind", "selector", "components", "history"},
                            QStringLiteral("loads[%1]").arg(i), issues);
        if (raw.value("kind").toString() != QLatin1String("pointForce")) {
            addIssue(issues, QStringLiteral("LOAD_TYPE_UNSUPPORTED"), QStringLiteral("loads[%1].kind").arg(i),
                     QStringLiteral("Only pointForce is supported in this lane."));
            continue;
        }
        QJsonObject selector;
        normalizeSelector(raw.value("selector").toObject(), QStringLiteral("loads[%1].selector").arg(i), issues, selector);
        if (selector.value("kind") == QLatin1String("allNodes"))
            addIssue(issues, QStringLiteral("LOAD_SELECTOR_NOT_SINGLE_NODE"), QStringLiteral("loads[%1].selector").arg(i),
                     QStringLiteral("pointForce must resolve to one physical node; allNodes would multiply the resultant during mesh refinement."));
        QJsonArray components = normalizeVector(raw.value("components"), Dimension::Force,
                                                QStringLiteral("loads[%1].components").arg(i), issues);
        auto rejectRodTransverse = [&](const QJsonArray &vector, const QString &path) {
            if (modelType == QLatin1String("axialRod") && vector.size() == 3 &&
                (vector.at(1).toDouble() != 0.0 || vector.at(2).toDouble() != 0.0))
                addIssue(issues, QStringLiteral("AXIAL_ROD_TRANSVERSE_INPUT_UNSUPPORTED"), path,
                         QStringLiteral("The axial-rod profile verifies only ux; transverse force components must be zero."));
        };
        rejectRodTransverse(components, QStringLiteral("loads[%1].components").arg(i));
        QJsonArray history;
        const QJsonArray rawHistory = arrayMember(raw, QStringLiteral("history"),
                                                  QStringLiteral("loads[%1].history").arg(i));
        if (!rawHistory.isEmpty() && analysisKind != QLatin1String("linearTransient"))
            addIssue(issues, QStringLiteral("LOAD_HISTORY_REQUIRES_TRANSIENT"),
                     QStringLiteral("loads[%1].history").arg(i),
                     QStringLiteral("Time-varying point-force history is supported only for linearTransient analysis."));
        double previousTime = -1.0;
        const double duration = analysis.value("duration").toDouble(std::numeric_limits<double>::infinity());
        for (int j = 0; j < rawHistory.size(); ++j) {
            const QJsonObject sample = rawHistory.at(j).toObject();
            rejectUnknownFields(sample, {"time", "components"},
                                QStringLiteral("loads[%1].history[%2]").arg(i).arg(j), issues);
            double time = 0.0;
            const QString timePath = QStringLiteral("loads[%1].history[%2].time").arg(i).arg(j);
            if (quantity(sample.value("time"), Dimension::Time, timePath, issues, time)) {
                if (time < 0.0 || (j > 0 && time <= previousTime))
                    addIssue(issues, QStringLiteral("LOAD_HISTORY_TIME_ORDER_INVALID"), timePath,
                             QStringLiteral("History times must be nonnegative and strictly increasing."));
                if (time > duration)
                    addIssue(issues, QStringLiteral("LOAD_HISTORY_EXCEEDS_DURATION"), timePath,
                             QStringLiteral("History samples may not exceed the declared analysis duration."));
                previousTime = time;
            }
            const QJsonArray force = normalizeVector(sample.value("components"), Dimension::Force,
                QStringLiteral("loads[%1].history[%2].components").arg(i).arg(j), issues);
            rejectRodTransverse(force, QStringLiteral("loads[%1].history[%2].components").arg(i).arg(j));
            history.append(QJsonObject{{"time", time}, {"components", force}});
        }
        if (!history.isEmpty() && !components.isEmpty()
            && history.first().toObject().value("components").toArray() != components)
            addIssue(issues, QStringLiteral("LOAD_HISTORY_BASE_MISMATCH"),
                     QStringLiteral("loads[%1].components").arg(i),
                     QStringLiteral("components must match the first force-history sample."));
        loads.append(QJsonObject{{"kind", "pointForce"}, {"selector", selector},
                                 {"components", components}, {"history", history}});
    }

    QJsonArray initialConditions;
    const QJsonArray rawInitialConditions = arrayMember(spec, QStringLiteral("initialConditions"),
                                                        QStringLiteral("initialConditions"));
    if (analysisKind != QLatin1String("linearTransient") && !rawInitialConditions.isEmpty())
        addIssue(issues, QStringLiteral("INITIAL_CONDITIONS_ANALYSIS_MISMATCH"), QStringLiteral("initialConditions"),
                 QStringLiteral("Initial conditions are supported only for linearTransient analysis."));
    if (analysisKind == QLatin1String("linearTransient") && rawInitialConditions.isEmpty())
        addIssue(issues, QStringLiteral("INITIAL_CONDITIONS_MISSING"), QStringLiteral("initialConditions"),
                 QStringLiteral("Transient analysis requires explicit initial conditions, including explicit zeros."));
    for (int i = 0; i < rawInitialConditions.size(); ++i) {
        const QJsonObject raw = rawInitialConditions.at(i).toObject();
        rejectUnknownFields(raw, {"type", "selector", "components"},
                            QStringLiteral("initialConditions[%1]").arg(i), issues);
        const QString type = raw.value("type").toString();
        if (modelType == QLatin1String("axialRod")
            && (type == QLatin1String("rotation") || type == QLatin1String("angularVelocity")))
            addIssue(issues, QStringLiteral("INITIAL_CONDITION_NOT_APPLICABLE_TO_ROD"),
                     QStringLiteral("initialConditions[%1].type").arg(i),
                     QStringLiteral("Axial rod templates do not have rotational degrees of freedom."));
        Dimension dimension = Dimension::Length;
        if (type == QLatin1String("velocity")) dimension = Dimension::Velocity;
        else if (type == QLatin1String("rotation")) dimension = Dimension::Rotation;
        else if (type == QLatin1String("angularVelocity")) dimension = Dimension::AngularVelocity;
        else if (type != QLatin1String("displacement")) {
            addIssue(issues, QStringLiteral("INITIAL_CONDITION_UNSUPPORTED"), QStringLiteral("initialConditions[%1].type").arg(i),
                     QStringLiteral("Supported initial conditions: displacement, velocity, rotation, angularVelocity."));
            continue;
        }
        QJsonObject selector;
        normalizeSelector(raw.value("selector").toObject(), QStringLiteral("initialConditions[%1].selector").arg(i), issues, selector);
        QJsonArray components = normalizeVector(raw.value("components"), dimension,
                                                QStringLiteral("initialConditions[%1].components").arg(i), issues);
        if (modelType == QLatin1String("axialRod") &&
            (type == QLatin1String("displacement") || type == QLatin1String("velocity")) &&
            components.size() == 3 && (components.at(1).toDouble() != 0.0 || components.at(2).toDouble() != 0.0))
            addIssue(issues, QStringLiteral("AXIAL_ROD_TRANSVERSE_INPUT_UNSUPPORTED"),
                     QStringLiteral("initialConditions[%1].components").arg(i),
                     QStringLiteral("The axial-rod profile verifies only ux; transverse initial state components must be zero."));
        initialConditions.append(QJsonObject{{"type", type}, {"selector", selector}, {"components", components}});
    }

    QJsonArray observables;
    const QJsonArray rawObservables = arrayMember(spec, QStringLiteral("observables"), QStringLiteral("observables"));
    if (rawObservables.isEmpty())
        addIssue(issues, QStringLiteral("OBSERVABLES_MISSING"), QStringLiteral("observables"),
                 QStringLiteral("At least one explicit quantity of interest is required."));
    for (int i = 0; i < rawObservables.size(); ++i) {
        const QJsonObject raw = rawObservables.at(i).toObject();
        rejectUnknownFields(raw, {"selector", "quantity", "component"},
                            QStringLiteral("observables[%1]").arg(i), issues);
        QJsonObject selector;
        normalizeSelector(raw.value("selector").toObject(), QStringLiteral("observables[%1].selector").arg(i), issues, selector);
        const QString quantityName = raw.value("quantity").toString();
        static const QSet<QString> observableKinds = {"displacement", "rotation", "frequency", "reaction", "stress"};
        if (!observableKinds.contains(quantityName))
            addIssue(issues, QStringLiteral("OBSERVABLE_UNSUPPORTED"), QStringLiteral("observables[%1].quantity").arg(i),
                     QStringLiteral("Unsupported observable '%1'.").arg(quantityName));
        const QString component = raw.value("component").toString();
        QSet<QString> components;
        if (quantityName == QLatin1String("displacement")) components = {"ux", "uy", "uz"};
        else if (quantityName == QLatin1String("rotation")) components = {"rx", "ry", "rz"};
        else if (quantityName == QLatin1String("reaction") || quantityName == QLatin1String("stress"))
            components = {"x", "y", "z", "ux", "uy", "uz"};
        else if (quantityName == QLatin1String("frequency")) components = {"mode"};
        if (!components.contains(component))
            addIssue(issues, QStringLiteral("OBSERVABLE_COMPONENT_UNSUPPORTED"),
                     QStringLiteral("observables[%1].component").arg(i),
                     QStringLiteral("Component '%1' is not valid for observable '%2'.").arg(component, quantityName));
        if (modelType == QLatin1String("axialRod") && quantityName == QLatin1String("rotation"))
            addIssue(issues, QStringLiteral("OBSERVABLE_NOT_APPLICABLE_TO_ROD"),
                     QStringLiteral("observables[%1].quantity").arg(i),
                     QStringLiteral("Axial rod templates do not have rotational degrees of freedom."));
        observables.append(QJsonObject{{"selector", selector}, {"quantity", quantityName},
                                       {"component", component}});
    }

    const QJsonObject rawPrecision = spec.value("precision").toObject();
    const QString precisionKey = analysisKind == QLatin1String("modal") ? QStringLiteral("frequencyRelative")
        : (analysisKind == QLatin1String("linearTransient") ? QStringLiteral("responseRelative")
                                                              : QStringLiteral("displacementRelative"));
    const QJsonValue precisionValue = rawPrecision.value(precisionKey);
    rejectUnknownFields(rawPrecision, {precisionKey}, QStringLiteral("precision"), issues);
    if (!precisionValue.isDouble() || !std::isfinite(precisionValue.toDouble())
        || precisionValue.toDouble() <= 0.0 || precisionValue.toDouble() >= 1.0)
        addIssue(issues, QStringLiteral("PRECISION_TARGET_MISSING"), QStringLiteral("precision.%1").arg(precisionKey),
                 QStringLiteral("Provide an explicit relative target in (0,1)."));
    QJsonObject precision{{precisionKey, precisionValue.toDouble()}};

    result.issues = issues;
    result.valid = issues.isEmpty();
    if (!result.valid) return result;
    result.normalizedSpec = QJsonObject{
        {"schemaVersion", SchemaVersion}, {"modelType", modelType},
        {"name", spec.value("name").toString(modelType)}, {"geometry", geometry},
        {"material", material}, {"section", section}, {"analysis", analysis},
        {"supports", supports}, {"loads", loads}, {"initialConditions", initialConditions},
        {"observables", observables}, {"precision", precision}};
    return result;
}

RTrustedMechanicsService::CandidateResult RTrustedMechanicsService::createCandidate(
    const QJsonObject &spec, const QString &sourceId, const QString &modelRevision)
{
    CandidateResult result;
    const ValidationResult validation = validate(spec);
    if (!validation.valid) {
        result.code = validation.issues.first().toObject().value("code").toString();
        result.message = validation.issues.first().toObject().value("message").toString();
        return result;
    }
    if (sourceId.trimmed().isEmpty() || modelRevision.trimmed().isEmpty()) {
        result.code = QStringLiteral("SOURCE_BINDING_MISSING");
        result.message = QStringLiteral("Candidate creation requires a nonempty source ID and model revision.");
        return result;
    }
    const QJsonObject normalized = validation.normalizedSpec;
    const QString modelType = normalized.value("modelType").toString();
    const QJsonObject geometry = normalized.value("geometry").toObject();
    const uint segments = uint(geometry.value("segments").toInt());
    const double length = geometry.value("length").toDouble();

    RModel model;
    model.setName(normalized.value("name").toString());
    model.setDescription(QStringLiteral("Trusted mechanics candidate; source %1 revision %2").arg(sourceId, modelRevision));
    model.setNNodes(segments + 1);
    for (uint i = 0; i <= segments; ++i)
        model.getNode(i).set(length * double(i) / double(segments), 0.0, 0.0);
    const RElementType elementType = modelType == QLatin1String("axialRod") ? R_ELEMENT_TRUSS1 : R_ELEMENT_BEAM1;
    for (uint i = 0; i < segments; ++i) {
        RElement element(elementType);
        element.setNodeId(0, i);
        element.setNodeId(1, i + 1);
        model.addElement(element, true, 0);
    }

    auto preflightSelectors = [&](const QString &field) {
        const QJsonArray entries = normalized.value(field).toArray();
        for (int i = 0; i < entries.size(); ++i) {
            const QJsonObject selector = entries.at(i).toObject().value("selector").toObject();
            const QJsonObject resolved = resolveSelector(model, selector, modelRevision, modelRevision);
            if (!resolved.value("valid").toBool()) {
                result.code = resolved.value("code").toString(QStringLiteral("SELECTOR_RESOLUTION_FAILED"));
                result.message = QStringLiteral("%1[%2] selector could not be resolved: %3")
                    .arg(field).arg(i).arg(result.code);
                return false;
            }
        }
        return true;
    };
    for (const QString &field : {QStringLiteral("supports"), QStringLiteral("loads"),
                                 QStringLiteral("initialConditions"), QStringLiteral("observables")}) {
        if (!preflightSelectors(field)) return result;
    }

    const QJsonObject specMaterial = normalized.value("material").toObject();
    RMaterial material(RMaterial::Solid);
    material.setName(specMaterial.value("name").toString());
    const QByteArray materialSeed = summaryDigest(specMaterial).toLatin1();
    const QByteArray materialHex = QCryptographicHash::hash(materialSeed, QCryptographicHash::Sha256).toHex().left(32);
    const QString materialUuid = QStringLiteral("{%1-%2-%3-%4-%5}")
        .arg(QString::fromLatin1(materialHex.mid(0, 8)), QString::fromLatin1(materialHex.mid(8, 4)),
             QString::fromLatin1(materialHex.mid(12, 4)), QString::fromLatin1(materialHex.mid(16, 4)),
             QString::fromLatin1(materialHex.mid(20, 12)));
    material.setID(QUuid(materialUuid));
    RMaterialProperty modulus(RMaterialProperty::ModulusOfElasiticity);
    modulus.add(293.15, specMaterial.value("youngModulus").toDouble()); material.add(modulus);
    RMaterialProperty poisson(RMaterialProperty::PoissonRatio);
    poisson.add(293.15, specMaterial.value("poissonRatio").toDouble()); material.add(poisson);
    RMaterialProperty density(RMaterialProperty::Density);
    density.add(293.15, specMaterial.value("density").toDouble()); material.add(density);
    // The current structural solver marks the STRESS/STRESS_MODAL material lane
    // incomplete without this property. Thermal conditions are outside this
    // ProblemSpec, so use the neutral value and expose it in inspection below.
    RMaterialProperty thermalExpansion(RMaterialProperty::ThermalExpansionCoefficient);
    thermalExpansion.add(293.15, 0.0); material.add(thermalExpansion);

    RLine &line = model.getLine(0);
    line.setName(modelType == QLatin1String("axialRod") ? QStringLiteral("Axial rod") : QStringLiteral("Cantilever beam"));
    line.setCrossArea(normalized.value("section").toObject().value("area").toDouble());
    line.setMaterial(material);
    if (modelType == QLatin1String("cantileverBeam")) {
        const QJsonObject section = normalized.value("section").toObject();
        const QJsonArray localY = section.value("localY").toArray();
        line.setSecondMomentY(section.value("iy").toDouble());
        line.setSecondMomentZ(section.value("iz").toDouble());
        line.setTorsionalConstant(section.value("torsionalConstant").toDouble());
        line.setSectionModulusY(section.value("wy").toDouble());
        line.setSectionModulusZ(section.value("wz").toDouble());
        line.setTorsionalSectionModulus(section.value("wt").toDouble());
        line.setLocalYDirection(RR3Vector(localY.at(0).toDouble(), localY.at(1).toDouble(), localY.at(2).toDouble()));
    }

    uint pointGroupIndex = 0;
    const QJsonArray supports = normalized.value("supports").toArray();
    for (int i = 0; i < supports.size(); ++i) {
        const QJsonObject support = supports.at(i).toObject();
        const QJsonObject selector = support.value("selector").toObject();
        const QString kind = selector.value("kind").toString();
        const QJsonArray dofs = support.value("dofs").toArray();
        if (kind == QLatin1String("allNodes")) {
            addDisplacementCondition(line, dofs);
            continue;
        }
        const QJsonObject resolved = resolveSelector(model, selector, modelRevision, modelRevision);
        const QJsonArray nodes = resolved.value("nodePositions").toArray();
        for (const QJsonValue &node : nodes) {
            addPointGroup(model, uint(node.toInt()), QStringLiteral("Support %1").arg(i + 1), material, pointGroupIndex);
            addDisplacementCondition(model.getPoint(pointGroupIndex - 1), dofs);
        }
    }

    const QJsonArray loads = normalized.value("loads").toArray();
    for (int i = 0; i < loads.size(); ++i) {
        const QJsonObject load = loads.at(i).toObject();
        const QJsonObject resolved = resolveSelector(model, load.value("selector").toObject(), modelRevision, modelRevision);
        const QJsonArray components = load.value("components").toArray();
        QJsonArray history = load.value("history").toArray();
        if (history.isEmpty()) history.append(QJsonObject{{"time", 0.0}, {"components", components}});
        for (const QJsonValue &node : resolved.value("nodePositions").toArray()) {
            addPointGroup(model, uint(node.toInt()), QStringLiteral("Load %1").arg(i + 1), material, pointGroupIndex);
            RBoundaryCondition force(R_BOUNDARY_CONDITION_FORCE);
            const QList<RVariableType> variables = {R_VARIABLE_FORCE_X, R_VARIABLE_FORCE_Y, R_VARIABLE_FORCE_Z};
            for (int axis = 0; axis < 3; ++axis) {
                RConditionComponent &component = force.getComponent(force.findComponentPosition(variables.at(axis)));
                for (const QJsonValue &sampleValue : history) {
                    const QJsonObject sample = sampleValue.toObject();
                    component.add(sample.value("time").toDouble(), sample.value("components").toArray().at(axis).toDouble());
                }
            }
            model.getPoint(pointGroupIndex - 1).addBoundaryCondition(force);
        }
    }

    const QJsonArray initialConditions = normalized.value("initialConditions").toArray();
    for (int i = 0; i < initialConditions.size(); ++i) {
        const QJsonObject initial = initialConditions.at(i).toObject();
        const QJsonObject resolved = resolveSelector(model, initial.value("selector").toObject(), modelRevision, modelRevision);
        const QString type = initial.value("type").toString();
        RInitialConditionType icType = R_INITIAL_CONDITION_DISPLACEMENT;
        QList<RVariableType> variables;
        Dimension dimension = Dimension::Length;
        if (type == QLatin1String("velocity")) { icType = R_INITIAL_CONDITION_VELOCITY; variables = {R_VARIABLE_VELOCITY_X, R_VARIABLE_VELOCITY_Y, R_VARIABLE_VELOCITY_Z}; dimension = Dimension::Velocity; }
        else if (type == QLatin1String("rotation")) { icType = R_INITIAL_CONDITION_ROTATION; variables = {R_VARIABLE_ROTATION_X, R_VARIABLE_ROTATION_Y, R_VARIABLE_ROTATION_Z}; dimension = Dimension::Rotation; }
        else if (type == QLatin1String("angularVelocity")) { icType = R_INITIAL_CONDITION_ANGULAR_VELOCITY; variables = {R_VARIABLE_ANGULAR_VELOCITY_X, R_VARIABLE_ANGULAR_VELOCITY_Y, R_VARIABLE_ANGULAR_VELOCITY_Z}; dimension = Dimension::AngularVelocity; }
        else variables = {R_VARIABLE_DISPLACEMENT_X, R_VARIABLE_DISPLACEMENT_Y, R_VARIABLE_DISPLACEMENT_Z};
        Q_UNUSED(dimension);
        for (const QJsonValue &node : resolved.value("nodePositions").toArray()) {
            addPointGroup(model, uint(node.toInt()), QStringLiteral("Initial condition %1").arg(i + 1), material, pointGroupIndex);
            RInitialCondition condition(icType);
            const QJsonArray values = initial.value("components").toArray();
            for (int axis = 0; axis < 3; ++axis) {
                RConditionComponent &component = condition.getComponent(condition.findComponentPosition(variables.at(axis)));
                component.add(0.0, values.at(axis).toDouble());
            }
            model.getPoint(pointGroupIndex - 1).addInitialCondition(condition);
        }
    }

    const QJsonObject analysis = normalized.value("analysis").toObject();
    const QString analysisKind = analysis.value("kind").toString();
    if (analysisKind == QLatin1String("modal")) {
        model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        RModalSetup &modal = model.getProblemSetup().getModalSetup();
        modal.setMethod(R_MODAL_MULTIPLE_MODES);
        modal.setNModesToExtract(uint(analysis.value("modes").toInt()));
    } else {
        model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
        if (analysisKind == QLatin1String("linearTransient")) {
            RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
            dynamics.setEnabled(true);
            dynamics.setMethod(RStructuralTimeIntegrationMethod::Newmark);
            dynamics.setTimeStep(analysis.value("timeStep").toDouble());
            dynamics.setNSteps(uint(analysis.value("steps").toInt()));
            dynamics.setRayleighAlpha(analysis.value("rayleighAlpha").toDouble());
            dynamics.setRayleighBeta(analysis.value("rayleighBeta").toDouble());
            dynamics.setOutputFrequency(1);
            dynamics.validate();
        }
    }

    for (uint i = 0; i < model.getNLines(); ++i) disableEnvironmentConditions(model.getLine(i));
    for (uint i = 0; i < model.getNPoints(); ++i) disableEnvironmentConditions(model.getPoint(i));

    result.valid = true;
    result.code = QStringLiteral("CANDIDATE_READY");
    result.message = QStringLiteral("Detached candidate created from validated ProblemSpec.");
    result.normalizedSpec = normalized;
    result.model = model;
    result.inspection = inspect(result.model, sourceId, modelRevision, normalized);
    return result;
}

QJsonObject RTrustedMechanicsService::resolveSelector(const RModel &model,
                                                       const QJsonObject &selector,
                                                       const QString &requestedRevision,
                                                       const QString &currentRevision)
{
    if (requestedRevision.trimmed().isEmpty() || currentRevision.trimmed().isEmpty())
        return QJsonObject{{"valid", false}, {"code", "SELECTOR_MODEL_REVISION_MISSING"},
                           {"requestedRevision", requestedRevision}, {"currentRevision", currentRevision}};
    if (requestedRevision != currentRevision)
        return QJsonObject{{"valid", false}, {"code", "SELECTOR_STALE_MODEL_REVISION"},
                           {"requestedRevision", requestedRevision}, {"currentRevision", currentRevision}};
    const QString kind = selector.value("kind").toString();
    QJsonArray positions;
    QJsonArray physicalIds;
    if (model.getNNodes() == 0)
        return QJsonObject{{"valid", false}, {"code", "SELECTOR_MODEL_EMPTY"},
                           {"modelRevision", currentRevision}};
    double minX = model.getNode(0).getX(), maxX = minX;
    for (uint i = 1; i < model.getNNodes(); ++i) {
        minX = std::min(minX, model.getNode(i).getX());
        maxX = std::max(maxX, model.getNode(i).getX());
    }
    if (kind == QLatin1String("fixedEnd") || kind == QLatin1String("freeEnd")) {
        const double target = kind == QLatin1String("fixedEnd") ? minX : maxX;
        for (uint i = 0; i < model.getNNodes(); ++i)
            if (std::abs(model.getNode(i).getX() - target) <= 1.0e-12 * std::max(1.0, maxX - minX)) positions.append(int(i));
    } else if (kind == QLatin1String("allNodes")) {
        for (uint i = 0; i < model.getNNodes(); ++i) positions.append(int(i));
    } else if (kind == QLatin1String("position")) {
        double target = 0.0;
        const QJsonValue x = selector.value("x");
        if (x.isDouble()) target = x.toDouble();
        else {
            QJsonArray localIssues;
            if (!quantity(x, Dimension::Length, QStringLiteral("selector.x"), localIssues, target))
                return QJsonObject{{"valid", false}, {"code", localIssues.first().toObject().value("code")},
                                   {"issues", localIssues}, {"modelRevision", currentRevision}};
        }
        double tolerance = 0.0;
        const QJsonValue rawTolerance = selector.value("tolerance");
        if (rawTolerance.isDouble()) tolerance = rawTolerance.toDouble();
        else if (rawTolerance.isObject()) {
            QJsonArray localIssues;
            if (!quantity(rawTolerance, Dimension::Length, QStringLiteral("selector.tolerance"), localIssues, tolerance))
                return QJsonObject{{"valid", false}, {"code", localIssues.first().toObject().value("code")},
                                   {"issues", localIssues}, {"modelRevision", currentRevision}};
        } else tolerance = std::max(1.0e-12, 1.0e-9 * (maxX - minX));
        double best = std::numeric_limits<double>::infinity();
        uint bestIndex = RConstants::eod;
        bool tied = false;
        for (uint i = 0; i < model.getNNodes(); ++i) {
            const double distance = std::abs(model.getNode(i).getX() - target);
            if (distance < best) { best = distance; bestIndex = i; tied = false; }
            else if (std::abs(distance - best) <= std::numeric_limits<double>::epsilon() * std::max(1.0, best)) tied = true;
        }
        if (bestIndex == RConstants::eod || best > tolerance || tied)
            return QJsonObject{{"valid", false}, {"code", tied ? "SELECTOR_AMBIGUOUS" : "SELECTOR_NOT_FOUND"},
                               {"distance", best}, {"tolerance", tolerance}, {"modelRevision", currentRevision}};
        positions.append(int(bestIndex));
    } else {
        return QJsonObject{{"valid", false}, {"code", "SELECTOR_UNSUPPORTED"},
                           {"modelRevision", currentRevision}};
    }
    for (const QJsonValue &position : positions)
        physicalIds.append(double(model.getNodePhysicalId(uint(position.toInt()))));
    return QJsonObject{{"valid", true}, {"selector", selector}, {"modelRevision", currentRevision},
                       {"nodePositions", positions}, {"physicalIds", physicalIds}};
}

QJsonObject RTrustedMechanicsService::physicalSummary(const QJsonObject &spec)
{
    const QJsonObject analysis = spec.value("analysis").toObject();
    QJsonObject physicalAnalysis{{"kind", analysis.value("kind")}};
    if (analysis.contains("duration")) physicalAnalysis.insert("duration", analysis.value("duration"));
    if (analysis.contains("rayleighAlpha")) physicalAnalysis.insert("rayleighAlpha", analysis.value("rayleighAlpha"));
    if (analysis.contains("rayleighBeta")) physicalAnalysis.insert("rayleighBeta", analysis.value("rayleighBeta"));
    return QJsonObject{
        {"schemaVersion", SchemaVersion}, {"modelType", spec.value("modelType")},
        {"geometry", QJsonObject{{"length", spec.value("geometry").toObject().value("length")}}},
        {"material", spec.value("material")}, {"section", spec.value("section")},
        {"analysis", physicalAnalysis}, {"supports", spec.value("supports")},
        {"loads", spec.value("loads")}, {"initialConditions", spec.value("initialConditions")},
        {"observables", spec.value("observables")}};
}

QJsonObject RTrustedMechanicsService::numericalSummary(const QJsonObject &spec)
{
    const QJsonObject analysis = spec.value("analysis").toObject();
    QJsonObject numericalAnalysis{{"kind", analysis.value("kind")}};
    if (analysis.value("kind") == QLatin1String("linearTransient"))
        numericalAnalysis.insert("integrationMethod", QStringLiteral("Newmark"));
    for (const QString &key : {QStringLiteral("modes"), QStringLiteral("timeStep"), QStringLiteral("steps")})
        if (analysis.contains(key)) numericalAnalysis.insert(key, analysis.value(key));
    return QJsonObject{{"mesh", QJsonObject{{"segments", spec.value("geometry").toObject().value("segments")}}},
                       {"analysis", numericalAnalysis}, {"precision", spec.value("precision")}};
}

QString RTrustedMechanicsService::summaryDigest(const QJsonObject &summary)
{
    const QByteArray canonical = QJsonDocument(canonicalObject(summary)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QJsonObject RTrustedMechanicsService::inspect(const RModel &model, const QString &sourceId,
                                               const QString &modelRevision,
                                               const QJsonObject &normalizedSpec,
                                               const QJsonObject &jobEvidence)
{
    QJsonArray nodes;
    for (uint i = 0; i < model.getNNodes(); ++i) {
        const RNode &node = model.getNode(i);
        nodes.append(QJsonObject{{"position", int(i)}, {"physicalId", double(model.getNodePhysicalId(i))},
                                 {"xyz_m", QJsonArray{node.getX(), node.getY(), node.getZ()}}});
    }
    QJsonArray elements;
    for (uint i = 0; i < model.getNElements(); ++i) {
        const RElement &element = model.getElement(i);
        QJsonArray connectivity;
        for (uint j = 0; j < element.size(); ++j) connectivity.append(int(element.getNodeId(j)));
        elements.append(QJsonObject{{"position", int(i)}, {"physicalId", double(element.getPhysicalId())},
                                    {"type", int(element.getType())}, {"connectivity", connectivity}});
    }
    QJsonArray lines;
    for (uint i = 0; i < model.getNLines(); ++i) {
        const RLine &line = model.getLine(i);
        QJsonObject summary = groupSummary(line);
        QJsonObject section{{"area_m2", line.getCrossArea()}};
        section.insert("iy_m4", line.getSecondMomentY());
        section.insert("iz_m4", line.getSecondMomentZ());
        section.insert("torsionalConstant_m4", line.getTorsionalConstant());
        section.insert("wy_m3", line.getSectionModulusY());
        section.insert("wz_m3", line.getSectionModulusZ());
        section.insert("wt_m3", line.getTorsionalSectionModulus());
        const RR3Vector &localY = line.getLocalYDirection();
        section.insert("localY", QJsonArray{localY[0], localY[1], localY[2]});
        summary.insert("section", section);
        lines.append(summary);
    }
    QJsonArray points;
    for (uint i = 0; i < model.getNPoints(); ++i) {
        const RPoint &point = model.getPoint(i);
        QJsonObject summary = groupSummary(point);
        summary.insert("volume_m3", point.getVolume());
        points.append(summary);
    }
    QJsonArray surfaces;
    for (uint i = 0; i < model.getNSurfaces(); ++i) {
        const RSurface &surface = model.getSurface(i);
        const RElementFormulation formulation = surface.getStructuralFormulation();
        QJsonObject summary = groupSummary(surface);
        summary.insert("thickness_m", surface.getThickness());
        summary.insert("structuralFormulation", int(formulation));
        summary.insert("structuralFormulationId", RElementDescriptor::formulationToId(formulation));
        surfaces.append(summary);
    }
    QJsonArray volumes;
    for (uint i = 0; i < model.getNVolumes(); ++i) volumes.append(groupSummary(model.getVolume(i)));

    const RProblemTaskItem &task = model.getProblemTaskTree();
    const RProblemSetup &setup = model.getProblemSetup();
    const RStructuralDynamicsSetup &dynamics = setup.getStructuralDynamicsSetup();
    const RModalSetup &modal = setup.getModalSetup();
    QJsonArray modalDampingRatios;
    for (double ratio : dynamics.getModalDampingRatios()) modalDampingRatios.append(ratio);
    QJsonObject analysis{{"problemType", int(task.getProblemType())},
                         {"modal", QJsonObject{{"method", int(modal.getMethod())},
                                               {"modes", int(modal.getNModesToExtract())},
                                               {"nIterations", int(modal.getNIterations())},
                                               {"convergenceValue", modal.getConvergenceValue()}}},
                         {"structuralDynamics", QJsonObject{{"enabled", dynamics.getEnabled()},
                             {"method", int(dynamics.getMethod())}, {"timeStep_s", dynamics.getTimeStep()},
                             {"rayleighAlpha_1_s", dynamics.getRayleighAlpha()},
                             {"rayleighBeta_s", dynamics.getRayleighBeta()},
                             {"rhoInfinity", dynamics.getRhoInfinity()},
                             {"newmarkGamma", dynamics.getNewmarkGamma()},
                             {"newmarkBeta", dynamics.getNewmarkBeta()},
                             {"modalDampingRatios", modalDampingRatios},
                             {"steps", int(dynamics.getNSteps())},
                             {"outputFrequency", int(dynamics.getOutputFrequency())}}}};
    QJsonObject modelState{{"name", model.getName()}, {"description", model.getDescription()},
                           {"nodes", nodes}, {"elements", elements}, {"lines", lines}, {"points", points},
                           {"surfaces", surfaces}, {"volumes", volumes}, {"analysis", analysis},
                           {"solverCompatibilityDefaults", QJsonArray{QJsonObject{
                               {"property", "thermalExpansionCoefficient"}, {"value", 0.0},
                               {"unit", "1/K"},
                               {"reason", "Required by the structural solver material mask; thermal conditions are not part of this ProblemSpec lane."}}}}};
    const QJsonObject observedPhysicalState{{"nodes", nodes}, {"elements", elements}, {"lines", lines},
                                             {"points", points}, {"surfaces", surfaces}, {"volumes", volumes},
                                             {"analysis", analysis}};
    const QString fingerprint = RModelCommandService::stateFingerprint(model);
    QJsonObject physical, numerical;
    QJsonArray selectorBindings;
    if (!normalizedSpec.isEmpty()) {
        physical = physicalSummary(normalizedSpec);
        numerical = numericalSummary(normalizedSpec);
        for (const QString &field : {QStringLiteral("supports"), QStringLiteral("loads"),
                                     QStringLiteral("initialConditions"), QStringLiteral("observables")}) {
            const QJsonArray entries = normalizedSpec.value(field).toArray();
            for (int i = 0; i < entries.size(); ++i) {
                const QJsonObject selector = entries.at(i).toObject().value("selector").toObject();
                QJsonObject binding = resolveSelector(model, selector, modelRevision, modelRevision);
                binding.insert("field", field);
                binding.insert("index", i);
                selectorBindings.append(binding);
            }
        }
    }
    return QJsonObject{{"operation", "analysis.inspect"}, {"schemaVersion", SchemaVersion},
                       {"source", QJsonObject{{"sourceId", sourceId}, {"modelRevision", modelRevision},
                                               {"modelFingerprint", fingerprint}}},
                       {"model", modelState}, {"mesh", QJsonObject{{"nodeCount", int(model.getNNodes())},
                            {"elementCount", int(model.getNElements())}, {"lineGroupCount", int(model.getNLines())},
                            {"surfaceGroupCount", int(model.getNSurfaces())},
                            {"volumeGroupCount", int(model.getNVolumes())}}},
                       {"physicalSummary", physical}, {"physicalSummaryBasis", "normalizedProblemSpec"},
                       {"physicalSummaryDigest", physical.isEmpty() ? QString() : summaryDigest(physical)},
                       {"observedPhysicalState", observedPhysicalState},
                       {"observedPhysicalStateDigest", summaryDigest(observedPhysicalState)},
                       {"numericalSummary", numerical}, {"numericalSummaryDigest", numerical.isEmpty() ? QString() : summaryDigest(numerical)},
                       {"selectorBindings", selectorBindings},
                       {"job", jobEvidence}, {"resultSource", jobEvidence.value("resultSource").toObject()}};
}

QJsonObject RTrustedMechanicsService::diagnose(const QJsonObject &spec, const RModel *model,
                                                const QString &sourceId, const QString &modelRevision,
                                                const QJsonObject &jobEvidence)
{
    QJsonArray issues;
    const ValidationResult validation = validate(spec);
    for (const QJsonValue &value : validation.issues) {
        const QJsonObject source = value.toObject();
        const QString code = source.value("code").toString();
        QJsonArray operations;
        QJsonArray causes;
        bool confirmation = true;
        if (code.startsWith("RESULT_")) { operations.append("refreshResultAndRecheck"); confirmation = false; }
        else if (code.startsWith("SUPPORT_") || code == QLatin1String("SUPPORTS_MISSING")) operations.append("proposeSupportChange");
        else if (code.startsWith("LOAD")) operations.append("requestLoadClarification");
        else if (code.startsWith("MATERIAL") || source.value("path").toString().startsWith("material."))
            operations.append("proposeMaterialAssignment");
        else operations.append("editProblemSpec");
        if (code.startsWith("UNIT_")) causes.append("quantity_unit_or_dimension_mismatch");
        else if (code.startsWith("SELECTOR_")) causes.append("semantic_selector_not_resolved");
        else if (code.startsWith("SUPPORT_")) causes.append("missing_support_direction");
        else if (code.startsWith("LOAD")) causes.append("load_definition_incomplete_or_ambiguous");
        else if (code.startsWith("MATERIAL") || source.value("path").toString().startsWith("material."))
            causes.append("material_definition_incomplete");
        else causes.append("invalid_or_unsupported_problem_specification");
        QJsonObject evidence = source.value("evidence").toObject();
        evidence.insert("path", source.value("path"));
        evidence.insert("message", source.value("message"));
        addDiagnostic(issues, code, source.value("severity").toString(), source.value("message").toString(),
                      evidence, causes, operations, confirmation, QJsonArray{source.value("path")});
    }

    const QString modelType = spec.value("modelType").toString();
    QSet<QString> missingSupportDirections;
    for (const QJsonValue &value : validation.issues) {
        const QJsonObject issue = value.toObject();
        if (issue.value("code") != QLatin1String("SUPPORT_MISSING_DOF")) continue;
        const QString missingDirection = issue.value("evidence").toObject().value("missingDirection").toString();
        if (!missingDirection.isEmpty()) missingSupportDirections.insert(missingDirection);
    }
    QJsonObject modelSupportEvidence;
    if (model) {
        modelSupportEvidence = actualSupportEvidence(*model, modelType);
        for (const QJsonValue &value : modelSupportEvidence.value("missingDirections").toArray())
            missingSupportDirections.insert(value.toString());
    }
    const bool modelUnderconstrained = modelSupportEvidence.value("hasUnrestrainedRigidMode").toBool();
    if (!missingSupportDirections.isEmpty() || modelUnderconstrained) {
        QJsonArray orderedMissing;
        for (const QString &dof : {QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz"),
                                   QStringLiteral("rx"), QStringLiteral("ry"), QStringLiteral("rz"),
                                   QStringLiteral("rigid-body-mode")})
            if (missingSupportDirections.contains(dof)) orderedMissing.append(dof);
        QJsonObject evidence{{"modelType", modelType}, {"missingDirections", orderedMissing},
                             {"supportRankMethod", modelSupportEvidence.value("method").toString(
                                  QStringLiteral("problem-spec-semantic-direction-coverage"))},
                             {"automaticPhysicalEdit", false}};
        if (!modelSupportEvidence.isEmpty()) evidence.insert("modelSupport", modelSupportEvidence);
        QJsonArray operations{"proposeSupportChange"};
        addDiagnostic(issues, QStringLiteral("BC_UNDERCONSTRAINED"), QStringLiteral("error"),
            QStringLiteral("Support configuration leaves expected rigid-body or rod transverse modes free."),
            evidence, QJsonArray{"missing_support_direction", "unsupported_free_mode"}, operations, true);
    }

    if (validation.valid) {
        const QJsonObject normalized = validation.normalizedSpec;
        bool hasNonzeroLoad = false;
        for (const QJsonValue &loadValue : normalized.value("loads").toArray()) {
            const QJsonObject load = loadValue.toObject();
            for (const QJsonValue &component : load.value("components").toArray())
                hasNonzeroLoad = hasNonzeroLoad || std::abs(component.toDouble()) > 0.0;
            for (const QJsonValue &sampleValue : load.value("history").toArray())
                for (const QJsonValue &component : sampleValue.toObject().value("components").toArray())
                    hasNonzeroLoad = hasNonzeroLoad || std::abs(component.toDouble()) > 0.0;
        }
        bool hasNonzeroInitialCondition = false;
        if (normalized.value("analysis").toObject().value("kind") == QLatin1String("linearTransient")) {
            for (const QJsonValue &initialValue : normalized.value("initialConditions").toArray())
                for (const QJsonValue &component : initialValue.toObject().value("components").toArray())
                    hasNonzeroInitialCondition = hasNonzeroInitialCondition || std::abs(component.toDouble()) > 0.0;
        }
        if (normalized.value("analysis").toObject().value("kind") != QLatin1String("modal")
            && !hasNonzeroLoad && !hasNonzeroInitialCondition) {
            addDiagnostic(issues, QStringLiteral("ZERO_EXCITATION"), QStringLiteral("warning"),
                QStringLiteral("No nonzero load or transient initial condition is declared; a zero response may be physically expected."),
                QJsonObject{{"loadCount", normalized.value("loads").toArray().size()},
                            {"hasNonzeroLoad", hasNonzeroLoad},
                            {"initialConditionCount", normalized.value("initialConditions").toArray().size()},
                            {"hasNonzeroInitialCondition", hasNonzeroInitialCondition}},
                QJsonArray{"zero_or_missing_excitation"}, QJsonArray{"requestLoadClarification"}, true);
        }
    }

    if (model) {
        if (validation.valid) {
            const QJsonObject normalized = validation.normalizedSpec;
            diagnoseModelAgainstSpec(*model, normalized, modelRevision, issues);
            const QString expectedModelType = normalized.value("modelType").toString();

            if (model->getNLines() == 0) {
                addDiagnostic(issues, QStringLiteral("MODEL_PHYSICS_CONFIGURATION_MISSING"), QStringLiteral("error"),
                    QStringLiteral("The model has no structural line group to carry section and material data."),
                    QJsonObject{{"lineGroupCount", 0}}, QJsonArray{"structural_group_missing"},
                    QJsonArray{"rebuildDetachedCandidate"}, true);
            } else {
                const RLine &line = model->getLine(0);
                const QJsonObject expectedSection = normalized.value("section").toObject();
                QJsonArray sectionMismatches;
                auto checkSection = [&](const QString &key, double observed) {
                    const double expected = expectedSection.value(key).toDouble();
                    if (!samePhysicalNumber(observed, expected))
                        sectionMismatches.append(QJsonObject{{"property", key}, {"expectedSI", expected}, {"observedSI", observed}});
                };
                checkSection(QStringLiteral("area"), line.getCrossArea());
                if (expectedModelType == QLatin1String("cantileverBeam")) {
                    checkSection(QStringLiteral("iy"), line.getSecondMomentY());
                    checkSection(QStringLiteral("iz"), line.getSecondMomentZ());
                    checkSection(QStringLiteral("torsionalConstant"), line.getTorsionalConstant());
                    checkSection(QStringLiteral("wy"), line.getSectionModulusY());
                    checkSection(QStringLiteral("wz"), line.getSectionModulusZ());
                    checkSection(QStringLiteral("wt"), line.getTorsionalSectionModulus());
                    const QJsonArray expectedLocalY = expectedSection.value("localY").toArray();
                    const RR3Vector &observedLocalY = line.getLocalYDirection();
                    const double localY[3] = {observedLocalY[0], observedLocalY[1], observedLocalY[2]};
                    for (int axis = 0; axis < 3; ++axis)
                        if (!samePhysicalNumber(localY[axis], expectedLocalY.at(axis).toDouble()))
                            sectionMismatches.append(QJsonObject{{"property", QStringLiteral("localY[%1]").arg(axis)},
                                {"expected", expectedLocalY.at(axis)}, {"observed", localY[axis]}});
                }
                if (!sectionMismatches.isEmpty())
                    addDiagnostic(issues, QStringLiteral("MODEL_SECTION_SPEC_MISMATCH"), QStringLiteral("error"),
                        QStringLiteral("Model section values differ from the validated ProblemSpec."),
                        QJsonObject{{"linePhysicalId", double(line.getPhysicalId())}, {"mismatches", sectionMismatches}},
                        QJsonArray{"candidate_modified_after_creation", "source_revision_changed"},
                        QJsonArray{"rebuildDetachedCandidate"}, true, QJsonArray{double(line.getPhysicalId())});

                const QJsonObject expectedMaterial = normalized.value("material").toObject();
                const RMaterial &actualMaterial = line.getMaterial();
                QJsonArray materialIdentityMismatches;
                if (actualMaterial.getState() != RMaterial::Solid)
                    materialIdentityMismatches.append(QJsonObject{{"property", "state"},
                        {"expected", int(RMaterial::Solid)}, {"observed", int(actualMaterial.getState())}});
                if (actualMaterial.getName() != expectedMaterial.value("name").toString())
                    materialIdentityMismatches.append(QJsonObject{{"property", "name"},
                        {"expected", expectedMaterial.value("name")}, {"observed", actualMaterial.getName()}});
                QSet<int> expectedPropertyTypes{
                    int(RMaterialProperty::ModulusOfElasiticity), int(RMaterialProperty::PoissonRatio),
                    int(RMaterialProperty::Density), int(RMaterialProperty::ThermalExpansionCoefficient)};
                QJsonArray unexpectedProperties;
                for (uint i = 0; i < actualMaterial.size(); ++i) {
                    const int type = int(actualMaterial.get(i).getType());
                    if (!expectedPropertyTypes.contains(type))
                        unexpectedProperties.append(QJsonObject{{"propertyType", type}});
                }
                if (!unexpectedProperties.isEmpty())
                    materialIdentityMismatches.append(QJsonObject{{"property", "unsupportedProperties"},
                        {"observed", unexpectedProperties}});
                if (!materialIdentityMismatches.isEmpty())
                    addDiagnostic(issues, QStringLiteral("MATERIAL_SPEC_MISMATCH"), QStringLiteral("error"),
                        QStringLiteral("Material state, name, or property types differ from the supported linear-isotropic ProblemSpec."),
                        QJsonObject{{"linePhysicalId", double(line.getPhysicalId())},
                                    {"mismatches", materialIdentityMismatches}},
                        QJsonArray{"material_type_or_identity_changed"},
                        QJsonArray{"proposeMaterialAssignment"}, true, QJsonArray{double(line.getPhysicalId())});
                const QList<QPair<RMaterialProperty::Type, QPair<QString, double>>> materialChecks = {
                    {RMaterialProperty::ModulusOfElasiticity, {QStringLiteral("youngModulus"), expectedMaterial.value("youngModulus").toDouble()}},
                    {RMaterialProperty::PoissonRatio, {QStringLiteral("poissonRatio"), expectedMaterial.value("poissonRatio").toDouble()}},
                    {RMaterialProperty::Density, {QStringLiteral("density"), expectedMaterial.value("density").toDouble()}},
                    {RMaterialProperty::ThermalExpansionCoefficient,
                     {QStringLiteral("solverCompatibility.thermalExpansionCoefficient"), 0.0}}
                };
                QJsonArray materialMismatches;
                QJsonArray missingProperties;
                for (const auto &check : materialChecks) {
                    const uint propertyPosition = actualMaterial.findPosition(check.first);
                    if (propertyPosition >= actualMaterial.size()) {
                        missingProperties.append(check.second.first);
                        continue;
                    }
                    const RMaterialProperty &property = actualMaterial.get(propertyPosition);
                    if (property.size() == 0) {
                        missingProperties.append(check.second.first);
                        continue;
                    }
                    if (property.size() != 1 || !samePhysicalNumber(property.getKey(0), 293.15))
                        materialMismatches.append(QJsonObject{{"property", check.second.first + QStringLiteral("TemperatureTable")},
                            {"expectedSingleReferenceTemperatureK", 293.15},
                            {"observedCount", int(property.size())},
                            {"observedFirstTemperatureK", property.getKey(0)}});
                    const double observed = property.getValue(0);
                    if (!samePhysicalNumber(observed, check.second.second))
                        materialMismatches.append(QJsonObject{{"property", check.second.first},
                            {"expectedSI", check.second.second}, {"observed", observed}});
                }
                if (!missingProperties.isEmpty())
                    addDiagnostic(issues, QStringLiteral("MATERIAL_PROPERTY_MISSING"), QStringLiteral("error"),
                        QStringLiteral("The assigned material lacks one or more required elastic structural properties."),
                        QJsonObject{{"linePhysicalId", double(line.getPhysicalId())}, {"missingProperties", missingProperties}},
                        QJsonArray{"material_incomplete"}, QJsonArray{"proposeMaterialAssignment"}, true,
                        QJsonArray{double(line.getPhysicalId())});
                if (!materialMismatches.isEmpty())
                    addDiagnostic(issues, QStringLiteral("MATERIAL_SPEC_MISMATCH"), QStringLiteral("error"),
                        QStringLiteral("Assigned material values differ from the validated ProblemSpec."),
                        QJsonObject{{"linePhysicalId", double(line.getPhysicalId())}, {"mismatches", materialMismatches}},
                        QJsonArray{"candidate_modified_after_creation", "material_source_changed"},
                        QJsonArray{"proposeMaterialAssignment"}, true, QJsonArray{double(line.getPhysicalId())});
            }
        }
        const std::vector<RNode> &nodes = model->getNodes();
        for (uint i = 0; i < model->getNElements(); ++i) {
            const RElement &element = model->getElement(i);
            if (R_ELEMENT_TYPE_IS_LINE(element.getType())) {
                double length = 0.0;
                if (!element.findLength(nodes, length) || !std::isfinite(length) || length <= 0.0)
                    addDiagnostic(issues, QStringLiteral("ELEMENT_DEGENERATE"), QStringLiteral("error"),
                        QStringLiteral("A line element has zero or invalid length."),
                        QJsonObject{{"elementPhysicalId", double(element.getPhysicalId())}, {"elementPosition", int(i)}, {"length_m", length}},
                        QJsonArray{"coincident_or_invalid_element_nodes"}, QJsonArray{"inspectGeometry"}, true,
                        QJsonArray{double(element.getPhysicalId())});
            }
        }
        for (uint i = 0; i < model->getNLines(); ++i) {
            if (model->getLine(i).getMaterial().size() == 0)
                addDiagnostic(issues, QStringLiteral("MATERIAL_MISSING"), QStringLiteral("error"),
                    QStringLiteral("A structural line group has no material properties."),
                    QJsonObject{{"linePhysicalId", double(model->getLine(i).getPhysicalId())}},
                    QJsonArray{"material_not_assigned"}, QJsonArray{"proposeMaterialAssignment"}, true,
                    QJsonArray{double(model->getLine(i).getPhysicalId())});
        }
    }

    if (!jobEvidence.isEmpty()) {
        const QString jobRevision = jobEvidence.value("modelRevision").toString();
        const QString jobSource = jobEvidence.value("sourceId").toString();
        const QJsonObject resultSource = jobEvidence.value("resultSource").toObject();
        const bool completed = jobEvidence.value("status").toString() == QLatin1String("completed");
        const QString expectedFingerprint = model ? RModelCommandService::stateFingerprint(*model) : QString();
        const bool resultIdentityComplete = !resultSource.value("path").toString().trimmed().isEmpty()
            && !resultSource.value("sourceId").toString().trimmed().isEmpty()
            && !resultSource.value("modelRevision").toString().trimmed().isEmpty()
            && !resultSource.value("modelFingerprint").toString().trimmed().isEmpty();
        const bool missingIdentity = jobRevision.isEmpty() || jobSource.isEmpty()
            || (completed && (!resultIdentityComplete || expectedFingerprint.isEmpty()));
        // Failed/running jobs do not promise a result binding. Diagnose stale
        // result identity only when a completed job or an actual result source
        // is present, so a failed solve remains an unclassified solver failure.
        if (completed || !resultSource.isEmpty()) {
            if (missingIdentity) {
                addDiagnostic(issues, QStringLiteral("RESULT_SOURCE_IDENTITY_MISSING"), QStringLiteral("error"),
                    QStringLiteral("Completed job evidence must bind a result path to its source, revision, and current model fingerprint."),
                    QJsonObject{{"sourceIdPresent", !jobSource.isEmpty()}, {"modelRevisionPresent", !jobRevision.isEmpty()},
                                {"resultPathPresent", !resultSource.value("path").toString().trimmed().isEmpty()},
                                {"resultSourceIdPresent", !resultSource.value("sourceId").toString().trimmed().isEmpty()},
                                {"resultRevisionPresent", !resultSource.value("modelRevision").toString().trimmed().isEmpty()},
                                {"resultFingerprintPresent", !resultSource.value("modelFingerprint").toString().trimmed().isEmpty()},
                                {"modelFingerprintAvailable", !expectedFingerprint.isEmpty()}},
                    QJsonArray{"result_identity_incomplete"}, QJsonArray{"refreshResultAndRecheck"}, false);
            } else if (jobRevision != modelRevision || jobSource != sourceId
                       || resultSource.value("sourceId").toString() != sourceId
                       || resultSource.value("modelRevision").toString() != modelRevision
                       || resultSource.value("modelFingerprint").toString() != expectedFingerprint) {
                addDiagnostic(issues, QStringLiteral("RESULT_SOURCE_STALE"), QStringLiteral("error"),
                    QStringLiteral("Job evidence is bound to a different source or model revision."),
                    QJsonObject{{"expectedSourceId", sourceId}, {"observedSourceId", jobSource},
                                {"expectedModelRevision", modelRevision}, {"observedModelRevision", jobRevision},
                                {"expectedResultSourceId", sourceId},
                                {"observedResultSourceId", resultSource.value("sourceId")},
                                {"expectedResultModelRevision", modelRevision},
                                {"observedResultModelRevision", resultSource.value("modelRevision")},
                                {"expectedModelFingerprint", expectedFingerprint},
                                {"observedModelFingerprint", resultSource.value("modelFingerprint")}},
                    QJsonArray{"source_revision_changed"}, QJsonArray{"refreshResultAndRecheck"}, false);
            }
        }
    }
    if (jobEvidence.value("status").toString() == QLatin1String("failed")) {
        addDiagnostic(issues, QStringLiteral("SOLVER_FAILURE_UNCLASSIFIED"), QStringLiteral("error"),
            QStringLiteral("Solver failed; the cause is not determined by the available evidence."),
            QJsonObject{{"jobId", jobEvidence.value("jobId")}, {"errorCode", jobEvidence.value("errorCode")}},
            {}, QJsonArray{"saveDiagnosticPackage", "stop"}, false);
    }

    QString status = QStringLiteral("ready");
    for (const QJsonValue &entry : issues) {
        const QString severity = entry.toObject().value("severity").toString();
        if (severity == QLatin1String("error")) { status = QStringLiteral("error"); break; }
        if (severity == QLatin1String("warning")) status = QStringLiteral("warning");
    }
    return QJsonObject{{"operation", "analysis.diagnose"}, {"schemaVersion", SchemaVersion}, {"status", status},
                       {"sourceId", sourceId}, {"modelRevision", modelRevision}, {"issues", issues}};
}

QJsonObject RTrustedMechanicsService::diagnoseFailure(const QJsonObject &spec, const RModel *model,
                                                       const QString &sourceId, const QString &modelRevision,
                                                       const QString &jobId, const QString &errorCode,
                                                       const QString &solverLog)
{
    const QJsonObject job{{"jobId", jobId}, {"status", "failed"}, {"errorCode", errorCode},
                          {"sourceId", sourceId}, {"modelRevision", modelRevision}};
    const QJsonObject report = diagnose(spec, model, sourceId, modelRevision, job);
    const QJsonObject snapshot = model ? inspect(*model, sourceId, modelRevision) : QJsonObject{};
    const QByteArray logBytes = solverLog.toUtf8();
    const QByteArray snapshotBytes = QJsonDocument(canonicalObject(snapshot)).toJson(QJsonDocument::Compact);
    return QJsonObject{{"operation", "analysis.diagnose.failurePackage"}, {"schemaVersion", SchemaVersion}, {"jobId", jobId},
                       {"sourceId", sourceId}, {"modelRevision", modelRevision},
                       {"errorCode", errorCode}, {"causeDetermined", false},
                       {"solverLog", solverLog},
                       {"solverLogSha256", QString::fromLatin1(QCryptographicHash::hash(logBytes, QCryptographicHash::Sha256).toHex())},
                       {"modelSnapshot", snapshot},
                       {"modelSnapshotSha256", QString::fromLatin1(QCryptographicHash::hash(snapshotBytes, QCryptographicHash::Sha256).toHex())},
                       {"diagnosticReport", report}};
}
