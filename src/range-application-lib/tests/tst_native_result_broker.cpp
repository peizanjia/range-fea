#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <functional>
#include <limits>
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

void addComputableTrussGroup(RModel &model, uint elementId = 0)
{
    model.setNLines(1);
    model.getLine(0).add(elementId);
    RMaterial material(RMaterial::Solid);
    for (const RMaterialProperty::Type type : {RMaterialProperty::ModulusOfElasiticity,
         RMaterialProperty::PoissonRatio, RMaterialProperty::Density, RMaterialProperty::ThermalExpansionCoefficient})
    {
        RMaterialProperty property(type);
        property.add(293.15, 1.0);
        material.add(property);
    }
    model.getLine(0).setMaterial(material);
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
    addComputableTrussGroup(model);
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
    void rejectsTamperedUnrequestedFrameAtEveryQueryBoundary();
    void exposesRotationsOnlyAtBeamNodes();
    void exposesTranslationsOnlyAtComputableNodes();
    void rejectsMissingOrInvalidTranslationNodeMaps();
    void rejectsTranslationAvailabilityDriftAcrossFrames();
    void preservesVerifiedHistoryOriginAndRejectsAncestorTampering();
    void marksLegacyHistoryOriginUnavailable();
    void queriesOnlyCompleteLiveFrames();
    void preservesMixedHistoryProofsAcrossSeriesPages();
    void snapshotsAllFramesAndRequestedChannelsInOrder();
    void rejectsInvalidSnapshotArgumentsWithoutPartialResults();
    void rejectsMalformedAndOversizedSnapshotMetadata();
    void rejectsMissingAndUnrepresentableControlWorkLedger();
    void publishesCompactOriginsAndReadsExpandedLegacyManifests();
    void rejectsMalformedCompactOriginStorage();
};

void NativeResultBrokerTest::publishesCompactOriginsAndReadsExpandedLegacyManifests()
{
    QTemporaryDir directory;
    const QString logical = writeDynamicFrame(directory.path(), 1);
    writeDynamicFrame(directory.path(), 2);
    QJsonObject declared = provenance();
    QJsonObject runtime = declared.value(QStringLiteral("runtime")).toObject();
    runtime.insert(QStringLiteral("buildIdentity"), QString(4096, QLatin1Char('x')));
    declared.insert(QStringLiteral("runtime"), runtime);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logical, declared);
    QVERIFY2(published.valid, qPrintable(published.message));
    QFile file(published.manifestPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    file.close();
    const QJsonObject stored = QJsonDocument::fromJson(bytes).object();
    QCOMPARE(stored.value(QStringLiteral("originEncoding")).toString(), QStringLiteral("range.native-origin-table/v1"));
    const QJsonObject table = stored.value(QStringLiteral("originTable")).toObject();
    QCOMPARE(table.size(), 1);
    const QJsonValue originalOrigin = published.manifest.value(QStringLiteral("frames")).toArray().first().toObject().value(QStringLiteral("origin"));
    const QByteArray originalBytes = QJsonDocument(originalOrigin.toObject()).toJson(QJsonDocument::Compact);
    const QString originSha = sha(originalBytes);
    QCOMPARE(table.value(originSha).toString().toUtf8(), originalBytes);
    QVERIFY(bytes.size() < QJsonDocument(published.manifest).toJson(QJsonDocument::Compact).size());
    for (const QJsonValue &value : stored.value(QStringLiteral("frames")).toArray())
    {
        QVERIFY(!value.toObject().contains(QStringLiteral("origin")));
        QCOMPARE(value.toObject().value(QStringLiteral("originRef")).toString(), originSha);
    }
    const auto snapshot = [&](const QString &digest)
    {
        return RNativeResultBroker::query(published.manifestPath, digest, QStringLiteral("snapshot"),
            {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}});
    };
    const QJsonObject compactSnapshot = snapshot(published.manifestSha256);
    QVERIFY2(compactSnapshot.value(QStringLiteral("valid")).toBool(), qPrintable(compactSnapshot.value(QStringLiteral("message")).toString()));
    for (const QJsonValue &value : compactSnapshot.value(QStringLiteral("frames")).toArray())
        QCOMPARE(value.toObject().value(QStringLiteral("originSha256")).toString(), originSha);
    // Expanded v1 manifests published before origin tables remain readable.
    const QByteArray legacyBytes = QJsonDocument(published.manifest).toJson(QJsonDocument::Indented);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(legacyBytes), qint64(legacyBytes.size()));
    file.close();
    const QJsonObject legacySnapshot = snapshot(sha(legacyBytes));
    QVERIFY2(legacySnapshot.value(QStringLiteral("valid")).toBool(), qPrintable(legacySnapshot.value(QStringLiteral("message")).toString()));
    QCOMPARE(legacySnapshot.value(QStringLiteral("frames")), compactSnapshot.value(QStringLiteral("frames")));
}

