#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <rbl_error.h>
#include <rbl_logger.h>
#include <rml_file_io.h>
#include <rmodel_command_session.h>
#include <cstdio>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
FILE *jsonOutput = stdout;
void require(bool valid, const char *message)
{
    if (!valid) throw RError(RError::Type::InvalidInput, R_ERROR_REF, "%s", message);
}
QString resolved(const QString &path, bool mustExist)
{
    require(!path.isEmpty(), "Missing file path.");
    QFileInfo info(path);
    if (info.exists())
    {
        require(info.isFile() && !info.isSymLink(), "Expected a regular file, not a symbolic link.");
        return info.canonicalFilePath();
    }
    require(!mustExist && !info.isSymLink(), "File does not exist.");
    const auto parent = info.dir().canonicalPath();
    require(!parent.isEmpty(), "Output directory must exist.");
    return QDir(parent).filePath(info.fileName());
}
QByteArray hash(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read file for hashing.");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&file), "File hash failed.");
    return digest.result().toHex();
}
void print(const QJsonObject &object)
{
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, bytes.size(), jsonOutput);
    std::fflush(jsonOutput);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("range-model-command");
    QCommandLineParser parser;
    parser.setApplicationDescription("Query or configure modal analysis through the shared application service.");
    parser.addHelpOption();
    parser.addOptions({
        QCommandLineOption(QStringList{"i", "input"}, "Complete native .ram/.rbm model.", "file"),
        QCommandLineOption(QStringList{"p", "parameters"}, "JSON object containing sourceSha256 and parameters.", "file"),
        QCommandLineOption(QStringList{"o", "output"}, "Separate native output model; never overwrites the input.", "file"),
        QCommandLineOption("query", "Print the current modal settings and source hash without editing."),
        QCommandLineOption("dry-run", "Validate a configuration without publishing a model."),
        QCommandLineOption("reconcile", "Observe and journal unfinished publication intents; requires --journal only."),
        QCommandLineOption("journal", "Trusted-host InteractionRecord v1 journal file.", "file")});
    parser.process(app);
    // RLogger writes directly to stdout even when a handler is installed.
    // Keep a dedicated copy for the protocol and route legacy console output
    // to stderr for this one-shot process, without changing the shared logger.
    std::fflush(stdout);
#ifdef Q_OS_WIN
    const int protocolFd = _dup(_fileno(stdout));
    FILE *protocol = protocolFd < 0 ? nullptr : _fdopen(protocolFd, "wb");
#else
    const int protocolFd = dup(fileno(stdout));
    FILE *protocol = protocolFd < 0 ? nullptr : fdopen(protocolFd, "w");
