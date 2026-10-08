#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include <rbl_progress.h>

#include "../src/native_progress_journal.h"

namespace
{
struct ProgressStateGuard
{
    RProgress &progress = RProgress::getInstance();
    RProgressPrintHandler printHandler = progress.getProgressPrintHandler();
    RProgressInitializeHandler initializeHandler = progress.getProgressInitializeHandler();
    RProgressFinalizeHandler finalizeHandler = progress.getProgressFinalizeHandler();
    bool printToLog = progress.getPrintToLog();

    ~ProgressStateGuard()
    {
        progress.setProgressPrintHandler(printHandler);
        progress.setProgressInitializeHandler(initializeHandler);
        progress.setProgressFinalizeHandler(finalizeHandler);
        progress.setPrintToLog(printToLog);
    }
};

QVector<QJsonObject> readRecords(const QString &path)
{
    QVector<QJsonObject> records;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return records;
    }
    while (!file.atEnd())
    {
        const QByteArray line = file.readLine();
        if (!line.endsWith('\n'))
        {
            return {};
        }
        const QJsonDocument document = QJsonDocument::fromJson(line.trimmed());
        if (!document.isObject())
        {
            return {};
        }
        records.push_back(document.object());
    }
    return records;
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return file.readAll();
}
}

class TestNativeProgressJournal : public QObject
{
    Q_OBJECT

private slots:
    void callbackRecordsAreVersionedOrderedAndFinite();
    void concurrentCallbacksProduceCompleteOrderedLines();
    void pulseRecordsNeverInventFractions();
    void invalidCallbackDisablesTheSidechannel();
    void missingEnvironmentIsANoop();
    void environmentHandshakeCreatesTheRequestedFile();
    void refusesExistingAndPartialTargetsWithoutChangingThem();
};