void NativeResultBrokerTest::rejectsMalformedCompactOriginStorage()
{
    QTemporaryDir directory;
    const QString logical = writeDynamicFrame(directory.path(), 1);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logical, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QFile file(published.manifestPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject stored = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QString originSha = stored.value(QStringLiteral("frames")).toArray().first().toObject().value(QStringLiteral("originRef")).toString();
    const auto tableSha = [](const QJsonObject &table)
    {
        QByteArray bytes("range.native-origin-table/v1\n");
        for (auto it = table.constBegin(); it != table.constEnd(); ++it)
        {
            const QByteArray encoded = it.value().toString().toUtf8();
            bytes += it.key().toLatin1() + '\n' + QByteArray::number(encoded.size()) + ':' + encoded + '\n';
        }
        return sha(bytes);
    };
    const auto rejected = [&](const QJsonObject &manifest, const QString &expectedCode = QStringLiteral("INVALID_HISTORY_PROVENANCE"))
    {
        const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        const QJsonObject response = RNativeResultBroker::query(published.manifestPath, sha(bytes), QStringLiteral("catalog"), {});
        QVERIFY(!response.value(QStringLiteral("valid")).toBool());
        QCOMPARE(response.value(QStringLiteral("code")).toString(), expectedCode);
    };
    QJsonObject changed = stored;
    changed.insert(QStringLiteral("originTableSha256"), sha("wrong-table"));
    rejected(changed);
    changed = stored;
    QJsonObject table = stored.value(QStringLiteral("originTable")).toObject();
    table.insert(originSha, QStringLiteral("{\"jobId\":\"forged\"}"));
    changed.insert(QStringLiteral("originTable"), table);
    changed.insert(QStringLiteral("originTableSha256"), tableSha(table));
    rejected(changed);
    for (int mutation = 0; mutation < 5; ++mutation)
    {
        changed = stored;
        QJsonObject frame = stored.value(QStringLiteral("frames")).toArray().first().toObject();
        if (mutation == 0) frame.insert(QStringLiteral("originRef"), sha("missing-origin"));
        if (mutation == 1) frame.remove(QStringLiteral("originRef"));
        if (mutation == 2) frame.insert(QStringLiteral("origin"), QJsonObject{});
        if (mutation == 3) frame.insert(QStringLiteral("originRef"), QJsonValue(QJsonValue::Null));
        if (mutation == 4) frame.insert(QStringLiteral("metadata"), QJsonObject{{QStringLiteral("originRef"), originSha}});
        changed.insert(QStringLiteral("frames"), QJsonArray{frame});
        rejected(changed);
    }
    changed = stored;
    changed.remove(QStringLiteral("originEncoding"));
    rejected(changed);
    changed = stored;
    table = stored.value(QStringLiteral("originTable")).toObject();
    table.insert(sha("null"), QStringLiteral("null"));
    changed.insert(QStringLiteral("originTable"), table);
    changed.insert(QStringLiteral("originTableSha256"), tableSha(table));
    rejected(changed);
    // A valid table cannot grant a forged origin the produced-job identity.
    changed = stored;
    const QByteArray forgedBytes("{\"jobId\":\"forged\"}");
    const QString forgedSha = sha(forgedBytes);
    table = {{forgedSha, QString::fromUtf8(forgedBytes)}};
    changed.insert(QStringLiteral("originTable"), table);
    changed.insert(QStringLiteral("originTableSha256"), tableSha(table));
    QJsonObject frame = stored.value(QStringLiteral("frames")).toArray().first().toObject();
    frame.insert(QStringLiteral("originRef"), forgedSha);
    changed.insert(QStringLiteral("frames"), QJsonArray{frame});
    rejected(changed);
}

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
    QFile storedStatic(published.manifestPath);
    QVERIFY(storedStatic.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(storedStatic.readAll()).object().contains("originEncoding"));
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
    QFile storedModal(published.manifestPath);
    QVERIFY(storedModal.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(storedModal.readAll()).object().contains("originEncoding"));
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
    checkpoint.controlChannelIds = {QStringLiteral("actuator-x"), QStringLiteral("actuator-y")};
    checkpoint.controlLastCommands = {0.0, 0.0};
    checkpoint.controlPowers = {0.0, 0.0};
    checkpoint.cumulativeControlWork = {1.25, -0.5};
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
    const QJsonObject snapshot = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}});
    QVERIFY2(snapshot.value(QStringLiteral("valid")).toBool(), qPrintable(snapshot.value(QStringLiteral("message")).toString()));
    QCOMPARE(snapshot.value(QStringLiteral("sourceKind")).toString(), QStringLiteral("native-active-fe"));
    const QJsonObject frame = snapshot.value(QStringLiteral("frames")).toArray().first().toObject();
    QCOMPARE(frame.value(QStringLiteral("modelSha256")), published.manifest.value(QStringLiteral("frames")).toArray()
        .first().toObject().value(QStringLiteral("sha256")));
    QCOMPARE(frame.value(QStringLiteral("values")).toArray().first().toDouble(),
        model.getVariable(model.findVariable(R_VARIABLE_DISPLACEMENT)).getValue(0, 0));
    const QJsonObject channels = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("channels"), {{QStringLiteral("offset"), 18}, {QStringLiteral("limit"), 1}});
    QCOMPARE(channels.value(QStringLiteral("channelCount")).toInt(), 19);
    const QJsonObject workMetadata{{QStringLiteral("channelId"), QStringLiteral("control:work")},
        {QStringLiteral("variableId"), QStringLiteral("control-cumulative-work")},
        {QStringLiteral("quantity"), QStringLiteral("controlWork")}, {QStringLiteral("axis"), QStringLiteral("scalar")},
        {QStringLiteral("unit"), QStringLiteral("J")}, {QStringLiteral("nodeTarget"), QStringLiteral("accepted-control-ledger")}};
    QCOMPARE(channels.value(QStringLiteral("channels")).toArray(), QJsonArray{workMetadata});
    const QJsonObject workSnapshot = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("control:work"),
            QStringLiteral("node:1:displacement:X")}}});
    QVERIFY2(workSnapshot.value(QStringLiteral("valid")).toBool(), qPrintable(workSnapshot.value(QStringLiteral("message")).toString()));
    QCOMPARE(workSnapshot.value(QStringLiteral("channels")).toArray().first().toObject(), workMetadata);
    QCOMPARE(workSnapshot.value(QStringLiteral("frames")).toArray().first().toObject().value(QStringLiteral("values")).toArray(),
        QJsonArray({0.75, model.getVariable(model.findVariable(R_VARIABLE_DISPLACEMENT)).getValue(0, 0)}));
    for (const QString &operation : {QStringLiteral("series"), QStringLiteral("summary")})
    {
        const QJsonObject rejected = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            operation, {{QStringLiteral("channelId"), QStringLiteral("control:work")}});
        QCOMPARE(rejected.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
        QVERIFY(!rejected.contains(QStringLiteral("nodePhysicalId")));
    }
    // Exercise a hash-bound but malformed physical work value. Binary MODEL
    // doubles are stored natively by RFileIO; this unique fixture value exists
    // only in cumulativeControlWork. The native checkpoint validation rejects
    // it even when all surrounding manifest file hashes are updated coherently.
    QFile rawModel(QDir(directory.path()).filePath(QStringLiteral("active-00001.rbm")));
    QVERIFY(rawModel.open(QIODevice::ReadOnly));
    QByteArray bytes = rawModel.readAll(); rawModel.close();
    const double originalWork = 1.25;
    const QByteArray workBytes(reinterpret_cast<const char *>(&originalWork), int(sizeof(double)));
    QCOMPARE(bytes.count(workBytes), 1);
    const double nonfinite = std::numeric_limits<double>::quiet_NaN();
    bytes.replace(bytes.indexOf(workBytes), int(sizeof(double)),
        QByteArray(reinterpret_cast<const char *>(&nonfinite), int(sizeof(double))));
    QVERIFY(rawModel.open(QIODevice::WriteOnly));
    QCOMPARE(rawModel.write(bytes), qint64(bytes.size())); rawModel.close();
    const QJsonObject staleWork = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("control:work")}}});
    QVERIFY(!staleWork.value(QStringLiteral("valid")).toBool());
    QVERIFY(!staleWork.contains(QStringLiteral("frames")));
    QJsonObject changedManifest = published.manifest;
    QJsonObject changedFrame = changedManifest.value(QStringLiteral("frames")).toArray().first().toObject();
    changedFrame.insert(QStringLiteral("sha256"), sha(bytes));
    changedManifest.insert(QStringLiteral("frames"), QJsonArray{changedFrame});
    QJsonObject terminal = changedManifest.value(QStringLiteral("terminalModel")).toObject();
    terminal.insert(QStringLiteral("sha256"), sha(bytes));
    changedManifest.insert(QStringLiteral("terminalModel"), terminal);
    const QByteArray manifestBytes = QJsonDocument(changedManifest).toJson(QJsonDocument::Compact);
    QFile manifestFile(published.manifestPath);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    QCOMPARE(manifestFile.write(manifestBytes), qint64(manifestBytes.size())); manifestFile.close();
    const QJsonObject rejected = RNativeResultBroker::query(published.manifestPath, sha(manifestBytes),
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("control:work")}}});
    QVERIFY(!rejected.value(QStringLiteral("valid")).toBool());
    QVERIFY(!rejected.contains(QStringLiteral("frames")));
}

