#include <rmesh_candidate_service.h>
#include <rgeometry_candidate_service.h>
#include <rml_mesh_generator.h>
#include <rbl_error.h>
#include <rml_file_io.h>
#include <rml_save_file.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace
{
using V = std::array<double,3>;
using Face = std::array<uint,3>;
using Edge = std::array<uint,2>;
constexpr uint inputNodeLimit = 5002;
constexpr uint triangleLimit = 10000;
constexpr uint outputNodeLimit = 10000;
constexpr uint outputElementLimit = 50000;
constexpr double estimatedTetLimit = 2000;
constexpr quint64 verificationPairLimit = 20000000;
constexpr double relativeTolerance = 1e-8;
constexpr double coordinateLimit = 1e6;
constexpr double backendVolumeSafetyFactor = 0.5;

V xyz(const RNode &n) { return {n.getX(),n.getY(),n.getZ()}; }
V sub(const V &a,const V &b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
V cross(const V &a,const V &b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double dot(const V &a,const V &b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double norm(const V &a) { return std::hypot(a[0],a[1],a[2]); }
double det(const V &a,const V &b,const V &c) { return dot(a,cross(b,c)); }
QJsonArray json(const V &v) { return {v[0],v[1],v[2]}; }
bool cancelled(const std::atomic_bool *flag) { return flag && flag->load(std::memory_order_relaxed); }
Face key(Face f) { std::sort(f.begin(),f.end()); return f; }
int faceOrientation(const Face &face)
{
    uint inversions=0;
    for (uint i=0;i<3;++i) for (uint j=i+1;j<3;++j) if (face[i]>face[j]) ++inversions;
    return inversions%2 ? -1 : 1;
}
Face outwardTetFace(const RElement &tet,uint omit)
{
    // Oriented boundary of a positive tetrahedron: (-1)^omit times the
    // remaining corner order. Cyclic permutations preserve this orientation.
    Face face{}; uint next=0;
    for (uint i=0;i<4;++i) if (i!=omit) face[next++]=tet.getNodeId(i);
    if (omit%2) std::swap(face[1],face[2]);
    return face;
}
struct TetFaceIncidence { uint count=0; Face outward{}; };
QString recordTetFace(std::map<Face,TetFaceIncidence> &faces,const Face &outward)
{
    auto &entry=faces[key(outward)];
    if (++entry.count>2) return "Nonmanifold output tetrahedral face.";
    if (entry.count==1) entry.outward=outward;
    else if (faceOrientation(entry.outward)==faceOrientation(outward))
        return "Interior tetrahedral face incidences have the same orientation; adjacent interiors must lie on opposite sides.";
    return {};
}
QString normalizeGeneratedBoundaryOrientation(RModel &model,const std::atomic_bool *cancel)
{
    if (!model.getNNodes() || model.getNNodes()>outputNodeLimit || !model.getNElements() || model.getNElements()>outputElementLimit)
        return "Output exceeds the fixed node/element budget.";
    std::map<Face,TetFaceIncidence> faces;
    for (uint i=0;i<model.getNElements();++i)
    {
        if (cancelled(cancel)) return "cancelled";
        const auto &element=model.getElement(i);
        if (element.getType()!=R_ELEMENT_TETRA1) continue;
        if (element.size()!=4) return "Invalid generated tetrahedral connectivity.";
        V points[4];
        for (uint j=0;j<4;++j)
        {
            if (element.getNodeId(j)>=model.getNNodes()) return "Invalid generated tetrahedral connectivity.";
            points[j]=xyz(model.getNode(element.getNodeId(j)));
        }
        const double jacobian=det(sub(points[1],points[0]),sub(points[2],points[0]),sub(points[3],points[0]));
        if (!std::isfinite(jacobian) || jacobian<=0)
            return "Generated tetrahedral jacobian must be positive; boundary normalization never repairs tetrahedra.";
        for (uint omit=0;omit<4;++omit)
        {
            const auto error=recordTetFace(faces,outwardTetFace(element,omit));
            if (!error.isEmpty()) return error;
        }
    }
    for (uint i=0;i<model.getNElements();++i)
    {
        if (cancelled(cancel)) return "cancelled";
        auto &element=model.getElement(i);
        if (element.getType()!=R_ELEMENT_TRI1) continue;
        if (element.size()!=3) return "Invalid generated boundary connectivity.";
        const Face face{element.getNodeId(0),element.getNodeId(1),element.getNodeId(2)};
        const auto found=faces.find(key(face));
        if (found==faces.end() || found->second.count!=1)
            return "Generated boundary triangle needs one uniquely adjacent tetrahedron.";
        // Existing Range/TetGen export may swap triangle normals. Normalize only
        // the generator result's corner order from exact adjacency, preserving
        // coordinates, stable IDs, labels and group membership. Verify never does this.
        if (faceOrientation(face)!=faceOrientation(found->second.outward))
        {
            const uint node=element.getNodeId(1); element.setNodeId(1,element.getNodeId(2)); element.setNodeId(2,node);
        }
    }
    return {};
}
QString hash(const QJsonObject &o)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
QJsonArray invalidations()
{
    return {"control","sensors","actuators","constraints","couplings","prescribed_motion",
            "boundary_conditions","environment_conditions","initial_conditions","restart",
            "results","modal","ROM","monitoring_points","derived_fields","problem_setup"};
}
struct Shell
{
    V low{}, high{}, centroid{};
    double scale = 0, volume = 0;
    std::vector<Face> faces;
    std::vector<V> normals;
    std::vector<double> areas;
    std::vector<uint> groups;
};
bool emptyMaterial(const RMaterial &m)
{
    return m.getState() == RMaterial::None && m.size() == 0 && m.getName().isEmpty();
}
bool sameMaterial(const RMaterial &a,const RMaterial &b)
{
    return (emptyMaterial(a) && emptyMaterial(b)) || a.toJson() == b.toJson();
}
bool coplanarInteriorsOverlap(const V (&a)[3],const V (&b)[3],const V &normal,double scale)
{
    // Separating-axis test in a coordinate plane: touching edges/vertices are permitted.
    int drop=0; for (int j=1;j<3;++j) if (std::abs(normal[j])>std::abs(normal[drop])) drop=j;
    const int x=(drop+1)%3,y=(drop+2)%3;
    for (int side=0;side<2;++side)
    {
        const V *t=side==0 ? a : b;
        for (int edge=0;edge<3;++edge)
        {
            const double nx=-(t[(edge+1)%3][y]-t[edge][y]),ny=t[(edge+1)%3][x]-t[edge][x];
            const auto projection=[&](const V &p) { return nx*(p[x]-a[0][x])+ny*(p[y]-a[0][y]); };
            double al=projection(a[0]),ah=al,bl=projection(b[0]),bh=bl;
            for (int j=1;j<3;++j) { const double av=projection(a[j]),bv=projection(b[j]); al=std::min(al,av); ah=std::max(ah,av); bl=std::min(bl,bv); bh=std::max(bh,bv); }
            if (std::min(ah,bh)-std::max(al,bl)<=1e-12*scale*std::hypot(nx,ny)) return false;
        }
    }
    return true;
}
bool validId(uint id,QSet<uint> &seen)
{
    if (!id || id == std::numeric_limits<uint>::max() || seen.contains(id)) return false;
    seen.insert(id); return true;
}
QString validateShell(const RModel &m,Shell &s,const std::atomic_bool *cancel)
{
    if (m.getNNodes()<4 || m.getNNodes()>inputNodeLimit || m.getNElements()<4 || m.getNElements()>triangleLimit)
        return "Input requires 4..5002 nodes and 4..10000 triangles.";
    if (!m.getNSurfaces() || m.getNPoints() || m.getNLines() || m.getNVolumes())
        return "Only labelled outer surface groups are supported; no points, lines or existing volumes.";
    QSet<uint> nodeIds, elementIds, groupIds;
    std::set<V> coordinates;
    s.low = s.high = xyz(m.getNode(0));
    for (uint i=0;i<m.getNNodes();++i)
    {
        if (!validId(m.getNodePhysicalId(i),nodeIds)) return "Invalid or duplicate physical node ID.";
        const V p=xyz(m.getNode(i));
        for (int j=0;j<3;++j)
        {
            if (!std::isfinite(p[j]) || std::abs(p[j])>coordinateLimit) return "Coordinates must be finite and within +/-1000000 m.";
            s.low[j]=std::min(s.low[j],p[j]); s.high[j]=std::max(s.high[j],p[j]);
        }
        if (!coordinates.insert(p).second) return "Coincident source vertices are unsupported.";
    }
    s.scale=norm(sub(s.high,s.low));
    if (!(s.scale>1e-9)) return "Shell length scale must exceed 1e-9 m.";
    s.groups.assign(m.getNElements(),std::numeric_limits<uint>::max());
    QSet<QString> names;
    for (uint g=0;g<m.getNSurfaces();++g)
    {
        const RSurface &surface=m.getSurface(g);
        if (!validId(surface.getPhysicalId(),groupIds) || surface.empty() || surface.getName().trimmed().isEmpty()
            || names.contains(surface.getName())) return "Surface labels require unique stable IDs, nonempty unique names and membership.";
        names.insert(surface.getName());
        if (!sameMaterial(surface.getMaterial(),m.getSurface(0).getMaterial()))
            return "Single region requires one identical material across every surface; heterogeneous region inference is unsupported.";
        for (uint j=0;j<surface.size();++j)
        {
            const uint e=surface.get(j);
            if (e>=m.getNElements() || s.groups[e]!=std::numeric_limits<uint>::max()) return "Every triangle must have exactly one surface label.";
            s.groups[e]=g;
        }
    }
    struct Incidence { uint face; int direction; };
    std::map<Edge,std::vector<Incidence>> edges;
    std::set<Face> unique;
    std::vector<bool> used(m.getNNodes(),false);
    V moment{};
    const V origin=s.low;
    for (uint i=0;i<m.getNElements();++i)
    {
        if (cancelled(cancel)) return "cancelled";
        const RElement &e=m.getElement(i);
        if (!validId(m.getElementPhysicalId(i),elementIds) || e.getType()!=R_ELEMENT_TRI1 || e.size()!=3)
            return "Only linear TRI1 with unique stable element IDs is supported.";
        Face f{};
        for (uint j=0;j<3;++j)
        {
            f[j]=e.getNodeId(j);
            if (f[j]>=m.getNNodes()) return "Triangle references an absent node.";
            used[f[j]]=true;
        }
        if (f[0]==f[1] || f[1]==f[2] || f[0]==f[2] || !unique.insert(key(f)).second)
            return "Repeated vertices or duplicate triangles are invalid.";
        if (s.groups[i]==std::numeric_limits<uint>::max()) return "Unlabelled input facet.";
        const V a=xyz(m.getNode(f[0])), b=xyz(m.getNode(f[1])), c=xyz(m.getNode(f[2]));
        const V n=cross(sub(b,a),sub(c,a));
        const double twiceArea=norm(n);
        if (!std::isfinite(twiceArea) || twiceArea<=1e-14*s.scale*s.scale) return "Degenerate or numerically unresolved triangle.";
        s.faces.push_back(f); s.normals.push_back(n); s.areas.push_back(twiceArea/2);
        const double volume=det(sub(a,origin),sub(b,origin),sub(c,origin))/6;
        s.volume+=volume;
        for (int j=0;j<3;++j) moment[j]+=volume*((a[j]-origin[j])+(b[j]-origin[j])+(c[j]-origin[j]))/4;
        for (uint j=0;j<3;++j)
        {
            const uint u=f[j],v=f[(j+1)%3];
            edges[{std::min(u,v),std::max(u,v)}].push_back({i,u<v ? 1 : -1});
        }
    }
    if (std::find(used.begin(),used.end(),false)!=used.end()) return "Isolated input nodes are unsupported.";
    std::vector<std::vector<uint>> adjacency(s.faces.size());
    for (const auto &entry:edges)
    {
        const auto &v=entry.second;
        if (v.size()!=2 || v[0].direction==v[1].direction) return "Shell edges need exactly two oppositely oriented incidences; holes/nonmanifold/orientation errors are invalid.";
        adjacency[v[0].face].push_back(v[1].face); adjacency[v[1].face].push_back(v[0].face);
    }
    std::vector<uint> queue{0}; std::vector<bool> visited(s.faces.size(),false); visited[0]=true;
    for (size_t i=0;i<queue.size();++i) for (uint j:adjacency[queue[i]]) if (!visited[j]) { visited[j]=true; queue.push_back(j); }
    if (queue.size()!=s.faces.size() || qint64(m.getNNodes())-qint64(edges.size())+qint64(s.faces.size())!=2)
        return "Only one connected genus-zero outer shell is supported.";
    if (!std::isfinite(s.volume) || s.volume<=1e-14*s.scale*s.scale*s.scale) return "Outward shell must enclose finite positive resolved volume.";
    for (int j=0;j<3;++j) s.centroid[j]=origin[j]+moment[j]/s.volume;
    // Independent conservative scope guard: every vertex is in every inward half-space.
    // This excludes concave shells and avoids claiming general self-intersection detection.
    for (size_t i=0;i<s.faces.size();++i)
    {
        if (cancelled(cancel)) return "cancelled";
        const V a=xyz(m.getNode(s.faces[i][0]));
        for (uint j=0;j<m.getNNodes();++j)
            if (dot(s.normals[i],sub(xyz(m.getNode(j)),a))>1e-10*s.scale*norm(s.normals[i]))
                return "First scope requires a convex outward shell; concave/self-intersecting surfaces are unsupported.";
    }
    // Convex supporting planes exclude transverse interior intersections. Explicitly
    // reject overlapping coplanar facets, which edge incidence alone cannot detect.
    for (uint i=0;i<s.faces.size();++i)
    {
        if (cancelled(cancel)) return "cancelled";
        const Face &f=s.faces[i];
        const V a[3]={xyz(m.getNode(f[0])),xyz(m.getNode(f[1])),xyz(m.getNode(f[2]))};
        for (uint j=i+1;j<s.faces.size();++j)
        {
            const Face &g=s.faces[j];
            uint shared=0; for (uint u:f) for (uint v:g) if (u==v) ++shared;
            if (shared>=2) continue; // Opposite incidence and convex outward normals already separate these.
            const V b[3]={xyz(m.getNode(g[0])),xyz(m.getNode(g[1])),xyz(m.getNode(g[2]))};
            bool coplanar=true;
            for (const V &p:b) if (std::abs(dot(s.normals[i],sub(p,a[0])))>1e-12*s.scale*norm(s.normals[i])) { coplanar=false; break; }
            if (coplanar && coplanarInteriorsOverlap(a,b,s.normals[i],s.scale)) return "Overlapping coplanar source facets are unsupported.";
        }
    }
    // Check each vertex link is one cycle, excluding pinched vertices even when edges pass.
    for (uint vertex=0;vertex<m.getNNodes();++vertex)
    {
        if (cancelled(cancel)) return "cancelled";
        std::map<uint,std::vector<uint>> link;
        for (const Face &f:s.faces)
            for (uint j=0;j<3;++j) if (f[j]==vertex) { link[f[(j+1)%3]].push_back(f[(j+2)%3]); link[f[(j+2)%3]].push_back(f[(j+1)%3]); }
        for (const auto &p:link) if (p.second.size()!=2) return "Nonmanifold vertex link.";
        std::set<uint> reached; std::vector<uint> todo{link.begin()->first};
        for (size_t i=0;i<todo.size();++i) if (reached.insert(todo[i]).second) for (uint n:link.at(todo[i])) if (!reached.contains(n)) todo.push_back(n);
        if (reached.size()!=link.size()) return "Disconnected vertex link.";
    }
    return {};
}
double facetIntersectionArea(const V (&out)[3],const V (&in)[3],double scale)
{
    const V n=cross(sub(in[1],in[0]),sub(in[2],in[0]));
    for (const V &p:out) if (std::abs(dot(n,sub(p,in[0])))>1e-9*scale*norm(n)) return 0;
    int drop=0; for (int j=1;j<3;++j) if (std::abs(n[j])>std::abs(n[drop])) drop=j;
    const int x=(drop+1)%3,y=(drop+2)%3;
    using P=std::array<double,2>;
    const auto project=[&](const V &p)->P { return {p[x]-in[0][x],p[y]-in[0][y]}; };
    const P clip[3]={project(in[0]),project(in[1]),project(in[2])};
    const double orientation=n[drop]>0 ? 1 : -1;
    std::vector<P> polygon{project(out[0]),project(out[1]),project(out[2])};
    // Independent convex polygon clipping computes physical coverage despite TetGen
    // legitimately merging coplanar input triangles and changing their diagonals.
    for (int edge=0;edge<3 && !polygon.empty();++edge)
    {
        const P a=clip[edge],b=clip[(edge+1)%3];
        const auto distance=[&](const P &p) { return orientation*((b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])); };
        std::vector<P> next;
        P previous=polygon.back(); double before=distance(previous);
        for (const P &current:polygon)
        {
            const double after=distance(current);
            if ((before>=0)!=(after>=0))
            {
                const double fraction=before/(before-after);
                next.push_back({previous[0]+fraction*(current[0]-previous[0]),previous[1]+fraction*(current[1]-previous[1])});
            }
            if (after>=0) next.push_back(current);
            previous=current; before=after;
        }
        polygon=std::move(next);
    }
    if (polygon.size()<3) return 0;
    double twiceArea=0;
    for (size_t i=1;i+1<polygon.size();++i)
        twiceArea+=(polygon[i][0]-polygon[0][0])*(polygon[i+1][1]-polygon[0][1])
                  -(polygon[i][1]-polygon[0][1])*(polygon[i+1][0]-polygon[0][0]);
    return std::abs(twiceArea)*norm(n)/(2*std::abs(n[drop]));
}
QString validateOutput(const RModel &source,const Shell &s,const RModel &m,double maximum,QJsonObject &report,const std::atomic_bool *cancel)
{
    if (!m.getNNodes() || m.getNNodes()>outputNodeLimit || !m.getNElements() || m.getNElements()>outputElementLimit)
        return "Output exceeds the fixed node/element budget.";
    if (m.getNPoints() || m.getNLines() || m.getNVolumes()!=1 || m.getNSurfaces()!=source.getNSurfaces())
        return "Output does not preserve the single region / surface marker contract.";
    if (quint64(m.getNNodes()+m.getNElements(R_ELEMENT_TRI1))*s.faces.size()>verificationPairLimit)
        return "Node/facet verification exceeds the fixed resource budget.";
    for (uint i=0;i<m.getNNodes();++i)
    {
        const V p=xyz(m.getNode(i));
        for (int j=0;j<3;++j)
            if (!std::isfinite(p[j]) || p[j]<s.low[j]-1e-9*s.scale || p[j]>s.high[j]+1e-9*s.scale)
                return "Output node is nonfinite or outside source bounds.";
        for (size_t f=0;f<s.faces.size();++f)
            if (dot(s.normals[f],sub(p,xyz(source.getNode(s.faces[f][0]))))>1e-9*s.scale*norm(s.normals[f]))
                return "Output node lies outside the convex source shell.";
        if (cancelled(cancel)) return "cancelled";
    }
    std::map<Face,TetFaceIncidence> tetFaces;
    std::map<Face,Face> boundary;
    std::set<std::array<uint,4>> uniqueTets;
    double volume=0, minimumDet=std::numeric_limits<double>::infinity(), maximumVolume=0, maximumRadiusEdgeRatio=0;
    V moment{};
    uint nTets=0,nTriangles=0;
    std::vector<bool> usedNodes(m.getNNodes(),false);
    for (uint i=0;i<m.getNElements();++i)
    {
        if (cancelled(cancel)) return "cancelled";
        const RElement &e=m.getElement(i);
        if ((e.getType()!=R_ELEMENT_TRI1 && e.getType()!=R_ELEMENT_TETRA1)
            || e.size()!=(e.getType()==R_ELEMENT_TRI1 ? 3u : 4u)) return "Output contains unsupported element types.";
        for (uint j=0;j<e.size();++j)
        {
            if (e.getNodeId(j)>=m.getNNodes()) return "Invalid output connectivity.";
            usedNodes[e.getNodeId(j)]=true;
            for (uint k=0;k<j;++k) if (e.getNodeId(j)==e.getNodeId(k)) return "Repeated output element vertex.";
        }
        if (e.getType()==R_ELEMENT_TRI1)
        {
            const Face face{e.getNodeId(0),e.getNodeId(1),e.getNodeId(2)};
            if (!boundary.emplace(key(face),face).second) return "Duplicate output surface triangle.";
            ++nTriangles; continue;
        }
        ++nTets;
        std::array<uint,4> ids{e.getNodeId(0),e.getNodeId(1),e.getNodeId(2),e.getNodeId(3)};
        std::sort(ids.begin(),ids.end());
        if (!uniqueTets.insert(ids).second) return "Duplicate output tetrahedron.";
        V p[4]; for (uint j=0;j<4;++j) p[j]=xyz(m.getNode(e.getNodeId(j)));
        const double jacobian=det(sub(p[1],p[0]),sub(p[2],p[0]),sub(p[3],p[0]));
        const auto numericalFailure=[&](const QString &kind) {
            QJsonArray nodeIds,coordinates;
            for (uint j=0;j<4;++j) { nodeIds.append(double(m.getNodePhysicalId(e.getNodeId(j)))); coordinates.append(json(p[j])); }
            report.insert("failedElement",QJsonObject{{"kind",kind},{"elementPosition",double(i)},
                {"physicalElementId",double(m.getElementPhysicalId(i))},{"nodePhysicalIds",nodeIds},{"coordinates",coordinates},
                {"detJac",jacobian},{"volume",jacobian/6},{"requestedMaxVolume",maximum},{"relativeTolerance",relativeTolerance}});
            return QString("%1: elementPosition=%2 physicalElementId=%3 detJac=%4 volume=%5 maxVolume=%6.")
                .arg(kind).arg(i).arg(m.getElementPhysicalId(i)).arg(jacobian,0,'g',17).arg(jacobian/6,0,'g',17).arg(maximum,0,'g',17);
        };
        // TetGen exports its native corner order. Require positivity; never silently repair it.
        if (!std::isfinite(jacobian) || jacobian<=0) return numericalFailure("nonpositive_or_nonfinite_tetrahedral_jacobian");
        const double v=jacobian/6;
        if (!std::isfinite(v) || v<=0 || v>maximum*(1+relativeTolerance)) return numericalFailure("max_volume_constraint_violation");
        minimumDet=std::min(minimumDet,jacobian); maximumVolume=std::max(maximumVolume,v); volume+=v;
        const V u=sub(p[1],p[0]),w=sub(p[2],p[0]),t=sub(p[3],p[0]);
        const V wt=cross(w,t),tu=cross(t,u),uw=cross(u,w);
        V circumcenter{};
        for (int j=0;j<3;++j) circumcenter[j]=(dot(u,u)*wt[j]+dot(w,w)*tu[j]+dot(t,t)*uw[j])/(2*jacobian);
        double shortest=std::numeric_limits<double>::infinity();
        for (int j=0;j<4;++j) for (int k=j+1;k<4;++k) shortest=std::min(shortest,norm(sub(p[j],p[k])));
        const double quality=norm(circumcenter)/shortest;
        if (!std::isfinite(quality) || shortest<=0) return "Unresolved tetrahedral circumradius/edge quality.";
        maximumRadiusEdgeRatio=std::max(maximumRadiusEdgeRatio,quality);
        for (int j=0;j<3;++j) moment[j]+=v*((p[0][j]-s.low[j])+(p[1][j]-s.low[j])+(p[2][j]-s.low[j])+(p[3][j]-s.low[j]))/4;
        for (uint omit=0;omit<4;++omit)
        {
            const auto error=recordTetFace(tetFaces,outwardTetFace(e,omit));
            if (!error.isEmpty()) return error;
        }
    }
    if (!nTets || std::find(usedNodes.begin(),usedNodes.end(),false)!=usedNodes.end()) return "Output contains no volume or unused nodes.";
    if (!std::isfinite(volume) || std::abs(volume-s.volume)>relativeTolerance*s.volume)
        return "Tetrahedral volume does not match the independently integrated oriented shell.";
    V centroid{};
    for (int j=0;j<3;++j)
    {
        centroid[j]=s.low[j]+moment[j]/volume;
        if (!std::isfinite(centroid[j]) || std::abs(centroid[j]-s.centroid[j])>relativeTolerance*s.scale)
            return "Volume centroid does not match the source shell.";
    }
    uint boundaryFaces=0;
    for (const auto &entry:tetFaces)
        if (entry.second.count==1)
        {
            ++boundaryFaces;
            const auto triangle=boundary.find(entry.first);
            if (triangle==boundary.end()) return "Missing physical boundary facet.";
            if (faceOrientation(triangle->second)!=faceOrientation(entry.second.outward))
                return "Boundary triangle orientation is inward relative to its uniquely adjacent positive tetrahedron.";
        }
        else if (boundary.contains(entry.first)) return "Interior face is labelled as boundary.";
    if (boundaryFaces!=boundary.size()) return "Output surface has facets absent from the tetrahedral boundary.";
    if (quint64(nTriangles)*s.faces.size()>verificationPairLimit) return "Facet verification exceeds the fixed resource budget.";
    std::vector<double> covered(s.faces.size(),0);
    std::vector<bool> labelled(m.getNElements(),false);
    QJsonArray labels;
    for (uint g=0;g<m.getNSurfaces();++g)
    {
        const RSurface &out=m.getSurface(g), &in=source.getSurface(g);
        if (out.getPhysicalId()!=in.getPhysicalId() || out.getName()!=in.getName() || !sameMaterial(out.getMaterial(),in.getMaterial()) || out.empty())
            return "Surface marker identity/name/material was lost.";
        for (uint j=0;j<out.size();++j)
        {
            if (cancelled(cancel)) return "cancelled";
            const uint e=out.get(j);
            if (e>=m.getNElements() || labelled[e] || m.getElement(e).getType()!=R_ELEMENT_TRI1) return "Invalid or ambiguous output surface label membership.";
            labelled[e]=true;
            const RElement &t=m.getElement(e);
            const V a=xyz(m.getNode(t.getNodeId(0))),b=xyz(m.getNode(t.getNodeId(1))),c=xyz(m.getNode(t.getNodeId(2)));
            const double area=norm(cross(sub(b,a),sub(c,a)))/2;
            if (!std::isfinite(area) || area<=0) return "Degenerate output boundary facet.";
            double mappedArea=0;
            const V output[3]={a,b,c};
            for (uint f=0;f<s.faces.size();++f)
            {
                if (s.groups[f]!=g) continue;
                const Face &sf=s.faces[f];
                const V input[3]={xyz(source.getNode(sf[0])),xyz(source.getNode(sf[1])),xyz(source.getNode(sf[2]))};
                const double intersection=facetIntersectionArea(output,input,s.scale);
                if (!std::isfinite(intersection)) return "Nonfinite physical facet coverage.";
                covered[f]+=intersection; mappedArea+=intersection;
            }
            if (std::abs(mappedArea-area)>relativeTolerance*area) return "Output boundary facet lost its physical source marker coverage.";
        }
        labels.append(QJsonObject{{"physicalGroupId",double(in.getPhysicalId())},{"name",in.getName()},
                                  {"sourceFacets",double(in.size())},{"candidateFacets",double(out.size())}});
    }
    const RVolume &region=m.getVolume(0);
    if (region.size()!=nTets) return "Single volume region membership is incomplete.";
    for (uint j=0;j<region.size();++j)
    {
        const uint e=region.get(j);
        if (e>=m.getNElements() || labelled[e] || m.getElement(e).getType()!=R_ELEMENT_TETRA1) return "Invalid/duplicate volume region membership.";
        labelled[e]=true;
    }
    if (std::find(labelled.begin(),labelled.end(),false)!=labelled.end()) return "Unlabelled output element.";
    for (uint f=0;f<covered.size();++f)
        if (std::abs(covered[f]-s.areas[f])>relativeTolerance*s.areas[f]) return "Boundary subdivision area does not cover every source physical facet exactly.";
    report.insert("surfaceLabels",labels); report.insert("boundaryFacetCoverageVerified",true);
    report.insert("outwardBoundaryOrientationVerified",true); report.insert("oppositeInteriorFaceOrientationVerified",true);
    report.insert("nodeCount",double(m.getNNodes())); report.insert("elementCount",double(m.getNElements()));
    report.insert("tetrahedronCount",double(nTets)); report.insert("boundaryTriangleCount",double(nTriangles));
    report.insert("minimumDetJac",minimumDet); report.insert("maximumTetVolume",maximumVolume);
    report.insert("maximumRadiusEdgeRatio",maximumRadiusEdgeRatio);
    report.insert("totalVolume",volume); report.insert("centroid",json(centroid));
    return {};
}
void clearMappings(RModel &m)
{
    m.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_NONE));
    m.setProblemSetup(RProblemSetup());
    m.setTimeSolver(RTimeSolver());
    m.setMonitoringPointManager(RMonitoringPointManager());
    m.clearResults(); m.clearBoundaryConditions(); m.clearInitialConditions(); m.clearEntityVariableData();
    // clearEnvironmentConditions() reinstalls gravity/temperature constructor defaults.
    // Detached candidates require genuinely unbound environment state, so remove each
    // condition explicitly rather than retain a new load case under the old mesh.
    for (uint i=0;i<m.getNElementGroups();++i)
    {
        RElementGroup *group=m.getElementGroupPtr(i);
        while (group->getNEnvironmentConditions()) group->removeEnvironmentCondition(0u);
    }
    m.setNVectorFields(0); m.setNScalarFields(0); m.setNStreamLines(0); m.setNCuts(0); m.setNIsos(0);
}
template<class T> QByteArray nativeBytes(const T &value)
{
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("Cannot stage binding verification.");
    const QString path=directory.filePath("value.bin");
    RSaveFile output(path,RSaveFile::BINARY);
    output.setVersion(RModel::version);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write binding verification.");
    RFileIO::writeBinary(output,value);
    if (!output.commit()) throw std::runtime_error("Cannot commit binding verification.");
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read binding verification.");
    return input.readAll();
}
RMeshCandidateService::MeshCandidate failure(RMeshCandidateService::MeshCandidate result,const QString &code,const QString &message)
{
    result.valid=false; result.candidate=RModel(); result.postGeometrySha256.clear(); result.code=code; result.message=message;
    return result;
}
RMeshCandidateService::MeshCandidate prepare(const RModel &source,const QJsonObject &a,Shell &s,const std::atomic_bool *cancel)
{
    RMeshCandidateService::MeshCandidate result;
    if (cancelled(cancel)) return failure(result,"cancelled","Mesh candidate cancelled before generation.");
    const QStringList fields{"unit","volumeUnit","maxVolume","radiusEdgeRatio"};
    if (a.size()!=fields.size()) return failure(result,"invalid_arguments","Missing or unknown mesh fields; arbitrary algorithms/TetGen strings are forbidden.");
    for (const QString &f:fields) if (!a.contains(f)) return failure(result,"invalid_arguments","Missing mesh field.");
    double volume=a.value("maxVolume").toDouble(-1), ratio=a.value("radiusEdgeRatio").toDouble(-1);
    if (a.value("unit")!=QJsonValue(QStringLiteral("m")) || a.value("volumeUnit")!=QJsonValue(QStringLiteral("m^3"))
        || !a.value("maxVolume").isDouble() || !std::isfinite(volume) || volume<1e-15 || volume>1e18
        || !a.value("radiusEdgeRatio").isDouble() || !std::isfinite(ratio) || ratio<1.2 || ratio>2.0)
        return failure(result,"invalid_arguments","Require m, m^3, finite maxVolume [1e-15,1e18] and radiusEdgeRatio [1.2,2.0].");
    // The existing generator serializes numbers with QString::number's six significant
    // digits. Bind/validate those effective values, rather than promise unpassed precision.
    volume=QString::number(volume,'g',6).toDouble();
    ratio=QString::number(ratio,'g',6).toDouble();
    // Final TetGen smoothing/improvement can enlarge refined tetrahedra. Use a
    // fixed tighter backend target, then retain the independent requested-volume
    // acceptance gate. This is one generator call, never an adaptive retry.
    const double backendVolume=QString::number(volume*backendVolumeSafetyFactor,'g',6).toDouble();
    const QString error=validateShell(source,s,cancel);
    if (!error.isEmpty()) return failure(result,error=="cancelled" ? "cancelled" : "invalid_source_surface",error);
    const double boxVolume=(s.high[0]-s.low[0])*(s.high[1]-s.low[1])*(s.high[2]-s.low[2]);
    if (!std::isfinite(boxVolume) || boxVolume/backendVolume>estimatedTetLimit)
        return failure(result,"resource_limit","Bounding-box volume / backendMaxVolume exceeds 2000 estimated tetrahedra; use a coarser request.");
    uint maxNode=0,maxElement=0;
    for (uint i=0;i<source.getNNodes();++i) maxNode=std::max(maxNode,source.getNodePhysicalId(i));
    for (uint i=0;i<source.getNElements();++i) maxElement=std::max(maxElement,source.getElementPhysicalId(i));
    if (quint64(maxNode)+2*outputNodeLimit>=std::numeric_limits<uint>::max()
        || quint64(maxElement)+2*outputElementLimit>=std::numeric_limits<uint>::max())
        return failure(result,"physical_id_exhausted","Not enough stable physical ID headroom for a bounded fresh candidate.");
    result.normalizedArguments={{"unit","m"},{"volumeUnit","m^3"},{"maxVolume",volume},{"radiusEdgeRatio",ratio}};
    result.sourceGeometrySha256=RGeometryCandidateService::geometryFingerprint(source);
    if (result.sourceGeometrySha256.isEmpty()) return failure(result,"invalid_source_surface","Cannot fingerprint source stable identities.");
    result.report={{"sourceGeometrySha256",result.sourceGeometrySha256},{"normalizedArgumentsSha256",hash(result.normalizedArguments)},
                   {"sourceVolume",s.volume},{"sourceCentroid",json(s.centroid)},
                   {"bounds",QJsonObject{{"minimum",json(s.low)},{"maximum",json(s.high)}}},
                   {"estimatedTetrahedra",boxVolume/backendVolume},{"invalidatedBindingKinds",invalidations()},
                   {"requestedMaxVolume",volume},{"backendMaxVolume",backendVolume},
                   {"backendVolumeSafetyFactor",backendVolumeSafetyFactor},
                   {"rebindingRequired",true},{"crossMeshMappingAvailable",false},
                   {"backend","existing_RMeshGenerator_TetGen"},{"regionPolicy","single_region_uniform_surface_material"},
                   {"bindingState","REQUIRES_REBINDING"},{"numericParameterSignificantDigits",6},
                   {"physicsState","UNBOUND"},{"noSolverTaskConfigured",true},
                   {"generatorBoundaryOrientationNormalization","unique_adjacent_positive_tetrahedron_corner_order_only"},
                   {"artifactBoundaryOrientationPolicy","outward_from_unique_adjacent_positive_tetrahedron"},
                   {"boundaryOrientationNormalizationAllowedDuringVerification",false},
                   {"tetGenInterruptPolicy","isolated_worker_process_termination"}};
    QJsonArray sourceLabels;
    for (uint g=0;g<source.getNSurfaces();++g)
        sourceLabels.append(QJsonObject{{"physicalGroupId",double(source.getSurface(g).getPhysicalId())},
            {"name",source.getSurface(g).getName()},{"material",source.getSurface(g).getMaterial().toJson()}});
    result.report.insert("sourceLabelsSha256",hash(QJsonObject{{"labels",sourceLabels}}));
    result.valid=true; result.code="preview_ready"; return result;
}
}

