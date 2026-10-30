// Minimal C++ stdio provider for range-external-io/1.
// The host owns FE time, accepted state, limits, and commit/rollback.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr std::size_t maximumLineBytes = 1024 * 1024;
const QString protocol = QStringLiteral("range-external-io/1");
const QJsonObject algorithm{{QStringLiteral("id"), QStringLiteral("range-example-cpp-provider")},
                            {QStringLiteral("version"), QStringLiteral("1")}};

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

bool readLine(std::string &line)
{
    line.clear();
    char value = 0;
    while (std::cin.get(value)) {
        if (value == '\n') return true;
        require(line.size() < maximumLineBytes, "Protocol line exceeds 1 MiB");
        line.push_back(value);
    }
    require(line.empty(), "Truncated protocol line");
    return false;
}

QString requiredString(const QJsonObject &object, const char *field)
{
    const auto value = object.value(QLatin1String(field));
    require(value.isString() && !value.toString().isEmpty(), field);
    return value.toString();
}

double finiteNumber(const QJsonValue &value, const char *field)
{
    require(value.isDouble() && std::isfinite(value.toDouble()), field);
    return value.toDouble();
}

QJsonObject envelope(const QJsonObject &request)
{
    return {{QStringLiteral("protocol"), request.value(QStringLiteral("protocol"))},
            {QStringLiteral("op"), request.value(QStringLiteral("op"))},
            {QStringLiteral("runId"), request.value(QStringLiteral("runId"))},
            {QStringLiteral("requestId"), request.value(QStringLiteral("requestId"))}};
}

void writeLine(const QJsonObject &response)
{
    const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    require(bytes.size() < static_cast<qsizetype>(maximumLineBytes), "Response exceeds 1 MiB");
    std::cout.write(bytes.constData(), bytes.size());
    std::cout.put('\n');
    std::cout.flush();
    require(std::cout.good(), "Protocol stdout write failed");
}

