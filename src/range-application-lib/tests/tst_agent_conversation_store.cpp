#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>

#include <ragent_conversation_store.h>
#include <rproject_catalog.h>

namespace
{
QString uuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

QJsonObject userEntry(const QString &text, const QString &messageId = uuid())
{
    return {{QStringLiteral("messageId"), messageId},
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("text"), text}};
}

QJsonObject assistantEntry(const QString &text, const QString &messageId = uuid(),
                           const QString &taskId = {})
{
    QJsonObject result{{QStringLiteral("messageId"), messageId},
                       {QStringLiteral("role"), QStringLiteral("assistant")},
                       {QStringLiteral("text"), text}};
    if (!taskId.isEmpty()) result.insert(QStringLiteral("taskId"), taskId);
    return result;
}

QJsonObject factEntry(const QString &messageId = uuid(), const QString &taskId = {})
{
    QJsonObject facts{{QStringLiteral("status"), QStringLiteral("accepted")},
                      {QStringLiteral("code"), QStringLiteral("operation.accepted")},
                      {QStringLiteral("command"), QStringLiteral("modal.preview")},
                      {QStringLiteral("operationId"), uuid()},
                      {QStringLiteral("jobId"), QStringLiteral("123456789012345678901")},
                      {QStringLiteral("sourceDocumentId"), uuid()},
                      {QStringLiteral("sourceRevision"), QStringLiteral("r1")},
                      {QStringLiteral("acceptedModelSha256"), QString(64, QLatin1Char('a'))},
                      {QStringLiteral("idempotencyKey"), uuid()},
                      {QStringLiteral("requestSha256"), QString(64, QLatin1Char('b'))},
                      {QStringLiteral("workflowProjectId"), uuid()}};
    QJsonObject result{{QStringLiteral("messageId"), messageId},
                       {QStringLiteral("role"), QStringLiteral("fact")},
                       {QStringLiteral("text"), QStringLiteral("Modal preview accepted.")},
                       {QStringLiteral("facts"), facts}};
    if (!taskId.isEmpty()) result.insert(QStringLiteral("taskId"), taskId);
    return result;
}

QJsonObject effectFact(const QString &status, const QString &taskId,
                       bool useRequestSha, const QString &messageId = uuid())
{
    QJsonObject entry = factEntry(messageId, taskId);
    QJsonObject facts = entry.value(QStringLiteral("facts")).toObject();
    facts.insert(QStringLiteral("status"), status);
    if (useRequestSha) facts.remove(QStringLiteral("idempotencyKey"));
    else facts.remove(QStringLiteral("requestSha256"));
    entry.insert(QStringLiteral("facts"), facts);
    return entry;
}

QJsonObject effectOutcome(const QJsonObject &source, const QString &status,
                          const QString &newMessageId = uuid())
{
    QJsonObject outcome = source;
    outcome.insert(QStringLiteral("messageId"), newMessageId);
    outcome.insert(QStringLiteral("text"), QStringLiteral("Effect lookup completed."));
    QJsonObject facts = outcome.value(QStringLiteral("facts")).toObject();
    facts.insert(QStringLiteral("status"), status);
    outcome.insert(QStringLiteral("facts"), facts);
    return outcome;
}

QJsonObject byteHeavyEntry(const QString &text)
{
    QJsonArray sourceRefs;
    QJsonArray artifactRefs;
    for (int index = 0; index < 16; ++index)
    {
        const QString suffix = QString::number(index).rightJustified(3, QLatin1Char('0'));
        sourceRefs.append(QJsonObject{
            {QStringLiteral("sourceId"), (QStringLiteral("source-") + suffix).leftJustified(128, QLatin1Char('s'))},
            {QStringLiteral("revision"), (QStringLiteral("revision-") + suffix).leftJustified(128, QLatin1Char('r'))},
            {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}});
        artifactRefs.append(QJsonObject{
            {QStringLiteral("artifactId"), (QStringLiteral("artifact-") + suffix).leftJustified(128, QLatin1Char('a'))},
            {QStringLiteral("sha256"), QString(64, QLatin1Char('b'))}});
    }
    return {{QStringLiteral("messageId"), uuid()},
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("text"), text},
            {QStringLiteral("sourceRefs"), sourceRefs},
            {QStringLiteral("artifactRefs"), artifactRefs}};
}

qint64 appendRecordSize(const QString &projectId, const QString &conversationId,
                        int revision, const QJsonArray &entries)
{
    const QJsonObject record{{QStringLiteral("schema"), QStringLiteral("range.agent-conversation-record/v1")},
                             {QStringLiteral("storageVersion"), RAgentConversationStore::StorageVersion},
                             {QStringLiteral("projectId"), projectId},
                             {QStringLiteral("conversationId"), conversationId},
                             {QStringLiteral("revision"), QString::number(revision)},
                             {QStringLiteral("commitId"), uuid()},
                             {QStringLiteral("previousSha256"), QString(64, QLatin1Char('a'))},
                             {QStringLiteral("kind"), QStringLiteral("append")},
                             {QStringLiteral("createdAtUtc"), QStringLiteral("2026-01-01T00:00:00.000Z")},
                             {QStringLiteral("entries"), entries}};
    return QJsonDocument(record).toJson(QJsonDocument::Compact).size();
}

qint64 conversationBytesOnDisk(const QString &path)
{
    qint64 total = 0;
    const QFileInfoList files = QDir(path).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &file : files) total += file.size();
    return total;
}

QString textForJsonGrowth(int growthFromOneCharacter)
{
    // Relative to "x", each quote adds one serialized escape byte and each
    // ordinary ASCII character adds one serialized byte.
    const int characterCount = growthFromOneCharacter + 1;
    const int textLength = qMin(characterCount, RAgentConversationStore::MaxTextBytesPerMessage);
    const int quoteCount = characterCount - textLength;
    return QString(quoteCount, QLatin1Char('"'))
        + QString(textLength - quoteCount, QLatin1Char('x'));
}

