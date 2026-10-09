#ifndef RPROJECT_CATALOG_H
#define RPROJECT_CATALOG_H

#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QLockFile>
#include <QThread>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>
#include <rml_model.h>

// Versioned, workspace-local identities and immutable native model snapshots.
// The catalog is single-thread-owned; mutating calls also use a workspace lock
// so separate CLI/GUI processes cannot overwrite each other's manifest.
class RProjectCatalog
{
public:
    static constexpr int StorageVersion = 1;

    struct SourceArtifact
    {
        QString path;
        QString sha256;

        QJsonObject toJson() const;
    };

    struct SourceBinding
    {
        // Canonical user-selected alias and the fully resolved terminal .ram/.rbm source chain.
        QString logicalSourcePath;
        QString resolvedSourcePath;
        QVector<SourceArtifact> sourceArtifacts;
        QString savedArtifactHash;

        bool operator==(const SourceBinding &other) const;
    };

    struct InstanceRef
    {
        QString documentId;
        QString sessionEpoch;
        quint64 documentRevision = 0;

        QJsonObject toJson() const;
    };

    class DocumentInstanceLease;
    using DocumentInstanceLeasePtr = std::shared_ptr<DocumentInstanceLease>;

    class PublicationLease
    {
    public:
        // Holds the cooperative workspace lock until the last shared owner releases it.
        ~PublicationLease();
        PublicationLease(const PublicationLease &) = delete;
        PublicationLease &operator=(const PublicationLease &) = delete;
        QString persistentDocumentId() const;
        QString expectedSourceRevision() const;
        QStringList reservedPaths() const;

    private:
        friend class RProjectCatalog;
        PublicationLease(QString projectId, QString workspaceRoot, QString lockPath,
                         QString documentId, QString sourceRevision, QStringList paths,
                         Qt::HANDLE owner, std::unique_ptr<QLockFile> lock,
                         DocumentInstanceLeasePtr instanceLease);
        QString projectIdentity;
        QString workspaceRoot;
        QString lockPath;
        QString documentIdentity;
        QString sourceRevision;
        QStringList paths;
        Qt::HANDLE ownerThread = nullptr;
        std::unique_ptr<QLockFile> lockFile;
        DocumentInstanceLeasePtr instanceLease;
    };
    using PublicationLeasePtr = std::shared_ptr<PublicationLease>;

    class DocumentInstanceLease
    {
    public:
        ~DocumentInstanceLease();
        DocumentInstanceLease(const DocumentInstanceLease &) = delete;
        DocumentInstanceLease &operator=(const DocumentInstanceLease &) = delete;
        QString persistentDocumentId() const;
        QString runtimeDocumentId() const;
        QString sessionEpoch() const;

    private:
        friend class RProjectCatalog;
        DocumentInstanceLease(QString projectId, QString workspaceRoot, QString lockPath,
                              QString documentId, InstanceRef instanceRef, Qt::HANDLE owner,
                              std::unique_ptr<QLockFile> lock);
        QString projectIdentity;
        QString workspaceRoot;
        QString lockPath;
        QString documentIdentity;
        InstanceRef runtimeInstance;
        Qt::HANDLE ownerThread = nullptr;
        std::unique_ptr<QLockFile> lockFile;
    };

    struct Document
    {
        QString persistentDocumentId;
        QString label;
        QString sourcePath;
        QString resolvedSourcePath;
        QVector<SourceArtifact> sourceArtifacts;
        QString originDocumentId;
        QString sourceRevision;
        QString savedArtifactHash;
        bool dirty = false;
        QJsonObject draftMetadata;
        bool tombstone = false;
        QString modelSha256;
        QString snapshotSourceRevision;
        QString snapshotRelativePath;
        QJsonObject snapshotInstanceRef;
        QJsonArray orphanSnapshots;
    };

    struct SnapshotArtifact
    {
        QString id;
        QString projectId;
        QString persistentDocumentId;
        QString sourceRevision;
        QString modelSha256;
        QString relativePath;
        InstanceRef instanceRef;

        QJsonObject toJson() const;
    };

    enum class OperationPhase { Intent, Accepted, ArtifactPublished, Observed, Unknown, Conflict };
    enum class OperationDisposition { Created, Reused, Conflict };

    // Only hashes and bounded identifiers are durable. Approval tokens and
    // normalized request bodies are never stored in the operations ledger.
    struct OperationAssociation
    {
        QString candidateSha256;
        QString approvalSummarySha256;
        QString acceptedRevision;
        QString acceptedModelSha256;
        QString diagnosticCode;
        // Original trusted-host observation used for exact idempotent replay.
        // It is persisted only after recursive size/depth/secret-key checks.
        QJsonObject observation;
    };

    struct OperationArtifactIntent
    {
        QString path;
        bool expectedPriorExists = false;
        QString expectedPriorSha256;
        // Empty until the caller has staged/serialized the exact bytes. An
        // empty intended hash can never be reconciled as a published artifact.
        QString intendedSha256;
        QString observedStatus;
        bool observedExists = false;
        QString observedSha256;
    };

