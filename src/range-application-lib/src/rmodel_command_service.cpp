#include <rmodel_command_service.h>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <rbl_utils.h>
#include <rbl_error.h>
#include <rml_condition_component.h>
#include <rml_variable.h>

namespace
{
const QString apiVersion = QStringLiteral("range.application/v1");
constexpr int DefaultPageSize = 100;
constexpr int MaximumPageSize = 200;
constexpr int MaximumTableSamples = 32;

QSet<QString> keysOf(const QJsonObject &object)
{
    const auto keys = object.keys();
    return QSet<QString>(keys.begin(), keys.end());
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    return keysOf(object) == expected;
}

bool keysWithin(const QJsonObject &object, const QSet<QString> &allowed)
{
    const auto keys = keysOf(object);
    return std::all_of(keys.cbegin(), keys.cend(), [&allowed](const QString &key) {
        return allowed.contains(key);
    });
}

bool integer(const QJsonValue &value, double minimum, double maximum)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum;
}

bool finiteNumber(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble());
}

QJsonObject diagnostic(const QString &code, const QString &message,
                       const QString &path = QString())
{
    QJsonObject result{{QStringLiteral("code"), code},
                       {QStringLiteral("severity"), QStringLiteral("error")},
                       {QStringLiteral("message"), message}};
    if (!path.isEmpty()) result.insert(QStringLiteral("path"), path);
    return result;
}

QJsonObject timeSolverParameters(const RTimeSolver &time)
{
    QJsonArray times;
    for (double value : time.getTimes()) times.append(value);
    return {{QStringLiteral("enabled"), time.getEnabled()},
            {QStringLiteral("inputStartTimeS"), time.getInputStartTime()},
            {QStringLiteral("inputTimeStepS"), time.getInputTimeStepSize()},
            {QStringLiteral("steps"), static_cast<double>(time.getInputNTimeSteps())},
            {QStringLiteral("outputFrequency"), static_cast<double>(time.getOutputFrequency())},
            {QStringLiteral("timeMarchApproximation"), static_cast<int>(time.getTimeMarchApproximation())},
            {QStringLiteral("currentStep"), static_cast<double>(time.getCurrentTimeStep())},
            {QStringLiteral("computedTimeS"), time.getComputedTime()},
            {QStringLiteral("timesS"), times}};
}

QJsonObject boundaryConditionParameters(const RBoundaryCondition &condition, bool complete = false)
{
    QJsonArray components;
    for (uint i = 0; i < condition.size(); ++i)
    {
        const RConditionComponent &component = condition.getComponent(i);
        QJsonArray samples;
        const uint sampleCount = complete ? component.size()
                                          : std::min<uint>(component.size(), MaximumTableSamples);
        for (uint j = 0; j < sampleCount; ++j)
        {
            samples.append(QJsonObject{{QStringLiteral("timeS"), component.getKey(j)},
                                      {QStringLiteral("value"), component.getValue(j)}});
        }
        components.append(QJsonObject{{QStringLiteral("variableId"), RVariable::getId(component.getType())},
                                      {QStringLiteral("name"), component.getName()},
                                      {QStringLiteral("unit"), component.getUnits()},
                                      {QStringLiteral("enabled"), component.getEnabled()},
                                      {QStringLiteral("samples"), samples},
                                      {QStringLiteral("sampleCount"), static_cast<double>(component.size())},
                                      {QStringLiteral("truncated"), component.size() > sampleCount}});
    }
    const auto &direction = condition.getLocalDirection();
    QJsonArray directionValues{direction[0], direction[1], direction[2]};
    return {{QStringLiteral("type"), RBoundaryCondition::getId(condition.getType())},
            {QStringLiteral("name"), RBoundaryCondition::getName(condition.getType())},
            {QStringLiteral("explicit"), condition.getExplicit()},
            {QStringLiteral("hasLocalDirection"), condition.getHasLocalDirection()},
            {QStringLiteral("explicitLocalDirection"), condition.getExplicitLocalDirection()},
            {QStringLiteral("localDirection"), directionValues},
            {QStringLiteral("components"), components}};
}

QJsonObject materialParameters(const RMaterial &material)
{
    QJsonArray properties;
    for (uint i = 0; i < material.size(); ++i)
    {
        const RMaterialProperty &property = material.get(i);
        QJsonArray samples;
        const uint sampleCount = std::min<uint>(property.size(), MaximumTableSamples);
        for (uint j = 0; j < sampleCount; ++j)
        {
            samples.append(QJsonObject{{QStringLiteral("temperatureK"), property.getKey(j)},
                                       {QStringLiteral("value"), property.getValue(j)}});
        }
        properties.append(QJsonObject{{QStringLiteral("type"), RMaterialProperty::getId(property.getType())},
                                      {QStringLiteral("name"), property.getName()},
                                      {QStringLiteral("unit"), property.getUnits()},
                                      {QStringLiteral("keyUnit"), property.getKeyUnits()},
                                      {QStringLiteral("samples"), samples},
                                      {QStringLiteral("sampleCount"), static_cast<double>(property.size())},
                                      {QStringLiteral("truncated"), property.size() > sampleCount}});
    }
    return {{QStringLiteral("id"), material.getID().toString(QUuid::WithoutBraces)},
            {QStringLiteral("name"), material.getName()},
            {QStringLiteral("state"), material.getState() == RMaterial::Solid
                 ? QStringLiteral("solid") : material.getState() == RMaterial::Gas
                     ? QStringLiteral("gas") : material.getState() == RMaterial::Liquid
                         ? QStringLiteral("liquid") : QStringLiteral("none")},
            {QStringLiteral("properties"), properties}};
}

QJsonObject groupParameters(const RModel &model, uint groupId, bool includeTables)
{
    const REntityGroupType type = model.getEntityGroupType(groupId, true);
    const uint localIndex = model.getEntityGroupPosition(groupId, true);
    const RElementGroup *group = model.getElementGroupPtr(groupId);
    QJsonArray boundaryConditions;
    for (uint i = 0; i < group->getNBoundaryConditions(); ++i)
    {
        const RBoundaryCondition &condition = group->getBoundaryCondition(i);
        boundaryConditions.append(includeTables
            ? boundaryConditionParameters(condition)
            : QJsonObject{{QStringLiteral("type"), RBoundaryCondition::getId(condition.getType())}});
    }
    QJsonArray uniqueNodes;
    QSet<uint> nodeIds;
    bool validMembership = true;
    for (uint i = 0; i < group->size(); ++i)
    {
        const uint elementId = group->get(i);
        if (elementId >= model.getNElements())
        {
            validMembership = false;
            continue;
        }
        const RElement &element = model.getElement(elementId);
        for (uint j = 0; j < element.size(); ++j) nodeIds.insert(element.getNodeId(j));
    }
    QList<uint> sortedNodeIds = nodeIds.values();
    std::sort(sortedNodeIds.begin(), sortedNodeIds.end());
    for (uint nodeId : sortedNodeIds) uniqueNodes.append(static_cast<double>(nodeId));

    const uint physicalId = group->getPhysicalId();
    QJsonObject result{{QStringLiteral("id"), QStringLiteral("g:%1").arg(physicalId)},
                       {QStringLiteral("physicalId"), static_cast<double>(physicalId)},
                       {QStringLiteral("type"), REntityGroup::getTypeName(type)},
                       {QStringLiteral("label"), group->getName()},
                       {QStringLiteral("elementCount"), static_cast<double>(group->size())},
                       {QStringLiteral("nodeCount"), static_cast<double>(nodeIds.size())},
                       {QStringLiteral("meshMembershipValid"), validMembership},
                       {QStringLiteral("material"), materialParameters(group->getMaterial())},
                       {QStringLiteral("boundaryConditions"), boundaryConditions}};
    Q_UNUSED(localIndex)
    return result;
}