void NativeResultBrokerTest::rejectsTamperedUnrequestedFrameAtEveryQueryBoundary()
{
    QTemporaryDir directory;
    const QString logicalPath = writeDynamicFrame(directory.path(), 1);
    writeDynamicFrame(directory.path(), 2);
    writeDynamicFrame(directory.path(), 3);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QFile altered(framePath(directory.path(), 2));
    QVERIFY(altered.open(QIODevice::Append));
    altered.write("changed");
    altered.close();
    const QStringList operations{QStringLiteral("catalog"), QStringLiteral("channels"),
        QStringLiteral("series"), QStringLiteral("summary"), QStringLiteral("snapshot")};
    for (const QString &operation : operations)
    {
        QJsonObject arguments;
        if (operation == QLatin1String("series") || operation == QLatin1String("summary"))
            arguments.insert(QStringLiteral("channelId"), QStringLiteral("node:1:displacement:X"));
        if (operation != QLatin1String("summary")) arguments.insert(QStringLiteral("limit"), 1);
        if (operation == QLatin1String("snapshot"))
            arguments = {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}};
        const QJsonObject result = RNativeResultBroker::query(published.manifestPath, published.manifestSha256, operation, arguments);
        QVERIFY(!result.value(QStringLiteral("valid")).toBool());
        QCOMPARE(result.value(QStringLiteral("code")).toString(), QStringLiteral("RESULT_FRAME_TAMPERED"));
    }
}

void NativeResultBrokerTest::exposesRotationsOnlyAtBeamNodes()
{
    QTemporaryDir directory;
    const QString logicalPath = QDir(directory.path()).filePath(QStringLiteral("run.rbm"));
    RModel model = dynamicModel(1, {11, 22, 33});
    model.setNLines(0);
    model.setNElements(2);
    RElement beam(R_ELEMENT_BEAM1);
    beam.setNodeId(0, 0); beam.setNodeId(1, 1);
    RElement truss(R_ELEMENT_TRUSS1);
    truss.setNodeId(0, 1); truss.setNodeId(1, 2);
    model.setElement(0, beam); model.setElement(1, truss);
    model.setNLines(1);
    model.getLine(0).add(0);
    model.getLine(0).add(1);
    RMaterial material(RMaterial::Solid);
    for (const RMaterialProperty::Type type : {RMaterialProperty::ModulusOfElasiticity,
         RMaterialProperty::PoissonRatio, RMaterialProperty::Density, RMaterialProperty::ThermalExpansionCoefficient})
    {
        RMaterialProperty property(type);
        property.add(293.15, 1.0);
        material.add(property);
    }
    model.getLine(0).setMaterial(material);
    addVector(model, R_VARIABLE_ROTATION, 1.0);
    addVector(model, R_VARIABLE_ANGULAR_VELOCITY, 2.0);
    addVector(model, R_VARIABLE_ANGULAR_ACCELERATION, 3.0);
    RStructuralDynamicsRestartData checkpoint = model.getProblemSetup().getStructuralDynamicsRestartData();
    checkpoint.meshElementCount = model.getNElements();
    checkpoint.meshFingerprint = RStructuralDynamicsRestartData::computeMeshFingerprint(model);
    model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
    model.write(logicalPath, true);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QCOMPARE(published.manifest.value(QStringLiteral("rotationNodePhysicalIds")).toArray(), QJsonArray({11, 22}));
    const QJsonObject catalog = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("channels"), {});
    QCOMPARE(catalog.value(QStringLiteral("channelCount")).toInt(), 45);
    for (const QJsonValue &value : catalog.value(QStringLiteral("channels")).toArray())
        QVERIFY(!value.toObject().value(QStringLiteral("channelId")).toString().startsWith(QStringLiteral("node:33:rotation")) &&
                !value.toObject().value(QStringLiteral("channelId")).toString().startsWith(QStringLiteral("node:33:angular")));
    for (const QString &quantity : {QStringLiteral("rotation"), QStringLiteral("angularVelocity"), QStringLiteral("angularAcceleration")})
    {
        const QJsonObject unavailable = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            QStringLiteral("series"), {{QStringLiteral("channelId"), QStringLiteral("node:33:%1:X").arg(quantity)}});
        QCOMPARE(unavailable.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
        const QJsonObject available = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            QStringLiteral("series"), {{QStringLiteral("channelId"), QStringLiteral("node:22:%1:X").arg(quantity)}});
        QVERIFY(available.value(QStringLiteral("valid")).toBool());
    }
    QTemporaryDir inactiveDirectory;
    model.setNLines(0);
    checkpoint = model.getProblemSetup().getStructuralDynamicsRestartData();
    checkpoint.meshFingerprint = RStructuralDynamicsRestartData::computeMeshFingerprint(model);
    model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
    model.write(QDir(inactiveDirectory.path()).filePath(QStringLiteral("run.rbm")), true);
    const auto inactive = RNativeResultBroker::publishResult(inactiveDirectory.path(),
        QDir(inactiveDirectory.path()).filePath(QStringLiteral("run.rbm")), provenance());
    QVERIFY2(inactive.valid, qPrintable(inactive.message));
    QVERIFY(inactive.manifest.value(QStringLiteral("rotationNodePhysicalIds")).toArray().isEmpty());
    QVERIFY(!inactive.manifest.value(QStringLiteral("availableVariables")).toArray().contains(QStringLiteral("var-rotation")));
}

