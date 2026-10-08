#include "native_results_widget.h"

#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
struct ResultStore
{
    int frameCount = 450;
    int channelCount = 201;
    int seriesQueries = 0;
    double valueShift = 0;
    bool nullSample = false;
    bool wrongArtifact = false;
    bool live = false;
    int liveRevision = 1;
    bool pending = false;
    bool unavailable = false;
    int catalogQueries = 0;
    QJsonObject read(const QString &operation, const QJsonObject &args)
    {
        // The UI may select registered identities, never ask this owner for paths.
        if (args.contains("path") || args.contains("manifestPath")) return {{"valid", false}, {"message", "Unexpected path"}};
        const QString kind = operation.section('.', 2, 2);
        const QString job = args.value("jobId").toString();
        const QString latestArtifact = "artifact-" + job + (live ? QString("-live-%1").arg(liveRevision) : QString());
        const QString artifact = wrongArtifact ? "other-artifact" : args.value("artifactId").toString(latestArtifact);
        QJsonObject response{{"valid", true}, {"operation", kind}, {"artifactId", artifact}};
        const int offset = args.contains("cursor") ? args.value("cursor").toString().toInt() : args.value("offset").toInt();
        const int limit = args.value("limit").toInt(200);
        if (kind == "catalog")
        {
            ++catalogQueries;
            if (pending) return {{"valid", false}, {"code", "RESULT_PENDING"}, {"message", "Waiting for the first accepted frame"}};
            if (unavailable) return {{"valid", false}, {"code", "RESULT_UNAVAILABLE"}, {"message", "Result unavailable"}};
            response.insert("jobId", job); response.insert("frameCount", frameCount);
            response.insert("analysis", QJsonObject{{"type", "structural-dynamics"}});
            response.insert("sourceKind", "native-passive");
            response.insert("sourceSnapshotRef", QJsonObject{{"projectId", "project-" + job}, {"sourceRevision", 7}});
            response.insert("proofScope", QJsonObject{{"scope", "frozen-software-inputs"}});
            response.insert("live", live); response.insert("completed", !live);
        }
        else if (kind == "channels")
        {
            QJsonArray page;
            for (int index = offset; index < qMin(channelCount, offset + limit); ++index)
                page.append(QJsonObject{{"channelId", QString("node:%1:displacement:X").arg(index + 1)}, {"unit", "m"}});
            response.insert("channels", page); response.insert("channelCount", channelCount); response.insert("offset", offset);
            if (offset + page.size() < channelCount) response.insert("nextCursor", QString::number(offset + page.size()));
        }
        else if (kind == "series")
        {
            ++seriesQueries;
            QJsonArray page;
            for (int index = offset; index < qMin(frameCount, offset + limit); ++index)
            {
                // Numbered records are sparse, and values are never synthesized for gaps.
                QJsonObject sample{{"recordNo", 2 * index + 1}, {"timeS", index * .01}, {"value", valueShift + index * .5}};
                const QString originKind = index == 201 ? "legacy-history-unattributed" : index < 250 ? "reused-history" : "produced";
                const QJsonValue origin = index == 201 ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{{"jobId", index < 250 ? "original-job" : job}, {"sourceId", index < 250 ? "original-source" : "current-source"}});
                sample.insert("originKind", originKind); sample.insert("origin", origin);
                if (index < 250) sample.insert("historyProof", QJsonObject{{"recordNo", 2 * index + 1}, {"originKind", originKind}, {"origin", origin}});
                if (nullSample && index == offset) sample.insert("value", QJsonValue::Null);
                page.append(sample);
            }
            response.insert("channelId", args.value("channelId")); response.insert("unit", "m");
            response.insert("samples", page); response.insert("offset", offset);
            if (offset + page.size() < frameCount) response.insert("nextCursor", QString::number(offset + page.size()));
        }
        else if (kind == "summary")
        {
            response.insert("channelId", args.value("channelId")); response.insert("unit", "m");
            response.insert("sampleCount", frameCount); response.insert("min", valueShift); response.insert("max", valueShift + (frameCount - 1) * .5);
            response.insert("mean", valueShift + (frameCount - 1) * .25); response.insert("rms", 1.0);
        }
        return response;
    }
};

// Read the exported frozen CSV using its actual quoted-cell rules, including
// commas/quotes in the provenance JSON, rather than substring matching rows.
QStringList csvRow(const QByteArray &bytes)
{
    const QString line = QString::fromUtf8(bytes);
    QStringList fields; QString field; bool quoted = false;
    for (int index = 0; index < line.size(); ++index)
    {
        const QChar character = line[index];
        if (character == '"')
        {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') { field += '"'; ++index; }
            else quoted = !quoted;
        }
        else if (character == ',' && !quoted) { fields.append(field); field.clear(); }
        else field += character;
    }
    fields.append(field); return fields;
}
}

