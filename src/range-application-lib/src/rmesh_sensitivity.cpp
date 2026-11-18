#include "rmesh_sensitivity.h"

#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
using Edge = std::pair<uint,uint>;
Edge edge(uint a,uint b) { return std::minmax(a,b); }
RR3Vector position(const RNode &n) { return RR3Vector(n.getX(),n.getY(),n.getZ()); }
bool finite(const RR3Vector &p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }

double cross(const RNode &a,const RNode &b,const RNode &c)
{
    return (b.getX()-a.getX())*(c.getY()-a.getY())-(b.getY()-a.getY())*(c.getX()-a.getX());
}

double lengthScale(const RModel &m)
{
    double xmin=m.getNode(0).getX(),xmax=xmin,ymin=m.getNode(0).getY(),ymax=ymin;
    for (const auto &n:m.getNodes())
    {
        xmin=std::min(xmin,n.getX()); xmax=std::max(xmax,n.getX());
        ymin=std::min(ymin,n.getY()); ymax=std::max(ymax,n.getY());
    }
    return std::max(xmax-xmin,ymax-ymin);
}

bool constant(const RConditionComponent &c)
{
    return c.size()==1 && std::isfinite(c.getKey(0)) && std::isfinite(c.getValue(0));
}

QString conditions(const RElementGroup &group,bool surface)
{
    if (group.getNInitialConditions()) return "Initial conditions are outside the planar static refinement contract.";
    for (uint i=0;i<group.getNBoundaryConditions();++i)
    {
        const auto &bc=group.getBoundaryCondition(i);
        const bool displacement=bc.getType()==R_BOUNDARY_CONDITION_DISPLACEMENT;
        if ((!displacement && (surface || bc.getType()!=R_BOUNDARY_CONDITION_FORCE)) || bc.getHasLocalDirection())
            return "Only global Cartesian displacement constraints and point/line forces are supported.";
        for (uint j=0;j<bc.size();++j)
        {
            const auto &c=bc.getComponent(j);
            const auto t=c.getType();
            if ((displacement && t!=R_VARIABLE_DISPLACEMENT_X && t!=R_VARIABLE_DISPLACEMENT_Y && t!=R_VARIABLE_DISPLACEMENT_Z)
                || (!displacement && t!=R_VARIABLE_FORCE_X && t!=R_VARIABLE_FORCE_Y && t!=R_VARIABLE_FORCE_Z)
                || !constant(c)) return "Boundary components must be finite constant Cartesian values.";
            if (!displacement && c.getEnabled() && t==R_VARIABLE_FORCE_Z && c.getValue(0)!=0)
                return "Out-of-plane forces are unsupported.";
            if (displacement && c.getEnabled() && t==R_VARIABLE_DISPLACEMENT_Z && c.getValue(0)!=0)
                return "Out-of-plane prescribed displacement is unsupported.";
        }
    }
    for (uint i=0;i<group.getNEnvironmentConditions();++i)
    {
        const auto &ec=group.getEnvironmentCondition(i);
        if (ec.getType()!=R_ENVIRONMENT_CONDITION_G_ACCELERATION && ec.getType()!=R_ENVIRONMENT_CONDITION_TEMPERATURE)
            return "Only constant temperature and gravitational acceleration are supported.";
        for (uint j=0;j<ec.size();++j)
        {
            const auto &c=ec.getComponent(j);
            const auto t=c.getType();
            const bool gravity=ec.getType()==R_ENVIRONMENT_CONDITION_G_ACCELERATION;
            if (!constant(c) || (gravity && t!=R_VARIABLE_G_ACCELERATION_X && t!=R_VARIABLE_G_ACCELERATION_Y && t!=R_VARIABLE_G_ACCELERATION_Z)
                || (!gravity && t!=R_VARIABLE_TEMPERATURE)) return "Environment components must be finite constant values.";
        }
    }
    return {};
}

bool property(const RMaterial &material,RMaterialProperty::Type type,double &value)
{
    const uint i=material.findPosition(type);
    if (i>=material.size()) return false;
    const auto &p=material.get(i);
    if (p.size()!=1 || !std::isfinite(p.getValue(0)) || !std::isfinite(p.getKey(0))) return false;
    value=p.getValue(0); return true;
}

