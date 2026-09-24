#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

#include "execution_event_journal.h"

namespace
{
QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return file.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes, QIODevice::OpenMode mode)
{
    QFile file(path);
    if (!file.open(mode))
    {
        return false;
    }
    return file.write(bytes) == bytes.size() && file.flush();
}
}

class TestExecutionEventJournal : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsCompleteUtf8LinesAndResumesSequence();
    void refusesForeignFileWithoutChangingIt();
    void reportsTruncatedTailAndRefusesToAlterIt();
    void refusesAnotherRunIdWithoutChangingIt();
    void excludesSecondWriterUntilFirstIsDestroyed();
    void stableEventIdRetriesAreIdempotentAndConflictsFail();
    void missingParentDoesNotCreateAFile();
};

void TestExecutionEventJournal::roundTripsCompleteUtf8LinesAndResumesSequence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("run.events.jsonl"));
    const QUuid runId = ExecutionEventJournal::createRunId();
    QVERIFY(!runId.isNull());

    ExecutionEvent first;
    QString error;
    {
        ExecutionEventJournal writer(path,runId);
        QVERIFY2(writer.append(QStringLiteral("SolverRunStarted"),QStringLiteral("started"),
                               {{QStringLiteral("label"),QStringLiteral("鋼索\n振動")}},
                               &error,&first),qPrintable(error));
        QCOMPARE(first.schemaVersion,1);
        QCOMPARE(first.sequence,quint64(1));
        QCOMPARE(first.runId,runId);
        QVERIFY(!first.eventId.isNull());
        QVERIFY(first.utc.endsWith(QLatin1Char('Z')));
    }
    ExecutionEvent second;
    {
        ExecutionEventJournal resumed(path,runId);
        QVERIFY2(resumed.append(QStringLiteral("RunComputed"),QStringLiteral("ok"),
                                {{QStringLiteral("energy"),0.25}},
                                &error,&second),qPrintable(error));
    }
    QCOMPARE(second.sequence,quint64(2));
    QVERIFY(second.eventId != first.eventId);

    const QByteArray bytes = readBytes(path);
    QVERIFY(!bytes.isEmpty());
    QVERIFY(bytes.endsWith('\n'));
    QCOMPARE(int(std::count(bytes.cbegin(),bytes.cend(),'\n')),2);
    QVERIFY(bytes.contains(QStringLiteral("鋼索").toUtf8()));

    const ExecutionEventReadResult read = ExecutionEventJournal::read(path);
    QVERIFY2(read.error.isEmpty(),qPrintable(read.error));
    QVERIFY(!read.truncatedTail);
    QCOMPARE(read.events.size(),2);
    QCOMPARE(read.events[0].eventId,first.eventId);
    QCOMPARE(read.events[0].payload.value(QStringLiteral("label")).toString(),
             QStringLiteral("鋼索\n振動"));
    QCOMPARE(read.events[1].eventId,second.eventId);
    QCOMPARE(read.events[1].payload.value(QStringLiteral("energy")).toDouble(),0.25);
}

void TestExecutionEventJournal::refusesForeignFileWithoutChangingIt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("important.txt"));
    const QByteArray original("user data; this is not a JSONL journal\n");
    QVERIFY(writeBytes(path,original,QIODevice::WriteOnly));

    ExecutionEventJournal writer(path,ExecutionEventJournal::createRunId());
    QString error;
    QVERIFY(!writer.append(QStringLiteral("RunStarted"),QStringLiteral("started"),{},&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(path),original);
}

void TestExecutionEventJournal::reportsTruncatedTailAndRefusesToAlterIt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("partial.events.jsonl"));
    const QUuid runId = ExecutionEventJournal::createRunId();
    QString error;
    {
        ExecutionEventJournal writer(path,runId);
        QVERIFY2(writer.append(QStringLiteral("RunStarted"),QStringLiteral("started"),{},&error),
                 qPrintable(error));
    }
    QVERIFY(writeBytes(path,QByteArray("{\"schemaVersion\":1,\"eventId\":"),
                       QIODevice::WriteOnly | QIODevice::Append));
    const QByteArray original = readBytes(path);
    const ExecutionEventReadResult read = ExecutionEventJournal::read(path);
    QVERIFY(read.error.isEmpty());
    QVERIFY(read.truncatedTail);
    QCOMPARE(read.events.size(),1);

    ExecutionEventJournal resumed(path,runId);
    QVERIFY(!resumed.append(QStringLiteral("RunComputed"),QStringLiteral("ok"),{},&error));
    QVERIFY(error.contains(QStringLiteral("truncated"),Qt::CaseInsensitive));
    QCOMPARE(readBytes(path),original);
}