class TestNativeResultsWidget : public QObject
{
    Q_OBJECT
private slots:
    void finitePagesAndChannelSelection();
    void nullIsUnavailableAndWrongArtifactIsRejected();
    void heldRangeMatchesTableAndFrozenCsv();
    void lateReplyCannotCrossSourceSwitch();
    void readsAll5000NativeSamples();
    void liveRefreshFollowsLatestButExplicitArtifactAndHeldSnapshotStayFixed();
    void mixedFrameOriginsBeyondFirstPageSurviveFrozenCsv();
    void pendingFirstFramePollsThroughLiveAndStopsAtTerminalOrFailure();
};

void TestNativeResultsWidget::finitePagesAndChannelSelection()
{
    ResultStore store; NativeResultsWidget widget;
    widget.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    widget.setArtifact("one", {}, "First source");
    QTRY_VERIFY(!widget.displaySnapshot().value("summary").toObject().isEmpty());
    auto *channels = widget.findChild<QComboBox *>("nativeResultChannel");
    QCOMPARE(channels->count(), 201);
    QCOMPARE(widget.displaySnapshot().value("samples").toArray().size(), 450);
    QCOMPARE(store.seriesQueries, 3);
    QVERIFY(widget.displaySnapshot().value("sparse").toBool());
    QVERIFY(widget.displaySnapshot().value("complete").toBool());
    channels->setCurrentIndex(200);
    QTRY_VERIFY(!widget.displaySnapshot().value("summary").toObject().isEmpty());
    QCOMPARE(widget.displaySnapshot().value("channelId").toString(), QString("node:201:displacement:X"));
    widget.refreshResults();
    QTRY_COMPARE(store.seriesQueries, 9);
    QCOMPARE(channels->currentIndex(), 200);
    QCOMPARE(widget.findChild<QTableWidget *>("nativeResultTable")->rowCount(), 450);
}

void TestNativeResultsWidget::nullIsUnavailableAndWrongArtifactIsRejected()
{
    ResultStore store; store.nullSample = true; NativeResultsWidget widget;
    QSignalSpy failures(&widget, &NativeResultsWidget::queryFailed);
    widget.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    widget.setArtifact("one", "artifact-one", "First source");
    QTRY_COMPARE(failures.count(), 1);
    QVERIFY(widget.displaySnapshot().value("samples").toArray().isEmpty());
    QCOMPARE(widget.findChild<QTableWidget *>("nativeResultTable")->rowCount(), 0);
    QVERIFY(!widget.findChild<QPushButton *>("nativeResultExport")->isEnabled());
    QVERIFY(widget.findChild<QLabel *>("nativeResultStatus")->text().contains("missing"));
    store.nullSample = false; store.wrongArtifact = true; widget.refreshResults();
    QTRY_COMPARE(failures.count(), 2);
    QCOMPARE(widget.displaySnapshot().value("artifactId").toString(), QString("artifact-one"));
    QVERIFY(widget.displaySnapshot().value("samples").toArray().isEmpty());
}

