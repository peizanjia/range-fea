#ifndef RNATIVE_RESULT_BROKER_H
#define RNATIVE_RESULT_BROKER_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>

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

    //! Validate a completed native result directory and atomically publish its
    //! immutable range.native-results/v1 manifest.
    static PublishResult publishResult(const QString &resultDirectory,
                                       const QString &latestArtifactPath,
                                       const QJsonObject &provenance);

    //! Read a hash-bound manifest and return one bounded, read-only query.
    static QJsonObject query(const QString &manifestPath,
                             const QString &expectedSha256,
                             const QString &operation,
                             const QJsonObject &arguments);

    //! Query only complete journal frames supplied by the trusted job manager.
    //! No success manifest is published; every frame is revalidated per query.
    static QJsonObject queryLive(const QString &resultDirectory,
                                const QJsonObject &provenance,
                                const QJsonArray &frames,
                                const QString &operation,
                                const QJsonObject &arguments);
};

#endif // RNATIVE_RESULT_BROKER_H
