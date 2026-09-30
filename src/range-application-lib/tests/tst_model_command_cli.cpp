#include <QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>

#include <rbl_error.h>
#include <rml_modal_setup.h>
#include <rml_model.h>
#include <rml_problem_task_item.h>

namespace
{
struct ProcessResult
{
    bool started = false;
    bool finished = false;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
    QString error;

    QString diagnostic() const
    {
        return QStringLiteral("program: %1\nstarted: %2\nfinished: %3\nexit status: %4\nexit code: %5\n"
                              "process error: %6\nstdout:\n%7\nstderr:\n%8")
            .arg(commandPath())
            .arg(started ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(finished ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(exitStatus == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crashed"))
            .arg(exitCode)
            .arg(error)
            .arg(QString::fromUtf8(standardOutput))
            .arg(QString::fromUtf8(standardError));
    }

    static QString commandPath()
    {
#ifdef Q_OS_WIN
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("range-model-command.exe"));
#else
        return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("range-model-command"));
#endif
    }
};

QString cliPath()
{
    return ProcessResult::commandPath();
}

ProcessResult runCli(const QStringList &arguments)
{
    QProcess process;
    process.setProgram(cliPath());
    process.setArguments(arguments);
    process.start();

    ProcessResult result;
    if (!process.waitForStarted(5000))
    {
        result.error = process.errorString();
        result.standardOutput = process.readAllStandardOutput();
        result.standardError = process.readAllStandardError();
        return result;
    }
    result.started = true;

    if (!process.waitForFinished(30000))
    {
        result.error = QStringLiteral("Timed out waiting for range-model-command.");
        process.kill();
        process.waitForFinished(5000);
    }
    else
    {
        result.finished = true;
        result.exitStatus = process.exitStatus();
        result.exitCode = process.exitCode();
    }
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
}

bool exitedWith(const ProcessResult &result, int exitCode)
{
    return result.started && result.finished && result.exitStatus == QProcess::NormalExit
        && result.exitCode == exitCode;
}

bool parseOutputObject(const ProcessResult &result, QJsonObject &object, QString &error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(result.standardOutput, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("CLI stdout is not a JSON object: %1").arg(parseError.errorString());
        return false;
    }
    object = document.object();
    return true;
}

bool readBytes(const QString &path, QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bytes = file.readAll();
    return file.error() == QFileDevice::NoError;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size() && file.flush();
}

QString makeNativeInput(const QString &path, QString &error)
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    model.addNode(RNode(1.25, -2.5, 3.75));
    auto &modal = model.getProblemSetup().getModalSetup();
    modal.setMethod(R_MODAL_DOMINANT_MODE);
    modal.setNIterations(37);
    modal.setNModesToExtract(2);
    modal.setConvergenceValue(2.5e-7);

    try
    {
        return model.write(path, false);
    }
    catch (const RError &exception)
    {
        error = exception.getMessage();
    }
    return {};
}

QByteArray sourceHash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QJsonObject requestedModalParameters()
{
    return {{"method", QStringLiteral("multiple")}, {"iterations", 321}, {"modes", 7},
            {"tolerance", 3.5e-8}, {"unit", QStringLiteral("1")}};
}

QByteArray parameterDocument(const QByteArray &hash, const QJsonObject &parameters,
                             const QString &rootExtra = QString())
{
    QJsonObject document{{"sourceSha256", QString::fromLatin1(hash)}, {"parameters", parameters}};
    if (!rootExtra.isEmpty()) document.insert(rootExtra, true);
    return QJsonDocument(document).toJson(QJsonDocument::Compact);
}

bool readOutputModel(const QString &path, RModel &model, QString &error)
{
    try
    {
        model.read(path);
        return true;
    }
    catch (const RError &exception)
    {
        error = exception.getMessage();
    }
    return false;
}
}

class TestModelCommandCli : public QObject
{
    Q_OBJECT

private slots:
    void queryReturnsHashAndModalSettings();
    void dryRunDoesNotWriteModelOrOutput();
    void publishesAsciiAndBinaryModels_data();
    void publishesAsciiAndBinaryModels();
    void rejectsInvalidConfigurations_data();
    void rejectsInvalidConfigurations();
    void rejectsAliasedOutputsAndModelLinks();
    void rejectsBusyOutputAndFailedOutputPath();
};

