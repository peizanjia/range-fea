#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

#include "../src/native_result_journal.h"

namespace
{
class EnvironmentGuard final
{
public:
    EnvironmentGuard()
    {
        for (const char *name : {NativeResultJournal::jobIdEnvironmentVariable,
                                 NativeResultJournal::pathEnvironmentVariable,
                                 NativeResultJournal::resultDirectoryEnvironmentVariable})
        {
            const QByteArray key(name);
            const bool wasSet = qEnvironmentVariableIsSet(name);
            this->values.append({key,wasSet,qgetenv(name)});
        }
    }

    ~EnvironmentGuard()
    {
        for (const auto &value : this->values)
        {
            if (value.wasSet) qputenv(value.key.constData(),value.value);
            else qunsetenv(value.key.constData());
        }
    }

private:
    struct Value { QByteArray key; bool wasSet; QByteArray value; };
    QList<Value> values;
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

RSolverResultFrame makeFrame(const QString &recordPath, const QString &logicalPath)
{
    RSolverResultFrame frame;
    frame.actualRecordPath = QFileInfo(recordPath).absoluteFilePath();
    frame.logicalPath = QFileInfo(logicalPath).absoluteFilePath();
    frame.recordNo = 3;
    frame.checkpointTime = 0.375;
    frame.nativeAnalysis = QStringLiteral("transient");
    return frame;
}
}

class TestNativeResultJournal : public QObject
{
    Q_OBJECT

private slots:
    void appendsFrameOnlyAfterHashingItsPersistedRecord();
    void refusesPathsOutsideTheTrustedResultDirectory();
    void rejectsNonFiniteFrameTimeWithoutWriting();
    void refusesJournalTargetReuseAndPartialEnvironment();
    void noEnvironmentIsANoop();
    void enforcesTheSixteenMiBBound();
};

void TestNativeResultJournal::appendsFrameOnlyAfterHashingItsPersistedRecord()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString resultDirectory = temporary.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    const QString recordPath = QDir(resultDirectory).filePath(QStringLiteral("history.0003.rbm"));
    const QString logicalPath = QDir(resultDirectory).filePath(QStringLiteral("history.rbm"));
    const QByteArray recordBytes("persisted accepted result frame\n");
    QVERIFY(writeFile(recordPath,recordBytes));
    QVERIFY(writeFile(logicalPath,QByteArrayLiteral("native link record\n")));

    const QString journalPath = temporary.filePath(QStringLiteral("journal.jsonl"));
    QString error;
    std::unique_ptr<NativeResultJournal> journal = NativeResultJournal::create(
        QStringLiteral("native-job-7"),journalPath,resultDirectory,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    QVERIFY(journal->isAvailable());
    QVERIFY(journal->appendFrame(makeFrame(recordPath,logicalPath)));

    const QList<QByteArray> lines = readFile(journalPath).split('\n');
    QVERIFY(lines.size() >= 2);
    const QJsonDocument document = QJsonDocument::fromJson(lines.first());
    QVERIFY(document.isObject());
    const QJsonObject record = document.object();
    QCOMPARE(record.value(QStringLiteral("protocolVersion")).toString(),
             QStringLiteral("range.native-result-frame/v1"));
    QCOMPARE(record.value(QStringLiteral("jobId")).toString(),QStringLiteral("native-job-7"));
    QCOMPARE(record.value(QStringLiteral("seq")).toDouble(),1.0);
    QCOMPARE(record.value(QStringLiteral("recordNo")).toDouble(),3.0);
    QCOMPARE(record.value(QStringLiteral("timeS")).toDouble(),0.375);
    QCOMPARE(record.value(QStringLiteral("relativePath")).toString(),
             QStringLiteral("history.0003.rbm"));
    QCOMPARE(record.value(QStringLiteral("sha256")).toString(),
             QString::fromLatin1(QCryptographicHash::hash(recordBytes,QCryptographicHash::Sha256).toHex()));
}

void TestNativeResultJournal::refusesPathsOutsideTheTrustedResultDirectory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString resultDirectory = temporary.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    const QString logicalPath = QDir(resultDirectory).filePath(QStringLiteral("history.rbm"));
    const QString outsidePath = temporary.filePath(QStringLiteral("outside.rbm"));
    QVERIFY(writeFile(logicalPath,QByteArrayLiteral("link")));
    QVERIFY(writeFile(outsidePath,QByteArrayLiteral("frame")));

