#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <vector>

#include <rml_file_utils.h>
#include <rml_model.h>

#include "rnative_result_broker.h"

namespace
{

QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject provenance(const QString &jobId = QStringLiteral("job-001"))
{
    const QString hash = sha(QByteArrayLiteral("test-input"));
    return QJsonObject{
        {QStringLiteral("jobId"), jobId},
        {QStringLiteral("proofScope"), QStringLiteral("solver-and-declared-controller-runtime/v1")},
        {QStringLiteral("sourceRuntimeStamp"), QJsonObject{
            {QStringLiteral("documentId"), QStringLiteral("doc-1")},
            {QStringLiteral("revision"), QStringLiteral("4")}}},
        {QStringLiteral("sourceSnapshotRef"), QJsonObject{
            {QStringLiteral("projectId"), QStringLiteral("project-1")},
            {QStringLiteral("sourceRevision"), QStringLiteral("4")}}},
        {QStringLiteral("inputSnapshotSha256"), hash},
        {QStringLiteral("inputPackageSha256"), sha(QByteArrayLiteral("package-input"))},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("fingerprintSha256"), sha(QByteArrayLiteral("runtime"))},
            {QStringLiteral("scope"), QStringLiteral("native-solver-build/v1")}}},
        {QStringLiteral("control"), QJsonObject{
            {QStringLiteral("enabled"), false},
            {QStringLiteral("controllerId"), QString()},
            {QStringLiteral("controllerVersionSha256"), QString()}}}
    };
}

void addVector(RModel &model, RVariableType type, double scale, bool missingUnits = false)
{
    RVariable variable(type, R_VARIABLE_APPLY_NODE);
    variable.resize(3, model.getNNodes());
    for (uint node = 0; node < model.getNNodes(); ++node)
    {
        variable.setValue(0, node, scale + double(node));
        variable.setValue(1, node, 10.0 + scale + double(node));
        variable.setValue(2, node, 20.0 + scale + double(node));
    }
    if (missingUnits) variable.setUnits(QString());
    model.addVariable(variable);
}

RModel dynamicModel(int recordNo,
                    const QVector<uint> &nodeIds = {1, 2},
                    bool missingDisplacementUnit = false,
                    bool includeVelocity = true,
                    bool includeAcceleration = true)
{
    RModel model;
    model.setName(QStringLiteral("result-frame"));
    model.setNNodes(uint(nodeIds.size()));
    for (int index = 0; index < nodeIds.size(); ++index)
    {
        model.setNode(uint(index), RNode(double(index), 0.0, 0.0));
        model.setNodePhysicalId(uint(index), uint(1000 + index));
    }
    for (int index = 0; index < nodeIds.size(); ++index)
        model.setNodePhysicalId(uint(index), nodeIds.at(index));
    model.setNElements(1);
    RElement truss(R_ELEMENT_TRUSS1);
    truss.setNodeId(0, 0);
    truss.setNodeId(1, 1);
    model.setElement(0, truss);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));

    std::vector<double> times;
    for (int step = 1; step <= recordNo; ++step) times.push_back(0.1 * step);
    RTimeSolver &time = model.getTimeSolver();
    time.setTimes(times);
    time.setEnabled(true);
    time.setInputNTimeSteps(uint(recordNo));
    time.setInputStartTime(0.1);
    time.setInputTimeStepSize(0.1);
    time.setCurrentTimeStep(uint(recordNo - 1));

    RStructuralDynamicsSetup &dynamics = model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true);
    dynamics.setTimeStep(0.1);
    dynamics.setNSteps(uint(recordNo));
    dynamics.setOutputFrequency(1);

    const double frameScale = 10.0 * time.getCurrentTime();
    addVector(model, R_VARIABLE_DISPLACEMENT, frameScale, missingDisplacementUnit);
    if (includeVelocity) addVector(model, R_VARIABLE_VELOCITY, 2.0 * frameScale);
    if (includeAcceleration) addVector(model, R_VARIABLE_ACCELERATION, 3.0 * frameScale);

    RStructuralDynamicsRestartData checkpoint;
    checkpoint.available = true;
    checkpoint.completedSteps = uint(recordNo);
    checkpoint.time = time.getCurrentTime();
    checkpoint.fixedTimeStep = 0.1;
    checkpoint.meshNodeCount = model.getNNodes();
    checkpoint.meshElementCount = model.getNElements();
    checkpoint.physicalDofCount = model.getNNodes();
    checkpoint.meshFingerprint = RStructuralDynamicsRestartData::computeMeshFingerprint(model);
    checkpoint.dofFingerprint = QString(64, QLatin1Char('a'));
    checkpoint.systemFingerprint = QString(64, QLatin1Char('b'));
    checkpoint.q.assign(model.getNNodes(), 0.0);
    checkpoint.v.assign(model.getNNodes(), 0.0);
    checkpoint.a.assign(model.getNNodes(), 0.0);
    checkpoint.externalForce.assign(model.getNNodes(), 0.0);
    model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
    return model;
}

