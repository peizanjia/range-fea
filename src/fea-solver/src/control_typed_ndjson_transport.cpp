#include "control_typed_ndjson_transport.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace
{
constexpr int protocolVersion = 2;
constexpr int maxCatalogEntries = 4096;
constexpr int maxControllerStateBytes = 1024*1024;

bool validId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 256 && id.trimmed() == id &&
           !id.contains(QLatin1Char('\n')) && !id.contains(QLatin1Char('\r')) &&
           !id.contains(QChar(0));
}

bool validQuantity(RStructuralTypedControlQuantity quantity)
{
    switch (quantity)
    {
        case RStructuralTypedControlQuantity::Force:
        case RStructuralTypedControlQuantity::CableReferenceLength:
        case RStructuralTypedControlQuantity::CableInitialTension:
        case RStructuralTypedControlQuantity::CableAnchorDisplacement:
        case RStructuralTypedControlQuantity::Voltage:
        case RStructuralTypedControlQuantity::Charge:
            return true;
    }
    return false;
}

QString quantityName(RStructuralTypedControlQuantity quantity)
{
    switch (quantity)
    {
        case RStructuralTypedControlQuantity::Force: return QStringLiteral("force");
        case RStructuralTypedControlQuantity::CableReferenceLength:
            return QStringLiteral("cable_reference_length");
        case RStructuralTypedControlQuantity::CableInitialTension:
            return QStringLiteral("cable_initial_tension");
        case RStructuralTypedControlQuantity::CableAnchorDisplacement:
            return QStringLiteral("cable_anchor_displacement");
        case RStructuralTypedControlQuantity::Voltage: return QStringLiteral("voltage");
        case RStructuralTypedControlQuantity::Charge: return QStringLiteral("charge");
    }
    throw std::invalid_argument("Unknown typed control quantity");
}

QJsonObject channelDescriptor(const RStructuralTypedControlChannel &channel)
{
    return QJsonObject{{QStringLiteral("id"),channel.channelId},
                       {QStringLiteral("quantity"),quantityName(channel.quantity)},
                       {QStringLiteral("unit"),rStructuralTypedControlUnit(channel.quantity)}};
}

bool failLocal(const QString &message, QString *error)
{
    if (error) *error = message;
    return false;
}

QString seedString(quint64 seed)
{
    return QString::number(seed);
}

bool matchesFixedInterval(double time, double nextTime, double fixedDt)
{
    if (!std::isfinite(time) || !std::isfinite(nextTime) || nextTime <= time)
        return false;
    const long double interval = static_cast<long double>(nextTime)-
                                 static_cast<long double>(time);
    const long double error = std::abs(interval-static_cast<long double>(fixedDt));
    const long double tolerance = 1.0e-9L*static_cast<long double>(fixedDt);
    return error <= tolerance;
}

bool matchesTypedCatalog(const QJsonValue &value,
                         const QVector<RStructuralTypedControlChannel> &channels)
{
    if (!value.isArray()) return false;
    const QJsonArray entries = value.toArray();
    if (entries.size() != channels.size()) return false;
    for (qsizetype i=0;i<entries.size();i++)
    {
        if (!entries[i].isObject() || entries[i].toObject() != channelDescriptor(channels[i]))
            return false;
    }
    return true;
}

QJsonArray sensorCatalog(const QMap<QString,QString> &sensorUnits)
{
    QJsonArray sensors;
    for (auto it=sensorUnits.cbegin();it!=sensorUnits.cend();++it)
        sensors.append(QJsonObject{{QStringLiteral("sensorId"),it.key()},
                                   {QStringLiteral("unit"),it.value()}});
    return sensors;
}

