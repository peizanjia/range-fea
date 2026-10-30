#ifndef RNATIVE_RESULT_BROKER_H
#define RNATIVE_RESULT_BROKER_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>
#include <memory>

class RModel;

//! Read-only publication and query boundary for verified native FE results.
class RNativeResultBroker
{
public:
    struct PublishResult
    {
        bool valid = false;
        QString code;
        QString message;
        QString manifestPath;
        QString manifestSha256;
        QString artifactId;
        QJsonObject manifest;
    };

    //! One actual parsed MODEL from a fully revalidated native result.
    //! Static/modal results use originKind "terminal" because their manifest
    //! records terminal identity directly and has no frame-origin field.
    struct VerifiedRecord
    {
        int recordNo = -1;
        double timeS = 0.0;
        QString modelSha256;
        QString originKind;
        std::shared_ptr<RModel> model;
    };

    //! Typed, path-free result of reading the models behind a hash-bound
    //! manifest. Failure results never contain partially trusted records.
    struct VerifiedModelsResult
    {
        bool valid = false;
        QString code;
        QString message;
        QString manifestSha256;
        QString artifactId;
        QString jobId;
        QString inputSnapshotSha256;
        QString inputPackageSha256;
        QJsonObject sourceSnapshotRef;
        QString sourceKind;
        QString analysisType;
        QVector<VerifiedRecord> records;
    };

    //! Validate a completed native result directory and atomically publish its
    //! immutable range.native-results/v1 manifest.
    static PublishResult publishResult(const QString &resultDirectory,
                                       const QString &latestArtifactPath,
                                       const QJsonObject &provenance);

    //! Read a hash-bound manifest and return one bounded, read-only query.
    //! snapshot accepts only channelIds (1..16 unique existing IDs), reads all
    //! persisted MODEL frames once, and returns a path-free compact DTO bounded
    //! to 10000 frames and 32 MiB. It requires a completed manifest.
    //! Revalidation includes at most 100000 unique current/ancestor artifacts.
    //! Controlled dynamics also expose control:work, the actual accepted
    //! checkpoint's summed actuator work in physical joules, via snapshot only.
    static QJsonObject query(const QString &manifestPath,
                             const QString &expectedSha256,
                             const QString &operation,
                             const QJsonObject &arguments);

    //! Return the actual terminal MODEL for static/modal manifests, or every
    //! accepted persisted MODEL frame for transient manifests. All paths,
    //! hashes, ancestry, times, and physical node identities are validated
    //! before a success DTO is returned.
    static VerifiedModelsResult readVerifiedModels(const QString &manifestPath,
                                                   const QString &expectedManifestSha256);

    //! Query only complete journal frames supplied by the trusted job manager.
    //! No success manifest is published; every frame is revalidated per query.
    static QJsonObject queryLive(const QString &resultDirectory,
                                const QJsonObject &provenance,
                                const QJsonArray &frames,
                                const QString &operation,
                                const QJsonObject &arguments);
};

#endif // RNATIVE_RESULT_BROKER_H