RModel staticModel()
{
    RModel model;
    model.setName(QStringLiteral("static-result"));
    model.setNNodes(2);
    model.setNode(0, RNode(0.0, 0.0, 0.0));
    model.setNode(1, RNode(1.0, 0.0, 0.0));
    model.setNElements(1);
    RElement truss(R_ELEMENT_TRUSS1);
    truss.setNodeId(0, 0);
    truss.setNodeId(1, 1);
    model.setElement(0, truss);
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS));
    addVector(model, R_VARIABLE_DISPLACEMENT, 0.0);
    return model;
}

QString writeDynamicFrame(const QString &directory, int recordNo,
                          const QVector<uint> &nodeIds = {1, 2},
                          bool missingUnit = false,
                          bool includeVelocity = true,
                          bool includeAcceleration = true)
{
    RModel model = dynamicModel(recordNo, nodeIds, missingUnit, includeVelocity, includeAcceleration);
    const QString logicalPath = QDir(directory).filePath(QStringLiteral("run.rbm"));
    model.write(logicalPath, true);
    return logicalPath;
}

QString framePath(const QString &directory, int recordNo)
{
    return QDir(directory).filePath(QStringLiteral("run-%1.rbm").arg(recordNo, 5, 10, QLatin1Char('0')));
}

class NativeResultBrokerTest : public QObject
{
    Q_OBJECT

private slots:
    void publishesBoundedSeriesAndStableSummary();
    void preservesSparseOutputAndOptionalFieldAvailability();
    void rejectsPartialFrameAndReorderedNodeMetadata();
    void resolvesReorderedVariableMetadataByEnum();
    void rejectsExternalLinkAndMissingUnits();
    void rejectsStaleManifestAndTamperedFrame();
    void catalogsStaticRawModelButRejectsTimeSeries();
    void catalogsModalResultAndRejectsTimeSeries();
    void summarizesExtremeFiniteValuesWithoutOverflow();
    void derivesSourceKindFromPersistedControlSetup();
};