bool boundSetup(const RProblemSetup &s)
{
    const auto &i=s.getStructuralInteractionSetup();
    const auto &r=s.getStructuralDynamicsRestartData();
    return s.getRestart() || s.getStructuralDynamicsSetup().getEnabled()
        || !i.getLinearConstraints().empty() || !i.getCouplings().empty() || !i.getSensors().empty()
        || !i.getActuators().empty() || !i.getPrescribedMotions().empty()
        || s.getStructuralControlSetup().enabled || !s.getStructuralControlSetup().controllerId.isEmpty()
        || !s.getStructuralControlSetup().channelConditioning.empty()
        || s.getStructuralCableSetup().enabled || !s.getStructuralCableSetup().members.empty()
        || !s.getStructuralCableSetup().meshFingerprint.isEmpty()
        || s.getStructuralPiezoSetup().enabled || !s.getStructuralPiezoSetup().ports.empty()
        || !s.getStructuralPiezoSetup().passiveElementIds.empty() || !s.getStructuralPiezoSetup().nodalBoundaryConditions.empty()
        || !s.getStructuralPiezoSetup().meshFingerprint.isEmpty()
        || s.getStructuralThermalControlSetup().enabled || !s.getStructuralThermalControlSetup().channels.empty()
        || !s.getStructuralThermalControlSetup().sparseObservationNodeIds.empty()
        || s.getStructuralTrussDynamicsSetup().hasNonDefaultData()
        || s.getStructuralTrussDynamicsRestartData().hasNonDefaultData()
        || s.getStructuralNativeControlSpec().hasNonDefaultData() || s.getStructuralNativeControlState().hasNonDefaultData()
        || s.getStructuralPiezoReadoutSetup().hasNonDefaultData() || s.getStructuralPiezoLayeredSetup().hasNonDefaultData()
        || s.getStructuralPiezoLayeredRestart().available || !s.getStructuralPiezoLayeredRestart().sourceFingerprint.isEmpty()
        || !s.getStructuralPiezoLayeredRestart().stateBytes.isEmpty() || s.getStructuralPiezoAxialSetup().hasNonDefaultData()
        || s.getStructuralPiezoAxialRestart().hasNonDefaultData()
        || r.available || r.meshNodeCount || r.meshElementCount || !r.meshFingerprint.isEmpty()
        || !r.q.empty() || !r.v.empty() || !r.a.empty() || !r.externalForce.empty()
        || r.supportResponseAvailable || r.controlAvailable || !r.supportReactions.empty()
        || !r.controlControllerState.isEmpty() || !s.getMeshSetup().getVariables().empty()
        || !s.getRadiationSetup().getViewFactorMatrixFile().isEmpty();
}

const RVariable &displacements(const RModel &m)
{
    const uint id=m.findVariable(R_VARIABLE_DISPLACEMENT);
    if (id==RConstants::eod) fail("The solved model has no displacement result.");
    const auto &v=m.getVariable(id);
    if (v.getApplyType()!=R_VARIABLE_APPLY_NODE || v.getNVectors()!=3 || v.getNValues()!=m.getNNodes())
        fail("Displacements must contain complete nodal XYZ values.");
    for (uint axis=0;axis<3;++axis)
    {
        if (v[axis].size()!=m.getNNodes()) fail("Displacement component size does not match the mesh.");
        for (uint n=0;n<m.getNNodes();++n)
            if (!std::isfinite(v.getValue(axis,n))) fail("Displacements contain a nonfinite value.");
    }
    return v;
}

struct Sample { std::vector<uint> nodes; std::vector<double> weights; };