QJsonObject fullGroupState(const RModel &model, uint groupId)
{
    const RElementGroup *group = model.getElementGroupPtr(groupId);
    const REntityGroupType type = model.getEntityGroupType(groupId, true);
    QJsonArray conditions;
    for (uint i = 0; i < group->getNBoundaryConditions(); ++i)
        conditions.append(boundaryConditionParameters(group->getBoundaryCondition(i), true));
    QJsonArray pointMembers;
    if (type == R_ENTITY_GROUP_POINT)
    {
        QVector<QPair<uint, QJsonObject>> ordered;
        for (uint i = 0; i < group->size(); ++i)
        {
            const uint elementId = group->get(i);
            if (elementId >= model.getNElements())
            {
                ordered.append({std::numeric_limits<uint>::max(),
                    QJsonObject{{QStringLiteral("valid"), false}, {QStringLiteral("elementIndex"), static_cast<double>(elementId)}}});
                continue;
            }
            const RElement &element = model.getElement(elementId);
            QJsonArray nodes;
            bool validNodes = true;
            for (uint nodeIndex = 0; nodeIndex < element.size(); ++nodeIndex)
            {
                const uint nodeId = element.getNodeId(nodeIndex);
                if (nodeId >= model.getNNodes())
                {
                    validNodes = false;
                    nodes.append(QJsonObject{{QStringLiteral("valid"), false},
                                             {QStringLiteral("nodeIndex"), static_cast<double>(nodeId)}});
                    continue;
                }
                const RNode &node = model.getNode(nodeId);
                nodes.append(QJsonObject{{QStringLiteral("physicalId"), static_cast<double>(model.getNodePhysicalId(nodeId))},
                                         {QStringLiteral("x"), node.getX()}, {QStringLiteral("y"), node.getY()},
                                         {QStringLiteral("z"), node.getZ()}});
            }
            const uint physicalId = model.getElementPhysicalId(elementId);
            ordered.append({physicalId, QJsonObject{{QStringLiteral("valid"), validNodes},
                {QStringLiteral("physicalId"), static_cast<double>(physicalId)},
                {QStringLiteral("type"), RElement::getId(element.getType())}, {QStringLiteral("nodes"), nodes}}});
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
            return left.first < right.first;
        });
        for (const auto &entry : ordered) pointMembers.append(entry.second);
    }
    return {{QStringLiteral("id"), QStringLiteral("g:%1").arg(group->getPhysicalId())},
            {QStringLiteral("type"), REntityGroup::getTypeName(type)},
            {QStringLiteral("label"), group->getName()},
            {QStringLiteral("elementCount"), static_cast<double>(group->size())},
            {QStringLiteral("material"), group->getMaterial().toJson()},
            {QStringLiteral("boundaryConditions"), conditions},
            {QStringLiteral("pointMembers"), pointMembers}};
}

