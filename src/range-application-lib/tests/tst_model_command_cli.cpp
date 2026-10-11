#include <QtTest>
#include <rbl_interaction_record_v1.h>
#include <rmodel_command_session.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QVector>

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

QString makeNativeStressInput(const QString &path, QString &error)
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    model.setNNodes(2);
    model.getNode(0).set(0.0, 0.0, 0.0);
    model.getNode(1).set(1.0, 0.0, 0.0);
    model.setNPoints(2);
    for (uint i = 0; i < 2; ++i)
    {
        model.getPoint(i).setName(QStringLiteral("Repeated point"));
        RElement point(R_ELEMENT_POINT);
        point.setNodeId(0, i);
        model.addElement(point, true, i);
    }
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
    void sharedCommandsAndStableTargetPagesRoundTrip();
    void dryRunDoesNotWriteModelOrOutput();
    void publishesAsciiAndBinaryModels_data();
    void publishesAsciiAndBinaryModels();
    void rejectsInvalidConfigurations_data();
    void rejectsInvalidConfigurations();
    void rejectsSourceAliasedOutputPathsAndReadsLinkedModels();
    void rejectsBusyOutputAndFailedOutputPath();
    void journalTracksPreviewRejectionAndPublication();
    void failedJournalPreventsPublication_data();
    void failedJournalPreventsPublication();
    void reconcilesInterruptedPublicationInFreshProcess_data();
    void reconcilesInterruptedPublicationInFreshProcess();
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

