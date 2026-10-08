#ifndef RMODEL_COMMAND_SERVICE_H
#define RMODEL_COMMAND_SERVICE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <rml_boundary_condition.h>
#include <rml_material.h>
#include <rml_model.h>
#include <rml_structural_dynamics_setup.h>
#include <rml_time_solver.h>

//! Pure, shared preview and read service for the supported native model edits.
//! The caller owns approval, source identity, revision checks, and publication.
class RModelCommandService
{
public:
    struct Delta
    {
        enum class Kind { None, Modal, StructuralDynamics, Material, Boundary };
        Kind kind = Kind::None;
        uint physicalId = 0;
        RBoundaryConditionType boundaryType = R_BOUNDARY_CONDITION_NONE;
        bool hadBeforeBoundary = false;
        RModalSetup beforeModal;
        RModalSetup afterModal;
        RStructuralDynamicsSetup beforeDynamics;
        RStructuralDynamicsSetup afterDynamics;
        RTimeSolver beforeTimeSolver;
        RTimeSolver afterTimeSolver;
        RMaterial beforeMaterial;
        RMaterial afterMaterial;
        RBoundaryCondition beforeBoundary;
        RBoundaryCondition afterBoundary;
        QJsonObject beforeState;
        QJsonObject afterState;
        QJsonObject normalizedArguments;
        QString changedPath;
        QJsonArray changedPaths;
    };

    struct PreviewResult
    {
        bool valid = false;
        QString code;
        QString message;
        RModel candidate;
        QJsonObject normalizedArguments;
        QJsonObject before;
        QJsonObject after;
        QJsonArray changedPaths;
        QJsonArray diagnostics;
        QString fingerprint;
        Delta delta;
    };

    //! Build and validate a complete candidate without mutating the input.
    static PreviewResult preview(const RModel &model, const QString &commandId,
                                 const QJsonObject &arguments,
                                 const QString &sourceId = QString(),
                                 const QString &revision = QString());

    //! Reapply a captured supported-field delta to an owned model snapshot.
    static PreviewResult applyDelta(const RModel &model, const Delta &delta, bool forward);

    static QJsonObject modalParameters(const RModalSetup &setup);
    static QJsonObject structuralDynamicsParameters(const RStructuralDynamicsSetup &setup);
    //! Hash the full supported edit surface (analysis, time grid, materials,
    //! boundary conditions, and stable target identities) for journal binding.
    static QString stateFingerprint(const RModel &model);
    static QJsonObject capabilities(const RModel &model);
    static QJsonObject context(const RModel &model, const QString &sourceId,
                               const QString &revision, const QJsonObject &pageRequest = {});
    static QJsonObject targets(const RModel &model, const QString &sourceId,
                               const QString &revision, const QJsonObject &pageRequest = {});
};

#endif
