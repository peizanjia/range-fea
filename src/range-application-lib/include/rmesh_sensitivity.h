#ifndef RMESH_SENSITIVITY_H
#define RMESH_SENSITIVITY_H

#include <QJsonArray>
#include <rml_model.h>
#include <vector>
#include <stdexcept>

//! Detached, bounded h-refinement for static XY plane stress/strain models.
namespace RMeshSensitivity
{
//! A requested refinement exceeds its element budget or minimum edge resolution.
class RefinementLimit : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
//! Empty when a global half-size candidate can preserve the supported physics.
QString eligibility(const RModel &source);
//! Preserve original nodes/IDs; append conforming edge midpoints and cell centers.
//! Throws std::runtime_error on unsupported input or an exceeded element budget.
RModel refine(const RModel &source, uint maxElements);
//! Local subdivision is available for the supported all-TRI1 surface contract.
bool supportsLocal(const RModel &source);
//! One conforming pass around a fixed location, expanding touched force groups.
RModel refineLocal(const RModel &source, const RR3Vector &point, double radiusM, uint maxElements);
//! Refine selected TRI1 element IDs with shared-edge closure and force preservation.
//! Reject empty/invalid marks, exceeded budgets, and new child edges below minimumEdgeM.
RModel refineMarked(const RModel &source, const std::vector<uint> &markedElements, uint maxElements, double minimumEdgeM = 0.0);
//! Up to twelve fixed displacement monitoring locations within the mesh.
//! With no displacement monitoring points, use the baseline maximum-displacement node.
std::vector<RR3Vector> probePositions(const RModel &source);
//! Evaluate complete finite nodal displacements at fixed coordinates (metres).
//! TRI1 uses barycentric and QUAD1 uses bilinear finite-element interpolation.
QJsonArray measurements(const RModel &solved, const std::vector<RR3Vector> &positions);
}

#endif