void TestModelCommandCli::sharedCommandsAndStableTargetPagesRoundTrip()
{
    QCOMPARE(RModel::version.toString(), QStringLiteral("1.3.21"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString setupError;
    const QString input = makeNativeStressInput(directory.filePath(QStringLiteral("stress.ram")), setupError);
    QVERIFY2(!input.isEmpty(), qPrintable(setupError));

    QByteArray original;
    QVERIFY(readBytes(input, original));
    const QByteArray originalHash = sourceHash(original);

    const auto capabilitiesResult = runCli({QStringLiteral("--input"), input,
                                             QStringLiteral("--capabilities")});
    QVERIFY2(exitedWith(capabilitiesResult, 0), qPrintable(capabilitiesResult.diagnostic()));
    QJsonObject capabilities;
    QString parseError;
    QVERIFY2(parseOutputObject(capabilitiesResult, capabilities, parseError), qPrintable(parseError));
    QVERIFY(capabilities.value(QStringLiteral("commands")).isArray());

    const auto contextResult = runCli({QStringLiteral("--input"), input, QStringLiteral("--context")});
    QVERIFY2(exitedWith(contextResult, 0), qPrintable(contextResult.diagnostic()));
    QJsonObject contextResponse;
    QVERIFY2(parseOutputObject(contextResult, contextResponse, parseError), qPrintable(parseError));
    QCOMPARE(contextResponse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(contextResponse.value(QStringLiteral("sourceSha256")).toString(), QString::fromLatin1(originalHash));

    const QString firstPage = directory.filePath(QStringLiteral("first-page.json"));
    QVERIFY(writeBytes(firstPage, QJsonDocument(QJsonObject{{QStringLiteral("limit"), 1}})
                                      .toJson(QJsonDocument::Compact)));
    const auto firstPageResult = runCli({QStringLiteral("--input"), input,
        QStringLiteral("--targets"), QStringLiteral("--page"), firstPage});
    QVERIFY2(exitedWith(firstPageResult, 0), qPrintable(firstPageResult.diagnostic()));
    QJsonObject firstPageResponse;
    QVERIFY2(parseOutputObject(firstPageResult, firstPageResponse, parseError), qPrintable(parseError));
    QCOMPARE(firstPageResponse.value(QStringLiteral("targets")).toArray().size(), 1);
    QVERIFY(firstPageResponse.value(QStringLiteral("hasMore")).toBool());
    const QString oldCursor = firstPageResponse.value(QStringLiteral("nextCursor")).toString();
    QVERIFY(!oldCursor.isEmpty());

    const QString secondPage = directory.filePath(QStringLiteral("second-page.json"));
    QVERIFY(writeBytes(secondPage, QJsonDocument(QJsonObject{{QStringLiteral("cursor"), oldCursor}})
                                       .toJson(QJsonDocument::Compact)));
    const auto secondPageResult = runCli({QStringLiteral("--input"), input,
        QStringLiteral("--targets"), QStringLiteral("--page"), secondPage});
    QVERIFY2(exitedWith(secondPageResult, 0), qPrintable(secondPageResult.diagnostic()));
    QJsonObject secondPageResponse;
    QVERIFY2(parseOutputObject(secondPageResult, secondPageResponse, parseError), qPrintable(parseError));
    QCOMPARE(secondPageResponse.value(QStringLiteral("targets")).toArray().size(), 1);
    QCOMPARE(secondPageResponse.value(QStringLiteral("targets")).toArray().first().toObject()
                 .value(QStringLiteral("id")).toString(), QStringLiteral("g:2"));

    const QString labelPage = directory.filePath(QStringLiteral("label-page.json"));
    QVERIFY(writeBytes(labelPage, QJsonDocument(QJsonObject{{QStringLiteral("labelQuery"), QStringLiteral("Repeated")},
        {QStringLiteral("limit"), 10}}).toJson(QJsonDocument::Compact)));
    const auto labelResult = runCli({QStringLiteral("--input"), input, QStringLiteral("--targets"),
        QStringLiteral("--page"), labelPage});
    QVERIFY2(exitedWith(labelResult, 0), qPrintable(labelResult.diagnostic()));
    QJsonObject labelResponse;
    QVERIFY2(parseOutputObject(labelResult, labelResponse, parseError), qPrintable(parseError));
    QVERIFY(labelResponse.value(QStringLiteral("ambiguous")).toBool());
    QCOMPARE(labelResponse.value(QStringLiteral("candidateIds")).toArray().size(), 2);

    const QString dynamicsParameters = directory.filePath(QStringLiteral("dynamics.json"));
    const QString dynamicsOutput = directory.filePath(QStringLiteral("dynamics.ram"));
    const QJsonObject dynamics{{QStringLiteral("enabled"), true}, {QStringLiteral("method"), QStringLiteral("newmark")},
        {QStringLiteral("timeStep"), 0.01}, {QStringLiteral("unitSI"), true},
        {QStringLiteral("steps"), 4}, {QStringLiteral("outputFrequency"), 2},
        {QStringLiteral("rayleighAlpha"), 0.0}, {QStringLiteral("rayleighBeta"), 0.0},
        {QStringLiteral("rhoInfinity"), 1.0}, {QStringLiteral("newmarkGamma"), 0.5},
        {QStringLiteral("newmarkBeta"), 0.25}, {QStringLiteral("modalDampingRatios"), QJsonArray{}}};
    QVERIFY(writeBytes(dynamicsParameters, parameterDocument(originalHash, dynamics)));
    const auto dynamicsResult = runCli({QStringLiteral("--input"), input,
        QStringLiteral("--command"), QStringLiteral("analysis.structural_dynamics.configure"),
        QStringLiteral("--parameters"), dynamicsParameters, QStringLiteral("--output"), dynamicsOutput});
    QVERIFY2(exitedWith(dynamicsResult, 0), qPrintable(dynamicsResult.diagnostic()));
    QJsonObject dynamicsResponse;
    QVERIFY2(parseOutputObject(dynamicsResult, dynamicsResponse, parseError), qPrintable(parseError));
    QCOMPARE(dynamicsResponse.value(QStringLiteral("status")).toString(), QStringLiteral("published"));

    RModel dynamicsModel;
    QVERIFY2(readOutputModel(dynamicsOutput, dynamicsModel, setupError), qPrintable(setupError));
    QVERIFY(dynamicsModel.getProblemSetup().getStructuralDynamicsSetup().getEnabled());

    QByteArray dynamicsBytes;
    QVERIFY(readBytes(dynamicsOutput, dynamicsBytes));
    const QString materialParameters = directory.filePath(QStringLiteral("material.json"));
    const QString materialOutput = directory.filePath(QStringLiteral("material.ram"));
    const QJsonObject material{{QStringLiteral("targetId"), QStringLiteral("g:1")},
        {QStringLiteral("name"), QStringLiteral("Steel")}, {QStringLiteral("youngModulusPa"), 2.0e11},
        {QStringLiteral("densityKgM3"), 8000.0}, {QStringLiteral("poissonRatio"), 0.3},
        {QStringLiteral("thermalExpansionPerK"), 1.2e-5}};
    QVERIFY(writeBytes(materialParameters, parameterDocument(sourceHash(dynamicsBytes), material)));
    const auto materialResult = runCli({QStringLiteral("--input"), dynamicsOutput,
        QStringLiteral("--command"), QStringLiteral("entity.material.set"),
        QStringLiteral("--parameters"), materialParameters, QStringLiteral("--output"), materialOutput});
    QVERIFY2(exitedWith(materialResult, 0), qPrintable(materialResult.diagnostic()));
    QJsonObject materialResponse;
    QVERIFY2(parseOutputObject(materialResult, materialResponse, parseError), qPrintable(parseError));
    QCOMPARE(materialResponse.value(QStringLiteral("status")).toString(), QStringLiteral("published"));

    QByteArray materialBytes;
    QVERIFY(readBytes(materialOutput, materialBytes));
    const QString boundaryParameters = directory.filePath(QStringLiteral("boundary.json"));
    const QString boundaryOutput = directory.filePath(QStringLiteral("boundary.ram"));
    const QJsonObject boundary{{QStringLiteral("targetId"), QStringLiteral("g:1")},
        {QStringLiteral("kind"), QStringLiteral("force")},
        {QStringLiteral("values"), QJsonArray{1.0, QJsonValue(QJsonValue::Null), -2.0}},
        {QStringLiteral("unit"), QStringLiteral("N")}};
    QVERIFY(writeBytes(boundaryParameters, parameterDocument(sourceHash(materialBytes), boundary)));
    const auto boundaryResult = runCli({QStringLiteral("--input"), materialOutput,
        QStringLiteral("--command"), QStringLiteral("entity.boundary.set"),
        QStringLiteral("--parameters"), boundaryParameters, QStringLiteral("--output"), boundaryOutput});
    QVERIFY2(exitedWith(boundaryResult, 0), qPrintable(boundaryResult.diagnostic()));
    QJsonObject boundaryResponse;
    QVERIFY2(parseOutputObject(boundaryResult, boundaryResponse, parseError), qPrintable(parseError));
    QCOMPARE(boundaryResponse.value(QStringLiteral("status")).toString(), QStringLiteral("published"));

    RModel boundaryModel;
    QVERIFY2(readOutputModel(boundaryOutput, boundaryModel, setupError), qPrintable(setupError));
    QCOMPARE(boundaryModel.getPoint(0).getPhysicalId(), 1u);
    QCOMPARE(boundaryModel.getPoint(1).getPhysicalId(), 2u);
    QCOMPARE(boundaryModel.getPoint(0).getMaterial().getName(), QStringLiteral("Steel"));
    QCOMPARE(boundaryModel.getPoint(0).getNBoundaryConditions(), 1u);
    const auto &force = boundaryModel.getPoint(0).getBoundaryCondition(0);
    QCOMPARE(force.getType(), R_BOUNDARY_CONDITION_FORCE);
    const uint forceY = force.findComponentPosition(R_VARIABLE_FORCE_Y);
    QVERIFY(!force.getComponent(forceY).getEnabled());

    QByteArray inputAfter;
    QVERIFY(readBytes(input, inputAfter));
    QCOMPARE(inputAfter, original);
    QVERIFY(originalHash != sourceHash(dynamicsBytes));

    const QString wrongHashParameters = directory.filePath(QStringLiteral("wrong-hash.json"));
    const QString rejectedOutput = directory.filePath(QStringLiteral("rejected.ram"));
    QVERIFY(writeBytes(wrongHashParameters, parameterDocument(QByteArray(64, '0'), boundary)));
    const auto wrongHashResult = runCli({QStringLiteral("--input"), materialOutput,
        QStringLiteral("--command"), QStringLiteral("entity.boundary.set"),
        QStringLiteral("--parameters"), wrongHashParameters, QStringLiteral("--output"), rejectedOutput});
    QVERIFY2(exitedWith(wrongHashResult, 2), qPrintable(wrongHashResult.diagnostic()));
    QJsonObject wrongHashResponse;
    QVERIFY2(parseOutputObject(wrongHashResult, wrongHashResponse, parseError), qPrintable(parseError));
    QCOMPARE(wrongHashResponse.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    QVERIFY(wrongHashResponse.value(QStringLiteral("message")).toString().contains(QStringLiteral("Stale source hash")));
    QVERIFY(!QFileInfo::exists(rejectedOutput));

    const QByteArray outputSentinel("leave rejected output untouched");
    QVERIFY(writeBytes(rejectedOutput, outputSentinel));
    const QString wrongUnitParameters = directory.filePath(QStringLiteral("wrong-unit.json"));
    QJsonObject wrongUnit = boundary;
    wrongUnit.insert(QStringLiteral("unit"), QStringLiteral("kN"));
    QVERIFY(writeBytes(wrongUnitParameters, parameterDocument(sourceHash(materialBytes), wrongUnit)));
    const auto wrongUnitResult = runCli({QStringLiteral("--input"), materialOutput,
        QStringLiteral("--command"), QStringLiteral("entity.boundary.set"),
        QStringLiteral("--parameters"), wrongUnitParameters, QStringLiteral("--output"), rejectedOutput});
    QVERIFY2(exitedWith(wrongUnitResult, 2), qPrintable(wrongUnitResult.diagnostic()));
    QJsonObject wrongUnitResponse;
    QVERIFY2(parseOutputObject(wrongUnitResult, wrongUnitResponse, parseError), qPrintable(parseError));
    QCOMPARE(wrongUnitResponse.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_units"));
    QByteArray rejectedOutputBytes;
    QVERIFY(readBytes(rejectedOutput, rejectedOutputBytes));
    QCOMPARE(rejectedOutputBytes, outputSentinel);

    const QString extraFieldParameters = directory.filePath(QStringLiteral("extra-field.json"));
    QJsonObject extraField = boundary;
    extraField.insert(QStringLiteral("unadvertised"), true);
    QVERIFY(writeBytes(extraFieldParameters, parameterDocument(sourceHash(materialBytes), extraField)));
    const auto extraFieldResult = runCli({QStringLiteral("--input"), materialOutput,
        QStringLiteral("--command"), QStringLiteral("entity.boundary.set"),
        QStringLiteral("--parameters"), extraFieldParameters, QStringLiteral("--output"), rejectedOutput});
    QVERIFY2(exitedWith(extraFieldResult, 2), qPrintable(extraFieldResult.diagnostic()));
    QJsonObject extraFieldResponse;
    QVERIFY2(parseOutputObject(extraFieldResult, extraFieldResponse, parseError), qPrintable(parseError));
    QCOMPARE(extraFieldResponse.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_parameters"));
    QVERIFY(readBytes(rejectedOutput, rejectedOutputBytes));
    QCOMPARE(rejectedOutputBytes, outputSentinel);
    QByteArray materialAfterRejectedCommands;
    QVERIFY(readBytes(materialOutput, materialAfterRejectedCommands));
    QCOMPARE(materialAfterRejectedCommands, materialBytes);

    const auto staleCursorResult = runCli({QStringLiteral("--input"), dynamicsOutput,
        QStringLiteral("--targets"), QStringLiteral("--page"), secondPage});
    QVERIFY2(exitedWith(staleCursorResult, 2), qPrintable(staleCursorResult.diagnostic()));
    QJsonObject staleCursorResponse;
    QVERIFY2(parseOutputObject(staleCursorResult, staleCursorResponse, parseError), qPrintable(parseError));
    QCOMPARE(staleCursorResponse.value(QStringLiteral("code")).toString(), QStringLiteral("stale_target_cursor"));
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

void TestModelCommandCli::rejectsSourceAliasedOutputPathsAndReadsLinkedModels()
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

    const QString middle = directory.filePath(QStringLiteral("middle.ram"));
    const QString link = directory.filePath(QStringLiteral("linked.ram"));
    RModel::writeLink(middle, input);
    RModel::writeLink(link, middle);
    const QStringList sourceChain{link, middle, input};
    QVector<QByteArray> originalChain;
    for (const QString &artifact : sourceChain)
    {
        QByteArray bytes;
        QVERIFY(readBytes(artifact, bytes));
        originalChain.append(bytes);
    }

    const auto linkResult = runCli({QStringLiteral("--input"), link, QStringLiteral("--query")});
    QVERIFY2(exitedWith(linkResult, 0), qPrintable(linkResult.diagnostic()));
    QJsonObject linkResponse;
    QString parseError;
    QVERIFY2(parseOutputObject(linkResult, linkResponse, parseError), qPrintable(parseError));
    QCOMPARE(linkResponse.value(QStringLiteral("sourceSha256")).toString(), QString::fromLatin1(sourceHash(originalInput)));
    QVERIFY(linkResponse.value(QStringLiteral("canConfigureModal")).toBool());
    const auto linkedTargetsResult = runCli({QStringLiteral("--input"), link, QStringLiteral("--targets")});
    QVERIFY2(exitedWith(linkedTargetsResult, 0), qPrintable(linkedTargetsResult.diagnostic()));
    QJsonObject linkedTargets;
    QVERIFY2(parseOutputObject(linkedTargetsResult, linkedTargets, parseError), qPrintable(parseError));
    QCOMPARE(linkedTargets.value(QStringLiteral("sourceId")).toString(),
             QFileInfo(link).canonicalFilePath() + QLatin1Char('#') + QString::fromLatin1(sourceHash(originalInput)));
    QCOMPARE(linkedTargets.value(QStringLiteral("revision")).toString(), QString::fromLatin1(sourceHash(originalInput)));
    for (int i = 0; i < sourceChain.size(); ++i)
    {
        QByteArray after;
        QVERIFY(readBytes(sourceChain.at(i), after));
        QCOMPARE(after, originalChain.at(i));
    }

    for (const QString &aliasedOutput : sourceChain)
    {
        const auto result = runCli({QStringLiteral("--input"), link,
                                    QStringLiteral("--parameters"), parameterPath,
                                    QStringLiteral("--output"), aliasedOutput});
        QVERIFY2(exitedWith(result, 2), qPrintable(result.diagnostic()));
        QJsonObject response;
        QVERIFY2(parseOutputObject(result, response, parseError), qPrintable(parseError));
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
        for (int i = 0; i < sourceChain.size(); ++i)
        {
            QByteArray after;
            QVERIFY(readBytes(sourceChain.at(i), after));
            QCOMPARE(after, originalChain.at(i));
        }
        QByteArray afterParameters;
        QVERIFY(readBytes(parameterPath, afterParameters));
        QCOMPARE(afterParameters, parameterBytes);
    }
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

void TestModelCommandCli::journalTracksPreviewRejectionAndPublication()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), error);
    QVERIFY2(!input.isEmpty(), qPrintable(error));
    QByteArray original;
    QVERIFY(readBytes(input, original));
    const QString parameters = directory.filePath(QStringLiteral("parameters.json"));
    const QString output = directory.filePath(QStringLiteral("published.ram"));
    const QString journal = directory.filePath(QStringLiteral("journal.json"));
    QVERIFY(writeBytes(parameters, parameterDocument(sourceHash(original), requestedModalParameters())));
    const QStringList base{QStringLiteral("--input"), input, QStringLiteral("--parameters"), parameters,
                           QStringLiteral("--journal"), journal};
    auto result = runCli(base + QStringList{QStringLiteral("--dry-run")});
    QVERIFY2(exitedWith(result, 0), qPrintable(result.diagnostic()));
    QVERIFY(!QFileInfo::exists(output));
    auto invalid = requestedModalParameters();
    invalid["unit"] = QStringLiteral("Hz");
    QVERIFY(writeBytes(parameters, parameterDocument(sourceHash(original), invalid)));
    result = runCli(base + QStringList{QStringLiteral("--output"), output});
    QVERIFY2(exitedWith(result, 2), qPrintable(result.diagnostic()));
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(writeBytes(parameters, parameterDocument(sourceHash(original), requestedModalParameters())));
    result = runCli(base + QStringList{QStringLiteral("--output"), output});
    QVERIFY2(exitedWith(result, 0), qPrintable(result.diagnostic()));
    QJsonObject response;
    QVERIFY2(parseOutputObject(result, response, error), qPrintable(error));
    QCOMPARE(response["status"].toString(), QStringLiteral("published"));
    QByteArray outputBytes, journalBytes, sourceAfter;
    QVERIFY(readBytes(output, outputBytes));
    QVERIFY(readBytes(journal, journalBytes));
    QVERIFY(readBytes(input, sourceAfter));
    QCOMPARE(sourceAfter, original);
    const auto actualHash = QString::fromLatin1(sourceHash(outputBytes));
    QCOMPARE(response["outputSha256"].toString(), actualHash);
    const auto envelope = QJsonDocument::fromJson(journalBytes).object();
    QCOMPARE(envelope["journalVersion"].toInt(), 1);
    const auto records = envelope["records"].toArray();
    QCOMPARE(records.size(), 5);
    const QStringList statuses{QStringLiteral("preview"), QStringLiteral("rejected"),
                               QStringLiteral("accepted"), QStringLiteral("publication_pending"), QStringLiteral("published")};
    QUuid parent;
    RInteractionRecordV1 last;
    for (qsizetype index = 0; index < records.size(); ++index)
    {
        RInteractionRecordV1 record;
        QVERIFY2(RInteractionRecordV1::fromJson(QJsonDocument(records[index].toObject()).toJson(), record, &error), qPrintable(error));
        QCOMPARE(record.projectId, QUuid(envelope["projectId"].toString()));
        QCOMPARE(record.parentRecordId, parent);
        QCOMPARE(record.resultStatus, statuses[index]);
        QCOMPARE(record.actorId, QStringLiteral("range-model-command"));
        QCOMPARE(record.selectionSnapshot["sourceSha256"].toString(), QString::fromLatin1(sourceHash(original)));
        parent = record.recordId;
        last = record;
    }
    QCOMPARE(last.selectionSnapshot["outputSha256"].toString(), actualHash);
    QCOMPARE(last.artifactRefs[0].toObject()["sha256"].toString(), actualHash);
    QCOMPARE(last.selectionSnapshot["outputPath"].toString(), QFileInfo(output).canonicalFilePath());
    const auto intent = records[3].toObject()["selectionSnapshot"].toObject();
    QCOMPARE(intent["expectedOutputSha256"].toString(), actualHash);
    QCOMPARE(last.selectionSnapshot["publicationId"], intent["publicationId"]);
    const auto reopened = runCli({QStringLiteral("--input"), output, QStringLiteral("--query")});
    QVERIFY2(exitedWith(reopened, 0), qPrintable(reopened.diagnostic()));
    QJsonObject reopenedResponse;
    QVERIFY2(parseOutputObject(reopened, reopenedResponse, error), qPrintable(error));
    QCOMPARE(reopenedResponse["sourceSha256"].toString(), actualHash);
    QVERIFY(reopenedResponse["modelId"] != response["modelId"]);
    QCOMPARE(reopenedResponse["modal"], response["modal"]);
}

void TestModelCommandCli::failedJournalPreventsPublication_data()
{
    QTest::addColumn<bool>("locked");
    QTest::newRow("locked journal") << true;
    QTest::newRow("malformed journal") << false;
}

void TestModelCommandCli::failedJournalPreventsPublication()
{
    QFETCH(bool, locked);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const QString input = makeNativeInput(directory.filePath(QStringLiteral("input.ram")), error);
    QVERIFY2(!input.isEmpty(), qPrintable(error));
    QByteArray original;
    QVERIFY(readBytes(input, original));
    const QString parameters = directory.filePath(QStringLiteral("parameters.json"));
    const QString output = directory.filePath(QStringLiteral("published.ram"));
    const QString journal = directory.filePath(QStringLiteral("journal.json"));
    QVERIFY(writeBytes(parameters, parameterDocument(sourceHash(original), requestedModalParameters())));
    const QByteArray previousOutput("keep-existing-output");
    QVERIFY(writeBytes(output, previousOutput));
    QLockFile lock(journal + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (locked) QVERIFY(lock.tryLock(0));
    else QVERIFY(writeBytes(journal, QByteArrayLiteral("{truncated")));
    const auto result = runCli({QStringLiteral("--input"), input, QStringLiteral("--parameters"), parameters,
                                QStringLiteral("--output"), output, QStringLiteral("--journal"), journal});
    QVERIFY2(exitedWith(result, 2), qPrintable(result.diagnostic()));
    QJsonObject response;
    QVERIFY2(parseOutputObject(result, response, error), qPrintable(error));
    QCOMPARE(response["code"].toString(), QStringLiteral("journal_failure"));
    QByteArray outputAfter, sourceAfter, journalAfter;
    QVERIFY(readBytes(output, outputAfter));
    QVERIFY(readBytes(input, sourceAfter));
    QCOMPARE(outputAfter, previousOutput);
    QCOMPARE(sourceAfter, original);
    if (!locked)
    {
        QVERIFY(readBytes(journal, journalAfter));
        QCOMPARE(journalAfter, QByteArrayLiteral("{truncated"));
    }
}

void TestModelCommandCli::reconcilesInterruptedPublicationInFreshProcess_data()
{
    QTest::addColumn<QString>("caseName");
    QTest::addColumn<QString>("expectedStatus");
    QTest::addColumn<int>("expectedExitCode");
    QTest::newRow("interrupted before first commit") << QStringLiteral("absent") << QStringLiteral("not_published") << 0;
    QTest::newRow("interrupted before replacing old output") << QStringLiteral("unchanged") << QStringLiteral("not_published") << 0;
    QTest::newRow("interrupted after atomic commit") << QStringLiteral("committed") << QStringLiteral("artifact_present") << 0;
    QTest::newRow("output differs from intent and old bytes") << QStringLiteral("conflict") << QStringLiteral("conflict") << 3;
    QTest::newRow("same bytes predate intent") << QStringLiteral("identical") << QStringLiteral("already_present") << 0;
}

void TestModelCommandCli::reconcilesInterruptedPublicationInFreshProcess()
{
    QFETCH(QString, caseName);
    QFETCH(QString, expectedStatus);
    QFETCH(int, expectedExitCode);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString output = directory.filePath(QStringLiteral("output.ram"));
    const QString journalPath = directory.filePath(QStringLiteral("journal.json"));
    const QString stagedPath = directory.filePath(QStringLiteral("staged.ram"));
    QByteArray stagedBytes;
    QString error;
    {
        RModel model;
        model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
        RModelCommandSession::JournalOptions options;
        options.path = journalPath;
        RModelCommandSession interrupted(model, options);
        const auto actualStaged = interrupted.model().write(stagedPath, false);
        QVERIFY(readBytes(actualStaged, stagedBytes));
        if (caseName == QStringLiteral("identical")) QVERIFY(writeBytes(output, stagedBytes));
        else if (caseName != QStringLiteral("absent")) QVERIFY(writeBytes(output, QByteArrayLiteral("previous-output")));
        QVERIFY2(interrupted.recordPublicationIntent(output, actualStaged, &error), qPrintable(error));
        if (caseName == QStringLiteral("committed"))
        {
            QSaveFile published(output);
            published.setDirectWriteFallback(false);
            QVERIFY(published.open(QIODevice::WriteOnly));
            QCOMPARE(published.write(stagedBytes), stagedBytes.size());
            QVERIFY(published.commit());
        }
        else if (caseName == QStringLiteral("conflict")) QVERIFY(writeBytes(output, QByteArrayLiteral("unexpected-writer")));
        // End the original session without publication acknowledgement. The
        // persisted states are exactly the two sides of the commit crash window.
    }
    QVERIFY(QFile::remove(stagedPath)); // Recovery cannot rely on staging surviving.
    const bool existedBefore = QFileInfo::exists(output);
    QByteArray before;
    if (existedBefore) QVERIFY(readBytes(output, before));
    const auto result = runCli({QStringLiteral("--reconcile"), QStringLiteral("--journal"), journalPath});
    QVERIFY2(exitedWith(result, expectedExitCode), qPrintable(result.diagnostic()));
    QJsonObject response;
    QVERIFY2(parseOutputObject(result, response, error), qPrintable(error));
    QCOMPARE(response["status"].toString(), QStringLiteral("reconciled"));
    QCOMPARE(response["apiVersion"].toString(), QStringLiteral("range.application/v1"));
    QVERIFY(response["observationOnly"].toBool());
    const auto outcomes = response["outcomes"].toArray();
    QCOMPARE(outcomes.size(), 1);
    QCOMPARE(outcomes[0].toObject()["status"].toString(), expectedStatus);
    QCOMPARE(outcomes[0].toObject()["expectedOutputSha256"].toString(), QString::fromLatin1(sourceHash(stagedBytes)));
    QCOMPARE(QFileInfo::exists(output), existedBefore);
    if (existedBefore)
    {
        QByteArray after;
        QVERIFY(readBytes(output, after));
        QCOMPARE(after, before);
    }
    QByteArray journalBytes;
    QVERIFY(readBytes(journalPath, journalBytes));
    const auto records = QJsonDocument::fromJson(journalBytes).object()["records"].toArray();
    QCOMPARE(records.size(), 2);
    RInteractionRecordV1 observation;
    QVERIFY2(RInteractionRecordV1::fromJson(QJsonDocument(records[1].toObject()).toJson(), observation, &error), qPrintable(error));
    QCOMPARE(observation.commandId, QStringLiteral("model.artifact.reconciled"));
    QCOMPARE(observation.resultStatus, expectedStatus);
    QVERIFY(observation.selectionSnapshot["observationOnly"].toBool());
    const auto again = runCli({QStringLiteral("--reconcile"), QStringLiteral("--journal"), journalPath});
    QVERIFY2(exitedWith(again, caseName == QStringLiteral("conflict") ? 3 : 0), qPrintable(again.diagnostic()));
    QVERIFY2(parseOutputObject(again, response, error), qPrintable(error));
    if (caseName == QStringLiteral("conflict"))
    {
        QCOMPARE(response["outcomes"].toArray().size(), 1);
        QCOMPARE(response["outcomes"].toArray()[0].toObject()["status"].toString(), QStringLiteral("conflict"));
        QVERIFY(response["outcomes"].toArray()[0].toObject()["alreadyRecorded"].toBool());
    }
    else QVERIFY(response["outcomes"].toArray().isEmpty());
    QByteArray afterRetry;
    QVERIFY(readBytes(journalPath, afterRetry));
    QCOMPARE(afterRetry, journalBytes);
}

QTEST_GUILESS_MAIN(TestModelCommandCli)

#include "tst_model_command_cli.moc"