bool makeSizedHeavyBatch(const QString &projectId, const QString &conversationId,
                         int revision, qint64 targetRecordBytes, QJsonArray *entries)
{
    constexpr int MaxTextBytes = RAgentConversationStore::MaxTextBytesPerMessage;
    constexpr int MaxGrowthPerVariableText = 2 * MaxTextBytes - 1;
    for (int count = 2; count <= RAgentConversationStore::MaxPageSize; ++count)
    {
        QJsonArray candidate;
        for (int index = 0; index < count - 2; ++index)
            candidate.append(byteHeavyEntry(QString(MaxTextBytes, QLatin1Char('"'))));
        candidate.append(byteHeavyEntry(QStringLiteral("x")));
        candidate.append(byteHeavyEntry(QStringLiteral("x")));

        const qint64 baseBytes = appendRecordSize(projectId, conversationId, revision, candidate);
        QJsonArray maxSizeCandidate = candidate;
        QJsonObject firstVariable = maxSizeCandidate.at(count - 2).toObject();
        QJsonObject secondVariable = maxSizeCandidate.at(count - 1).toObject();
        firstVariable.insert(QStringLiteral("text"), QString(MaxTextBytes, QLatin1Char('"')));
        secondVariable.insert(QStringLiteral("text"), QString(MaxTextBytes, QLatin1Char('"')));
        maxSizeCandidate.replace(count - 2, firstVariable);
        maxSizeCandidate.replace(count - 1, secondVariable);
        const qint64 maxBytes = appendRecordSize(projectId, conversationId, revision, maxSizeCandidate);
        if (targetRecordBytes < baseBytes || targetRecordBytes > maxBytes) continue;

        const qint64 growth = targetRecordBytes - baseBytes;
        if (growth > 2LL * MaxGrowthPerVariableText) continue;
        const int firstGrowth = int(qMin(growth, qint64(MaxGrowthPerVariableText)));
        const int secondGrowth = int(growth - firstGrowth);
        QJsonObject firstSized = candidate.at(count - 2).toObject();
        QJsonObject secondSized = candidate.at(count - 1).toObject();
        firstSized.insert(QStringLiteral("text"), textForJsonGrowth(firstGrowth));
        secondSized.insert(QStringLiteral("text"), textForJsonGrowth(secondGrowth));
        candidate.replace(count - 2, firstSized);
        candidate.replace(count - 1, secondSized);
        if (appendRecordSize(projectId, conversationId, revision, candidate) != targetRecordBytes)
            return false;
        if (entries) *entries = candidate;
        return true;
    }
    return false;
}

QJsonObject appendUserMessages(RAgentConversationStore &store, const QString &conversationId,
                              int count, QString *error)
{
    QString expectedRevision = QStringLiteral("1");
    QJsonObject current;
    int remaining = count;
    int messageIndex = 0;
    while (remaining > 0)
    {
        const int batchSize = qMin(remaining, RAgentConversationStore::MaxPageSize);
        QJsonArray batch;
        for (int index = 0; index < batchSize; ++index)
            batch.append(userEntry(QStringLiteral("Capacity message %1").arg(messageIndex++)));
        current = store.append(conversationId, expectedRevision, batch, error);
        if (current.isEmpty()) return {};
        expectedRevision = current.value(QStringLiteral("revision")).toString();
        remaining -= batchSize;
    }
    return current;
}

struct ProjectFixture
{
    QTemporaryDir directory;
    std::unique_ptr<RProjectCatalog> catalog;
    QString error;

    bool open()
    {
        catalog = RProjectCatalog::open(directory.path(), &error);
        return bool(catalog);
    }

    bool configure(RAgentConversationStore &store)
    {
        return store.configure(catalog->workspace(), catalog->projectId(), &error);
    }

    QString conversationPath(const QString &conversationId) const
    {
        return QDir(catalog->workspace()).filePath(QStringLiteral("conversations/%1").arg(conversationId));
    }
};
}

class TstAgentConversationStore : public QObject
{
    Q_OBJECT

private slots:
    void createAppendReopenAndExport();
    void boundedPagesBranchAndDeleteSource();
    void revisionConflictAndExactRetry();
    void tamperedCommittedRecordFailsClosedAndIsPreserved();
    void partialUncommittedBlobIsDiagnosedAndIgnored();
    void sensitiveValuesAndAssignmentsAreRedacted();
    void unsupportedRolesAndAuthorityFieldsAreRejected();
    void executionReferencesPersistBoundTrustedIdentity();
    void cancellationFactsPersistWhenNoChildJobsExist();
    void lookupFactsRemainHistoricalEvidence();
    void unresolvedReservationsProtectLateOutcome();
    void unresolvedReservationProtectsConversationByteLimit();
    void duplicateUnknownReferenceReservesOnce();
    void branchCopiesUnresolvedReservations();
    void unverifiedDeletionContentsArePreserved();
    void workspaceProjectBindingIsChecked();
};

void TstAgentConversationStore::createAppendReopenAndExport()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore first;
    QVERIFY2(project.configure(first), qPrintable(project.error));

    const QJsonObject created = first.create(QStringLiteral("Beam review"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    QCOMPARE(created.value(QStringLiteral("revision")).toString(), QStringLiteral("1"));
    QCOMPARE(created.value(QStringLiteral("messageCount")).toInt(), 0);

    const QString taskId = uuid();
    const QString userMessageId = uuid();
    QJsonArray entries{userEntry(QStringLiteral("Why did this mode shift?"), userMessageId),
                       assistantEntry(QStringLiteral("The accepted source revision is r1."), uuid(), taskId),
                       factEntry(uuid(), taskId)};
    const QJsonObject appended = first.append(created.value(QStringLiteral("conversationId")).toString(),
                                               QStringLiteral("1"), entries, &project.error);
    QVERIFY2(!appended.isEmpty(), qPrintable(project.error));
    QCOMPARE(appended.value(QStringLiteral("revision")).toString(), QStringLiteral("2"));
    QCOMPARE(appended.value(QStringLiteral("messageCount")).toInt(), 3);

    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject page = reopened.page(created.value(QStringLiteral("conversationId")).toString(),
                                           0, 50, &project.error);
    QVERIFY2(!page.isEmpty(), qPrintable(project.error));
    QCOMPARE(page.value(QStringLiteral("revision")).toString(), QStringLiteral("2"));
    QCOMPARE(page.value(QStringLiteral("messages")).toArray().size(), 3);
    QCOMPARE(page.value(QStringLiteral("messages")).toArray().at(0).toObject()
                 .value(QStringLiteral("messageId")).toString(), userMessageId);

    const QJsonObject exported = reopened.exportConversation(
        created.value(QStringLiteral("conversationId")).toString(), &project.error);
    QVERIFY2(!exported.isEmpty(), qPrintable(project.error));
    QCOMPARE(exported.value(QStringLiteral("schema")).toString(),
             QStringLiteral("range.agent-conversation-export/v1"));
    QCOMPARE(exported.value(QStringLiteral("messages")).toArray().size(), 3);
    const QJsonObject exportedFact = exported.value(QStringLiteral("messages")).toArray().at(2).toObject();
    QCOMPARE(exportedFact.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("jobId")).toString(), QStringLiteral("123456789012345678901"));
    const QJsonObject exportedFacts = exportedFact.value(QStringLiteral("facts")).toObject();
    QVERIFY(!QUuid(exportedFacts.value(QStringLiteral("idempotencyKey")).toString()).isNull());
    QCOMPARE(exportedFacts.value(QStringLiteral("requestSha256")).toString(), QString(64, QLatin1Char('b')));
    QVERIFY(!QUuid(exportedFacts.value(QStringLiteral("workflowProjectId")).toString()).isNull());
    QVERIFY(!exportedFact.contains(QStringLiteral("approvalToken")));
    QVERIFY(!exportedFact.contains(QStringLiteral("pendingApproval")));

    const QJsonObject listed = reopened.list(0, 20, &project.error);
    QVERIFY2(!listed.isEmpty(), qPrintable(project.error));
    QCOMPARE(listed.value(QStringLiteral("totalCount")).toInt(), 1);
    QCOMPARE(listed.value(QStringLiteral("conversations")).toArray().size(), 1);
    QCOMPARE(listed.value(QStringLiteral("conversations")).toArray().first().toObject()
                 .value(QStringLiteral("messageCount")).toInt(), 3);
}

