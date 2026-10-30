#ifndef RAGENT_EVALUATION_CAPTURE_STORE_H
#define RAGENT_EVALUATION_CAPTURE_STORE_H

#include <QByteArray>
#include <QJsonObject>
#include <QMutex>
#include <QString>

// Bounded append-only JSONL capture for explicitly selected evaluation runs.
// It refuses an existing file, detects prefix changes, and never rewrites prior
// bytes. Hashes provide tamper evidence, not protection from deliberate edits.
class RAgentEvaluationCaptureStore final
{
public:
    static constexpr int MaxEvents = 512;
    static constexpr qint64 MaxTotalBytes = 8LL * 1024 * 1024;
    static constexpr int MaxEventBytes = 128 * 1024;

    // Creates a new empty ledger at the explicit path. Existing files are
    // rejected and never overwritten.
    bool configure(const QString &path, QString *error = nullptr);

    // Appends one redacted event to the configured ledger.
    bool append(const QJsonObject &event, QString *error = nullptr);

private:
    QMutex mutex;
    QString ledgerPath;
    QByteArray expectedPrefix;
    QString previousHash;
    int nextSequence = 1;
    int eventCount = 0;
    bool configured = false;
};

#endif // RAGENT_EVALUATION_CAPTURE_STORE_H
