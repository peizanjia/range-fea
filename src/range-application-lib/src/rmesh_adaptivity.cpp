#include "rmesh_adaptivity.h"
#include "rmesh_sensitivity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>

namespace
{
using Edge = std::pair<uint,uint>;
using Vector = std::array<double,2>;
using Mask = std::array<bool,2>;
Edge edge(uint a,uint b) { return std::minmax(a,b); }
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }

double property(const RMaterial &material,RMaterialProperty::Type type)
{
    const uint id=material.findPosition(type);
    return id<material.size() ? material.get(id).getValue(0) : 0.0;
}

Vector force(const RElementGroup &group)
{
    Vector value{{0.0,0.0}};
    if (!group.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) return value;
    const auto &condition=group.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
    for (uint j=0;j<condition.size();++j)
    {
        const auto &c=condition.getComponent(j);
        if (!c.getEnabled()) continue;
        if (c.getType()==R_VARIABLE_FORCE_X) value[0]=c.getValue(0);
        if (c.getType()==R_VARIABLE_FORCE_Y) value[1]=c.getValue(0);
    }
    return value;
}

Mask constraints(const RElementGroup &group)
{
    Mask mask{{false,false}};
    if (!group.hasBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT)) return mask;
    const auto &condition=group.getBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT);
    for (uint j=0;j<condition.size();++j)
    {
        const auto &c=condition.getComponent(j);
        if (!c.getEnabled()) continue;
        if (c.getType()==R_VARIABLE_DISPLACEMENT_X) mask[0]=true;
        if (c.getType()==R_VARIABLE_DISPLACEMENT_Y) mask[1]=true;
    }
    return mask;
}

void constrain(Mask &target,const Mask &mask)
{
    for (uint axis=0;axis<2;++axis) target[axis]=target[axis] || mask[axis];
}

const RVariable &displacements(const RModel &model)
{
    const uint id=model.findVariable(R_VARIABLE_DISPLACEMENT);
    if (id==RConstants::eod) fail("The adaptive estimator requires a solved nodal displacement result.");
    const auto &value=model.getVariable(id);
    if (value.getApplyType()!=R_VARIABLE_APPLY_NODE || value.getNVectors()!=3 || value.getNValues()!=model.getNNodes())
        fail("The adaptive estimator requires complete nodal XYZ displacements.");
    for (uint axis=0;axis<3;++axis)
    {
        if (value[axis].size()!=model.getNNodes()) fail("Displacement component size does not match the mesh.");
        for (uint n=0;n<model.getNNodes();++n)
            if (!std::isfinite(value.getValue(axis,n))) fail("Displacements contain a nonfinite value.");
    }
    return value;
}

struct Cell
{
    uint element=0;
    double stiffness=0.0;
    double indicator=0.0;
    std::array<double,3> resultant{{0.0,0.0,0.0}}; // Nxx, Nyy, Nxy, in N/m.
};
struct Side
{
    size_t cell=0;
    Vector normal{{0.0,0.0}};
};
struct Face
{
    double length=0.0;
    std::vector<Side> sides;
    Vector load{{0.0,0.0}}; // Force per original segment divided by its length.
    Mask fixed{{false,false}};
};

Vector traction(const Cell &cell,const Vector &normal)
{
    return {{cell.resultant[0]*normal[0]+cell.resultant[2]*normal[1],
             cell.resultant[2]*normal[0]+cell.resultant[1]*normal[1]}};
}
double squared(const Vector &value) { return value[0]*value[0]+value[1]*value[1]; }
}

