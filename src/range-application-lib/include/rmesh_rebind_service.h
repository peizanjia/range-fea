#ifndef RMESH_REBIND_SERVICE_H
#define RMESH_REBIND_SERVICE_H

#include <QJsonObject>
#include <rml_model.h>

//! Detached WF14 static binding. No Session, publication, or approval authority.
class RMeshRebindService
{
public:
    struct RebindCandidate
    {
        bool valid = false;
        QString code;
        QString message;
        RModel candidate; //!< Empty on failure; source is never modified.
        QJsonObject normalizedArguments;
        QString sourceGeometrySha256;
        QString postGeometrySha256;
        //! Existing model-command supported-state fingerprints, not native-file hashes.
        QString sourceStateSha256;
        QString postStateSha256;
        QJsonObject report;
    };

    //! Exact schema: unitSI=true, staticStress=true,
    //! material={youngModulusPa,poissonRatio,densityKgM3},
    //! constraints=[{nodePhysicalId,values:[number|null,number|null,number|null]}],
    //! loads=[{nodePhysicalId,forceN:[number,number,number]}].
    //! Only an unbound, face-connected TETRA1 mesh with its exact convex TRI1
    //! boundary, checked through the shared shell-only mesh preview (no mesher).
    //! Requires full default-reset setup, allowing only the two exact bbox sizing
    //! seeds derived by GUI mesh initialization; candidate setup is fully reset.
    //! Creates fresh single-node POINT targets using the source's current allocators.
    static RebindCandidate preview(const RModel &source, const QJsonObject &arguments);
    //! Read-only proof that this bounded static tetrahedral model's POINT
    //! targets and planar TRI1 exterior add no material-dependent stiffness,
    //! mass or loads. Reuses the full rebind geometry guard; never changes state.
    static bool hasMaterialFreeStaticBoundaryRoles(const RModel &model);
    static QJsonObject capabilities();
};

#endif
