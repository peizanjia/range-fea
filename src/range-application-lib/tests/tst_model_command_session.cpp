#include <QtTest>
#include <rmodel_command_session.h>
#include <rbl_interaction_record_v1.h>
#include <rbl_error.h>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <limits>
#include <algorithm>
#include <array>
#include <map>
#include <exception>

namespace {
RModel unboundStaticTetra()
{
    RModel model; model.setNNodes(4); model.setNSurfaces(1); model.setNVolumes(1);
    const double xyz[4][3] = {{0,0,0}, {1,0,0}, {0,1,0}, {0,0,1}};
    for (uint n = 0; n < 4; ++n)
    {
        model.getNode(n).set(xyz[n][0], xyz[n][1], xyz[n][2]);
        model.setNodePhysicalId(n, 101 + n * 10);
    }
    const uint faces[4][3] = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    for (const auto &face : faces)
    {
        RElement element(R_ELEMENT_TRI1);
        for (uint n = 0; n < 3; ++n) element.setNodeId(n, face[n]);
        model.addElement(element, true, 0);
    }
    RElement tetra(R_ELEMENT_TETRA1);
    for (uint n = 0; n < 4; ++n) tetra.setNodeId(n, n);
    model.addElement(tetra, true, 0);
    model.setProblemTaskTree(RProblemTaskItem()); model.setProblemSetup(RProblemSetup()); model.setTimeSolver(RTimeSolver());
    for (uint g = 0; g < model.getNElementGroups(); ++g)
        while (model.getElementGroupPtr(g)->getNEnvironmentConditions()) model.getElementGroupPtr(g)->removeEnvironmentCondition(0u);
    return model;
}
QJsonObject staticBindingParameters()
{
    const QJsonValue none(QJsonValue::Null);
    return {{"unitSI", true}, {"staticStress", true},
        {"material", QJsonObject{{"youngModulusPa", 1000}, {"poissonRatio", 0.25}, {"densityKgM3", 1}}},
        {"constraints", QJsonArray{
            QJsonObject{{"nodePhysicalId", 101}, {"values", QJsonArray{0,0,0}}},
            QJsonObject{{"nodePhysicalId", 111}, {"values", QJsonArray{none,0,0}}},
            QJsonObject{{"nodePhysicalId", 121}, {"values", QJsonArray{none,none,0}}}}},
        {"loads", QJsonArray{QJsonObject{{"nodePhysicalId", 131}, {"forceN", QJsonArray{0,0,-1}}}}}};
}
RModel unboundStaticGrid()
{
    constexpr uint side = 4, width = side + 1;
    const auto node = [](uint x, uint y, uint z) { return x + width * (y + width * z); };
    RModel model; model.setNNodes(width * width * width); model.setNSurfaces(1); model.setNVolumes(1);
    for (uint z = 0; z < width; ++z) for (uint y = 0; y < width; ++y) for (uint x = 0; x < width; ++x)
    {
        const uint n = node(x,y,z); model.getNode(n).set(double(x)/side, double(y)/side, double(z)/side);
        // setNNodes has already assigned IDs 1..125 to all nodes. Remap
        // outside that live range instead of colliding with another node.
        model.setNodePhysicalId(n, 1001 + n * 10);
    }
    using Face = std::array<uint,3>;
    std::map<Face, std::pair<uint,Face>> faces;
    const uint localFaces[4][3] = {{1,2,3}, {0,3,2}, {0,1,3}, {0,2,1}};
    for (uint z = 0; z < side; ++z) for (uint y = 0; y < side; ++y) for (uint x = 0; x < side; ++x)
    {
        const uint a=node(x,y,z), b=node(x+1,y,z), c=node(x+1,y+1,z), d=node(x,y+1,z),
            e=node(x,y+1,z+1), f=node(x,y,z+1), g=node(x+1,y,z+1), h=node(x+1,y+1,z+1);
        const uint tets[6][4] = {{a,b,c,h}, {a,c,d,h}, {a,d,e,h}, {a,e,f,h}, {a,f,g,h}, {a,g,b,h}};
        for (const auto &tet : tets)
        {
            RElement element(R_ELEMENT_TETRA1);
            for (uint j = 0; j < 4; ++j) element.setNodeId(j, tet[j]);
            model.addElement(element, true, 0);
            for (const auto &local : localFaces)
            {
                const Face outward{tet[local[0]], tet[local[1]], tet[local[2]]};
                Face key=outward; std::sort(key.begin(),key.end());
                auto &entry=faces[key]; ++entry.first; entry.second=outward;
            }
        }
    }
    for (const auto &entry : faces)
    {
        if (entry.second.first != 1) continue;
        RElement boundary(R_ELEMENT_TRI1);
        for (uint j = 0; j < 3; ++j) boundary.setNodeId(j, entry.second.second[j]);
        model.addElement(boundary, true, 0);
    }
    model.setProblemTaskTree(RProblemTaskItem()); model.setProblemSetup(RProblemSetup()); model.setTimeSolver(RTimeSolver());
    for (uint group = 0; group < model.getNElementGroups(); ++group)
        while (model.getElementGroupPtr(group)->getNEnvironmentConditions()) model.getElementGroupPtr(group)->removeEnvironmentCondition(0u);
    return model;
}
bool decodeStaticBindingAudit(const RInteractionRecordV1 &record, QJsonObject &parameters)
{
    if (record.selectionSnapshot["parametersEncoding"] != "json-text-chunks/v1"
        || record.parameters.size() != 2 || record.parameters["encoding"] != "json-text-chunks/v1"
        || !record.parameters["chunks"].isArray()) return false;
    const auto chunks = record.parameters["chunks"].toArray();
    if (chunks.isEmpty() || chunks.size() > 32) return false;
    QByteArray encoded;
    for (const auto &value : chunks)
    {
        if (!value.isString()) return false;
        const QString chunk = value.toString();
        const auto bytes = chunk.toUtf8();
        if (chunk.isEmpty() || chunk.size() > 4096 || bytes.size() > 4096
            || chunk.front().isLowSurrogate() || chunk.back().isHighSurrogate()) return false;
        encoded += bytes;
        if (encoded.size() > 65536) return false;
    }
    QJsonParseError error;
    const auto decoded = QJsonDocument::fromJson(encoded, &error);
    if (error.error != QJsonParseError::NoError || !decoded.isObject()
        || decoded.toJson(QJsonDocument::Compact) != encoded) return false;
    parameters = decoded.object(); return true;
}
RModel modalModel()
{
    RModel model;
    model.setProblemTaskTree(RProblemTaskItem(R_PROBLEM_STRESS_MODAL));
    return model;
}
QJsonObject request(const RModelCommandSession &session, QString command = "analysis.modal.configure")
{
    const auto current = session.query();
    auto parameters = current["modal"].toObject();
    parameters["modes"] = 3;
    parameters["method"] = "multiple";
    return {{"apiVersion", "range.application/v1"}, {"commandId", command},
        {"commandVersion", 1}, {"modelId", current["modelId"]},
        {"expectedRevision", current["revision"]},
        {"requestId", QUuid::createUuid().toString()},
        {"idempotencyKey", QUuid::createUuid().toString()},
        {"parameters", command == "analysis.modal.configure" ? parameters : QJsonObject{}},
        {"dryRun", false}};
}
bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}
QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool rewriteFirstRecordAsLegacyModalHash(const QString &path, const QJsonObject &modal)
{
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(readBytes(path), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    QJsonObject envelope = document.object();
    QJsonArray records = envelope.value(QStringLiteral("records")).toArray();
    if (records.isEmpty() || !records.first().isObject()) return false;
    QJsonObject first = records.first().toObject();
    QJsonObject selectionSnapshot = first.value(QStringLiteral("selectionSnapshot")).toObject();
    selectionSnapshot.insert(QStringLiteral("stateHashScope"), QStringLiteral("modal_configuration"));
    selectionSnapshot.remove(QStringLiteral("stateHashScopeVersion"));
    first.insert(QStringLiteral("selectionSnapshot"), selectionSnapshot);
    const QString modalHash = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(modal).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    first.insert(QStringLiteral("preStateHash"), modalHash);
    first.insert(QStringLiteral("postStateHash"), modalHash);
    records[0] = first;
    envelope.insert(QStringLiteral("records"), records);
    return writeBytes(path, QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
}
// The original scope-v2 supported-state hash for this empty modal fixture.
// Keep the historical hash algorithm separate from the current geometry scope.
QString emptyModalStateHashV2(const RModel &model)
{
    const auto &time = model.getTimeSolver();
    QJsonArray times;
    for (double value : time.getTimes()) times.append(value);
    const QJsonObject timeState{{"enabled", time.getEnabled()},
        {"inputStartTimeS", time.getInputStartTime()}, {"inputTimeStepS", time.getInputTimeStepSize()},
        {"steps", double(time.getInputNTimeSteps())}, {"outputFrequency", double(time.getOutputFrequency())},
        {"timeMarchApproximation", int(time.getTimeMarchApproximation())},
        {"currentStep", double(time.getCurrentTimeStep())}, {"computedTimeS", time.getComputedTime()},
        {"timesS", times}};
    const QJsonObject state{{"taskMask", model.getProblemTaskTree().getProblemTypeMask()},
        {"modal", RModelCommandService::modalParameters(model.getProblemSetup().getModalSetup())},
        {"structuralDynamics", RModelCommandService::structuralDynamicsParameters(model.getProblemSetup().getStructuralDynamicsSetup())},
        {"timeSolver", timeState}, {"groups", QJsonArray{}}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(state).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

bool rewriteSecondRecordAsLegacyV2(const QString &path, const QString &hash)
{
    auto envelope = QJsonDocument::fromJson(readBytes(path)).object();
    auto records = envelope["records"].toArray();
    if (records.size() != 2) return false;
    auto record = records[1].toObject();
    auto selection = record["selectionSnapshot"].toObject();
    selection["stateHashScopeVersion"] = 2;
    record["selectionSnapshot"] = selection;
    record["preStateHash"] = hash;
    record["postStateHash"] = hash;
    records[1] = record;
    envelope["records"] = records;
    return writeBytes(path, QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
}

bool journalRecords(const QString &path, QVector<RInteractionRecordV1> &records, QString &error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(readBytes(path), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        error = QStringLiteral("Cannot parse journal envelope.");
        return false;
    }
    const auto envelope = document.object();
    if (envelope["journalVersion"].toInt() != 1 || !envelope["records"].isArray()) return false;
    QUuid parent;
    records.clear();
    for (const auto &value : envelope["records"].toArray())
    {
        RInteractionRecordV1 record;
        if (!RInteractionRecordV1::fromJson(QJsonDocument(value.toObject()).toJson(), record, &error)
            || record.projectId != QUuid(envelope["projectId"].toString())
            || record.parentRecordId != parent) return false;
        parent = record.recordId;
        records.append(record);
    }
    return true;
}
}

class TestModelCommandSession : public QObject
{
    Q_OBJECT
private slots:
    void staticRebindJournalPreviewAcceptUndoRedoAndRollback();
    void staticRebindLargeNullableAuditChunksRoundTrip();
    void geometryJournalAtomicityAndNoOpHistory();
    void geometryNoOpPreservesRedoHistory();
    void ownedModelPreviewAndReplay();
    void rejectWithoutMutation_data();
    void rejectWithoutMutation();
    void undoRedoAndStaleReplay();
    void unsupportedModelAndForeignIdentity();
    void boundedHistory();
    void branchAndRetryPreserveModel();
    void lifecycleRejectsEditsAndOldSessions();
    void durableRecordsPreserveRevisionsAndSanitizeRejections();
    void journalFailurePreservesModelHistoryAndLifecycle();
    void malformedJournalIsNotOverwritten_data();
    void malformedJournalIsNotOverwritten();
    void publicationUsesActualBytesAndSupportsAuditRetry();
    void externalChangesAuditActualStateAndInvalidate_data();
    void externalChangesAuditActualStateAndInvalidate();
    void externalAuditFailureStillInvalidatesStaleIdentity();
    void publicationIntentFailureAndMismatchPreserveEvidence();
    void nullableBoundaryAuditPreservesDisabledAxes();
};

void TestModelCommandSession::staticRebindJournalPreviewAcceptUndoRedoAndRollback()
{
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const RModel source = unboundStaticTetra();
    RModelCommandSession::JournalOptions options; options.path = directory.filePath("static-rebind.json");
    RModelCommandSession session(source, options);
    const auto initial = session.query();
    auto command = request(session, "mesh.rebind.static"); command["parameters"] = staticBindingParameters();
    auto dry = command; dry["dryRun"] = true;
    const auto preview = session.execute(dry);
    QVERIFY2(preview["status"].toString() == QString("preview"),
             qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
    QCOMPARE(preview["report"].toObject()["physicsState"].toString(), QString("BOUND_STATIC"));
    QCOMPARE(session.query(), initial); QCOMPARE(session.model().getNPoints(), 0u);
    auto bad = request(session, "mesh.rebind.static");
    auto badParameters = staticBindingParameters(); badParameters.insert("callback", "untrusted"); bad["parameters"] = badParameters;
    QCOMPARE(session.execute(bad)["status"].toString(), QString("rejected"));
    QCOMPARE(session.query(), initial);
    QLockFile lock(options.path + ".lock"); lock.setStaleLockTime(0); QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(command)["code"].toString(), QString("journal_failure"));
    QCOMPARE(session.query(), initial); QCOMPARE(session.model().getNElements(), 5u);
    lock.unlock();
    const auto accepted = session.execute(command);
    QCOMPARE(accepted["status"].toString(), QString("accepted"));
    QCOMPARE(session.execute(command), accepted);
    QCOMPARE(accepted["units"].toObject()["force"].toString(), QString("N"));
    QCOMPARE(session.model().getNPoints(), 4u); QCOMPARE(session.model().getNElements(), 9u);
    const QString boundHash = RModelCommandService::stateFingerprint(session.model());
    const uint pointId = session.model().getPoint(3).getPhysicalId();
    const uint elementId = session.model().getElementPhysicalId(session.model().getPoint(3).get(0));
    const auto beforeUndo = session.query();
    const auto undoRequest = request(session, "model.undo");
    QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(undoRequest)["code"].toString(), QString("journal_failure"));
    QCOMPARE(session.query(), beforeUndo); QCOMPARE(session.model().getNPoints(), 4u);
    lock.unlock();
    const auto undone = session.execute(undoRequest);
    QCOMPARE(undone["status"].toString(), QString("accepted"));
    QCOMPARE(undone["report"].toObject()["physicsState"].toString(), QString("UNBOUND"));
    QCOMPARE(session.model().getNPoints(), 0u); QCOMPARE(session.model().getNElements(), 5u);
    QCOMPARE(RModelCommandService::stateFingerprint(session.model()), RModelCommandService::stateFingerprint(source));
    QCOMPARE(session.execute(command)["code"].toString(), QString("stale_revision"));
    const auto redoRequest = request(session, "model.redo");
    const auto beforeRedo = session.query();
    QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(redoRequest)["code"].toString(), QString("journal_failure"));
    QCOMPARE(session.query(), beforeRedo); QCOMPARE(session.model().getNPoints(), 0u);
    lock.unlock();
    QCOMPARE(session.execute(redoRequest)["status"].toString(), QString("accepted"));
    QCOMPARE(RModelCommandService::stateFingerprint(session.model()), boundHash);
    QCOMPARE(session.model().getPoint(3).getPhysicalId(), pointId);
    QCOMPARE(session.model().getElementPhysicalId(session.model().getPoint(3).get(0)), elementId);
    const auto &force = session.model().getPoint(3).getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
    QCOMPARE(force.getComponent(force.findComponentPosition(R_VARIABLE_FORCE_Z)).getValue(0), -1.0);
    QVERIFY(!force.getHasLocalDirection());
    const QString path = session.model().write(directory.filePath("bound.rbm"), false);
    RModel reopened; reopened.read(path);
    QCOMPARE(RModelCommandService::stateFingerprint(reopened), boundHash);
    QCOMPARE(reopened.getPoint(3).getPhysicalId(), pointId);
    QVector<RInteractionRecordV1> records; QString error;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    for (const auto &record : records) QCOMPARE(record.selectionSnapshot["stateHashScopeVersion"].toInt(), 3);
    bool sawBinding = false, sawUndo = false, sawRedo = false;
    for (const auto &record : records)
    {
        if (record.resultStatus != "accepted") continue;
        if (record.commandId == "mesh.rebind.static")
        {
            sawBinding = true;
            QJsonObject decoded;
            QVERIFY(decodeStaticBindingAudit(record, decoded));
            QCOMPARE(decoded, staticBindingParameters());
        }
        sawUndo = sawUndo || record.commandId == "model.undo";
        sawRedo = sawRedo || record.commandId == "model.redo";
    }
    QVERIFY(sawBinding); QVERIFY(sawUndo); QVERIFY(sawRedo);
}

void TestModelCommandSession::staticRebindLargeNullableAuditChunksRoundTrip()
{
    try
    {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto source = unboundStaticGrid();
    auto parameters = staticBindingParameters();
    auto constraints = parameters["constraints"].toArray();
    const uint fixedNodes[3] = {0,1,5};
    for (int c = 0; c < 3; ++c)
    {
        auto constraint = constraints[c].toObject();
        constraint["nodePhysicalId"] = double(source.getNodePhysicalId(fixedNodes[c]));
        constraints[c] = constraint;
    }
    parameters["constraints"] = constraints;
    QJsonArray loads;
    for (uint n = 0; n < source.getNNodes(); ++n)
        loads.append(QJsonObject{{"nodePhysicalId", double(source.getNodePhysicalId(n))}, {"forceN", QJsonArray{0,0,-1}}});
    parameters["loads"] = loads;
    const QByteArray normalizedBytes = QJsonDocument(parameters).toJson(QJsonDocument::Compact);
    QVERIFY(normalizedBytes.size() > 4096); QVERIFY(normalizedBytes.size() < 65536);
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath("large-binding.json");
    // The strict binding schema has ASCII keys and numeric/null values. Unicode
    // is tested only in legal trusted metadata, not invented binding fields.
    options.sourcePath = directory.filePath(QStringLiteral("\u4f86\u6e90-\u6e2c\u8a66-\U0001f9ea.rbm"));
    options.sourceSha256 = QString(64, 'a');
    RModelCommandSession session(source, options);
    auto command = request(session, "mesh.rebind.static"); command["parameters"] = parameters;
    QVERIFY(QJsonDocument(command).toJson(QJsonDocument::Compact).size() < 65536);
    const auto accepted = session.execute(command);
    QVERIFY2(accepted["status"] == "accepted", qPrintable(QJsonDocument(accepted).toJson(QJsonDocument::Compact)));
    QCOMPARE(session.model().getNPoints(), source.getNNodes());
    QVector<RInteractionRecordV1> records; QString error;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    QCOMPARE(records.size(), 1);
    const auto &record = records.first();
    QVERIFY(record.parameters["chunks"].toArray().size() > 1);
    QVERIFY(record.parameters["chunks"].toArray().size() <= 32);
    QCOMPARE(record.selectionSnapshot["sourcePath"].toString(), options.sourcePath);
    QCOMPARE(record.selectionSnapshot["stateHashScopeVersion"].toInt(), 3);
    QJsonObject decoded; QVERIFY(decodeStaticBindingAudit(record, decoded));
    QCOMPARE(decoded, accepted["normalizedParameters"].toObject()); QCOMPARE(decoded, parameters);
    QVERIFY(decoded["constraints"].toArray()[1].toObject()["values"].toArray()[0].isNull());
    auto corrupt = record; auto chunks = corrupt.parameters["chunks"].toArray();
    chunks[0] = QJsonValue(QJsonValue::Null); corrupt.parameters["chunks"] = chunks;
    QVERIFY(!decodeStaticBindingAudit(corrupt, decoded));
    corrupt = record; chunks = corrupt.parameters["chunks"].toArray();
    chunks[0] = QString(4097, 'x'); corrupt.parameters["chunks"] = chunks;
    QVERIFY(!decodeStaticBindingAudit(corrupt, decoded));
    corrupt = record; chunks = corrupt.parameters["chunks"].toArray();
    chunks[0] = chunks[0].toString().mid(1); corrupt.parameters["chunks"] = chunks;
    QVERIFY(!decodeStaticBindingAudit(corrupt, decoded));
    const auto before = session.query(); const QByteArray journalBefore = readBytes(options.path);
    auto oversized = request(session, "mesh.rebind.static");
    auto oversizedParameters = parameters; oversizedParameters["unsupported"] = QString(65536, 'x');
    oversized["parameters"] = oversizedParameters;
    const auto rejected = session.execute(oversized);
    QCOMPARE(rejected["code"].toString(), QString("invalid_request"));
    QCOMPARE(session.query(), before);
    // Rejection may append an audit observation; no accepted binding is added.
    QVERIFY(readBytes(options.path) != journalBefore);
    QVERIFY(journalRecords(options.path, records, error));
    QCOMPARE(records.last().resultStatus, QString("rejected"));
    }
    catch (const RError &error)
    {
        QFAIL(qPrintable(QStringLiteral("Native static binding audit fixture failed: ") + error.getMessage()));
    }
    catch (const std::exception &error)
    {
        QFAIL(qPrintable(QStringLiteral("Static binding audit fixture failed: ") + QString::fromUtf8(error.what())));
    }
}

void TestModelCommandSession::geometryJournalAtomicityAndNoOpHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = modalModel();
    model.setNNodes(1);
    model.getNode(0).set(0.0, 0.0, 0.0);
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath("geometry.json");
    RModelCommandSession session(model, options);
    auto command = request(session, "geometry.node_coordinates.set");
    command["parameters"] = QJsonObject{{"unit", "m"}, {"directionPolicy", "preserve_global"},
        {"nodes", QJsonArray{QJsonObject{{"physicalNodeId", double(model.getNodePhysicalId(0))},
            {"coordinates", QJsonArray{1.25, 2.5, 3.75}}}}}};
    const auto initial = session.query();
    QLockFile lock(options.path + ".lock");
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(command)["code"].toString(), QStringLiteral("journal_failure"));
    QCOMPARE(session.query(), initial);
    QCOMPARE(session.model().getNode(0).getX(), 0.0);
    lock.unlock();
    QCOMPARE(session.execute(command)["status"].toString(), QStringLiteral("accepted"));
    const auto accepted = session.query();
    auto undo = request(session, "model.undo");
    QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(undo)["code"].toString(), QStringLiteral("journal_failure"));
    QCOMPARE(session.query(), accepted);
    lock.unlock();
    QCOMPARE(session.execute(undo)["status"].toString(), QStringLiteral("accepted"));
    QCOMPARE(session.model().getNode(0).getX(), 0.0);
    QCOMPARE(session.execute(request(session, "model.redo"))["status"].toString(), QStringLiteral("accepted"));
    QCOMPARE(session.model().getNode(0).getX(), 1.25);
    auto noOp = request(session, "geometry.node_coordinates.set");
    noOp["parameters"] = command["parameters"];
    const auto noOpResult = session.execute(noOp);
    QCOMPARE(noOpResult["status"].toString(), QStringLiteral("accepted"));
    QVERIFY(noOpResult["changedPaths"].toArray().isEmpty());
    // No-op adds no geometry history entry: one undo returns to the source.
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QStringLiteral("accepted"));
    QCOMPARE(session.model().getNode(0).getX(), 0.0);
    QCOMPARE(session.execute(request(session, "model.undo"))["code"].toString(), QStringLiteral("empty_history"));
    QVector<RInteractionRecordV1> records;
    QString error;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    for (const auto &record : records)
        QCOMPARE(record.selectionSnapshot["stateHashScopeVersion"].toInt(), 3);
    const auto &first = records.first();
    QCOMPARE(first.commandId, QStringLiteral("geometry.node_coordinates.set"));
    QVERIFY(first.preStateHash != first.postStateHash);
    QCOMPARE(first.parameters, command["parameters"].toObject());
}

