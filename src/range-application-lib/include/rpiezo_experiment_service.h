#ifndef RPIEZO_EXPERIMENT_SERVICE_H
#define RPIEZO_EXPERIMENT_SERVICE_H
#include <rml_model.h>
#include <QJsonObject>
#include <QJsonArray>

//! Pure whole-experiment composer. No session mutation or artifact publication.
class RPiezoExperimentService
{
public:
    struct Result {
        bool valid=false;
        QString code,message;
        RModel candidate;
        QJsonObject normalizedArguments,report,units,invalidation;
        QJsonArray changedPaths,diagnostics;
    };
    static Result preview(const RModel &source,const QJsonObject &recipe);
    //! Empty object means unsupported geometry; IDs are taken from the actual mesh.
    static QJsonObject exampleRecipe(const RModel &source,const QString &family={},
                                    const QJsonArray &selectedMemberIds={});
    //! Reconstruct editable source declarations from an already bound experiment.
    static QJsonObject recipeFromModel(const RModel &source);
    //! Only empty sources may generate topology. Throws on invalid arguments.
    static RModel templateCandidate(const RModel &source,const QJsonObject &arguments);
};
#endif
