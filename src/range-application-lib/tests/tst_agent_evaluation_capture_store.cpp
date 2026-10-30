#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QTemporaryDir>

#include <ragent_evaluation_capture_store.h>

namespace
{
QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size() && file.flush();
}

QJsonObject physicsEvent(int step)
{
    return {{QStringLiteral("eventType"), QStringLiteral("solver.step.accepted")},
            {QStringLiteral("step"), step},
            {QStringLiteral("timeSeconds"), step * 0.01},
            {QStringLiteral("tipDisplacementMm"), step * 0.0004},
            {QStringLiteral("contentSha256"), QString(64, QLatin1Char('a'))},
            {QStringLiteral("argumentsSha256"), QString(64, QLatin1Char('b'))}};
}
}

class TstAgentEvaluationCaptureStore : public QObject
{
    Q_OBJECT

private slots:
    void persistentChainedPrefix();
    void refusesExistingFileWithoutChangingIt();
    void refusesReparsePointInCapturePath();
    void externalMutationFailsClosedAndIsPreserved();
    void concurrentWriterBytesAreNeverReplaced();
    void invalidEventsDoNotMutateThePrefix();
};

void TstAgentEvaluationCaptureStore::persistentChainedPrefix()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("nested/evaluation.jsonl"));

    RAgentEvaluationCaptureStore store;
    QString error;
    QVERIFY2(store.configure(path, &error), qPrintable(error));
    QCOMPARE(readAll(path), QByteArray());

    QVERIFY2(store.append(physicsEvent(1), &error), qPrintable(error));
    const QByteArray firstPrefix = readAll(path);
    QVERIFY(!firstPrefix.isEmpty());
    QVERIFY(firstPrefix.endsWith('\n'));
    QVERIFY2(store.append(physicsEvent(2), &error), qPrintable(error));
    const QByteArray fullLedger = readAll(path);
    QVERIFY(fullLedger.startsWith(firstPrefix));

    const QList<QByteArray> lines = fullLedger.split('\n');
    QCOMPARE(lines.size(), 3);
    const QString genesis(64, QLatin1Char('0'));
    QString previousHash = genesis;
    for (int index = 0; index < 2; ++index)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(lines.at(index), &parseError);
        QVERIFY(parseError.error == QJsonParseError::NoError);
        QVERIFY(document.isObject());
        QJsonObject record = document.object();
        QCOMPARE(record.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.agent-evaluation-capture/v1"));
        QCOMPARE(record.value(QStringLiteral("sequence")).toInt(), index + 1);
        QCOMPARE(record.value(QStringLiteral("previousHash")).toString(), previousHash);
        QVERIFY(record.value(QStringLiteral("payload")).isObject());
        const QString entryHash = record.take(QStringLiteral("entryHash")).toString();
        QCOMPARE(entryHash.size(), 64);
        const QByteArray canonical = QJsonDocument(record).toJson(QJsonDocument::Compact);
        const QByteArray expectedHash = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
        QCOMPARE(entryHash.toLatin1(), expectedHash);
        previousHash = entryHash;
    }
    QVERIFY(lines.last().isEmpty());
}

void TstAgentEvaluationCaptureStore::refusesExistingFileWithoutChangingIt()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("existing.jsonl"));
    const QByteArray original("caller-owned bytes\n");
    QVERIFY(writeAll(path, original));

    RAgentEvaluationCaptureStore store;
    QString error;
    QVERIFY(!store.configure(path, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readAll(path), original);
    QVERIFY(!store.append(physicsEvent(1), &error));
    QCOMPARE(readAll(path), original);
}

void TstAgentEvaluationCaptureStore::refusesReparsePointInCapturePath()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString target = temporary.filePath(QStringLiteral("outside"));
    const QString linkedParent = temporary.filePath(QStringLiteral("linked"));
    QVERIFY(QDir().mkpath(target));

#ifdef Q_OS_WIN
    const QString command = QStringLiteral("mklink /J \"%1\" \"%2\"")
                                .arg(QDir::toNativeSeparators(linkedParent),
                                     QDir::toNativeSeparators(target));
    QProcess junction;
    junction.setProgram(QStringLiteral("cmd.exe"));
    junction.setNativeArguments(QStringLiteral("/d /c ") + command);
    junction.start();
    QVERIFY2(junction.waitForFinished(10000), qPrintable(junction.errorString()));
    const QByteArray junctionOutput = junction.readAllStandardOutput()
                                      + junction.readAllStandardError();
    QVERIFY2(junction.exitStatus() == QProcess::NormalExit && junction.exitCode() == 0,
             junctionOutput.constData());
#else
    if (!QFile::link(target, linkedParent)) QSKIP("filesystem cannot create a directory symbolic link");
#endif
#ifdef Q_OS_WIN
    QVERIFY(QFileInfo(linkedParent).isJunction());
#else
    QVERIFY(QFileInfo(linkedParent).isSymLink());
