#include "control_ndjson_transport.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

#include <cmath>
#include <utility>

namespace
{
constexpr int protocolVersion = 1;
constexpr int maxCatalogEntries = 4096;

bool finiteJson(const QJsonValue &value, int depth = 0)
{
    if (depth > 32) return false;
    // Qt can turn a non-finite number into JSON null during serialization.
    // Controller state has no nullable fields in v1, so reject null as well.
    if (value.isNull()) return false;
    if (value.isDouble()) return std::isfinite(value.toDouble());
    if (value.isArray())
    {
        for (const QJsonValue &item : value.toArray())
            if (!finiteJson(item,depth+1)) return false;
    }
    if (value.isObject())
    {
        for (const QJsonValue &item : value.toObject())
            if (!finiteJson(item,depth+1)) return false;
    }
    return !value.isUndefined();
}

bool validId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 256 && !id.contains(QLatin1Char('\n')) &&
           !id.contains(QLatin1Char('\r'));
}

bool validSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value)
        if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
              (character >= QLatin1Char('a') && character <= QLatin1Char('f'))))
            return false;
    return true;
}

bool failLocal(const QString &message, QString *error)
{
    if (error) *error = message;
    return false;
}
}

ControlNdjsonTransport::ControlNdjsonTransport(ControlTransportConfig config)
    : config(std::move(config))
{
    this->process.setProcessChannelMode(QProcess::SeparateChannels);
    this->process.setReadChannel(QProcess::StandardOutput);
    QObject::connect(&this->process,&QProcess::readyReadStandardError,
                     &this->process,[this]() { this->collectStderr(); });
}

ControlNdjsonTransport::~ControlNdjsonTransport()
{
    this->stop();
}

bool ControlNdjsonTransport::start(QString *error)
{
    if (this->ready || this->process.state() != QProcess::NotRunning)
        return failLocal(QStringLiteral("Control transport is already running"),error);
    if (!QFileInfo(this->config.program).isAbsolute() ||
        !QFileInfo(this->config.program).isFile() ||
        this->config.timeoutMs <= 0 || this->config.maxLineBytes < 1024 ||
        this->config.maxLineBytes > 16*1024*1024 ||
        this->config.sensorUnits.size() > maxCatalogEntries ||
        this->config.actuatorIds.size() > maxCatalogEntries ||
        !validId(this->config.controllerId) ||
        !validSha256(this->config.controllerRevision) ||
        !validSha256(this->config.modelSnapshotSha256))
        return failLocal(QStringLiteral("Invalid control transport configuration"),error);

    QJsonArray sensors;
    for (auto it=this->config.sensorUnits.cbegin();it!=this->config.sensorUnits.cend();++it)
    {
        if (!validId(it.key()) || it.value().isEmpty())
            return failLocal(QStringLiteral("Invalid sensor catalog"),error);
        sensors.append(QJsonObject{{QStringLiteral("sensorId"),it.key()},
                                   {QStringLiteral("unit"),it.value()}});
    }
    QJsonArray actuators;
    QStringList sortedActuators = this->config.actuatorIds;
    sortedActuators.sort();
    for (int i=0;i<sortedActuators.size();i++)
    {
        if (!validId(sortedActuators[i]) ||
            (i > 0 && sortedActuators[i] == sortedActuators[i-1]))
            return failLocal(QStringLiteral("Invalid actuator catalog"),error);
        actuators.append(sortedActuators[i]);
    }
    if (actuators.isEmpty())
        return failLocal(QStringLiteral("Control transport needs an actuator"),error);

    this->stdoutBuffer.clear();
    this->stderrTail.clear();
    this->process.setProgram(this->config.program);
    this->process.setArguments(this->config.arguments);
    this->process.start(QIODevice::ReadWrite);
    if (!this->process.waitForStarted(this->config.timeoutMs))
        return this->failRemote(QStringLiteral("Controller process failed to start"),error);

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject request{{QStringLiteral("protocolVersion"),protocolVersion},
                              {QStringLiteral("requestId"),requestId},
                              {QStringLiteral("op"),QStringLiteral("handshake")},
                              {QStringLiteral("controllerId"),this->config.controllerId},
                              {QStringLiteral("controllerRevision"),this->config.controllerRevision},
                              {QStringLiteral("modelSnapshotSha256"),this->config.modelSnapshotSha256},
                              {QStringLiteral("deterministicSeed"),
                               QString::number(this->config.deterministicSeed)},
                              {QStringLiteral("capabilities"),QJsonObject{
                                  {QStringLiteral("pureStep"),true},
                                  {QStringLiteral("controlMode"),QStringLiteral("scalar-nodal-force")},
                                  {QStringLiteral("sensors"),sensors},
                                  {QStringLiteral("actuatorIds"),actuators}}}};
    QJsonObject response;
    if (!this->exchange(request,response,error)) return false;
    const QJsonValue capabilitiesValue = response.value(QStringLiteral("capabilities"));
    if (response.value(QStringLiteral("op")) != QStringLiteral("handshake") ||
        response.value(QStringLiteral("ok")) != true || !capabilitiesValue.isObject() ||
        response.value(QStringLiteral("controllerId")) != this->config.controllerId ||
        response.value(QStringLiteral("controllerRevision")) != this->config.controllerRevision ||
        response.value(QStringLiteral("modelSnapshotSha256")) != this->config.modelSnapshotSha256 ||
        response.value(QStringLiteral("deterministicSeed")) !=
            QString::number(this->config.deterministicSeed))
        return this->failRemote(QStringLiteral("Invalid controller handshake"),error);
    const QJsonObject capabilities = capabilitiesValue.toObject();
    if (capabilities.value(QStringLiteral("pureStep")) != true ||
        capabilities.value(QStringLiteral("controlMode")) != QStringLiteral("scalar-nodal-force") ||
        !capabilities.value(QStringLiteral("actuatorIds")).isArray())
        return this->failRemote(QStringLiteral("Controller does not support the required pure scalar-force mode"),error);
    QJsonArray negotiated = capabilities.value(QStringLiteral("actuatorIds")).toArray();
    QStringList negotiatedIds;
    for (const QJsonValue &id : negotiated)
    {
        if (!id.isString()) return this->failRemote(QStringLiteral("Invalid handshake actuator ID"),error);
        negotiatedIds.append(id.toString());
    }
    negotiatedIds.sort();
    if (negotiatedIds != sortedActuators)
        return this->failRemote(QStringLiteral("Controller actuator catalog does not match the model"),error);
    this->ready = true;
    return true;
}