void TstAgentConversationStore::boundedPagesBranchAndDeleteSource()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));

    const QJsonObject source = store.create(QStringLiteral("Long conversation"), &project.error);
    QVERIFY2(!source.isEmpty(), qPrintable(project.error));
    const QString sourceId = source.value(QStringLiteral("conversationId")).toString();
    QJsonArray firstBatch;
    for (int index = 0; index < 50; ++index)
        firstBatch.append(userEntry(QStringLiteral("Question %1").arg(index)));
    QJsonObject updated = store.append(sourceId, QStringLiteral("1"), firstBatch, &project.error);
    QVERIFY2(!updated.isEmpty(), qPrintable(project.error));
    QCOMPARE(updated.value(QStringLiteral("revision")).toString(), QStringLiteral("2"));

    QJsonArray secondBatch;
    QString boundaryMessageId;
    QString assistantMessageId;
    QString taskId = uuid();
    for (int index = 50; index < 54; ++index)
    {
        const QJsonObject item = index == 51
            ? assistantEntry(QStringLiteral("Task result remains historical."), uuid(), taskId)
            : userEntry(QStringLiteral("Question %1").arg(index));
        secondBatch.append(item);
        if (index == 51) assistantMessageId = item.value(QStringLiteral("messageId")).toString();
    }
    const QJsonObject fact = factEntry(uuid(), taskId);
    boundaryMessageId = fact.value(QStringLiteral("messageId")).toString();
    secondBatch.append(fact);
    updated = store.append(sourceId, QStringLiteral("2"), secondBatch, &project.error);
    QVERIFY2(!updated.isEmpty(), qPrintable(project.error));
    QCOMPARE(updated.value(QStringLiteral("messageCount")).toInt(), 55);

    const QJsonObject firstPage = store.page(sourceId, 0, 50, &project.error);
    QVERIFY2(!firstPage.isEmpty(), qPrintable(project.error));
    QCOMPARE(firstPage.value(QStringLiteral("messages")).toArray().size(), 50);
    QVERIFY(firstPage.value(QStringLiteral("hasMore")).toBool());
    const QJsonObject secondPage = store.page(sourceId, 50, 50, &project.error);
    QVERIFY2(!secondPage.isEmpty(), qPrintable(project.error));
    QCOMPARE(secondPage.value(QStringLiteral("messages")).toArray().size(), 5);
    QVERIFY(!secondPage.value(QStringLiteral("hasMore")).toBool());

    updated = store.append(sourceId, QStringLiteral("3"),
                           QJsonArray{assistantEntry(QStringLiteral("One later message."))}, &project.error);
    QVERIFY2(!updated.isEmpty(), qPrintable(project.error));
    QCOMPARE(updated.value(QStringLiteral("messageCount")).toInt(), 56);

    const QJsonObject branch = store.branch(sourceId, boundaryMessageId,
                                            QStringLiteral("Read-only follow-up"), &project.error);
    QVERIFY2(!branch.isEmpty(), qPrintable(project.error));
    QCOMPARE(branch.value(QStringLiteral("messageCount")).toInt(), 55);
    QVERIFY(branch.value(QStringLiteral("branchSource")).isObject());
    QCOMPARE(branch.value(QStringLiteral("branchSource")).toObject()
                 .value(QStringLiteral("conversationId")).toString(), sourceId);

    const QString branchId = branch.value(QStringLiteral("conversationId")).toString();
    const QJsonObject branchExport = store.exportConversation(branchId, &project.error);
    QVERIFY2(!branchExport.isEmpty(), qPrintable(project.error));
    const QJsonArray copiedMessages = branchExport.value(QStringLiteral("messages")).toArray();
    QVERIFY(copiedMessages.size() == 55);
    const QJsonObject copiedAssistant = copiedMessages.at(51).toObject();
    QVERIFY(copiedAssistant.value(QStringLiteral("messageId")).toString() != assistantMessageId);
    QCOMPARE(copiedAssistant.value(QStringLiteral("taskId")).toString(), taskId);
    QCOMPARE(copiedAssistant.value(QStringLiteral("branchOrigin")).toObject()
                 .value(QStringLiteral("messageId")).toString(), assistantMessageId);
    QVERIFY(!copiedAssistant.contains(QStringLiteral("approval")));
    QVERIFY(!copiedAssistant.contains(QStringLiteral("approvalToken")));
    const QJsonObject copiedFact = copiedMessages.at(54).toObject();
    QCOMPARE(copiedFact.value(QStringLiteral("taskId")).toString(), taskId);
    QCOMPARE(copiedFact.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("r1"));
    QVERIFY(!QUuid(copiedFact.value(QStringLiteral("facts")).toObject()
                       .value(QStringLiteral("idempotencyKey")).toString()).isNull());
    QCOMPARE(copiedFact.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("requestSha256")).toString(), QString(64, QLatin1Char('b')));
    QVERIFY(!QUuid(copiedFact.value(QStringLiteral("facts")).toObject()
                       .value(QStringLiteral("workflowProjectId")).toString()).isNull());
    QCOMPARE(copiedFact.value(QStringLiteral("branchOrigin")).toObject()
                 .value(QStringLiteral("messageId")).toString(), boundaryMessageId);

    QVERIFY2(store.remove(sourceId, updated.value(QStringLiteral("revision")).toString(), &project.error),
             qPrintable(project.error));
    const QJsonObject survivingBranch = store.page(branchId, 50, 10, &project.error);
    QVERIFY2(!survivingBranch.isEmpty(), qPrintable(project.error));
    QCOMPARE(survivingBranch.value(QStringLiteral("messageCount")).toInt(), 55);
    QCOMPARE(survivingBranch.value(QStringLiteral("messages")).toArray().size(), 5);
    QVERIFY(!QFileInfo::exists(project.conversationPath(sourceId)));
    QVERIFY(QFileInfo::exists(project.conversationPath(branchId)));
}

