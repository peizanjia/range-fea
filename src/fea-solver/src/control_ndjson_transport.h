#ifndef CONTROL_NDJSON_TRANSPORT_H
#define CONTROL_NDJSON_TRANSPORT_H

#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QStringList>
#include <QVector>

//! Process transport only: the caller owns accepted controller state and step commit.
struct ControlTransportConfig
{
    QString program;
    QStringList arguments;
    QString controllerId;
    QString controllerRevision;
    QString modelSnapshotSha256;
    quint64 deterministicSeed = 0;
    QMap<QString,QString> sensorUnits;
    QStringList actuatorIds;
    int timeoutMs = 5000;
    int maxLineBytes = 1024*1024;
    //! Empty keeps the original v1 protocol; otherwise negotiate WF09 extension.
    QJsonObject runtimeIdentity;
};

struct ControlObservation
{
    QString sensorId;
    double value = 0.0;
    QString unit;
};

struct ControlForceCommand
{
    QString actuatorId;
    double force = 0.0;
};

struct ControlStepReply
{
    QVector<ControlForceCommand> commands;
    QJsonObject nextControllerState;
};

//! Version 1, newline-delimited UTF-8 JSON over a child process's stdio.
//! The child must be pure with respect to each compute request: retries and
//! rollback are coordinated by the caller, not by hidden process state.
class ControlNdjsonTransport
{
public:
    explicit ControlNdjsonTransport(ControlTransportConfig config);
    ~ControlNdjsonTransport();

    bool start(QString *error = nullptr);
    bool compute(quint64 stepId, double time, double dt,
                 const QVector<ControlObservation> &observations,
                 const QJsonObject &acceptedControllerState,
                 ControlStepReply &reply, QString *error = nullptr,
                 const QJsonObject &runtimeParameters = {});
    void stop();
    bool isReady() const;
    QByteArray standardErrorTail();

private:
    bool exchange(const QJsonObject &request, QJsonObject &response, QString *error);
    bool failRemote(const QString &message, QString *error);
    void collectStderr();

    ControlTransportConfig config;
    QProcess process;
    QByteArray stdoutBuffer;
    QByteArray stderrTail;
    bool ready = false;
};

#endif // CONTROL_NDJSON_TRANSPORT_H