void TestModelCommandSession::geometryNoOpPreservesRedoHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = modalModel();
    model.setNNodes(2);
    model.getNode(0).set(0.0, 0.0, 0.0);
    model.getNode(1).set(2.0, 0.0, 0.0);
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath("geometry-noop-redo.json");
    RModelCommandSession session(model, options);

    const QString sourceHash = session.query()["supportedStateFingerprint"].toString();
    auto edit = request(session, "geometry.node_coordinates.set");
    edit["parameters"] = QJsonObject{{"unit", "m"}, {"directionPolicy", "preserve_global"},
        {"nodes", QJsonArray{
            QJsonObject{{"physicalNodeId", double(model.getNodePhysicalId(0))},
                        {"coordinates", QJsonArray{1.25, 2.5, 3.75}}},
            QJsonObject{{"physicalNodeId", double(model.getNodePhysicalId(1))},
                        {"coordinates", QJsonArray{2.0, 1.0, 0.5}}}}}};
    QCOMPARE(session.execute(edit)["status"].toString(), QStringLiteral("accepted"));
    const QString editedHash = session.query()["supportedStateFingerprint"].toString();
    QVERIFY(editedHash != sourceHash);
    QVERIFY(session.query()["canUndo"].toBool());
    QVERIFY(!session.query()["canRedo"].toBool());

    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QStringLiteral("accepted"));
    const auto undone = session.query();
    QCOMPARE(undone["supportedStateFingerprint"].toString(), sourceHash);
    QVERIFY(undone["canRedo"].toBool());

    auto noOp = request(session, "geometry.node_coordinates.set");
    noOp["parameters"] = QJsonObject{{"unit", "m"}, {"directionPolicy", "preserve_global"},
        {"nodes", QJsonArray{
            QJsonObject{{"physicalNodeId", double(model.getNodePhysicalId(0))},
                        {"coordinates", QJsonArray{0.0, 0.0, 0.0}}},
            QJsonObject{{"physicalNodeId", double(model.getNodePhysicalId(1))},
                        {"coordinates", QJsonArray{2.0, 0.0, 0.0}}}}}};
    const auto noOpResult = session.execute(noOp);
    QCOMPARE(noOpResult["status"].toString(), QStringLiteral("accepted"));
    QVERIFY(noOpResult["changedPaths"].toArray().isEmpty());
    const auto afterNoOp = session.query();
    QCOMPARE(afterNoOp["supportedStateFingerprint"].toString(), sourceHash);
    QVERIFY(afterNoOp["canRedo"].toBool());

    QCOMPARE(session.execute(request(session, "model.redo"))["status"].toString(), QStringLiteral("accepted"));
    QCOMPARE(session.model().getNode(0).getX(), 1.25);
    QCOMPARE(session.model().getNode(0).getY(), 2.5);
    QCOMPARE(session.model().getNode(0).getZ(), 3.75);
    QCOMPARE(session.model().getNode(1).getX(), 2.0);
    QCOMPARE(session.model().getNode(1).getY(), 1.0);
    QCOMPARE(session.model().getNode(1).getZ(), 0.5);
    const auto redone = session.query();
    QCOMPARE(redone["supportedStateFingerprint"].toString(), editedHash);
    QVERIFY(redone["canUndo"].toBool());
    QVERIFY(!redone["canRedo"].toBool());

    QVector<RInteractionRecordV1> records;
    QString error;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    QVERIFY(records.size() >= 4);
    QCOMPARE(records[records.size() - 2].commandId, QStringLiteral("geometry.node_coordinates.set"));
    QCOMPARE(records[records.size() - 2].preStateHash, sourceHash);
    QCOMPARE(records[records.size() - 2].postStateHash, sourceHash);
    QCOMPARE(records.last().commandId, QStringLiteral("model.redo"));
    QCOMPARE(records.last().postStateHash, editedHash);
}

