#ifndef RTRUSTED_VERIFICATION_SERVICE_H
#define RTRUSTED_VERIFICATION_SERVICE_H

#include <QJsonObject>
#include <QString>

//! Independent, read-only verification of the Stage 1 SI rod/beam lane.
class RTrustedVerificationService
{
public:
    //! expectedIdentity requires artifactId, jobId, inputSnapshotSha256,
    //! inputPackageSha256 and the complete sourceSnapshotRef including its
    //! pre-solve modelSha256. Result values are read only through the
    //! hash-bound native broker; callers cannot supply numerical evidence.
    //! Every report metric has pass/fail/notComputed/notApplicable status.
    static QJsonObject verify(const QJsonObject &problemSpec,
                              const QJsonObject &expectedIdentity,
                              const QString &manifestPath,
                              const QString &expectedManifestSha256,
                              bool includeConvergenceMeasurements = false);
};

#endif