bool ControlNdjsonTransport::compute(quint64 stepId, double time, double dt,
                                     const QVector<ControlObservation> &observations,
                                     const QJsonObject &acceptedControllerState,
                                     ControlStepReply &reply, QString *error)
{
    if (!this->ready) return failLocal(QStringLiteral("Control transport is not ready"),error);
    if (!std::isfinite(time) || !std::isfinite(dt) || dt <= 0.0 ||
        !finiteJson(acceptedControllerState))
        return failLocal(QStringLiteral("Invalid control step time, dt or controller state"),error);
    if (observations.size() != this->config.sensorUnits.size())
        return failLocal(QStringLiteral("Control observation catalog is incomplete"),error);
    QMap<QString,bool> observed;
    QJsonArray values;
    for (const ControlObservation &observation : observations)
    {
        if (!this->config.sensorUnits.contains(observation.sensorId) ||
            observed.contains(observation.sensorId) ||
            observation.unit != this->config.sensorUnits.value(observation.sensorId) ||
            !std::isfinite(observation.value))
            return failLocal(QStringLiteral("Invalid or duplicate control observation"),error);
        observed.insert(observation.sensorId,true);
        values.append(QJsonObject{{QStringLiteral("sensorId"),observation.sensorId},
                                  {QStringLiteral("value"),observation.value},
                                  {QStringLiteral("unit"),observation.unit}});
    }
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString step = QString::number(stepId);
    QJsonObject request{{QStringLiteral("protocolVersion"),protocolVersion},
                        {QStringLiteral("requestId"),requestId},
                        {QStringLiteral("op"),QStringLiteral("compute")},
                        {QStringLiteral("stepId"),step},
                        {QStringLiteral("controllerId"),this->config.controllerId},
                        {QStringLiteral("controllerRevision"),this->config.controllerRevision},
                        {QStringLiteral("modelSnapshotSha256"),this->config.modelSnapshotSha256},
                        {QStringLiteral("deterministicSeed"),
                         QString::number(this->config.deterministicSeed)},
                        {QStringLiteral("time"),time},
                        {QStringLiteral("dt"),dt},
                        {QStringLiteral("observations"),values},
                        {QStringLiteral("acceptedControllerState"),acceptedControllerState}};
    QJsonObject response;
    if (!this->exchange(request,response,error)) return false;
    if (response.value(QStringLiteral("op")) != QStringLiteral("compute") ||
        response.value(QStringLiteral("stepId")) != step ||
        !response.value(QStringLiteral("commands")).isArray() ||
        !response.value(QStringLiteral("nextControllerState")).isObject())
        return this->failRemote(QStringLiteral("Invalid control reply envelope"),error);
    const QJsonArray commands = response.value(QStringLiteral("commands")).toArray();
    if (commands.size() != this->config.actuatorIds.size())
        return this->failRemote(QStringLiteral("Control reply has an incomplete actuator catalog"),error);
    QMap<QString,bool> commanded;
    ControlStepReply candidate;
    for (const QJsonValue &item : commands)
    {
        if (!item.isObject()) return this->failRemote(QStringLiteral("Invalid control command"),error);
        const QJsonObject command = item.toObject();
        const QString id = command.value(QStringLiteral("actuatorId")).toString();
        const QJsonValue force = command.value(QStringLiteral("force"));
        if (!this->config.actuatorIds.contains(id) || commanded.contains(id) ||
            !force.isDouble() || !std::isfinite(force.toDouble()) ||
            command.value(QStringLiteral("unit")) != QStringLiteral("N"))
            return this->failRemote(QStringLiteral("Invalid, duplicate or out-of-catalog force command"),error);
        commanded.insert(id,true);
        candidate.commands.append({id,force.toDouble()});
    }
    candidate.nextControllerState = response.value(QStringLiteral("nextControllerState")).toObject();
    if (!finiteJson(candidate.nextControllerState))
        return this->failRemote(QStringLiteral("Invalid next controller state"),error);
    reply = std::move(candidate);
    return true;
}