bool locate(const RModel &m,const RR3Vector &p,Sample &sample)
{
    if (!finite(p) || !m.getNNodes()) return false;
    const double scale=lengthScale(m);
    const double tolerance=std::max(1e-10*scale,64*std::numeric_limits<double>::epsilon()
        *std::max({1.0,std::abs(p[0]),std::abs(p[1]),std::abs(p[2])}));
    if (std::abs(p[2]-m.getNode(0).getZ())>tolerance) return false;
    // Exact original nodes retain their identity and are never replaced by nearest-node sampling.
    for (uint n=0;n<m.getNNodes();++n)
    {
        const auto &node=m.getNode(n);
        if (p[0]==node.getX() && p[1]==node.getY() && p[2]==node.getZ())
        {
            sample={{n},{1.0}}; return true;
        }
    }
    constexpr double naturalTolerance=1e-9;
    for (uint e=0;e<m.getNElements();++e)
    {
        const auto &el=m.getElement(e);
        if (el.getType()!=R_ELEMENT_TRI1 && el.getType()!=R_ELEMENT_QUAD1) continue;
        std::vector<uint> nodes;
        bool valid=true;
        for (uint j=0;j<el.size();++j)
        {
            const uint n=el.getNodeId(j);
            if (n>=m.getNNodes()) { valid=false; break; }
            nodes.push_back(n);
        }
        if (!valid) continue;
        if (el.getType()==R_ELEMENT_TRI1)
        {
            if (nodes.size()!=3) continue;
            const auto &a=m.getNode(nodes[0]),&b=m.getNode(nodes[1]),&c=m.getNode(nodes[2]);
            const double det=cross(a,b,c);
            if (!std::isfinite(det) || det==0) continue;
            const double u=((p[0]-a.getX())*(c.getY()-a.getY())-(p[1]-a.getY())*(c.getX()-a.getX()))/det;
            const double v=((b.getX()-a.getX())*(p[1]-a.getY())-(b.getY()-a.getY())*(p[0]-a.getX()))/det;
            if (u>=-naturalTolerance && v>=-naturalTolerance && u+v<=1+naturalTolerance)
            {
                sample={nodes,{1-u-v,u,v}}; return true;
            }
            continue;
        }
        if (nodes.size()!=4) continue;
        double xi=0,eta=0;
        // Invert the isoparametric bilinear map; bounded iterates cannot walk
        // into remote roots of a distorted quadrilateral.
        for (uint iteration=0;iteration<30;++iteration)
        {
            const std::array<double,4> weights{{(1-xi)*(1-eta)/4,(1+xi)*(1-eta)/4,
                (1+xi)*(1+eta)/4,(1-xi)*(1+eta)/4}};
            const std::array<double,4> dx{{-(1-eta)/4,(1-eta)/4,(1+eta)/4,-(1+eta)/4}};
            const std::array<double,4> dy{{-(1-xi)/4,-(1+xi)/4,(1+xi)/4,(1-xi)/4}};
            double x=0,y=0,jxx=0,jxy=0,jyx=0,jyy=0;
            const auto &origin=m.getNode(nodes[0]);
            for (uint j=0;j<4;++j)
            {
                const auto &n=m.getNode(nodes[j]);
                const double nx=n.getX()-origin.getX(),ny=n.getY()-origin.getY();
                x+=weights[j]*nx; y+=weights[j]*ny;
                jxx+=dx[j]*nx; jxy+=dy[j]*nx; jyx+=dx[j]*ny; jyy+=dy[j]*ny;
            }
            const double rx=x-(p[0]-origin.getX()),ry=y-(p[1]-origin.getY());
            if (std::hypot(rx,ry)<=tolerance && std::abs(xi)<=1+naturalTolerance && std::abs(eta)<=1+naturalTolerance)
            {
                sample={nodes,std::vector<double>(weights.begin(),weights.end())}; return true;
            }
            const double det=jxx*jyy-jxy*jyx;
            if (!std::isfinite(det) || det==0) break;
            xi-=(jyy*rx-jxy*ry)/det; eta-=(-jyx*rx+jxx*ry)/det;
            if (!std::isfinite(xi) || !std::isfinite(eta) || std::abs(xi)>2 || std::abs(eta)>2) break;
        }
    }
    return false;
}

RElement element(RElementType type,std::initializer_list<uint> nodes)
{
    RElement e(type); uint i=0;
    for (uint n:nodes) e.setNodeId(i++,n);
    return e;
}

void halveLineForce(RLine &line)
{
    if (!line.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) return;
    auto &bc=line.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
    for (uint j=0;j<bc.size();++j)
    {
        auto &c=bc.getComponent(j);
        for (uint k=0;k<c.size();++k) c.add(c.getKey(k),c.getValue(k)/2);
    }
}
}