    QString error;
    std::unique_ptr<NativeResultJournal> journal = NativeResultJournal::create(
        QStringLiteral("job"),temporary.filePath(QStringLiteral("journal.jsonl")),
        resultDirectory,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    QVERIFY(!journal->appendFrame(makeFrame(outsidePath,logicalPath)));
    QVERIFY(!journal->isAvailable());
    QVERIFY(journal->unavailableReason().contains(QStringLiteral("outside"),Qt::CaseInsensitive));
    QVERIFY(readFile(temporary.filePath(QStringLiteral("journal.jsonl"))).isEmpty());
}

void TestNativeResultJournal::rejectsNonFiniteFrameTimeWithoutWriting()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString resultDirectory = temporary.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    const QString recordPath = QDir(resultDirectory).filePath(QStringLiteral("record.rbm"));
    const QString logicalPath = QDir(resultDirectory).filePath(QStringLiteral("history.rbm"));
    QVERIFY(writeFile(recordPath,QByteArrayLiteral("frame")));
    QVERIFY(writeFile(logicalPath,QByteArrayLiteral("link")));

    QString error;
    const QString journalPath = temporary.filePath(QStringLiteral("journal.jsonl"));
    std::unique_ptr<NativeResultJournal> journal = NativeResultJournal::create(
        QStringLiteral("job"),journalPath,resultDirectory,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    RSolverResultFrame frame = makeFrame(recordPath,logicalPath);
    frame.checkpointTime = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!journal->appendFrame(frame));
    QVERIFY(!journal->isAvailable());
    QVERIFY(journal->unavailableReason().contains(QStringLiteral("non-finite"),Qt::CaseInsensitive));
    QVERIFY(readFile(journalPath).isEmpty());
}

void TestNativeResultJournal::refusesJournalTargetReuseAndPartialEnvironment()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString resultDirectory = temporary.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    const QString journalPath = temporary.filePath(QStringLiteral("journal.jsonl"));
    QVERIFY(writeFile(journalPath,QByteArrayLiteral("preserve")));
    QString error;
    QVERIFY(!NativeResultJournal::create(QStringLiteral("job"),journalPath,
                                         resultDirectory,&error));
    QCOMPARE(readFile(journalPath),QByteArrayLiteral("preserve"));

    EnvironmentGuard guard;
    qputenv(NativeResultJournal::jobIdEnvironmentVariable,"job");
    qunsetenv(NativeResultJournal::pathEnvironmentVariable);
    qunsetenv(NativeResultJournal::resultDirectoryEnvironmentVariable);
    error.clear();
    QVERIFY(!NativeResultJournal::createFromEnvironment(&error));
    QVERIFY(error.contains(QStringLiteral("required together"),Qt::CaseInsensitive));
}

void TestNativeResultJournal::noEnvironmentIsANoop()
{
    EnvironmentGuard guard;
    qunsetenv(NativeResultJournal::jobIdEnvironmentVariable);
    qunsetenv(NativeResultJournal::pathEnvironmentVariable);
    qunsetenv(NativeResultJournal::resultDirectoryEnvironmentVariable);
    QString error;
    QVERIFY(!NativeResultJournal::createFromEnvironment(&error));
    QVERIFY(error.isEmpty());
}

void TestNativeResultJournal::enforcesTheSixteenMiBBound()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString resultDirectory = temporary.filePath(QStringLiteral("results"));
    QVERIFY(QDir().mkpath(resultDirectory));
    const QString recordPath = QDir(resultDirectory).filePath(QStringLiteral("r.rbm"));
    const QString logicalPath = QDir(resultDirectory).filePath(QStringLiteral("l.rbm"));
    QVERIFY(writeFile(recordPath,QByteArrayLiteral("r")));
    QVERIFY(writeFile(logicalPath,QByteArrayLiteral("l")));

    QString error;
    const QString journalPath = temporary.filePath(QStringLiteral("bounded.jsonl"));
    std::unique_ptr<NativeResultJournal> journal = NativeResultJournal::create(
        QString(256,QLatin1Char('j')),journalPath,resultDirectory,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    const RSolverResultFrame frame = makeFrame(recordPath,logicalPath);
    quint64 acceptedRecords = 0;
    while (journal->appendFrame(frame))
    {
        ++acceptedRecords;
    }
    QVERIFY(acceptedRecords > 0);
    QVERIFY(!journal->isAvailable());
    QVERIFY(journal->unavailableReason().contains(QStringLiteral("16 MiB"),Qt::CaseInsensitive));
    QVERIFY(QFileInfo(journalPath).size() <= 16*1024*1024);
}

QTEST_APPLESS_MAIN(TestNativeResultJournal)

#include "tst_native_result_journal.moc"
