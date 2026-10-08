#include <rproject_catalog.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QUuid>
#include <rbl_error.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
FILE *jsonOutput = stdout;

void require(bool condition, const QString &message)
{
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}

void print(const QJsonObject &object)
{
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), jsonOutput);
    std::fflush(jsonOutput);
}

QString canonicalInput(const QString &path)
{
    const QFileInfo info(path);
    require(info.exists() && info.isFile() && !info.isSymLink(),
            QStringLiteral("Snapshot input must be an existing regular non-symbolic file."));
    const QString canonical = QDir::cleanPath(QDir::fromNativeSeparators(info.canonicalFilePath()));
    require(!canonical.isEmpty(), QStringLiteral("Could not canonicalize snapshot input."));
    return canonical;
}

void requireExistingCatalogManifest(const QString &workspace)
{
    const QString path = QDir(QFileInfo(workspace).absoluteFilePath()).filePath(QStringLiteral("project.json"));
    const QFileInfo info(path);
    require(info.exists() && info.isFile() && !info.isSymLink(),
            QStringLiteral("Project does not exist; use --create first."));
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("range-project"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Query and maintain a versioned Range project catalog."));
    parser.addHelpOption();
    parser.addOptions({
        QCommandLineOption(QStringList{QStringLiteral("w"), QStringLiteral("workspace")},
                           QStringLiteral("Explicit project workspace directory."), QStringLiteral("path")),
        QCommandLineOption(QStringLiteral("query"), QStringLiteral("Read the project catalog.")),
        QCommandLineOption(QStringLiteral("document"), QStringLiteral("Query one persistent document ID or select it for --snapshot."),
                           QStringLiteral("id")),
        QCommandLineOption(QStringLiteral("create"), QStringLiteral("Create the project manifest if it does not exist.")),
        QCommandLineOption(QStringLiteral("snapshot"), QStringLiteral("Create an immutable snapshot from a registered saved model file."),
                           QStringLiteral("input")),
        QCommandLineOption(QStringLiteral("register"), QStringLiteral("Register a model source path."),
                           QStringLiteral("input")),
        QCommandLineOption(QStringList{QStringLiteral("l"), QStringLiteral("label")},
                           QStringLiteral("Document label for --register."), QStringLiteral("label"))});
    parser.addPositionalArgument(QStringLiteral("label"),
                                 QStringLiteral("Optional label following --register INPUT."),
                                 QStringLiteral("[label]"));
    parser.process(app);

    // Legacy model IO can write through RLogger to stdout. Keep a protocol
    // descriptor for JSON and route those diagnostics to stderr in this CLI.
    std::fflush(stdout);
#ifdef Q_OS_WIN
    const int protocolFd = _dup(_fileno(stdout));
    FILE *protocol = protocolFd < 0 ? nullptr : _fdopen(protocolFd, "wb");
    const int redirected = _dup2(_fileno(stderr), _fileno(stdout));
#else
    const int protocolFd = dup(fileno(stdout));
    FILE *protocol = protocolFd < 0 ? nullptr : fdopen(protocolFd, "w");
    const int redirected = dup2(fileno(stderr), fileno(stdout));
#endif
    if (!protocol)
    {
        std::fputs("Cannot open JSON output channel.\n", stderr);
        return 2;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> protocolGuard(protocol, &std::fclose);
    if (redirected < 0)
    {
        std::fputs("Cannot redirect model diagnostics.\n", stderr);
        return 2;
    }
    jsonOutput = protocol;

    try
    {
        require(parser.isSet(QStringLiteral("workspace")),
                QStringLiteral("An explicit --workspace path is required."));
        const QString workspace = parser.value(QStringLiteral("workspace"));
        const QStringList positional = parser.positionalArguments();
        const bool query = parser.isSet(QStringLiteral("query"));
        const bool create = parser.isSet(QStringLiteral("create"));
        const bool snapshot = parser.isSet(QStringLiteral("snapshot"));
        const bool registration = parser.isSet(QStringLiteral("register"));
        const bool document = parser.isSet(QStringLiteral("document"));
        const bool documentQuery = document && !snapshot;
        const int commands = int(query) + int(create) + int(snapshot) + int(registration)
            + int(documentQuery && !query);
        require(commands == 1, QStringLiteral("Choose exactly one of --query, --create, --snapshot, or --register."));
        require(!parser.isSet(QStringLiteral("label")) || registration,
                QStringLiteral("--label is valid only with --register."));
        if (registration)
            require(positional.size() <= 1, QStringLiteral("--register accepts at most one positional label."));
        else
            require(positional.isEmpty(), QStringLiteral("Unexpected positional argument."));
        if (snapshot)
            require(document, QStringLiteral("--snapshot requires --document ID."));
        else if (query)
            require(!parser.isSet(QStringLiteral("snapshot")), QStringLiteral("Invalid query options."));
        if (parser.isSet(QStringLiteral("label")) && !positional.isEmpty())
            throw std::runtime_error("Use either --label or a positional label, not both.");

        QString error;
        if (query || documentQuery || snapshot || registration)
            requireExistingCatalogManifest(workspace);
        const QFileInfo manifestInfo(QDir(QFileInfo(workspace).absoluteFilePath())
                                         .filePath(QStringLiteral("project.json")));
        const bool existedBefore = manifestInfo.exists();
        auto catalog = RProjectCatalog::open(workspace, &error);
        require(bool(catalog), error.isEmpty() ? QStringLiteral("Could not open project workspace.") : error);

        if (create)
        {
            print({{QStringLiteral("protocolVersion"), QStringLiteral("range.project/v1")},
                   {QStringLiteral("storageVersion"), RProjectCatalog::StorageVersion},
                   {QStringLiteral("projectId"), catalog->projectId()},
                   {QStringLiteral("created"), !existedBefore}});
            return 0;
        }
        if (query || documentQuery)
        {
            if (document)
            {
                const auto found = catalog->document(parser.value(QStringLiteral("document")), &error);
                require(found.has_value(), error.isEmpty() ? QStringLiteral("Persistent document was not found.") : error);
                QJsonArray sourceArtifacts;
                for (const auto &artifact : found->sourceArtifacts)
                    sourceArtifacts.append(artifact.toJson());
                const QJsonObject documentRecord{
                    {QStringLiteral("persistentDocumentId"), found->persistentDocumentId},
                    {QStringLiteral("label"), found->label},
                    {QStringLiteral("sourcePath"), found->sourcePath},
                    {QStringLiteral("resolvedSourcePath"), found->resolvedSourcePath},
                    {QStringLiteral("sourceArtifacts"), sourceArtifacts},
                    {QStringLiteral("originDocumentId"), found->originDocumentId},
                    {QStringLiteral("sourceRevision"), found->sourceRevision},
                    {QStringLiteral("savedArtifactHash"), found->savedArtifactHash},
                    {QStringLiteral("dirty"), found->dirty},
                    {QStringLiteral("draftMetadata"), found->draftMetadata},
                    {QStringLiteral("tombstone"), found->tombstone},
                    {QStringLiteral("modelSha256"), found->modelSha256},
                    {QStringLiteral("snapshotSourceRevision"), found->snapshotSourceRevision},
                    {QStringLiteral("snapshotRelativePath"), found->snapshotRelativePath},
                    {QStringLiteral("snapshotInstanceRef"), found->snapshotInstanceRef},
                    {QStringLiteral("orphanSnapshots"), found->orphanSnapshots}};
                print({{QStringLiteral("protocolVersion"), QStringLiteral("range.project/v1")},
                       {QStringLiteral("storageVersion"), RProjectCatalog::StorageVersion},
                       {QStringLiteral("projectId"), catalog->projectId()},
                       {QStringLiteral("document"), documentRecord},
                       {QStringLiteral("capability"), RProjectCatalog::capabilityDescription()}});
            }
            else
            {
                const QJsonObject result = catalog->query(&error);
                require(!result.isEmpty(), error.isEmpty() ? QStringLiteral("Could not query project.") : error);
                print(result);
            }
            return 0;
        }
        if (registration)
        {
            const QString label = parser.isSet(QStringLiteral("label"))
                                      ? parser.value(QStringLiteral("label"))
                                      : positional.value(0);
            require(!label.isEmpty(), QStringLiteral("--register requires a label after INPUT or --label LABEL."));
            const QString id = catalog->registerDocument(parser.value(QStringLiteral("register")), label,
                                                          QString(), &error);
            require(!id.isEmpty(), error.isEmpty() ? QStringLiteral("Could not register model source.") : error);
            print({{QStringLiteral("protocolVersion"), QStringLiteral("range.project/v1")},
                   {QStringLiteral("storageVersion"), RProjectCatalog::StorageVersion},
                   {QStringLiteral("projectId"), catalog->projectId()},
                   {QStringLiteral("persistentDocumentId"), id},
                   {QStringLiteral("label"), label}});
            return 0;
        }

        const auto registered = catalog->document(parser.value(QStringLiteral("document")), &error);
        require(registered.has_value(), error.isEmpty() ? QStringLiteral("Persistent document was not found.") : error);
        require(!registered->tombstone, QStringLiteral("Cannot snapshot a tombstoned document."));
        require(!registered->dirty, QStringLiteral("CLI snapshot requires a clean saved document."));
        const QString input = canonicalInput(parser.value(QStringLiteral("snapshot")));
        require(input == registered->sourcePath,
                QStringLiteral("Snapshot input must match the registered canonical source path."));
        const auto sourceBinding = RProjectCatalog::resolveSourceBinding(input, &error);
        require(sourceBinding.has_value(), error.isEmpty()
                    ? QStringLiteral("Registered native source is unavailable.") : error);
        bool sourceMatches = sourceBinding->resolvedSourcePath == registered->resolvedSourcePath
            && sourceBinding->savedArtifactHash == registered->savedArtifactHash
            && sourceBinding->sourceArtifacts.size() == registered->sourceArtifacts.size();
        for (qsizetype index = 0; sourceMatches && index < sourceBinding->sourceArtifacts.size(); ++index)
            sourceMatches = sourceBinding->sourceArtifacts[index].path == registered->sourceArtifacts[index].path
                && sourceBinding->sourceArtifacts[index].sha256 == registered->sourceArtifacts[index].sha256;
        require(sourceMatches, QStringLiteral("Registered source bytes changed; explicit reload is required."));
        RProjectCatalog::InstanceRef instanceRef;
        instanceRef.documentId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        instanceRef.sessionEpoch = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        instanceRef.documentRevision = 0;
        QString errorCode;
        auto instanceLease = catalog->acquireDocumentInstance(registered->persistentDocumentId,
                                                               instanceRef, &error, &errorCode);
        require(bool(instanceLease), error.isEmpty()
                    ? QStringLiteral("Could not acquire the document instance lease.") : error);
        RModel model;
        model.read(sourceBinding->resolvedSourcePath);
        RProjectCatalog::SnapshotArtifact artifact;
        require(catalog->captureSnapshot(registered->persistentDocumentId, model, instanceRef,
                                         &artifact, &error, registered->sourceRevision,
                                         &errorCode, instanceLease),
                error.isEmpty() ? QStringLiteral("Could not publish model snapshot.") : error);
        print({{QStringLiteral("protocolVersion"), QStringLiteral("range.project/v1")},
               {QStringLiteral("storageVersion"), RProjectCatalog::StorageVersion},
               {QStringLiteral("artifact"), artifact.toJson()}});
        return 0;
    }
    catch (const std::exception &exception)
    {
        QTextStream(stderr) << "range-project: " << QString::fromUtf8(exception.what()) << Qt::endl;
        return 2;
    }
    catch (const RError &exception)
    {
        QTextStream(stderr) << "range-project: " << exception.getMessage() << Qt::endl;
        return 2;
    }
}