QString RMeshSensitivity::eligibility(const RModel &m)
{
    if (m.getProblemTaskTree().getProblemTypeMask()!=R_PROBLEM_STRESS || m.getTimeSolver().getEnabled())
        return "Mesh sensitivity requires a linear static stress problem.";
    if (!m.getNNodes() || !m.getNElements() || !m.getNSurfaces() || m.getNVolumes())
        return "A nonempty planar surface mesh without volume elements is required.";
    if (boundSetup(m.getProblemSetup()) || m.getNVectorFields() || m.getNScalarFields()
        || m.getNStreamLines() || m.getNCuts() || m.getNIsos())
        return "Mesh-bound interactions, control, restart, transfer, and derived entities are unsupported.";
    const double z=m.getNode(0).getZ();
    std::set<std::array<double,3>> coordinates;
    for (const auto &n:m.getNodes())
    {
        if (!finite(position(n)) || n.getZ()!=z) return "All nodes must have finite coordinates in one constant-Z plane.";
        if (!coordinates.insert({n.getX(),n.getY(),n.getZ()}).second) return "Coincident mesh nodes are unsupported.";
    }
    const double scale=lengthScale(m);
    if (!(scale>0) || !std::isfinite(scale)) return "Planar mesh dimensions are invalid.";
    std::vector<uint> owners(m.getNElements(),0);
    std::vector<bool> used(m.getNNodes(),false);
    std::map<Edge,uint> edges;
    std::set<std::vector<uint>> cells;
    for (uint e=0;e<m.getNElements();++e)
    {
        const auto &el=m.getElement(e); const auto type=el.getType();
        if (type!=R_ELEMENT_POINT && type!=R_ELEMENT_TRUSS1 && type!=R_ELEMENT_TRI1 && type!=R_ELEMENT_QUAD1)
            return "Only POINT, zero-area TRUSS1, TRI1 and QUAD1 are supported.";
        const uint expected=type==R_ELEMENT_POINT ? 1 : type==R_ELEMENT_TRUSS1 ? 2 : type==R_ELEMENT_TRI1 ? 3 : 4;
        if (el.size()!=expected || el.hasDuplicateNodes()) return "Invalid element connectivity.";
        for (uint j=0;j<el.size();++j) if (el.getNodeId(j)>=m.getNNodes()) return "Invalid element node index.";
        if (type!=R_ELEMENT_TRI1 && type!=R_ELEMENT_QUAD1) continue;
        std::vector<uint> cell;
        for (uint j=0;j<el.size();++j) cell.push_back(el.getNodeId(j));
        std::sort(cell.begin(),cell.end());
        if (!cells.insert(cell).second) return "Duplicate surface elements are unsupported.";
        double orientation=0;
        for (uint j=0;j<el.size();++j)
        {
            const auto &a=m.getNode(el.getNodeId(j)),&b=m.getNode(el.getNodeId((j+1)%el.size())),
                &c=m.getNode(el.getNodeId((j+2)%el.size()));
            const double det=cross(a,b,c);
            const double local=std::max(std::hypot(b.getX()-a.getX(),b.getY()-a.getY()),
                std::hypot(c.getX()-a.getX(),c.getY()-a.getY()));
            if (!std::isfinite(det) || std::abs(det)<=1e-14*local*local || (orientation!=0 && det*orientation<=0))
                return "Degenerate triangles and nonconvex or folded quadrilaterals are unsupported.";
            orientation=det;
            if (++edges[edge(el.getNodeId(j),el.getNodeId((j+1)%el.size()))]>2)
                return "Nonmanifold surface edges are unsupported.";
            used[el.getNodeId(j)]=true;
        }
    }
    if (std::find(used.begin(),used.end(),false)!=used.end()) return "Every node must belong to a supported surface element.";
    std::vector<std::array<double,3>> prescribed(m.getNNodes(),
        {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN()});
    for (uint g=0;g<m.getNElementGroups();++g)
    {
        const auto &group=*m.getElementGroupPtr(g);
        const auto type=m.getEntityGroupType(g);
        if (group.empty()) return "Empty element groups are unsupported.";
        const QString error=conditions(group,type==R_ENTITY_GROUP_SURFACE);
        if (!error.isEmpty()) return error;
        for (uint j=0;j<group.size();++j)
        {
            const uint e=group.get(j);
            if (e>=m.getNElements() || ++owners[e]!=1) return "Each element must have exactly one owning group.";
            if (RElementGroup::getGroupType(m.getElement(e).getType())!=type) return "Element group type does not match its elements.";
        }
        if (group.hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT))
        {
            const auto &bc=group.getBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT);
            for (uint c=0;c<bc.size();++c)
            {
                const auto &component=bc.getComponent(c);
                if (!component.getEnabled()) continue;
                const uint axis=component.getType()==R_VARIABLE_DISPLACEMENT_X ? 0 :
                    component.getType()==R_VARIABLE_DISPLACEMENT_Y ? 1 : 2;
                for (uint j=0;j<group.size();++j)
                {
                    const auto &el=m.getElement(group.get(j));
                    for (uint k=0;k<el.size();++k)
                    {
                        double &old=prescribed[el.getNodeId(k)][axis];
                        const double value=component.getValue(0);
                        if (std::isfinite(old) && old!=value) return "Conflicting displacement values at a shared node are unsupported.";
                        old=value;
                    }
                }
            }
        }
    }
    if (std::find(owners.begin(),owners.end(),0)!=owners.end()) return "Each element must have exactly one owning group.";
    for (uint g=0;g<m.getNPoints();++g)
        if (m.getPoint(g).getVolume()!=0) return "Nonzero point volume is unsupported.";
    std::set<Edge> lineEdges;
    for (uint g=0;g<m.getNLines();++g)
    {
        const auto &line=m.getLine(g);
        if (line.getCrossArea()!=0) return "Only zero-area line carriers are supported.";
        for (uint j=0;j<line.size();++j)
        {
            const auto &el=m.getElement(line.get(j));
            const auto key=edge(el.getNodeId(0),el.getNodeId(1));
            if (!edges.count(key)) return "Every line must coincide with a complete surface edge.";
            if (!lineEdges.insert(key).second) return "Duplicate or overlapping line carriers are unsupported.";
        }
    }
    for (uint g=0;g<m.getNSurfaces();++g)
    {
        const auto &s=m.getSurface(g);
        if ((s.getStructuralFormulation()!=RElementFormulation::PlaneStress && s.getStructuralFormulation()!=RElementFormulation::PlaneStrain)
            || !std::isfinite(s.getThickness()) || s.getThickness()<=0) return "Positive-thickness plane stress or plane strain surfaces are required.";
        double E=0,nu=0,rho=0;
        if (!property(s.getMaterial(),RMaterialProperty::ModulusOfElasiticity,E) || E<=0
            || !property(s.getMaterial(),RMaterialProperty::PoissonRatio,nu) || nu<=-1 || nu>=0.5)
            return "Each surface needs finite constant positive elasticity and a valid Poisson ratio.";
        bool gravity=false;
        if (s.hasEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION))
        {
            const auto &ec=s.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
            for (uint j=0;j<ec.size();++j)
            {
                const auto &component=ec.getComponent(j);
                if (!component.getEnabled() || component.getValue(0)==0) continue;
                gravity=true;
                if (component.getType()!=R_VARIABLE_G_ACCELERATION_Z) continue;
                for (uint k=0;k<s.size();++k)
                {
                    const auto &el=m.getElement(s.get(k));
                    for (uint n=0;n<el.size();++n)
                        if (!std::isfinite(prescribed[el.getNodeId(n)][2]))
                            return "Out-of-plane gravity requires Uz constraints across the loaded surface.";
                }
            }
        }
        const bool hasDensity=s.getMaterial().findPosition(RMaterialProperty::Density)<s.getMaterial().size();
        if ((gravity || hasDensity) && (!property(s.getMaterial(),RMaterialProperty::Density,rho) || rho<=0))
            return "Gravity requires a finite constant positive material density.";
        // Results of thermal coupling are never silently dropped by refinement.
        for (uint j=0;j<m.getNVariables();++j)
            if (m.getVariable(j).getType()==R_VARIABLE_TEMPERATURE) return "Retained coupled temperature results are unsupported.";
    }
    return {};
}