void TstAgentConversationStore::revisionConflictAndExactRetry()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Revision test"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString id = created.value(QStringLiteral("conversationId")).toString();
    const QString messageId = uuid();
    const QJsonArray entries{userEntry(QStringLiteral("Exactly once"), messageId)};

    const QJsonObject first = store.append(id, QStringLiteral("1"), entries, &project.error);
    QVERIFY2(!first.isEmpty(), qPrintable(project.error));
    const QJsonObject retry = store.append(id, QStringLiteral("1"), entries, &project.error);
    QVERIFY2(!retry.isEmpty(), qPrintable(project.error));
    QCOMPARE(retry.value(QStringLiteral("revision")).toString(), QStringLiteral("2"));
    QCOMPARE(retry.value(QStringLiteral("messageCount")).toInt(), 1);

    QString conflict;
    QVERIFY(store.append(id, QStringLiteral("1"),
                         QJsonArray{userEntry(QStringLiteral("Different retry"), messageId)}, &conflict).isEmpty());
    QVERIFY(!conflict.isEmpty());
    QVERIFY(store.append(id, QStringLiteral("1"),
                         QJsonArray{userEntry(QStringLiteral("Stale revision"))}, &conflict).isEmpty());
    QVERIFY(!conflict.isEmpty());

    const QJsonObject next = store.append(id, QStringLiteral("2"),
                                          QJsonArray{assistantEntry(QStringLiteral("Continued."))}, &project.error);
    QVERIFY2(!next.isEmpty(), qPrintable(project.error));
    QCOMPARE(next.value(QStringLiteral("revision")).toString(), QStringLiteral("3"));
}

void TstAgentConversationStore::tamperedCommittedRecordFailsClosedAndIsPreserved()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject first = store.create(QStringLiteral("Tamper target"), &project.error);
    QVERIFY2(!first.isEmpty(), qPrintable(project.error));
    const QJsonObject second = store.create(QStringLiteral("Untouched conversation"), &project.error);
    QVERIFY2(!second.isEmpty(), qPrintable(project.error));
    const QString id = first.value(QStringLiteral("conversationId")).toString();
    const QString untouchedId = second.value(QStringLiteral("conversationId")).toString();
    const QString directory = project.conversationPath(id);
    const QStringList blobs = QDir(directory).entryList({QStringLiteral("record-*.json")}, QDir::Files);
    QCOMPARE(blobs.size(), 1);
    const QString blobPath = QDir(directory).filePath(blobs.first());
    QFile blob(blobPath);
    QVERIFY(blob.open(QIODevice::Append));
    QCOMPARE(blob.write(" "), qint64(1));
    blob.close();

    QString error;
    QVERIFY(store.page(id, 0, 10, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(blobPath));
    QVERIFY(QFileInfo::exists(project.conversationPath(untouchedId)));
    const QStringList untouchedBlobs = QDir(project.conversationPath(untouchedId))
                                           .entryList({QStringLiteral("record-*.json")}, QDir::Files);
    QCOMPARE(untouchedBlobs.size(), 1);
    QVERIFY(QFileInfo::exists(QDir(project.conversationPath(untouchedId)).filePath(untouchedBlobs.first())));
}

void TstAgentConversationStore::partialUncommittedBlobIsDiagnosedAndIgnored()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Interrupted commit"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString id = created.value(QStringLiteral("conversationId")).toString();
    const QString orphanPath = QDir(project.conversationPath(id)).filePath(
        QStringLiteral("record-r00000002-%1.json").arg(uuid()));
    QFile orphan(orphanPath);
    QVERIFY(orphan.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(orphan.write("{"), qint64(1));
    orphan.close();

    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject page = reopened.page(id, 0, 10, &project.error);
    QVERIFY2(!page.isEmpty(), qPrintable(project.error));
    QCOMPARE(page.value(QStringLiteral("revision")).toString(), QStringLiteral("1"));
    QCOMPARE(page.value(QStringLiteral("messages")).toArray().size(), 0);
    QCOMPARE(page.value(QStringLiteral("diagnostics")).toArray().first().toObject()
                 .value(QStringLiteral("code")).toString(), QStringLiteral("orphan_uncommitted_records"));
    QVERIFY(QFileInfo::exists(orphanPath));
}

