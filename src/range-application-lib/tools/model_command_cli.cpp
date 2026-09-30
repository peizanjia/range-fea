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
        QCommandLineOption("dry-run", "Validate a configuration without publishing a model.")});
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
    try
    {
        require(parser.positionalArguments().isEmpty(), "Unexpected positional arguments.");
        const bool query = parser.isSet("query"), preview = parser.isSet("dry-run");
        require(!query || (!preview && !parser.isSet("parameters") && !parser.isSet("output")),
                "Query cannot be combined with editing options.");
        require(!preview || !parser.isSet("output"), "Preview must not specify an output.");
        const QString input = resolved(parser.value("input"), true);
        const QString output = (!query && !preview) ? resolved(parser.value("output"), false) : QString();
        const QString parameterPath = query ? QString() : resolved(parser.value("parameters"), true);
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
        RModelCommandSession session(model);
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
        require(published.commit(), "Atomic model publication failed.");
        response["status"] = "published";
        response["output"] = output;
        print(response);
        return 0;
    }
    catch (const RError &error) { print({{"status", "rejected"}, {"message", error.getMessage()}}); }
    catch (const std::exception &error) { print({{"status", "rejected"}, {"message", error.what()}}); }
    return 2;
}
