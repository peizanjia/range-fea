#ifndef RTRUSTED_EXPERIMENT_SERVICE_H
#define RTRUSTED_EXPERIMENT_SERVICE_H

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>

class QLockFile;

//! Durable, host-neutral transaction core for bounded trusted experiments.
//!
//! This service owns the authorization and state record. It deliberately does
//! not schedule a solver or publish a candidate; a Host executor must use the
//! returned immutable authorization object when creating a detached solve.
class RTrustedExperimentService
{
public:
    static constexpr int SchemaVersion = 1;
    static constexpr int HardSolveLimit = 8;
    static constexpr int HardRepairLimit = 3;
    static constexpr int HardModelTurnLimit = 12;

    //! Canonical lifecycle values serialized by the trusted experiment Host.
    static const QStringList &lifecycleStates();

    struct Outcome
    {
        bool ok = false;
        QString code;
        QString message;
        QJsonObject data;
    };

    //! Values must come from the current Host/Session source snapshot.
    struct SourceIdentity
    {
        QString workspaceId;
        QString sessionId;
        QString sessionEpoch;
        QString documentId;
        QString documentRevision;
        QString snapshotSha256;

        QJsonObject toJson() const;
        bool isValid() const;
    };

    explicit RTrustedExperimentService(const QString &storageDirectory);

    //! Mint and durably store an authorization after exact source comparison.
    //! Request schema:
    //! {schemaVersion, sourceBinding, problemSpec, allowedNumericalRanges?, budgets?, stopConditions?}.
    //! A caller-supplied `authorized` field is rejected as an unknown field.
    Outcome authorize(const QJsonObject &request, const SourceIdentity &currentSource);

    //! Build the Host review payload without creating a durable experiment record.
    Outcome previewAuthorization(const QJsonObject &request, const SourceIdentity &currentSource);

    //! Atomically persist the exact authorization previously shown for Host
    //! approval as a prepared transaction. The caller must provide the preview
    //! identity and authorization unchanged.
    Outcome authorizePreview(const QJsonObject &request,
                             const SourceIdentity &currentSource,
                             const QString &experimentId,
                             const QJsonObject &expectedAuthorization);

    //! Materialize the authorized transaction. Repeated calls are idempotent.
    Outcome create(const QString &experimentId, const SourceIdentity &currentSource);

    //! Persist the one-shot run request. Repeated calls never schedule another run.
    Outcome run(const QString &experimentId, const SourceIdentity &currentSource);

    //! Read the durable record without changing it.
    Outcome query(const QString &experimentId) const;

    //! Persist cancellation before the Host stops the current child operation.
    Outcome cancel(const QString &experimentId);

    //! Host-executor state transitions. Every mutating transition rechecks the
    //! source binding and cancellation flag and persists before returning.
    Outcome beginModelTurn(const QString &experimentId, const SourceIdentity &currentSource);
    Outcome beginSolve(const QString &experimentId,
                       const SourceIdentity &currentSource,
                       const QJsonObject &problemSpec,
                       const QString &inputSnapshotSha256,
                       const QString &inputPackageSha256);
    //! Durably consume the active attempt immediately before child registration.
    //! A claimed attempt is never eligible for a second submission.
    Outcome claimSolveSubmission(const QString &experimentId,
                                 const SourceIdentity &currentSource,
                                 const QString &attemptId);
    Outcome finishSolve(const QString &experimentId,
                        const SourceIdentity &currentSource,
                        const QString &attemptId,
                        bool succeeded,
                        const QJsonObject &solveIdentity = {},
                        const QString &failureDisposition = QString(),
                        const QString &failureCode = QString(),
                        const QString &failureMessage = QString());
    Outcome beginRepair(const QString &experimentId,
                        const SourceIdentity &currentSource,
                        const QJsonObject &repairedProblemSpec);
    Outcome finishRepair(const QString &experimentId,
                         const SourceIdentity &currentSource,
                         const QJsonObject &repairedProblemSpec,
                         const QJsonObject &observedPhysicalSummary);
    Outcome markNeedsConfirmation(const QString &experimentId,
                                  const SourceIdentity &currentSource,
                                  const QString &reason,
                                  const QJsonObject &evidence = {});
    Outcome stop(const QString &experimentId,
                 const SourceIdentity &currentSource,
                 const QString &code,
                 const QString &message,
                 const QJsonObject &evidence = {});

    //! Calls the Stage 3 verifier itself; no public API accepts a claimed report.
    Outcome verifyCandidate(const QString &experimentId,
                            const SourceIdentity &currentSource,
                            const QJsonObject &expectedIdentity,
                            const QString &manifestPath,
                            const QString &manifestSha256,
                            const QJsonObject &costEvidence = {});

    //! Return an authorized uniform mesh refinement only for measured precision failures.
    static bool proposeAuthorizedMeshRefinement(const QJsonObject &problemSpec,
                                                const QJsonObject &authorization,
                                                const QJsonObject &verificationReport,
                                                QJsonObject *repairedProblemSpec,
                                                QString *reason = nullptr);

private:
    Outcome authorizeInternal(const QJsonObject &request,
                              const SourceIdentity &currentSource,
                              const QString &experimentId,
                              const QJsonObject *expectedAuthorization,
                              bool persist);
    QString recordPath(const QString &experimentId) const;
    bool loadRecord(const QString &experimentId, QJsonObject *record, QString *error) const;
    bool saveRecord(const QString &experimentId, const QJsonObject &record, QString *error) const;
    bool lockRecord(const QString &experimentId, QLockFile *lock, QString *error) const;
    bool sourceMatches(const QJsonObject &record, const SourceIdentity &currentSource) const;
    bool authorizeMutation(QJsonObject *record,
                           const SourceIdentity &currentSource,
                           QString *errorCode,
                           QString *errorMessage) const;
    static bool isExperimentId(const QString &experimentId);
    static Outcome success(const QJsonObject &data = {});
    static Outcome failure(const QString &code, const QString &message, const QJsonObject &data = {});

    QString storageDirectory;
    mutable QMutex mutex;
};

#endif