void NativeResultBrokerTest::exposesTranslationsOnlyAtComputableNodes()
{
    QTemporaryDir directory;
    RModel model = dynamicModel(1, {11, 22, 33, 44});
    model.setNLines(0);
    model.setNElements(2);
    RElement inactiveBeam(R_ELEMENT_BEAM1);
    inactiveBeam.setNodeId(0, 0);
    inactiveBeam.setNodeId(1, 1);
    model.setElement(0, inactiveBeam, false);
    RElement supportedTruss(R_ELEMENT_TRUSS1);
    supportedTruss.setNodeId(0, 1);
    supportedTruss.setNodeId(1, 2);
    model.setElement(1, supportedTruss, false);
    addComputableTrussGroup(model, 1);
    QCOMPARE(model.getLine(0).size(), 1U);
    QCOMPARE(model.getLine(0).get(0), 1U);
    RBoundaryCondition support(R_BOUNDARY_CONDITION_DISPLACEMENT);
    for (const RVariableType type : {R_VARIABLE_DISPLACEMENT_X, R_VARIABLE_DISPLACEMENT_Y,
                                     R_VARIABLE_DISPLACEMENT_Z})
        support.getComponent(support.findComponentPosition(type)).add(0.0, 0.0);
    model.getLine(0).addBoundaryCondition(support);
    RStructuralDynamicsRestartData checkpoint = model.getProblemSetup().getStructuralDynamicsRestartData();
    checkpoint.meshElementCount = model.getNElements();
    checkpoint.meshFingerprint = RStructuralDynamicsRestartData::computeMeshFingerprint(model);
    model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);

    const QString logicalPath = QDir(directory.path()).filePath(QStringLiteral("run.rbm"));
    model.write(logicalPath, true);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    QCOMPARE(published.manifest.value(QStringLiteral("nodePhysicalIds")).toArray(), QJsonArray({11, 22, 33, 44}));
    QCOMPARE(published.manifest.value(QStringLiteral("translationNodePhysicalIds")).toArray(), QJsonArray({22, 33}));

    const QJsonObject catalog = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("channels"), {});
    QVERIFY2(catalog.value(QStringLiteral("valid")).toBool(), qPrintable(catalog.value(QStringLiteral("message")).toString()));
    QCOMPARE(catalog.value(QStringLiteral("channelCount")).toInt(), 18);
    for (const QJsonValue &value : catalog.value(QStringLiteral("channels")).toArray())
    {
        const QString channelId = value.toObject().value(QStringLiteral("channelId")).toString();
        QVERIFY(!channelId.startsWith(QStringLiteral("node:11:")));
        QVERIFY(!channelId.startsWith(QStringLiteral("node:44:")));
    }

    for (const uint inactiveNode : {11U, 44U})
    {
        for (const QString &quantity : {QStringLiteral("displacement"), QStringLiteral("velocity"),
                                        QStringLiteral("acceleration")})
        {
            const QString channelId = QStringLiteral("node:%1:%2:X").arg(inactiveNode).arg(quantity);
            const QJsonObject series = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
                QStringLiteral("series"), {{QStringLiteral("channelId"), channelId}});
            QCOMPARE(series.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
            const QJsonObject summary = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
                QStringLiteral("summary"), {{QStringLiteral("channelId"), channelId}});
            QCOMPARE(summary.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
        }
    }
    for (const uint supportedNode : {22U, 33U})
    {
        for (const QString &quantity : {QStringLiteral("displacement"), QStringLiteral("velocity"),
                                        QStringLiteral("acceleration")})
        {
            const QString channelId = QStringLiteral("node:%1:%2:X").arg(supportedNode).arg(quantity);
            const QJsonObject series = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
                QStringLiteral("series"), {{QStringLiteral("channelId"), channelId}});
            QVERIFY2(series.value(QStringLiteral("valid")).toBool(), qPrintable(series.value(QStringLiteral("message")).toString()));
            const QJsonObject summary = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
                QStringLiteral("summary"), {{QStringLiteral("channelId"), channelId}});
            QVERIFY2(summary.value(QStringLiteral("valid")).toBool(), qPrintable(summary.value(QStringLiteral("message")).toString()));
        }
    }

    QFile frame(framePath(directory.path(), 1));
    QVERIFY(frame.open(QIODevice::ReadOnly));
    const QString frameSha = sha(frame.readAll());
    frame.close();
    const QJsonArray liveFrames{QJsonObject{{QStringLiteral("recordNo"), 1},
        {QStringLiteral("timeS"), 0.1}, {QStringLiteral("seq"), 1},
        {QStringLiteral("relativePath"), QStringLiteral("run-00001.rbm")}, {QStringLiteral("sha256"), frameSha}}};
    const QJsonObject liveCatalog = RNativeResultBroker::queryLive(directory.path(), provenance(), liveFrames,
        QStringLiteral("channels"), {});
    QVERIFY2(liveCatalog.value(QStringLiteral("valid")).toBool(),
             qPrintable(liveCatalog.value(QStringLiteral("message")).toString()));
    QCOMPARE(liveCatalog.value(QStringLiteral("channelCount")).toInt(), 18);
    for (const uint supportedNode : {22U, 33U})
    {
        for (const QString &quantity : {QStringLiteral("displacement"), QStringLiteral("velocity"),
                                        QStringLiteral("acceleration")})
        {
            const QString channelId = QStringLiteral("node:%1:%2:X").arg(supportedNode).arg(quantity);
            const QJsonObject liveAvailable = RNativeResultBroker::queryLive(directory.path(), provenance(), liveFrames,
                QStringLiteral("series"), {{QStringLiteral("channelId"), channelId}});
            QVERIFY2(liveAvailable.value(QStringLiteral("valid")).toBool(),
                     qPrintable(liveAvailable.value(QStringLiteral("message")).toString()));
        }
    }
    for (const uint inactiveNode : {11U, 44U})
    {
        for (const QString &quantity : {QStringLiteral("displacement"), QStringLiteral("velocity"),
                                        QStringLiteral("acceleration")})
        {
            const QString channelId = QStringLiteral("node:%1:%2:X").arg(inactiveNode).arg(quantity);
            const QJsonObject liveUnavailable = RNativeResultBroker::queryLive(directory.path(), provenance(), liveFrames,
                QStringLiteral("series"), {{QStringLiteral("channelId"), channelId}});
            QCOMPARE(liveUnavailable.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
        }
    }
}