#endif

    RAgentEvaluationCaptureStore store;
    QString error;
    QVERIFY(!store.configure(linkedParent + QStringLiteral("/nested/evaluation.jsonl"), &error));
    QVERIFY(error.contains(QStringLiteral("symbolic link or directory junction")));
    QVERIFY(!QFileInfo(QDir(linkedParent).filePath(QStringLiteral("nested"))).exists());
#ifdef Q_OS_WIN
    QVERIFY(QDir().rmdir(linkedParent));
    QVERIFY(QFileInfo(target).isDir());
#endif
}

void TstAgentEvaluationCaptureStore::externalMutationFailsClosedAndIsPreserved()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("evaluation.jsonl"));

    RAgentEvaluationCaptureStore store;
    QString error;
    QVERIFY2(store.configure(path, &error), qPrintable(error));
    QVERIFY2(store.append(physicsEvent(1), &error), qPrintable(error));
    const QByteArray originalPrefix = readAll(path);
    const QByteArray externallyChanged = originalPrefix + QByteArrayLiteral("{\"external\":true}\n");
    QVERIFY(writeAll(path, externallyChanged));

    QVERIFY(!store.append(physicsEvent(2), &error));
    QVERIFY(error.contains(QStringLiteral("changed")));
    QCOMPARE(readAll(path), externallyChanged);
    QVERIFY(!store.append(physicsEvent(3), &error));
    QCOMPARE(readAll(path), externallyChanged);
}

void TstAgentEvaluationCaptureStore::concurrentWriterBytesAreNeverReplaced()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("evaluation.jsonl"));

    RAgentEvaluationCaptureStore store;
    QString error;
    QVERIFY2(store.configure(path, &error), qPrintable(error));
    QVERIFY2(store.append(physicsEvent(1), &error), qPrintable(error));
    const QByteArray originalPrefix = readAll(path);

    QFile uncooperativeWriter(path);
    QVERIFY(uncooperativeWriter.open(QIODevice::WriteOnly | QIODevice::Append));
    const QByteArray externalRecord("external writer bytes\n");
    QCOMPARE(uncooperativeWriter.write(externalRecord), qint64(externalRecord.size()));
    QVERIFY(uncooperativeWriter.flush());

    QVERIFY(!store.append(physicsEvent(2), &error));
    QCOMPARE(readAll(path), originalPrefix + externalRecord);
    uncooperativeWriter.close();
}

void TstAgentEvaluationCaptureStore::invalidEventsDoNotMutateThePrefix()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("evaluation.jsonl"));

    RAgentEvaluationCaptureStore store;
    QString error;
    QVERIFY2(store.configure(path, &error), qPrintable(error));
    const QByteArray genesis = readAll(path);

    const QJsonObject oversized{{QStringLiteral("physicsTrace"),
                                 QString(RAgentEvaluationCaptureStore::MaxEventBytes, QLatin1Char('x'))}};
    QVERIFY(!store.append(oversized, &error));
    QCOMPARE(readAll(path), genesis);

    const QJsonObject credential{{QStringLiteral("metadata"),
                                  QJsonObject{{QStringLiteral("Authorization"), QStringLiteral("fixture-token")}}}};
    QVERIFY(!store.append(credential, &error));
    QCOMPARE(readAll(path), genesis);

    const QJsonObject rawPrompt{{QStringLiteral("trace"),
                                 QJsonArray{QJsonObject{{QStringLiteral("promptText"), QStringLiteral("full prompt")}}}}};
    QVERIFY(!store.append(rawPrompt, &error));
    QCOMPARE(readAll(path), genesis);

    const QJsonObject encodedSecret{{QStringLiteral("toolCalls"), QJsonArray{QJsonObject{
        {QStringLiteral("argumentsJson"), QStringLiteral("{\"nested\":{\"api_key\":\"fixture\"}}")}}}}};
    QVERIFY(!store.append(encodedSecret, &error));
    QCOMPARE(readAll(path), genesis);

    const QJsonObject encodedArray{{QStringLiteral("argumentsJson"), QStringLiteral("[1,2,3]")}};
    QVERIFY(!store.append(encodedArray, &error));
    QCOMPARE(readAll(path), genesis);

    const QJsonObject malformedArguments{{QStringLiteral("argumentsJson"), QStringLiteral("{not-json")}};
    QVERIFY(!store.append(malformedArguments, &error));
    QCOMPARE(readAll(path), genesis);

    QVERIFY2(store.append(physicsEvent(1), &error), qPrintable(error));
    const QByteArray acceptedPrefix = readAll(path);
    const QJsonObject nestedSecret{{QStringLiteral("items"),
                                   QJsonArray{QJsonObject{{QStringLiteral("api_key"), QStringLiteral("fixture")}}}}};
    QVERIFY(!store.append(nestedSecret, &error));
    QCOMPARE(readAll(path), acceptedPrefix);
    QVERIFY2(store.append(physicsEvent(2), &error), qPrintable(error));
    QVERIFY(readAll(path).startsWith(acceptedPrefix));
}

QTEST_APPLESS_MAIN(TstAgentEvaluationCaptureStore)
#include "tst_agent_evaluation_capture_store.moc"