void TstAgentConversationStore::sensitiveValuesAndAssignmentsAreRedacted()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    store.setSensitiveValues({QStringLiteral("sk-live-sensitive-literal")});
    const QJsonObject created = store.create(QStringLiteral("Credential test"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QJsonArray entries{userEntry(QStringLiteral(
        "Key sk-live-sensitive-literal; api_key=ABC123 token: DEF456 password='GHI789'."))};
    const QJsonObject appended = store.append(created.value(QStringLiteral("conversationId")).toString(),
                                              QStringLiteral("1"), entries, &project.error);
    QVERIFY2(!appended.isEmpty(), qPrintable(project.error));

    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject exported = reopened.exportConversation(
        created.value(QStringLiteral("conversationId")).toString(), &project.error);
    QVERIFY2(!exported.isEmpty(), qPrintable(project.error));
    const QString savedText = exported.value(QStringLiteral("messages")).toArray().first().toObject()
                                  .value(QStringLiteral("text")).toString();
    QVERIFY(!savedText.contains(QStringLiteral("sk-live-sensitive-literal")));
    QVERIFY(!savedText.contains(QStringLiteral("ABC123")));
    QVERIFY(!savedText.contains(QStringLiteral("DEF456")));
    QVERIFY(!savedText.contains(QStringLiteral("GHI789")));
    QVERIFY(savedText.contains(QStringLiteral("[REDACTED]")));
}

void TstAgentConversationStore::unsupportedRolesAndAuthorityFieldsAreRejected()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Schema test"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString id = created.value(QStringLiteral("conversationId")).toString();

    QJsonObject withApproval = userEntry(QStringLiteral("Visible text only"));
    withApproval.insert(QStringLiteral("approvalToken"), QStringLiteral("must-not-persist"));
    QString error;
    QVERIFY(store.append(id, QStringLiteral("1"), QJsonArray{withApproval}, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    QJsonObject toolMessage = assistantEntry(QStringLiteral("Raw tool payload"));
    toolMessage.insert(QStringLiteral("role"), QStringLiteral("tool"));
    QVERIFY(store.append(id, QStringLiteral("1"), QJsonArray{toolMessage}, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    QJsonObject factWithBlob = factEntry();
    QJsonObject facts = factWithBlob.value(QStringLiteral("facts")).toObject();
    facts.insert(QStringLiteral("request"), QJsonObject{{QStringLiteral("payload"), QStringLiteral("arbitrary")} });
    factWithBlob.insert(QStringLiteral("facts"), facts);
    QVERIFY(store.append(id, QStringLiteral("1"), QJsonArray{factWithBlob}, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    QJsonObject factWithApprovalPointer = factEntry();
    facts = factWithApprovalPointer.value(QStringLiteral("facts")).toObject();
    facts.insert(QStringLiteral("approvalToken"), QStringLiteral("must-not-persist"));
    factWithApprovalPointer.insert(QStringLiteral("facts"), facts);
    QVERIFY(store.append(id, QStringLiteral("1"), QJsonArray{factWithApprovalPointer}, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    QJsonObject factWithPreviewPointer = factEntry();
    facts = factWithPreviewPointer.value(QStringLiteral("facts")).toObject();
    facts.insert(QStringLiteral("previewPointer"), QStringLiteral("must-not-persist"));
    factWithPreviewPointer.insert(QStringLiteral("facts"), facts);
    QVERIFY(store.append(id, QStringLiteral("1"), QJsonArray{factWithPreviewPointer}, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    const QJsonObject unchanged = store.page(id, 0, 10, &project.error);
    QVERIFY2(!unchanged.isEmpty(), qPrintable(project.error));
    QCOMPARE(unchanged.value(QStringLiteral("messageCount")).toInt(), 0);
}

void TstAgentConversationStore::executionReferencesPersistBoundTrustedIdentity()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Trusted execution intent"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString conversationId = created.value(QStringLiteral("conversationId")).toString();
    const QString taskId = uuid();
    const QString experimentId = uuid();
    const QString previewId = uuid();
    const QString sourceDocumentId = uuid();
    const QString sourceSessionEpoch = uuid();
    const QString authorizationDigest(64, QLatin1Char('c'));
    const QString trustedAuthorizationPreviewId = uuid();
    const QString trustedAuthorizationExperimentId = uuid();
    const QString trustedAuthorizationDigest(64, QLatin1Char('d'));
    const QJsonObject facts{{QStringLiteral("status"), QStringLiteral("pending")},
        {QStringLiteral("code"), QStringLiteral("OUTCOME_NOT_OBSERVED")},
        {QStringLiteral("command"), QStringLiteral("experiment.run")},
        {QStringLiteral("idempotencyKey"), uuid()},
        {QStringLiteral("experimentId"), experimentId},
        {QStringLiteral("previewId"), previewId},
        {QStringLiteral("sourceDocumentId"), sourceDocumentId},
        {QStringLiteral("sourceSessionEpoch"), sourceSessionEpoch},
        {QStringLiteral("sourceRevision"), QStringLiteral("7")},
        {QStringLiteral("authorizationDigest"), authorizationDigest},
        {QStringLiteral("authorizationMode"), QStringLiteral("bounded_task_authorization")},
        {QStringLiteral("trustedAuthorizationPreviewId"), trustedAuthorizationPreviewId},
        {QStringLiteral("trustedAuthorizationExperimentId"), trustedAuthorizationExperimentId},
        {QStringLiteral("trustedAuthorizationDigest"), trustedAuthorizationDigest}};
    const QJsonObject intent{{QStringLiteral("messageId"), uuid()},
        {QStringLiteral("taskId"), taskId}, {QStringLiteral("role"), QStringLiteral("fact")},
        {QStringLiteral("text"), QStringLiteral("Trusted experiment intent was recorded before submission.")},
        {QStringLiteral("facts"), facts}};
    const QJsonObject appended = store.append(conversationId, QStringLiteral("1"), QJsonArray{intent}, &project.error);
    QVERIFY2(!appended.isEmpty(), qPrintable(project.error));
    QCOMPARE(appended.value(QStringLiteral("messageCount")).toInt(), 1);

    QJsonObject malformedFacts = facts;
    malformedFacts.insert(QStringLiteral("authorizationDigest"), QStringLiteral("invalid-digest"));
    QJsonObject malformedIntent = intent;
    malformedIntent.insert(QStringLiteral("messageId"), uuid());
    malformedIntent.insert(QStringLiteral("facts"), malformedFacts);
    QString error;
    QVERIFY(store.append(conversationId, QStringLiteral("2"), QJsonArray{malformedIntent}, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject saved = reopened.exportConversation(conversationId, &project.error);
    QVERIFY2(!saved.isEmpty(), qPrintable(project.error));
    const QJsonObject savedFacts = saved.value(QStringLiteral("messages")).toArray().first().toObject()
        .value(QStringLiteral("facts")).toObject();
    QCOMPARE(savedFacts.value(QStringLiteral("experimentId")).toString(), experimentId);
    QCOMPARE(savedFacts.value(QStringLiteral("previewId")).toString(), previewId);
    QCOMPARE(savedFacts.value(QStringLiteral("sourceDocumentId")).toString(), sourceDocumentId);
    QCOMPARE(savedFacts.value(QStringLiteral("sourceSessionEpoch")).toString(), sourceSessionEpoch);
    QCOMPARE(savedFacts.value(QStringLiteral("sourceRevision")).toString(), QStringLiteral("7"));
    QCOMPARE(savedFacts.value(QStringLiteral("authorizationDigest")).toString(), authorizationDigest);
    QCOMPARE(savedFacts.value(QStringLiteral("authorizationMode")).toString(), QStringLiteral("bounded_task_authorization"));
    QCOMPARE(savedFacts.value(QStringLiteral("trustedAuthorizationPreviewId")).toString(), trustedAuthorizationPreviewId);
    QCOMPARE(savedFacts.value(QStringLiteral("trustedAuthorizationExperimentId")).toString(), trustedAuthorizationExperimentId);
    QCOMPARE(savedFacts.value(QStringLiteral("trustedAuthorizationDigest")).toString(), trustedAuthorizationDigest);
}

void TstAgentConversationStore::cancellationFactsPersistWhenNoChildJobsExist()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Cancellation evidence"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString conversationId = created.value(QStringLiteral("conversationId")).toString();
    const QString experimentId = uuid();
    const QJsonObject facts{{QStringLiteral("schema"), QStringLiteral("range.agent-experiment-cancellation/v1")},
        {QStringLiteral("experimentId"), experimentId},
        {QStringLiteral("sourceDocumentId"), uuid()}, {QStringLiteral("sourceSessionEpoch"), uuid()},
        {QStringLiteral("authorizationDigest"), QString(64, QLatin1Char('d'))},
        {QStringLiteral("sourceRevision"), QStringLiteral("12")},
        {QStringLiteral("trigger"), QStringLiteral("user_stop")},
        {QStringLiteral("status"), QStringLiteral("cancelled")},
        {QStringLiteral("code"), QStringLiteral("CANCELLED")},
        {QStringLiteral("beforeState"), QStringLiteral("prepared")},
        {QStringLiteral("afterState"), QStringLiteral("cancelled")},
        {QStringLiteral("cancellationPersisted"), QStringLiteral("true")},
        {QStringLiteral("childCancellationRequestFailed"), QStringLiteral("false")},
        {QStringLiteral("childJobSetMatches"), QStringLiteral("true")},
        {QStringLiteral("childJobsTerminal"), QStringLiteral("true")},
        {QStringLiteral("childJobCount"), QStringLiteral("0")},
        {QStringLiteral("terminalChildJobCount"), QStringLiteral("0")},
        {QStringLiteral("childJobStates"), QStringLiteral("none")},
        {QStringLiteral("childJobsTruncated"), QStringLiteral("false")}};
    const QJsonObject entry{{QStringLiteral("messageId"), uuid()},
        {QStringLiteral("taskId"), uuid()}, {QStringLiteral("role"), QStringLiteral("fact")},
        {QStringLiteral("text"), QStringLiteral("The Host confirmed cancellation before a child job started.")},
        {QStringLiteral("facts"), facts}};
    const QJsonObject appended = store.append(conversationId, QStringLiteral("1"), QJsonArray{entry}, &project.error);
    QVERIFY2(!appended.isEmpty(), qPrintable(project.error));

    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject page = reopened.page(conversationId, 0, 10, &project.error);
    QVERIFY2(!page.isEmpty(), qPrintable(project.error));
    QCOMPARE(page.value(QStringLiteral("messages")).toArray().size(), 1);
    const QJsonObject persistedFacts = page.value(QStringLiteral("messages")).toArray().first().toObject()
        .value(QStringLiteral("facts")).toObject();
    QCOMPARE(persistedFacts.value(QStringLiteral("experimentId")).toString(), experimentId);
    QCOMPARE(persistedFacts.value(QStringLiteral("childJobStates")).toString(), QStringLiteral("none"));
    QCOMPARE(persistedFacts.value(QStringLiteral("childJobCount")).toString(), QStringLiteral("0"));
    QCOMPARE(persistedFacts.value(QStringLiteral("childJobSetMatches")).toString(), QStringLiteral("true"));
}

void TstAgentConversationStore::lookupFactsRemainHistoricalEvidence()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Reconciliation evidence"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString conversationId = created.value(QStringLiteral("conversationId")).toString();

    QJsonObject pending = factEntry();
    QJsonObject pendingFacts = pending.value(QStringLiteral("facts")).toObject();
    pendingFacts.insert(QStringLiteral("status"), QStringLiteral("pending"));
    pendingFacts.insert(QStringLiteral("workflowProjectId"), project.catalog->projectId());
    pending.insert(QStringLiteral("facts"), pendingFacts);
    QJsonObject unknown = factEntry();
    QJsonObject unknownFacts = unknown.value(QStringLiteral("facts")).toObject();
    unknownFacts.insert(QStringLiteral("status"), QStringLiteral("unknown"));
    unknownFacts.insert(QStringLiteral("workflowProjectId"), project.catalog->projectId());
    unknown.insert(QStringLiteral("facts"), unknownFacts);

    const QJsonObject appended = store.append(conversationId, QStringLiteral("1"),
                                               QJsonArray{pending, unknown}, &project.error);
    QVERIFY2(!appended.isEmpty(), qPrintable(project.error));
    const QJsonObject rejected = effectOutcome(pending, QStringLiteral("rejected"));
    const QJsonObject afterRejection = store.append(
        conversationId, appended.value(QStringLiteral("revision")).toString(),
        QJsonArray{rejected}, &project.error);
    QVERIFY2(!afterRejection.isEmpty(), qPrintable(project.error));
    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject exported = reopened.exportConversation(conversationId, &project.error);
    QVERIFY2(!exported.isEmpty(), qPrintable(project.error));
    const QJsonArray messages = exported.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 3);
    const QJsonObject savedPending = messages.at(0).toObject();
    const QJsonObject savedUnknown = messages.at(1).toObject();
    const QJsonObject savedRejected = messages.at(2).toObject();
    QCOMPARE(savedPending.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QCOMPARE(savedUnknown.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("status")).toString(), QStringLiteral("unknown"));
    QCOMPARE(savedRejected.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("status")).toString(), QStringLiteral("rejected"));
    QCOMPARE(savedPending.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("workflowProjectId")).toString(), project.catalog->projectId());
    for (const QJsonObject &message : {savedPending, savedUnknown, savedRejected})
    {
        QVERIFY(!message.contains(QStringLiteral("approval")));
        QVERIFY(!message.contains(QStringLiteral("approvalToken")));
        QVERIFY(!message.contains(QStringLiteral("previewPointer")));
        QVERIFY(!message.contains(QStringLiteral("pendingControl")));
    }
}

void TstAgentConversationStore::unresolvedReservationsProtectLateOutcome()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));

    const QJsonObject full = store.create(QStringLiteral("Full message history"), &project.error);
    QVERIFY2(!full.isEmpty(), qPrintable(project.error));
    const QString fullId = full.value(QStringLiteral("conversationId")).toString();
    const QJsonObject at511 = appendUserMessages(store, fullId, 511, &project.error);
    QVERIFY2(!at511.isEmpty(), qPrintable(project.error));
    QCOMPARE(at511.value(QStringLiteral("messageCount")).toInt(), 511);
    const QString firstTaskId = uuid();
    const QJsonObject unfitPending = effectFact(QStringLiteral("pending"), firstTaskId, false);
    QVERIFY(store.append(fullId, at511.value(QStringLiteral("revision")).toString(),
                         QJsonArray{unfitPending}, &project.error).isEmpty());

    const QJsonObject source = store.create(QStringLiteral("Reserved late outcome"), &project.error);
    QVERIFY2(!source.isEmpty(), qPrintable(project.error));
    const QString sourceId = source.value(QStringLiteral("conversationId")).toString();
    const QJsonObject at510 = appendUserMessages(store, sourceId, 510, &project.error);
    QVERIFY2(!at510.isEmpty(), qPrintable(project.error));
    const QString taskId = uuid();
    const QJsonObject pending = effectFact(QStringLiteral("pending"), taskId, false);
    const QJsonObject tooEarlyOutcome = effectOutcome(pending, QStringLiteral("accepted"));
    QVERIFY(store.append(sourceId, at510.value(QStringLiteral("revision")).toString(),
                         QJsonArray{pending, tooEarlyOutcome}, &project.error).isEmpty());
    const QJsonObject afterPending = store.append(
        sourceId, at510.value(QStringLiteral("revision")).toString(), QJsonArray{pending}, &project.error);
    QVERIFY2(!afterPending.isEmpty(), qPrintable(project.error));
    QCOMPARE(afterPending.value(QStringLiteral("messageCount")).toInt(), 511);

    const QJsonObject cancellation = effectFact(QStringLiteral("cancelled"), taskId, false);
    QVERIFY(store.append(sourceId, afterPending.value(QStringLiteral("revision")).toString(),
                         QJsonArray{cancellation}, &project.error).isEmpty());
    const QJsonObject userFinished = assistantEntry(QStringLiteral("User finished."), uuid(), taskId);
    QVERIFY(store.append(sourceId, afterPending.value(QStringLiteral("revision")).toString(),
                         QJsonArray{userFinished}, &project.error).isEmpty());
    QJsonObject oversizedReceipt = effectOutcome(pending, QStringLiteral("accepted"));
    oversizedReceipt.insert(QStringLiteral("text"), QString(9000, QLatin1Char('r')));
    QVERIFY(store.append(sourceId, afterPending.value(QStringLiteral("revision")).toString(),
                         QJsonArray{oversizedReceipt}, &project.error).isEmpty());

    const QJsonObject accepted = effectOutcome(pending, QStringLiteral("accepted"));
    const QJsonObject resolved = store.append(
        sourceId, afterPending.value(QStringLiteral("revision")).toString(), QJsonArray{accepted}, &project.error);
    QVERIFY2(!resolved.isEmpty(), qPrintable(project.error));
    QCOMPARE(resolved.value(QStringLiteral("messageCount")).toInt(), 512);
}

void TstAgentConversationStore::unresolvedReservationProtectsConversationByteLimit()
{
    constexpr qint64 ConversationByteLimit = 20LL * 1024 * 1024;
    constexpr qint64 ReceiptReservationBytes = 8LL * 1024;
    constexpr qint64 DesiredFreeBytes = 12LL * 1024;
    constexpr int HeavyBatchSize = RAgentConversationStore::MaxPageSize;

    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));

    const QJsonObject created = store.create(QStringLiteral("Conversation byte reservation"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString conversationId = created.value(QStringLiteral("conversationId")).toString();
    const QString taskId = uuid();
    const QJsonObject pending = effectFact(QStringLiteral("pending"), taskId, false);
    QJsonObject current = store.append(conversationId, QStringLiteral("1"), QJsonArray{pending}, &project.error);
    QVERIFY2(!current.isEmpty(), qPrintable(project.error));

    const QJsonObject heavyTemplate = byteHeavyEntry(
        QString(RAgentConversationStore::MaxTextBytesPerMessage, QLatin1Char('"')));
    const qint64 heavyEntryBytes = QJsonDocument(heavyTemplate).toJson(QJsonDocument::Compact).size();
    QVERIFY(heavyEntryBytes >= 40LL * 1024);
    QVERIFY(heavyEntryBytes <= 43LL * 1024);

    int heavyMessageCount = 0;
    QString expectedRevision = current.value(QStringLiteral("revision")).toString();
    QJsonArray lastAttemptedHeavyBatch;
    while (true)
    {
        QJsonArray heavyBatch;
        for (int index = 0; index < HeavyBatchSize; ++index)
        {
            QJsonObject entry = heavyTemplate;
            entry.insert(QStringLiteral("messageId"), uuid());
            heavyBatch.append(entry);
        }
        lastAttemptedHeavyBatch = heavyBatch;
        const QJsonObject appended = store.append(conversationId, expectedRevision, heavyBatch, &project.error);
        if (appended.isEmpty()) break;
        current = appended;
        expectedRevision = current.value(QStringLiteral("revision")).toString();
        heavyMessageCount += HeavyBatchSize;
    }
    QVERIFY(!project.error.isEmpty());
    QVERIFY(heavyMessageCount >= HeavyBatchSize);
    QVERIFY(heavyMessageCount + 1 < RAgentConversationStore::MaxMessagesPerConversation);
    QCOMPARE(current.value(QStringLiteral("messageCount")).toInt(), heavyMessageCount + 1);
    QCOMPARE(lastAttemptedHeavyBatch.size(), HeavyBatchSize);

    const QString projectId = project.catalog->projectId();
    const qint64 bytesBeforeFill = conversationBytesOnDisk(project.conversationPath(conversationId));
    const qint64 freeBeforeFill = ConversationByteLimit - bytesBeforeFill;
    QVERIFY(freeBeforeFill > DesiredFreeBytes);
    const qint64 rejectedBatchBytes = appendRecordSize(
        projectId, conversationId, expectedRevision.toInt() + 1, lastAttemptedHeavyBatch);
    QVERIFY(rejectedBatchBytes + ReceiptReservationBytes > freeBeforeFill);

    const qint64 fillRecordBytes = freeBeforeFill - DesiredFreeBytes;
    QJsonArray finalFill;
    QVERIFY(makeSizedHeavyBatch(projectId, conversationId, expectedRevision.toInt() + 1,
                                fillRecordBytes, &finalFill));
    QVERIFY(!finalFill.isEmpty());
    QVERIFY(finalFill.size() <= RAgentConversationStore::MaxPageSize);
    QCOMPARE(appendRecordSize(projectId, conversationId, expectedRevision.toInt() + 1, finalFill),
             fillRecordBytes);
    current = store.append(conversationId, expectedRevision, finalFill, &project.error);
    QVERIFY2(!current.isEmpty(), qPrintable(project.error));
    QVERIFY(current.value(QStringLiteral("messageCount")).toInt()
            < RAgentConversationStore::MaxMessagesPerConversation);
    QCOMPARE(conversationBytesOnDisk(project.conversationPath(conversationId)),
             ConversationByteLimit - DesiredFreeBytes);

    const QString filledRevision = current.value(QStringLiteral("revision")).toString();
    QJsonArray attemptedAppend{assistantEntry(QString(8 * 1024, QLatin1Char('x')))};
    const qint64 attemptedAppendBytes = appendRecordSize(
        projectId, conversationId, filledRevision.toInt() + 1, attemptedAppend);
    QVERIFY(attemptedAppendBytes <= DesiredFreeBytes);
    QVERIFY(attemptedAppendBytes + ReceiptReservationBytes > DesiredFreeBytes);
    const qint64 bytesBeforeRejectedAppend = conversationBytesOnDisk(project.conversationPath(conversationId));
    QVERIFY(store.append(conversationId, filledRevision, attemptedAppend, &project.error).isEmpty());
    QVERIFY(!project.error.isEmpty());
    QCOMPARE(conversationBytesOnDisk(project.conversationPath(conversationId)), bytesBeforeRejectedAppend);
    const QJsonObject unchanged = store.page(conversationId, 0, 1, &project.error);
    QVERIFY2(!unchanged.isEmpty(), qPrintable(project.error));
    QCOMPARE(unchanged.value(QStringLiteral("revision")).toString(), filledRevision);

    QJsonObject receipt = effectOutcome(pending, QStringLiteral("accepted"));
    receipt.insert(QStringLiteral("text"), QString(6 * 1024, QLatin1Char('r')));
    const QJsonArray receiptBatch{receipt};
    const qint64 receiptRecordBytes = appendRecordSize(
        projectId, conversationId, filledRevision.toInt() + 1, receiptBatch);
    QVERIFY(receiptRecordBytes <= ReceiptReservationBytes);
    const QJsonObject resolved = store.append(conversationId, filledRevision, receiptBatch, &project.error);
    QVERIFY2(!resolved.isEmpty(), qPrintable(project.error));
    QCOMPARE(resolved.value(QStringLiteral("revision")).toString(),
             QString::number(filledRevision.toInt() + 1));
    QCOMPARE(resolved.value(QStringLiteral("messageCount")).toInt(),
             current.value(QStringLiteral("messageCount")).toInt() + 1);
    QCOMPARE(conversationBytesOnDisk(project.conversationPath(conversationId)),
             bytesBeforeRejectedAppend + receiptRecordBytes);

    RAgentConversationStore reopened;
    QVERIFY2(project.configure(reopened), qPrintable(project.error));
    const QJsonObject lastPage = reopened.page(conversationId,
                                               resolved.value(QStringLiteral("messageCount")).toInt() - 1,
                                               1, &project.error);
    QVERIFY2(!lastPage.isEmpty(), qPrintable(project.error));
    const QJsonArray savedMessages = lastPage.value(QStringLiteral("messages")).toArray();
    QCOMPARE(savedMessages.size(), 1);
    const QJsonObject savedReceipt = savedMessages.first().toObject();
    QCOMPARE(savedReceipt.value(QStringLiteral("messageId")).toString(),
             receipt.value(QStringLiteral("messageId")).toString());
    QCOMPARE(savedReceipt.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("status")).toString(), QStringLiteral("accepted"));
    QCOMPARE(savedReceipt.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("idempotencyKey")).toString(),
             pending.value(QStringLiteral("facts")).toObject()
                 .value(QStringLiteral("idempotencyKey")).toString());
    QCOMPARE(savedReceipt.value(QStringLiteral("text")).toString(), QString(6 * 1024, QLatin1Char('r')));
}

void TstAgentConversationStore::duplicateUnknownReferenceReservesOnce()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Duplicate lookup fact"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString id = created.value(QStringLiteral("conversationId")).toString();
    const QJsonObject at509 = appendUserMessages(store, id, 509, &project.error);
    QVERIFY2(!at509.isEmpty(), qPrintable(project.error));

    const QString taskId = uuid();
    const QJsonObject firstUnknown = effectFact(QStringLiteral("unknown"), taskId, false);
    QJsonObject secondUnknown = firstUnknown;
    secondUnknown.insert(QStringLiteral("messageId"), uuid());
    const QJsonObject afterUnknowns = store.append(
        id, at509.value(QStringLiteral("revision")).toString(), QJsonArray{firstUnknown, secondUnknown},
        &project.error);
    QVERIFY2(!afterUnknowns.isEmpty(), qPrintable(project.error));
    QCOMPARE(afterUnknowns.value(QStringLiteral("messageCount")).toInt(), 511);

    QJsonObject wrongTaskOutcome = effectOutcome(firstUnknown, QStringLiteral("accepted"));
    wrongTaskOutcome.insert(QStringLiteral("taskId"), uuid());
    QVERIFY(store.append(id, afterUnknowns.value(QStringLiteral("revision")).toString(),
                         QJsonArray{wrongTaskOutcome}, &project.error).isEmpty());

    const QJsonObject outcome = effectOutcome(firstUnknown, QStringLiteral("accepted"));
    const QJsonObject resolved = store.append(
        id, afterUnknowns.value(QStringLiteral("revision")).toString(), QJsonArray{outcome}, &project.error);
    QVERIFY2(!resolved.isEmpty(), qPrintable(project.error));
    QCOMPARE(resolved.value(QStringLiteral("messageCount")).toInt(), 512);
}

