#ifndef RPROJECT_CATALOG_H
#define RPROJECT_CATALOG_H

#include <QJsonArray>
#include <QJsonObject>
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

    static QJsonObject capabilityDescription();

private:
    RProjectCatalog(QString canonicalWorkspace, QString projectId);
    bool validateInstanceLease(const QString &persistentDocumentId, const InstanceRef *instanceRef,
                              const DocumentInstanceLeasePtr &lease,
                              QString *error, QString *errorCode) const;

    QString root;
    QString projectIdentity;
    Qt::HANDLE ownerThread = nullptr;
};

#endif