void TestNativeResultsWidget::heldRangeMatchesTableAndFrozenCsv()
{
    ResultStore store; store.frameCount = 100; store.channelCount = 2;
    NativeResultsWidget widget;
    widget.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    widget.setArtifact("one", "artifact-one", "First source");
    QTRY_VERIFY(!widget.displaySnapshot().value("summary").toObject().isEmpty());
    widget.findChild<QComboBox *>("nativeResultRange")->setCurrentIndex(1);
    widget.setHeld(true);
    const auto frozen = widget.displaySnapshot();
    QVERIFY(frozen.value("samples").toArray().size() < store.frameCount);
    QVERIFY(!widget.findChild<QComboBox *>("nativeResultChannel")->isEnabled());
    QVERIFY(!widget.findChild<QComboBox *>("nativeResultRange")->isEnabled());
    store.valueShift = 1000;
    widget.refreshResults();
    QTRY_COMPARE(store.seriesQueries, 2);
    // Repeated hold/refresh does not relabel a snapshot with new metadata.
    widget.setHeld(true);
    QCOMPARE(widget.displaySnapshot(), frozen);
    auto *table = widget.findChild<QTableWidget *>("nativeResultTable");
    const auto points = frozen.value("samples").toArray();
    QCOMPARE(table->rowCount(), points.size());
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QString error; const QString path = directory.filePath("frozen.csv");
    QVERIFY2(widget.exportCsv(path, &error), qPrintable(error));
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto lines = file.readAll().split('\n'); QCOMPARE(lines.size(), points.size() + 2);
    for (int index = 0; index < points.size(); ++index)
    {
        const auto row = csvRow(lines[index + 1]); QCOMPARE(row.size(), 15);
        QCOMPARE(row[0], frozen.value("jobId").toString()); QCOMPARE(row[1], frozen.value("artifactId").toString());
        QCOMPARE(row[3], frozen.value("channelId").toString()); QCOMPARE(row[4], frozen.value("unit").toString());
        QCOMPARE(QJsonDocument::fromJson(row[5].toUtf8()).object(), frozen.value("range").toObject());
        QCOMPARE(QJsonDocument::fromJson(row[6].toUtf8()).object(), frozen.value("provenance").toObject());
        QCOMPARE(row[9].toInt(), points[index].toObject().value("recordNo").toInt());
        QCOMPARE(row[10].toDouble(), points[index].toObject().value("timeS").toDouble());
        QCOMPARE(row[11], table->item(index, 2)->text());
    }
    widget.setHeld(false);
    QTRY_COMPARE(widget.displaySnapshot().value("samples").toArray().first().toObject().value("value").toDouble(),
                 points.first().toObject().value("value").toDouble() + 1000.0);
    widget.setArtifact("two", {}, "Second source");
    QTRY_COMPARE(widget.displaySnapshot().value("artifactId").toString(), QString("artifact-two"));
    QVERIFY(!widget.findChild<QPushButton *>("nativeResultHold")->isChecked());
    QCOMPARE(widget.displaySnapshot().value("jobId").toString(), QString("two"));
}

void TestNativeResultsWidget::lateReplyCannotCrossSourceSwitch()
{
    ResultStore store; store.frameCount = 2; store.channelCount = 1; NativeResultsWidget widget;
    bool switched = false;
    widget.setQueryHandler([&](const auto &operation, const auto &args) {
        const auto response = store.read(operation, args);
        if (!switched && args.value("jobId").toString() == "old")
        { switched = true; widget.setArtifact("new", {}, "New source"); }
        return response;
    });
    widget.setArtifact("old", {}, "Old source");
    QTRY_VERIFY(!widget.displaySnapshot().value("summary").toObject().isEmpty());
    const auto result = widget.displaySnapshot();
    QCOMPARE(result.value("jobId").toString(), QString("new"));
    QCOMPARE(result.value("artifactId").toString(), QString("artifact-new"));
    QCOMPARE(result.value("provenance").toObject().value("sourceSnapshotRef").toObject().value("projectId").toString(), QString("project-new"));
    QCOMPARE(store.seriesQueries, 1);
}

void TestNativeResultsWidget::readsAll5000NativeSamples()
{
    ResultStore store; store.frameCount = 5000; store.channelCount = 1; NativeResultsWidget widget;
    widget.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    widget.setArtifact("one", {}, "First source");
    QTRY_VERIFY_WITH_TIMEOUT(!widget.displaySnapshot().value("summary").toObject().isEmpty(), 10000);
    QCOMPARE(store.seriesQueries, 25);
    QCOMPARE(widget.displaySnapshot().value("samples").toArray().size(), 5000);
    QCOMPARE(widget.displaySnapshot().value("samples").toArray().last().toObject().value("recordNo").toInt(), 9999);
}

void TestNativeResultsWidget::liveRefreshFollowsLatestButExplicitArtifactAndHeldSnapshotStayFixed()
{
    ResultStore store; store.frameCount = 2; store.channelCount = 1; store.live = true;
    NativeResultsWidget latest;
    latest.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    latest.setArtifact("one", {}, "Latest source"); latest.show();
    QTRY_VERIFY(!latest.displaySnapshot().value("summary").toObject().isEmpty());
    QCOMPARE(latest.displaySnapshot().value("artifactId").toString(), QString("artifact-one-live-1"));
    latest.setHeld(true); const auto frozen = latest.displaySnapshot();
    store.liveRevision = 2; store.valueShift = 20; store.frameCount = 3;
    auto *timer = latest.findChild<QTimer *>("nativeResultLiveTimer"); QVERIFY(timer);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QTRY_COMPARE(store.seriesQueries, 2); QCOMPARE(latest.displaySnapshot(), frozen);
    latest.setHeld(false);
    QTRY_COMPARE(latest.displaySnapshot().value("artifactId").toString(), QString("artifact-one-live-2"));
    QTRY_VERIFY(!latest.displaySnapshot().value("summary").toObject().isEmpty());
    store.live = false;
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QTRY_COMPARE(latest.displaySnapshot().value("artifactId").toString(), QString("artifact-one"));
    QTRY_VERIFY(!latest.displaySnapshot().value("summary").toObject().isEmpty());
    QVERIFY(latest.displaySnapshot().value("provenance").toObject().value("completed").toBool());
    const int terminalQueries = store.seriesQueries;
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection)); QTest::qWait(30);
    QCOMPARE(store.seriesQueries, terminalQueries);
    store.live = true; store.liveRevision = 3;
    NativeResultsWidget fixed;
    fixed.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    fixed.setArtifact("one", "artifact-one-live-1", "Explicit frozen artifact"); fixed.show();
    QTRY_VERIFY(!fixed.displaySnapshot().value("summary").toObject().isEmpty());
    fixed.refreshResults(); QTRY_COMPARE(store.seriesQueries, terminalQueries + 2);
    QCOMPARE(fixed.displaySnapshot().value("artifactId").toString(), QString("artifact-one-live-1"));
}

