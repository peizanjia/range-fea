#ifndef MODEL_COMMAND_HOST_H
#define MODEL_COMMAND_HOST_H

#include <QObject>
#include <QJsonObject>
#include <QList>
#include <memory>
#include <rml_modal_setup.h>

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
    bool configureModal(const QList<uint> &modelIDs, const RModalSetup &setup, QString *error = nullptr);
    bool recordArtifactPublished(uint modelID, const QString &path, QString *error = nullptr);
    //! Audit a completed legacy GUI mutation and retire its old command identity.
    bool recordExternalChange(uint modelID, const QString &reason, QString *error = nullptr);
    QString journalPath() const;
    static bool isBusy();

signals:
    void commandRejected(const QString &message);

private:
    struct State;
    std::unique_ptr<State> state;
    Session *session;
    void pruneClosedModels();
};

#endif