void TestModelCommandSession::ownedModelPreviewAndReplay()
{
    auto input = modalModel();
    const auto originalModes = input.getProblemSetup().getModalSetup().getNModesToExtract();
    RModelCommandSession session(input);
    auto command = request(session);
    const auto original = session.query();
    command["dryRun"] = true;
    QCOMPARE(session.execute(command)["status"].toString(), QString("preview"));
    QCOMPARE(session.query(), original);
    command["dryRun"] = false;
    const auto accepted = session.execute(command);
    QCOMPARE(accepted["status"].toString(), QString("accepted"));
    QCOMPARE(session.model().getProblemSetup().getModalSetup().getNModesToExtract(), 3u);
    QCOMPARE(input.getProblemSetup().getModalSetup().getNModesToExtract(), originalModes);
    const auto after = session.query();
    QCOMPARE(session.execute(command), accepted);
    QCOMPARE(session.query(), after);
    command["requestId"] = "different-request";
    QCOMPARE(session.execute(command)["code"].toString(), QString("idempotency_conflict"));
    QCOMPARE(session.query(), after);
}

void TestModelCommandSession::rejectWithoutMutation_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<QJsonValue>("value");
    QTest::newRow("zero modes") << QString("modes") << QJsonValue(0);
    QTest::newRow("fraction") << QString("iterations") << QJsonValue(1.5);
    QTest::newRow("huge") << QString("iterations") << QJsonValue(1000001);
    QTest::newRow("bool") << QString("modes") << QJsonValue(true);
    QTest::newRow("string") << QString("modes") << QJsonValue("3");
    QTest::newRow("zero tolerance") << QString("tolerance") << QJsonValue(0);
    QTest::newRow("large tolerance") << QString("tolerance") << QJsonValue(2);
    QTest::newRow("nan") << QString("tolerance") << QJsonValue(std::numeric_limits<double>::quiet_NaN());
    QTest::newRow("bad method") << QString("method") << QJsonValue("unknown");
    QTest::newRow("units") << QString("unit") << QJsonValue("Hz");
    QTest::newRow("unknown") << QString("extra") << QJsonValue(1);
}