void TestNativeResultsWidget::mixedFrameOriginsBeyondFirstPageSurviveFrozenCsv()
{
    ResultStore store; store.channelCount = 1; NativeResultsWidget widget;
    widget.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    widget.setArtifact("one", {}, "Mixed produced and reused records");
    QTRY_VERIFY(!widget.displaySnapshot().value("summary").toObject().isEmpty());
    widget.setHeld(true); const auto frozen = widget.displaySnapshot();
    QTemporaryDir directory; QString error;
    const QString path = directory.filePath("mixed-history.csv"); QVERIFY(widget.exportCsv(path, &error));
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto lines = file.readAll().split('\n');
    auto *table = widget.findChild<QTableWidget *>("nativeResultTable");
    for (const int index : {200, 201, 249, 250, 449})
    {
        const auto point = frozen.value("samples").toArray()[index].toObject(); const auto row = csvRow(lines[index + 1]);
        QCOMPARE(row.size(), 15); QCOMPARE(row[12], point.value("originKind").toString());
        QCOMPARE(table->item(index, 3)->text(), row[12]);
        if (index == 201) { QCOMPARE(row[13], QString("null")); QVERIFY(table->item(index, 3)->toolTip().contains("null")); }
        else QCOMPARE(QJsonDocument::fromJson(row[13].toUtf8()).object(), point.value("origin").toObject());
        if (index < 250) QCOMPARE(QJsonDocument::fromJson(row[14].toUtf8()).object(), point.value("historyProof").toObject());
        else QCOMPARE(row[14], QString("null"));
    }
}

void TestNativeResultsWidget::pendingFirstFramePollsThroughLiveAndStopsAtTerminalOrFailure()
{
    ResultStore store; store.frameCount = 2; store.channelCount = 1; store.pending = true; store.live = true;
    NativeResultsWidget widget; QSignalSpy failures(&widget, &NativeResultsWidget::queryFailed);
    widget.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    widget.setArtifact("one", {}, "Running source awaiting its first frame"); widget.show();
    QTRY_COMPARE(failures.count(), 1);
    QVERIFY(widget.displaySnapshot().value("samples").toArray().isEmpty());
    store.pending = false;
    // Exercise real timer events, including the initially empty catalog case.
    QTRY_VERIFY_WITH_TIMEOUT(!widget.displaySnapshot().value("summary").toObject().isEmpty(), 4000);
    QCOMPARE(widget.displaySnapshot().value("artifactId").toString(), QString("artifact-one-live-1"));
    store.live = false;
    QTRY_COMPARE_WITH_TIMEOUT(widget.displaySnapshot().value("artifactId").toString(), QString("artifact-one"), 4000);
    QTRY_VERIFY(!widget.displaySnapshot().value("summary").toObject().isEmpty());
    const int terminalReads = store.catalogQueries; QTest::qWait(1100); QCOMPARE(store.catalogQueries, terminalReads);
    store.unavailable = true; widget.refreshResults();
    QTRY_COMPARE(failures.count(), 2);
    const int failureReads = store.catalogQueries; QTest::qWait(1100); QCOMPARE(store.catalogQueries, failureReads);
    store.unavailable = false; store.pending = true;
    NativeResultsWidget fixed;
    fixed.setQueryHandler([&](const auto &operation, const auto &args) { return store.read(operation, args); });
    fixed.setArtifact("one", "artifact-one-live-1", "Explicit fixed artifact"); fixed.show();
    QTRY_COMPARE(store.catalogQueries, failureReads + 1);
    QTest::qWait(1100); QCOMPARE(store.catalogQueries, failureReads + 1);
}

int runNativeResultsWidgetTests(int argc, char **argv)
{
    TestNativeResultsWidget test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_native_results_widget.moc"
