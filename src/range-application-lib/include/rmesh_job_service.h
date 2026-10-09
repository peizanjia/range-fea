#ifndef RMESH_JOB_SERVICE_H
#define RMESH_JOB_SERVICE_H

#include <atomic>
#include <QJsonObject>
#include <rml_model.h>

//! Detached mesh jobs. This class has no Session, approval or acceptance authority.
class RMeshJobService
{
public:
    struct Result
    {
        bool valid = false;
        QString code;
        QString message;
        QJsonObject descriptor;
        RModel candidate; //!< Present only after independent artifact verification.
        QJsonObject toJson() const;
    };

    //! Exact input fields: schemaVersion=1, mode="closed_surface_tetrahedra",
    //! attemptId (UUID), sourceSnapshotPath/Sha256, options, runtimeContext,
    //! workerPath/Sha256, jobDirectory (new UUID directory under an existing parent).
    //! runtimeContext exact keys: authority, persistentModelPath/Sha256,
    //! runtimeSnapshotPath/Sha256, sourceSnapshotRef, sourceRuntimeStamp.
    //! prepare freezes normalized options and hashes, creates request.json exclusively.
    static Result prepare(const QJsonObject &request);
    //! Fixed worker entry. actualWorkerPath must be the process's appFilePath.
    //! Replayed, interrupted or partial attempts are never regenerated automatically.
    static Result runWorker(const QJsonObject &frozenRequest, const QString &actualWorkerPath,
                            const std::atomic_bool *cancel = nullptr);
    //! Rereads native MODEL bytes and checks source/context/worker/artifact/report hashes,
    //! independently recomputes mesh quality/IDs/labels/binding invalidation.
    static Result verifyCandidate(const QJsonObject &frozenRequest);
    //! Read-only reconciliation. A complete verified receipt is candidate_ready only;
    //! absence, corruption, cancellation and partial files never become success.
    static Result queryReconcile(const QJsonObject &frozenRequest);
    static QString fileSha256(const QString &path);
    static QString objectSha256(const QJsonObject &object);
};

#endif