bool matchesSensorCatalog(const QJsonValue &value, const QMap<QString,QString> &sensorUnits)
{
    if (!value.isArray()) return false;
    const QJsonArray entries = value.toArray();
    if (entries.size() != sensorUnits.size()) return false;
    qsizetype index = 0;
    for (auto it=sensorUnits.cbegin();it!=sensorUnits.cend();++it,++index)
    {
        const QJsonObject expected{{QStringLiteral("sensorId"),it.key()},
                                  {QStringLiteral("unit"),it.value()}};
        if (!entries[index].isObject() || entries[index].toObject() != expected)
            return false;
    }
    return true;
}

bool strictBase64Decode(const QString &text, QByteArray &decoded)
{
    const qint64 maxEncodedBytes = 4*((qint64(maxControllerStateBytes)+2)/3);
    const QByteArray encoded = text.toLatin1();
    if (encoded.size() > maxEncodedBytes || QString::fromLatin1(encoded) != text)
        return false;
    decoded = QByteArray::fromBase64(encoded);
    if (decoded.size() > maxControllerStateBytes || decoded.toBase64() != encoded)
        return false;
    return true;
}
}

ControlTypedNdjsonTransport::ControlTypedNdjsonTransport(ControlTypedTransportConfig config)
    : config(std::move(config))
{
    this->process.setProcessChannelMode(QProcess::SeparateChannels);
    this->process.setReadChannel(QProcess::StandardOutput);
    QObject::connect(&this->process,&QProcess::readyReadStandardError,
                     &this->process,[this]() { this->collectStderr(); });
}

ControlTypedNdjsonTransport::~ControlTypedNdjsonTransport()
{
    this->stop();
}

bool ControlTypedNdjsonTransport::start(QString *error)
{
    if (this->ready || this->process.state() != QProcess::NotRunning)
        return failLocal(QStringLiteral("Typed control transport is already running"),error);
    if (!QFileInfo(this->config.program).isAbsolute() ||
        !QFileInfo(this->config.program).isFile() ||
        this->config.timeoutMs <= 0 || this->config.maxLineBytes < 1024 ||
        this->config.maxLineBytes > 16*1024*1024 ||
        this->config.sensorUnits.size() > maxCatalogEntries ||
        this->config.channels.isEmpty() || this->config.channels.size() > maxCatalogEntries ||
        !validId(this->config.controllerIdentity) ||
        this->config.configurationSha256.size() != 32 ||
        !std::isfinite(this->config.fixedDt) || this->config.fixedDt <= 0.0)
        return failLocal(QStringLiteral("Invalid typed control transport configuration"),error);

    for (auto it=this->config.sensorUnits.cbegin();it!=this->config.sensorUnits.cend();++it)
    {
        if (!validId(it.key()) || it.value().isEmpty() || it.value().size() > 32 ||
            it.value().contains(QChar(0)) || it.value().contains(QLatin1Char('\n')) ||
            it.value().contains(QLatin1Char('\r')))
            return failLocal(QStringLiteral("Invalid typed sensor catalog"),error);
    }

    QSet<QString> channelIds;
    for (const auto &channel : this->config.channels)
    {
        if (!validId(channel.channelId) || channelIds.contains(channel.channelId) ||
            !validQuantity(channel.quantity))
            return failLocal(QStringLiteral("Invalid or duplicate typed channel catalog"),error);
        channelIds.insert(channel.channelId);
    }

    this->digestHex = QString::fromLatin1(this->config.configurationSha256.toHex());
    const QJsonArray sensors = sensorCatalog(this->config.sensorUnits);
    QJsonArray channels;
    for (const auto &channel : this->config.channels)
        channels.append(channelDescriptor(channel));

    this->stdoutBuffer.clear();
    this->stderrTail.clear();
    this->process.setProgram(this->config.program);
    this->process.setArguments(this->config.arguments);
    this->process.start(QIODevice::ReadWrite);
    if (!this->process.waitForStarted(this->config.timeoutMs))
        return this->failRemote(QStringLiteral("Typed controller process failed to start"),error);

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject request{
        {QStringLiteral("protocolVersion"),protocolVersion},
        {QStringLiteral("requestId"),requestId},
        {QStringLiteral("op"),QStringLiteral("handshake")},
        {QStringLiteral("controllerIdentity"),this->config.controllerIdentity},
        {QStringLiteral("configurationSha256"),this->digestHex},
        {QStringLiteral("deterministicSeed"),seedString(this->config.deterministicSeed)},
        {QStringLiteral("fixedDt"),this->config.fixedDt},
        {QStringLiteral("commandSchema"),QStringLiteral("typed-values")},
        {QStringLiteral("capabilities"),QJsonObject{
            {QStringLiteral("pureStep"),true},
            {QStringLiteral("commandSchema"),QStringLiteral("typed-values")},
            {QStringLiteral("channels"),channels},
            {QStringLiteral("sensors"),sensors}}}};

    QJsonObject response;
    if (!this->exchange(request,response,error)) return false;
    const QJsonObject capabilities = response.value(QStringLiteral("capabilities")).toObject();
    if (response.value(QStringLiteral("op")) != QStringLiteral("handshake") ||
        response.value(QStringLiteral("ok")) != true ||
        response.value(QStringLiteral("controllerIdentity")) != this->config.controllerIdentity ||
        response.value(QStringLiteral("configurationSha256")) != this->digestHex ||
        response.value(QStringLiteral("deterministicSeed")) != seedString(this->config.deterministicSeed) ||
        !response.value(QStringLiteral("fixedDt")).isDouble() ||
        response.value(QStringLiteral("fixedDt")).toDouble() != this->config.fixedDt ||
        response.value(QStringLiteral("commandSchema")) != QStringLiteral("typed-values") ||
        !response.value(QStringLiteral("capabilities")).isObject() ||
        capabilities.value(QStringLiteral("pureStep")) != true ||
        capabilities.value(QStringLiteral("commandSchema")) != QStringLiteral("typed-values") ||
        !matchesTypedCatalog(capabilities.value(QStringLiteral("channels")),this->config.channels) ||
        !matchesSensorCatalog(capabilities.value(QStringLiteral("sensors")),this->config.sensorUnits))
        return this->failRemote(QStringLiteral("Invalid typed controller handshake or catalog"),error);

    this->ready = true;
    return true;
}

