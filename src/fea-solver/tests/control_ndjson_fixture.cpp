#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    QCoreApplication app(argc,argv);
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("normal");
    std::string line;
    while (std::getline(std::cin,line))
    {
        const QJsonDocument input = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        if (!input.isObject()) return 2;
        const QJsonObject request = input.object();
        const QString op = request.value(QStringLiteral("op")).toString();
        QJsonObject response{{QStringLiteral("protocolVersion"),1},
                             {QStringLiteral("requestId"),request.value(QStringLiteral("requestId"))},
                             {QStringLiteral("op"),op}};
        if (op == QStringLiteral("handshake"))
        {
            const QJsonObject offered = request.value(QStringLiteral("capabilities")).toObject();
            response.insert(QStringLiteral("ok"),true);
            for (const QString &field : {QStringLiteral("controllerId"),
                                         QStringLiteral("controllerRevision"),
                                         QStringLiteral("modelSnapshotSha256"),
                                         QStringLiteral("deterministicSeed")})
                response.insert(field,request.value(field));
            if (mode == QStringLiteral("bad-identity"))
                response.insert(QStringLiteral("controllerRevision"),QStringLiteral("wrong"));
            response.insert(QStringLiteral("capabilities"),QJsonObject{
                {QStringLiteral("pureStep"),mode != QStringLiteral("bad-handshake")},
                {QStringLiteral("controlMode"),QStringLiteral("scalar-nodal-force")},
                {QStringLiteral("actuatorIds"),offered.value(QStringLiteral("actuatorIds"))}});
        }
        else if (op == QStringLiteral("compute"))
        {
            if (mode == QStringLiteral("timeout")) QThread::msleep(1500);
            if (mode == QStringLiteral("exit")) return 3;
            if (mode == QStringLiteral("oversize"))
            {
                std::cout << std::string(4096,'x') << std::flush;
                continue;
            }
            response.insert(QStringLiteral("stepId"),request.value(QStringLiteral("stepId")));
            const QJsonObject accepted = request.value(QStringLiteral("acceptedControllerState")).toObject();
            const int count = accepted.value(QStringLiteral("count")).toInt();
            const double bias = accepted.value(QStringLiteral("bias")).toDouble();
            const QJsonArray observations = request.value(QStringLiteral("observations")).toArray();
            const double displacement = observations.at(0).toObject().value(QStringLiteral("value")).toDouble();
            const double force = -2.0*displacement+bias;
            QJsonArray commands;
            commands.append(QJsonObject{{QStringLiteral("actuatorId"),
                                         mode == QStringLiteral("bad-id") ? QStringLiteral("unknown") : QStringLiteral("tip-force")},
                                        {QStringLiteral("force"),mode == QStringLiteral("bad-force")
                                            ? QJsonValue(QStringLiteral("NaN")) : QJsonValue(force)},
                                        {QStringLiteral("unit"),QStringLiteral("N")}});
            if (mode == QStringLiteral("duplicate")) commands.append(commands.at(0));
            response.insert(QStringLiteral("commands"),commands);
            response.insert(QStringLiteral("nextControllerState"),
                            QJsonObject{{QStringLiteral("count"),count+1},
                                        {QStringLiteral("bias"),bias}});
            if (mode == QStringLiteral("wrong-step"))
                response.insert(QStringLiteral("stepId"),QStringLiteral("999"));
            if (mode == QStringLiteral("stderr"))
                std::cerr << "controller diagnostic, not protocol" << std::endl;
        }
        else return 4;
        if (mode == QStringLiteral("wrong-request"))
            response.insert(QStringLiteral("requestId"),QStringLiteral("wrong"));
        std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).constData() << std::endl;
    }
    return 0;
}
