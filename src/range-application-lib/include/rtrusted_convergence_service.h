#ifndef RTRUSTED_CONVERGENCE_SERVICE_H
#define RTRUSTED_CONVERGENCE_SERVICE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QByteArray>
#include <QString>
#include <QtGlobal>

//! Exact compact JSON serialization shared by experiment persistence and preflight sizing.
namespace RTrustedRecordSerialization
{
QByteArray compact(const QJsonObject &object);
qsizetype maximumTransientRecordBytes(const QJsonObject &problemSpec,
                                      int frames,
                                      int displacementQois);
}

//! Deterministic planning and comparison primitives for bounded convergence studies.
class RTrustedConvergenceService
{
public:
    struct Outcome
    {
        bool ok = false;
        QString code;
        QString message;
        QJsonObject data;
    };

    static constexpr double FixedRelativeTolerance = 0.01;
    static constexpr int MaximumLevels = 4;
    static constexpr int MaximumReferenceDofs = 192;
    static constexpr int MaximumTransientFrames = 10000;
    static constexpr qsizetype MaximumSelectorIdentityBytes = 256;
    static constexpr qsizetype MaximumConvergenceMeasurementBytes = 64 * 1024;
    static constexpr qsizetype MaximumDurableRecordBytes = 4 * 1024 * 1024;

    //! Exact compact-JSON size of the maximum permitted durable record shape.
    static qsizetype estimateMaximumTransientRecordBytes(int frames, int displacementQois);

    //! The same maximum-shape calculation with the actual ProblemSpec QOI selectors.
    static qsizetype estimateMaximumTransientRecordBytes(const QJsonObject &problemSpec,
                                                         int frames,
                                                         int displacementQois);

    //! Study has exactly {kind, maximumLevels}; level zero is the supplied baseline.
    static Outcome validateStudy(const QJsonObject &problemSpec, const QJsonObject &study);

    //! Produce a raw, mechanics-valid candidate by changing only mesh or time-grid fields.
    //! levelIndex is one-based: 1 is the first refinement after the baseline.
    static Outcome proposeLevel(const QJsonObject &baselineSpec,
                                const QJsonObject &study,
                                int levelIndex,
                                QJsonObject *candidateSpec);

    //! Compare two verified measurement objects. `kind` may be mesh/timeStep or an analysis kind.
    static Outcome compareMeasurements(const QString &kind,
                                      const QJsonObject &previous,
                                      const QJsonObject &current);

    //! Requires the last `consecutiveRequired` valid adjacent comparisons to meet the fixed 1% rule.
    static bool isConverged(const QJsonArray &adjacentChanges, int consecutiveRequired = 2);
};

#endif
