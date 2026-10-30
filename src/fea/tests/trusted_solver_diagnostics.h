#ifndef TRUSTED_SOLVER_DIAGNOSTICS_H
#define TRUSTED_SOLVER_DIAGNOSTICS_H

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>

// Test-only capture. Never serialize the process environment or application settings.
class TrustedSolverDiagnostics
{
public:
    static QString directoryTemplate()
    {
        const QString root = qEnvironmentVariable("RANGE_TEST_DIAGNOSTIC_ROOT",
            QDir::tempPath() + QStringLiteral("/range-trusted-diagnostics-20261027"));
        if (!QDir().mkpath(root)) return {};
        return QDir(root).filePath(QStringLiteral("authorized-repair-XXXXXX"));
    }

    static QString sha256(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file)) return {};
        return QString::fromLatin1(hash.result().toHex());
    }

    static bool writeJson(const QString &path, const QJsonObject &value)
    {
        QSaveFile file(path);
        const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    }

    static bool append(const QString &path, const QString &text)
    {
        QFile file(path);
        const QByteArray bytes = text.toUtf8();
        return file.open(QIODevice::WriteOnly | QIODevice::Append)
            && file.write(bytes) == bytes.size() && file.flush();
    }

    // Missing outputs are evidence too. Copy only the isolated solver artifact tree;
    // the runtime/profile is retained separately and is never recursively exported.
    static bool captureJob(const QString &bundle, const QJsonObject &record,
                           const QString &executable, const QString &taskServer, uint threads)
    {
        const QString jobId = record.value(QStringLiteral("jobId")).toString();
        if (jobId.isEmpty() || jobId.contains('/') || jobId.contains('\\') || jobId.contains("..")) return false;
        const QString destination = QDir(bundle).filePath(QStringLiteral("children/") + jobId);
        if (QFileInfo::exists(destination)) return false;
        if (!QDir().mkpath(destination)) return false;
        QJsonObject captured;
        for (const char *key : {"jobId", "state", "processStarted", "processExited", "processExitCode",
                               "nativeProcessId", "inputSnapshotRef", "inputSnapshotSha256", "inputPackageSha256",
                               "resultArtifactRef", "resultArtifactSha256", "resultArtifactVerified",
                               "resultManifestRef", "resultManifestSha256", "resultManifestVerified",
                               "logArtifactRefs", "nativeResultProvenance", "message"})
            captured.insert(QString::fromLatin1(key), record.value(QString::fromLatin1(key)));
        const QJsonObject logs = record.value(QStringLiteral("logArtifactRefs")).toObject();
        const QStringList arguments{
            QStringLiteral("--file=") + record.value(QStringLiteral("resultArtifactRef")).toString(),
            QStringLiteral("--log-file=") + logs.value(QStringLiteral("solverLog")).toString(),
            QStringLiteral("--convergence-file=") + logs.value(QStringLiteral("convergence")).toString(),
            QStringLiteral("--monitoring-file=") + logs.value(QStringLiteral("monitoring")).toString(),
            QStringLiteral("--nthreads=") + QString::number(threads),
            QStringLiteral("--task-id=") + jobId,
            QStringLiteral("--task-server=") + taskServer};
        captured.insert(QStringLiteral("command"), QJsonObject{
            {QStringLiteral("executable"), executable},
            {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
            {QStringLiteral("workingDirectory"), QDir::currentPath()},
            {QStringLiteral("captureScope"), QStringLiteral("reconstructed modal initArguments; no native/checkpoint flags")},
            {QStringLiteral("solverExecutableSha256"), sha256(executable)}});
        const QString input = record.value(QStringLiteral("inputSnapshotRef")).toString();
        QJsonArray inventory;
        bool complete = true;
        bool inputCopied = false;
        const QFileInfo inputInfo(input);
        const QString inputHash = sha256(input);
        const QString declaredInputHash = record.value(QStringLiteral("inputSnapshotSha256")).toString();
        // initFiles puts the frozen input in <job>/input and results/logs in sibling directories.
        if (input.isEmpty() || !inputInfo.isAbsolute() || !inputInfo.isFile()
            || inputInfo.isSymLink() || inputHash.isEmpty()
            || (!declaredInputHash.isEmpty() && declaredInputHash != inputHash)) complete = false;
        else
        {
            const QDir source(QFileInfo(input).absolutePath() + QStringLiteral("/.."));
            QDirIterator files(source.absolutePath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                               QDirIterator::Subdirectories);
            while (files.hasNext())
            {
                files.next();
                const QFileInfo info = files.fileInfo();
                if (info.isSymLink()) { complete = false; continue; }
                const QString relative = source.relativeFilePath(info.absoluteFilePath());
                const QString target = QDir(destination).filePath(QStringLiteral("artifacts/") + relative);
                const QString sourceHash = sha256(info.absoluteFilePath());
                const bool copied = QDir().mkpath(QFileInfo(target).absolutePath())
                    && QFile::copy(info.absoluteFilePath(), target);
                const QString capturedHash = copied ? sha256(target) : QString();
                const bool matches = copied && !sourceHash.isEmpty() && capturedHash == sourceHash;
                if (info.absoluteFilePath() == inputInfo.absoluteFilePath())
                    inputCopied = matches && sourceHash == inputHash;
                inventory.append(QJsonObject{{QStringLiteral("path"), relative},
                    {QStringLiteral("bytes"), double(info.size())}, {QStringLiteral("sha256"), sourceHash},
                    {QStringLiteral("capturedSha256"), capturedHash}, {QStringLiteral("copied"), matches}});
                complete = complete && matches;
            }
        }
        complete = complete && inputCopied && !inventory.isEmpty();
        QJsonArray outputs;
        for (const QString &key : {QStringLiteral("resultArtifactRef"), QStringLiteral("resultManifestRef")})
        {
            const QString path = record.value(key).toString();
            outputs.append(QJsonObject{{QStringLiteral("role"), key}, {QStringLiteral("path"), path},
                {QStringLiteral("exists"), !path.isEmpty() && QFileInfo(path).isFile()},
                {QStringLiteral("sha256"), path.isEmpty() ? QString() : sha256(path)}});
        }
        for (auto it = logs.begin(); it != logs.end(); ++it)
        {
            const QString path = it.value().toString();
            outputs.append(QJsonObject{{QStringLiteral("role"), it.key()}, {QStringLiteral("path"), path},
                {QStringLiteral("exists"), !path.isEmpty() && QFileInfo(path).isFile()},
                {QStringLiteral("sha256"), path.isEmpty() ? QString() : sha256(path)}});
        }
        captured.insert(QStringLiteral("outputs"), outputs);
        captured.insert(QStringLiteral("files"), inventory);
        captured.insert(QStringLiteral("copyComplete"), complete);
        return writeJson(QDir(destination).filePath(QStringLiteral("diagnostic.json")), captured) && complete;
    }
};

#endif