void NativeResultBrokerTest::rejectsMissingOrInvalidTranslationNodeMaps()
{
    const auto rewrittenManifestQuery = [](const std::function<void(QJsonObject &)> &mutate)
    {
        QTemporaryDir directory;
        const QString logicalPath = writeDynamicFrame(directory.path(), 1);
        const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
        if (!published.valid) return QJsonObject{{QStringLiteral("valid"), true}};

        QJsonObject manifest = published.manifest;
        mutate(manifest);
        const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QFile file(published.manifestPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
            return QJsonObject{{QStringLiteral("valid"), true}};
        file.close();
        return RNativeResultBroker::query(published.manifestPath, sha(bytes), QStringLiteral("channels"), {});
    };

    const QJsonObject missing = rewrittenManifestQuery([](QJsonObject &manifest)
    {
        manifest.remove(QStringLiteral("translationNodePhysicalIds"));
    });
    QVERIFY(!missing.value(QStringLiteral("valid")).toBool());
    QCOMPARE(missing.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_MANIFEST"));

    for (const QJsonArray invalidMap : {QJsonArray{1, 1}, QJsonArray{9999}})
    {
        const QJsonObject invalid = rewrittenManifestQuery([&invalidMap](QJsonObject &manifest)
        {
            manifest.insert(QStringLiteral("translationNodePhysicalIds"), invalidMap);
        });
        QVERIFY(!invalid.value(QStringLiteral("valid")).toBool());
        QCOMPARE(invalid.value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_MANIFEST"));
    }
}

void NativeResultBrokerTest::rejectsTranslationAvailabilityDriftAcrossFrames()
{
    QTemporaryDir directory;
    const QString logicalPath = writeDynamicFrame(directory.path(), 1);
    RModel secondFrame = dynamicModel(2);
    secondFrame.setNLines(0);
    RStructuralDynamicsRestartData checkpoint = secondFrame.getProblemSetup().getStructuralDynamicsRestartData();
    checkpoint.meshFingerprint = RStructuralDynamicsRestartData::computeMeshFingerprint(secondFrame);
    secondFrame.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
    secondFrame.write(logicalPath, true);

    const auto published = RNativeResultBroker::publishResult(directory.path(), logicalPath, provenance());
    QVERIFY(!published.valid);
    QCOMPARE(published.code, QStringLiteral("INCONSISTENT_FRAME_METADATA"));
}

void NativeResultBrokerTest::preservesVerifiedHistoryOriginAndRejectsAncestorTampering()
{
    QTemporaryDir originalDirectory, resumedDirectory;
    const QString originalLogical = writeDynamicFrame(originalDirectory.path(), 1);
    QJsonObject originalProvenance = provenance(QStringLiteral("old-job"));
    originalProvenance.insert(QStringLiteral("inputSnapshotSha256"), sha("old-input"));
    originalProvenance.insert(QStringLiteral("runtime"), QJsonObject{{QStringLiteral("fingerprintSha256"), sha("old-runtime")},
        {QStringLiteral("scope"), QStringLiteral("old-runtime/v1")}});
    originalProvenance.insert(QStringLiteral("sourceSnapshotRef"), QJsonObject{{QStringLiteral("sourceRevision"), QStringLiteral("1")}});
    const auto original = RNativeResultBroker::publishResult(originalDirectory.path(), originalLogical, originalProvenance);
    QVERIFY2(original.valid, qPrintable(original.message));
    const QString resumedLogical = writeDynamicFrame(resumedDirectory.path(), 1);
    writeDynamicFrame(resumedDirectory.path(), 2);
    const QJsonObject originalFrame = original.manifest.value(QStringLiteral("frames")).toArray().first().toObject();
    QFile copied(framePath(resumedDirectory.path(), 1));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    const QString copiedSha = sha(copied.readAll());
    copied.close();
    const QJsonObject proof{{QStringLiteral("recordNo"), 1}, {QStringLiteral("relativePath"), QStringLiteral("run-00001.rbm")},
        {QStringLiteral("sha256"), copiedSha}, {QStringLiteral("originalFrameSha256"), originalFrame.value(QStringLiteral("sha256"))},
        {QStringLiteral("originKind"), QStringLiteral("reused-history")}, {QStringLiteral("origin"), originalFrame.value(QStringLiteral("origin"))},
        {QStringLiteral("originManifestPath"), original.manifestPath}, {QStringLiteral("originManifestSha256"), original.manifestSha256}};
    QJsonObject resumedProvenance = provenance(QStringLiteral("new-job"));
    resumedProvenance.insert(QStringLiteral("reusedHistory"), QJsonArray{proof});
    QJsonObject forgedProof = proof;
    forgedProof.insert(QStringLiteral("origin"), provenance(QStringLiteral("new-job")));
    QJsonObject forgedProvenance = resumedProvenance;
    forgedProvenance.insert(QStringLiteral("reusedHistory"), QJsonArray{forgedProof});
    const auto forged = RNativeResultBroker::publishResult(resumedDirectory.path(), resumedLogical, forgedProvenance);
    QCOMPARE(forged.code, QStringLiteral("INVALID_HISTORY_PROVENANCE"));
    const auto resumed = RNativeResultBroker::publishResult(resumedDirectory.path(), resumedLogical, resumedProvenance);
    QVERIFY2(resumed.valid, qPrintable(resumed.message));
    const QJsonArray frames = resumed.manifest.value(QStringLiteral("frames")).toArray();
    QCOMPARE(frames.at(0).toObject().value(QStringLiteral("origin")), originalFrame.value(QStringLiteral("origin")));
    QCOMPARE(frames.at(0).toObject().value(QStringLiteral("origin")).toObject().value(QStringLiteral("jobId")).toString(), QStringLiteral("old-job"));
    QCOMPARE(frames.at(1).toObject().value(QStringLiteral("origin")).toObject().value(QStringLiteral("jobId")).toString(), QStringLiteral("new-job"));
    const QJsonObject firstPage = RNativeResultBroker::query(resumed.manifestPath, resumed.manifestSha256,
        QStringLiteral("catalog"), {{QStringLiteral("limit"), 1}});
    QVERIFY(firstPage.value(QStringLiteral("valid")).toBool());
    QCOMPARE(firstPage.value(QStringLiteral("reusedHistoryCount")).toInt(), 1);
    QCOMPARE(firstPage.value(QStringLiteral("reusedHistoryScope")).toString(), QStringLiteral("catalog-frame-page"));
    QCOMPARE(firstPage.value(QStringLiteral("reusedHistory")).toArray(), QJsonArray{proof});
    const QJsonObject secondPage = RNativeResultBroker::query(resumed.manifestPath, resumed.manifestSha256,
        QStringLiteral("catalog"), {{QStringLiteral("limit"), 1}, {QStringLiteral("offset"), 1}});
    QVERIFY(secondPage.value(QStringLiteral("valid")).toBool());
    QCOMPARE(secondPage.value(QStringLiteral("reusedHistoryCount")).toInt(), 1);
    QVERIFY(secondPage.value(QStringLiteral("reusedHistory")).toArray().isEmpty());
    QFile tampered(framePath(originalDirectory.path(), 1));
    QVERIFY(tampered.open(QIODevice::Append)); tampered.write("changed"); tampered.close();
    const QJsonObject rejected = RNativeResultBroker::query(resumed.manifestPath, resumed.manifestSha256, QStringLiteral("catalog"), {});
    QVERIFY(!rejected.value(QStringLiteral("valid")).toBool());
    const QJsonObject snapshotRejected = RNativeResultBroker::query(resumed.manifestPath, resumed.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}});
    QVERIFY(!snapshotRejected.value(QStringLiteral("valid")).toBool());
    QVERIFY(!snapshotRejected.contains(QStringLiteral("frames")));
}

void NativeResultBrokerTest::marksLegacyHistoryOriginUnavailable()
{
    QTemporaryDir directory;
    const QString logical = writeDynamicFrame(directory.path(), 1);
    writeDynamicFrame(directory.path(), 2);
    QFile copied(framePath(directory.path(), 1));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    const QString copiedSha = sha(copied.readAll());
    copied.close();
    QJsonObject declared = provenance();
    declared.insert(QStringLiteral("reusedHistory"), QJsonArray{QJsonObject{
        {QStringLiteral("recordNo"), 1}, {QStringLiteral("relativePath"), QStringLiteral("run-00001.rbm")},
        {QStringLiteral("sha256"), copiedSha}, {QStringLiteral("originalFrameSha256"), copiedSha},
        {QStringLiteral("sourceRestartSha256"), sha("legacy-restart")},
        {QStringLiteral("originKind"), QStringLiteral("legacy-history-unattributed")},
        {QStringLiteral("origin"), QJsonValue(QJsonValue::Null)}}});
    const auto published = RNativeResultBroker::publishResult(directory.path(), logical, declared);
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonObject history = published.manifest.value(QStringLiteral("frames")).toArray().first().toObject();
    QVERIFY(history.value(QStringLiteral("origin")).isNull());
    QCOMPARE(history.value(QStringLiteral("originKind")).toString(), QStringLiteral("legacy-history-unattributed"));
}

void NativeResultBrokerTest::queriesOnlyCompleteLiveFrames()
{
    QTemporaryDir directory;
    writeDynamicFrame(directory.path(), 1);
    QFile file(framePath(directory.path(), 1));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString digest = sha(file.readAll());
    file.close();
    const QJsonArray frames{QJsonObject{{QStringLiteral("recordNo"), 1}, {QStringLiteral("timeS"), 0.1},
        {QStringLiteral("seq"), 1}, {QStringLiteral("relativePath"), QStringLiteral("run-00001.rbm")}, {QStringLiteral("sha256"), digest}}};
    const QJsonObject snapshot = RNativeResultBroker::queryLive(directory.path(), provenance(), frames,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}});
    QCOMPARE(snapshot.value(QStringLiteral("code")).toString(), QStringLiteral("UNSUPPORTED_OPERATION"));
    QVERIFY(!snapshot.value(QStringLiteral("valid")).toBool());
    QFile pending(framePath(directory.path(), 2));
    QVERIFY(pending.open(QIODevice::WriteOnly)); pending.write("partial"); pending.close();
    const QJsonObject catalog = RNativeResultBroker::queryLive(directory.path(), provenance(), frames, QStringLiteral("catalog"), {});
    QVERIFY2(catalog.value(QStringLiteral("valid")).toBool(), qPrintable(catalog.value(QStringLiteral("message")).toString()));
    QCOMPARE(catalog.value(QStringLiteral("frameCount")).toInt(), 1);
    QCOMPARE(catalog.value(QStringLiteral("generation")).toInt(), 1);
    QVERIFY(catalog.value(QStringLiteral("live")).toBool());
    QVERIFY(!catalog.value(QStringLiteral("completed")).toBool());
    QVERIFY(!QFile::exists(QDir(directory.path()).filePath(QStringLiteral("native-result.manifest.json"))));
    QVERIFY(file.open(QIODevice::Append)); file.write("changed"); file.close();
    const QJsonObject changed = RNativeResultBroker::queryLive(directory.path(), provenance(), frames, QStringLiteral("channels"), {});
    QCOMPARE(changed.value(QStringLiteral("code")).toString(), QStringLiteral("RESULT_FRAME_TAMPERED"));
}

