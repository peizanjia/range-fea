#ifndef RUNTIME_CONTROL_BRIDGE_H
#define RUNTIME_CONTROL_BRIDGE_H

#include "runtime_parameter_gate.h"
#include <QObject>
#include <QJsonObject>
#include <QTcpSocket>
#include <memory>
#include <mutex>

class NativeExecutionGate;

// Socket ownership never leaves the application event-loop thread. Worker
// methods transfer value copies through queued calls and share only the gate.
class RuntimeControlBridge final : public QObject
{
public:
    struct Config { quint16 port = 0; QByteArray token; QString runId; double gainNsm = 0; int timeoutMs = 5000; QString taskId; QString jobId; };
    RuntimeControlBridge(Config config, std::shared_ptr<NativeExecutionGate> execution, QObject *parent = nullptr);
    ~RuntimeControlBridge() override;
    static std::optional<Config> environmentConfig(); // throws for partial/invalid opt-in
    double bootstrapGain() const { return config_.gainNsm; }
    QString runId() const { return config_.runId; }
    QString taskId() const { return config_.taskId; }
    QString jobId() const { return config_.jobId; }
    bool initialize(RuntimeParameterGate::Identity identity, RuntimeParameterGate::State initial,
                    std::vector<RuntimeParameterGate::Limit> limits, QString *error = nullptr);
    std::shared_ptr<RuntimeParameterGate> parameterGate() const;
    bool publish(const RuntimeParameterGate::Receipt &receipt, QString *error = nullptr);
    void stop();

    static QJsonObject identityJson(const RuntimeParameterGate::Identity &identity);
    static QJsonObject parametersJson(const RuntimeParameterGate::Parameters &parameters);
    static bool strictJson(const QByteArray &bytes, QJsonObject &object);

private:
    bool initializeOnMain(QString *error);
    bool send(const QJsonObject &frame);
    bool publishOnMain(const RuntimeParameterGate::Receipt &receipt);
    QJsonObject receiptJson(const RuntimeParameterGate::Receipt &receipt) const;
    void readFrames();
    void command(const QJsonObject &frame);
    void disconnected();
    QJsonObject envelope(const QString &op, const QString &requestId) const;

    Config config_;
    const std::shared_ptr<NativeExecutionGate> execution_;
    QTcpSocket socket_;
    QByteArray input_;
    RuntimeParameterGate::Identity identity_;
    mutable std::mutex mutex_;
    std::shared_ptr<RuntimeParameterGate> gate_;
    bool ready_ = false;
    bool stopping_ = false;
};

#endif
