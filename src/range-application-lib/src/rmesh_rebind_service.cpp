#include <rmesh_rebind_service.h>
#include <rgeometry_candidate_service.h>
#include <rmesh_candidate_service.h>
#include <rmodel_command_service.h>
#include <rbl_error.h>
#include <rml_file_io.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace
{
using V = std::array<double,3>;
using Face = std::array<uint,3>;
constexpr uint nodeLimit = 10000;
constexpr uint elementLimit = 50000;
constexpr int bindingLimit = 1000;
constexpr double coordinateLimit = 1e6;
constexpr double forceLimit = 1e12;
constexpr double rankTolerance = 1e-10;
constexpr quint64 convexHalfSpacePairLimit = 20000000;
V xyz(const RNode &n) { return {n.getX(),n.getY(),n.getZ()}; }
V sub(const V &a,const V &b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
V cross(const V &a,const V &b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double dot(const V &a,const V &b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double norm(const V &a) { return std::hypot(a[0],a[1],a[2]); }
QJsonArray json(const V &v) { return {v[0],v[1],v[2]}; }
Face key(Face f) { std::sort(f.begin(),f.end()); return f; }
bool exact(const QJsonObject &o,const QStringList &fields)
{
    if (o.size()!=fields.size()) return false;
    for (const QString &f:fields) if (!o.contains(f)) return false;
    return true;
}
bool finite(const QJsonValue &v,double limit)
{
    return v.isDouble() && std::isfinite(v.toDouble()) && std::abs(v.toDouble())<=limit;
}
bool physicalId(const QJsonValue &v,uint &id)
{
    if (!v.isDouble()) return false;
    const double n=v.toDouble();
    if (!std::isfinite(n) || n<1 || n>=double(RConstants::eod) || std::floor(n)!=n) return false;
    id=uint(n); return true;
}
QString hash(const QJsonObject &o)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
// Existing native serializers require a real RSaveFile with a file engine.
// Use private automatically cleaned staging; never publish an owned model file.
template<class T> QByteArray nativeBytes(const T &value)
{
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("Cannot stage static binding verification.");
    const QString path=directory.filePath("value.bin");
    RSaveFile output(path,RSaveFile::BINARY); output.setVersion(RModel::version);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write static binding verification.");
    RFileIO::writeBinary(output,value);
    if (!output.commit()) throw std::runtime_error("Cannot commit static binding verification.");
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read static binding verification.");
    const QByteArray bytes=input.readAll();
    if (input.error()!=QFile::NoError) throw std::runtime_error("Cannot complete static binding verification read.");
    return bytes;
}
bool emptyRestart(const RStructuralDynamicsRestartData &r)
{
    // Native serialization omits inactive legacy restart vectors. Check these
    // too so unavailable never means stale state is safe to silently discard.
    const auto fields=[](const RStructuralDynamicsRestartData &s) {
        return std::tie(s.available,s.completedSteps,s.time,s.fixedTimeStep,s.meshNodeCount,s.meshElementCount,
            s.physicalDofCount,s.meshFingerprint,s.dofFingerprint,s.systemFingerprint,s.q,s.v,s.a,s.externalForce,
            s.cumulativeExternalWork,s.cumulativeDampingDissipation,s.initialMechanicalEnergy,s.supportResponseAvailable,
            s.supportFingerprint,s.supportReactions,s.supportPowers,s.cumulativeSupportWork,s.controlAvailable,
            s.controlProtocolVersion,s.controlFingerprint,s.controlChannelIds,s.controlLastCommands,s.controlPowers,
            s.cumulativeControlWork,s.controlControllerState,s.controlDeterministicSeed);
    };
    const RStructuralDynamicsRestartData defaults; return fields(r)==fields(defaults);
}
bool defaultUnboundSetup(const RModel &source,QJsonObject &proof)
{
    const QByteArray defaults=nativeBytes(RProblemSetup());
    const auto &setup=source.getProblemSetup();
    if (nativeBytes(setup)==defaults)
    {
        proof={{"profile","default_reset"},{"fullDefaultRemainderVerified",true}};
        return true;
    }
    // Model::initializeMeshInput seeds these two fields on native load and
    // Session import. They are bbox-derived sizing seeds, not measured mesh
    // edge extrema. Recompute from coordinates; never trust retained settings.
    V low=xyz(source.getNode(0)),high=low;
    for (uint n=0;n<source.getNNodes();++n)
    {
        const V p=xyz(source.getNode(n));
        for (int axis=0;axis<3;++axis)
        {
            if (!std::isfinite(p[axis]) || std::abs(p[axis])>coordinateLimit) return false;
            low[axis]=std::min(low[axis],p[axis]); high[axis]=std::max(high[axis],p[axis]);
        }
    }
    const V extent=sub(high,low);
    const double volumeConstraint=extent[0]*extent[1]*extent[2]/1000.0;
    const double maxSeed=std::cbrt(12.0*volumeConstraint/std::sqrt(2.0)),minSeed=maxSeed*0.1;
    if (!std::isfinite(maxSeed) || !(minSeed>0)
        || setup.getMeshSetup().getMinEdgeLength()!=minSeed
        || setup.getMeshSetup().getMaxEdgeLength()!=maxSeed) return false;
    RProblemSetup remainder(setup);
    remainder.getMeshSetup().setMinEdgeLength(0.0);
    remainder.getMeshSetup().setMaxEdgeLength(0.0);
    if (nativeBytes(remainder)!=defaults) return false;
    proof={{"profile","default_reset_with_bbox_sizing_seeds"},
        {"sizingSeedOrigin","Model::initializeMeshInput"},{"fullDefaultRemainderVerified",true},
        {"bboxSizingSeedsVerified",true},{"bboxVolumeConstraintM3",volumeConstraint},
        {"minimumSizingSeedM",minSeed},{"maximumSizingSeedM",maxSeed}};
    return true;
}
struct Binding
{
    uint physicalId=0, node=0;
    QJsonArray values;
};
QString convexBoundary(const RModel &source,QJsonObject &report)
{
    // Reconstruct only the exterior shell. These temporary positions/IDs are
    // never binding targets; explicit source IDs remain authoritative below.
    const QJsonObject profile=RMeshCandidateService::capabilities();
    const uint boundaryNodeLimit=uint(profile.value("inputNodeMaximum").toDouble());
    const uint boundaryTriangleLimit=uint(profile.value("inputTriangleMaximum").toDouble());
    std::set<uint> boundaryNodes; uint triangles=0;
    for (uint g=0;g<source.getNSurfaces();++g)
        for (uint j=0;j<source.getSurface(g).size();++j)
        {
            ++triangles; const auto &e=source.getElement(source.getSurface(g).get(j));
            for (uint k=0;k<3;++k) boundaryNodes.insert(e.getNodeId(k));
        }
    if (!boundaryNodeLimit || !boundaryTriangleLimit || boundaryNodes.size()>boundaryNodeLimit || triangles>boundaryTriangleLimit)
        return "Convex boundary exceeds the shared WF14 shell preview profile.";
    if (quint64(source.getNNodes())*triangles>convexHalfSpacePairLimit)
        return "Convex volume-node half-space verification exceeds the fixed 20000000-pair budget.";
    RModel shell; shell.setNNodes(uint(boundaryNodes.size())); shell.setNSurfaces(source.getNSurfaces());
    std::map<uint,uint> positions; uint position=0; QJsonArray sourceBoundaryPhysicalNodeIds;
    V low=xyz(source.getNode(*boundaryNodes.begin())),high=low;
    for (uint sourcePosition:boundaryNodes)
    {
        const V p=xyz(source.getNode(sourcePosition)); shell.getNode(position).set(p[0],p[1],p[2]);
        positions[sourcePosition]=position++; sourceBoundaryPhysicalNodeIds.append(double(source.getNodePhysicalId(sourcePosition)));
        for (int axis=0;axis<3;++axis) { low[axis]=std::min(low[axis],p[axis]); high[axis]=std::max(high[axis],p[axis]); }
    }
    RMaterial unusedMaterial; unusedMaterial.setID(QUuid());
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        // Old materials play no role in geometric eligibility or new physics.
        shell.getSurface(g).setMaterial(unusedMaterial);
        shell.getSurface(g).setName(QString("WF14 eligibility boundary g:%1").arg(source.getSurface(g).getPhysicalId()));
        for (uint j=0;j<source.getSurface(g).size();++j)
        {
            const auto &e=source.getElement(source.getSurface(g).get(j)); RElement triangle(R_ELEMENT_TRI1);
            for (uint k=0;k<3;++k) triangle.setNodeId(k,positions.at(e.getNodeId(k)));
            shell.addElement(triangle,true,g);
        }
    }
    const V extent=sub(high,low); const double boxVolume=extent[0]*extent[1]*extent[2],scale=norm(extent);
    if (!std::isfinite(boxVolume) || !(boxVolume>0) || !std::isfinite(scale) || !(scale>0))
        return "Convex shell requires finite positive bounding-box volume and length scale.";
    // A coarsest bounded profile request is only used for the pure shell guard.
    // No generator or quality/volume target for the current tetrahedra is run.
    const double maxVolume=std::clamp(boxVolume,1e-15,1e18);
    const auto preview=RMeshCandidateService::preview(shell,QJsonObject{{"unit","m"},{"volumeUnit","m^3"},
        {"maxVolume",maxVolume},{"radiusEdgeRatio",1.6}});
    if (!preview.valid) return "WF14 convex boundary preview rejected the mesh: "+preview.message;
    for (uint g=0;g<source.getNSurfaces();++g)
        for (uint j=0;j<source.getSurface(g).size();++j)
        {
            const auto &e=source.getElement(source.getSurface(g).get(j)); const V origin=xyz(source.getNode(e.getNodeId(0)));
            const V normal=cross(sub(xyz(source.getNode(e.getNodeId(1))),origin),sub(xyz(source.getNode(e.getNodeId(2))),origin));
            const double tolerance=1e-10*scale*norm(normal);
            for (uint n=0;n<source.getNNodes();++n)
                if (dot(normal,sub(xyz(source.getNode(n)),origin))>tolerance)
                    return "Every volume node must lie inside the verified convex boundary half-spaces.";
        }
    report={{"scope","WF14_convex_single_region"},{"boundaryShellPreviewVerified",true},
        {"volumeNodesInsideConvexHalfSpacesVerified",true},{"generatorInvoked",false},
        {"boundaryNodeCount",double(boundaryNodes.size())},{"boundaryTriangleCount",double(triangles)},
        {"sourceBoundaryNodePhysicalIds",sourceBoundaryPhysicalNodeIds},{"shellPreviewArguments",preview.normalizedArguments},
        {"shellPreviewGeometrySha256",preview.sourceGeometrySha256},{"convexShellVolumeM3",preview.report.value("sourceVolume")},
        {"halfSpacePairCount",double(quint64(source.getNNodes())*triangles)},
        {"halfSpacePairMaximum",double(convexHalfSpacePairLimit)}};
    return {};
}
// Independently validate conforming single-region topology. Every boundary face
// must be explicitly labelled once; face connectivity excludes bodies touching
// only along an edge/node, for which a single six-mode check is insufficient.
QString meshGeometry(const RModel &m,QJsonObject &eligibility,bool includePointTargets=false)
{
    if (m.getNNodes()<4 || m.getNNodes()>nodeLimit || m.getNElements()<5 || m.getNElements()>elementLimit
        || (!includePointTargets && m.getNPoints()) || m.getNPoints()>2*uint(bindingLimit)
        || m.getNLines() || m.getNVolumes()!=1 || !m.getNSurfaces()
        || m.getNSurfaces()>elementLimit) return "Requires bounded TETRA1 volume and TRI1 boundary groups only.";
    std::set<uint> ids;
    std::set<V> coordinates;
    for (uint n=0;n<m.getNNodes();++n)
    {
        const uint id=m.getNodePhysicalId(n);
        if (!id || id==RConstants::eod || !ids.insert(id).second) return "Invalid or duplicate physical node IDs.";
        const V p=xyz(m.getNode(n));
        for (double c:p) if (!std::isfinite(c) || std::abs(c)>coordinateLimit) return "Invalid SI node coordinates.";
        if (!coordinates.insert(p).second) return "Coincident nodes are unsupported.";
    }
    ids.clear(); std::vector<bool> covered(m.getNElements(),false),used(m.getNNodes(),false);
    std::map<Face,std::vector<uint>> faces;
    std::map<Face,Face> outward;
    std::set<Face> boundary;
    std::set<std::array<uint,4>> tetrahedra;
    std::vector<uint> tets; double tetrahedronVolume=0;
    bool pointTail=false;
    for (uint e=0;e<m.getNElements();++e)
    {
        if (!m.getElementPhysicalId(e) || m.getElementPhysicalId(e)==RConstants::eod
            || !ids.insert(m.getElementPhysicalId(e)).second) return "Invalid or duplicate physical element IDs.";
        const RElement &el=m.getElement(e);
        if (includePointTargets && el.getType()==R_ELEMENT_POINT)
        {
            if (el.size()!=1 || el.getNodeId(0)>=m.getNNodes()) return "Invalid point target connectivity.";
            pointTail=true;
            continue;
        }
        if (pointTail) return "Static point targets must follow the complete tetrahedral mesh.";
        if ((el.getType()!=R_ELEMENT_TRI1 && el.getType()!=R_ELEMENT_TETRA1)
            || el.size()!=(el.getType()==R_ELEMENT_TRI1 ? 3u : 4u)) return "Only linear TRI1/TETRA1 is supported.";
        std::set<uint> distinct;
        for (uint j=0;j<el.size();++j)
            if (el.getNodeId(j)>=m.getNNodes() || !distinct.insert(el.getNodeId(j)).second) return "Invalid mesh connectivity.";
        if (el.getType()==R_ELEMENT_TRI1)
        {
            if (!boundary.insert(key({el.getNodeId(0),el.getNodeId(1),el.getNodeId(2)})).second) return "Duplicate boundary facet.";
            continue;
        }
        std::array<uint,4> sorted{el.getNodeId(0),el.getNodeId(1),el.getNodeId(2),el.getNodeId(3)};
        std::sort(sorted.begin(),sorted.end());
        if (!tetrahedra.insert(sorted).second) return "Duplicate tetrahedron.";
        const V a=xyz(m.getNode(el.getNodeId(0))),b=sub(xyz(m.getNode(el.getNodeId(1))),a),
            c=sub(xyz(m.getNode(el.getNodeId(2))),a),d=sub(xyz(m.getNode(el.getNodeId(3))),a);
        const double length=std::max({norm(b),norm(c),norm(d)}),det=dot(b,cross(c,d));
        if (!std::isfinite(det) || !(det>1e-14*length*length*length)) return "Nonpositive or unresolved tetrahedron Jacobian.";
        tetrahedronVolume+=det/6;
        const uint t=uint(tets.size()); tets.push_back(e);
        const uint f[4][3]={{1,2,3},{0,3,2},{0,1,3},{0,2,1}};
        for (const auto &local:f)
        {
            Face face{el.getNodeId(local[0]),el.getNodeId(local[1]),el.getNodeId(local[2])};
            const auto old=outward.find(key(face));
            if (old!=outward.end())
            {
                const Face &previous=old->second; const V p=xyz(m.getNode(face[0])),o=xyz(m.getNode(previous[0]));
                if (!(dot(cross(sub(xyz(m.getNode(face[1])),p),sub(xyz(m.getNode(face[2])),p)),
                          cross(sub(xyz(m.getNode(previous[1])),o),sub(xyz(m.getNode(previous[2])),o)))<0))
                    return "Tetrahedra sharing a face must lie on opposite sides.";
            }
            faces[key(face)].push_back(t); outward[key(face)]=face;
        }
        for (uint j=0;j<4;++j) used[el.getNodeId(j)]=true;
    }
    if (tets.empty() || std::find(used.begin(),used.end(),false)!=used.end()) return "Every node must belong to the volume.";
    std::vector<std::vector<uint>> adjacency(tets.size()); std::set<Face> expected;
    for (const auto &entry:faces)
    {
        if (entry.second.size()==1) expected.insert(entry.first);
        else if (entry.second.size()==2)
        {
            adjacency[entry.second[0]].push_back(entry.second[1]); adjacency[entry.second[1]].push_back(entry.second[0]);
        }
        else return "Nonmanifold tetrahedron face incidence.";
    }
    if (boundary!=expected) return "TRI1 boundary must match every exterior tetrahedron face exactly.";
    std::vector<bool> reached(tets.size(),false); std::vector<uint> queue{0}; reached[0]=true;
    for (size_t q=0;q<queue.size();++q) for (uint t:adjacency[queue[q]]) if (!reached[t]) { reached[t]=true; queue.push_back(t); }
    if (queue.size()!=tets.size()) return "Only one face-connected volume is supported.";
    ids.clear();
    for (uint g=0;g<m.getNElementGroups();++g)
    {
        const RElementGroup &group=*m.getElementGroupPtr(g);
        if (!group.getPhysicalId() || group.getPhysicalId()==RConstants::eod || !ids.insert(group.getPhysicalId()).second || group.empty())
            return "Invalid or empty physical element group.";
    }
    const auto membership=[&](const RElementGroup &group,RElementType type) {
        for (uint j=0;j<group.size();++j)
        {
            const uint e=group.get(j);
            if (e>=m.getNElements() || covered[e] || m.getElement(e).getType()!=type) return false;
            covered[e]=true;
        }
        return true;
    };
    if (!membership(m.getVolume(0),R_ELEMENT_TETRA1)) return "Volume group membership is invalid.";
    for (uint g=0;g<m.getNSurfaces();++g) if (!membership(m.getSurface(g),R_ELEMENT_TRI1)) return "Surface group membership is invalid.";
    if (includePointTargets)
        for (uint g=0;g<m.getNPoints();++g) if (!membership(m.getPoint(g),R_ELEMENT_POINT)) return "Point group membership is invalid.";
    if (std::find(covered.begin(),covered.end(),false)!=covered.end()) return "Every mesh element needs exactly one group.";
    for (uint e=0;e<m.getNElements();++e) if (m.getElement(e).getType()==R_ELEMENT_TRI1)
    {
        const auto &el=m.getElement(e); const Face face{el.getNodeId(0),el.getNodeId(1),el.getNodeId(2)};
        const Face f=outward.at(key(face));
        const V p=xyz(m.getNode(face[0])),o=xyz(m.getNode(f[0]));
        if (!(dot(cross(sub(xyz(m.getNode(face[1])),p),sub(xyz(m.getNode(face[2])),p)),
                  cross(sub(xyz(m.getNode(f[1])),o),sub(xyz(m.getNode(f[2])),o)))>0)) return "Boundary triangles must face outwards.";
    }
    const QString error=convexBoundary(m,eligibility); if (!error.isEmpty()) return error;
    const double shellVolume=eligibility.value("convexShellVolumeM3").toDouble();
    if (!std::isfinite(tetrahedronVolume) || !(shellVolume>0) || std::abs(tetrahedronVolume-shellVolume)>1e-8*shellVolume)
        return "Oriented tetrahedron volume must equal the verified simple convex shell volume.";
    eligibility.insert("tetrahedronVolumeM3",tetrahedronVolume); eligibility.insert("tetrahedronAndConvexShellVolumeAgree",true);
    return {};
}
QString mesh(const RModel &m,QJsonObject &eligibility)
{
    if (m.getProblemTaskTree().getProblemTypeMask()!=R_PROBLEM_NONE)
        return "Static rebinding requires an unbound NONE task tree.";
    if (m.getNNodes()<4 || m.getNNodes()>nodeLimit || m.getNElements()<5 || m.getNElements()>elementLimit
        || m.getNPoints() || m.getNLines() || m.getNVolumes()!=1 || !m.getNSurfaces()
        || m.getNSurfaces()>elementLimit) return "Requires bounded TETRA1 volume and TRI1 boundary groups only.";
    const auto &setup=m.getProblemSetup(); const auto &interactions=setup.getStructuralInteractionSetup();
    QJsonObject setupProof;
    if (!emptyRestart(setup.getStructuralDynamicsRestartData()) || m.getProblemTaskTree().getNChildren()
        || !interactions.getLinearConstraints().empty() || !interactions.getCouplings().empty() || !interactions.getSensors().empty()
        || !interactions.getActuators().empty() || !interactions.getPrescribedMotions().empty()
        || !setup.getStructuralControlSetup().controllerId.isEmpty() || !setup.getStructuralControlSetup().channelConditioning.empty()
        || !setup.getStructuralCableSetup().meshFingerprint.isEmpty() || !setup.getStructuralCableSetup().members.empty()
        || !setup.getStructuralPiezoSetup().meshFingerprint.isEmpty() || !setup.getStructuralPiezoSetup().ports.empty()
        || !setup.getStructuralPiezoSetup().nodalBoundaryConditions.empty() || !setup.getRadiationSetup().getViewFactorMatrixFile().isEmpty()
        || !setup.getStructuralThermalControlSetup().channels.empty() || !setup.getStructuralThermalControlSetup().sparseObservationNodeIds.empty()
        || !setup.getStructuralDynamicsSetup().getModalDampingRatios().empty() || !setup.getMeshSetup().getVariables().empty()
        || m.getTimeSolver().getTimes().size()>1 || m.getNVariables() || !m.getMonitoringPointManager().empty()
        || m.getNVectorFields() || m.getNScalarFields() || m.getNStreamLines() || m.getNCuts() || m.getNIsos()
        || nativeBytes(m.getProblemTaskTree())!=nativeBytes(RProblemTaskItem())
        || !defaultUnboundSetup(m,setupProof) || nativeBytes(m.getTimeSolver())!=nativeBytes(RTimeSolver()))
        return "Unbound mesh must have default-reset problem/time setup and no retained results, monitoring, or derived mappings.";
    for (uint g=0;g<m.getNElementGroups();++g)
    {
        const auto &group=*m.getElementGroupPtr(g);
        if (group.getNBoundaryConditions() || group.getNInitialConditions() || group.getNEnvironmentConditions())
            return "Unbound geometry may not retain old boundary, initial, or environment conditions.";
        if (!group.getData().getVariableData().empty()) return "Unbound geometry may not retain entity variable mappings.";
    }
    const QString error=meshGeometry(m,eligibility);
    if (error.isEmpty()) eligibility.insert("sourceSetupProof",setupProof);
    return error;
}
QString bindings(const RModel &m,const QJsonValue &v,bool load,std::vector<Binding> &out)
{
    if (!v.isArray() || v.toArray().isEmpty() || v.toArray().size()>bindingLimit) return "Each binding array requires 1..1000 targets.";
    std::set<uint> ids;
    for (const QJsonValue &entry:v.toArray())
    {
        if (!entry.isObject()) return "Each target must be an object.";
        const QJsonObject o=entry.toObject(); const QString field=load ? "forceN" : "values";
        Binding b;
        if (!exact(o,{"nodePhysicalId",field}) || !physicalId(o.value("nodePhysicalId"),b.physicalId)
            || !ids.insert(b.physicalId).second) return "Missing, unknown, duplicate, or invalid target fields/physical IDs.";
        b.node=m.findNodePositionByPhysicalId(b.physicalId);
        if (b.node==RConstants::eod) return "Target physical node ID does not exist; remapping is forbidden.";
        if (!o.value(field).isArray() || o.value(field).toArray().size()!=3) return "Target vectors require exactly three global XYZ entries.";
        b.values=o.value(field).toArray(); bool enabled=false,nonzero=false;
        for (const QJsonValue &c:b.values)
        {
            if (!load && c.isNull()) continue;
            if (!finite(c,load ? forceLimit : coordinateLimit)) return "Target vector has a nonfinite, wrong-type, or out-of-bounds SI component.";
            enabled=true; nonzero=nonzero || c.toDouble()!=0;
        }
        if (!enabled || (load && !nonzero)) return "Constraints need an enabled axis; each force vector must be nonzero.";
        out.push_back(b);
    }
    std::sort(out.begin(),out.end(),[](const Binding &a,const Binding &b) { return a.physicalId<b.physicalId; });
    return {};
}
// Six exact rigid displacement fields: three translations and omega cross r.
// Center/scale geometry, then twice reorthogonalize across ALL mesh DOFs so the
// constraint rank is independent of coordinate origin, units, and axis inertia.
int constraintRank(const RModel &m,const std::vector<Binding> &constraints)
{
    V center{},low=xyz(m.getNode(0)),high=low;
    for (uint n=0;n<m.getNNodes();++n)
    {
        const V p=xyz(m.getNode(n));
        for (int j=0;j<3;++j) { center[j]+=p[j]/m.getNNodes(); low[j]=std::min(low[j],p[j]); high[j]=std::max(high[j],p[j]); }
    }
    const double scale=norm(sub(high,low)); if (!(scale>0)) return 0;
    std::array<std::vector<double>,6> q;
    for (auto &column:q) column.resize(3*m.getNNodes(),0);
    for (uint n=0;n<m.getNNodes();++n)
    {
        V r=sub(xyz(m.getNode(n)),center); for (double &c:r) c/=scale;
        for (int j=0;j<3;++j) q[j][3*n+j]=1;
        q[3][3*n+1]=-r[2]; q[3][3*n+2]=r[1];
        q[4][3*n]=r[2]; q[4][3*n+2]=-r[0];
        q[5][3*n]=-r[1]; q[5][3*n+1]=r[0];
    }
    for (int j=0;j<6;++j)
    {
        for (int pass=0;pass<2;++pass) for (int k=0;k<j;++k)
        {
            double projection=0; for (size_t i=0;i<q[j].size();++i) projection+=q[k][i]*q[j][i];
            for (size_t i=0;i<q[j].size();++i) q[j][i]-=projection*q[k][i];
        }
        double length=0; for (double v:q[j]) length+=v*v; length=std::sqrt(length);
        if (!(length>1e-12*std::sqrt(double(m.getNNodes())))) return j;
        for (double &v:q[j]) v/=length;
    }
    std::vector<std::array<double,6>> a;
    for (const Binding &b:constraints) for (int axis=0;axis<3;++axis) if (!b.values[axis].isNull())
    {
        std::array<double,6> row{}; for (int j=0;j<6;++j) row[j]=q[j][3*b.node+axis]; a.push_back(row);
    }
    double maximum=0; for (const auto &row:a) for (double v:row) maximum=std::max(maximum,std::abs(v));
    int rank=0;
    while (rank<6 && rank<int(a.size()))
    {
        int bestRow=rank,bestCol=rank; double pivot=0;
        for (int i=rank;i<int(a.size());++i) for (int j=rank;j<6;++j)
            if (std::abs(a[i][j])>pivot) { pivot=std::abs(a[i][j]); bestRow=i; bestCol=j; }
        if (pivot<=rankTolerance*maximum) break;
        std::swap(a[rank],a[bestRow]); for (auto &row:a) std::swap(row[rank],row[bestCol]);
        for (int i=rank+1;i<int(a.size());++i)
        {
            const double factor=a[i][rank]/a[rank][rank];
            for (int j=rank+1;j<6;++j) a[i][j]-=factor*a[rank][j]; a[i][rank]=0;
        }
        ++rank;
    }
    return rank;
}
RBoundaryCondition condition(const Binding &binding,bool load)
{
    RBoundaryCondition bc(load ? R_BOUNDARY_CONDITION_FORCE : R_BOUNDARY_CONDITION_DISPLACEMENT);
    const RVariableType vars[3]={load ? R_VARIABLE_FORCE_X : R_VARIABLE_DISPLACEMENT_X,
        load ? R_VARIABLE_FORCE_Y : R_VARIABLE_DISPLACEMENT_Y,load ? R_VARIABLE_FORCE_Z : R_VARIABLE_DISPLACEMENT_Z};
    for (int axis=0;axis<3;++axis)
    {
        const uint position=bc.findComponentPosition(vars[axis]);
        if (position>=bc.size()) throw std::runtime_error("Missing native boundary XYZ component.");
        RConditionComponent &c=bc.getComponent(position); c.clear(); c.setEnabled(!binding.values[axis].isNull());
        if (c.getEnabled()) c.add(0,binding.values[axis].toDouble());
    }
    return bc;
}
}

bool RMeshRebindService::hasMaterialFreeStaticBoundaryRoles(const RModel &m)
{
    try
    {
        if (m.getProblemTaskTree().getProblemTypeMask()!=R_PROBLEM_STRESS
            || m.getNNodes()<4 || m.getNNodes()>nodeLimit || m.getNElements()<5 || m.getNElements()>elementLimit
            || m.getNPoints()>2*uint(bindingLimit) || m.getNLines() || m.getNVolumes()!=1
            || !m.getNSurfaces() || m.getNSurfaces()>elementLimit) return false;
        const auto &setup=m.getProblemSetup(); QJsonObject proof;
        if (!emptyRestart(setup.getStructuralDynamicsRestartData()) || !defaultUnboundSetup(m,proof)
            || nativeBytes(m.getTimeSolver())!=nativeBytes(RTimeSolver())
            || nativeBytes(m.getProblemTaskTree())!=nativeBytes(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS)))
            return false;
        const auto &material=m.getVolume(0).getMaterial();
        if (!material.validForProblemType(R_PROBLEM_STRESS)) return false;
        const auto value=[&material](RMaterialProperty::Type type,double &number) {
            const uint position=material.findPosition(type);
            if (position>=material.size() || material.get(position).size()!=1) return false;
            number=material.get(position).getValue(0); return std::isfinite(number);
        };
        double e=0,nu=0,rho=0,alpha=0;
        if (!value(RMaterialProperty::ModulusOfElasiticity,e) || !(e>0)
            || !value(RMaterialProperty::PoissonRatio,nu) || !(nu>-1 && nu<0.5)
            || !value(RMaterialProperty::Density,rho) || !(rho>0)
            || !value(RMaterialProperty::ThermalExpansionCoefficient,alpha) || alpha!=0) return false;
        // The native POINT matrix has zero stiffness; volume==0 also excludes
        // density-dependent mass/own-weight. Only explicit nodal bindings qualify.
        for (uint p=0;p<m.getNPoints();++p)
        {
            const auto &point=m.getPoint(p);
            if (point.getVolume()!=0 || point.size()!=1 || !point.getNBoundaryConditions()
                || point.getNBoundaryConditions()>2) return false;
            for (uint c=0;c<point.getNBoundaryConditions();++c)
            {
                const auto type=point.getBoundaryCondition(c).getType();
                if (type!=R_BOUNDARY_CONDITION_FORCE && type!=R_BOUNDARY_CONDITION_DISPLACEMENT) return false;
            }
        }
        // Axisymmetric surfaces remain structural even when thickness is zero.
        // Planar zero-thickness exterior triangles carry no material-dependent
        // K/M/thermal/own-weight in RSolverStress's structuralSurface branch.
        for (uint s=0;s<m.getNSurfaces();++s)
        {
            const auto &surface=m.getSurface(s);
            const auto formulation=surface.getStructuralFormulation();
            if (surface.getThickness()!=0 || (formulation!=RElementFormulation::PlaneStress
                                             && formulation!=RElementFormulation::PlaneStrain)
                || surface.getNBoundaryConditions()) return false;
        }
        if (m.getVolume(0).getNBoundaryConditions()) return false;
        for (uint g=0;g<m.getNElementGroups();++g)
        {
            const auto &group=*m.getElementGroupPtr(g);
            if (group.getNInitialConditions() || group.getNEnvironmentConditions()
                || !group.getData().getVariableData().empty()) return false;
        }
        QJsonObject geometryProof;
        return meshGeometry(m,geometryProof,true).isEmpty();
    }
    catch (...) { return false; }
}

RMeshRebindService::RebindCandidate RMeshRebindService::preview(const RModel &source,const QJsonObject &arguments)
{
    RebindCandidate result;
    const auto fail=[&](const QString &code,const QString &message) {
        result.valid=false; result.code=code; result.message=message; result.candidate=RModel();
        result.postGeometrySha256.clear(); result.postStateSha256.clear(); result.report={}; return result;
    };
    try
    {
        if (!exact(arguments,{"unitSI","staticStress","material","constraints","loads"})
            || arguments.value("unitSI")!=QJsonValue(true) || arguments.value("staticStress")!=QJsonValue(true)
            || !arguments.value("material").isObject()) return fail("invalid_arguments","Expected exact static SI material, constraints, and loads fields.");
        const QJsonObject material=arguments.value("material").toObject();
        if (!exact(material,{"youngModulusPa","poissonRatio","densityKgM3"})
            || !finite(material.value("youngModulusPa"),1e18) || material.value("youngModulusPa").toDouble()<1e-12
            || !finite(material.value("densityKgM3"),1e12) || material.value("densityKgM3").toDouble()<1e-12
            || !finite(material.value("poissonRatio"),1) || material.value("poissonRatio").toDouble()<=-1
            || material.value("poissonRatio").toDouble()>=0.5) return fail("invalid_material","Material requires E in [1e-12,1e18] Pa, density in [1e-12,1e12] kg/m^3, and -1<nu<0.5.");
        const double e=material.value("youngModulusPa").toDouble(),nu=material.value("poissonRatio").toDouble();
        const double mu=e/(2*(1+nu)),lambda=e*nu/((1+nu)*(1-2*nu)),bulk=e/(3*(1-2*nu));
        if (!std::isfinite(mu) || !std::isfinite(lambda) || !std::isfinite(bulk) || !std::isfinite(lambda+2*mu)
            || !(mu>0) || !(bulk>0)) return fail("invalid_material","Isotropic constitutive coefficients must be finite and positive definite.");
        QJsonObject eligibility;
        const QString meshError=mesh(source,eligibility); if (!meshError.isEmpty()) return fail("unsupported_mesh",meshError);
        std::vector<Binding> constraints,loads;
        QString error=bindings(source,arguments.value("constraints"),false,constraints);
        if (!error.isEmpty()) return fail("invalid_constraints",error);
        error=bindings(source,arguments.value("loads"),true,loads); if (!error.isEmpty()) return fail("invalid_loads",error);
        const int rank=constraintRank(source,constraints);
        if (rank!=6) return fail("insufficient_constraints",QString("Prescribed global axes constrain %1 of six rigid-body modes.").arg(rank));
        result.sourceGeometrySha256=RGeometryCandidateService::geometryFingerprint(source);
        result.sourceStateSha256=RModelCommandService::stateFingerprint(source);
        RModel candidate(source);
        // Replace every model-bound analysis surface before constructing new bindings.
        candidate.setProblemSetup(RProblemSetup()); candidate.setTimeSolver(RTimeSolver());
        candidate.setMonitoringPointManager(RMonitoringPointManager());
        candidate.clearResults(); candidate.removeAllVariables(); candidate.clearBoundaryConditions();
        candidate.clearInitialConditions(); candidate.clearEntityVariableData();
        // Legacy clearEnvironmentConditions restores gravity/temperature
        // defaults. Erase entries individually to preserve zero-EC eligibility.
        for (uint g=0;g<candidate.getNElementGroups();++g)
            while (candidate.getElementGroupPtr(g)->getNEnvironmentConditions())
                candidate.getElementGroupPtr(g)->removeEnvironmentCondition(0u);
        candidate.setNVectorFields(0); candidate.setNScalarFields(0); candidate.setNStreamLines(0); candidate.setNCuts(0); candidate.setNIsos(0);
        RMaterial emptyMaterial; emptyMaterial.setID(QUuid());
        for (uint g=0;g<candidate.getNElementGroups();++g) candidate.getElementGroupPtr(g)->setMaterial(emptyMaterial);
        RMaterial solid(RMaterial::Solid); solid.setName("WF14 rebound isotropic solid");
        // Deterministic identity derived from explicit new material data; no old UUID/table reuse.
        solid.setID(QUuid::createUuidV5(QUuid("{3349913a-1c5f-4fb4-a61c-5b34af9fa78e}"),QJsonDocument(material).toJson(QJsonDocument::Compact)));
        const RMaterialProperty::Type fields[4]={RMaterialProperty::ModulusOfElasiticity,RMaterialProperty::PoissonRatio,
            RMaterialProperty::Density,RMaterialProperty::ThermalExpansionCoefficient};
        // Native Stress requires a thermal-expansion property even in a purely
        // mechanical solve. Explicit zero adds no coupled/thermal load.
        const double values[4]={e,nu,material.value("densityKgM3").toDouble(),0};
        for (int i=0;i<4;++i) { RMaterialProperty p(fields[i]); p.clear(); p.add(293.15,values[i]); solid.add(p); }
        if (!solid.validForProblemType(R_PROBLEM_STRESS)) return fail("invalid_material","Native Stress material requirements are not satisfied.");
        candidate.getVolume(0).setMaterial(solid);
        std::set<uint> sourceElementIds,sourceGroupIds;
        for (uint i=0;i<source.getNElements();++i) sourceElementIds.insert(source.getElementPhysicalId(i));
        for (uint i=0;i<source.getNElementGroups();++i) sourceGroupIds.insert(source.getElementGroupPtr(i)->getPhysicalId());
        std::map<uint,uint> targets; QJsonArray targetReports,newElementIds,newGroupIds,constraintIds,loadIds,normalizedConstraints,normalizedLoads;
        std::map<uint,uint> targetNodes;
        for (const auto *list:{&constraints,&loads}) for (const Binding &b:*list) targetNodes[b.physicalId]=b.node;
        if (quint64(source.getNElements())+targetNodes.size()>elementLimit) return fail("model_limit","Adding point targets exceeds the element limit.");
        for (const auto &entry:targetNodes)
        {
            const uint point=candidate.getNPoints(); candidate.addPoint();
            RElement element(R_ELEMENT_POINT); element.setNodeId(0,entry.second); candidate.addElement(element,true,point);
            RPoint &group=candidate.getPoint(point); group.setName(QString("WF14 node %1").arg(entry.first));
            group.removeAllBoundaryConditions(); group.removeAllInitialConditions();
            while (group.getNEnvironmentConditions()) group.removeEnvironmentCondition(0u);
            group.setMaterial(emptyMaterial);
            const uint elementId=candidate.getElementPhysicalId(candidate.getNElements()-1),groupId=group.getPhysicalId();
            if (!elementId || elementId==RConstants::eod || !groupId || groupId==RConstants::eod
                || !sourceElementIds.insert(elementId).second || !sourceGroupIds.insert(groupId).second)
                return fail("invalid_identity","Fresh target IDs collide with source identities.");
            targets[entry.first]=point; newElementIds.append(double(elementId)); newGroupIds.append(double(groupId));
            targetReports.append(QJsonObject{{"nodePhysicalId",double(entry.first)},{"elementPhysicalId",double(elementId)},
                {"groupPhysicalId",double(groupId)},{"targetId",QString("g:%1").arg(groupId)}});
        }
        for (const Binding &b:constraints)
        {
            auto &group=candidate.getPoint(targets.at(b.physicalId)); group.addBoundaryCondition(condition(b,false));
            constraintIds.append(double(b.physicalId)); normalizedConstraints.append(QJsonObject{{"nodePhysicalId",double(b.physicalId)},{"values",b.values}});
        }
        V resultant{},moment{};
        for (const Binding &b:loads)
        {
            auto &group=candidate.getPoint(targets.at(b.physicalId)); group.addBoundaryCondition(condition(b,true));
            const V force{b.values[0].toDouble(),b.values[1].toDouble(),b.values[2].toDouble()},torque=cross(xyz(candidate.getNode(b.node)),force);
            for (int j=0;j<3;++j) { resultant[j]+=force[j]; moment[j]+=torque[j]; }
            loadIds.append(double(b.physicalId)); normalizedLoads.append(QJsonObject{{"nodePhysicalId",double(b.physicalId)},{"forceN",b.values}});
        }
        for (double v:resultant) if (!std::isfinite(v)) return fail("invalid_loads","Nonfinite force resultant.");
        for (double v:moment) if (!std::isfinite(v)) return fail("invalid_loads","Nonfinite global-origin load moment.");
        // This is the last physics-changing operation: no partial candidate can solve.
        candidate.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
        candidate.clearResults(); candidate.removeAllVariables();
        if (candidate.getProblemTaskTree().getProblemTypeMask()!=R_PROBLEM_STRESS
            || candidate.getProblemSetup().getStructuralDynamicsSetup().getEnabled()
            || candidate.getProblemSetup().getStructuralControlSetup().enabled
            || candidate.getProblemSetup().getStructuralDynamicsRestartData().available || candidate.getProblemSetup().getRestart())
            return fail("invalid_candidate","Candidate did not remain a fresh static-only analysis.");
        if (nativeBytes(candidate.getProblemSetup())!=nativeBytes(RProblemSetup())
            || nativeBytes(candidate.getTimeSolver())!=nativeBytes(RTimeSolver()) || !emptyRestart(candidate.getProblemSetup().getStructuralDynamicsRestartData())
            || candidate.getNVariables() || constraintRank(candidate,constraints)!=6)
            return fail("invalid_candidate","Static candidate setup, result state, or rigid-body rank changed unexpectedly.");
        for (uint g=0;g<candidate.getNElementGroups();++g)
        {
            const auto &group=*candidate.getElementGroupPtr(g);
            if (group.getNEnvironmentConditions() || group.getNInitialConditions() || !group.getData().getVariableData().empty())
                return fail("invalid_candidate","Static candidate retained an environment/initial condition or entity mapping.");
        }
        if (candidate.getVolume(0).getMaterial().toJson()!=solid.toJson())
            return fail("invalid_candidate","Static volume material changed during binding.");
        for (const auto *list:{&constraints,&loads}) for (const Binding &b:*list)
        {
            const bool load=list==&loads; const auto &group=candidate.getPoint(targets.at(b.physicalId));
            const auto type=load ? R_BOUNDARY_CONDITION_FORCE : R_BOUNDARY_CONDITION_DISPLACEMENT;
            if (group.size()!=1 || candidate.getElement(group.get(0)).getType()!=R_ELEMENT_POINT
                || candidate.getElement(group.get(0)).getNodeId(0)!=b.node || !group.hasBoundaryCondition(type)
                || nativeBytes(group.getBoundaryCondition(type))!=nativeBytes(condition(b,load)))
                return fail("invalid_candidate","Fresh single-node boundary target failed typed-condition verification.");
        }
        result.normalizedArguments={{"unitSI",true},{"staticStress",true},{"material",material},{"constraints",normalizedConstraints},{"loads",normalizedLoads}};
        result.postGeometrySha256=RGeometryCandidateService::geometryFingerprint(candidate);
        result.postStateSha256=RModelCommandService::stateFingerprint(candidate);
        result.report={{"physicsState","BOUND_STATIC"},{"bindingState","BOUND_STATIC"},{"requiresRebinding",false},
            {"unitSI",true},{"staticStress",true},{"taskMask",double(R_PROBLEM_STRESS)},{"meshEligibility",eligibility},
            {"sourceGeometrySha256",result.sourceGeometrySha256},{"postGeometrySha256",result.postGeometrySha256},
            {"sourceStateSha256",result.sourceStateSha256},{"postStateSha256",result.postStateSha256},
            {"bindingFingerprintScope","supported_model_command_state"},{"normalizedArgumentsSha256",hash(result.normalizedArguments)},
            {"newTargets",targetReports},{"newElementPhysicalIds",newElementIds},{"newGroupPhysicalIds",newGroupIds},
            {"constraintNodePhysicalIds",constraintIds},{"loadNodePhysicalIds",loadIds},{"rigidBodyConstraintRank",rank},
            {"rigidBodyModeCount",6},{"rankRelativeTolerance",rankTolerance},{"material",material},{"thermalExpansionPerK",0},
            {"materialUnits",QJsonObject{{"youngModulusPa","Pa"},{"poissonRatio","1"},{"densityKgM3","kg/m^3"}}},
            {"volumePhysicalId",double(candidate.getVolume(0).getPhysicalId())},{"forceResultantN",json(resultant)},
            {"forceMomentAboutGlobalOriginNm",json(moment)},{"controlEnabled",false},{"dynamicsEnabled",false},
            {"restartAvailable",false},{"romAvailable",false},{"resultVariableCount",double(candidate.getNVariables())}};
        result.valid=true; result.code="ok"; result.message="Complete detached static binding; publication and solve validation remain caller-owned.";
        result.candidate=candidate; return result;
    }
    catch (const RError &error) { return fail("invalid_candidate",error.getMessage()); }
    catch (const std::exception &error) { return fail("invalid_candidate",QString::fromUtf8(error.what())); }
    catch (...) { return fail("invalid_candidate","Native static binding failed."); }
}

QJsonObject RMeshRebindService::capabilities()
{
    const QJsonObject meshProfile=RMeshCandidateService::capabilities();
    return {{"commandId","mesh.rebind.static"},{"version",1},{"unitSI",true},{"staticStress",true},
        {"supportedElements",QJsonArray{"TETRA1","TRI1"}},{"newBindingElement","POINT"},
        {"maximumNodes",double(nodeLimit)},{"maximumElementsIncludingTargets",double(elementLimit)},
        {"meshScope","WF14_convex_single_region"},{"shellGuard","RMeshCandidateService.preview"},
        {"maximumBoundaryNodes",meshProfile.value("inputNodeMaximum")},{"maximumBoundaryTriangles",meshProfile.value("inputTriangleMaximum")},
        {"convexHalfSpacePairMaximum",double(convexHalfSpacePairLimit)},
        {"maximumConstraints",bindingLimit},{"maximumLoads",bindingLimit},
        {"maximumAbsoluteCoordinateM",coordinateLimit},{"maximumAbsoluteDisplacementM",coordinateLimit},
        {"maximumAbsoluteForceN",forceLimit},{"requiredRigidBodyConstraintRank",6},
        {"rankRelativeTolerance",rankTolerance},{"publishes",false},{"solves",false}};
}
