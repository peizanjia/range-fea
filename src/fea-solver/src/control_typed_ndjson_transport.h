#ifndef CONTROL_TYPED_NDJSON_TRANSPORT_H
#define CONTROL_TYPED_NDJSON_TRANSPORT_H

#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QStringList>
#include <QVector>

#include "rstructural_typed_control.h"

//! Process configuration for the pure, typed-value NDJSON protocol v2.
struct ControlTypedTransportConfig
{
    QString program;
    QStringList arguments;
    QString controllerIdentity;
    quint64 deterministicSeed = 0;
    double fixedDt = 0.0;
    QByteArray configurationSha256; //!< Raw 32-byte SHA-256 digest.
    QVector<RStructuralTypedControlChannel> channels; //!< Ordered typed catalog.
    QMap<QString,QString> sensorUnits; //!< Stable sensor ID to SI unit.
    int timeoutMs = 5000;
    int maxLineBytes = 2*1024*1024; //!< Bounds a full JSON line, including base64 state.
};

//! Version 2 typed controller process using newline-delimited UTF-8 JSON.
//!
//! The transport does not retain controller state between requests. Each
//! request carries the caller's last accepted opaque state, so retry and commit
//! decisions remain owned by the FE driver.
class ControlTypedNdjsonTransport
{
    public:
        explicit ControlTypedNdjsonTransport(ControlTypedTransportConfig config);
        ~ControlTypedNdjsonTransport();

        bool start(QString *error = nullptr);
        bool compute(quint64 stepId,
                     const RStructuralControlObservation &observation,
                     RStructuralTypedControlDecision &decision,
                     QString *error = nullptr);
        void stop();
        bool isReady() const;
        QByteArray standardErrorTail();

    private:
        bool exchange(const QJsonObject &request, QJsonObject &response, QString *error);
        bool failRemote(const QString &message, QString *error);
        void collectStderr();

        ControlTypedTransportConfig config;
        QProcess process;
        QByteArray stdoutBuffer;
        QByteArray stderrTail;
        QString digestHex;
        bool ready = false;
};

#endif // CONTROL_TYPED_NDJSON_TRANSPORT_H