bool ControlNdjsonTransport::exchange(const QJsonObject &request, QJsonObject &response, QString *error)
{
    const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';
    if (bytes.size() > this->config.maxLineBytes)
        return failLocal(QStringLiteral("Control request exceeds the line limit"),error);
    QElapsedTimer timer;
    timer.start();
    if (this->process.write(bytes) != bytes.size())
        return this->failRemote(QStringLiteral("Failed to queue controller request"),error);
    while (this->process.bytesToWrite() > 0)
    {
        const int remaining = this->config.timeoutMs-int(timer.elapsed());
        if (remaining <= 0 || !this->process.waitForBytesWritten(remaining))
            return this->failRemote(QStringLiteral("Controller request write timed out"),error);
    }
    while (true)
    {
        this->stdoutBuffer += this->process.readAllStandardOutput();
        this->collectStderr();
        const qsizetype newline = this->stdoutBuffer.indexOf('\n');
        if (newline >= 0)
        {
            if (newline+1 > this->config.maxLineBytes)
                return this->failRemote(QStringLiteral("Controller reply exceeds the line limit"),error);
            QByteArray line = this->stdoutBuffer.left(newline);
            this->stdoutBuffer.remove(0,newline+1);
            if (line.endsWith('\r')) line.chop(1);
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line,&parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return this->failRemote(QStringLiteral("Controller reply is not a JSON object"),error);
            response = document.object();
            if (!response.value(QStringLiteral("protocolVersion")).isDouble() ||
                response.value(QStringLiteral("protocolVersion")).toInt() != protocolVersion ||
                !response.value(QStringLiteral("requestId")).isString() ||
                response.value(QStringLiteral("requestId")).toString() != request.value(QStringLiteral("requestId")).toString())
                return this->failRemote(QStringLiteral("Controller protocol version or request ID mismatch"),error);
            return true;
        }
        if (this->stdoutBuffer.size() > this->config.maxLineBytes)
            return this->failRemote(QStringLiteral("Controller reply exceeds the line limit"),error);
        const int remaining = this->config.timeoutMs-int(timer.elapsed());
        if (remaining <= 0 || this->process.state() == QProcess::NotRunning ||
            !this->process.waitForReadyRead(remaining))
            return this->failRemote(QStringLiteral("Controller reply timed out or process exited"),error);
    }
}

bool ControlNdjsonTransport::failRemote(const QString &message, QString *error)
{
    if (error) *error = message;
    this->stop();
    return false;
}

void ControlNdjsonTransport::collectStderr()
{
    this->stderrTail += this->process.readAllStandardError();
    if (this->stderrTail.size() > 4096)
        this->stderrTail = this->stderrTail.right(4096);
}

void ControlNdjsonTransport::stop()
{
    this->ready = false;
    this->collectStderr();
    if (this->process.state() != QProcess::NotRunning)
    {
        // A pure controller can finish on stdin EOF, releasing sockets and other
        // resources normally. Keep bounded termination for an unresponsive child.
        this->process.closeWriteChannel();
        if (!this->process.waitForFinished(200))
        {
            this->process.terminate();
            if (!this->process.waitForFinished(200))
            {
                this->process.kill();
                this->process.waitForFinished(1000);
            }
        }
    }
    this->collectStderr();
    this->stdoutBuffer.clear();
}

bool ControlNdjsonTransport::isReady() const
{
    return this->ready && this->process.state() == QProcess::Running;
}

QByteArray ControlNdjsonTransport::standardErrorTail()
{
    this->collectStderr();
    return this->stderrTail;
}