RModel RMeshSensitivity::refine(const RModel &source,uint maxElements)
{
    const QString error=eligibility(source);
    if (!error.isEmpty()) fail(error);
    quint64 count=0;
    for (uint e=0;e<source.getNElements();++e)
    {
        const auto type=source.getElement(e).getType();
        count+=type==R_ELEMENT_POINT ? 1 : type==R_ELEMENT_TRUSS1 ? 2 : 4;
    }
    if (count>maxElements || count>=RConstants::eod) fail("The refined mesh exceeds the element budget.");
    RModel result(source);
    result.removeAllVariables();
    result.clearSurfaceNeighbors(); result.clearVolumeNeighbors();
    std::map<Edge,uint> midpoints;
    const auto midpoint=[&](uint a,uint b) {
        const Edge key=edge(a,b); const auto found=midpoints.find(key);
        if (found!=midpoints.end()) return found->second;
        const auto &na=source.getNode(a),&nb=source.getNode(b);
        const uint n=result.getNNodes();
        result.addNode(RNode(na.getX()/2+nb.getX()/2,na.getY()/2+nb.getY()/2,na.getZ()));
        midpoints.emplace(key,n); return n;
    };
    const auto publish=[&](uint e,uint group,const std::vector<RElement> &children) {
        result.setElement(e,children.front(),false);
        for (size_t j=1;j<children.size();++j) result.addElement(children[j],true,group);
    };
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        const auto &surface=source.getSurface(g);
        for (uint j=0;j<surface.size();++j)
        {
            const uint e=surface.get(j); const auto &el=source.getElement(e);
            const uint a=el.getNodeId(0),b=el.getNodeId(1),c=el.getNodeId(2),ab=midpoint(a,b),bc=midpoint(b,c);
            if (el.getType()==R_ELEMENT_TRI1)
            {
                const uint ca=midpoint(c,a);
                publish(e,g,{element(R_ELEMENT_TRI1,{a,ab,ca}),element(R_ELEMENT_TRI1,{ab,b,bc}),
                    element(R_ELEMENT_TRI1,{ca,bc,c}),element(R_ELEMENT_TRI1,{ab,bc,ca})});
            }
            else
            {
                const uint d=el.getNodeId(3),cd=midpoint(c,d),da=midpoint(d,a),center=result.getNNodes();
                const auto &na=source.getNode(a),&nb=source.getNode(b),&nc=source.getNode(c),&nd=source.getNode(d);
                result.addNode(RNode(na.getX()/4+nb.getX()/4+nc.getX()/4+nd.getX()/4,
                    na.getY()/4+nb.getY()/4+nc.getY()/4+nd.getY()/4,na.getZ()));
                publish(e,g,{element(R_ELEMENT_QUAD1,{a,ab,center,da}),element(R_ELEMENT_QUAD1,{ab,b,bc,center}),
                    element(R_ELEMENT_QUAD1,{center,bc,c,cd}),element(R_ELEMENT_QUAD1,{da,center,cd,d})});
            }
        }
    }
    for (uint g=0;g<source.getNLines();++g)
    {
        const auto &line=source.getLine(g);
        for (uint j=0;j<line.size();++j)
        {
            const uint e=line.get(j); const auto &el=source.getElement(e);
            const uint a=el.getNodeId(0),b=el.getNodeId(1),ab=midpoints.at(edge(a,b));
            publish(e,g,{element(R_ELEMENT_TRUSS1,{a,ab}),element(R_ELEMENT_TRUSS1,{ab,b})});
        }
        halveLineForce(result.getLine(g));
    }
    result.clearSurfaceNeighbors(); result.clearVolumeNeighbors();
    return result;
}