bool ControlTypedNdjsonTransport::compute(
    quint64 stepId,
    const RStructuralControlObservation &observation,
    RStructuralTypedControlDecision &decision,
    QString *error)
{
    if (!this->ready) return failLocal(QStringLiteral("Typed control transport is not ready"),error);
    if (!matchesFixedInterval(observation.acceptedTime,observation.nextTime,this->config.fixedDt) ||
        observation.deterministicSeed != this->config.deterministicSeed ||
        observation.priorControllerState.size() > maxControllerStateBytes)
        return failLocal(QStringLiteral("Invalid typed control time, seed or accepted state"),error);

    if (observation.sensors.size() != this->config.sensorUnits.size())
        return failLocal(QStringLiteral("Typed control observation catalog is incomplete"),error);
    QMap<QString,double> observed;
    for (const auto &sensor : observation.sensors)
    {
        if (!this->config.sensorUnits.contains(sensor.sensorId) || observed.contains(sensor.sensorId) ||
            sensor.unit != this->config.sensorUnits.value(sensor.sensorId) ||
            sensor.unit.size() > 32 || !std::isfinite(sensor.value))
            return failLocal(QStringLiteral("Invalid or duplicate typed control observation"),error);
        observed.insert(sensor.sensorId,sensor.value);
    }
    for (auto it=this->config.sensorUnits.cbegin();it!=this->config.sensorUnits.cend();++it)
        if (!observed.contains(it.key()))
            return failLocal(QStringLiteral("Typed control observation catalog is incomplete"),error);

    QJsonArray values;
    for (auto it=this->config.sensorUnits.cbegin();it!=this->config.sensorUnits.cend();++it)
    {
        values.append(QJsonObject{{QStringLiteral("sensorId"),it.key()},
                                  {QStringLiteral("value"),observed.value(it.key())},
                                  {QStringLiteral("unit"),it.value()}});
    }
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString step = QString::number(stepId);
    const QByteArray acceptedStateBase64 = observation.priorControllerState.toBase64();
    const QJsonObject request{
        {QStringLiteral("protocolVersion"),protocolVersion},
        {QStringLiteral("requestId"),requestId},
        {QStringLiteral("op"),QStringLiteral("compute")},
        {QStringLiteral("stepId"),step},
        {QStringLiteral("controllerIdentity"),this->config.controllerIdentity},
        {QStringLiteral("configurationSha256"),this->digestHex},
        {QStringLiteral("deterministicSeed"),seedString(this->config.deterministicSeed)},
        {QStringLiteral("time"),observation.acceptedTime},
        {QStringLiteral("nextTime"),observation.nextTime},
        {QStringLiteral("fixedDt"),this->config.fixedDt},
        {QStringLiteral("observations"),values},
        {QStringLiteral("acceptedControllerState"),QString::fromLatin1(acceptedStateBase64)}};

    QJsonObject response;
    if (!this->exchange(request,response,error)) return false;
    if (response.value(QStringLiteral("op")) != QStringLiteral("compute") ||
        response.value(QStringLiteral("ok")) != true ||
        response.value(QStringLiteral("stepId")) != step ||
        response.value(QStringLiteral("controllerIdentity")) != this->config.controllerIdentity ||
        response.value(QStringLiteral("configurationSha256")) != this->digestHex ||
        response.value(QStringLiteral("commandSchema")) != QStringLiteral("typed-values") ||
        !response.value(QStringLiteral("commands")).isArray() ||
        !response.value(QStringLiteral("nextControllerState")).isString())
        return this->failRemote(QStringLiteral("Invalid typed control reply envelope"),error);

    const QJsonArray commands = response.value(QStringLiteral("commands")).toArray();
    if (commands.size() != this->config.channels.size())
        return this->failRemote(QStringLiteral("Typed reply has an incomplete channel catalog"),error);

    QVector<bool> commanded(this->config.channels.size(),false);
    RStructuralTypedControlDecision candidate;
    candidate.requestedCommands.resize(std::size_t(this->config.channels.size()));
    for (const QJsonValue &item : commands)
    {
        if (!item.isObject())
            return this->failRemote(QStringLiteral("Invalid typed control command"),error);
        const QJsonObject command = item.toObject();
        const QJsonValue idValue = command.value(QStringLiteral("channelId"));
        const QJsonValue quantityValue = command.value(QStringLiteral("quantity"));
        const QJsonValue unitValue = command.value(QStringLiteral("unit"));
        const QJsonValue value = command.value(QStringLiteral("value"));
        if (!idValue.isString() || !quantityValue.isString() || !unitValue.isString() ||
            !value.isDouble() || !std::isfinite(value.toDouble()))
            return this->failRemote(QStringLiteral("Typed command fields have invalid JSON types"),error);
        const QString id = idValue.toString();
        int position = -1;
        for (int i=0;i<this->config.channels.size();i++)
        {
            if (this->config.channels[i].channelId == id)
            {
                position = i;
                break;
            }
        }
        if (position < 0 || commanded[position])
            return this->failRemote(QStringLiteral("Unexpected or duplicate typed channel ID"),error);
        const auto &expected = this->config.channels[position];
        if (quantityValue.toString() != quantityName(expected.quantity) ||
            unitValue.toString() != rStructuralTypedControlUnit(expected.quantity))
            return this->failRemote(QStringLiteral("Typed command quantity or unit mismatch"),error);
        commanded[position] = true;
        candidate.requestedCommands[std::size_t(position)] =
            {expected.channelId,expected.quantity,value.toDouble()};
    }
    for (bool seen : commanded)
        if (!seen)
            return this->failRemote(QStringLiteral("Typed reply is missing a channel command"),error);

    QByteArray nextState;
    if (!strictBase64Decode(response.value(QStringLiteral("nextControllerState")).toString(),nextState))
        return this->failRemote(QStringLiteral("Invalid or oversized base64 controller state"),error);
    candidate.nextControllerState = std::move(nextState);
    decision = std::move(candidate);
    return true;
}