void TstAgentConversationStore::branchCopiesUnresolvedReservations()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject source = store.create(QStringLiteral("Source with pending effect"), &project.error);
    QVERIFY2(!source.isEmpty(), qPrintable(project.error));
    const QString sourceId = source.value(QStringLiteral("conversationId")).toString();
    const QJsonObject at510 = appendUserMessages(store, sourceId, 510, &project.error);
    QVERIFY2(!at510.isEmpty(), qPrintable(project.error));
    const QString taskId = uuid();
    const QJsonObject pending = effectFact(QStringLiteral("unknown"), taskId, true);
    const QJsonObject afterPending = store.append(
        sourceId, at510.value(QStringLiteral("revision")).toString(), QJsonArray{pending}, &project.error);
    QVERIFY2(!afterPending.isEmpty(), qPrintable(project.error));
    const QString boundary = pending.value(QStringLiteral("messageId")).toString();

    const QJsonObject branch = store.branch(sourceId, boundary,
                                            QStringLiteral("Independent history"), &project.error);
    QVERIFY2(!branch.isEmpty(), qPrintable(project.error));
    QCOMPARE(branch.value(QStringLiteral("messageCount")).toInt(), 511);
    const QString branchId = branch.value(QStringLiteral("conversationId")).toString();
    QVERIFY(store.append(branchId, QStringLiteral("1"),
                         QJsonArray{userEntry(QStringLiteral("Cannot consume the receipt slot."))},
                         &project.error).isEmpty());

    const QJsonObject branchAccepted = effectOutcome(pending, QStringLiteral("accepted"));
    const QJsonObject resolvedBranch = store.append(branchId, QStringLiteral("1"),
                                                    QJsonArray{branchAccepted}, &project.error);
    QVERIFY2(!resolvedBranch.isEmpty(), qPrintable(project.error));
    QCOMPARE(resolvedBranch.value(QStringLiteral("messageCount")).toInt(), 512);

    const QJsonObject sourceAppend = assistantEntry(QStringLiteral("Source is still unresolved."));
    QVERIFY(store.append(sourceId, afterPending.value(QStringLiteral("revision")).toString(),
                         QJsonArray{sourceAppend}, &project.error).isEmpty());
}

