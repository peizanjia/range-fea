#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "workflow_process.h"

namespace {
const char fixture[] = R"PY(
import json, sys, threading, time
lock = threading.Lock()
queries = {}
threads = []
def reply(request, result=None, code="OK", raw=None):
    value = {"apiVersion":"range.workflow/v1", "requestId":request["requestId"],
        "status":"ok" if code == "OK" else "error", "code":code, "result":result or {}}
    with lock:
        print(raw if raw is not None else json.dumps(value), flush=True)
def later(request, delay, result):
    time.sleep(delay)
    reply(request, result)
def schedule(request, delay, result):
    worker = threading.Thread(target=later, args=(request, delay, result))
    threads.append(worker)
    worker.start()
for line in sys.stdin:
    request = json.loads(line)
    operation, parameters = request["operation"], request["parameters"]
    if operation == "capabilities.query":
        reply(request, {"apiVersion":"range.workflow/v1"})
    elif operation == "host.read.cancel":
        target = parameters["requestId"]
        options = queries.get(target, {})
        result = {"requestId":target, "readCancelled":True}
        if options.get("cancelDelay"):
            schedule(request, options["cancelDelay"], result)
        else:
            reply(request, result)
    elif parameters.get("bad") == "unknown":
        request["requestId"] = "00000000-0000-4000-8000-000000000000"
        reply(request)
    elif parameters.get("bad") == "duplicate":
        value = json.dumps({"apiVersion":"range.workflow/v1", "requestId":request["requestId"],
            "status":"ok", "code":"OK", "result":{}})
        reply(request, raw=value[:-1] + ',"c\u006fde":"OK"}')
    elif parameters.get("delay"):
        queries[request["requestId"]] = parameters
        schedule(request, parameters["delay"], {"tag":parameters.get("tag", "late")})
    else:
        reply(request, {"tag":parameters.get("tag", "control")})
for worker in threads:
    worker.join()
)PY";

bool hasId(const QSignalSpy &spy, const QString &id)
{
    for (const auto &row : spy) if (row[0].toString() == id) return true;
    return false;
}
}

class TestWorkflowTransport : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary;
    WorkflowProcess transport;