void TestModelCommandCli::queryReturnsHashAndModalSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));

    QByteArray original;
    QVERIFY(readBytes(input, original));
    const auto result = runCli({QStringLiteral("--input"), input, QStringLiteral("--query")});
    QVERIFY2(exitedWith(result, 0), qPrintable(result.diagnostic()));

    QJsonObject response;
    QString parseError;
    QVERIFY2(parseOutputObject(result, response, parseError), qPrintable(parseError));
    QCOMPARE(response.value(QStringLiteral("sourceSha256")).toString(), QString::fromLatin1(sourceHash(original)));
    QVERIFY(response.value(QStringLiteral("canConfigureModal")).toBool());
    const auto modal = response.value(QStringLiteral("modal")).toObject();
    QCOMPARE(modal.value(QStringLiteral("method")).toString(), QStringLiteral("dominant"));
    QCOMPARE(modal.value(QStringLiteral("iterations")).toInt(), 37);
    QCOMPARE(modal.value(QStringLiteral("modes")).toInt(), 2);
    QVERIFY(qFuzzyCompare(modal.value(QStringLiteral("tolerance")).toDouble(), 2.5e-7));
    QCOMPARE(modal.value(QStringLiteral("unit")).toString(), QStringLiteral("1"));

    QByteArray after;
    QVERIFY(readBytes(input, after));
    QCOMPARE(after, original);
}

void TestModelCommandCli::dryRunDoesNotWriteModelOrOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));

    QByteArray original;
    QVERIFY(readBytes(input, original));
    const QString parameters = directory.filePath(QStringLiteral("parameters.json"));
    QVERIFY(writeBytes(parameters, parameterDocument(sourceHash(original), requestedModalParameters())));
    const QString output = directory.filePath(QStringLiteral("preview-output.ram"));

    const auto result = runCli({QStringLiteral("--input"), input,
                                QStringLiteral("--parameters"), parameters,
                                QStringLiteral("--dry-run")});
    QVERIFY2(exitedWith(result, 0), qPrintable(result.diagnostic()));
    QJsonObject response;
    QString parseError;
    QVERIFY2(parseOutputObject(result, response, parseError), qPrintable(parseError));
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("preview"));
    QCOMPARE(response.value(QStringLiteral("sourceSha256")).toString(), QString::fromLatin1(sourceHash(original)));
    QCOMPARE(response.value(QStringLiteral("modal")).toObject().value(QStringLiteral("modes")).toInt(), 7);
    QVERIFY(!QFileInfo::exists(output));

    QByteArray after;
    QVERIFY(readBytes(input, after));
    QCOMPARE(after, original);
}

void TestModelCommandCli::publishesAsciiAndBinaryModels_data()
{
    QTest::addColumn<QString>("inputExtension");
    QTest::addColumn<QString>("outputExtension");
    QTest::newRow("native ASCII") << QStringLiteral("ram") << QStringLiteral("ram");
    QTest::newRow("native binary") << QStringLiteral("rbm") << QStringLiteral("rbm");
}