void NativeResultBrokerTest::preservesMixedHistoryProofsAcrossSeriesPages()
{
    QTemporaryDir originalDirectory, resumedDirectory;
    const QString originalLogical = writeDynamicFrame(originalDirectory.path(), 1);
    const auto original = RNativeResultBroker::publishResult(originalDirectory.path(), originalLogical,
        provenance(QStringLiteral("original-job")));
    QVERIFY2(original.valid, qPrintable(original.message));
    const QString resumedLogical = writeDynamicFrame(resumedDirectory.path(), 1);
    writeDynamicFrame(resumedDirectory.path(), 2);
    writeDynamicFrame(resumedDirectory.path(), 3);
    const auto frameHash = [&resumedDirectory](int recordNo)
    {
        QFile file(framePath(resumedDirectory.path(), recordNo));
        if (!file.open(QIODevice::ReadOnly)) return QString();
        return sha(file.readAll());
    };
    const QJsonObject originalFrame = original.manifest.value(QStringLiteral("frames")).toArray().first().toObject();
    const QJsonObject known{{QStringLiteral("recordNo"), 1},
        {QStringLiteral("relativePath"), QStringLiteral("run-00001.rbm")},
        {QStringLiteral("sha256"), frameHash(1)},
        {QStringLiteral("originalFrameSha256"), originalFrame.value(QStringLiteral("sha256"))},
        {QStringLiteral("originKind"), QStringLiteral("reused-history")},
        {QStringLiteral("origin"), originalFrame.value(QStringLiteral("origin"))},
        {QStringLiteral("originManifestPath"), original.manifestPath},
        {QStringLiteral("originManifestSha256"), original.manifestSha256}};
    const QJsonObject legacy{{QStringLiteral("recordNo"), 2},
        {QStringLiteral("relativePath"), QStringLiteral("run-00002.rbm")},
        {QStringLiteral("sha256"), frameHash(2)},
        {QStringLiteral("originalFrameSha256"), frameHash(2)},
        {QStringLiteral("sourceRestartSha256"), sha("legacy-restart")},
        {QStringLiteral("originKind"), QStringLiteral("legacy-history-unattributed")},
        {QStringLiteral("origin"), QJsonValue(QJsonValue::Null)}};
    QJsonObject declared = provenance(QStringLiteral("resumed-job"));
    declared.insert(QStringLiteral("reusedHistory"), QJsonArray{known, legacy});
    const auto published = RNativeResultBroker::publishResult(resumedDirectory.path(), resumedLogical, declared);
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonObject snapshot = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}});
    QVERIFY2(snapshot.value(QStringLiteral("valid")).toBool(), qPrintable(snapshot.value(QStringLiteral("message")).toString()));
    const QJsonArray compactFrames = snapshot.value(QStringLiteral("frames")).toArray();
    QCOMPARE(compactFrames.size(), 3);
    QCOMPARE(compactFrames.at(0).toObject().value(QStringLiteral("historyProofSha256")).toString(),
        sha(QJsonDocument(known).toJson(QJsonDocument::Compact)));
    QCOMPARE(compactFrames.at(1).toObject().value(QStringLiteral("historyProofSha256")).toString(),
        sha(QJsonDocument(legacy).toJson(QJsonDocument::Compact)));
    QCOMPARE(compactFrames.at(1).toObject().value(QStringLiteral("originSha256")).toString(), sha("null"));
    QCOMPARE(compactFrames.at(2).toObject().value(QStringLiteral("historyProofSha256")).toString(), sha("null"));
    const QByteArray compactSnapshot = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    QVERIFY(!compactSnapshot.contains("originManifestPath"));
    QVERIFY(!compactSnapshot.contains("relativePath"));
    QVERIFY(!compactSnapshot.contains(original.manifestPath.toUtf8()));
    QJsonObject arguments{{QStringLiteral("channelId"), QStringLiteral("node:1:displacement:X")},
        {QStringLiteral("limit"), 1}};
    for (int pageIndex = 0; pageIndex < 3; ++pageIndex)
    {
        const QJsonObject page = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            QStringLiteral("series"), arguments);
        QVERIFY2(page.value(QStringLiteral("valid")).toBool(), qPrintable(page.value(QStringLiteral("message")).toString()));
        const QJsonArray samples = page.value(QStringLiteral("samples")).toArray();
        QCOMPARE(samples.size(), 1);
        const QJsonObject sample = samples.first().toObject();
        QCOMPARE(sample.value(QStringLiteral("recordNo")).toInt(), pageIndex + 1);
        QVERIFY(sample.contains(QStringLiteral("historyProof")));
        if (pageIndex < 2)
        {
            const QJsonObject proof = pageIndex == 0 ? known : legacy;
            QCOMPARE(sample.value(QStringLiteral("historyProof")).toObject(), proof);
            QCOMPARE(sample.value(QStringLiteral("originKind")), proof.value(QStringLiteral("originKind")));
            QCOMPARE(sample.value(QStringLiteral("origin")), proof.value(QStringLiteral("origin")));
            QVERIFY(page.contains(QStringLiteral("nextCursor")));
            arguments.insert(QStringLiteral("cursor"), page.value(QStringLiteral("nextCursor")));
        }
        else
        {
            QVERIFY(sample.value(QStringLiteral("historyProof")).isNull());
            QCOMPARE(sample.value(QStringLiteral("originKind")).toString(), QStringLiteral("produced"));
            QCOMPARE(sample.value(QStringLiteral("origin")).toObject().value(QStringLiteral("jobId")).toString(), QStringLiteral("resumed-job"));
            QVERIFY(!page.contains(QStringLiteral("nextCursor")));
        }
    }
}