bool RMeshSensitivity::supportsLocal(const RModel &source)
{
    if (!eligibility(source).isEmpty()) return false;
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        const auto &surface=source.getSurface(g);
        for (uint j=0;j<surface.size();++j)
            if (source.getElement(surface.get(j)).getType()!=R_ELEMENT_TRI1) return false;
    }
    return true;
}

RModel RMeshSensitivity::refineLocal(const RModel &source,const RR3Vector &point,double radiusM,uint maxElements)
{
    if (!supportsLocal(source)) fail("Local sensitivity requires a supported all-TRI1 planar static mesh.");
    if (!finite(point) || !std::isfinite(radiusM) || radiusM<=0)
        fail("Local refinement needs a finite point and a positive radius in metres.");
    Sample containing;
    if (!locate(source,point,containing)) fail("The local refinement point is outside the supported planar mesh.");
    std::set<Edge> marked;
    std::vector<uint> probeSupport;
    for (size_t i=0;i<containing.nodes.size();++i)
        if (containing.weights[i]>1e-9) probeSupport.push_back(containing.nodes[i]);
    bool includedContaining=false;
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        const auto &surface=source.getSurface(g);
        for (uint j=0;j<surface.size();++j)
        {
            const auto &el=source.getElement(surface.get(j));
            const auto &a=source.getNode(el.getNodeId(0)),&b=source.getNode(el.getNodeId(1)),&c=source.getNode(el.getNodeId(2));
            const double cx=a.getX()/3+b.getX()/3+c.getX()/3,cy=a.getY()/3+b.getY()/3+c.getY()/3;
            // At a vertex or edge include all incident cells regardless of order.
            const bool contains=std::all_of(probeSupport.begin(),probeSupport.end(),
                [&](uint n) { return el.hasNodeId(n); });
            includedContaining=includedContaining || contains;
            if (!contains && std::hypot(cx-point[0],cy-point[1])>radiusM) continue;
            for (uint k=0;k<3;++k) marked.insert(edge(el.getNodeId(k),el.getNodeId((k+1)%3)));
        }
    }
    if (!includedContaining || marked.empty()) fail("No containing triangle could be selected for local refinement.");
    // FORCE denotes total force per line element in the native stress solver.
    // A group stores one condition table, so its split state must be uniform.
    // Existing line edges are disjoint by eligibility; this expansion cannot
    // introduce a partially split second force group.
    std::vector<bool> splitForceGroup(source.getNLines(),false);
    for (uint g=0;g<source.getNLines();++g)
    {
        const auto &line=source.getLine(g);
        if (!line.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
        bool touched=false;
        for (uint j=0;j<line.size();++j)
        {
            const auto &el=source.getElement(line.get(j));
            touched=touched || marked.count(edge(el.getNodeId(0),el.getNodeId(1)));
        }
        if (!touched) continue;
        splitForceGroup[g]=true;
        for (uint j=0;j<line.size();++j)
        {
            const auto &el=source.getElement(line.get(j));
            marked.insert(edge(el.getNodeId(0),el.getNodeId(1)));
        }
    }
    quint64 count=source.getNElements();
    for (uint e=0;e<source.getNElements();++e)
    {
        const auto &el=source.getElement(e);
        if (el.getType()==R_ELEMENT_TRI1)
            for (uint j=0;j<3;++j) count+=marked.count(edge(el.getNodeId(j),el.getNodeId((j+1)%3)));
        else if (el.getType()==R_ELEMENT_TRUSS1) count+=marked.count(edge(el.getNodeId(0),el.getNodeId(1)));
    }
    if (count>maxElements || count>=RConstants::eod) fail("The locally refined mesh exceeds the element budget.");
    RModel result(source);
    result.removeAllVariables(); result.clearSurfaceNeighbors(); result.clearVolumeNeighbors();
    std::map<Edge,uint> midpoints;
    for (const auto &key:marked)
    {
        const auto &a=source.getNode(key.first),&b=source.getNode(key.second);
        const uint n=result.getNNodes();
        result.addNode(RNode(a.getX()/2+b.getX()/2,a.getY()/2+b.getY()/2,a.getZ()));
        midpoints.emplace(key,n);
    }
    const auto publish=[&](uint e,uint group,const std::vector<RElement> &children) {
        result.setElement(e,children.front(),false);
        for (size_t j=1;j<children.size();++j) result.addElement(children[j],true,group);
    };
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        const auto &surface=source.getSurface(g);
        for (uint j=0;j<surface.size();++j)
        {
            const uint e=surface.get(j); const auto &el=source.getElement(e);
            std::array<uint,3> n{{el.getNodeId(0),el.getNodeId(1),el.getNodeId(2)}};
            std::array<bool,3> split{{marked.count(edge(n[0],n[1]))!=0,marked.count(edge(n[1],n[2]))!=0,
                marked.count(edge(n[2],n[0]))!=0}};
            const uint number=uint(split[0])+uint(split[1])+uint(split[2]);
            if (!number) continue;
            if (number==3)
            {
                const uint ab=midpoints.at(edge(n[0],n[1])),bc=midpoints.at(edge(n[1],n[2])),ca=midpoints.at(edge(n[2],n[0]));
                publish(e,g,{element(R_ELEMENT_TRI1,{n[0],ab,ca}),element(R_ELEMENT_TRI1,{ab,n[1],bc}),
                    element(R_ELEMENT_TRI1,{ca,bc,n[2]}),element(R_ELEMENT_TRI1,{ab,bc,ca})});
                continue;
            }
            uint first=0;
            while (!(split[first] && (number==1 || split[(first+1)%3]))) ++first;
            const uint a=n[first],b=n[(first+1)%3],c=n[(first+2)%3],ab=midpoints.at(edge(a,b));
            if (number==1)
                publish(e,g,{element(R_ELEMENT_TRI1,{a,ab,c}),element(R_ELEMENT_TRI1,{ab,b,c})});
            else
            {
                const uint bc=midpoints.at(edge(b,c));
                publish(e,g,{element(R_ELEMENT_TRI1,{ab,b,bc}),element(R_ELEMENT_TRI1,{a,ab,c}),element(R_ELEMENT_TRI1,{ab,bc,c})});
            }
        }
    }
    for (uint g=0;g<source.getNLines();++g)
    {
        const auto &line=source.getLine(g);
        for (uint j=0;j<line.size();++j)
        {
            const uint e=line.get(j); const auto &el=source.getElement(e);
            const uint a=el.getNodeId(0),b=el.getNodeId(1);
            const auto found=midpoints.find(edge(a,b));
            if (found==midpoints.end()) continue;
            publish(e,g,{element(R_ELEMENT_TRUSS1,{a,found->second}),element(R_ELEMENT_TRUSS1,{found->second,b})});
        }
        if (splitForceGroup[g]) halveLineForce(result.getLine(g));
    }
    result.clearSurfaceNeighbors(); result.clearVolumeNeighbors();
    return result;
}

