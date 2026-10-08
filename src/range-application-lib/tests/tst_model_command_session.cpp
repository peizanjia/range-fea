#include <QtTest>
#include <rmodel_command_session.h>
#include <rbl_interaction_record_v1.h>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <limits>

namespace {
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
    QCOMPARE(records[2].selectionSnapshot["stateHashScope"].toString(), QStringLiteral("supported_model_state"));
    QCOMPARE(records[2].selectionSnapshot["stateHashScopeVersion"].toInt(), 2);
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