void TestModelCommandSession::rejectWithoutMutation()
{
    QFETCH(QString, field);
    QFETCH(QJsonValue, value);
    RModelCommandSession session(modalModel());
    const auto before = session.query();
    auto command = request(session);
    auto parameters = command["parameters"].toObject();
    parameters[field] = value;
    command["parameters"] = parameters;
    QCOMPARE(session.execute(command)["code"].toString(), QString("invalid_parameters"));
    QCOMPARE(session.query(), before);
    command = request(session);
    command["extra"] = 1;
    QCOMPARE(session.execute(command)["code"].toString(), QString("invalid_request"));
    QCOMPARE(session.query(), before);
}

void TestModelCommandSession::undoRedoAndStaleReplay()
{
    RModelCommandSession session(modalModel());
    const auto initial = session.query();
    auto command = request(session);
    QCOMPARE(session.execute(command)["status"].toString(), QString("accepted"));
    const auto firstRevision = session.query().value("revision").toString();
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QString("accepted"));
    QCOMPARE(session.query()["modal"], initial["modal"]);
    QVERIFY(session.query()["revision"] != initial["revision"]);
    QCOMPARE(session.execute(command)["code"].toString(), QString("stale_revision"));
    QCOMPARE(session.execute(request(session, "model.redo"))["status"].toString(), QString("accepted"));
    QCOMPARE(session.model().getProblemSetup().getModalSetup().getNModesToExtract(), 3u);
    QVERIFY(session.query().value("revision").toString() != firstRevision);
    const auto before = session.query();
    auto stale = request(session);
    stale["expectedRevision"] = initial["revision"];
    QCOMPARE(session.execute(stale)["code"].toString(), QString("stale_revision"));
    QCOMPARE(session.query(), before);
    QCOMPARE(session.execute(request(session, "model.redo"))["code"].toString(), QString("empty_history"));
}