QString resultFingerprint(const RModel &model, const RModel &candidate,
                          const QString &sourceId, const QString &revision,
                          const QString &commandId, const QJsonObject &arguments,
                          const QJsonObject &before, const QJsonObject &after)
{
    const QJsonObject record{{QStringLiteral("sourceId"), sourceId},
                             {QStringLiteral("revision"), revision},
                             {QStringLiteral("commandId"), commandId},
                             {QStringLiteral("arguments"), arguments},
                             {QStringLiteral("before"), before},
                             {QStringLiteral("after"), after},
                             {QStringLiteral("preStateFingerprint"), RModelCommandService::stateFingerprint(model)},
                             {QStringLiteral("postStateFingerprint"), RModelCommandService::stateFingerprint(candidate)}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(record).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

bool parseTargetId(const QJsonValue &value, uint &physicalId)
{
    if (!value.isString()) return false;
    static const QRegularExpression expression(QStringLiteral("^g:([1-9][0-9]*)$"));
    const QRegularExpressionMatch match = expression.match(value.toString());
    if (!match.hasMatch()) return false;
    bool ok = false;
    const qulonglong parsed = match.captured(1).toULongLong(&ok, 10);
    if (!ok || parsed > std::numeric_limits<uint>::max() || parsed == 0
        || parsed == std::numeric_limits<uint>::max()) return false;
    physicalId = static_cast<uint>(parsed);
    return true;
}

const RElementGroup *findGroup(const RModel &model, uint physicalId, REntityGroupType *typeOut,
                               uint *indexOut)
{
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    if (!model.findElementGroupByPhysicalId(physicalId, &type, &index)) return nullptr;
    if (typeOut) *typeOut = type;
    if (indexOut) *indexOut = index;
    switch (type)
    {
        case R_ENTITY_GROUP_POINT: return &model.getPoint(index);
        case R_ENTITY_GROUP_LINE: return &model.getLine(index);
        case R_ENTITY_GROUP_SURFACE: return &model.getSurface(index);
        case R_ENTITY_GROUP_VOLUME: return &model.getVolume(index);
        default: return nullptr;
    }
}

RElementGroup *findGroup(RModel &model, uint physicalId, REntityGroupType *typeOut,
                         uint *indexOut)
{
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    if (!model.findElementGroupByPhysicalId(physicalId, &type, &index)) return nullptr;
    if (typeOut) *typeOut = type;
    if (indexOut) *indexOut = index;
    switch (type)
    {
        case R_ENTITY_GROUP_POINT: return &model.getPoint(index);
        case R_ENTITY_GROUP_LINE: return &model.getLine(index);
        case R_ENTITY_GROUP_SURFACE: return &model.getSurface(index);
        case R_ENTITY_GROUP_VOLUME: return &model.getVolume(index);
        default: return nullptr;
    }
}

QJsonObject timeGridState(const RModel &model)
{
    return timeSolverParameters(model.getTimeSolver());
}

bool readStructuralDynamics(const QJsonObject &arguments, RStructuralDynamicsSetup &setup,
                            QString &code, QString &message)
{
    static const QSet<QString> expected{
        QStringLiteral("enabled"), QStringLiteral("method"), QStringLiteral("timeStep"),
        QStringLiteral("unitSI"), QStringLiteral("steps"), QStringLiteral("outputFrequency"),
        QStringLiteral("rayleighAlpha"), QStringLiteral("rayleighBeta"),
        QStringLiteral("rhoInfinity"), QStringLiteral("newmarkGamma"),
        QStringLiteral("newmarkBeta"), QStringLiteral("modalDampingRatios")};
    if (!exactKeys(arguments, expected)
        || !arguments.value(QStringLiteral("enabled")).isBool()
        || arguments.value(QStringLiteral("unitSI")) != QJsonValue(true)
        || !arguments.value(QStringLiteral("method")).isString()
        || !finiteNumber(arguments.value(QStringLiteral("timeStep")))
        || !integer(arguments.value(QStringLiteral("steps")), 1, R_TIME_STEP_MAX_NUMBER)
        || !integer(arguments.value(QStringLiteral("outputFrequency")), 1, R_TIME_MAX_OUTPUT_FREQUENCY)
        || !finiteNumber(arguments.value(QStringLiteral("rayleighAlpha")))
        || !finiteNumber(arguments.value(QStringLiteral("rayleighBeta")))
        || !finiteNumber(arguments.value(QStringLiteral("rhoInfinity")))
        || !finiteNumber(arguments.value(QStringLiteral("newmarkGamma")))
        || !finiteNumber(arguments.value(QStringLiteral("newmarkBeta")))
        || !arguments.value(QStringLiteral("modalDampingRatios")).isArray())
    {
        code = QStringLiteral("invalid_parameters");
        message = QStringLiteral("Expected the full SI structural dynamics configuration with exact fields.");
        return false;
    }

    const QString method = arguments.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("newmark")) setup.setMethod(RStructuralTimeIntegrationMethod::Newmark);
    else if (method == QStringLiteral("generalizedAlpha")) setup.setMethod(RStructuralTimeIntegrationMethod::GeneralizedAlpha);
    else if (method == QStringLiteral("centralDifference")) setup.setMethod(RStructuralTimeIntegrationMethod::CentralDifference);
    else
    {
        code = QStringLiteral("invalid_parameters");
        message = QStringLiteral("Unsupported structural dynamics method.");
        return false;
    }

    try
    {
        setup.setEnabled(arguments.value(QStringLiteral("enabled")).toBool());
        setup.setTimeStep(arguments.value(QStringLiteral("timeStep")).toDouble());
        setup.setNSteps(static_cast<uint>(arguments.value(QStringLiteral("steps")).toDouble()));
        setup.setOutputFrequency(static_cast<uint>(arguments.value(QStringLiteral("outputFrequency")).toDouble()));
        setup.setRayleighAlpha(arguments.value(QStringLiteral("rayleighAlpha")).toDouble());
        setup.setRayleighBeta(arguments.value(QStringLiteral("rayleighBeta")).toDouble());
        setup.setRhoInfinity(arguments.value(QStringLiteral("rhoInfinity")).toDouble());
        setup.setNewmarkGamma(arguments.value(QStringLiteral("newmarkGamma")).toDouble());
        setup.setNewmarkBeta(arguments.value(QStringLiteral("newmarkBeta")).toDouble());
        std::vector<double> ratios;
        const QJsonArray jsonRatios = arguments.value(QStringLiteral("modalDampingRatios")).toArray();
        ratios.reserve(static_cast<std::size_t>(jsonRatios.size()));
        for (const QJsonValue &value : jsonRatios)
        {
            if (!finiteNumber(value))
            {
                code = QStringLiteral("invalid_parameters");
                message = QStringLiteral("Modal damping ratios must be finite numbers.");
                return false;
            }
            ratios.push_back(value.toDouble());
        }
        setup.setModalDampingRatios(ratios);
        setup.validate();
    }
    catch (const std::exception &error)
    {
        code = QStringLiteral("invalid_parameters");
        message = QString::fromUtf8(error.what());
        return false;
    }
    catch (...)
    {
        code = QStringLiteral("invalid_parameters");
        message = QStringLiteral("Structural dynamics settings failed native validation.");
        return false;
    }

    // The native solver constructs each time as (stepIndex * dt).  Setup
    // validation checks dt and step count separately, so validate their largest
    // generated product before any candidate grid or JSON result is built.
    const double finalTime = static_cast<double>(setup.getNSteps()) * setup.getTimeStep();
    if (!std::isfinite(finalTime))
    {
        code = QStringLiteral("unsupported_parameters");
        message = QStringLiteral("The configured timeStep and steps exceed the maximum finite SI time grid.");
        return false;
    }

    if (setup.getMethod() == RStructuralTimeIntegrationMethod::Newmark
        && (std::fabs(setup.getNewmarkGamma() - 0.5) > 1e-12
            || std::fabs(setup.getNewmarkBeta() - 0.25) > 1e-12))
    {
        code = QStringLiteral("unsupported_parameters");
        message = QStringLiteral("The current native solver supports Newmark average acceleration only (gamma=0.5, beta=0.25).");
        return false;
    }
    if (!setup.getModalDampingRatios().empty()
        && (setup.getRayleighAlpha() != 0.0 || setup.getRayleighBeta() != 0.0))
    {
        code = QStringLiteral("unsupported_parameters");
        message = QStringLiteral("Modal damping and Rayleigh damping cannot be combined by the current native solver.");
        return false;
    }
    return true;
}

QJsonObject encodedCursor(const QString &sourceId, const QString &revision,
                          int offset, int limit, const QString &labelQuery)
{
    const QByteArray data = QJsonDocument(QJsonObject{
        {QStringLiteral("sourceId"), sourceId}, {QStringLiteral("revision"), revision},
        {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit},
        {QStringLiteral("labelQuery"), labelQuery}}).toJson(QJsonDocument::Compact);
    return {{QStringLiteral("cursor"), QString::fromLatin1(
                data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}};
}

bool parsePageRequest(const QString &sourceId, const QString &revision,
                      const QJsonObject &request, int &offset, int &limit,
                      QString &labelQuery, QString &code, QString &message)
{
    if (sourceId.trimmed().isEmpty() || sourceId.size() > 256
        || revision.trimmed().isEmpty() || revision.size() > 256)
    {
        code = QStringLiteral("invalid_context");
        message = QStringLiteral("A source identity and revision are required.");
        return false;
    }
    if (!keysWithin(request, {QStringLiteral("limit"), QStringLiteral("offset"),
                              QStringLiteral("cursor"), QStringLiteral("labelQuery")})
        || (request.contains(QStringLiteral("cursor")) && request.contains(QStringLiteral("offset"))))
    {
        code = QStringLiteral("invalid_pagination");
        message = QStringLiteral("Use only limit and offset or a cursor, with an optional labelQuery.");
        return false;
    }
    limit = DefaultPageSize;
    offset = 0;
    labelQuery.clear();
    if (request.contains(QStringLiteral("limit")))
    {
        if (!integer(request.value(QStringLiteral("limit")), 1, MaximumPageSize))
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("Page limit must be an integer from 1 through 200.");
            return false;
        }
        limit = request.value(QStringLiteral("limit")).toInt();
    }
    if (request.contains(QStringLiteral("labelQuery")))
    {
        if (!request.value(QStringLiteral("labelQuery")).isString()
            || request.value(QStringLiteral("labelQuery")).toString().size() > 128)
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("labelQuery must be a string no longer than 128 characters.");
            return false;
        }
        labelQuery = request.value(QStringLiteral("labelQuery")).toString().trimmed();
    }
    if (request.contains(QStringLiteral("cursor")))
    {
        if (!request.value(QStringLiteral("cursor")).isString())
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("Cursor must be an encoded string.");
            return false;
        }
        const QByteArray encoded = request.value(QStringLiteral("cursor")).toString().toLatin1();
        const QByteArray decoded = QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(decoded, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !exactKeys(document.object(), {QStringLiteral("sourceId"), QStringLiteral("revision"),
                    QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("labelQuery")}))
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("Cursor is malformed.");
            return false;
        }
        const QJsonObject cursor = document.object();
        if (cursor.value(QStringLiteral("sourceId")).toString() != sourceId)
        {
            code = QStringLiteral("stale_target_cursor");
            message = QStringLiteral("Cursor belongs to another source identity.");
            return false;
        }
        if (cursor.value(QStringLiteral("revision")).toString() != revision)
        {
            code = QStringLiteral("stale_revision");
            message = QStringLiteral("Targets changed after this cursor was issued.");
            return false;
        }
        if (!integer(cursor.value(QStringLiteral("offset")), 0, std::numeric_limits<int>::max())
            || !integer(cursor.value(QStringLiteral("limit")), 1, MaximumPageSize)
            || !cursor.value(QStringLiteral("labelQuery")).isString())
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("Cursor contains invalid pagination state.");
            return false;
        }
        if (request.contains(QStringLiteral("limit"))
            && limit != cursor.value(QStringLiteral("limit")).toInt())
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("Page limit cannot change while following a cursor.");
            return false;
        }
        if (request.contains(QStringLiteral("labelQuery"))
            && labelQuery != cursor.value(QStringLiteral("labelQuery")).toString())
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("labelQuery cannot change while following a cursor.");
            return false;
        }
        offset = cursor.value(QStringLiteral("offset")).toInt();
        limit = cursor.value(QStringLiteral("limit")).toInt();
        labelQuery = cursor.value(QStringLiteral("labelQuery")).toString();
    }
    else if (request.contains(QStringLiteral("offset")))
    {
        if (!integer(request.value(QStringLiteral("offset")), 0, std::numeric_limits<int>::max()))
        {
            code = QStringLiteral("invalid_pagination");
            message = QStringLiteral("Offset must be a nonnegative integer.");
            return false;
        }
        offset = request.value(QStringLiteral("offset")).toInt();
    }
    return true;
}