    struct DurableOperation
    {
        QString projectId;
        QString ownerRuntimeId;
        QString operationId;
        QString idempotencyKey;
        QString normalizedRequestSha256;
        OperationPhase phase = OperationPhase::Intent;
        OperationAssociation association;
        QVector<OperationArtifactIntent> artifacts;
        QDateTime createdAtUtc;
        QDateTime updatedAtUtc;
    };

    struct BeginOperationResult
    {
        OperationDisposition disposition = OperationDisposition::Conflict;
        DurableOperation operation;
    };

    struct OperationPage
    {
        QVector<DurableOperation> operations;
        QString nextCursor;
    };

    struct CloseRetry
    {
        QString retryId;
        QString operationId;
        QString diagnosticCode;
        QString status;
        QJsonObject payload;
        QDateTime createdAtUtc;
    };

    static std::unique_ptr<RProjectCatalog> open(const QString &workspace,
                                                  QString *error = nullptr);
    static std::optional<SourceBinding> resolveSourceBinding(const QString &sourcePath,
                                                              QString *error = nullptr,
                                                              QString *errorCode = nullptr);
    // One runtime writer per persistent document; release by dropping the lease pointer.
    DocumentInstanceLeasePtr acquireDocumentInstance(const QString &persistentDocumentId,
                                                      const InstanceRef &instanceRef,
                                                      QString *error = nullptr,
                                                      QString *errorCode = nullptr);

    QString workspace() const;
    QString projectId() const;
    QString runtimeId() const;
    QJsonObject query(QString *error = nullptr) const;
    std::optional<Document> document(const QString &persistentDocumentId,
                                     QString *error = nullptr) const;

    // Existing active source aliases reuse their identity. Changed bytes at an
    // existing alias are rejected for explicit reload handling by the host.
    QString registerDocument(const QString &sourcePath, const QString &label,
                             const QString &originDocumentId = QString(),
                             QString *error = nullptr, QString *errorCode = nullptr);
    QString registerDocument(const SourceBinding &sourceBinding, const QString &label,
                             const QString &originDocumentId = QString(),
                             QString *error = nullptr, QString *errorCode = nullptr);
    // Creates a new unsaved persistent identity; the first recordSave keeps it.
    QString createDocument(const QString &label,
                           const QString &originDocumentId = QString(),
                           QString *error = nullptr);
    // Creates an unsaved identity linked to an active origin document.
    QString createCopy(const QString &originDocumentId, const QString &label,
                       QString *error = nullptr);
    bool recordAccepted(const QString &persistentDocumentId, const QString &sourceRevision,
                        bool dirty = false, const QJsonObject &draftMetadata = {},
                        QString *error = nullptr,
                        const QString &expectedSourceRevision = QString(),
                        QString *errorCode = nullptr,
                        const DocumentInstanceLeasePtr &instanceLease = {});
    bool renameDocument(const QString &persistentDocumentId, const QString &label,
                        QString *error = nullptr,
                        const QString &expectedSourceRevision = QString(),
                        QString *errorCode = nullptr,
                        const DocumentInstanceLeasePtr &instanceLease = {});
    bool recordSave(const QString &persistentDocumentId, const QString &sourcePath,
                    QString *error = nullptr,
                    const QString &expectedSourceRevision = QString(),
                    QString *errorCode = nullptr,
                    const DocumentInstanceLeasePtr &instanceLease = {});
    bool recordSave(const QString &persistentDocumentId, const QString &sourcePath,
                    const PublicationLeasePtr &lease, const QString &expectedSourceRevision,
                    QString *error = nullptr, QString *errorCode = nullptr,
                    const DocumentInstanceLeasePtr &instanceLease = {});
    // Commits only if the actual native source chain still exactly matches the
    // worker's precomputed written binding while the publication lease is held.
    bool recordSave(const QString &persistentDocumentId,
                    const SourceBinding &expectedWrittenBinding,
                    const PublicationLeasePtr &lease,
                    const QString &expectedSourceRevision,
                    QString *error = nullptr, QString *errorCode = nullptr,
                    const DocumentInstanceLeasePtr &instanceLease = {});
    // Acquire on the catalog owner thread before worker IO; recordSave must be called on owner thread.
    PublicationLeasePtr reservePublication(const QString &persistentDocumentId,
                                           const QStringList &actualOutputPaths,
                                           const QString &expectedSourceRevision,
                                           QString *error = nullptr,
                                           QString *errorCode = nullptr,
                                           const DocumentInstanceLeasePtr &instanceLease = {});
    bool acceptReload(const QString &persistentDocumentId, const QString &sourcePath,
                      const QString &newSourceRevision, QString *error = nullptr,
                      const QString &expectedSourceRevision = QString(),
                      QString *errorCode = nullptr,
                      bool *reopenedSavedSource = nullptr,
                      const DocumentInstanceLeasePtr &instanceLease = {});
    bool acceptReload(const SourceBinding &sourceBinding, const QString &persistentDocumentId,
                      const QString &newSourceRevision, QString *error = nullptr,
                      const QString &expectedSourceRevision = QString(),
                      QString *errorCode = nullptr,
                      bool *reopenedSavedSource = nullptr,
                      const DocumentInstanceLeasePtr &instanceLease = {});
    bool tombstone(const QString &persistentDocumentId, QString *error = nullptr,
                   QString *errorCode = nullptr,
                   const DocumentInstanceLeasePtr &instanceLease = {});
    bool captureSnapshot(const QString &persistentDocumentId, const RModel &detachedModel,
                         const InstanceRef &instanceRef, SnapshotArtifact *artifact,
                         QString *error = nullptr,
                         const QString &expectedSourceRevision = QString(),
                         QString *errorCode = nullptr,
                         const DocumentInstanceLeasePtr &instanceLease = {});