QString RMeshAdaptivity::eligibility(const RModel &source)
{
    const QString error=RMeshSensitivity::eligibility(source);
    if (!error.isEmpty()) return error;
    // The common scope was checked above; supportsLocal() would repeat that
    // complete validation just to add this all-triangle restriction.
    for (uint g=0;g<source.getNPoints();++g)
    {
        const auto value=force(source.getPoint(g));
        if (value[0]!=0.0 || value[1]!=0.0)
            return "Adaptive residual estimation does not support nonzero point forces; use distributed boundary line forces.";
    }
    std::map<Edge,uint> incidence;
    for (uint g=0;g<source.getNSurfaces();++g)
    {
        const auto &surface=source.getSurface(g);
        const auto &material=surface.getMaterial();
        const uint alpha=material.findPosition(RMaterialProperty::ThermalExpansionCoefficient);
        if (alpha<material.size())
        {
            const auto &table=material.get(alpha);
            if (table.empty()) return "Adaptive residual estimation requires a finite zero thermal-expansion coefficient or no thermal-expansion property.";
            for (uint j=0;j<table.size();++j)
                if (!std::isfinite(table.getKey(j)) || !std::isfinite(table.getValue(j)) || table.getValue(j)!=0.0)
                    return "Adaptive residual estimation supports mechanical strain only; nonzero thermal-expansion coefficients are unsupported.";
        }
        for (uint j=0;j<surface.size();++j)
        {
            const auto &el=source.getElement(surface.get(j));
            if (el.getType()!=R_ELEMENT_TRI1)
                return "Adaptive residual estimation requires an all-TRI3 (TRI1) plane stress or plane strain mesh.";
            for (uint k=0;k<3;++k) ++incidence[edge(el.getNodeId(k),el.getNodeId((k+1)%3))];
        }
    }
    for (uint g=0;g<source.getNLines();++g)
    {
        const auto &line=source.getLine(g);
        const auto value=force(line);
        if (value[0]==0.0 && value[1]==0.0) continue;
        for (uint j=0;j<line.size();++j)
        {
            const auto &el=source.getElement(line.get(j));
            if (incidence[edge(el.getNodeId(0),el.getNodeId(1))]!=1)
                return "Adaptive residual estimation supports line forces on exterior boundary edges only.";
        }
    }
    return {};
}