QJsonObject pageError(const QString &sourceId, const QString &revision,
                      const QString &code, const QString &message)
{
    return {{QStringLiteral("apiVersion"), apiVersion}, {QStringLiteral("status"), QStringLiteral("rejected")},
            {QStringLiteral("sourceId"), sourceId}, {QStringLiteral("revision"), revision},
            {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

QJsonObject currentResourceState(const RModel &model, RModelCommandService::Delta::Kind kind,
                                 uint physicalId = 0, RBoundaryConditionType boundaryType = R_BOUNDARY_CONDITION_NONE)
{
    if (kind == RModelCommandService::Delta::Kind::Modal)
        return RModelCommandService::modalParameters(model.getProblemSetup().getModalSetup());
    if (kind == RModelCommandService::Delta::Kind::StructuralDynamics)
        return {{QStringLiteral("structuralDynamics"), RModelCommandService::structuralDynamicsParameters(
                    model.getProblemSetup().getStructuralDynamicsSetup())},
                {QStringLiteral("timeSolver"), timeGridState(model)}};
    REntityGroupType type = R_ENTITY_GROUP_NONE;
    uint index = RConstants::eod;
    const RElementGroup *group = findGroup(model, physicalId, &type, &index);
    if (!group) return {};
    Q_UNUSED(type)
    Q_UNUSED(index)
    if (kind == RModelCommandService::Delta::Kind::Material)
        return materialParameters(group->getMaterial());
    if (kind == RModelCommandService::Delta::Kind::Boundary)
    {
        return group->hasBoundaryCondition(boundaryType)
            ? boundaryConditionParameters(group->getBoundaryCondition(boundaryType)) : QJsonObject{};
    }
    return {};
}

bool sameJson(const QJsonObject &left, const QJsonObject &right)
{
    return QJsonDocument(left).toJson(QJsonDocument::Compact)
        == QJsonDocument(right).toJson(QJsonDocument::Compact);
}

void appendSamples(RConditionComponent &component, double timeS, double value)
{
    component.clear();
    component.add(timeS, value);
}

} // namespace

QJsonObject RModelCommandService::modalParameters(const RModalSetup &setup)
{
    return {{QStringLiteral("method"), setup.getMethod() == R_MODAL_MULTIPLE_MODES
                 ? QStringLiteral("multiple") : QStringLiteral("dominant")},
            {QStringLiteral("iterations"), static_cast<double>(setup.getNIterations())},
            {QStringLiteral("modes"), static_cast<double>(setup.getNModesToExtract())},
            {QStringLiteral("tolerance"), setup.getConvergenceValue()},
            {QStringLiteral("unit"), QStringLiteral("1")}};
}

QJsonObject RModelCommandService::structuralDynamicsParameters(const RStructuralDynamicsSetup &setup)
{
    QString method = QStringLiteral("newmark");
    if (setup.getMethod() == RStructuralTimeIntegrationMethod::GeneralizedAlpha)
        method = QStringLiteral("generalizedAlpha");
    else if (setup.getMethod() == RStructuralTimeIntegrationMethod::CentralDifference)
        method = QStringLiteral("centralDifference");
    QJsonArray ratios;
    for (double ratio : setup.getModalDampingRatios()) ratios.append(ratio);
    return {{QStringLiteral("enabled"), setup.getEnabled()},
            {QStringLiteral("method"), method},
            {QStringLiteral("timeStep"), setup.getTimeStep()},
            {QStringLiteral("unitSI"), true},
            {QStringLiteral("steps"), static_cast<double>(setup.getNSteps())},
            {QStringLiteral("outputFrequency"), static_cast<double>(setup.getOutputFrequency())},
            {QStringLiteral("rayleighAlpha"), setup.getRayleighAlpha()},
            {QStringLiteral("rayleighBeta"), setup.getRayleighBeta()},
            {QStringLiteral("rhoInfinity"), setup.getRhoInfinity()},
            {QStringLiteral("newmarkGamma"), setup.getNewmarkGamma()},
            {QStringLiteral("newmarkBeta"), setup.getNewmarkBeta()},
            {QStringLiteral("modalDampingRatios"), ratios}};
}

QString RModelCommandService::stateFingerprint(const RModel &model)
{
    QJsonArray groups;
    // findElementGroupByPhysicalId performs the model-wide uniqueness check.
    REntityGroupType ignoredType = R_ENTITY_GROUP_NONE;
    uint ignoredIndex = RConstants::eod;
    try
    {
        model.findElementGroupByPhysicalId(1, &ignoredType, &ignoredIndex);
    }
    catch (...)
    {
        return QString::fromLatin1(QCryptographicHash::hash(
            QByteArrayLiteral("invalid-model-target-identities"), QCryptographicHash::Sha256).toHex());
    }
    QVector<QPair<uint, QJsonObject>> orderedGroups;
    orderedGroups.reserve(static_cast<qsizetype>(model.getNElementGroups()));
    for (uint i = 0; i < model.getNElementGroups(); ++i)
    {
        const QJsonObject group = fullGroupState(model, i);
        orderedGroups.append({model.getElementGroupPtr(i)->getPhysicalId(), group});
    }
    std::sort(orderedGroups.begin(), orderedGroups.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    for (const auto &entry : orderedGroups) groups.append(entry.second);
    const QJsonObject state{{QStringLiteral("taskMask"), model.getProblemTaskTree().getProblemTypeMask()},
                            {QStringLiteral("modal"), modalParameters(model.getProblemSetup().getModalSetup())},
                            {QStringLiteral("structuralDynamics"), structuralDynamicsParameters(
                                 model.getProblemSetup().getStructuralDynamicsSetup())},
                            {QStringLiteral("timeSolver"), timeSolverParameters(model.getTimeSolver())},
                            {QStringLiteral("groups"), groups}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(state).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QJsonObject RModelCommandService::capabilities(const RModel &model)
{
    const RProblemTypeMask taskMask = model.getProblemTaskTree().getProblemTypeMask();
    const bool modal = taskMask & R_PROBLEM_STRESS_MODAL;
    const bool stressOnly = taskMask == R_PROBLEM_STRESS;
    const bool structural = stressOnly || taskMask == R_PROBLEM_STRESS_MODAL;
    const QJsonArray commands{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.modal.configure")},
                    {QStringLiteral("version"), 1}, {QStringLiteral("supported"), modal},
                    {QStringLiteral("units"), QJsonObject{{QStringLiteral("tolerance"), QStringLiteral("1")}}},
                    {QStringLiteral("methods"), QJsonArray{QStringLiteral("dominant"), QStringLiteral("multiple")}},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("method"), QStringLiteral("iterations"),
                        QStringLiteral("modes"), QStringLiteral("tolerance"), QStringLiteral("unit")}}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("analysis.structural_dynamics.configure")},
                    {QStringLiteral("version"), 1}, {QStringLiteral("supported"), stressOnly},
                    {QStringLiteral("maximumGeneratedTimeS"), std::numeric_limits<double>::max()},
                    {QStringLiteral("timeStepMaximumRule"), QStringLiteral("timeStep * steps must be finite; maximum timeStep is maximumGeneratedTimeS / steps")},
                    {QStringLiteral("units"), QJsonObject{{QStringLiteral("timeStep"), QStringLiteral("s")},
                        {QStringLiteral("rayleighAlpha"), QStringLiteral("1/s")},
                        {QStringLiteral("rayleighBeta"), QStringLiteral("s")},
                        {QStringLiteral("unitSI"), QStringLiteral("boolean true")},
                        {QStringLiteral("rhoInfinity"), QStringLiteral("1")},
                        {QStringLiteral("newmarkGamma"), QStringLiteral("1")},
                        {QStringLiteral("newmarkBeta"), QStringLiteral("1")},
                        {QStringLiteral("modalDampingRatios"), QStringLiteral("1")}}},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("enabled"), QStringLiteral("method"),
                        QStringLiteral("timeStep"), QStringLiteral("unitSI"), QStringLiteral("steps"),
                        QStringLiteral("outputFrequency"), QStringLiteral("rayleighAlpha"),
                        QStringLiteral("rayleighBeta"), QStringLiteral("rhoInfinity"),
                        QStringLiteral("newmarkGamma"), QStringLiteral("newmarkBeta"),
                        QStringLiteral("modalDampingRatios")}},
                    {QStringLiteral("methods"), QJsonArray{QStringLiteral("newmark"),
                        QStringLiteral("generalizedAlpha"), QStringLiteral("centralDifference")}},
                    {QStringLiteral("constraints"), QJsonArray{QStringLiteral("unitSI must be true"),
                        QStringLiteral("Newmark supports average acceleration only"),
                        QStringLiteral("modal damping cannot combine with Rayleigh damping")}}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("entity.material.set")},
                    {QStringLiteral("version"), 1}, {QStringLiteral("supported"), structural},
                    {QStringLiteral("targetId"), QStringLiteral("g:<physical uint>")},
                    {QStringLiteral("targetIdScope"), QStringLiteral("model-local stable physical group id across Point, Line, Surface, and Volume")},
                    {QStringLiteral("labelResolution"), QStringLiteral("labelQuery is read-only and returns candidate IDs; writes require targetId")},
                    {QStringLiteral("units"), QJsonObject{{QStringLiteral("youngModulusPa"), QStringLiteral("Pa")},
                        {QStringLiteral("densityKgM3"), QStringLiteral("kg/m^3")},
                        {QStringLiteral("poissonRatio"), QStringLiteral("1")},
                        {QStringLiteral("thermalExpansionPerK"), QStringLiteral("1/K")},
                        {QStringLiteral("referenceTemperatureK"), QStringLiteral("K")}}},
                    {QStringLiteral("referenceTemperatureDefaultK"), 293.15},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("targetId"), QStringLiteral("name"),
                        QStringLiteral("youngModulusPa"), QStringLiteral("densityKgM3"),
                        QStringLiteral("poissonRatio"), QStringLiteral("thermalExpansionPerK")}},
                    {QStringLiteral("optionalDefaults"), QJsonObject{{QStringLiteral("referenceTemperatureK"), 293.15}}},
                    {QStringLiteral("properties"), QJsonArray{QStringLiteral("ModulusOfElasiticity"),
                        QStringLiteral("Density"), QStringLiteral("PoissonRatio"),
                        QStringLiteral("ThermalExpansionCoefficient")}},
                    {QStringLiteral("constraints"), QJsonArray{QStringLiteral("tabulated versions of the four edited properties are rejected")}}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("entity.boundary.set")},
                    {QStringLiteral("version"), 1}, {QStringLiteral("supported"), structural},
                    {QStringLiteral("targetId"), QStringLiteral("g:<physical uint>")},
                    {QStringLiteral("targetIdScope"), QStringLiteral("model-local stable physical group id across Point, Line, Surface, and Volume")},
                    {QStringLiteral("labelResolution"), QStringLiteral("labelQuery is read-only and returns candidate IDs; writes require targetId")},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("targetId"), QStringLiteral("kind"),
                        QStringLiteral("values"), QStringLiteral("unit")}},
                    {QStringLiteral("optionalDefaults"), QJsonObject{{QStringLiteral("timeS"), 0.0}}},
                    {QStringLiteral("kinds"), QJsonArray{QStringLiteral("displacement"), QStringLiteral("force")}},
                    {QStringLiteral("units"), QJsonObject{{QStringLiteral("displacement"), QStringLiteral("m")},
                        {QStringLiteral("force"), QStringLiteral("N")}, {QStringLiteral("timeS"), QStringLiteral("s")}}},
                    {QStringLiteral("values"), QStringLiteral("three fixed global XYZ values; null disables an axis")},
                    {QStringLiteral("kindSupport"), QJsonObject{
                        {QStringLiteral("displacement"), QJsonObject{{QStringLiteral("tasks"), QJsonArray{QStringLiteral("Stress"), QStringLiteral("Stress Modal")}},
                            {QStringLiteral("targetGroups"), QJsonArray{QStringLiteral("point"), QStringLiteral("line"), QStringLiteral("surface"), QStringLiteral("volume")}}}},
                        {QStringLiteral("force"), QJsonObject{{QStringLiteral("tasks"), QJsonArray{QStringLiteral("Stress only")}},
                            {QStringLiteral("targetGroups"), QJsonArray{QStringLiteral("point")}},
                            {QStringLiteral("pointElementCount"), 1}, {QStringLiteral("nodesPerPointElement"), 1}}}}},
                    {QStringLiteral("forceSemantics"), QStringLiteral("total nodal vector on one point element with one node (N)")},
                    {QStringLiteral("constraints"), QJsonArray{QStringLiteral("force requires a Stress-only model and a single-node Point group"),
                        QStringLiteral("displacement requires Stress or Stress Modal and a Point, Line, Surface, or Volume group")}}}
    };
    return {{QStringLiteral("apiVersion"), apiVersion},
            {QStringLiteral("taskMask"), taskMask},
            {QStringLiteral("sourceIdentityBinding"), QStringLiteral("Target pages and cursors bind the wrapper sourceId and current revision; command previews bind their supplied sourceId and revision.")},
            {QStringLiteral("fingerprintScope"), QStringLiteral("Accepted analysis and time-grid settings, stable element-group IDs/types/labels, complete material and boundary state, and Point-group member identity/geometry. Selection and visibility state are excluded; wrappers must enforce their accepted source revision for other model changes.")},
            {QStringLiteral("targetPagination"), QJsonObject{{QStringLiteral("minimumLimit"), 1},
                {QStringLiteral("maximumLimit"), MaximumPageSize}, {QStringLiteral("defaultLimit"), DefaultPageSize},
                {QStringLiteral("cursorBoundBy"), QJsonArray{QStringLiteral("sourceId"), QStringLiteral("revision"),
                    QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("labelQuery")}}}},
            {QStringLiteral("supportedEntityGroups"), QJsonArray{QStringLiteral("point"), QStringLiteral("line"),
                QStringLiteral("surface"), QStringLiteral("volume")}},
            {QStringLiteral("commands"), commands}};
}

