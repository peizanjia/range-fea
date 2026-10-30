#ifndef RPIEZO_AXIAL_EXPERIMENT_SERVICE_H
#define RPIEZO_AXIAL_EXPERIMENT_SERVICE_H

#include "rpiezo_experiment_service.h"
#include <rml_structural_piezo_layered_setup.h>

//! Internal shared declaration validation; no layered geometry is compiled here.
namespace RPiezoExperimentDeclarations {
RStructuralPiezoLayeredSetup parseAxialCommon(const QJsonObject &recipe);
void validateController(const QJsonObject &recipe,const RStructuralPiezoLayeredSetup &declarations);
//! Strict shared time-function declarations and the union of named force channels.
QJsonArray forceChannels(const QJsonObject &excitation);
//! Resolve only named global axes or persisted source-frame local axes.
std::array<double,6> forceDirection(const QJsonObject &channel,const QString &family,
                                  const RStructuralPiezoLayeredFrame &frame);
}

//! Pure actual TRUSS1/POINT whole-experiment composer.
class RPiezoAxialExperimentService
{
public:
    static RPiezoExperimentService::Result preview(const RModel &source,const QJsonObject &recipe);
    static QJsonObject exampleRecipe(const RModel &source,const QJsonArray &selection);
    static QJsonObject recipeFromModel(const RModel &source);
    static RModel templateCandidate(const RModel &source,const QJsonObject &arguments);
};

#endif