void NativeResultBrokerTest::publishesBoundedSeriesAndStableSummary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logicalPath = writeDynamicFrame(directory.path(), 1);
    writeDynamicFrame(directory.path(), 2);

    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QCOMPARE(published.manifest.value(QStringLiteral("apiVersion")).toString(),
             QStringLiteral("range.native-results/v1"));
    QCOMPARE(published.manifest.value(QStringLiteral("sourceKind")).toString(),
             QStringLiteral("native-passive-fe"));
    QCOMPARE(published.manifest.value(QStringLiteral("analysis")).toObject()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("structural-dynamics"));

    const QJsonObject catalog = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("catalog"),
        QJsonObject{{QStringLiteral("limit"), 1}});
    QVERIFY(catalog.value(QStringLiteral("valid")).toBool());
    QCOMPARE(catalog.value(QStringLiteral("frameCount")).toInt(), 2);
    QCOMPARE(catalog.value(QStringLiteral("frames")).toArray().at(0).toObject()
                 .value(QStringLiteral("recordNo")).toInt(), 1);
    QVERIFY(catalog.contains(QStringLiteral("nextCursor")));

    const QJsonObject channels = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("channels"),
        QJsonObject{{QStringLiteral("limit"), 1}});
    QVERIFY(channels.value(QStringLiteral("valid")).toBool());
    QCOMPARE(channels.value(QStringLiteral("channelCount")).toInt(), 18);
    QCOMPARE(channels.value(QStringLiteral("channels")).toArray().at(0).toObject()
                 .value(QStringLiteral("channelId")).toString(), QStringLiteral("node:1:displacement:X"));
    QCOMPARE(channels.value(QStringLiteral("channels")).toArray().at(0).toObject()
                 .value(QStringLiteral("unit")).toString(), QStringLiteral("m"));

    const QString channelId = QStringLiteral("node:2:displacement:X");
    const QJsonObject firstPage = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), channelId}, {QStringLiteral("limit"), 1}});
    QVERIFY(firstPage.value(QStringLiteral("valid")).toBool());
    const QJsonObject firstSample = firstPage.value(QStringLiteral("samples")).toArray().at(0).toObject();
    QCOMPARE(firstSample.value(QStringLiteral("recordNo")).toInt(), 1);
    QCOMPARE(firstSample.value(QStringLiteral("timeS")).toDouble(), 0.1);
    QCOMPARE(firstSample.value(QStringLiteral("value")).toDouble(), 2.0);

    const QJsonObject yAxis = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:2:displacement:Y")},
                    {QStringLiteral("limit"), 1}});
    QVERIFY(yAxis.value(QStringLiteral("valid")).toBool());
    QCOMPARE(yAxis.value(QStringLiteral("samples")).toArray().at(0).toObject()
                 .value(QStringLiteral("value")).toDouble(), 12.0);
    const QJsonObject zAxis = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:2:displacement:Z")},
                    {QStringLiteral("limit"), 1}});
    QVERIFY(zAxis.value(QStringLiteral("valid")).toBool());
    QCOMPARE(zAxis.value(QStringLiteral("samples")).toArray().at(0).toObject()
                 .value(QStringLiteral("value")).toDouble(), 22.0);

    const QJsonObject lastPage = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), channelId},
                    {QStringLiteral("cursor"), firstPage.value(QStringLiteral("nextCursor"))}});
    QVERIFY(lastPage.value(QStringLiteral("valid")).toBool());
    QCOMPARE(lastPage.value(QStringLiteral("samples")).toArray().at(0).toObject()
                 .value(QStringLiteral("recordNo")).toInt(), 2);

    const QJsonObject summary = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("summary"),
        QJsonObject{{QStringLiteral("channelId"), channelId}});
    QVERIFY(summary.value(QStringLiteral("valid")).toBool());
    QCOMPARE(summary.value(QStringLiteral("sampleCount")).toInt(), 2);
    QCOMPARE(summary.value(QStringLiteral("mean")).toDouble(), 2.5);
    QVERIFY(std::fabs(summary.value(QStringLiteral("rms")).toDouble() - std::sqrt(6.5)) < 1e-12);
    QCOMPARE(summary.value(QStringLiteral("min")).toDouble(), 2.0);
    QCOMPARE(summary.value(QStringLiteral("max")).toDouble(), 3.0);
}

void NativeResultBrokerTest::preservesSparseOutputAndOptionalFieldAvailability()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logicalPath = writeDynamicFrame(directory.path(), 1, {1, 2}, false, true, true);
    writeDynamicFrame(directory.path(), 3, {1, 2}, false, false, true);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonArray frames = published.manifest.value(QStringLiteral("frames")).toArray();
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0).toObject().value(QStringLiteral("recordNo")).toInt(), 1);
    QCOMPARE(frames.at(1).toObject().value(QStringLiteral("recordNo")).toInt(), 3);
    QVERIFY(!published.manifest.value(QStringLiteral("availableVariables")).toArray()
                 .contains(QStringLiteral("var-velocity")));
    const QJsonObject unavailable = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:1:velocity:X")}});
    QVERIFY(!unavailable.value(QStringLiteral("valid")).toBool());
    QCOMPARE(unavailable.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
}

void NativeResultBrokerTest::rejectsPartialFrameAndReorderedNodeMetadata()
{
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString logicalPath = writeDynamicFrame(directory.path(), 1);
        writeDynamicFrame(directory.path(), 2);
        QFile partial(framePath(directory.path(), 2));
        QVERIFY(partial.open(QIODevice::ReadWrite));
        QVERIFY(partial.resize(12));
        partial.close();
        const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
        QVERIFY(!published.valid);
        QCOMPARE(published.code, QStringLiteral("PARTIAL_NATIVE_FILE"));
    }

    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString logicalPath = writeDynamicFrame(directory.path(), 1, {1, 2});
        writeDynamicFrame(directory.path(), 2, {2, 1});
        const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
        QVERIFY(!published.valid);
        QCOMPARE(published.code, QStringLiteral("INCONSISTENT_FRAME_METADATA"));
    }
}