#endif
    if (!protocol)
    {
        if (protocolFd >= 0)
        {
#ifdef Q_OS_WIN
            _close(protocolFd);
#else
            close(protocolFd);
#endif
        }
        std::fputs("Cannot open JSON output channel.\n", stderr);
        return 2;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> protocolGuard(protocol, &std::fclose);
#ifdef Q_OS_WIN
    const int redirected = _dup2(_fileno(stderr), _fileno(stdout));
#else
    const int redirected = dup2(fileno(stderr), fileno(stdout));
#endif
    if (redirected < 0)
    {
        std::fputs("Cannot redirect model diagnostics.\n", stderr);
        return 2;
    }
    jsonOutput = protocol;
    bool artifactPublished = false;
    QJsonObject publicationResponse;
    try
    {
        require(parser.positionalArguments().isEmpty(), "Unexpected positional arguments.");
        if (parser.isSet("reconcile"))
        {
            require(parser.isSet("journal") && !parser.isSet("input") && !parser.isSet("parameters")
                        && !parser.isSet("output") && !parser.isSet("query") && !parser.isSet("dry-run"),
                    "Reconcile requires --journal and cannot be combined with model commands.");
            RModelCommandSession::JournalOptions recovery;
            recovery.path = resolved(parser.value("journal"), true);
            recovery.actorId = QStringLiteral("range-model-command-recovery");
            const auto result = RModelCommandSession::reconcilePublications(recovery);
            print(result);
            if (result.value("status").toString() == QStringLiteral("rejected")) return 2;
            if (result.value("status").toString() != QStringLiteral("reconciled")) return 3;
            for (const auto &value : result.value("outcomes").toArray())
            {
                const QString status = value.toObject().value("status").toString();
                if (status != QStringLiteral("artifact_present") && status != QStringLiteral("already_present")
                    && status != QStringLiteral("not_published")) return 3;
            }
            return 0;
        }
        const bool query = parser.isSet("query"), preview = parser.isSet("dry-run");
        require(!query || (!preview && !parser.isSet("parameters") && !parser.isSet("output")),
                "Query cannot be combined with editing options.");
        require(!preview || !parser.isSet("output"), "Preview must not specify an output.");
        const QString input = resolved(parser.value("input"), true);
        const QString output = (!query && !preview) ? resolved(parser.value("output"), false) : QString();
        const QString parameterPath = query ? QString() : resolved(parser.value("parameters"), true);
        const QString journalPath = parser.isSet("journal") ? resolved(parser.value("journal"), false) : QString();
        const QString extension = QFileInfo(input).suffix();
        const bool ascii = extension == RModel::getDefaultFileExtension(false);
        require(ascii || extension == RModel::getDefaultFileExtension(true), "Input must be a .ram/.rbm model.");
        if (!output.isEmpty())
        {
            require(output.compare(input, Qt::CaseInsensitive) != 0
                    && output.compare(parameterPath, Qt::CaseInsensitive) != 0, "Output must be distinct from inputs.");
            require(QFileInfo(output).suffix() == RModel::getDefaultFileExtension(false)
                    || QFileInfo(output).suffix() == RModel::getDefaultFileExtension(true), "Output must be .ram/.rbm.");
        }
        require(journalPath.isEmpty()
                    || (journalPath.compare(input, Qt::CaseInsensitive) != 0
                        && journalPath.compare(parameterPath, Qt::CaseInsensitive) != 0
                        && journalPath.compare(output, Qt::CaseInsensitive) != 0),
                "Journal must be distinct from model and parameter files.");
        QStringList paths{input};
        if (!output.isEmpty()) paths.append(output);
        if (!parameterPath.isEmpty()) paths.append(parameterPath);
        paths.removeDuplicates();
        paths.sort(Qt::CaseInsensitive);
        std::vector<std::unique_ptr<QLockFile>> locks;
        for (const auto &path : paths)
        {
            auto lock = std::make_unique<QLockFile>(path + ".lock");
            lock->setStaleLockTime(0);
            require(lock->tryLock(0), "An input/output file is busy.");
            locks.push_back(std::move(lock));
        }
        const auto sourceHash = hash(input);
        const bool existed = !output.isEmpty() && QFileInfo::exists(output);
        const auto outputHash = existed ? hash(output) : QByteArray();
        RFile native(input, ascii ? RFile::ASCII : RFile::BINARY);
        require(native.open(ascii ? QIODevice::ReadOnly | QIODevice::Text : QIODevice::ReadOnly), "Cannot open model.");
        RFileHeader header;
        if (ascii) RFileIO::readAscii(native, header); else RFileIO::readBinary(native, header);
        require(header.getType() == R_FILE_TYPE_MODEL, "Model links are unsupported; supply the complete model.");
        native.close();
        RModel model;
        model.read(input);
        require(hash(input) == sourceHash, "Source changed while reading.");
        RModelCommandSession::JournalOptions journalOptions;
        journalOptions.path = journalPath;
        journalOptions.actorId = QStringLiteral("range-model-command");
        journalOptions.sourcePath = input;
        journalOptions.sourceSha256 = QString::fromLatin1(sourceHash);
        RModelCommandSession session(model, journalOptions);
        if (query)
        {
            auto result = session.query();
            result["sourceSha256"] = QString::fromLatin1(sourceHash);
            print(result);
            return 0;
        }
        QFile parametersFile(parameterPath);
        require(parametersFile.open(QIODevice::ReadOnly) && parametersFile.size() <= 65536, "Parameters must be readable and at most 64 KiB.");
        const auto parameterBytes = parametersFile.readAll();
        parametersFile.close();
        const auto parameterHash = QCryptographicHash::hash(parameterBytes, QCryptographicHash::Sha256).toHex();
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(parameterBytes, &error);
        require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid parameters JSON object.");
        const auto object = document.object();
        require(object.size() == 2 && object.value("parameters").isObject()
                && object.value("sourceSha256").isString(), "Expected sourceSha256 and parameters only.");
        require(object.value("sourceSha256").toString().toLatin1() == sourceHash, "Stale source hash; query the source again.");
        const auto current = session.query();
        const QJsonObject command{{"apiVersion", "range.application/v1"}, {"commandId", "analysis.modal.configure"},
            {"commandVersion", 1}, {"modelId", current["modelId"]}, {"expectedRevision", current["revision"]},
            {"requestId", QUuid::createUuid().toString()}, {"idempotencyKey", QUuid::createUuid().toString()},
            {"parameters", object["parameters"]}, {"dryRun", preview}};
        auto response = session.execute(command);
        if (response.value("status") == QJsonValue("rejected")) { print(response); return 2; }
        response["sourceSha256"] = QString::fromLatin1(sourceHash);
        if (preview) { print(response); return 0; }
        QTemporaryDir staging(QFileInfo(output).dir().filePath(".range-model-command-XXXXXX"));
        require(staging.isValid(), "Cannot create output staging directory.");
        const auto stagedPath = session.model().write(staging.filePath("model." + QFileInfo(output).suffix()), false);
        QFile staged(stagedPath);
        require(staged.open(QIODevice::ReadOnly), "Cannot read staged model.");
        QSaveFile published(output);
        published.setDirectWriteFallback(false);
        require(published.open(QIODevice::WriteOnly), "Cannot stage final output.");
        while (!staged.atEnd())
        {
            const auto bytes = staged.read(1024 * 1024);
            require(!bytes.isEmpty() && published.write(bytes) == bytes.size(), "Output copy failed.");
        }
        require(resolved(parser.value("input"), true) == input && hash(input) == sourceHash
                && resolved(parser.value("parameters"), true) == parameterPath && hash(parameterPath) == parameterHash
                && resolved(parser.value("output"), false) == output
                && QFileInfo::exists(output) == existed && (!existed || hash(output) == outputHash),
                "Input/output changed before publication; no model published.");
        publicationResponse = response;
        publicationResponse["status"] = journalPath.isEmpty() ? "published_unverified" : "published_unlogged";
        publicationResponse["output"] = output;
        if (!journalPath.isEmpty())
        {
            QString journalError;
            require(session.recordPublicationIntent(output, stagedPath, &journalError),
                    "Cannot persist publication intent; no model published.");
        }
        require(published.commit(), "Atomic model publication failed.");
        artifactPublished = true;
        QByteArray outputSha256;
        if (!journalPath.isEmpty())
        {
            QString journaledSha256;
            QString journalError;
            if (!session.recordArtifactPublished(output, &journaledSha256, &journalError))
            {
                outputSha256 = journaledSha256.toLatin1();
                response["status"] = "published_unlogged";
                response["output"] = output;
                if (!outputSha256.isEmpty()) response["outputSha256"] = QString::fromLatin1(outputSha256);
                response["code"] = "journal_failure";
                response["auditError"] = QStringLiteral("The model file was published, but its journal event failed.");
                print(response);
                return 3;
            }
            outputSha256 = journaledSha256.toLatin1();
        }
        else outputSha256 = hash(output);
        response["status"] = "published";
        response["output"] = output;
        response["outputSha256"] = QString::fromLatin1(outputSha256);
        print(response);
        return 0;
    }
    catch (const RError &error)
    {
        if (artifactPublished)
        {
            publicationResponse["message"] = "The model file was published, but post-publication verification failed.";
            print(publicationResponse);
            return 3;
        }
        print({{"apiVersion", "range.application/v1"}, {"status", "rejected"},
               {"code", "command_failure"}, {"message", error.getMessage()}});
    }
    catch (const std::exception &error)
    {
        if (artifactPublished)
        {
            publicationResponse["message"] = "The model file was published, but post-publication verification failed.";
            print(publicationResponse);
            return 3;
        }
        print({{"apiVersion", "range.application/v1"}, {"status", "rejected"},
               {"code", "command_failure"}, {"message", error.what()}});
    }
    return 2;
}
