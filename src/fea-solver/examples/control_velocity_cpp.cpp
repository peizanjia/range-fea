// Host C++ reference controller: pure v1 velocity feedback, Qt Core JSON only.
// This executable calculates requested force; it never drives physical I/O.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr std::size_t maximumLine = 1024 * 1024;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
double number(const QJsonValue &value, const char *message) {
    require(value.isDouble() && std::isfinite(value.toDouble()), message);
    return value.toDouble();
}
QString id(const QJsonValue &value) {
    require(value.isString(), "ID must be a string");
    const auto text = value.toString();
    require(!text.isEmpty() && text.size() <= 256 && !text.contains('\n') && !text.contains('\r'), "Invalid ID");
    return text;
}
void sha(const QJsonValue &value) {
    const auto text = id(value);
    require(text.size() == 64, "SHA256 must have 64 hex characters");
    for (const auto c : text)
        require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "Invalid SHA256");
}
bool readLine(std::string &line) {
    line.clear(); char c;
    while (std::cin.get(c)) {
        if (c == '\n') return true;
        require(line.size() < maximumLine, "Protocol line exceeds limit");
        line.push_back(c);
    }
    require(line.empty(), "Truncated protocol line");
    return false;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        require(argc == 2, "Usage: range-control-velocity GAIN_Ns_per_m");
        bool ok = false;
        const double gain = QString::fromLocal8Bit(argv[1]).toDouble(&ok);
        require(ok && std::isfinite(gain) && gain >= 0.0, "Gain must be finite and nonnegative");
        QString sensorId, actuatorId;
        QMap<QString,QString> sensorUnits;
        bool ready = false;
        std::string line;
        while (readLine(line)) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
            require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid JSON object");
            const auto request = document.object();
            require(number(request["protocolVersion"], "Invalid protocol") == 1, "Unsupported protocol");
            id(request["requestId"]);
            const auto op = id(request["op"]);
            QJsonObject response{{"protocolVersion",1}, {"requestId",request["requestId"]}, {"op",op}};
            if (op == "handshake") {
                require(!ready, "Duplicate handshake");
                id(request["controllerId"]); sha(request["controllerRevision"]); sha(request["modelSnapshotSha256"]);
                const auto seed = id(request["deterministicSeed"]);
                for (const auto c : seed) require(c >= '0' && c <= '9', "Seed must be decimal digits");
                require(request["capabilities"].isObject(), "Missing capabilities");
                const auto caps = request["capabilities"].toObject();
                require(caps["pureStep"].isBool() && caps["pureStep"].toBool() && caps["controlMode"] == "scalar-nodal-force", "Unsupported mode");
                require(caps["sensors"].isArray() && caps["actuatorIds"].isArray(), "Invalid catalogs");
                const auto sensors = caps["sensors"].toArray(), actuators = caps["actuatorIds"].toArray();
                require(!sensors.isEmpty() && sensors.size() <= 4096 && actuators.size() == 1, "Exactly one actuator and bounded sensor catalog required");
                for (const auto &entry : sensors) {
                    require(entry.isObject(), "Invalid sensor entry");
                    const auto sensor = entry.toObject();
                    const auto name = id(sensor["sensorId"]), unit = id(sensor["unit"]);
                    require(!sensorUnits.contains(name), "Duplicate sensor ID");
                    sensorUnits[name] = unit;
                    if (unit == "m/s") {
                        require(sensorId.isEmpty(), "Exactly one velocity sensor required");
                        sensorId = name;
                    }
                }
                require(!sensorId.isEmpty(), "Velocity sensor required");
                actuatorId = id(actuators[0]);
                response["ok"] = true;
                response["capabilities"] = QJsonObject{{"pureStep",true},{"controlMode","scalar-nodal-force"},{"actuatorIds",actuators}};
                for (const auto field : {"controllerId","controllerRevision","modelSnapshotSha256","deterministicSeed"}) response[field] = request[field];
                ready = true;
            } else if (op == "compute") {
                require(ready, "Handshake required");
                id(request["stepId"]);
                require(number(request["dt"], "Invalid dt") > 0, "dt must be positive");
                require(request["observations"].isArray(), "Missing observations");
                const auto observations = request["observations"].toArray();
                require(observations.size() == sensorUnits.size(), "Observation count mismatch");
                QSet<QString> seen; double velocity = 0.0;
                for (const auto &entry : observations) {
                    require(entry.isObject(), "Invalid observation entry");
                    const auto observation = entry.toObject();
                    const auto name = id(observation["sensorId"]);
                    require(sensorUnits.contains(name) && !seen.contains(name) && observation["unit"] == sensorUnits[name], "Observation catalog mismatch");
                    seen.insert(name);
                    const double value = number(observation["value"], "Invalid observation value");
                    if (name == sensorId) velocity = value;
                }
                const double force = -gain * velocity;
                require(std::isfinite(force), "Command overflow");
                require(request["acceptedControllerState"].isObject(), "Invalid accepted state");
                const auto state = request["acceptedControllerState"].toObject();
                const double count = state.contains("sampleCount") ? number(state["sampleCount"], "Invalid sample count") : 0.0;
                require(count >= 0 && count < 9007199254740991.0 && count == std::floor(count), "Invalid sample count");
                response["stepId"] = request["stepId"];
                response["commands"] = QJsonArray{QJsonObject{{"actuatorId",actuatorId},{"force",force},{"unit","N"}}};
                response["nextControllerState"] = QJsonObject{{"sampleCount",count+1}};
            } else throw std::runtime_error("Unknown operation");
            std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).constData() << std::endl;
            require(bool(std::cout), "stdout write failed");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Velocity controller rejected request: " << error.what() << std::endl;
        return 2;
    }
}