void NativeResultBrokerTest::resolvesReorderedVariableMetadataByEnum()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logicalPath = writeDynamicFrame(directory.path(), 1);
    RModel reordered = dynamicModel(2);
    const RVariable displacement(reordered.getVariable(reordered.findVariable(R_VARIABLE_DISPLACEMENT)));
    const RVariable velocity(reordered.getVariable(reordered.findVariable(R_VARIABLE_VELOCITY)));
    const RVariable acceleration(reordered.getVariable(reordered.findVariable(R_VARIABLE_ACCELERATION)));
    reordered.removeAllVariables();
    reordered.addVariable(acceleration);
    reordered.addVariable(velocity);
    reordered.addVariable(displacement);
    reordered.write(QDir(directory.path()).filePath(QStringLiteral("run.rbm")), true);

    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonObject series = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:2:displacement:X")},
                    {QStringLiteral("offset"), 1},
                    {QStringLiteral("limit"), 1}});
    QVERIFY(series.value(QStringLiteral("valid")).toBool());
    QCOMPARE(series.value(QStringLiteral("samples")).toArray().at(0).toObject()
                 .value(QStringLiteral("recordNo")).toInt(), 2);
}

void NativeResultBrokerTest::rejectsExternalLinkAndMissingUnits()
{
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString logicalPath = writeDynamicFrame(directory.path(), 1, {1, 2}, true);
        const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
        QVERIFY(!published.valid);
        QCOMPARE(published.code, QStringLiteral("INVALID_RESULT_FIELD"));
    }

    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString logicalPath = writeDynamicFrame(directory.path(), 1);
        const QString external = QDir(directory.path()).filePath(QStringLiteral(".."));
        const QString outsideTarget = QDir(external).filePath(QStringLiteral("outside.rbm"));
        RModel::writeLink(logicalPath, outsideTarget);
        const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
        QVERIFY(!published.valid);
        QCOMPARE(published.code, QStringLiteral("UNSAFE_RESULT_PATH"));
    }
}

void NativeResultBrokerTest::rejectsStaleManifestAndTamperedFrame()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logicalPath = writeDynamicFrame(directory.path(), 1);
    writeDynamicFrame(directory.path(), 2);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));

    const QJsonObject stale = RNativeResultBroker::query(published.manifestPath,
        QString(64, QLatin1Char('0')), QStringLiteral("catalog"), QJsonObject{});
    QVERIFY(!stale.value(QStringLiteral("valid")).toBool());
    QCOMPARE(stale.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_MANIFEST_SHA256"));

    QFile tampered(framePath(directory.path(), 1));
    QVERIFY(tampered.open(QIODevice::WriteOnly | QIODevice::Append));
    QCOMPARE(tampered.write(QByteArrayLiteral("changed")), qint64(7));
    tampered.close();
    const QJsonObject series = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:1:displacement:X")},
                    {QStringLiteral("limit"), 1}});
    QVERIFY(!series.value(QStringLiteral("valid")).toBool());
    QCOMPARE(series.value(QStringLiteral("code")).toString(), QStringLiteral("RESULT_FRAME_TAMPERED"));
}

void NativeResultBrokerTest::catalogsStaticRawModelButRejectsTimeSeries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = staticModel();
    const QString logicalPath = QDir(directory.path()).filePath(QStringLiteral("static.rbm"));
    model.write(logicalPath, true);
    const QString rawModelPath = RFileUtils::getFileNameWithTimeStep(logicalPath, 0);
    const auto published = RNativeResultBroker::publishResult(directory.path(), rawModelPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QCOMPARE(published.manifest.value(QStringLiteral("logicalLatest")).toObject()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("MODEL"));
    QCOMPARE(published.manifest.value(QStringLiteral("analysis")).toObject()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("static"));
    QCOMPARE(published.manifest.value(QStringLiteral("frames")).toArray().size(), 0);

    const QJsonObject catalog = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("catalog"), QJsonObject{});
    QVERIFY(catalog.value(QStringLiteral("valid")).toBool());
    QCOMPARE(catalog.value(QStringLiteral("analysis")).toObject()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("static"));
    const QJsonObject series = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:1:displacement:X")}});
    QVERIFY(!series.value(QStringLiteral("valid")).toBool());
    QCOMPARE(series.value(QStringLiteral("code")).toString(), QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"));
}