std::vector<RR3Vector> RMeshSensitivity::probePositions(const RModel &source)
{
    std::vector<RR3Vector> positions;
    for (const auto &point:source.getMonitoringPointManager())
    {
        if (point.getVariableType()!=R_VARIABLE_DISPLACEMENT) continue;
        const RR3Vector p=point.getPosition();
        const auto duplicate=std::find_if(positions.begin(),positions.end(),[&](const RR3Vector &old) {
            return p[0]==old[0] && p[1]==old[1] && p[2]==old[2];
        });
        if (duplicate!=positions.end()) continue;
        Sample sample;
        if (!locate(source,p,sample)) fail("A displacement monitoring point is outside the supported planar mesh.");
        if (positions.size()==12) fail("Choose at most twelve displacement monitoring points for mesh sensitivity.");
        positions.push_back(p);
    }
    if (positions.empty())
    {
        if (!source.getNNodes()) fail("Cannot select probes from an empty mesh.");
        const auto &v=displacements(source);
        uint best=0; double magnitude=-1;
        for (uint n=0;n<source.getNNodes();++n)
        {
            const double value=std::hypot(v.getValue(0,n),v.getValue(1,n),v.getValue(2,n));
            if (value>magnitude) { best=n; magnitude=value; }
        }
        positions.push_back(position(source.getNode(best)));
    }
    return positions;
}

QJsonArray RMeshSensitivity::measurements(const RModel &solved,const std::vector<RR3Vector> &positions)
{
    if (positions.empty() || positions.size()>12) fail("Mesh sensitivity requires one to twelve fixed probe locations.");
    const auto &v=displacements(solved);
    QJsonArray values;
    for (const auto &p:positions)
    {
        Sample sample;
        if (!locate(solved,p,sample)) fail("A fixed probe is outside the solved mesh.");
        double ux=0,uy=0;
        for (size_t i=0;i<sample.nodes.size();++i)
        {
            ux+=sample.weights[i]*v.getValue(0,sample.nodes[i]);
            uy+=sample.weights[i]*v.getValue(1,sample.nodes[i]);
        }
        if (!std::isfinite(ux*1000) || !std::isfinite(uy*1000)) fail("Interpolated probe displacement is not finite.");
        values.append(QJsonObject{{"xM",p[0]},{"yM",p[1]},{"zM",p[2]},{"uxMm",ux*1000},{"uyMm",uy*1000}});
    }
    return values;
}