QJsonObject RModelCommandService::targets(const RModel &model, const QString &sourceId,
                                          const QString &revision, const QJsonObject &pageRequest)
{
    int offset = 0;
    int limit = DefaultPageSize;
    QString labelQuery;
    QString code, message;
    if (!parsePageRequest(sourceId, revision, pageRequest, offset, limit, labelQuery, code, message))
        return pageError(sourceId, revision, code, message);
    REntityGroupType ignoredType = R_ENTITY_GROUP_NONE;
    uint ignoredIndex = RConstants::eod;
    try
    {
        model.findElementGroupByPhysicalId(1, &ignoredType, &ignoredIndex);
    }
    catch (...)
    {
        return pageError(sourceId, revision, QStringLiteral("invalid_model"),
                         QStringLiteral("Element group physical identifiers are invalid or duplicated."));
    }

    QJsonArray page;
    int total = 0;
    for (uint i = 0; i < model.getNElementGroups(); ++i)
    {
        const RElementGroup *group = model.getElementGroupPtr(i);
        if (!labelQuery.isEmpty()
            && !group->getName().contains(labelQuery, Qt::CaseInsensitive)) continue;
        if (total >= offset && page.size() < limit) page.append(groupParameters(model, i, true));
        ++total;
    }
    const int pageCount = offset >= total ? 0 : std::min(limit, total - offset);
    const int end = offset >= total ? total : offset + pageCount;
    QJsonObject result{{QStringLiteral("apiVersion"), apiVersion}, {QStringLiteral("status"), QStringLiteral("ok")},
                       {QStringLiteral("sourceId"), sourceId}, {QStringLiteral("revision"), revision},
                       {QStringLiteral("offset"), offset}, {QStringLiteral("limit"), limit},
                       {QStringLiteral("total"), total}, {QStringLiteral("targets"), page},
                       {QStringLiteral("hasMore"), end < total},
                       {QStringLiteral("labelQuery"), labelQuery},
                       {QStringLiteral("ambiguous"), !labelQuery.isEmpty() && total > 1}};
    if (end < total)
        result.insert(QStringLiteral("nextCursor"), encodedCursor(sourceId, revision, end, limit, labelQuery)
                      .value(QStringLiteral("cursor")));
    if (!labelQuery.isEmpty())
    {
        QJsonArray candidates;
        for (int i = 0; i < page.size(); ++i)
            candidates.append(page.at(i).toObject().value(QStringLiteral("id")));
        result.insert(QStringLiteral("candidateIds"), candidates);
    }
    return result;
}

QJsonObject RModelCommandService::context(const RModel &model, const QString &sourceId,
                                          const QString &revision, const QJsonObject &pageRequest)
{
    const QJsonObject page = targets(model, sourceId, revision, pageRequest);
    if (page.value(QStringLiteral("status")).toString() != QStringLiteral("ok")) return page;
    const RProblemTypeMask taskMask = model.getProblemTaskTree().getProblemTypeMask();
    QJsonArray taskNames;
    for (RProblemType type : RProblem::getTypes(taskMask))
    {
        taskNames.append(RProblem::getName(type));
    }
    return {{QStringLiteral("apiVersion"), apiVersion}, {QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("sourceId"), sourceId}, {QStringLiteral("revision"), revision},
            {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("taskMask"), taskMask},
                {QStringLiteral("tasks"), taskNames}}},
            {QStringLiteral("configuration"), QJsonObject{
                {QStringLiteral("modal"), modalParameters(model.getProblemSetup().getModalSetup())},
                {QStringLiteral("structuralDynamics"), structuralDynamicsParameters(
                    model.getProblemSetup().getStructuralDynamicsSetup())},
                {QStringLiteral("timeSolverGrid"), timeGridState(model)}}},
            {QStringLiteral("capabilities"), capabilities(model)},
            {QStringLiteral("targetPage"), page}};
}

