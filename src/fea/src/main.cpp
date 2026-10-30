#include <rbl_arguments_parser.h>
#include <rbl_logger.h>
#include <QDir>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <algorithm>
#include <array>
#include <cstdio>

#include "application.h"
#include "solver_manager.h"

int main(int argc, char *argv[])
{
    const bool trustedEvidenceReopenMode = argc == 2
        && QByteArray(argv[1]) == QByteArrayLiteral("--trusted-evidence-reopen");
    if (trustedEvidenceReopenMode)
    {
        // Keep the machine-readable, read-only evidence mode out of the GUI
        // command-line parser. Its sole input is a bounded JSON request on stdin.
        argc = 1;
        argv[1] = nullptr;
    }

    // An explicit portable profile keeps settings, sessions and application
    // data together. Validate it before constructing any application settings.
    const QString profile = qEnvironmentVariable("RANGE_APPLICATION_PROFILE_DIR");
    if (!profile.isEmpty())
    {
        const QFileInfo directory(profile);
        bool valid = QDir::isAbsolutePath(profile) && directory.isDir()
                     && !directory.isSymLink()
                     && QDir::cleanPath(directory.absoluteFilePath()).compare(
                            QDir::cleanPath(directory.canonicalFilePath()), Qt::CaseInsensitive) == 0;
        QString ancestor = directory.absoluteFilePath();
        while (valid)
        {
            const QFileInfo entry(ancestor);
            valid = entry.exists() && entry.isDir() && !entry.isSymLink();
            const QString parent = entry.absolutePath();
            if (parent == ancestor) break;
            ancestor = parent;
        }
        // Existing profiles may contain linked settings or data below an
        // ordinary root. Check the complete existing tree before any writes.
        QDirIterator entries(profile, QDir::AllEntries | QDir::NoDotAndDotDot
                             | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        const QString prefix = QDir::cleanPath(directory.canonicalFilePath()) + '/';
        while (valid && entries.hasNext())
        {
            entries.next();
            const QFileInfo entry = entries.fileInfo();
            valid = !entry.isSymLink() && !entry.isJunction()
                    && !entry.canonicalFilePath().isEmpty()
                    && QDir::cleanPath(entry.canonicalFilePath()).startsWith(prefix, Qt::CaseInsensitive);
        }
        for (const QString &name : {QStringLiteral("settings"), QStringLiteral("system-settings"),
                                    QStringLiteral("application-data")})
        {
            if (!valid) break;
            const QString path = QDir(profile).filePath(name);
            valid = QDir().mkpath(path);
            const QFileInfo entry(path);
            valid = valid && entry.isDir() && !entry.isSymLink() && !entry.isJunction()
                    && !entry.canonicalFilePath().isEmpty()
                    && QDir::cleanPath(entry.canonicalFilePath()).startsWith(prefix, Qt::CaseInsensitive);
        }
        if (!valid)
        {
            std::fprintf(stderr, "RANGE_APPLICATION_PROFILE_DIR requires an existing absolute directory without symbolic aliases.\n");
            return 2;
        }
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QDir(profile).filePath("settings"));
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                           QDir(profile).filePath("system-settings"));
    }
    if (!trustedEvidenceReopenMode) RArgumentsParser::printHeader("GUI");

    QLocale::setDefault(QLocale::c());

    // Uncomment in case there are some issues with fractional scaling
//    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);