void TestModelCommandCli::publishesAsciiAndBinaryModels()
{
    QFETCH(QString, inputExtension);
    QFETCH(QString, outputExtension);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.%1").arg(inputExtension)), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));

    QByteArray original;
    QVERIFY(readBytes(input, original));
    const QByteArray originalHash = sourceHash(original);
    const QString parameters = directory.filePath(QStringLiteral("parameters.json"));
    QVERIFY(writeBytes(parameters, parameterDocument(originalHash, requestedModalParameters())));
    const QString output = directory.filePath(QStringLiteral("published.%1").arg(outputExtension));

    const auto result = runCli({QStringLiteral("--input"), input,
                                QStringLiteral("--parameters"), parameters,
                                QStringLiteral("--output"), output});
    QVERIFY2(exitedWith(result, 0), qPrintable(result.diagnostic()));
    QJsonObject response;
    QString parseError;
    QVERIFY2(parseOutputObject(result, response, parseError), qPrintable(parseError));
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("published"));
    QCOMPARE(response.value(QStringLiteral("sourceSha256")).toString(), QString::fromLatin1(originalHash));
    QCOMPARE(QFileInfo(response.value(QStringLiteral("output")).toString()).canonicalFilePath(),
             QFileInfo(output).canonicalFilePath());
    QCOMPARE(response.value(QStringLiteral("modal")).toObject().value(QStringLiteral("modes")).toInt(), 7);

    QByteArray after;
    QVERIFY(readBytes(input, after));
    QCOMPARE(after, original);
    QByteArray publishedBytes;
    QVERIFY(readBytes(output, publishedBytes));
    QVERIFY(publishedBytes != original);

    RModel published;
    QVERIFY2(readOutputModel(output, published, setupError), qPrintable(setupError));
    QCOMPARE(published.getProblemTaskTree().getProblemTypeMask(), int(R_PROBLEM_STRESS_MODAL));
    QCOMPARE(published.getNNodes(), 1u);
    QCOMPARE(published.getNode(0).getX(), 1.25);
    QCOMPARE(published.getNode(0).getY(), -2.5);
    QCOMPARE(published.getNode(0).getZ(), 3.75);
    const auto &modal = published.getProblemSetup().getModalSetup();
    QCOMPARE(modal.getMethod(), R_MODAL_MULTIPLE_MODES);
    QCOMPARE(modal.getNIterations(), 321u);
    QCOMPARE(modal.getNModesToExtract(), 7u);
    QVERIFY(qFuzzyCompare(modal.getConvergenceValue(), 3.5e-8));
}

void TestModelCommandCli::rejectsInvalidConfigurations_data()
{
    QTest::addColumn<QString>("caseName");
    QTest::newRow("invalid unit") << QStringLiteral("invalid-unit");
    QTest::newRow("stale source hash") << QStringLiteral("stale-hash");
    QTest::newRow("unknown parameter") << QStringLiteral("unknown-parameter");
    QTest::newRow("unknown root field") << QStringLiteral("unknown-root-field");
}

void TestModelCommandCli::rejectsInvalidConfigurations()
{
    QFETCH(QString, caseName);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));

    QByteArray originalInput;
    QVERIFY(readBytes(input, originalInput));
    const QByteArray originalHash = sourceHash(originalInput);
    QJsonObject parameters = requestedModalParameters();
    QByteArray requestHash = originalHash;
    QString rootExtra;
    if (caseName == QStringLiteral("invalid-unit"))
        parameters.insert(QStringLiteral("unit"), QStringLiteral("Hz"));
    else if (caseName == QStringLiteral("stale-hash"))
        requestHash = QByteArray(64, '0');
    else if (caseName == QStringLiteral("unknown-parameter"))
        parameters.insert(QStringLiteral("unexpected"), 1);
    else if (caseName == QStringLiteral("unknown-root-field"))
        rootExtra = QStringLiteral("unexpected");

    const QString parameterPath = directory.filePath(QStringLiteral("parameters.json"));
    QVERIFY(writeBytes(parameterPath, parameterDocument(requestHash, parameters, rootExtra)));
    const QString output = directory.filePath(QStringLiteral("preexisting.ram"));
    const QByteArray originalOutput("keep-existing-output-bytes");
    QVERIFY(writeBytes(output, originalOutput));

    const auto result = runCli({QStringLiteral("--input"), input,
                                QStringLiteral("--parameters"), parameterPath,
                                QStringLiteral("--output"), output});
    QVERIFY2(exitedWith(result, 2), qPrintable(result.diagnostic()));
    QJsonObject response;
    QString parseError;
    QVERIFY2(parseOutputObject(result, response, parseError), qPrintable(parseError));
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));

    QByteArray afterInput, afterOutput;
    QVERIFY(readBytes(input, afterInput));
    QVERIFY(readBytes(output, afterOutput));
    QCOMPARE(afterInput, originalInput);
    QCOMPARE(afterOutput, originalOutput);
}