RModelCommandService::PreviewResult RModelCommandService::preview(
    const RModel &model, const QString &commandId, const QJsonObject &arguments,
    const QString &sourceId, const QString &revision)
{
    PreviewResult result;
    auto reject = [&result](const QString &code, const QString &message, const QString &path = QString()) {
        result.valid = false;
        result.code = code;
        result.message = message;
        result.diagnostics = QJsonArray{diagnostic(code, message, path)};
        return result;
    };

    try
    {
        if (commandId == QStringLiteral("analysis.modal.configure"))
        {
            static const QSet<QString> expected{QStringLiteral("method"), QStringLiteral("iterations"),
                QStringLiteral("modes"), QStringLiteral("tolerance"), QStringLiteral("unit")};
            if (!(model.getProblemTaskTree().getProblemTypeMask() & R_PROBLEM_STRESS_MODAL))
                return reject(QStringLiteral("unsupported_analysis"), QStringLiteral("The model has no structural modal task."));
            if (!exactKeys(arguments, expected)
                || (arguments.value(QStringLiteral("method")) != QJsonValue(QStringLiteral("dominant"))
                    && arguments.value(QStringLiteral("method")) != QJsonValue(QStringLiteral("multiple")))
                || arguments.value(QStringLiteral("unit")) != QJsonValue(QStringLiteral("1"))
                || !integer(arguments.value(QStringLiteral("iterations")), R_MODAL_ITERATIONS_MIN_NUMBER, R_MODAL_ITERATIONS_MAX_NUMBER)
                || !integer(arguments.value(QStringLiteral("modes")), R_MODAL_MODES_MIN_NUMBER, R_MODAL_MODES_MAX_NUMBER)
                || !finiteNumber(arguments.value(QStringLiteral("tolerance")))
                || arguments.value(QStringLiteral("tolerance")).toDouble() <= 0.0
                || arguments.value(QStringLiteral("tolerance")).toDouble() > R_MODAL_CONVERGENCE_MAX_VALUE)
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("Invalid modal settings or dimensionless unit."));
            result.delta.kind = Delta::Kind::Modal;
            result.delta.beforeModal = model.getProblemSetup().getModalSetup();
            result.delta.afterModal = result.delta.beforeModal;
            result.delta.afterModal.setMethod(arguments.value(QStringLiteral("method")) == QJsonValue(QStringLiteral("multiple"))
                ? R_MODAL_MULTIPLE_MODES : R_MODAL_DOMINANT_MODE);
            result.delta.afterModal.setNIterations(static_cast<uint>(arguments.value(QStringLiteral("iterations")).toDouble()));
            result.delta.afterModal.setNModesToExtract(static_cast<uint>(arguments.value(QStringLiteral("modes")).toDouble()));
            result.delta.afterModal.setConvergenceValue(arguments.value(QStringLiteral("tolerance")).toDouble());
            result.before = modalParameters(result.delta.beforeModal);
            result.after = modalParameters(result.delta.afterModal);
            result.normalizedArguments = result.after;
            result.delta.normalizedArguments = result.normalizedArguments;
            result.delta.beforeState = result.before;
            result.delta.afterState = result.after;
            result.delta.changedPath = QStringLiteral("/problemSetup/modal");
        }
        else if (commandId == QStringLiteral("analysis.structural_dynamics.configure"))
        {
            if (model.getProblemTaskTree().getProblemTypeMask() != R_PROBLEM_STRESS)
                return reject(QStringLiteral("unsupported_analysis"), QStringLiteral("Second-order dynamics requires a Stress-only task tree."));
            result.delta.kind = Delta::Kind::StructuralDynamics;
            result.delta.beforeDynamics = model.getProblemSetup().getStructuralDynamicsSetup();
            result.delta.afterDynamics = result.delta.beforeDynamics;
            QString code, message;
            if (!readStructuralDynamics(arguments, result.delta.afterDynamics, code, message))
                return reject(code, message);
            result.delta.beforeTimeSolver = model.getTimeSolver();
            result.delta.afterTimeSolver = result.delta.beforeTimeSolver;
            result.delta.afterTimeSolver.setEnabled(result.delta.afterDynamics.getEnabled());
            result.delta.afterTimeSolver.setInputStartTime(0.0);
            result.delta.afterTimeSolver.setInputNTimeSteps(result.delta.afterDynamics.getNSteps());
            result.delta.afterTimeSolver.setInputTimeStepSize(result.delta.afterDynamics.getTimeStep());
            result.delta.afterTimeSolver.setOutputFrequency(result.delta.afterDynamics.getOutputFrequency());
            result.delta.afterTimeSolver.setComputedTime(0.0);
            result.delta.afterTimeSolver.setTimes(RTimeSolver::findTimesVector(
                result.delta.afterDynamics.getNSteps(), 0.0, result.delta.afterDynamics.getTimeStep()));
            result.delta.afterTimeSolver.setCurrentTimeStep(0);
            result.before = {{QStringLiteral("structuralDynamics"), structuralDynamicsParameters(result.delta.beforeDynamics)},
                             {QStringLiteral("timeSolver"), timeSolverParameters(result.delta.beforeTimeSolver)}};
            result.after = {{QStringLiteral("structuralDynamics"), structuralDynamicsParameters(result.delta.afterDynamics)},
                            {QStringLiteral("timeSolver"), timeSolverParameters(result.delta.afterTimeSolver)}};
            result.normalizedArguments = structuralDynamicsParameters(result.delta.afterDynamics);
            result.delta.normalizedArguments = result.normalizedArguments;
            result.delta.beforeState = result.before;
            result.delta.afterState = result.after;
            result.delta.changedPath = QStringLiteral("/problemSetup/structuralDynamics");
            result.delta.changedPaths = QJsonArray{result.delta.changedPath, QStringLiteral("/timeSolver")};
        }
        else if (commandId == QStringLiteral("entity.material.set"))
        {
            static const QSet<QString> required{QStringLiteral("targetId"), QStringLiteral("name"),
                QStringLiteral("youngModulusPa"), QStringLiteral("densityKgM3"),
                QStringLiteral("poissonRatio"), QStringLiteral("thermalExpansionPerK")};
            static const QSet<QString> allowed{QStringLiteral("targetId"), QStringLiteral("name"),
                QStringLiteral("youngModulusPa"), QStringLiteral("densityKgM3"),
                QStringLiteral("poissonRatio"), QStringLiteral("thermalExpansionPerK"),
                QStringLiteral("referenceTemperatureK")};
            const RProblemTypeMask taskMask = model.getProblemTaskTree().getProblemTypeMask();
            if (taskMask != R_PROBLEM_STRESS && taskMask != R_PROBLEM_STRESS_MODAL)
                return reject(QStringLiteral("unsupported_analysis"), QStringLiteral("Solid material edits require Stress or Stress Modal."));
            if (!keysWithin(arguments, allowed) || !std::all_of(required.cbegin(), required.cend(),
                    [&arguments](const QString &key) { return arguments.contains(key); }))
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("Material settings require the exact supported fields."));
            uint physicalId = 0;
            if (!parseTargetId(arguments.value(QStringLiteral("targetId")), physicalId))
                return reject(QStringLiteral("invalid_target"), QStringLiteral("targetId must be g:<nonzero physical group id>."), QStringLiteral("/targetId"));
            REntityGroupType type = R_ENTITY_GROUP_NONE;
            uint localIndex = RConstants::eod;
            const RElementGroup *group = findGroup(model, physicalId, &type, &localIndex);
            if (!group) return reject(QStringLiteral("stale_target"), QStringLiteral("The target group no longer exists."), QStringLiteral("/targetId"));
            Q_UNUSED(type)
            Q_UNUSED(localIndex)
            const QString name = arguments.value(QStringLiteral("name")).toString().trimmed();
            const double referenceTemperature = arguments.contains(QStringLiteral("referenceTemperatureK"))
                ? arguments.value(QStringLiteral("referenceTemperatureK")).toDouble() : 293.15;
            if (!arguments.value(QStringLiteral("name")).isString() || name.isEmpty() || name.size() > 256
                || !finiteNumber(arguments.value(QStringLiteral("youngModulusPa")))
                || !finiteNumber(arguments.value(QStringLiteral("densityKgM3")))
                || !finiteNumber(arguments.value(QStringLiteral("poissonRatio")))
                || !finiteNumber(arguments.value(QStringLiteral("thermalExpansionPerK")))
                || (arguments.contains(QStringLiteral("referenceTemperatureK"))
                    && !finiteNumber(arguments.value(QStringLiteral("referenceTemperatureK"))))
                || arguments.value(QStringLiteral("youngModulusPa")).toDouble() <= 0.0
                || arguments.value(QStringLiteral("densityKgM3")).toDouble() <= 0.0
                || arguments.value(QStringLiteral("poissonRatio")).toDouble() <= -1.0
                || arguments.value(QStringLiteral("poissonRatio")).toDouble() >= 0.5
                || referenceTemperature < 0.0)
                return reject(QStringLiteral("invalid_material"), QStringLiteral("Material values must be finite SI values with E>0, density>0, -1<nu<0.5, and reference temperature >=0."));

            const RMaterial &oldMaterial = group->getMaterial();
            const RMaterialProperty::Type fields[]{RMaterialProperty::ModulusOfElasiticity,
                RMaterialProperty::Density, RMaterialProperty::PoissonRatio,
                RMaterialProperty::ThermalExpansionCoefficient};
            for (RMaterialProperty::Type field : fields)
            {
                const uint position = oldMaterial.findPosition(field);
                if (position < oldMaterial.size() && oldMaterial.get(position).size() > 1)
                    return reject(QStringLiteral("tabulated_material"),
                        QStringLiteral("A tabulated edited material property cannot be replaced without explicit table conversion."),
                        QStringLiteral("/material/%1").arg(RMaterialProperty::getId(field)));
            }
            result.delta.kind = Delta::Kind::Material;
            result.delta.physicalId = physicalId;
            result.delta.beforeMaterial = oldMaterial;
            result.delta.afterMaterial = oldMaterial;
            result.delta.afterMaterial.setState(RMaterial::Solid);
            result.delta.afterMaterial.setName(name);
            const double values[]{arguments.value(QStringLiteral("youngModulusPa")).toDouble(),
                arguments.value(QStringLiteral("densityKgM3")).toDouble(),
                arguments.value(QStringLiteral("poissonRatio")).toDouble(),
                arguments.value(QStringLiteral("thermalExpansionPerK")).toDouble()};
            for (int i = 0; i < 4; ++i)
            {
                RMaterialProperty property(fields[i]);
                property.add(referenceTemperature, values[i]);
                result.delta.afterMaterial.add(property);
            }
            result.before = materialParameters(result.delta.beforeMaterial);
            result.after = materialParameters(result.delta.afterMaterial);
            result.normalizedArguments = arguments;
            result.normalizedArguments.insert(QStringLiteral("name"), name);
            result.normalizedArguments.insert(QStringLiteral("referenceTemperatureK"), referenceTemperature);
            result.delta.normalizedArguments = result.normalizedArguments;
            result.delta.beforeState = result.before;
            result.delta.afterState = result.after;
            result.delta.changedPath = QStringLiteral("/entityGroups/g:%1/material").arg(physicalId);
        }
        else if (commandId == QStringLiteral("entity.boundary.set"))
        {
            static const QSet<QString> required{QStringLiteral("targetId"), QStringLiteral("kind"),
                QStringLiteral("values"), QStringLiteral("unit")};
            static const QSet<QString> allowed{QStringLiteral("targetId"), QStringLiteral("kind"),
                QStringLiteral("values"), QStringLiteral("unit"), QStringLiteral("timeS")};
            if (!keysWithin(arguments, allowed) || !std::all_of(required.cbegin(), required.cend(),
                    [&arguments](const QString &key) { return arguments.contains(key); }))
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("Boundary settings require targetId, kind, values, and unit."));
            const QString kind = arguments.value(QStringLiteral("kind")).toString();
            const bool displacement = kind == QStringLiteral("displacement");
            const bool force = kind == QStringLiteral("force");
            if (!displacement && !force)
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("Boundary kind must be displacement or force."), QStringLiteral("/kind"));
            const RProblemTypeMask taskMask = model.getProblemTaskTree().getProblemTypeMask();
            if ((force && taskMask != R_PROBLEM_STRESS)
                || (displacement && taskMask != R_PROBLEM_STRESS && taskMask != R_PROBLEM_STRESS_MODAL))
                return reject(QStringLiteral("unsupported_analysis"), force
                    ? QStringLiteral("Force edits require a Stress-only task tree.")
                    : QStringLiteral("Displacement edits require Stress or Stress Modal."));
            const QString requiredUnit = displacement ? QStringLiteral("m") : QStringLiteral("N");
            if (arguments.value(QStringLiteral("unit")) != QJsonValue(requiredUnit))
                return reject(QStringLiteral("invalid_units"), QStringLiteral("Boundary values must use the declared SI unit (%1).")
                              .arg(requiredUnit), QStringLiteral("/unit"));
            const QJsonArray values = arguments.value(QStringLiteral("values")).toArray();
            if (!arguments.value(QStringLiteral("values")).isArray() || values.size() != 3)
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("values must contain exactly three global XYZ entries."), QStringLiteral("/values"));
            int enabledCount = 0;
            for (const QJsonValue &value : values)
            {
                if (value.isNull()) continue;
                if (!finiteNumber(value))
                    return reject(QStringLiteral("invalid_parameters"), QStringLiteral("Each enabled XYZ value must be a finite number; null disables an axis."), QStringLiteral("/values"));
                ++enabledCount;
            }
            if (enabledCount == 0)
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("At least one XYZ component must be enabled."), QStringLiteral("/values"));
            const double timeS = arguments.contains(QStringLiteral("timeS"))
                ? arguments.value(QStringLiteral("timeS")).toDouble() : 0.0;
            if ((arguments.contains(QStringLiteral("timeS")) && !finiteNumber(arguments.value(QStringLiteral("timeS"))))
                || timeS < 0.0)
                return reject(QStringLiteral("invalid_parameters"), QStringLiteral("timeS must be a finite nonnegative number."), QStringLiteral("/timeS"));
            uint physicalId = 0;
            if (!parseTargetId(arguments.value(QStringLiteral("targetId")), physicalId))
                return reject(QStringLiteral("invalid_target"), QStringLiteral("targetId must be g:<nonzero physical group id>."), QStringLiteral("/targetId"));
            REntityGroupType groupType = R_ENTITY_GROUP_NONE;
            uint localIndex = RConstants::eod;
            const RElementGroup *group = findGroup(model, physicalId, &groupType, &localIndex);
            if (!group) return reject(QStringLiteral("stale_target"), QStringLiteral("The target group no longer exists."), QStringLiteral("/targetId"));
            if (force)
            {
                if (groupType != R_ENTITY_GROUP_POINT || group->size() != 1)
                    return reject(QStringLiteral("unsupported_target"), QStringLiteral("Force requires one Point group containing exactly one point element."), QStringLiteral("/targetId"));
                const uint elementId = group->get(0);
                if (elementId >= model.getNElements() || model.getElement(elementId).getType() != R_ELEMENT_POINT
                    || model.getElement(elementId).size() != 1
                    || model.getElement(elementId).getNodeId(0) >= model.getNNodes())
                    return reject(QStringLiteral("unsupported_target"), QStringLiteral("Force requires a valid single-node point element; distributed force is not supported."), QStringLiteral("/targetId"));
            }
            result.delta.kind = Delta::Kind::Boundary;
            result.delta.physicalId = physicalId;
            result.delta.boundaryType = displacement ? R_BOUNDARY_CONDITION_DISPLACEMENT : R_BOUNDARY_CONDITION_FORCE;
            result.delta.hadBeforeBoundary = group->hasBoundaryCondition(result.delta.boundaryType);
            if (result.delta.hadBeforeBoundary)
                result.delta.beforeBoundary = group->getBoundaryCondition(result.delta.boundaryType);
            RBoundaryCondition condition(result.delta.boundaryType);
            const RVariableType variables[3] = {displacement ? R_VARIABLE_DISPLACEMENT_X : R_VARIABLE_FORCE_X,
                displacement ? R_VARIABLE_DISPLACEMENT_Y : R_VARIABLE_FORCE_Y,
                displacement ? R_VARIABLE_DISPLACEMENT_Z : R_VARIABLE_FORCE_Z};
            for (int i = 0; i < 3; ++i)
            {
                const uint position = condition.findComponentPosition(variables[i]);
                if (position >= condition.size())
                    return reject(QStringLiteral("invalid_model"), QStringLiteral("Native boundary condition is missing a required XYZ component."));
                RConditionComponent &component = condition.getComponent(position);
                if (values.at(i).isNull())
                {
                    component.setEnabled(false);
                }
                else
                {
                    component.setEnabled(true);
                    appendSamples(component, timeS, values.at(i).toDouble());
                }
            }
            result.delta.afterBoundary = condition;
            result.before = result.delta.hadBeforeBoundary
                ? boundaryConditionParameters(result.delta.beforeBoundary) : QJsonObject{};
            result.after = boundaryConditionParameters(result.delta.afterBoundary);
            result.normalizedArguments = arguments;
            result.normalizedArguments.insert(QStringLiteral("timeS"), timeS);
            result.delta.normalizedArguments = result.normalizedArguments;
            result.delta.beforeState = result.before;
            result.delta.afterState = result.after;
            result.delta.changedPath = QStringLiteral("/entityGroups/g:%1/boundaryConditions/%2")
                .arg(physicalId).arg(displacement ? QStringLiteral("displacement") : QStringLiteral("force"));
        }
        else
        {
            return reject(QStringLiteral("unknown_command"), QStringLiteral("This command is not supported by the shared model service."));
        }

        if (result.delta.kind == Delta::Kind::Modal
            && sameJson(result.before, result.after)) result.delta.kind = Delta::Kind::None;
        if (result.delta.kind != Delta::Kind::None)
            result.changedPaths = result.delta.changedPaths.isEmpty()
                ? QJsonArray{result.delta.changedPath} : result.delta.changedPaths;
        result.delta.changedPaths = result.changedPaths;
        result.delta.changedPath = result.changedPaths.isEmpty() ? QString() : result.delta.changedPath;

        result.candidate = model;
        if (commandId == QStringLiteral("analysis.modal.configure"))
            result.candidate.getProblemSetup().setModalSetup(result.delta.afterModal);
        else if (commandId == QStringLiteral("analysis.structural_dynamics.configure"))
        {
            result.candidate.getProblemSetup().setStructuralDynamicsSetup(result.delta.afterDynamics);
            result.candidate.setTimeSolver(result.delta.afterTimeSolver);
        }
        else if (commandId == QStringLiteral("entity.material.set"))
        {
            RElementGroup *group = findGroup(result.candidate, result.delta.physicalId, nullptr, nullptr);
            if (!group) return reject(QStringLiteral("stale_target"), QStringLiteral("The target group no longer exists."));
            group->setMaterial(result.delta.afterMaterial);
        }
        else if (commandId == QStringLiteral("entity.boundary.set"))
        {
            RElementGroup *group = findGroup(result.candidate, result.delta.physicalId, nullptr, nullptr);
            if (!group) return reject(QStringLiteral("stale_target"), QStringLiteral("The target group no longer exists."));
            group->addBoundaryCondition(result.delta.afterBoundary);
        }
        result.valid = true;
        result.code.clear();
        result.message.clear();
        result.diagnostics = QJsonArray{};
        result.fingerprint = resultFingerprint(model, result.candidate, sourceId, revision, commandId,
                                               result.normalizedArguments, result.before, result.after);
        return result;
    }
    catch (const std::exception &error)
    {
        return reject(QStringLiteral("invalid_model"), QString::fromUtf8(error.what()));
    }
    catch (...)
    {
        return reject(QStringLiteral("invalid_model"), QStringLiteral("Model validation failed."));
    }
}