#ifdef Q_OS_WIN
    // Keep file selection in visible Qt dialogs, avoiding native shell
    // extension loading into the long-lived engineering GUI process.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif

    int exitValue = 0;
    Application application(argc, argv);
    if (trustedEvidenceReopenMode)
    {
        constexpr size_t maximumRequestBytes = 8 * 1024 * 1024;
        QByteArray requestBytes;
        std::array<char, 8192> buffer{};
        bool inputValid = true;
        while (true)
        {
            const size_t count = std::fread(buffer.data(), 1, buffer.size(), stdin);
            if (count > 0)
            {
                if (requestBytes.size() > qint64(maximumRequestBytes) - qint64(count))
                {
                    inputValid = false;
                    break;
                }
                requestBytes.append(buffer.data(), qsizetype(count));
            }
            if (count < buffer.size())
            {
                if (std::ferror(stdin)) inputValid = false;
                break;
            }
        }

        QJsonParseError parseError;
        const QJsonDocument document = inputValid
            ? QJsonDocument::fromJson(requestBytes, &parseError) : QJsonDocument{};
        const QJsonObject request = document.object();
        const auto keysOf = [](const QJsonObject &object) {
            QSet<QString> keys;
            for (const QString &key : object.keys()) keys.insert(key);
            return keys;
        };
        const QSet<QString> baseKeys{QStringLiteral("jobId"), QStringLiteral("artifactId"),
            QStringLiteral("manifestSha256"), QStringLiteral("expectedCaptureIdentity"),
            QStringLiteral("problemSpec")};
        const QSet<QString> singleTopKeys{QStringLiteral("schema"), QStringLiteral("jobId"),
            QStringLiteral("artifactId"), QStringLiteral("manifestSha256"),
            QStringLiteral("expectedCaptureIdentity"), QStringLiteral("problemSpec")};
        const QSet<QString> batchTopKeys{QStringLiteral("schema"), QStringLiteral("requests")};
        const bool batchMode = request.value(QStringLiteral("schema"))
            == QLatin1String("range.trusted-artifact-reopen-batch/v1");
        QJsonArray requests;
        if (batchMode)
            requests = request.value(QStringLiteral("requests")).toArray();
        else
        {
            QJsonObject singleRequest = request;
            singleRequest.remove(QStringLiteral("schema"));
            requests.append(singleRequest);
        }
        bool requestValid = inputValid && document.isObject()
            && keysOf(request) == (batchMode ? batchTopKeys : singleTopKeys)
            && requests.size() > 0 && requests.size() <= 180;
        const QSet<QString> requiredIdentityKeys{QStringLiteral("jobId"), QStringLiteral("artifactId"),
            QStringLiteral("manifestSha256"), QStringLiteral("inputSnapshotSha256"),
            QStringLiteral("inputPackageSha256"), QStringLiteral("sourceSnapshotRefSha256"),
            QStringLiteral("normalizedProblemSpecSha256"), QStringLiteral("physicalSummarySha256"),
            QStringLiteral("trustedExperimentId"), QStringLiteral("trustedExperimentAttemptId"),
            QStringLiteral("trustedAuthorizationDigest")};
        for (const QJsonValue &requestValue : requests)
        {
            const QJsonObject item = requestValue.toObject();
            const QJsonObject expectedIdentity = item.value(QStringLiteral("expectedCaptureIdentity")).toObject();
            QSet<QString> allowedItemKeys = baseKeys;
            if (item.contains(QStringLiteral("slotId"))) allowedItemKeys.insert(QStringLiteral("slotId"));
            if (item.contains(QStringLiteral("includeConvergenceMeasurements")))
                allowedItemKeys.insert(QStringLiteral("includeConvergenceMeasurements"));
            requestValid = requestValid && requestValue.isObject()
                && keysOf(item) == allowedItemKeys
                && item.value(QStringLiteral("jobId")).isString()
                && !item.value(QStringLiteral("jobId")).toString().isEmpty()
                && item.value(QStringLiteral("artifactId")).isString()
                && !item.value(QStringLiteral("artifactId")).toString().isEmpty()
                && item.value(QStringLiteral("manifestSha256")).isString()
                && item.value(QStringLiteral("expectedCaptureIdentity")).isObject()
                && keysOf(expectedIdentity) == requiredIdentityKeys
                && item.value(QStringLiteral("problemSpec")).isObject()
                && (!item.contains(QStringLiteral("slotId"))
                    || (item.value(QStringLiteral("slotId")).isString()
                        && QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"))
                            .match(item.value(QStringLiteral("slotId")).toString()).hasMatch()))
                && (!item.contains(QStringLiteral("includeConvergenceMeasurements"))
                    || item.value(QStringLiteral("includeConvergenceMeasurements")).isBool());
        }
        QSet<QString> seenSlotIds;
        QSet<QString> seenArtifactBindings;
        for (const QJsonValue &requestValue : requests)
        {
            const QJsonObject item = requestValue.toObject();
            if (batchMode && !item.contains(QStringLiteral("slotId")))
                requestValid = false;
            if (!item.contains(QStringLiteral("slotId"))) continue;
            const QString slotId = item.value(QStringLiteral("slotId")).toString();
            if (seenSlotIds.contains(slotId))
                requestValid = false;
            seenSlotIds.insert(slotId);
            const QJsonArray artifactBinding{item.value(QStringLiteral("jobId")),
                                             item.value(QStringLiteral("artifactId"))};
            const QString bindingKey = QString::fromUtf8(
                QJsonDocument(artifactBinding).toJson(QJsonDocument::Compact));
            if (seenArtifactBindings.contains(bindingKey))
                requestValid = false;
            seenArtifactBindings.insert(bindingKey);
        }
        if (batchMode)
            requestValid = requestValid && requests.size() == request.value(QStringLiteral("requests")).toArray().size();
        else
            requestValid = requestValid && request.value(QStringLiteral("schema"))
                == QLatin1String("range.trusted-artifact-reopen-request/v1");
        if (!requestValid)
        {
            const QJsonObject error{{QStringLiteral("schema"), QStringLiteral("range.trusted-artifact-reopen/v1")},
                {QStringLiteral("valid"), false}, {QStringLiteral("artifactReverified"), false},
                {QStringLiteral("code"), QStringLiteral("INVALID_REQUEST")},
                {QStringLiteral("message"), inputValid
                    ? QStringLiteral("The trusted evidence reopen request is malformed or unsupported.")
                    : QStringLiteral("The request stream is unreadable or exceeds 8 MiB.")}};
            const QByteArray response = QJsonDocument(error).toJson(QJsonDocument::Compact);
            std::fwrite(response.constData(), 1, size_t(response.size()), stdout);
            std::fputc('\n', stdout);
            return 2;
        }

        // The job registry is profile scoped. Use the same in-process owner
        // path as the GUI, but do not show a window, configure a Provider, or
        // enter the event loop.
        application.initialize(false);
        QJsonArray results;
        bool allValid = true;
        for (const QJsonValue &requestValue : requests)
        {
            const QJsonObject item = requestValue.toObject();
            const QJsonObject result = SolverManager::getInstance().trustedResultVerification(
                item.value(QStringLiteral("jobId")).toString(),
                item.value(QStringLiteral("artifactId")).toString(),
                item.value(QStringLiteral("manifestSha256")).toString(),
                item.value(QStringLiteral("problemSpec")).toObject(),
                item.value(QStringLiteral("expectedCaptureIdentity")).toObject(),
                item.value(QStringLiteral("includeConvergenceMeasurements")).toBool(false));
            QJsonObject row = result;
            if (item.contains(QStringLiteral("slotId")))
                row.insert(QStringLiteral("slotId"), item.value(QStringLiteral("slotId")));
            allValid = allValid && result.value(QStringLiteral("valid")).toBool();
            results.append(row);
        }
        QJsonObject output;
        if (batchMode)
            output = {{QStringLiteral("schema"), QStringLiteral("range.trusted-artifact-reopen-batch/v1")},
                      {QStringLiteral("results"), results}};
        else
            output = results.first().toObject();
        const QByteArray response = QJsonDocument(output).toJson(QJsonDocument::Compact);
        if (response.size() > qint64(maximumRequestBytes))
        {
            const QJsonObject error{{QStringLiteral("schema"), QStringLiteral("range.trusted-artifact-reopen/v1")},
                {QStringLiteral("valid"), false}, {QStringLiteral("artifactReverified"), false},
                {QStringLiteral("code"), QStringLiteral("OUTPUT_LIMIT")},
                {QStringLiteral("message"), QStringLiteral("The verification response exceeds 8 MiB.")}};
            const QByteArray boundedError = QJsonDocument(error).toJson(QJsonDocument::Compact);
            std::fwrite(boundedError.constData(), 1, size_t(boundedError.size()), stdout);
            std::fputc('\n', stdout);
            return 3;
        }
        std::fwrite(response.constData(), 1, size_t(response.size()), stdout);
        std::fputc('\n', stdout);
        return batchMode || allValid ? 0 : 3;
    }

    QString evaluationCaptureError;
    if (!application.configureEvaluationCaptureFromEnvironment(&evaluationCaptureError))
    {
        std::fprintf(stderr, "Agent evaluation capture configuration failed: %s\n",
                     evaluationCaptureError.toUtf8().constData());
        return 2;
    }
    if ((exitValue = application.exec()) != 0)
    {
        RLogger::info("Application has terminated with error code (%d).\n", exitValue);
    }

    RArgumentsParser::printFooter();
    return exitValue;
} /* main */
