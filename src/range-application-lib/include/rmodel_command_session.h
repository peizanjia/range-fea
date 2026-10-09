#ifndef RMODEL_COMMAND_SESSION_H
#define RMODEL_COMMAND_SESSION_H

#include <QJsonObject>
#include <QJsonArray>
#include <QThread>
#include <QString>
#include <memory>
#include <rml_model.h>
#include <rmodel_command_service.h>

// Single-owner editing session with an optional durable interaction journal.
// The input model is copied; callers publish the accepted model separately.
// Tokens are session-local, not file hashes.
class RModelCommandSession
{
public:
    static constexpr int MaxCommands = 256;

    enum class Lifecycle { Ready, Running, Closed };

    // Trusted host configuration. An empty path disables durable journaling.
    // The actor and source fields are supplied by the host, never by a command.
    struct JournalOptions
    {
        QString path;
        QString actorId = QStringLiteral("trusted-host");
        QString sourcePath;
        QString sourceSha256;
    };

    explicit RModelCommandSession(const RModel &model);
    RModelCommandSession(const RModel &model, JournalOptions journal);
    ~RModelCommandSession();
    RModelCommandSession(const RModelCommandSession &) = delete;
    RModelCommandSession &operator=(const RModelCommandSession &) = delete;

    const RModel &model() const;
    QJsonObject query() const;
    QJsonObject capabilities() const;
    QJsonObject context(const QString &sourceId, const QJsonObject &pageRequest = {}) const;
    QJsonObject targets(const QString &sourceId, const QJsonObject &pageRequest = {}) const;
    QJsonObject execute(const QJsonObject &request);
    // Trusted-host lifecycle transitions. A failed journal append returns false
    // without changing lifecycle. Repeating the current transition is a no-op.
    bool setRunning(bool running);
    bool close();
    // Build the exact close interaction record and its expected journal
    // predecessor without appending it. Persist the returned payload through
    // the project operations close queue before calling closeWithRetryPayload.
    QJsonObject prepareCloseRetry(const QString &retryId, const QString &operationId,
                                  QString *error = nullptr) const;
    bool closeWithRetryPayload(const QJsonObject &payload, QString *error = nullptr);
    static bool validateCloseRetryPayload(const QJsonObject &payload,
                                          QString *error = nullptr,
                                          QString *errorCode = nullptr);
    // Replays the exact close record only when the journal has its captured
    // predecessor or already contains the same recordId and bytes.
    static bool replayCloseRetry(const QJsonObject &payload,
                                 QString *error = nullptr,
                                 QString *errorCode = nullptr);
    // Called AFTER a trusted host changes its live model. The old identity is
    // closed even on audit failure because its snapshot is already stale.
    bool recordExternalChange(const QString &reason, const RModel &changedModel,
                              QString *error = nullptr);
    // Persist destination and actual staged bytes BEFORE committing the output.
    bool recordPublicationIntent(const QString &outputPath, const QString &stagedPath,
                                 QString *error = nullptr);
    // Reconcile at most 256 unfinished intents in a fresh process. This observes
    // files and records outcomes; it never republishes or reconstructs a model.
    static QJsonObject reconcilePublications(const JournalOptions &journal);
    // Call after the host has atomically published a native model file. The
    // method hashes the actual file bytes before writing the publication event.
    bool recordArtifactPublished(const QString &outputPath, QString *outputSha256 = nullptr,
                                 QString *error = nullptr);
    static QJsonObject modalParameters(const RModalSetup &setup);

private:
    struct State;
    std::unique_ptr<State> state;
    QString identity;
    Qt::HANDLE ownerThread;
    QString revision() const;
    bool appendJournalEvent(const QString &commandId, const QString &resultStatus,
                            const QString &modelRevision, const QString &preStateHash,
                            const QString &postStateHash, const QJsonObject &parameters,
                            const QJsonObject &selectionSnapshot, const QJsonArray &artifactRefs,
                            const QJsonArray &diagnostics, QString *error = nullptr) const;
};

#endif