void TestExecutionEventJournal::refusesAnotherRunIdWithoutChangingIt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("run.events.jsonl"));
    const QUuid firstRunId = ExecutionEventJournal::createRunId();
    QString error;
    {
        ExecutionEventJournal writer(path,firstRunId);
        QVERIFY2(writer.append(QStringLiteral("RunStarted"),QStringLiteral("started"),{},&error),
                 qPrintable(error));
    }
    const QByteArray original = readBytes(path);
    ExecutionEventJournal wrongRun(path,ExecutionEventJournal::createRunId());
    QVERIFY(!wrongRun.append(QStringLiteral("RunComputed"),QStringLiteral("ok"),{},&error));
    QVERIFY(error.contains(QStringLiteral("another run ID"),Qt::CaseInsensitive));
    QCOMPARE(readBytes(path),original);
}

void TestExecutionEventJournal::excludesSecondWriterUntilFirstIsDestroyed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("locked.events.jsonl"));
    const QUuid runId = ExecutionEventJournal::createRunId();
    QString error;
    ExecutionEventJournal second(path,runId);
    {
        ExecutionEventJournal first(path,runId);
        QVERIFY2(first.append(QStringLiteral("RunStarted"),QStringLiteral("started"),{},&error),
                 qPrintable(error));
        const QByteArray original = readBytes(path);
        QVERIFY(!second.append(QStringLiteral("RunComputed"),QStringLiteral("ok"),{},&error));
        QVERIFY(error.contains(QStringLiteral("locked"),Qt::CaseInsensitive));
        QCOMPARE(readBytes(path),original);
    }
    QVERIFY2(second.append(QStringLiteral("RunComputed"),QStringLiteral("ok"),{},&error),
             qPrintable(error));
    const ExecutionEventReadResult read = ExecutionEventJournal::read(path);
    QVERIFY2(read.error.isEmpty(),qPrintable(read.error));
    QCOMPARE(read.events.size(),2);
    QCOMPARE(read.events[0].sequence,quint64(1));
    QCOMPARE(read.events[1].sequence,quint64(2));
}

void TestExecutionEventJournal::stableEventIdRetriesAreIdempotentAndConflictsFail()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("idempotent.events.jsonl"));
    const QUuid runId = ExecutionEventJournal::createRunId();
    const QUuid eventId = QUuid::createUuid();
    const QString type = QStringLiteral("RunComputed");
    const QString status = QStringLiteral("ok");
    const QJsonObject payload{{QStringLiteral("energy"),0.25}};
    QString error;
    ExecutionEvent original;
    QByteArray originalBytes;
    {
        ExecutionEventJournal writer(path,runId);
        QVERIFY2(writer.append(type,status,payload,eventId,&error,&original),qPrintable(error));
        QCOMPARE(original.eventId,eventId);
        originalBytes = readBytes(path);
        ExecutionEvent replay;
        QVERIFY2(writer.append(type,status,payload,eventId,&error,&replay),qPrintable(error));
        QCOMPARE(replay.eventId,original.eventId);
        QCOMPARE(replay.sequence,original.sequence);
        QCOMPARE(replay.utc,original.utc);
        QCOMPARE(readBytes(path),originalBytes);
        QVERIFY(!writer.append(type,status,{{QStringLiteral("energy"),0.5}},eventId,&error));
        QVERIFY(error.contains(QStringLiteral("different content"),Qt::CaseInsensitive));
        QCOMPARE(readBytes(path),originalBytes);
    }
    {
        ExecutionEventJournal resumed(path,runId);
        ExecutionEvent replay;
        QVERIFY2(resumed.append(type,status,payload,eventId,&error,&replay),qPrintable(error));
        QCOMPARE(replay.sequence,original.sequence);
        QCOMPARE(replay.utc,original.utc);
        QCOMPARE(readBytes(path),originalBytes);
        QVERIFY(!resumed.append(QStringLiteral("OtherEvent"),status,payload,eventId,&error));
        QVERIFY(error.contains(QStringLiteral("different content"),Qt::CaseInsensitive));
        QCOMPARE(readBytes(path),originalBytes);
    }
    const ExecutionEventReadResult read = ExecutionEventJournal::read(path);
    QVERIFY2(read.error.isEmpty(),qPrintable(read.error));
    QCOMPARE(read.events.size(),1);
}

void TestExecutionEventJournal::missingParentDoesNotCreateAFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("missing/run.events.jsonl"));
    ExecutionEventJournal writer(path,ExecutionEventJournal::createRunId());
    QString error;
    QVERIFY(!writer.append(QStringLiteral("RunStarted"),QStringLiteral("started"),{},&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(path));
}

QTEST_APPLESS_MAIN(TestExecutionEventJournal)
#include "tst_execution_event_journal.moc"