void TestModelCommandSession::unsupportedModelAndForeignIdentity()
{
    RModelCommandSession noModal{RModel{}};
    QVERIFY(!noModal.query()["canConfigureModal"].toBool());
    QCOMPARE(noModal.execute(request(noModal))["code"].toString(), QString("unsupported_analysis"));
    RModelCommandSession first(modalModel()), second(modalModel());
    QCOMPARE(second.execute(request(first))["code"].toString(), QString("wrong_model"));
    auto command = request(first);
    command["commandVersion"] = 2;
    QCOMPARE(first.execute(command)["code"].toString(), QString("invalid_request"));
}

void TestModelCommandSession::boundedHistory()
{
    RModelCommandSession session(modalModel());
    auto first = request(session);
    QCOMPARE(session.execute(first)["status"].toString(), QString("accepted"));
    for (int i = 1; i < RModelCommandSession::MaxCommands; ++i)
        QCOMPARE(session.execute(request(session))["status"].toString(), QString("accepted"));
    const auto before = session.query();
    QCOMPARE(session.execute(request(session))["code"].toString(), QString("session_limit"));
    QCOMPARE(session.query(), before);
    QCOMPARE(session.execute(first)["code"].toString(), QString("stale_revision"));
}

void TestModelCommandSession::branchAndRetryPreserveModel()
{
    auto source = modalModel();
    source.addNode(RNode(1.25, -2.5, 3.75));
    RModelCommandSession session(source);
    auto command = request(session);
    auto parameters = command["parameters"].toObject();
    parameters["modes"] = -1;
    command["parameters"] = parameters;
    QCOMPARE(session.execute(command)["status"].toString(), QString("rejected"));
    parameters["modes"] = 3;
    command["parameters"] = parameters;
    QCOMPARE(session.execute(command)["status"].toString(), QString("accepted"));
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QString("accepted"));
    QVERIFY(session.query()["canRedo"].toBool());
    auto branch = request(session);
    parameters["modes"] = 7;
    branch["parameters"] = parameters;
    QCOMPARE(session.execute(branch)["status"].toString(), QString("accepted"));
    QVERIFY(!session.query()["canRedo"].toBool());
    QCOMPARE(session.execute(request(session, "model.redo"))["code"].toString(), QString("empty_history"));
    QCOMPARE(session.model().getNNodes(), source.getNNodes());
    QCOMPARE(session.model().getNode(0).getX(), 1.25);
    QCOMPARE(session.model().getNode(0).getY(), -2.5);
    QCOMPARE(session.model().getNode(0).getZ(), 3.75);
}

