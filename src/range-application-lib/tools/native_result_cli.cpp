#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTextStream>

#include "rnative_result_broker.h"

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("range-native-results"));
    application.setApplicationVersion(QStringLiteral("1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Read hash-bound native result manifests and MODEL frames."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption manifestOption(QStringList{QStringLiteral("manifest")},
        QStringLiteral("Path to native-result.manifest.json."), QStringLiteral("path"));
    const QCommandLineOption shaOption(QStringList{QStringLiteral("expected-sha")},
        QStringLiteral("Expected lowercase manifest SHA-256."), QStringLiteral("sha256"));
    const QCommandLineOption operationOption(QStringList{QStringLiteral("operation")},
        QStringLiteral("Read-only operation: catalog, channels, series, or summary."), QStringLiteral("name"));
    const QCommandLineOption parametersOption(QStringList{QStringLiteral("parameters")},
        QStringLiteral("Optional inline JSON object of bounded query arguments."), QStringLiteral("json"));
    parser.addOption(manifestOption);
    parser.addOption(shaOption);
    parser.addOption(operationOption);
    parser.addOption(parametersOption);
    parser.process(application);

    const auto printError = [](const QString &code, const QString &message, int exitCode)
    {
        const QJsonObject error{{QStringLiteral("valid"), false},
                                {QStringLiteral("code"), code},
                                {QStringLiteral("message"), message}};
        QTextStream output(stdout);
        output << QJsonDocument(error).toJson(QJsonDocument::Compact) << Qt::endl;
        return exitCode;
    };

    if (!parser.isSet(manifestOption) || !parser.isSet(shaOption) || !parser.isSet(operationOption))
        return printError(QStringLiteral("USAGE_ERROR"),
            QStringLiteral("--manifest, --expected-sha, and --operation are required."), 2);

    QJsonObject parameters;
    if (parser.isSet(parametersOption))
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(parser.value(parametersOption).toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return printError(QStringLiteral("INVALID_PARAMETERS"),
                QStringLiteral("--parameters must be an inline JSON object."), 2);
        parameters = document.object();
    }

    const QJsonObject result = RNativeResultBroker::query(parser.value(manifestOption),
        parser.value(shaOption), parser.value(operationOption), parameters);
    QTextStream output(stdout);
    output << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return result.value(QStringLiteral("valid")).toBool(false) ? 0 : 3;
}
