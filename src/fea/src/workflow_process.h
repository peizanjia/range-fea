#ifndef WORKFLOW_PROCESS_H
#define WORKFLOW_PROCESS_H

#include <QObject>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>

//! Nonblocking GUI transport. Only this trusted host can submit host.approve.
//! The workflow service owns its workspace; GUI document state stays in Session.
class WorkflowProcess : public QObject
{
    Q_OBJECT
public:
    struct Configuration
    {
        QString python, packageDirectory, solver, native, examples, workspace, nativeResults;
        int requestTimeoutMs = 30000;
        bool trustedExtensions = false;
    };
    explicit WorkflowProcess(QObject *parent = nullptr);
    ~WorkflowProcess() override;
    static Configuration defaultConfiguration(const QString &workspace);
    void start(const Configuration &configuration);
    void stop();
    bool isReady() const;
    bool isStopped() const { return process.state() == QProcess::NotRunning; }
    QString request(const QString &operation, const QJsonObject &parameters = {});
    const Configuration &configuration() const { return config; }

signals:
    void readyChanged(bool ready);
    void processStateChanged();
    void replyReceived(const QString &requestId, const QString &operation, const QJsonObject &result);
    void requestFailed(const QString &requestId, const QString &operation,
                       const QString &code, const QString &message);
    void serviceError(const QString &message);

private:
    struct Pending { QString operation; qint64 deadline; QString cancelTarget; };
    QProcess process;
    QTimer deadlineTimer;
    QElapsedTimer clock;
    Configuration config;
    QMap<QString, Pending> pending;
    QMap<QString, Pending> retiredReads;
    QStringList retiredOrder;
    QByteArray output, diagnostics;
    QString handshakeId;
    bool ready = false;
    bool stopping = false;
    quint64 generation = 0;
    void consumeOutput();
    void cancelRead(const QString &requestId);
    void failTransport(const QString &code, const QString &message);
    void setReady(bool value);
};

#endif
