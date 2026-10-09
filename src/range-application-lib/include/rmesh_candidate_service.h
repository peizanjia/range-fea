#ifndef RMESH_CANDIDATE_SERVICE_H
#define RMESH_CANDIDATE_SERVICE_H

#include <atomic>
#include <QJsonObject>
#include <rml_model.h>

//! Detached, bounded WF14 candidates. No Session, file publication or acceptance authority.
class RMeshCandidateService
{
public:
    struct MeshCandidate
    {
        bool valid = false;
        QString code;
        QString message;
        RModel candidate; //!< Empty on preview/failure/cancellation; never a live model reference.
        QJsonObject normalizedArguments;
        QString sourceGeometrySha256;
        QString postGeometrySha256;
        QJsonObject report;
    };

    //! Exact fields: unit="m", volumeUnit="m^3", maxVolume in [1e-15,1e18],
    //! radiusEdgeRatio in [1.2,2.0]. Only a convex, connected outward TRI1 shell.
    //! One backend call targets 0.5 * maxVolume; independent acceptance uses maxVolume.
    //! Preview validates without invoking a mesher. Its valid result has no candidate model.
    static MeshCandidate preview(const RModel &source, const QJsonObject &arguments);
    //! Cancellation is cooperative before/after the generator and during validation.
    //! TetGen itself requires an isolated worker process to interrupt safely.
    static MeshCandidate generate(const RModel &source, const QJsonObject &arguments,
                                  const std::atomic_bool *cancel = nullptr);
    //! Verify an already loaded candidate without invoking TetGen or changing either model.
    //! Checks fresh physical IDs/allocators, cleared mappings and actual mesh/label quality.
    //! Requires outward TRI1 boundaries and opposite interior face orientation;
    //! never normalizes the corner order of a loaded artifact.
    static MeshCandidate verify(const RModel &source, const RModel &candidate,
                                const QJsonObject &arguments,
                                const std::atomic_bool *cancel = nullptr);
    static QJsonObject capabilities();
};

#endif