QJsonObject evaluate(const QJsonObject &request, const QJsonArray &ports)
{
    const auto role = requiredString(request, "role");
    require(role == QStringLiteral("excitation") || role == QStringLiteral("controller"), "Unknown role");
    const double time = finiteNumber(request.value(QStringLiteral("time")), "Invalid simulation time");
    const double period = finiteNumber(request.value(QStringLiteral("samplePeriod")), "Invalid sample period");
    require(period > 0.0, "Invalid simulation interval");

    QJsonArray outputs;
    int index = 0;
    double measurement = 0.0;
    const auto inputs = request.value(QStringLiteral("inputs")).toArray();
    for (const auto &inputValue : inputs) {
        require(inputValue.isObject(), "Invalid input signal");
        const auto input = inputValue.toObject();
        require(requiredString(input, "unit") == QStringLiteral("m/s"),
                "This example controller accepts velocity inputs in m/s only");
        measurement += finiteNumber(input.value(QStringLiteral("value")), "Invalid velocity input");
    }

    for (const auto &portValue : ports) {
        require(portValue.isObject(), "Invalid registered port");
        const auto port = portValue.toObject();
        if (port.value(QStringLiteral("role")).toString() != role
            || port.value(QStringLiteral("direction")).toString() != QStringLiteral("output"))
            continue;

        const auto quantity = port.value(QStringLiteral("quantity")).toString();
        const auto unit = port.value(QStringLiteral("unit")).toString();
        double value = 0.0;
        if (role == QStringLiteral("excitation")) {
            require(quantity == QStringLiteral("force") || quantity == QStringLiteral("moment"),
                    "Excitation outputs must be force or moment");
            const double amplitude = 0.05 * double(index + 1);
            const double frequency = double(index + 1);
            value = amplitude * std::sin(2.0 * std::acos(-1.0) * frequency * time);
        } else {
            require(quantity == QStringLiteral("voltage") && unit == QStringLiteral("V")
                        && port.value(QStringLiteral("reference")).toString() == QStringLiteral("absolute"),
                    "Controller outputs must be absolute voltage in V");
            require(!inputs.isEmpty(), "This example controller requires a velocity input");
            const auto state = request.value(QStringLiteral("state")).toObject();
            const double oldIntegral = finiteNumber(state.value(QStringLiteral("integral")),
                                                    "Controller state has no finite integral");
            const double integral = oldIntegral + period * measurement;
            value = -2.0 * measurement - 0.1 * integral;
        }
        require(std::isfinite(value), "Non-finite algorithm output");
        outputs.append(QJsonObject{{QStringLiteral("id"), port.value(QStringLiteral("id"))},
                                   {QStringLiteral("unit"), unit},
                                   {QStringLiteral("reference"), port.value(QStringLiteral("reference"))},
                                   {QStringLiteral("value"), value}});
        ++index;
    }

    QJsonObject response = envelope(request);
    response.insert(QStringLiteral("step"), request.value(QStringLiteral("step")));
    response.insert(QStringLiteral("tick"), request.value(QStringLiteral("tick")));
    response.insert(QStringLiteral("acceptedStateDigest"),
                     request.value(QStringLiteral("acceptedStateDigest")));
    response.insert(QStringLiteral("outputs"), outputs);
    if (role == QStringLiteral("excitation")) {
        const auto state = request.value(QStringLiteral("state")).toObject();
        const auto oldSamples = state.value(QStringLiteral("samples")).toString(QStringLiteral("0"));
        bool ok = false;
        const auto sampleCount = oldSamples.toULongLong(&ok);
        require(ok && sampleCount < std::numeric_limits<qulonglong>::max(),
                "Excitation sample counter is invalid or exhausted");
        response.insert(QStringLiteral("candidateState"),
                        QJsonObject{{QStringLiteral("samples"), QString::number(sampleCount + 1)}});
    } else {
        const auto state = request.value(QStringLiteral("state")).toObject();
        const double oldIntegral = finiteNumber(state.value(QStringLiteral("integral")),
                                                "Controller state has no finite integral");
        response.insert(QStringLiteral("candidateState"),
                        QJsonObject{{QStringLiteral("integral"), oldIntegral + period * measurement}});
    }
    return response;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    try {
        require(argc == 1, "Usage: range-external-io-cpp (stdio protocol)");
        std::string line;
        QString runId;
        QString previousRequestId;
        QJsonArray ports;
        QSet<QString> roles;
        bool ready = false;
        while (readLine(line)) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
            require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid JSON request");
            const auto request = document.object();
            require(requiredString(request, "protocol") == protocol, "Unsupported protocol");
            const auto operation = requiredString(request, "op");
            const auto currentRun = requiredString(request, "runId");
            const auto requestId = requiredString(request, "requestId");
            require(!requestId.isEmpty() && requestId.size() <= 18, "Request counter is out of range");
            bool idOk = false;
            const auto currentNumber = requestId.toULongLong(&idOk);
            require(idOk && QString::number(currentNumber) == requestId, "Request ID must be decimal");
            if (!previousRequestId.isEmpty()) {
                bool previousOk = false;
                const auto previousNumber = previousRequestId.toULongLong(&previousOk);
                require(previousOk && currentNumber > previousNumber, "Request IDs must increase");
            }
            previousRequestId = requestId;

            if (operation == QStringLiteral("handshake")) {
                require(!ready && request.value(QStringLiteral("algorithm")).toObject() == algorithm,
                        "Unexpected or repeated handshake");
                ports = request.value(QStringLiteral("ports")).toArray();
                require(!ports.isEmpty(), "Handshake has no physical ports");
                for (const auto &value : ports) {
                    require(value.isObject(), "Invalid physical port");
                    const auto role = value.toObject().value(QStringLiteral("role")).toString();
                    require(role == QStringLiteral("excitation") || role == QStringLiteral("controller"),
                            "Unknown physical port role");
                    roles.insert(role);
                }
                runId = currentRun;
                auto response = envelope(request);
                for (const auto *key : {"modelDigest", "configDigest", "mappingDigest"})
                    response.insert(QLatin1String(key), request.value(QLatin1String(key)));
                response.insert(QStringLiteral("algorithm"), algorithm);
                response.insert(QStringLiteral("ports"), ports);
                response.insert(QStringLiteral("capabilities"),
                                QJsonObject{{QStringLiteral("explicitState"), true},
                                            {QStringLiteral("futurePreview"), false}});
                QJsonObject initialState;
                if (roles.contains(QStringLiteral("excitation")))
                    initialState.insert(QStringLiteral("excitation"),
                                        QJsonObject{{QStringLiteral("samples"), QStringLiteral("0")}});
                if (roles.contains(QStringLiteral("controller")))
                    initialState.insert(QStringLiteral("controller"),
                                        QJsonObject{{QStringLiteral("integral"), 0.0}});
                response.insert(QStringLiteral("initialState"), initialState);
                ready = true;
                writeLine(response);
            } else if (operation == QStringLiteral("evaluate")) {
                require(ready && currentRun == runId, "Handshake/run identity mismatch");
                const auto role = requiredString(request, "role");
                require(roles.contains(role), "Unregistered role");
                writeLine(evaluate(request, ports));
            } else if (operation == QStringLiteral("close")) {
                require(ready && currentRun == runId, "Handshake/run identity mismatch");
                writeLine(envelope(request));
                return 0;
            } else {
                throw std::runtime_error("Unknown operation");
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