private slots:
    void init()
    {
        QVERIFY(temporary.isValid());
        QVERIFY(QDir().mkpath(temporary.filePath("range_workflow")));
        QFile script(temporary.filePath("range_workflow/__main__.py"));
        QVERIFY(script.open(QIODevice::WriteOnly)); QCOMPARE(script.write(fixture), qint64(sizeof(fixture) - 1)); script.close();
        QFile init(temporary.filePath("range_workflow/__init__.py")); QVERIFY(init.open(QIODevice::WriteOnly)); init.close();
        auto config = WorkflowProcess::defaultConfiguration(temporary.filePath("workspace"));
        QVERIFY2(QFileInfo(config.python).isFile(), "A Python interpreter is required for this stdio fixture.");
        config.packageDirectory = temporary.path(); config.examples = temporary.path();
        config.solver = config.python; config.native = config.python; config.nativeResults.clear();
        config.requestTimeoutMs = 1000;
        transport.start(config);
        QTRY_VERIFY_WITH_TIMEOUT(transport.isReady(), 3000);
    }

    void cleanup()
    {
        transport.stop();
        QTRY_VERIFY_WITH_TIMEOUT(transport.isStopped(), 5000);
    }

    void outOfOrderReadsLeaveFastControlResponsive()
    {
        QSignalSpy replies(&transport, &WorkflowProcess::replyReceived);
        QSignalSpy failures(&transport, &WorkflowProcess::requestFailed);
        const auto slow = transport.request("result.plot.query", {{"delay", .6}, {"tag", "slow"}});
        const auto fast = transport.request("result.raw.query", {{"delay", .2}, {"tag", "fast"}});
        const auto control = transport.request("runtime.parameters.query");
        QTRY_VERIFY_WITH_TIMEOUT(hasId(replies, control), 200);
        QVERIFY(!hasId(replies, slow));
        QTRY_VERIFY_WITH_TIMEOUT(hasId(replies, fast), 500);
        QVERIFY(!hasId(replies, slow));
        QTRY_VERIFY_WITH_TIMEOUT(hasId(replies, slow), 1000);
        QCOMPARE(failures.size(), 0);
    }

    void readTimeoutRetiresOnlyItsIdAndAcceptsLateReply()
    {
        QSignalSpy replies(&transport, &WorkflowProcess::replyReceived);
        QSignalSpy failures(&transport, &WorkflowProcess::requestFailed);
        QSignalSpy errors(&transport, &WorkflowProcess::serviceError);
        const auto read = transport.request("result.raw.export", {{"delay", 1.4}});
        QTRY_VERIFY_WITH_TIMEOUT(hasId(failures, read), 1800);
        QCOMPARE(failures[0][2].toString(), QString("REQUEST_TIMEOUT"));
        QVERIFY(transport.isReady());
        const auto control = transport.request("job.cancel");
        QTRY_VERIFY_WITH_TIMEOUT(hasId(replies, control), 400);
        QTest::qWait(600);
        QVERIFY(!hasId(replies, read)); QVERIFY(transport.isReady()); QCOMPARE(errors.size(), 0);
        const auto next = transport.request("runtime.parameters.query");
        QTRY_VERIFY_WITH_TIMEOUT(hasId(replies, next), 400);
    }

    void overdueCancelReceiptKeepsServiceUsableAndIgnoresValidatedLateAck()
    {
        QSignalSpy replies(&transport, &WorkflowProcess::replyReceived);
        QSignalSpy failures(&transport, &WorkflowProcess::requestFailed);
        QSignalSpy errors(&transport, &WorkflowProcess::serviceError);
        const auto read = transport.request("result.plot.query", {{"delay", 2.4}, {"cancelDelay", 1.4}});
        QTRY_VERIFY_WITH_TIMEOUT(hasId(failures, read), 1800);
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 2, 1800);
        QCOMPARE(failures[1][1].toString(), QString("host.read.cancel"));
        QCOMPARE(failures[1][2].toString(), QString("REQUEST_TIMEOUT"));
        QVERIFY(transport.isReady());
        const auto control = transport.request("runtime.parameters.query");
        QTRY_VERIFY_WITH_TIMEOUT(hasId(replies, control), 400);
        QTest::qWait(700);
        QVERIFY(transport.isReady()); QVERIFY(!hasId(replies, read)); QCOMPARE(errors.size(), 0);
    }

    void unknownAndDuplicateResponsesCloseProtocol_data()
    {
        QTest::addColumn<QString>("bad");
        QTest::newRow("unknown-id") << QString("unknown");
        QTest::newRow("escaped-duplicate-key") << QString("duplicate");
    }
    void unknownAndDuplicateResponsesCloseProtocol()
    {
        QFETCH(QString, bad);
        QSignalSpy failures(&transport, &WorkflowProcess::requestFailed);
        const auto id = transport.request("project.query", {{"bad", bad}});
        QTRY_VERIFY_WITH_TIMEOUT(hasId(failures, id), 1000);
        QCOMPARE(failures[0][2].toString(), QString("INVALID_RESPONSE"));
        QVERIFY(!transport.isReady());
        QTRY_VERIFY_WITH_TIMEOUT(transport.isStopped(), 3000);
    }

    void mutationTimeoutStillClosesAllRequests()
    {
        QSignalSpy failures(&transport, &WorkflowProcess::requestFailed);
        const auto mutation = transport.request("command.execute", {{"delay", 2.5}});
        const auto read = transport.request("result.plot.query", {{"delay", 2.5}});
        QTRY_VERIFY_WITH_TIMEOUT(hasId(failures, mutation) && hasId(failures, read), 1800);
        for (const auto &row : failures) QCOMPARE(row[2].toString(), QString("REQUEST_TIMEOUT"));
        QVERIFY(!transport.isReady());
        QTRY_VERIFY_WITH_TIMEOUT(transport.isStopped(), 3000);
    }
};

QTEST_GUILESS_MAIN(TestWorkflowTransport)
#include "tst_workflow_transport.moc"