RMeshCandidateService::MeshCandidate RMeshCandidateService::preview(const RModel &source,const QJsonObject &arguments)
{
    Shell shell;
    return prepare(source,arguments,shell,nullptr);
}

RMeshCandidateService::MeshCandidate RMeshCandidateService::generate(const RModel &source,const QJsonObject &arguments,const std::atomic_bool *cancel)
{
    Shell shell;
    MeshCandidate result=prepare(source,arguments,shell,cancel);
    if (!result.valid) return result;
    try
    {
        RModel candidate(source);
        clearMappings(candidate);
        RMeshInput input;
        input.setVerbose(false); input.setVolumeMesh(true); input.setQualityMesh(true);
        input.setOutputEdges(false); input.setReconstruct(false); input.setUseSizeFunction(false);
        input.setRadiusEdgeRatio(result.normalizedArguments.value("radiusEdgeRatio").toDouble());
        input.setVolumeConstraint(result.report.value("backendMaxVolume").toDouble());
        input.setTolerance(1e-10); input.setSurfaceIntegrityCheck(true); input.setKeepResults(false); input.setUseTetGenInputParams(false);
        result.report.insert("backendParameters",candidate.generateMeshTetGenInputParams(input));
        result.report.insert("backendParametersSha256",hash(QJsonObject{{"parameters",result.report.value("backendParameters")}}));
        if (cancelled(cancel)) return failure(result,"cancelled","Mesh candidate cancelled before TetGen.");
        RMeshGenerator::generate(input,candidate);
        if (cancelled(cancel)) return failure(result,"cancelled","Mesh candidate cancelled after TetGen; output discarded.");
        const QString orientationError=normalizeGeneratedBoundaryOrientation(candidate,cancel);
        if (!orientationError.isEmpty()) return failure(result,orientationError=="cancelled" ? "cancelled" : "invalid_candidate_mesh",orientationError);
        const QString error=validateOutput(source,shell,candidate,result.normalizedArguments.value("maxVolume").toDouble(),result.report,cancel);
        if (!error.isEmpty()) return failure(result,error=="cancelled" ? "cancelled" : "invalid_candidate_mesh",error);
        // Include generated IDs when choosing a fresh range: resizing preserves old slots
        // and allocates new ones, so merely assigning above the source maximum can collide.
        uint maxNode=0,maxElement=0;
        for (uint i=0;i<source.getNNodes();++i) maxNode=std::max(maxNode,source.getNodePhysicalId(i));
        for (uint i=0;i<source.getNElements();++i) maxElement=std::max(maxElement,source.getElementPhysicalId(i));
        for (uint i=0;i<candidate.getNNodes();++i) maxNode=std::max(maxNode,candidate.getNodePhysicalId(i));
        for (uint i=0;i<candidate.getNElements();++i) maxElement=std::max(maxElement,candidate.getElementPhysicalId(i));
        if (quint64(maxNode)+candidate.getNNodes()>=std::numeric_limits<uint>::max()
            || quint64(maxElement)+candidate.getNElements()>=std::numeric_limits<uint>::max())
            return failure(result,"physical_id_exhausted","Fresh physical ID allocation would overflow.");
        for (uint i=0;i<candidate.getNNodes();++i) candidate.setNodePhysicalId(i,maxNode+i+1);
        for (uint i=0;i<candidate.getNElements();++i) candidate.setElementPhysicalId(i,maxElement+i+1);
        candidate.getVolume(0).setName("WF14 single region");
        candidate.getVolume(0).setMaterial(source.getSurface(0).getMaterial());
        clearMappings(candidate);
        if (cancelled(cancel)) return failure(result,"cancelled","Mesh candidate cancelled; output discarded.");
        result.postGeometrySha256=RGeometryCandidateService::geometryFingerprint(candidate);
        if (result.postGeometrySha256.isEmpty()) return failure(result,"invalid_candidate_mesh","Candidate stable identity validation failed.");
        result.report.insert("freshPhysicalNodeIds",true); result.report.insert("freshPhysicalElementIds",true);
        result.report.insert("postGeometrySha256",result.postGeometrySha256);
        result.report.insert("region",QJsonObject{{"name",candidate.getVolume(0).getName()},
            {"physicalGroupId",double(candidate.getVolume(0).getPhysicalId())},{"material",candidate.getVolume(0).getMaterial().toJson()}});
        result.report.insert("qualityVolumeSha256",hash(QJsonObject{{"minimumDetJac",result.report.value("minimumDetJac")},
            {"maximumTetVolume",result.report.value("maximumTetVolume")},{"totalVolume",result.report.value("totalVolume")},
            {"maximumRadiusEdgeRatio",result.report.value("maximumRadiusEdgeRatio")},
            {"centroid",result.report.value("centroid")},{"postGeometrySha256",result.postGeometrySha256}}));
        result.report.insert("reportSha256",hash(result.report));
        result.candidate=candidate; result.valid=true; result.code="candidate_ready";
        result.message="Detached candidate validated; acceptance and binding rebuild are required.";
        return result;
    }
    catch (const RError &error) { return failure(result,"mesh_generation_failed",error.getMessage()); }
    catch (const std::exception &error) { return failure(result,"mesh_generation_failed",QString::fromUtf8(error.what())); }
    catch (...) { return failure(result,"mesh_generation_failed","Mesh backend failed without a diagnostic."); }
}

