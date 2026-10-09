#ifndef RAGENT_CONVERSATION_STORE_H
#define RAGENT_CONVERSATION_STORE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Project-bound, bounded persistence for observable Agent messages and
// accepted host facts. This class never stores runner/provider history,
// system or tool messages, private reasoning, approval state, or credentials.
class RAgentConversationStore
{
public:
    static constexpr int StorageVersion = 1;
    static constexpr int MaxMessagesPerConversation = 512;
    static constexpr int MaxTextBytesPerMessage = 16 * 1024;
    static constexpr int MaxLiveConversationsPerProject = 32;
    static constexpr int MaxPageSize = 50;

    bool configure(const QString &workspace, const QString &projectId,
                   QString *error = nullptr);

    // Literal values are kept only in this instance's memory and redacted
    // from visible user/assistant text before any durable write.
    void setSensitiveValues(const QStringList &values);

    // Observable entries require {messageId: UUID, role: user|assistant|fact,
    // text}; optional taskId is a UUID. A fact role also requires a scalar
    // `facts` object whose only accepted keys are status, code, command,
    // operationId, jobId, sourceDocumentId, sourceRevision,
    // acceptedModelSha256, idempotencyKey, requestSha256, and
    // workflowProjectId. The last three are historical lookup references
    // only; they carry no approval or execution authority. Each pending/unknown
    // fact lookup reference reserves capacity until a later accepted/rejected
    // fact with the same task and reference is appended. Optional sourceRefs use
    // {sourceId, revision, sha256}; artifactRefs use {artifactId, sha256}.
    // Branch provenance uses branchOrigin {conversationId, messageId}.
    //
    // create/append/branch return a summary with a decimal-string revision.
    // list returns {schema, projectId, offset, limit, totalCount,
    // conversations, diagnostics}. page returns the summary fields plus
    // {offset, messages, hasMore}. exportConversation returns a bounded full
    // observable transcript with project and conversation identity.
    QJsonObject create(const QString &title, QString *error = nullptr);
    QJsonObject append(const QString &conversationId, const QString &expectedRevision,
                       const QJsonArray &entries, QString *error = nullptr);
    QJsonObject list(int offset, int limit, QString *error = nullptr) const;
    QJsonObject page(const QString &conversationId, int offset, int limit,
                     QString *error = nullptr) const;
    QJsonObject branch(const QString &conversationId, const QString &upToMessageId,
                       const QString &title, QString *error = nullptr);
    QJsonObject exportConversation(const QString &conversationId,
                                  QString *error = nullptr) const;
    bool remove(const QString &conversationId, const QString &expectedRevision,
                QString *error = nullptr);

    QString workspace() const;
    QString projectId() const;

private:
    QString workspaceRoot;
    QString projectIdentity;
    QStringList sensitiveLiterals;
};

#endif // RAGENT_CONVERSATION_STORE_H