bool ControlTypedNdjsonTransport::exchange(const QJsonObject &request,
                                           QJsonObject &response,
                                           QString *error)
{
    const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';
    if (bytes.size() > this->config.maxLineBytes)
        return failLocal(QStringLiteral("Typed control request exceeds the line limit"),error);
    QElapsedTimer timer;
    timer.start();
    if (this->process.write(bytes) != bytes.size())
        return this->failRemote(QStringLiteral("Failed to queue typed controller request"),error);
    while (this->process.bytesToWrite() > 0)
    {
        const int remaining = this->config.timeoutMs-int(timer.elapsed());
        if (remaining <= 0 || !this->process.waitForBytesWritten(remaining))
            return this->failRemote(QStringLiteral("Typed controller request write timed out"),error);
    }
    while (true)
    {
        this->stdoutBuffer += this->process.readAllStandardOutput();
        this->collectStderr();
        const qsizetype newline = this->stdoutBuffer.indexOf('\n');
        if (newline >= 0)
        {
            if (newline+1 > this->config.maxLineBytes)
                return this->failRemote(QStringLiteral("Typed controller reply exceeds the line limit"),error);
            QByteArray line = this->stdoutBuffer.left(newline);
            this->stdoutBuffer.remove(0,newline+1);
            if (line.endsWith('\r')) line.chop(1);
            if (QString::fromUtf8(line).toUtf8() != line)
                return this->failRemote(QStringLiteral("Typed controller reply is not valid UTF-8"),error);
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line,&parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return this->failRemote(QStringLiteral("Typed controller reply is not a JSON object"),error);
            response = document.object();
            if (!response.value(QStringLiteral("protocolVersion")).isDouble() ||
                response.value(QStringLiteral("protocolVersion")).toInt() != protocolVersion ||
                !response.value(QStringLiteral("requestId")).isString() ||
                response.value(QStringLiteral("requestId")).toString() !=
                    request.value(QStringLiteral("requestId")).toString())
                return this->failRemote(QStringLiteral("Typed controller protocol version or request ID mismatch"),error);
            return true;
        }
        if (this->stdoutBuffer.size() > this->config.maxLineBytes)
            return this->failRemote(QStringLiteral("Typed controller reply exceeds the line limit"),error);
        const int remaining = this->config.timeoutMs-int(timer.elapsed());
        if (remaining <= 0 || this->process.state() == QProcess::NotRunning ||
            !this->process.waitForReadyRead(remaining))
            return this->failRemote(QStringLiteral("Typed controller reply timed out or process exited"),error);
    }
}

bool ControlTypedNdjsonTransport::failRemote(const QString &message, QString *error)
{
    this->stop();
    if (error) *error = this->stderrTail.isEmpty() ? message :
        message+QStringLiteral("; controller stderr: ")+QString::fromUtf8(this->stderrTail);
    return false;
}

void ControlTypedNdjsonTransport::collectStderr()
{
    this->stderrTail += this->process.readAllStandardError();
    if (this->stderrTail.size() > 4096)
        this->stderrTail = this->stderrTail.right(4096);
}

void ControlTypedNdjsonTransport::stop()
{
    this->ready = false;
    this->collectStderr();
    if (this->process.state() != QProcess::NotRunning)
    {
        this->process.terminate();
        if (!this->process.waitForFinished(200))
        {
            this->process.kill();
            this->process.waitForFinished(1000);
        }
    }
    this->collectStderr();
    this->stdoutBuffer.clear();
}

bool ControlTypedNdjsonTransport::isReady() const
{
    return this->ready && this->process.state() == QProcess::Running;
}

QByteArray ControlTypedNdjsonTransport::standardErrorTail()
{
    this->collectStderr();
    return this->stderrTail;
}