RMeshCandidateService::MeshCandidate RMeshCandidateService::verify(const RModel &source,const RModel &candidate,
                                                                  const QJsonObject &arguments,const std::atomic_bool *cancel)
{
    Shell shell;
    MeshCandidate result=prepare(source,arguments,shell,cancel);
    if (!result.valid) return result;
    try
    {
        const QString error=validateOutput(source,shell,candidate,result.normalizedArguments.value("maxVolume").toDouble(),result.report,cancel);
        if (!error.isEmpty()) return failure(result,error=="cancelled" ? "cancelled" : "invalid_candidate_mesh",error);
        QSet<uint> sourceNodes,sourceElements,seenNodes,seenElements;
        for (uint i=0;i<source.getNNodes();++i) sourceNodes.insert(source.getNodePhysicalId(i));
        for (uint i=0;i<source.getNElements();++i) sourceElements.insert(source.getElementPhysicalId(i));
        // Fingerprint validates the public stable IDs. Probe allocation on a detached
        // copy as the allocator validator itself is private; never resize the input.
        result.postGeometrySha256=RGeometryCandidateService::geometryFingerprint(candidate);
        if (result.postGeometrySha256.isEmpty()) return failure(result,"invalid_candidate_identity","Candidate IDs or allocators are invalid.");
        for (uint i=0;i<candidate.getNNodes();++i)
            if (!validId(candidate.getNodePhysicalId(i),seenNodes) || sourceNodes.contains(candidate.getNodePhysicalId(i)))
                return failure(result,"invalid_candidate_identity","Candidate reused a source node ID.");
        for (uint i=0;i<candidate.getNElements();++i)
            if (!validId(candidate.getElementPhysicalId(i),seenElements) || sourceElements.contains(candidate.getElementPhysicalId(i)))
                return failure(result,"invalid_candidate_identity","Candidate reused a source element ID.");
        uint maxNode=0,maxElement=0,maxGroup=0;
        for (uint id:seenNodes) maxNode=std::max(maxNode,id);
        for (uint id:seenElements) maxElement=std::max(maxElement,id);
        for (uint i=0;i<candidate.getNElementGroups();++i) maxGroup=std::max(maxGroup,candidate.getElementGroupPtr(i)->getPhysicalId());
        RModel allocatorProbe(candidate);
        allocatorProbe.setNNodes(candidate.getNNodes()+1);
        allocatorProbe.setNElements(candidate.getNElements()+1);
        allocatorProbe.setNVolumes(candidate.getNVolumes()+1);
        if (allocatorProbe.getNodePhysicalId(candidate.getNNodes())<=maxNode
            || allocatorProbe.getElementPhysicalId(candidate.getNElements())<=maxElement
            || allocatorProbe.getVolume(candidate.getNVolumes()).getPhysicalId()<=maxGroup)
            return failure(result,"invalid_candidate_identity","Candidate allocators do not advance past existing physical IDs.");
        if (candidate.getVolume(0).getName()!=QStringLiteral("WF14 single region")
            || !sameMaterial(candidate.getVolume(0).getMaterial(),source.getSurface(0).getMaterial()))
            return failure(result,"invalid_candidate_labels","Candidate region name/material differs from the frozen source.");
        if (candidate.getName()!=source.getName() || candidate.getDescription()!=source.getDescription())
            return failure(result,"invalid_candidate_context","Candidate changed source metadata.");
        if (nativeBytes(candidate.getProblemTaskTree())!=nativeBytes(RProblemTaskItem(R_PROBLEM_NONE)))
            return failure(result,"invalid_candidate_bindings","Candidate must have no solver task until explicit rebinding.");
        if (candidate.getNVariables() || candidate.getNVectorFields() || candidate.getNScalarFields()
            || candidate.getNStreamLines() || candidate.getNCuts() || candidate.getNIsos()
            || !candidate.getMonitoringPointManager().empty()
            || nativeBytes(candidate.getProblemSetup())!=nativeBytes(RProblemSetup())
            || nativeBytes(candidate.getTimeSolver())!=nativeBytes(RTimeSolver()))
            return failure(result,"invalid_candidate_bindings","Candidate retains source setup, results or derived bindings.");
        for (uint i=0;i<candidate.getNSurfaces()+candidate.getNVolumes();++i)
        {
            const RElementGroup &group=i<candidate.getNSurfaces()
                ? static_cast<const RElementGroup &>(candidate.getSurface(i))
                : static_cast<const RElementGroup &>(candidate.getVolume(i-candidate.getNSurfaces()));
            if (group.getNBoundaryConditions() || group.getNInitialConditions() || group.getNEnvironmentConditions()
                || !group.getData().getVariableData().empty())
                return failure(result,"invalid_candidate_bindings",
                    QString("Candidate group %1 physicalId=%2 retains boundary=%3 initial=%4 environment=%5 displayBindings=%6.")
                        .arg(i).arg(group.getPhysicalId()).arg(group.getNBoundaryConditions())
                        .arg(group.getNInitialConditions()).arg(group.getNEnvironmentConditions())
                        .arg(qulonglong(group.getData().getVariableData().size())));
        }
        RMeshInput input;
        input.setVerbose(false); input.setVolumeMesh(true); input.setQualityMesh(true);
        input.setOutputEdges(false); input.setReconstruct(false); input.setUseSizeFunction(false);
        input.setRadiusEdgeRatio(result.normalizedArguments.value("radiusEdgeRatio").toDouble());
        input.setVolumeConstraint(result.report.value("backendMaxVolume").toDouble());
        input.setTolerance(1e-10); input.setSurfaceIntegrityCheck(true); input.setKeepResults(false); input.setUseTetGenInputParams(false);
        result.report.insert("backendParameters",candidate.generateMeshTetGenInputParams(input));
        result.report.insert("backendParametersSha256",hash(QJsonObject{{"parameters",result.report.value("backendParameters")}}));
        result.report.insert("freshPhysicalNodeIds",true); result.report.insert("freshPhysicalElementIds",true);
        result.report.insert("postGeometrySha256",result.postGeometrySha256);
        result.report.insert("region",QJsonObject{{"name",candidate.getVolume(0).getName()},
            {"physicalGroupId",double(candidate.getVolume(0).getPhysicalId())},{"material",candidate.getVolume(0).getMaterial().toJson()}});
        result.report.insert("qualityVolumeSha256",hash(QJsonObject{{"minimumDetJac",result.report.value("minimumDetJac")},
            {"maximumTetVolume",result.report.value("maximumTetVolume")},{"totalVolume",result.report.value("totalVolume")},
            {"maximumRadiusEdgeRatio",result.report.value("maximumRadiusEdgeRatio")},
            {"centroid",result.report.value("centroid")},{"postGeometrySha256",result.postGeometrySha256}}));
        result.report.insert("reportSha256",hash(result.report));
        if (cancelled(cancel)) return failure(result,"cancelled","Candidate verification cancelled.");
        result.candidate=candidate; result.code="candidate_ready";
        result.message="Loaded candidate verified; requires explicit acceptance and rebinding.";
        return result;
    }
    catch (const RError &error) { return failure(result,"candidate_verification_failed",error.getMessage()); }
    catch (const std::exception &error) { return failure(result,"candidate_verification_failed",QString::fromUtf8(error.what())); }
    catch (...) { return failure(result,"candidate_verification_failed","Candidate verification failed."); }
}

