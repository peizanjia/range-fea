#ifndef RMESH_ADAPTIVITY_H
#define RMESH_ADAPTIVITY_H

#include <rml_model.h>
#include <vector>

//! Equilibrium-residual guidance for static, Cartesian XY TRI3 membranes.
//! This stiffness-weighted indicator is a heuristic, not a certified error bound.
namespace RMeshAdaptivity
{
//! Empty for the supported mechanical scope; results are checked by estimate().
QString eligibility(const RModel &source);

struct Estimate
{
    double relativeError = 0.0; //!< sqrt(indicatorEnergy / (2*strainEnergy)).
    double indicatorEnergy = 0.0; //!< Sum of squared element indicators, in joules.
    double strainEnergy = 0.0; //!< Integrated elastic strain energy, in joules.
    std::vector<uint> markedElements; //!< Original element IDs, descending indicator; ID breaks ties.
};

//! Recover constant TRI3 stress from complete finite nodal displacement results.
//! Dörfler marking selects the shortest sorted prefix carrying markingFraction
//! of indicatorEnergy. The fraction must be in (0,1]. An exactly zero indicator
//! needs no refinement; a nonzero indicator with zero strain energy is rejected.
//! Throws std::runtime_error for unsupported input or nonfinite arithmetic.
Estimate estimate(const RModel &solved, double markingFraction = 0.5);
}

#endif