void TstAgentConversationStore::unverifiedDeletionContentsArePreserved()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY2(project.configure(store), qPrintable(project.error));
    const QJsonObject created = store.create(QStringLiteral("Preserve unknown file"), &project.error);
    QVERIFY2(!created.isEmpty(), qPrintable(project.error));
    const QString id = created.value(QStringLiteral("conversationId")).toString();
    const QString unknownPath = QDir(project.conversationPath(id)).filePath(QStringLiteral("notes.txt"));
    QFile unknown(unknownPath);
    QVERIFY(unknown.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(unknown.write("keep"), qint64(4));
    unknown.close();

    QString error;
    QVERIFY(!store.remove(id, QStringLiteral("1"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(unknownPath));
    QVERIFY(QFileInfo::exists(project.conversationPath(id)));
}

void TstAgentConversationStore::workspaceProjectBindingIsChecked()
{
    ProjectFixture project;
    QVERIFY2(project.open(), qPrintable(project.error));
    RAgentConversationStore store;
    QVERIFY(!store.configure(project.catalog->workspace(), uuid(), &project.error));
    QVERIFY(!project.error.isEmpty());
    QVERIFY(store.create(QStringLiteral("Unbound"), &project.error).isEmpty());
    QVERIFY(!project.error.isEmpty());
}

QTEST_GUILESS_MAIN(TstAgentConversationStore)
#include "tst_agent_conversation_store.moc"
