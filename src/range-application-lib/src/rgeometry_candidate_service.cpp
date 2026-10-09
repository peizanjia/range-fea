#include <rgeometry_candidate_service.h>

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace
{
using XYZ = std::array<double,3>;
constexpr double coordinateLimit = 1.0e6;
constexpr double minimumLength = 1.0e-12;
constexpr double minimumDirectionSine = 1.0e-10;

XYZ xyz(const RNode &n) { return {n.getX(), n.getY(), n.getZ()}; }
QJsonArray json(const XYZ &v) { return {v[0], v[1], v[2]}; }
bool exactCoordinates(const XYZ &a, const XYZ &b)
{
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i] || (a[i] == 0.0 && std::signbit(a[i]) != std::signbit(b[i]))) return false;
    return true;
}
bool finiteBounded(const XYZ &v)
{
    return std::all_of(v.begin(), v.end(), [](double x) {
        return std::isfinite(x) && std::abs(x) <= coordinateLimit;
    });
}
bool fields(const QJsonObject &o, const QStringList &keys)
{
    if (o.size() != keys.size()) return false;
    for (const QString &key : keys) if (!o.contains(key)) return false;
    return true;
}
bool number(const QJsonValue &v, double &out)
{
    if (!v.isDouble()) return false;
    out = v.toDouble();
    return std::isfinite(out);
}
bool vector(const QJsonValue &v, XYZ &out, bool bounded = true)
{
    if (!v.isArray() || v.toArray().size() != 3) return false;
    const QJsonArray a = v.toArray();
    for (int i = 0; i < 3; ++i) if (!number(a[i], out[i])) return false;
    return !bounded || finiteBounded(out);
}
bool physicalId(const QJsonValue &v, uint &id)
{
    double d = 0.0;
    if (!number(v, d) || d <= 0.0 || d >= double(std::numeric_limits<uint>::max())
        || std::floor(d) != d) return false;
    id = uint(d);
    return true;
}
double norm(const XYZ &v) { return std::hypot(v[0], v[1], v[2]); }
XYZ difference(const XYZ &a, const XYZ &b)
{
    return {a[0]-b[0], a[1]-b[1], a[2]-b[2]};
}
bool normalize(XYZ &v)
{
    // Scale first so finite axis components close to DBL_MAX cannot overflow the norm.
    const double scale = std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
    if (!std::isfinite(scale) || scale == 0.0) return false;
    for (double &x : v) x /= scale;
    const double length = norm(v);
    for (double &x : v) x /= length;
    return true;
}
bool validIdentities(const RModel &model)
{
    // RModel's allocator validator is private. Validate the public stable identity
    // surface here; this service never allocates/replaces entities or their IDs.
    const auto check = [](uint id,QSet<uint> &seen) {
        if (id == 0 || id == std::numeric_limits<uint>::max() || seen.contains(id)) return false;
        seen.insert(id);
        return true;
    };
    QSet<uint> nodes, elements, groups;
    for (uint i=0;i<model.getNNodes();++i) if (!check(model.getNodePhysicalId(i),nodes)) return false;
    for (uint i=0;i<model.getNElements();++i) if (!check(model.getElementPhysicalId(i),elements)) return false;
    for (uint i=0;i<model.getNElementGroups();++i) if (!check(model.getElementGroupPtr(i)->getPhysicalId(),groups)) return false;
    return true;
}
QString validate(const RModel &model)
{
    if (!validIdentities(model)) return QStringLiteral("Invalid stable physical identities.");
    if (model.getNNodes() == 0) return QStringLiteral("Geometry must contain at least one node.");
    for (uint i = 0; i < model.getNNodes(); ++i)
        if (!finiteBounded(xyz(model.getNode(i)))) return QStringLiteral("Node coordinates must be finite and within +/-1000000 m.");
    for (uint i = 0; i < model.getNElements(); ++i)
    {
        const RElement &e = model.getElement(i);
        if (e.getType() == R_ELEMENT_POINT)
        {
            if (e.size() != 1 || e.getNodeId(0) >= model.getNNodes())
                return QStringLiteral("Invalid point connectivity.");
            continue;
        }
        if (e.getType() != R_ELEMENT_TRUSS1 && e.getType() != R_ELEMENT_BEAM1)
            return QStringLiteral("Only POINT, TRUSS1 and BEAM1 elements have a validated geometry quality guard.");
        if (e.size() != 2 || e.getNodeId(0) >= model.getNNodes()
            || e.getNodeId(1) >= model.getNNodes()) return QStringLiteral("Invalid line connectivity.");
        XYZ axis = difference(xyz(model.getNode(e.getNodeId(1))), xyz(model.getNode(e.getNodeId(0))));
        if (norm(axis) <= minimumLength) return QStringLiteral("Line length must exceed 1e-12 m.");
        if (e.getType() == R_ELEMENT_BEAM1)
        {
            normalize(axis);
            bool found = false;
            for (uint l = 0; l < model.getNLines(); ++l)
            {
                const RLine &line = model.getLine(l);
                uint position = 0;
                if (!line.findPosition(i, &position)) continue;
                found = true;
                const RR3Vector &raw = line.getLocalYDirection();
                XYZ y{raw[0],raw[1],raw[2]};
                if (!std::all_of(y.begin(),y.end(),[](double x){return std::isfinite(x);}) || !normalize(y))
                    return QStringLiteral("Beam local Y must be a finite nonzero global direction.");
                const XYZ cross{axis[1]*y[2]-axis[2]*y[1],axis[2]*y[0]-axis[0]*y[2],axis[0]*y[1]-axis[1]*y[0]};
                if (norm(cross) <= minimumDirectionSine)
                    return QStringLiteral("Beam local Y is parallel to its axis under preserve_global.");
            }
            if (!found) return QStringLiteral("Every BEAM1 must belong to a line carrying its local Y direction.");
        }
    }
    for (uint i = 0; i < model.getNElementGroups(); ++i)
    {
        const RElementGroup *g = model.getElementGroupPtr(i);
        for (uint j = 0; j < g->getNBoundaryConditions(); ++j)
        {
            const RBoundaryCondition &bc = g->getBoundaryCondition(j);
            if (bc.getHasLocalDirection() && !bc.getExplicitLocalDirection())
                return QStringLiteral("Geometry-derived boundary directions cannot guarantee preserve_global; bind an explicit global direction first.");
        }
    }
    return {};
}
}