QJsonObject RMeshCandidateService::capabilities()
{
    return {{"command","mesh.closed_surface.generate"},{"sourceElementTypes",QJsonArray{"TRI1"}},
            {"candidateElementTypes",QJsonArray{"TRI1","TETRA1"}},{"sourceShell","convex_connected_outward_closed_genus_zero"},
            {"unsupportedSourceFeatures",QJsonArray{"concave_shell","self_intersections","multiple_shells","multiple_regions","heterogeneous_materials","existing_volume_mesh"}},
            {"unit","m"},{"volumeUnit","m^3"},{"maxVolumeMinimum",1e-15},{"maxVolumeMaximum",1e18},
            {"radiusEdgeRatioMinimum",1.2},{"radiusEdgeRatioMaximum",2.0},
            {"numericParameterSignificantDigits",6},{"bindingState","REQUIRES_REBINDING"},
            {"physicsState","UNBOUND"},{"candidateSolverTaskConfigured",false},
            {"artifactBoundaryOrientationPolicy","outward_from_unique_adjacent_positive_tetrahedron"},
            {"boundaryOrientationNormalizationAllowedDuringVerification",false},
            {"backendVolumeSafetyFactor",backendVolumeSafetyFactor},
            {"inputNodeMaximum",double(inputNodeLimit)},{"inputTriangleMaximum",double(triangleLimit)},
            {"outputNodeMaximum",double(outputNodeLimit)},{"outputElementMaximum",double(outputElementLimit)},
            {"boundingVolumePerMaxVolumeMaximum",estimatedTetLimit*backendVolumeSafetyFactor},
            {"boundingVolumePerBackendMaxVolumeMaximum",estimatedTetLimit},
            {"resourceEstimateVolumeField","backendMaxVolume"},{"facetVerificationPairMaximum",double(verificationPairLimit)},
            {"preservesPhysicalNodeIds",false},{"preservesPhysicalElementIds",false},
            {"surfaceLabels","marker_and_independent_facet_coverage_verified"},
            {"regionPolicy","single_region_uniform_surface_material"},{"crossMeshMappingAvailable",false},
            {"invalidatedBindingKinds",invalidations()},{"arbitraryTetGenArguments",false},
            {"inProcessTetGenCancellation",false}};
}