void NativeResultBrokerTest::snapshotsAllFramesAndRequestedChannelsInOrder()
{
    QTemporaryDir directory;
    const QString logical = writeDynamicFrame(directory.path(), 1);
    writeDynamicFrame(directory.path(), 3); // Sparse output must remain sparse.
    const auto published = RNativeResultBroker::publishResult(directory.path(), logical, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonArray requested{QStringLiteral("node:2:velocity:Z"),
        QStringLiteral("node:1:displacement:X"), QStringLiteral("node:2:acceleration:Y")};
    const QJsonObject snapshot = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), requested}});
    QVERIFY2(snapshot.value(QStringLiteral("valid")).toBool(), qPrintable(snapshot.value(QStringLiteral("message")).toString()));
    QCOMPARE(snapshot.value(QStringLiteral("apiVersion")).toString(), QStringLiteral("range.native-series-snapshot/v1"));
    QCOMPARE(snapshot.value(QStringLiteral("schemaId")).toString(), QStringLiteral("range.native-series-snapshot/v1"));
    QCOMPARE(snapshot.value(QStringLiteral("manifestSha256")).toString(), published.manifestSha256);
    QCOMPARE(snapshot.value(QStringLiteral("sourceKind")).toString(), QStringLiteral("native-passive-fe"));
    QCOMPARE(snapshot.value(QStringLiteral("sourceSnapshotRef")), published.manifest.value(QStringLiteral("sourceSnapshotRef")));
    QCOMPARE(snapshot.value(QStringLiteral("frameCount")).toInt(), 2);
    QVERIFY(snapshot.value(QStringLiteral("completed")).toBool());
    QVERIFY(!snapshot.value(QStringLiteral("live")).toBool());
    const QJsonArray frames = snapshot.value(QStringLiteral("frames")).toArray();
    const QJsonArray metadata = snapshot.value(QStringLiteral("channels")).toArray();
    QCOMPARE(metadata.size(), requested.size());
    const QJsonObject catalog = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("channels"), {});
    for (int index = 0; index < requested.size(); ++index)
    {
        const QJsonObject channel = metadata.at(index).toObject();
        QCOMPARE(channel.value(QStringLiteral("channelId")), requested.at(index));
        QVERIFY(catalog.value(QStringLiteral("channels")).toArray().contains(channel));
        const QJsonObject oldSeries = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            QStringLiteral("series"), {{QStringLiteral("channelId"), requested.at(index)}});
        QVERIFY(oldSeries.value(QStringLiteral("valid")).toBool());
        const QJsonArray oldSamples = oldSeries.value(QStringLiteral("samples")).toArray();
        QCOMPARE(oldSamples.size(), frames.size());
        for (int frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
        {
            const QJsonObject frame = frames.at(frameIndex).toObject();
            const QJsonObject oldSample = oldSamples.at(frameIndex).toObject();
            const QJsonObject original = published.manifest.value(QStringLiteral("frames")).toArray().at(frameIndex).toObject();
            QCOMPARE(frame.value(QStringLiteral("recordNo")), oldSample.value(QStringLiteral("recordNo")));
            QCOMPARE(frame.value(QStringLiteral("timeS")), oldSample.value(QStringLiteral("timeS")));
            QCOMPARE(frame.value(QStringLiteral("values")).toArray().at(index), oldSample.value(QStringLiteral("value")));
            QCOMPARE(frame.value(QStringLiteral("modelSha256")), original.value(QStringLiteral("sha256")));
            QCOMPARE(frame.value(QStringLiteral("originSha256")).toString(),
                sha(QJsonDocument(original.value(QStringLiteral("origin")).toObject()).toJson(QJsonDocument::Compact)));
            QCOMPARE(frame.value(QStringLiteral("historyProofSha256")).toString(), sha("null"));
            QCOMPARE(frame.keys(), QStringList({QStringLiteral("historyProofSha256"), QStringLiteral("modelSha256"),
                QStringLiteral("originKind"), QStringLiteral("originSha256"), QStringLiteral("recordNo"),
                QStringLiteral("timeS"), QStringLiteral("values")}));
        }
    }
    QCOMPARE(frames.last().toObject().value(QStringLiteral("recordNo")).toInt(), 3);
    QVERIFY(!QJsonDocument(snapshot).toJson(QJsonDocument::Compact).contains("Path"));
}

void NativeResultBrokerTest::rejectsInvalidSnapshotArgumentsWithoutPartialResults()
{
    QTemporaryDir directory;
    const QString logical = writeDynamicFrame(directory.path(), 1);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logical, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonObject noWork = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("control:work")}}});
    QCOMPARE(noWork.value(QStringLiteral("code")).toString(), QStringLiteral("CHANNEL_UNAVAILABLE"));
    QVERIFY(!noWork.contains(QStringLiteral("frames")));
    const QString id = QStringLiteral("node:1:displacement:X");
    QJsonArray tooMany;
    for (int index = 0; index < 17; ++index) tooMany.append(id);
    const QList<QJsonObject> invalid{
        {}, {{QStringLiteral("channelIds"), QJsonArray{}}}, {{QStringLiteral("channelIds"), id}},
        {{QStringLiteral("channelIds"), QJsonArray{id, id}}},
        {{QStringLiteral("channelIds"), QJsonArray{id, 2}}},
        {{QStringLiteral("channelIds"), tooMany}},
        {{QStringLiteral("channelIds"), QJsonArray{id}}, {QStringLiteral("path"), QStringLiteral("private.rbm")}},
        {{QStringLiteral("channelIds"), QJsonArray{id}}, {QStringLiteral("limit"), 1}},
        {{QStringLiteral("channelIds"), QJsonArray{id, QStringLiteral("node:999:displacement:X")}}}
    };
    for (const QJsonObject &arguments : invalid)
    {
        const QJsonObject result = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
            QStringLiteral("snapshot"), arguments);
        QVERIFY(!result.value(QStringLiteral("valid")).toBool());
        QVERIFY(!result.contains(QStringLiteral("frames")));
        QVERIFY(!result.contains(QStringLiteral("channels")));
    }
    const QJsonObject stale = RNativeResultBroker::query(published.manifestPath, QString(64, QLatin1Char('0')),
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{id}}});
    QCOMPARE(stale.value(QStringLiteral("code")).toString(), QStringLiteral("STALE_MANIFEST_SHA256"));
    const QJsonObject allChannels = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("channels"), {});
    for (const QJsonValue &metadata : allChannels.value(QStringLiteral("channels")).toArray())
        QVERIFY(metadata.toObject().value(QStringLiteral("channelId")).toString() != QLatin1String("control:work"));
    QJsonArray maximum;
    for (int index = 0; index < 16; ++index)
        maximum.append(allChannels.value(QStringLiteral("channels")).toArray().at(index).toObject().value(QStringLiteral("channelId")));
    const QJsonObject accepted = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
        QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), maximum}});
    QVERIFY(accepted.value(QStringLiteral("valid")).toBool());
    QCOMPARE(accepted.value(QStringLiteral("frames")).toArray().first().toObject().value(QStringLiteral("values")).toArray().size(), 16);
}