void NativeResultBrokerTest::catalogsModalResultAndRejectsTimeSeries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = staticModel();
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    model.getProblemSetup().getModalSetup().setMode(0);
    const QString logicalPath = QDir(directory.path()).filePath(QStringLiteral("modal.rbm"));
    model.write(logicalPath, true);

    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QCOMPARE(published.manifest.value(QStringLiteral("analysis")).toObject()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("modal"));
    QCOMPARE(published.manifest.value(QStringLiteral("analysis")).toObject()
                 .value(QStringLiteral("modeIndex")).toInt(), 0);
    QCOMPARE(published.manifest.value(QStringLiteral("frames")).toArray().size(), 0);
    const QJsonObject series = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("series"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:1:displacement:X")}});
    QVERIFY(!series.value(QStringLiteral("valid")).toBool());
    QCOMPARE(series.value(QStringLiteral("code")).toString(), QStringLiteral("UNSUPPORTED_ANALYSIS_MODE"));
}

void NativeResultBrokerTest::summarizesExtremeFiniteValuesWithoutOverflow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logicalPath = QDir(directory.path()).filePath(QStringLiteral("extreme.rbm"));
    RModel positive = dynamicModel(1);
    uint displacement = positive.findVariable(R_VARIABLE_DISPLACEMENT);
    positive.getVariable(displacement).setValue(0, 0, 1.0e308);
    positive.write(logicalPath, true);

    RModel negative = dynamicModel(2);
    displacement = negative.findVariable(R_VARIABLE_DISPLACEMENT);
    negative.getVariable(displacement).setValue(0, 0, -1.0e308);
    negative.write(logicalPath, true);

    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonObject summary = RNativeResultBroker::query(published.manifestPath,
        published.manifestSha256, QStringLiteral("summary"),
        QJsonObject{{QStringLiteral("channelId"), QStringLiteral("node:1:displacement:X")}});
    QVERIFY(summary.value(QStringLiteral("valid")).toBool());
    QCOMPARE(summary.value(QStringLiteral("mean")).toDouble(), 0.0);
    QCOMPARE(summary.value(QStringLiteral("rms")).toDouble(), 1.0e308);
    QCOMPARE(summary.value(QStringLiteral("min")).toDouble(), -1.0e308);
    QCOMPARE(summary.value(QStringLiteral("max")).toDouble(), 1.0e308);
}

void NativeResultBrokerTest::derivesSourceKindFromPersistedControlSetup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = dynamicModel(1);
    RStructuralControlSetup &control = model.getProblemSetup().getStructuralControlSetup();
    control.enabled = true;
    control.controllerId = QStringLiteral("test-controller");
    RStructuralDynamicsRestartData checkpoint = model.getProblemSetup().getStructuralDynamicsRestartData();
    checkpoint.controlAvailable = true;
    checkpoint.controlFingerprint = sha(QByteArrayLiteral("control-map"));
    checkpoint.controlChannelIds = {QStringLiteral("actuator-x")};
    checkpoint.controlLastCommands = {0.0};
    checkpoint.controlPowers = {0.0};
    checkpoint.cumulativeControlWork = {0.0};
    model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
    const QString logicalPath = QDir(directory.path()).filePath(QStringLiteral("active.rbm"));
    model.write(logicalPath, true);

    QJsonObject activeProvenance = provenance(QStringLiteral("job-active"));
    activeProvenance.insert(QStringLiteral("control"), QJsonObject{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("controllerId"), QStringLiteral("test-controller")},
        {QStringLiteral("controllerVersionSha256"), sha(QByteArrayLiteral("controller-policy"))}});
    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, activeProvenance);
    QVERIFY2(published.valid, qPrintable(published.message));
    QCOMPARE(published.manifest.value(QStringLiteral("sourceKind")).toString(),
             QStringLiteral("native-active-fe"));
}

} // namespace

QTEST_GUILESS_MAIN(NativeResultBrokerTest)
#include "tst_native_result_broker.moc"