    // Project-scoped durable operation history. The request body is hashed
    // and discarded; only typed, bounded association metadata is retained.
    BeginOperationResult beginOperation(const QString &idempotencyKey,
                                        const QJsonObject &normalizedRequest,
                                        const OperationAssociation &association = {},
                                        const PublicationLeasePtr &publicationLease = {},
                                        QString *error = nullptr,
                                        QString *errorCode = nullptr);
    bool advanceOperation(const QString &operationId, OperationPhase phase,
                          const OperationAssociation &association = {},
                          const PublicationLeasePtr &publicationLease = {},
                          QString *error = nullptr,
                          QString *errorCode = nullptr);
    // Call once for a newly created trusted runtime. Any prior unfinished
    // operation becomes unknown; this never retries or infers a side effect.
    bool markUnfinishedOperationsUnknown(
        const PublicationLeasePtr &publicationLease = {},
        QString *error = nullptr, QString *errorCode = nullptr);
    std::optional<DurableOperation> queryOperation(
        const QString &operationId, const PublicationLeasePtr &publicationLease = {},
        QString *error = nullptr,
        QString *errorCode = nullptr) const;
    std::optional<DurableOperation> queryOperationByIdempotencyKey(
        const QString &idempotencyKey,
        const PublicationLeasePtr &publicationLease = {},
        QString *error = nullptr, QString *errorCode = nullptr) const;
    OperationPage queryOperations(int limit = 100, const QString &afterOperationId = {},
                                  const PublicationLeasePtr &publicationLease = {},
                                  QString *error = nullptr,
                                  QString *errorCode = nullptr) const;

    // Persist a multi-file GUI save intent before writes. expected prior
    // hashes are checked under the existing workspace publication lease.
    BeginOperationResult beginSavePublication(
        const QString &persistentDocumentId, const QString &idempotencyKey,
        const QJsonObject &normalizedRequest, const QString &expectedSourceRevision,
        const QVector<OperationArtifactIntent> &artifacts,
        const PublicationLeasePtr &publicationLease,
        const OperationAssociation &association = {},
        QString *error = nullptr, QString *errorCode = nullptr);
    bool bindSaveArtifactHash(const QString &operationId, const QString &path,
                              const QString &intendedSha256,
                              const PublicationLeasePtr &publicationLease,
                              QString *error = nullptr,
                              QString *errorCode = nullptr);
    bool reconcileSavePublication(const QString &operationId,
                                  const PublicationLeasePtr &publicationLease = {},
                                  QString *error = nullptr,
                                  QString *errorCode = nullptr);

    // Durable retry queue for close-time bookkeeping that could not be
    // completed. The exact close record is retained with its predecessor hash.
    bool queueCloseRetry(const QString &retryId, const QString &operationId,
                         const QString &diagnosticCode,
                         const QJsonObject &payload,
                         const PublicationLeasePtr &publicationLease = {},
                         QString *error = nullptr, QString *errorCode = nullptr);
    QVector<CloseRetry> pendingCloseRetries(
                                            const PublicationLeasePtr &publicationLease = {},
                                            QString *error = nullptr,
                                            QString *errorCode = nullptr) const;
    bool completeCloseRetry(const QString &retryId,
                            const PublicationLeasePtr &publicationLease = {},
                            QString *error = nullptr,
                            QString *errorCode = nullptr);

    static QJsonObject capabilityDescription();

private:
    RProjectCatalog(QString canonicalWorkspace, QString projectId, QString runtimeId,
                    std::unique_ptr<QLockFile> runtimeLease);
    bool validateInstanceLease(const QString &persistentDocumentId, const InstanceRef *instanceRef,
                              const DocumentInstanceLeasePtr &lease,
                              QString *error, QString *errorCode) const;
    bool validatePublicationLease(const PublicationLeasePtr &lease,
                                  QString *error, QString *errorCode) const;

    QString root;
    QString projectIdentity;
    QString runtimeIdentity;
    std::unique_ptr<QLockFile> runtimeLease;
    Qt::HANDLE ownerThread = nullptr;
};

#endif