void TestModelCommandSession::lifecycleRejectsEditsAndOldSessions()
{
    RModelCommandSession session(modalModel());
    auto first = request(session);
    QCOMPARE(session.execute(first)["status"].toString(), QStringLiteral("accepted"));
    const auto before = session.query();
    QVERIFY(session.setRunning(true));
    QCOMPARE(session.query()["lifecycle"].toString(), QStringLiteral("running"));
    QVERIFY(session.query()["busy"].toBool());
    for (const QString &command : {QStringLiteral("analysis.modal.configure"), QStringLiteral("model.undo"), QStringLiteral("model.redo")})
        QCOMPARE(session.execute(request(session, command))["code"].toString(), QStringLiteral("session_busy"));
    QCOMPARE(session.execute(first)["code"].toString(), QStringLiteral("session_busy"));
    auto preview = request(session);
    preview["dryRun"] = true;
    QCOMPARE(session.execute(preview)["status"].toString(), QStringLiteral("preview"));
    QCOMPARE(session.query()["revision"], before["revision"]);
    QVERIFY(session.setRunning(false));
    QCOMPARE(session.query(), before);
    const auto stale = request(session);
    QVERIFY(session.close());
    QVERIFY(session.close());
    QVERIFY(!session.setRunning(false));
    QCOMPARE(session.query()["lifecycle"].toString(), QStringLiteral("closed"));
    QCOMPARE(session.execute(stale)["code"].toString(), QStringLiteral("session_closed"));
    RModelCommandSession reopened(session.model());
    QVERIFY(reopened.query()["modelId"] != before["modelId"]);
    QCOMPARE(reopened.execute(stale)["code"].toString(), QStringLiteral("wrong_model"));
}

void TestModelCommandSession::durableRecordsPreserveRevisionsAndSanitizeRejections()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("interactions.json"));
    options.actorId = QStringLiteral("test-host");
    options.sourcePath = directory.filePath(QStringLiteral("source.ram"));
    options.sourceSha256 = QString(64, QLatin1Char('a'));
    RModelCommandSession session(modalModel(), options);
    const auto initial = session.query();
    auto command = request(session);
    command["dryRun"] = true;
    QCOMPARE(session.execute(command)["status"].toString(), QStringLiteral("preview"));
    QCOMPARE(session.query(), initial);
    QVERIFY(rewriteFirstRecordAsLegacyModalHash(options.path, initial.value(QStringLiteral("modal")).toObject()));
    auto invalid = request(session);
    invalid["parameters"] = QJsonObject{{"apiKey", "do-not-copy-secret"}, {"invalid", QJsonValue(QJsonValue::Null)}};
    invalid["commandId"] = QStringLiteral("do-not-copy-secret");
    QCOMPARE(session.execute(invalid)["code"].toString(), QStringLiteral("unknown_command"));
    QVERIFY(!readBytes(options.path).contains("do-not-copy-secret"));
    const QString v2Hash = emptyModalStateHashV2(modalModel());
    QVERIFY(v2Hash != RModelCommandService::stateFingerprint(modalModel()));
    QVERIFY(rewriteSecondRecordAsLegacyV2(options.path, v2Hash));
    command["dryRun"] = false;
    const auto accepted = session.execute(command);
    QCOMPARE(accepted["status"].toString(), QStringLiteral("accepted"));
    const auto journalBeforeReplay = readBytes(options.path);
    QCOMPARE(session.execute(command), accepted);
    QCOMPARE(readBytes(options.path), journalBeforeReplay);
    QVERIFY(session.setRunning(true));
    QVERIFY(session.setRunning(false));
    QVERIFY(session.close());
    RModelCommandSession reopened(session.model(), options);
    auto reopenedPreview = request(reopened);
    reopenedPreview["dryRun"] = true;
    QCOMPARE(reopened.execute(reopenedPreview)["status"].toString(), QStringLiteral("preview"));

    QVector<RInteractionRecordV1> records;
    QString error;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    QCOMPARE(records.size(), 7);
    QCOMPARE(records[0].resultStatus, QStringLiteral("preview"));
    QCOMPARE(records[1].resultStatus, QStringLiteral("rejected"));
    QVERIFY(records[1].parameters.isEmpty());
    QCOMPARE(records[1].commandId, QStringLiteral("application.request.reject"));
    QCOMPARE(records[1].selectionSnapshot["commandSha256"].toString(), QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(invalid).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
    QCOMPARE(records[2].resultStatus, QStringLiteral("accepted"));
    QCOMPARE(records[2].modelRevision, QStringLiteral("session:") + accepted["revision"].toString());
    QCOMPARE(records[2].selectionSnapshot["preRevision"], initial["revision"]);
    QCOMPARE(records[2].selectionSnapshot["postRevision"], accepted["revision"]);
    QCOMPARE(records[2].actorId, options.actorId);
    QCOMPARE(records[2].selectionSnapshot["sourceSha256"].toString(), options.sourceSha256);
    QCOMPARE(records[0].selectionSnapshot["stateHashScope"].toString(), QStringLiteral("modal_configuration"));
    QVERIFY(!records[0].selectionSnapshot.contains(QStringLiteral("stateHashScopeVersion")));
    QCOMPARE(records[0].preStateHash, records[0].postStateHash);
    QCOMPARE(records[1].selectionSnapshot["stateHashScope"].toString(), QStringLiteral("supported_model_state"));
    QCOMPARE(records[1].selectionSnapshot["stateHashScopeVersion"].toInt(), 2);
    QCOMPARE(records[1].preStateHash, v2Hash);
    QCOMPARE(records[1].postStateHash, v2Hash);
    QCOMPARE(records[2].selectionSnapshot["stateHashScope"].toString(), QStringLiteral("supported_model_state"));
    QCOMPARE(records[2].selectionSnapshot["stateHashScopeVersion"].toInt(), 3);
    QCOMPARE(records[2].parameters, command["parameters"].toObject());
    QCOMPARE(records[3].resultStatus, QStringLiteral("running"));
    QVERIFY(!records[3].runId.isNull());
    QCOMPARE(records[4].runId, records[3].runId);
    QCOMPARE(records[4].resultStatus, QStringLiteral("ready"));
    QCOMPARE(records[5].resultStatus, QStringLiteral("closed"));
    QVERIFY(records[6].sessionId != records[5].sessionId);
}