void NativeResultBrokerTest::rejectsMalformedAndOversizedSnapshotMetadata()
{
    QTemporaryDir directory;
    const QString logical = writeDynamicFrame(directory.path(), 1);
    const auto published = RNativeResultBroker::publishResult(directory.path(), logical, provenance());
    QVERIFY2(published.valid, qPrintable(published.message));
    const QJsonObject arguments{{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X")}}};
    const auto rewrite = [&published](const QJsonObject &manifest)
    {
        const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        QFile file(published.manifestPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return QString();
        file.close();
        return sha(bytes);
    };
    const auto queryChanged = [&published, &arguments, &rewrite](const QJsonObject &manifest)
    {
        return RNativeResultBroker::query(published.manifestPath, rewrite(manifest), QStringLiteral("snapshot"), arguments);
    };
    QJsonObject huge = published.manifest;
    QJsonArray hugeFrames;
    for (int index = 0; index < 10001; ++index)
        hugeFrames.append(published.manifest.value(QStringLiteral("frames")).toArray().first());
    huge.insert(QStringLiteral("frames"), hugeFrames);
    QCOMPARE(queryChanged(huge).value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_MANIFEST"));

    QJsonObject malformed = published.manifest;
    QJsonObject frame = malformed.value(QStringLiteral("frames")).toArray().first().toObject();
    frame.insert(QStringLiteral("timeS"), QJsonValue(QJsonValue::Null));
    malformed.insert(QStringLiteral("frames"), QJsonArray{frame});
    QCOMPARE(queryChanged(malformed).value(QStringLiteral("code")).toString(), QStringLiteral("INVALID_MANIFEST"));

    // A freshly published, otherwise valid source must fail without rewriting
    // private path metadata or silently truncating its frozen identity.
    QVERIFY(!rewrite(published.manifest).isEmpty());
    QList<QJsonObject> rejectedReferences{
        {{QStringLiteral("privatePath"), directory.path()}},
        {{QStringLiteral("opaque"), QString(4097, QLatin1Char('x'))}}};
    const QStringList privatePaths{QStringLiteral("C:\\Users\\YOGA\\private"),
        QStringLiteral("\\\\server\\private\\model.rbm"), QStringLiteral("file:///private/model.rbm"),
        QStringLiteral("../private/model.rbm"), QStringLiteral("./private/model.rbm"),
        QStringLiteral("..\\private\\model.rbm"), QStringLiteral("private/model.rbm"),
        QStringLiteral("model.rbm"), QStringLiteral("/private/model.rbm")};
    for (const QString &privatePath : privatePaths)
    {
        rejectedReferences.append(QJsonObject{{privatePath, QJsonObject{{QStringLiteral("opaque"), QStringLiteral("x")}}}});
        rejectedReferences.append(QJsonObject{{QStringLiteral("opaque"), QJsonArray{QJsonObject{{QStringLiteral("nested"), privatePath}}}}});
    }
    for (const QJsonObject &reference : rejectedReferences)
    {
        QJsonObject declared = provenance();
        declared.insert(QStringLiteral("sourceSnapshotRef"), reference);
        QFile::remove(published.manifestPath);
        const auto privateSource = RNativeResultBroker::publishResult(directory.path(), logical, declared);
        QVERIFY2(privateSource.valid, qPrintable(privateSource.message));
        const QJsonObject rejected = RNativeResultBroker::query(privateSource.manifestPath, privateSource.manifestSha256,
            QStringLiteral("snapshot"), arguments);
        QVERIFY(!rejected.value(QStringLiteral("valid")).toBool());
        QVERIFY(!rejected.contains(QStringLiteral("frames")));
        QVERIFY(!rejected.contains(QStringLiteral("sourceSnapshotRef")));
    }
    QJsonObject declared = provenance();
    const QJsonObject versionedReference{{QStringLiteral("kind"), QStringLiteral("range.model-origin/v1")},
        {QStringLiteral("projectId"), QStringLiteral("project-1")},
        {QStringLiteral("schemaId"), QStringLiteral("range.model-snapshot/v1")}};
    declared.insert(QStringLiteral("sourceSnapshotRef"), versionedReference);
    QFile::remove(published.manifestPath);
    const auto versionedSource = RNativeResultBroker::publishResult(directory.path(), logical, declared);
    QVERIFY2(versionedSource.valid, qPrintable(versionedSource.message));
    const QJsonObject accepted = RNativeResultBroker::query(versionedSource.manifestPath, versionedSource.manifestSha256,
        QStringLiteral("snapshot"), arguments);
    QVERIFY2(accepted.value(QStringLiteral("valid")).toBool(), qPrintable(accepted.value(QStringLiteral("message")).toString()));
    QCOMPARE(accepted.value(QStringLiteral("sourceSnapshotRef")).toObject(), versionedReference);
}

void NativeResultBrokerTest::rejectsMissingAndUnrepresentableControlWorkLedger()
{
    for (bool missing : {true, false})
    {
        QTemporaryDir directory;
        RModel model = dynamicModel(1);
        RStructuralControlSetup &control = model.getProblemSetup().getStructuralControlSetup();
        control.enabled = true;
        control.controllerId = QStringLiteral("test-controller");
        RStructuralDynamicsRestartData checkpoint = model.getProblemSetup().getStructuralDynamicsRestartData();
        if (!missing)
        {
            checkpoint.controlAvailable = true;
            checkpoint.controlFingerprint = sha("control-map");
            checkpoint.controlChannelIds = {QStringLiteral("a"), QStringLiteral("b")};
            checkpoint.controlLastCommands = {0.0, 0.0};
            checkpoint.controlPowers = {0.0, 0.0};
            checkpoint.cumulativeControlWork = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
        }
        model.getProblemSetup().setStructuralDynamicsRestartData(checkpoint);
        const QString logical = QDir(directory.path()).filePath(QStringLiteral("work.rbm"));
        model.write(logical, true);
        QJsonObject declared = provenance();
        declared.insert(QStringLiteral("control"), QJsonObject{{QStringLiteral("enabled"), true},
            {QStringLiteral("controllerId"), QStringLiteral("test-controller")},
            {QStringLiteral("controllerVersionSha256"), sha("controller")}});
        const auto published = RNativeResultBroker::publishResult(directory.path(), logical, declared);
        if (missing)
        {
            QVERIFY(!published.valid);
            QCOMPARE(published.code, QStringLiteral("INVALID_ACCEPTED_CHECKPOINT"));
        }
        else
        {
            QVERIFY2(published.valid, qPrintable(published.message));
            const QJsonObject result = RNativeResultBroker::query(published.manifestPath, published.manifestSha256,
                QStringLiteral("snapshot"), {{QStringLiteral("channelIds"), QJsonArray{QStringLiteral("node:1:displacement:X"),
                    QStringLiteral("control:work")}}});
            QCOMPARE(result.value(QStringLiteral("code")).toString(), QStringLiteral("NONFINITE_RESULT_FIELD"));
            QVERIFY(!result.contains(QStringLiteral("frames")));
        }
    }
}

} // namespace

QTEST_GUILESS_MAIN(NativeResultBrokerTest)
#include "tst_native_result_broker.moc"
