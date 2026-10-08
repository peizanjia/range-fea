#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTextStream>
#include <QFile>

#include "rnative_result_broker.h"
#include <cstdio>
#include <memory>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

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
    const QCommandLineOption resultDirectoryOption(QStringList{QStringLiteral("result-directory")},
        QStringLiteral("Completed result directory to publish."), QStringLiteral("path"));
    const QCommandLineOption latestOption(QStringList{QStringLiteral("latest")},
        QStringLiteral("Logical latest native artifact to publish."), QStringLiteral("path"));
    const QCommandLineOption provenanceOption(QStringList{QStringLiteral("provenance-file")},
        QStringLiteral("Bounded provenance JSON file owned by the caller."), QStringLiteral("path"));
    parser.addOption(resultDirectoryOption);
    parser.addOption(latestOption);
    parser.addOption(provenanceOption);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Optional publish command."));
    parser.process(application);

    // Preserve stdout for exactly one JSON reply; native readers' diagnostics
    // use stderr, following range-workflow-native's established channel split.
    std::fflush(stdout);
#ifdef Q_OS_WIN
    const int jsonFd = _dup(_fileno(stdout));
    FILE *channel = jsonFd < 0 ? nullptr : _fdopen(jsonFd, "wb");
    const int redirected = _dup2(_fileno(stderr), _fileno(stdout));
#else
    const int jsonFd = dup(fileno(stdout));
    FILE *channel = jsonFd < 0 ? nullptr : fdopen(jsonFd, "w");
    const int redirected = dup2(fileno(stderr), fileno(stdout));
#endif
    if (!channel || redirected < 0) return 2;
    std::unique_ptr<FILE, decltype(&std::fclose)> outputGuard(channel, &std::fclose);
    const auto print = [channel](const QJsonObject &value) {
        const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), channel);
        std::fflush(channel);
    };

    const auto printError = [&print](const QString &code, const QString &message, int exitCode)
    {
        const QJsonObject error{{QStringLiteral("valid"), false},
                                {QStringLiteral("code"), code},
                                {QStringLiteral("message"), message}};
        print(error);
        return exitCode;
    };

    if (parser.positionalArguments() == QStringList{QStringLiteral("publish")})
    {
        if (!parser.isSet(resultDirectoryOption) || !parser.isSet(latestOption) || !parser.isSet(provenanceOption) ||
            parser.isSet(manifestOption) || parser.isSet(shaOption) || parser.isSet(operationOption) || parser.isSet(parametersOption))
            return printError(QStringLiteral("USAGE_ERROR"),
                QStringLiteral("publish requires --result-directory, --latest, and --provenance-file only."), 2);
        QFile file(parser.value(provenanceOption));
        constexpr qint64 maximumBytes = 16 * 1024 * 1024;
        if (!file.open(QIODevice::ReadOnly) || file.size() > maximumBytes)
            return printError(QStringLiteral("INVALID_PROVENANCE"), QStringLiteral("The provenance file is unreadable or exceeds 16 MiB."), 2);
        const QByteArray bytes = file.read(maximumBytes + 1);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
        if (bytes.size() > maximumBytes || !file.atEnd() || file.error() != QFile::NoError ||
            parseError.error != QJsonParseError::NoError || !document.isObject())
            return printError(QStringLiteral("INVALID_PROVENANCE"), QStringLiteral("The provenance file must contain one bounded JSON object."), 2);
        const auto published = RNativeResultBroker::publishResult(parser.value(resultDirectoryOption),
            parser.value(latestOption), document.object());
        QJsonObject result{{QStringLiteral("valid"), published.valid},
            {QStringLiteral("code"), published.code}, {QStringLiteral("message"), published.message}};
        if (published.valid)
        {
            result.insert(QStringLiteral("manifestPath"), published.manifestPath);
            result.insert(QStringLiteral("manifestSha256"), published.manifestSha256);
            result.insert(QStringLiteral("artifactId"), published.artifactId);
            result.insert(QStringLiteral("manifest"), published.manifest);
        }
        print(result);
        return published.valid ? 0 : 3;
    }
    if (!parser.positionalArguments().isEmpty() || parser.isSet(resultDirectoryOption) ||
        parser.isSet(latestOption) || parser.isSet(provenanceOption))
        return printError(QStringLiteral("USAGE_ERROR"), QStringLiteral("Only publish accepts publication arguments."), 2);
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
    print(result);
    return result.value(QStringLiteral("valid")).toBool(false) ? 0 : 3;
}
