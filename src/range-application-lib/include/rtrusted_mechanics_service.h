#ifndef RTRUSTED_MECHANICS_SERVICE_H
#define RTRUSTED_MECHANICS_SERVICE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <rml_model.h>

//! Versioned, shared application boundary for the first trusted rod/beam lane.
//! It builds detached candidates; the caller owns source revision checks,
//! approval, solving, and publication.
class RTrustedMechanicsService
{
public:
    struct ValidationResult
    {
        bool valid = false;
        QJsonObject normalizedSpec;
        QJsonArray issues;
    };

    struct CandidateResult
    {
        bool valid = false;
        QString code;
        QString message;
        RModel model;
        QJsonObject normalizedSpec;
        QJsonObject inspection;
    };

    static constexpr int SchemaVersion = 1;

    //! Validate dimensions and required physics, then normalize all quantities to SI.
    static ValidationResult validate(const QJsonObject &spec);

    //! Build a detached TRUSS1 rod or BEAM1 cantilever candidate from a valid spec.
    static CandidateResult createCandidate(const QJsonObject &spec,
                                           const QString &sourceId,
                                           const QString &modelRevision);

    //! Implements analysis.inspect: return physics, mesh, analysis, selector, source, and result identity.
    static QJsonObject inspect(const RModel &model,
                               const QString &sourceId,
                               const QString &modelRevision,
                               const QJsonObject &normalizedSpec = {},
                               const QJsonObject &jobEvidence = {});

    //! Re-resolve semantic endpoint/all-node/coordinate selectors against this revision.
    static QJsonObject resolveSelector(const RModel &model,
                                       const QJsonObject &selector,
                                       const QString &requestedRevision,
                                       const QString &currentRevision);

    //! Stable summaries used to prove numeric repair did not change the physics.
    static QJsonObject physicalSummary(const QJsonObject &normalizedSpec);
    static QJsonObject numericalSummary(const QJsonObject &normalizedSpec);
    static QString summaryDigest(const QJsonObject &summary);

    //! Implements analysis.diagnose. It never edits the model or changes physics.
    static QJsonObject diagnose(const QJsonObject &spec,
                               const RModel *model,
                               const QString &sourceId,
                               const QString &modelRevision,
                               const QJsonObject &jobEvidence = {});

    //! Bind a failed job's log/error code and model snapshot identity into a diagnosis package.
    static QJsonObject diagnoseFailure(const QJsonObject &spec,
                                       const RModel *model,
                                       const QString &sourceId,
                                       const QString &modelRevision,
                                       const QString &jobId,
                                       const QString &errorCode,
                                       const QString &solverLog);
};

#endif
