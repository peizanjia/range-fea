#ifndef RGEOMETRY_CANDIDATE_SERVICE_H
#define RGEOMETRY_CANDIDATE_SERVICE_H

#include <array>
#include <vector>
#include <QJsonArray>
#include <QJsonObject>
#include <rml_model.h>

//! Pure WF14 geometry candidate construction. Authority/CAS/publication belong to the caller.
class RGeometryCandidateService
{
public:
    struct NodeDelta
    {
        uint physicalNodeId = 0;
        std::array<double,3> beforeXYZ{};
        std::array<double,3> afterXYZ{};
    };
    struct GeometryDelta
    {
        std::vector<NodeDelta> nodes; //!< Sorted by stable physical node ID; changed nodes only.
        QString preGeometrySha256;
        QString postGeometrySha256;
        QJsonArray affectedPhysicalNodeIds;
        bool noOp = false;
    };
    struct GeometryCandidate
    {
        bool valid = false;
        QString code;
        QString message;
        RModel candidate;
        GeometryDelta delta;
        QJsonObject normalizedArguments;
    };

    //! Coordinates, center, translation in metres; right-handed axis-angle in degrees.
    //! Only POINT/TRUSS1/BEAM1 meshes (or isolated nodes) are currently supported.
    //! directionPolicy must be preserve_global: all non-coordinate model data are copied intact.
    static GeometryCandidate preview(const RModel &model, const QString &commandId,
                                     const QJsonObject &arguments);
    //! Exact binary coordinate/type/connectivity hash, ordered by stable physical IDs.
    //! Includes every node/element, including unsupported element types; invalid IDs/references return empty.
    static QString geometryFingerprint(const RModel &model);
    static QJsonObject capabilities();
};

#endif