RMeshAdaptivity::Estimate RMeshAdaptivity::estimate(const RModel &model,double markingFraction)
{
    const QString error=eligibility(model);
    if (!error.isEmpty()) fail(error);
    if (!std::isfinite(markingFraction) || markingFraction<=0.0 || markingFraction>1.0)
        fail("The Dörfler marking fraction must be finite and in (0,1].");
    const auto &u=displacements(model);
    Estimate result;
    std::vector<Cell> cells;
    std::map<Edge,Face> faces;
    for (uint g=0;g<model.getNSurfaces();++g)
    {
        const auto &surface=model.getSurface(g);
        const auto &material=surface.getMaterial();
        const double E=property(material,RMaterialProperty::ModulusOfElasiticity);
        const double nu=property(material,RMaterialProperty::PoissonRatio);
        const double t=surface.getThickness(),mu=E/(2.0*(1.0+nu));
        const double lambda=surface.getStructuralFormulation()==RElementFormulation::PlaneStress
            ? E*nu/(1.0-nu*nu) : E*nu/((1.0+nu)*(1.0-2.0*nu));
        // Coercivity in the symmetric-tensor norm, including auxetic materials.
        const double stiffness=t*std::min(2.0*mu,2.0*(mu+lambda));
        if (!(stiffness>0.0) || !std::isfinite(stiffness)) fail("The membrane stiffness is not finite and positive.");
        Vector body{{0.0,0.0}};
        if (surface.hasEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION))
        {
            const auto &gravity=surface.getEnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION);
            const double density=property(material,RMaterialProperty::Density);
            for (uint j=0;j<gravity.size();++j)
            {
                const auto &c=gravity.getComponent(j);
                if (!c.getEnabled()) continue;
                if (c.getType()==R_VARIABLE_G_ACCELERATION_X) body[0]=t*density*c.getValue(0);
                if (c.getType()==R_VARIABLE_G_ACCELERATION_Y) body[1]=t*density*c.getValue(0);
            }
        }
        const Mask fixed=constraints(surface);
        // Residuals act on admissible displacement variations. A Cartesian
        // component prescribed over the whole surface has no such variation;
        // its body load is balanced by a reaction, not a discretization error.
        for (uint axis=0;axis<2;++axis) if (fixed[axis]) body[axis]=0.0;
        for (uint j=0;j<surface.size();++j)
        {
            const uint id=surface.get(j);
            const auto &el=model.getElement(id);
            const auto &a=model.getNode(el.getNodeId(0)),&b=model.getNode(el.getNodeId(1)),&c=model.getNode(el.getNodeId(2));
            const double det=(b.getX()-a.getX())*(c.getY()-a.getY())-(b.getY()-a.getY())*(c.getX()-a.getX());
            const double area=std::abs(det)/2.0;
            const std::array<double,3> dx{{(b.getY()-c.getY())/det,(c.getY()-a.getY())/det,(a.getY()-b.getY())/det}};
            const std::array<double,3> dy{{(c.getX()-b.getX())/det,(a.getX()-c.getX())/det,(b.getX()-a.getX())/det}};
            double ex=0.0,ey=0.0,gamma=0.0;
            // Subtract the first nodal displacement for translation robustness.
            for (uint k=1;k<3;++k)
            {
                const double ux=u.getValue(0,el.getNodeId(k))-u.getValue(0,el.getNodeId(0));
                const double uy=u.getValue(1,el.getNodeId(k))-u.getValue(1,el.getNodeId(0));
                ex+=dx[k]*ux; ey+=dy[k]*uy; gamma+=dy[k]*ux+dx[k]*uy;
            }
            Cell cell;
            cell.element=id; cell.stiffness=stiffness;
            cell.resultant={{t*((2.0*mu+lambda)*ex+lambda*ey),
                             t*(lambda*ex+(2.0*mu+lambda)*ey),t*mu*gamma}};
            const double energy=area*(ex*cell.resultant[0]+ey*cell.resultant[1]+gamma*cell.resultant[2])/2.0;
            if (!std::isfinite(energy) || energy<0.0) fail("Recovered elastic strain energy is not finite and nonnegative.");
            result.strainEnergy+=energy;
            double h=0.0;
            for (uint k=0;k<3;++k)
            {
                const uint n0=el.getNodeId(k),n1=el.getNodeId((k+1)%3);
                const auto &p=model.getNode(n0),&q=model.getNode(n1);
                const double vx=q.getX()-p.getX(),vy=q.getY()-p.getY(),length=std::hypot(vx,vy);
                h=std::max(h,length);
                auto &face=faces[edge(n0,n1)];
                face.length=length;
                const double sign=det>0.0 ? 1.0 : -1.0;
                face.sides.push_back({cells.size(),{{sign*vy/length,-sign*vx/length}}});
                constrain(face.fixed,fixed);
            }
            // Constant TRI3 stress has zero elementwise divergence.
            cell.indicator=h*h*area*squared(body)/stiffness;
            cells.push_back(cell);
        }
    }
    for (uint g=0;g<model.getNLines();++g)
    {
        const auto &line=model.getLine(g);
        const auto value=force(line);
        const auto fixed=constraints(line);
        for (uint j=0;j<line.size();++j)
        {
            const auto &el=model.getElement(line.get(j));
            auto &face=faces.at(edge(el.getNodeId(0),el.getNodeId(1)));
            for (uint axis=0;axis<2;++axis) face.load[axis]=value[axis]/face.length;
            constrain(face.fixed,fixed);
        }
    }
    for (const auto &entry:faces)
    {
        const auto &face=entry.second;
        auto &a=cells[face.sides[0].cell];
        auto residual=traction(a,face.sides[0].normal);
        if (face.sides.size()==2)
        {
            auto &b=cells[face.sides[1].cell];
            const auto other=traction(b,face.sides[1].normal);
            // Full-edge prescriptions from a line or either adjacent surface
            // also remove the corresponding interior test-function trace.
            // A jump in that component represents a Dirichlet reaction.
            for (uint axis=0;axis<2;++axis)
                residual[axis]=face.fixed[axis] ? 0.0 : residual[axis]+other[axis];
            const double contribution=face.length*face.length*squared(residual)/std::max(a.stiffness,b.stiffness);
            a.indicator+=contribution/2.0; b.indicator+=contribution/2.0;
        }
        else
        {
            // Only full edge constraints define Dirichlet boundary components.
            // An isolated constrained point must not erase a Neumann residual.
            for (uint axis=0;axis<2;++axis)
                residual[axis]=face.fixed[axis] ? 0.0 : residual[axis]-face.load[axis];
            a.indicator+=face.length*face.length*squared(residual)/a.stiffness;
        }
    }
    for (const auto &cell:cells)
    {
        if (!std::isfinite(cell.indicator) || cell.indicator<0.0) fail("The residual indicator is not finite and nonnegative.");
        result.indicatorEnergy+=cell.indicator;
    }
    if (!std::isfinite(result.indicatorEnergy) || !std::isfinite(result.strainEnergy))
        fail("The accumulated estimator energy is not finite.");
    if (result.indicatorEnergy==0.0) return result;
    if (!(result.strainEnergy>0.0))
        fail("A nonzero equilibrium residual cannot be normalized by zero elastic strain energy.");
    result.relativeError=(std::sqrt(result.indicatorEnergy)/std::sqrt(result.strainEnergy))/std::sqrt(2.0);
    if (!std::isfinite(result.relativeError)) fail("The normalized residual indicator is not finite.");
    std::sort(cells.begin(),cells.end(),[](const Cell &a,const Cell &b) {
        return a.indicator==b.indicator ? a.element<b.element : a.indicator>b.indicator;
    });
    const double target=markingFraction*result.indicatorEnergy;
    double selected=0.0;
    for (const auto &cell:cells)
    {
        if (cell.indicator==0.0) break;
        result.markedElements.push_back(cell.element);
        selected+=cell.indicator;
        if (selected>=target) break;
    }
    return result;
}
