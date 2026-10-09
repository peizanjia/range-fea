#ifndef MESH_JOB_CONTROLLER_H
#define MESH_JOB_CONTROLLER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <memory>

class Session;

//! GUI-thread owner of isolated mesh workers and detached candidate receipts.
//! Approval methods are C++ trusted-host/UI boundaries, never callable operations.
class MeshJobController : public QObject
{
    Q_OBJECT
public:
    explicit MeshJobController(Session *session, QObject *parent = nullptr);
    ~MeshJobController() override;
    QJsonObject previewGenerate(uint modelID, const QJsonObject &options);
    QJsonObject approveGenerate(const QString &previewId);
    QJsonObject previewAccept(const QString &attemptId);
    QJsonObject approveAccept(const QString &previewId);
    QJsonObject query(const QString &attemptId);
    QJsonArray jobs();
    QJsonObject cancel(const QString &attemptId);
    void invalidatePreviews();
    void shutdown();

signals:
    void jobsChanged();
    void previewsInvalidated();
    void eventPublished(const QJsonObject &event);
    void candidateAccepted(const QString &attemptId, const QString &runtimeDocumentId);

private:
    struct State;
    std::unique_ptr<State> state;
    QPointer<Session> session;
    bool ensureWorkspace(QString *error);
    bool persist(QString *error);
    bool persistStopDecision(const QString &attemptId, const QString &decision, QString *error);
    bool sourceIsCurrent(const QJsonObject &capture, bool verifyLiveBytes, QString *error);
    QJsonObject view(const QString &attemptId) const;
    void startNext();
    void poll();
};

#endif
