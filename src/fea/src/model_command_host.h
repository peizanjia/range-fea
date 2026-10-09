#ifndef MODEL_COMMAND_HOST_H
#define MODEL_COMMAND_HOST_H

#include <QObject>
#include <QJsonObject>
#include <QList>
#include <memory>
#include <rml_modal_setup.h>
#include <rproject_catalog.h>

class Session;

// GUI-thread owner of persistent command snapshots. No Model pointer survives a
// call: QList<Model> can move when another document is opened or closed.
class ModelCommandHost : public QObject
{
    Q_OBJECT
public:
    explicit ModelCommandHost(Session *session);
    ~ModelCommandHost() override;
    QJsonObject query(uint modelID);
    QJsonObject execute(uint modelID, const QJsonObject &request);
    //! Versioned GUI facade for read queries and approved modal/history commands.
    //! Requests and callbacks are currently confined to the GUI thread.
    QJsonObject handle(const QJsonObject &request);
    //! Trusted GUI approval boundary. This is deliberately not a handle operation.
    bool approvePreview(const QString &previewId);
    bool configureModal(const QList<uint> &modelIDs, const RModalSetup &setup, QString *error = nullptr);
    bool recordArtifactPublished(uint modelID, const QString &path, QString *error = nullptr,
                                 const RProjectCatalog::PublicationLeasePtr &publicationLease = {});
    //! Audit a completed legacy GUI mutation and retire its old command identity.
    bool recordExternalChange(uint modelID, const QString &reason, QString *error = nullptr);
    QString journalPath() const;
    //! Runtime session namespace. Persistent project/document identities are separate.
    QString sessionEpoch() const;
    bool closeDurableSessions(QString *error = nullptr);
    static bool isBusy();
    static bool isMutationBlocked();

signals:
    void commandRejected(const QString &message);
    void eventPublished(const QJsonObject &event);

private:
    struct State;
    std::unique_ptr<State> state;
    Session *session;
    void pruneClosedModels();
    bool closeEditingDurably(const QString &documentId, QString *error = nullptr,
                             const RProjectCatalog::PublicationLeasePtr &publicationLease = {});
    bool recordExternalChangeAfterNotification(uint modelID, const QString &reason,
                                               QString *error = nullptr);
};

#endif