RModelCommandService::PreviewResult RModelCommandService::applyDelta(
    const RModel &model, const Delta &delta, bool forward)
{
    PreviewResult result;
    auto reject = [&result](const QString &code, const QString &message) {
        result.valid = false;
        result.code = code;
        result.message = message;
        result.diagnostics = QJsonArray{diagnostic(code, message)};
        return result;
    };
    if (delta.kind == Delta::Kind::None)
        return reject(QStringLiteral("empty_history"), QStringLiteral("No supported model change is stored in this history entry."));
    try
    {
        const QJsonObject expected = forward ? delta.beforeState : delta.afterState;
        const QJsonObject next = forward ? delta.afterState : delta.beforeState;
        result.before = currentResourceState(model, delta.kind, delta.physicalId, delta.boundaryType);
        bool stateMatches = false;
        if (delta.kind == Delta::Kind::Material || delta.kind == Delta::Kind::Boundary)
        {
            const RElementGroup *group = findGroup(model, delta.physicalId, nullptr, nullptr);
            if (!group) return reject(QStringLiteral("stale_target"), QStringLiteral("The target group no longer exists."));
            if (delta.kind == Delta::Kind::Material)
            {
                const RMaterial &expectedMaterial = forward ? delta.beforeMaterial : delta.afterMaterial;
                stateMatches = sameJson(group->getMaterial().toJson(), expectedMaterial.toJson());
            }
            else
            {
                const bool expectedExists = forward ? delta.hadBeforeBoundary : true;
                if (group->hasBoundaryCondition(delta.boundaryType) == expectedExists)
                {
                    if (!expectedExists) stateMatches = true;
                    else
                    {
                        const RBoundaryCondition &expectedCondition = forward
                            ? (delta.hadBeforeBoundary ? delta.beforeBoundary : delta.afterBoundary)
                            : delta.afterBoundary;
                        stateMatches = sameJson(boundaryConditionParameters(
                            group->getBoundaryCondition(delta.boundaryType), true),
                            boundaryConditionParameters(expectedCondition, true));
                    }
                }
            }
        }
        else stateMatches = sameJson(result.before, expected);
        if (!stateMatches)
        {
            return reject(QStringLiteral("stale_history"), QStringLiteral("Model state no longer matches the stored edit."));
        }
        result.after = next;
        result.normalizedArguments = delta.normalizedArguments;
        result.changedPaths = delta.changedPaths.isEmpty()
            ? QJsonArray{delta.changedPath} : delta.changedPaths;
        result.delta = delta;
        result.candidate = model;
        if (delta.kind == Delta::Kind::Modal)
            result.candidate.getProblemSetup().setModalSetup(forward ? delta.afterModal : delta.beforeModal);
        else if (delta.kind == Delta::Kind::StructuralDynamics)
        {
            result.candidate.getProblemSetup().setStructuralDynamicsSetup(forward ? delta.afterDynamics : delta.beforeDynamics);
            result.candidate.setTimeSolver(forward ? delta.afterTimeSolver : delta.beforeTimeSolver);
        }
        else
        {
            RElementGroup *group = findGroup(result.candidate, delta.physicalId, nullptr, nullptr);
            if (!group) return reject(QStringLiteral("stale_target"), QStringLiteral("The target group no longer exists."));
            if (delta.kind == Delta::Kind::Material)
                group->setMaterial(forward ? delta.afterMaterial : delta.beforeMaterial);
            else if (delta.kind == Delta::Kind::Boundary)
            {
                const bool shouldExist = forward || delta.hadBeforeBoundary;
                if (shouldExist) group->addBoundaryCondition(forward ? delta.afterBoundary : delta.beforeBoundary);
                else if (group->hasBoundaryCondition(delta.boundaryType)) group->removeBoundaryCondition(delta.boundaryType);
            }
        }
        result.valid = true;
        result.fingerprint = resultFingerprint(model, result.candidate, QString(), QString(),
            forward ? QStringLiteral("model.redo") : QStringLiteral("model.undo"),
            result.normalizedArguments, result.before, result.after);
        return result;
    }
    catch (const std::exception &error)
    {
        return reject(QStringLiteral("invalid_model"), QString::fromUtf8(error.what()));
    }
    catch (...)
    {
        return reject(QStringLiteral("invalid_model"), QStringLiteral("Model history could not be applied."));
    }
}