void TestModelCommandSession::journalFailurePreservesModelHistoryAndLifecycle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("journal.json"));
    RModelCommandSession session(modalModel(), options);
    const auto before = session.query();
    const auto command = request(session);
    QLockFile lock(options.path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(command)["code"].toString(), QStringLiteral("journal_failure"));
    QCOMPARE(session.query(), before);
    QVERIFY(!session.setRunning(true));
    QVERIFY(!session.close());
    QCOMPARE(session.query(), before);
    lock.unlock();
    QCOMPARE(session.execute(command)["status"].toString(), QStringLiteral("accepted"));
    const auto accepted = session.query();
    QVERIFY(lock.tryLock(0));
    QCOMPARE(session.execute(request(session, "model.undo"))["code"].toString(), QStringLiteral("journal_failure"));
    QCOMPARE(session.query(), accepted);
    lock.unlock();
    QCOMPARE(session.execute(request(session, "model.undo"))["status"].toString(), QStringLiteral("accepted"));
    QCOMPARE(session.query()["modal"], before["modal"]);
    QVERIFY(session.query()["revision"] != before["revision"]);
}

void TestModelCommandSession::malformedJournalIsNotOverwritten_data()
{
    QTest::addColumn<QString>("caseName");
    QTest::newRow("truncated JSON") << QStringLiteral("truncated");
    QTest::newRow("invalid record") << QStringLiteral("invalid-record");
    QTest::newRow("broken parent chain") << QStringLiteral("parent");
    QTest::newRow("duplicate record") << QStringLiteral("duplicate");
}

void TestModelCommandSession::malformedJournalIsNotOverwritten()
{
    QFETCH(QString, caseName);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("journal.json"));
    RModelCommandSession session(modalModel(), options);
    QCOMPARE(session.execute(request(session))["status"].toString(), QStringLiteral("accepted"));
    QByteArray corrupt;
    if (caseName == QStringLiteral("truncated")) corrupt = QByteArrayLiteral("{\"journalVersion\":1,");
    else
    {
        auto envelope = QJsonDocument::fromJson(readBytes(options.path)).object();
        auto records = envelope["records"].toArray();
        auto record = records[0].toObject();
        if (caseName == QStringLiteral("invalid-record")) record["resultStatus"] = QJsonValue(QJsonValue::Null);
        else if (caseName == QStringLiteral("parent")) record["parentRecordId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (caseName == QStringLiteral("duplicate")) records.append(record);
        else records[0] = record;
        envelope["records"] = records;
        corrupt = QJsonDocument(envelope).toJson();
    }
    QVERIFY(writeBytes(options.path, corrupt));
    const auto before = session.query();
    QCOMPARE(session.execute(request(session))["code"].toString(), QStringLiteral("journal_failure"));
    QCOMPARE(session.query(), before);
    QCOMPARE(readBytes(options.path), corrupt);
}

void TestModelCommandSession::publicationUsesActualBytesAndSupportsAuditRetry()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("journal.json"));
    RModelCommandSession session(modalModel(), options);
    QCOMPARE(session.execute(request(session))["status"].toString(), QStringLiteral("accepted"));
    const auto output = session.model().write(directory.filePath(QStringLiteral("published.ram")), false);
    const auto actualBytes = readBytes(output);
    QVERIFY(!actualBytes.isEmpty());
    const auto expectedHash = QString::fromLatin1(QCryptographicHash::hash(actualBytes, QCryptographicHash::Sha256).toHex());
    QLockFile lock(options.path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    QString actualHash, error;
    QVERIFY(!session.recordArtifactPublished(output, &actualHash, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(actualHash, expectedHash);
    QCOMPARE(readBytes(output), actualBytes);
    lock.unlock();
    QVERIFY2(session.recordArtifactPublished(output, &actualHash, &error), qPrintable(error));
    QCOMPARE(actualHash, expectedHash);
    QVector<RInteractionRecordV1> records;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.last().resultStatus, QStringLiteral("published"));
    QCOMPARE(records.last().selectionSnapshot["outputPath"].toString(), QFileInfo(output).canonicalFilePath());
    QCOMPARE(records.last().selectionSnapshot["outputSha256"].toString(), expectedHash);
    QCOMPARE(records.last().artifactRefs[0].toObject()["sha256"].toString(), expectedHash);
    const auto journalBefore = readBytes(options.path);
    QVERIFY(!session.recordArtifactPublished(directory.filePath(QStringLiteral("missing.ram")), &actualHash, &error));
    QCOMPARE(readBytes(options.path), journalBefore);
}

void TestModelCommandSession::externalChangesAuditActualStateAndInvalidate_data()
{
    QTest::addColumn<QString>("reason");
    for (const QString &reason : {QStringLiteral("undo"), QStringLiteral("redo"),
                                 QStringLiteral("reload"), QStringLiteral("external_edit")})
        QTest::newRow(qPrintable(reason)) << reason;
}

void TestModelCommandSession::externalChangesAuditActualStateAndInvalidate()
{
    QFETCH(QString, reason);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("journal.json"));
    auto original = modalModel();
    RModelCommandSession session(original, options);
    const auto oldRequest = request(session);
    auto changed = original;
    changed.getProblemSetup().getModalSetup().setNModesToExtract(17);
    QString error;
    QVERIFY2(session.recordExternalChange(reason, changed, &error), qPrintable(error));
    QCOMPARE(session.query()["lifecycle"].toString(), QStringLiteral("closed"));
    QCOMPARE(session.execute(oldRequest)["code"].toString(), QStringLiteral("session_closed"));
    QVector<RInteractionRecordV1> records;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    QCOMPARE(records[0].commandId, QStringLiteral("model.external.") + reason);
    QCOMPARE(records[0].resultStatus, QStringLiteral("external_changed"));
    QCOMPARE(records[0].selectionSnapshot["reason"].toString(), reason);
    QVERIFY(records[0].selectionSnapshot["identityInvalidated"].toBool());
    QVERIFY(records[0].preStateHash != records[0].postStateHash);
    QCOMPARE(records[0].selectionSnapshot["preRevision"], oldRequest["expectedRevision"]);
    QCOMPARE(records[0].preStateHash, RModelCommandService::stateFingerprint(original));
    QCOMPARE(records[0].postStateHash, RModelCommandService::stateFingerprint(changed));
    RModelCommandSession reopened(changed, options);
    QCOMPARE(reopened.execute(oldRequest)["code"].toString(), QStringLiteral("wrong_model"));
}