void TestModelCommandCli::rejectsAliasedOutputsAndModelLinks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));
    QByteArray originalInput;
    QVERIFY(readBytes(input, originalInput));
    const QString parameterPath = directory.filePath(QStringLiteral("parameters.json"));
    const QByteArray parameterBytes = parameterDocument(sourceHash(originalInput), requestedModalParameters());
    QVERIFY(writeBytes(parameterPath, parameterBytes));

    for (const QString &aliasedOutput : {input, parameterPath})
    {
        const auto result = runCli({QStringLiteral("--input"), input,
                                    QStringLiteral("--parameters"), parameterPath,
                                    QStringLiteral("--output"), aliasedOutput});
        QVERIFY2(exitedWith(result, 2), qPrintable(result.diagnostic()));
        QJsonObject response;
        QString parseError;
        QVERIFY2(parseOutputObject(result, response, parseError), qPrintable(parseError));
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));

        QByteArray afterInput, afterParameters;
        QVERIFY(readBytes(input, afterInput));
        QVERIFY(readBytes(parameterPath, afterParameters));
        QCOMPARE(afterInput, originalInput);
        QCOMPARE(afterParameters, parameterBytes);
    }

    const QString link = directory.filePath(QStringLiteral("linked.ram"));
    RModel::writeLink(link, input);
    const auto linkResult = runCli({QStringLiteral("--input"), link, QStringLiteral("--query")});
    QVERIFY2(exitedWith(linkResult, 2), qPrintable(linkResult.diagnostic()));
    QJsonObject linkResponse;
    QString parseError;
    QVERIFY2(parseOutputObject(linkResult, linkResponse, parseError), qPrintable(parseError));
    QCOMPARE(linkResponse.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    QByteArray afterInput;
    QVERIFY(readBytes(input, afterInput));
    QCOMPARE(afterInput, originalInput);
}

void TestModelCommandCli::rejectsBusyOutputAndFailedOutputPath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));
    QByteArray originalInput;
    QVERIFY(readBytes(input, originalInput));
    const QString parameterPath = directory.filePath(QStringLiteral("parameters.json"));
    QVERIFY(writeBytes(parameterPath, parameterDocument(sourceHash(originalInput), requestedModalParameters())));

    const QString busyOutput = directory.filePath(QStringLiteral("busy.ram"));
    const QByteArray originalOutput("busy-output-must-survive");
    QVERIFY(writeBytes(busyOutput, originalOutput));
    QLockFile outputLock(busyOutput + QStringLiteral(".lock"));
    outputLock.setStaleLockTime(0);
    QVERIFY(outputLock.tryLock(0));

    const auto busyResult = runCli({QStringLiteral("--input"), input,
                                    QStringLiteral("--parameters"), parameterPath,
                                    QStringLiteral("--output"), busyOutput});
    QVERIFY2(exitedWith(busyResult, 2), qPrintable(busyResult.diagnostic()));
    QJsonObject busyResponse;
    QString parseError;
    QVERIFY2(parseOutputObject(busyResult, busyResponse, parseError), qPrintable(parseError));
    QCOMPARE(busyResponse.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    outputLock.unlock();

    QByteArray afterBusyOutput, afterBusyInput;
    QVERIFY(readBytes(busyOutput, afterBusyOutput));
    QVERIFY(readBytes(input, afterBusyInput));
    QCOMPARE(afterBusyOutput, originalOutput);
    QCOMPARE(afterBusyInput, originalInput);

    const QString blocker = directory.filePath(QStringLiteral("not-a-directory"));
    QVERIFY(writeBytes(blocker, QByteArrayLiteral("file blocks output directory")));
    const QString impossibleOutput = QDir(blocker).filePath(QStringLiteral("result.ram"));
    const auto failedPathResult = runCli({QStringLiteral("--input"), input,
                                          QStringLiteral("--parameters"), parameterPath,
                                          QStringLiteral("--output"), impossibleOutput});
    QVERIFY2(exitedWith(failedPathResult, 2), qPrintable(failedPathResult.diagnostic()));
    QJsonObject failedPathResponse;
    QVERIFY2(parseOutputObject(failedPathResult, failedPathResponse, parseError), qPrintable(parseError));
    QCOMPARE(failedPathResponse.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    QVERIFY(!QFileInfo::exists(impossibleOutput));

    QByteArray blockerAfter, inputAfter;
    QVERIFY(readBytes(blocker, blockerAfter));
    QVERIFY(readBytes(input, inputAfter));
    QCOMPARE(blockerAfter, QByteArrayLiteral("file blocks output directory"));
    QCOMPARE(inputAfter, originalInput);
}

QTEST_GUILESS_MAIN(TestModelCommandCli)

#include "tst_model_command_cli.moc"