QString RGeometryCandidateService::geometryFingerprint(const RModel &model)
{
    if (!validIdentities(model)) return {};
    std::vector<uint> nodes(model.getNNodes()), elements(model.getNElements());
    std::iota(nodes.begin(), nodes.end(), 0u);
    std::iota(elements.begin(), elements.end(), 0u);
    std::sort(nodes.begin(),nodes.end(),[&](uint a,uint b){return model.getNodePhysicalId(a)<model.getNodePhysicalId(b);});
    std::sort(elements.begin(),elements.end(),[&](uint a,uint b){return model.getElementPhysicalId(a)<model.getElementPhysicalId(b);});
    QByteArray bytes;
    QDataStream stream(&bytes,QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << quint32(1) << quint32(nodes.size());
    for (uint i : nodes)
    {
        stream << quint32(model.getNodePhysicalId(i));
        for (double x : xyz(model.getNode(i))) stream << x;
    }
    stream << quint32(elements.size());
    for (uint i : elements)
    {
        const RElement &e = model.getElement(i);
        stream << quint32(model.getElementPhysicalId(i)) << quint32(e.getType()) << quint32(e.size());
        for (uint j = 0; j < e.size(); ++j)
        {
            if (e.getNodeId(j) >= model.getNNodes()) return {};
            stream << quint32(model.getNodePhysicalId(e.getNodeId(j)));
        }
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}

RGeometryCandidateService::GeometryCandidate RGeometryCandidateService::preview(
    const RModel &model, const QString &commandId, const QJsonObject &arguments)
{
    GeometryCandidate result;
    const auto fail = [&](const QString &code,const QString &message) {
        result.delta = GeometryDelta{};
        result.code = code; result.message = message; return result;
    };
    const bool set = commandId == QStringLiteral("geometry.node_coordinates.set");
    const bool transform = commandId == QStringLiteral("geometry.rigid_transform");
    if (!set && !transform) return fail("unsupported_command","Unknown geometry command.");
    const QStringList keys = set
        ? QStringList{"unit","directionPolicy","nodes"}
        : QStringList{"unit","angleUnit","directionPolicy","center","axis","angle","translation"};
    if (!fields(arguments,keys)) return fail("invalid_arguments","Missing or unknown geometry fields.");
    if (arguments.value("unit") != QJsonValue(QStringLiteral("m"))
        || arguments.value("directionPolicy") != QJsonValue(QStringLiteral("preserve_global"))
        || (transform && arguments.value("angleUnit") != QJsonValue(QStringLiteral("deg"))))
        return fail("invalid_arguments","Required units are m/deg and directionPolicy is preserve_global.");
    const QString beforeError = validate(model);
    if (!beforeError.isEmpty()) return fail("invalid_source_geometry",beforeError);
    std::vector<std::pair<uint,XYZ>> edits;
    QJsonObject normalized = arguments;
    if (set)
    {
        if (!arguments.value("nodes").isArray() || arguments.value("nodes").toArray().isEmpty())
            return fail("invalid_arguments","nodes must be a nonempty array.");
        QSet<uint> seen;
        for (const QJsonValue &value : arguments.value("nodes").toArray())
        {
            if (!value.isObject() || !fields(value.toObject(),{"physicalNodeId","coordinates"}))
                return fail("invalid_arguments","Each node needs exactly physicalNodeId and coordinates.");
            const QJsonObject entry = value.toObject();
            uint id = 0;
            XYZ coordinates{};
            if (!physicalId(entry.value("physicalNodeId"),id) || !vector(entry.value("coordinates"),coordinates))
                return fail("invalid_arguments","Invalid physical node ID or coordinate vector.");
            if (seen.contains(id)) return fail("invalid_arguments","Duplicate physical node ID.");
            seen.insert(id);
            const uint pos = model.findNodePositionByPhysicalId(id);
            if (pos >= model.getNNodes()) return fail("unknown_target","Unknown physical node ID.");
            edits.emplace_back(pos,coordinates);
        }
    }
    else
    {
        XYZ center{}, axis{}, translation{};
        double angle = 0.0;
        if (!vector(arguments.value("center"),center) || !vector(arguments.value("translation"),translation)
            || !vector(arguments.value("axis"),axis,false) || !normalize(axis)
            || !number(arguments.value("angle"),angle) || std::abs(angle) > 360.0)
            return fail("invalid_arguments","Invalid center, translation, axis, or angle in [-360,360] deg.");
        // Preserve the submitted axis in the replay contract. Normalization
        // is internal math; repeating it on a rounded unit vector can change
        // the exact candidate fingerprint despite an unchanged source model.
        const double radians = angle * std::acos(-1.0) / 180.0;
        // Full turns are exact identity, avoiding an accidental floating-point non-noop.
        const double c = std::abs(angle) == 360.0 ? 1.0 : std::cos(radians);
        const double s = std::abs(angle) == 360.0 ? 0.0 : std::sin(radians);
        for (uint i = 0; i < model.getNNodes(); ++i)
        {
            const XYZ x = xyz(model.getNode(i));
            const XYZ v = difference(x,center);
            const double dot = axis[0]*v[0]+axis[1]*v[1]+axis[2]*v[2];
            const XYZ cross{axis[1]*v[2]-axis[2]*v[1],axis[2]*v[0]-axis[0]*v[2],axis[0]*v[1]-axis[1]*v[0]};
            XYZ out{};
            for (int j=0;j<3;++j)
                out[j] = c == 1.0 && s == 0.0 ? x[j]+translation[j]
                    : center[j]+c*v[j]+s*cross[j]+(1.0-c)*axis[j]*dot+translation[j];
            if (!finiteBounded(out)) return fail("invalid_candidate_geometry","Transformed coordinate exceeds +/-1000000 m.");
            edits.emplace_back(i,out);
        }
    }
    RModel candidate(model);
    for (const auto &edit : edits)
    {
        const XYZ before = xyz(model.getNode(edit.first));
        candidate.getNode(edit.first).set(edit.second[0],edit.second[1],edit.second[2]);
        if (!exactCoordinates(before,edit.second))
            result.delta.nodes.push_back({model.getNodePhysicalId(edit.first),before,edit.second});
    }
    const QString afterError = validate(candidate);
    if (!afterError.isEmpty()) return fail("invalid_candidate_geometry",afterError);
    std::sort(result.delta.nodes.begin(),result.delta.nodes.end(),[](const NodeDelta &a,const NodeDelta &b){return a.physicalNodeId<b.physicalNodeId;});
    for (const NodeDelta &d : result.delta.nodes) result.delta.affectedPhysicalNodeIds.append(double(d.physicalNodeId));
    result.delta.preGeometrySha256 = geometryFingerprint(model);
    result.delta.postGeometrySha256 = geometryFingerprint(candidate);
    result.delta.noOp = result.delta.nodes.empty();
    result.candidate = candidate;
    result.normalizedArguments = normalized;
    result.valid = true;
    result.code = result.delta.noOp ? QStringLiteral("no_op") : QStringLiteral("candidate_ready");
    return result;
}

QJsonObject RGeometryCandidateService::capabilities()
{
    return {{"commands",QJsonArray{"geometry.node_coordinates.set","geometry.rigid_transform"}},
            {"supportedElementTypes",QJsonArray{"POINT","TRUSS1","BEAM1"}},
            {"selection","whole_model_rigid_transform_or_explicit_physical_node_ids"},
            {"unit","m"},{"angleUnit","deg"},{"directionPolicy","preserve_global"},
            {"coordinateAbsoluteMaximum",coordinateLimit},{"angleMinimum",-360.0},{"angleMaximum",360.0},
            {"minimumLineLength",minimumLength},{"minimumBeamDirectionSine",minimumDirectionSine},
            {"preservesPhysicalIdentities",true},{"geometryDerivedBoundaryDirectionsSupported",false}};
}