void TestNativeProgressJournal::callbackRecordsAreVersionedOrderedAndFinite()
{
    ProgressStateGuard state;
    RProgress &progress = RProgress::getInstance();
    progress.setPrintToLog(false);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("progress.jsonl"));
    QString error;
    std::unique_ptr<NativeProgressJournal> journal = NativeProgressJournal::create(
        QStringLiteral("job-42"),path,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    QVERIFY(journal->isAvailable());

    {
        NativeProgressHandlerScope handlers(journal.get());
        RProgressInitialize(QStringLiteral("Assembling stiffness"));
        RProgressPrint(0.001); // The sub-percent update is sampled out.
        RProgressPrint(0.011);
        RProgressPrint(0.019); // Same 1% bucket is sampled out.
        RProgressPrint(0.021);
        RProgressFinalize(QStringLiteral("Done"));

        RProgressInitialize(QStringLiteral("Solving linear system"));
        RProgressPrint(0.5);
        RProgressFinalize(QStringLiteral("Done"));
    }

    QCOMPARE(progress.getProgressPrintHandler(),state.printHandler);
    QCOMPARE(progress.getProgressInitializeHandler(),state.initializeHandler);
    QCOMPARE(progress.getProgressFinalizeHandler(),state.finalizeHandler);

    const QVector<QJsonObject> records = readRecords(path);
    QCOMPARE(records.size(),9);
    const QStringList keys{
        QStringLiteral("fraction"),QStringLiteral("jobId"),QStringLiteral("kind"),
        QStringLiteral("message"),QStringLiteral("phaseId"),
        QStringLiteral("protocolVersion"),QStringLiteral("pulse"),QStringLiteral("seq")};
    for (qsizetype i=0;i<records.size();++i)
    {
        const QJsonObject &record = records.at(i);
        QCOMPARE(record.keys(),keys);
        QCOMPARE(record.value(QStringLiteral("protocolVersion")).toString(),
                 QStringLiteral("range.native-progress/v1"));
        QCOMPARE(record.value(QStringLiteral("jobId")).toString(),QStringLiteral("job-42"));
        QCOMPARE(record.value(QStringLiteral("seq")).toDouble(),double(i+1));
        QVERIFY(record.value(QStringLiteral("phaseId")).isDouble());
        QVERIFY(record.value(QStringLiteral("pulse")).isBool());
        if (record.value(QStringLiteral("pulse")).toBool())
        {
            QVERIFY(record.value(QStringLiteral("fraction")).isNull());
        }
        else
        {
            const double fraction = record.value(QStringLiteral("fraction")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            QVERIFY(std::isfinite(fraction));
            QVERIFY(fraction >= 0.0 && fraction <= 1.0);
        }
    }
    QCOMPARE(records.at(0).value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_started"));
    QCOMPARE(records.at(0).value(QStringLiteral("phaseId")).toInt(),1);
    QCOMPARE(records.at(0).value(QStringLiteral("fraction")).toDouble(),0.0);
    QCOMPARE(records.at(1).value(QStringLiteral("kind")).toString(),
             QStringLiteral("fraction"));
    QCOMPARE(records.at(1).value(QStringLiteral("fraction")).toDouble(),0.011);
    QCOMPARE(records.at(2).value(QStringLiteral("fraction")).toDouble(),0.021);
    QCOMPARE(records.at(4).value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_finished"));
    QCOMPARE(records.at(4).value(QStringLiteral("fraction")).toDouble(),1.0);
    QCOMPARE(records.at(5).value(QStringLiteral("phaseId")).toInt(),2);
    QCOMPARE(records.at(5).value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_started"));
    QCOMPARE(records.at(8).value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_finished"));
}

void TestNativeProgressJournal::pulseRecordsNeverInventFractions()
{
    ProgressStateGuard state;
    RProgress &progress = RProgress::getInstance();
    progress.setPrintToLog(false);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("pulse.jsonl"));
    QString error;
    std::unique_ptr<NativeProgressJournal> journal = NativeProgressJournal::create(
        QStringLiteral("pulse-job"),path,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    {
        NativeProgressHandlerScope handlers(journal.get());
        RProgressInitialize(QStringLiteral("Building preconditioner"),true);
        for (int i=0;i<128;++i)
        {
            RProgressPrint(0.75);
        }
        RProgressFinalize(QStringLiteral("Done"));
    }

    const QVector<QJsonObject> records = readRecords(path);
    QCOMPARE(records.size(),5);
    for (const QJsonObject &record : records)
    {
        QVERIFY(record.value(QStringLiteral("pulse")).toBool());
        QVERIFY(record.value(QStringLiteral("fraction")).isNull());
    }
    QCOMPARE(records.first().value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_started"));
    QCOMPARE(records.last().value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_finished"));
}

void TestNativeProgressJournal::concurrentCallbacksProduceCompleteOrderedLines()
{
    ProgressStateGuard state;
    RProgress &progress = RProgress::getInstance();
    progress.setPrintToLog(false);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("concurrent.jsonl"));
    QString error;
    std::unique_ptr<NativeProgressJournal> journal = NativeProgressJournal::create(
        QStringLiteral("parallel-job"),path,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    {
        NativeProgressHandlerScope handlers(journal.get());
        RProgressInitialize(QStringLiteral("Parallel assembly"));
        std::vector<std::thread> callbacks;
        for (int threadIndex=0;threadIndex<4;++threadIndex)
        {
            callbacks.emplace_back([threadIndex]()
            {
                for (int i=1;i<=100;++i)
                {
                    const int shiftedBucket = ((i + threadIndex * 23 - 1) % 100) + 1;
                    RProgressPrint(double(shiftedBucket) / 100.0);
                }
            });
        }
        for (std::thread &callback : callbacks)
        {
            callback.join();
        }
        RProgressFinalize(QStringLiteral("Done"));
    }

    const QVector<QJsonObject> records = readRecords(path);
    QVERIFY(records.size() > 2);
    QCOMPARE(records.first().value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_started"));
    QCOMPARE(records.last().value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_finished"));
    for (qsizetype i=0;i<records.size();++i)
    {
        QCOMPARE(records.at(i).value(QStringLiteral("seq")).toDouble(),double(i+1));
    }
}

void TestNativeProgressJournal::invalidCallbackDisablesTheSidechannel()
{
    ProgressStateGuard state;
    RProgress &progress = RProgress::getInstance();
    progress.setPrintToLog(false);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("invalid.jsonl"));
    QString error;
    std::unique_ptr<NativeProgressJournal> journal = NativeProgressJournal::create(
        QStringLiteral("invalid-job"),path,&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    {
        NativeProgressHandlerScope handlers(journal.get());
        RProgressInitialize(QStringLiteral("Assembly"));
        RProgressPrint(std::numeric_limits<double>::quiet_NaN());
        QVERIFY(!journal->isAvailable());
        QVERIFY(journal->unavailableReason().contains(QStringLiteral("non-finite")));
        RProgressFinalize(QStringLiteral("Done"));
    }

    const QVector<QJsonObject> records = readRecords(path);
    QCOMPARE(records.size(),1);
    QCOMPARE(records.first().value(QStringLiteral("kind")).toString(),
             QStringLiteral("phase_started"));
}

void TestNativeProgressJournal::missingEnvironmentIsANoop()
{
    const QByteArray previousJobId = qgetenv(NativeProgressJournal::jobIdEnvironmentVariable);
    const QByteArray previousPath = qgetenv(NativeProgressJournal::pathEnvironmentVariable);
    qunsetenv(NativeProgressJournal::jobIdEnvironmentVariable);
    qunsetenv(NativeProgressJournal::pathEnvironmentVariable);
    const auto restoreEnvironment = qScopeGuard([&]()
    {
        if (previousJobId.isNull())
        {
            qunsetenv(NativeProgressJournal::jobIdEnvironmentVariable);
        }
        else
        {
            qputenv(NativeProgressJournal::jobIdEnvironmentVariable,previousJobId);
        }
        if (previousPath.isNull())
        {
            qunsetenv(NativeProgressJournal::pathEnvironmentVariable);
        }
        else
        {
            qputenv(NativeProgressJournal::pathEnvironmentVariable,previousPath);
        }
    });
    Q_UNUSED(restoreEnvironment);

    QString error;
    std::unique_ptr<NativeProgressJournal> journal =
        NativeProgressJournal::createFromEnvironment(&error);
    QVERIFY(!journal);
    QVERIFY(error.isEmpty());

    qputenv(NativeProgressJournal::jobIdEnvironmentVariable,QByteArray("partial-job"));
    journal = NativeProgressJournal::createFromEnvironment(&error);
    QVERIFY(!journal);
    QVERIFY(error.contains(QStringLiteral("Both RANGE_NATIVE_JOB_ID")));
    qunsetenv(NativeProgressJournal::jobIdEnvironmentVariable);

    ProgressStateGuard state;
    NativeProgressHandlerScope handlers(journal.get());
    RProgress &progress = RProgress::getInstance();
    QCOMPARE(progress.getProgressPrintHandler(),state.printHandler);
    QCOMPARE(progress.getProgressInitializeHandler(),state.initializeHandler);
    QCOMPARE(progress.getProgressFinalizeHandler(),state.finalizeHandler);
}

void TestNativeProgressJournal::environmentHandshakeCreatesTheRequestedFile()
{
    const QByteArray previousJobId = qgetenv(NativeProgressJournal::jobIdEnvironmentVariable);
    const QByteArray previousPath = qgetenv(NativeProgressJournal::pathEnvironmentVariable);
    const auto restoreEnvironment = qScopeGuard([&]()
    {
        if (previousJobId.isNull())
        {
            qunsetenv(NativeProgressJournal::jobIdEnvironmentVariable);
        }
        else
        {
            qputenv(NativeProgressJournal::jobIdEnvironmentVariable,previousJobId);
        }
        if (previousPath.isNull())
        {
            qunsetenv(NativeProgressJournal::pathEnvironmentVariable);
        }
        else
        {
            qputenv(NativeProgressJournal::pathEnvironmentVariable,previousPath);
        }
    });
    Q_UNUSED(restoreEnvironment);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("requested.jsonl"));
    qputenv(NativeProgressJournal::jobIdEnvironmentVariable,QByteArray("env-job"));
    qputenv(NativeProgressJournal::pathEnvironmentVariable,path.toUtf8());

    QString error;
    std::unique_ptr<NativeProgressJournal> journal =
        NativeProgressJournal::createFromEnvironment(&error);
    QVERIFY2(journal != nullptr,qPrintable(error));
    QVERIFY(journal->isAvailable());
    QVERIFY(QFileInfo::exists(path));
}

void TestNativeProgressJournal::refusesExistingAndPartialTargetsWithoutChangingThem()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.jsonl"));
    const QByteArray partial("{\"protocolVersion\":\"range.native-progress/v1\"");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(file.write(partial),qint64(partial.size()));
        QVERIFY(file.flush());
    }

    QString error;
    std::unique_ptr<NativeProgressJournal> journal = NativeProgressJournal::create(
        QStringLiteral("job-42"),path,&error);
    QVERIFY(!journal);
    QVERIFY(error.contains(QStringLiteral("already exists")));
    QCOMPARE(readBytes(path),partial);

    const QString missingParentPath = directory.filePath(
        QStringLiteral("missing/progress.jsonl"));
    journal = NativeProgressJournal::create(QStringLiteral("job-42"),missingParentPath,&error);
    QVERIFY(!journal);
    QVERIFY(error.contains(QStringLiteral("parent")));
    QVERIFY(!QFileInfo::exists(missingParentPath));
}

QTEST_GUILESS_MAIN(TestNativeProgressJournal)
#include "tst_native_progress_journal.moc"
