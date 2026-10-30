#ifndef RPIEZO_SOLID_RECIPE_H
#define RPIEZO_SOLID_RECIPE_H
#include <rml_model.h>
#include <QJsonObject>
#include <QJsonArray>
namespace RPiezoSolidRecipe {
bool supports(const RModel &);
RModel templateCandidate(const RModel &,const QJsonObject &);
QJsonObject example(const RModel &,const QJsonArray &selection={});
}
#endif