void TestModelCommandSession::externalAuditFailureStillInvalidatesStaleIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("journal.json"));
    RModelCommandSession session(modalModel(), options);
    const auto stale = request(session);
    QLockFile lock(options.path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    auto changed = modalModel();
    changed.getProblemSetup().getModalSetup().setNModesToExtract(19);
    QString error;
    QVERIFY(!session.recordExternalChange(QStringLiteral("undo"), changed, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(session.query()["lifecycle"].toString(), QStringLiteral("closed"));
    QVERIFY(!session.setRunning(false));
    lock.unlock();
    QCOMPARE(session.execute(stale)["code"].toString(), QStringLiteral("session_closed"));
    RModelCommandSession invalidReason(modalModel(), options);
    QVERIFY(!invalidReason.recordExternalChange(QStringLiteral("untrusted-reason"), changed, &error));
    QCOMPARE(invalidReason.query()["lifecycle"].toString(), QStringLiteral("closed"));
}

void TestModelCommandSession::publicationIntentFailureAndMismatchPreserveEvidence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("journal.json"));
    RModelCommandSession session(modalModel(), options);
    const QString staged = session.model().write(directory.filePath(QStringLiteral("staged.ram")), false);
    const QString output = directory.filePath(QStringLiteral("output.ram"));
    const auto original = session.query();
    QString error;
    QLockFile lock(options.path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    QVERIFY(lock.tryLock(0));
    QVERIFY(!session.recordPublicationIntent(output, staged, &error));
    QCOMPARE(session.query(), original);
    QVERIFY(!QFileInfo::exists(output));
    lock.unlock();
    QVERIFY2(session.recordPublicationIntent(output, staged, &error), qPrintable(error));
    QCOMPARE(session.execute(request(session))["code"].toString(), QStringLiteral("publication_pending"));
    QVERIFY(writeBytes(output, QByteArrayLiteral("does-not-match-staging")));
    QString actual;
    const auto journalBefore = readBytes(options.path);
    QVERIFY(!session.recordArtifactPublished(output, &actual, &error));
    QCOMPARE(readBytes(options.path), journalBefore);
    QVERIFY(writeBytes(output, readBytes(staged)));
    QVERIFY2(session.recordArtifactPublished(output, &actual, &error), qPrintable(error));
    const auto acknowledged = readBytes(options.path);
    QVERIFY(session.recordArtifactPublished(output, &actual, &error));
    QCOMPARE(readBytes(options.path), acknowledged);
    const auto reconciled = RModelCommandSession::reconcilePublications(options);
    QCOMPARE(reconciled["status"].toString(), QStringLiteral("reconciled"));
    QVERIFY(reconciled["outcomes"].toArray().isEmpty());
}

void TestModelCommandSession::nullableBoundaryAuditPreservesDisabledAxes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    RModel model = modalModel();
    model.setNLines(1);
    const QString target = QStringLiteral("g:%1").arg(model.getLine(0).getPhysicalId());
    RModelCommandSession::JournalOptions options;
    options.path = directory.filePath(QStringLiteral("nullable-boundary.json"));
    RModelCommandSession session(model, options);
    auto command = request(session, QStringLiteral("entity.boundary.set"));
    command["parameters"] = QJsonObject{{"targetId", target}, {"kind", "displacement"},
        {"values", QJsonArray{0.0, QJsonValue(QJsonValue::Null), 0.0}}, {"unit", "m"}};
    command["dryRun"] = true;
    const auto preview = session.execute(command);
    QCOMPARE(preview["status"].toString(), QStringLiteral("preview"));
    command["dryRun"] = false;
    const auto accepted = session.execute(command);
    QVERIFY2(accepted["status"].toString() == QStringLiteral("accepted"),
             qPrintable(QJsonDocument(accepted).toJson(QJsonDocument::Compact)));
    const QJsonObject normalized = accepted["normalizedParameters"].toObject();
    QVector<RInteractionRecordV1> records;
    QString error;
    QVERIFY2(journalRecords(options.path, records, error), qPrintable(error));
    QCOMPARE(records.size(), 2);
    for (const auto &record : records)
    {
        QCOMPARE(record.commandId, QStringLiteral("entity.boundary.set"));
        QCOMPARE(record.selectionSnapshot["parametersEncoding"].toString(), QStringLiteral("json-text/v1"));
        const auto decoded = QJsonDocument::fromJson(record.parameters["json"].toString().toUtf8());
        QVERIFY(decoded.isObject());
        QCOMPARE(decoded.object(), normalized);
        QVERIFY(decoded.object()["values"].toArray().at(1).isNull());
        QVERIFY(record.validate(&error));
    }
    QCOMPARE(session.execute(command), accepted);
    QVERIFY(journalRecords(options.path, records, error));
    QCOMPARE(records.size(), 2);
}

QTEST_APPLESS_MAIN(TestModelCommandSession)
#include "tst_model_command_session.moc"
