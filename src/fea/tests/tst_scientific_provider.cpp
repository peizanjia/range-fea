#include <QtTest>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QApplication>
#include <QDialog>
#include <cstdio>

#include "agent_widget.h"
#include "application.h"
#include "main_window.h"
#include "session.h"
#include "workflow_widget.h"

namespace
{
const QString extensionId = QStringLiteral("acceptance.scientific.generated");
bool sha256(const QJsonValue &value)
{
    return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
        .match(value.toString()).hasMatch();
}

// The root driver serially prepares scientific_widget_bootstrap.py's fixture.
// This test consumes real registered native jobs; it never fabricates receipts,
// copies ownership records, or substitutes reference code for provider code.
class TestScientificProvider : public QObject
{
    Q_OBJECT
private slots:
    void deepSeekGeneratesScientificV2()
    {
        const auto credentialsPath = qEnvironmentVariable("RANGE_TEST_DEEPSEEK_CREDENTIALS_FILE");
        if (credentialsPath.isEmpty()) QSKIP("Opt-in real provider acceptance: credentials flag absent.");

        const auto fixturePath = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_FIXTURE");
        QVERIFY2(QFileInfo(fixturePath).isAbsolute() && QFileInfo(fixturePath).isFile(),
                 "RANGE_TEST_SCIENTIFIC_FIXTURE must name an absolute bootstrap fixture.json.");
        QFile fixtureFile(fixturePath);
        QVERIFY(fixtureFile.open(QIODevice::ReadOnly));
        QJsonParseError parseError;
        const auto fixtureDocument = QJsonDocument::fromJson(fixtureFile.readAll(), &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        const auto fixture = fixtureDocument.object();
        const auto workspace = fixture.value("workspace").toString();
        QVERIFY(QFileInfo(workspace).isAbsolute() && QFileInfo(workspace).isDir());
        // Keep conversation state separate from user projects and bootstrap ownership.
        QTemporaryDir conversation;
        QVERIFY(conversation.isValid());
        QString projectError;
        QVERIFY(Application::instance()->getSession()->openProject(conversation.path(), &projectError));

        auto *window = Application::instance()->getMainWindow();
        auto *workflow = window->findChild<WorkflowWidget *>();
        auto *agent = window->findChild<AgentWidget *>();
        QVERIFY(workflow); QVERIFY(agent);
        auto config = WorkflowProcess::defaultConfiguration(workspace);
        config.python = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_PYTHON");
        config.packageDirectory = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_PACKAGE");
        config.solver = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_SOLVER");
        config.native = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_NATIVE");
        config.examples = qEnvironmentVariable("RANGE_TEST_SCIENTIFIC_EXAMPLES");
        for (const auto &path : {config.python, config.solver, config.native})
            QVERIFY2(QFileInfo(path).isAbsolute() && QFileInfo(path).isFile(),
                     "Scientific Python, solver and native paths must be explicit absolute files.");
        for (const auto &path : {config.packageDirectory, config.examples})
            QVERIFY2(QFileInfo(path).isAbsolute() && QFileInfo(path).isDir(),
                     "Scientific package and examples paths must be explicit absolute directories.");
        const QString fixturePython = fixture.value("python").toString();
        const QString fixtureSolver = fixture.value("solver").toString();
        const QString fixtureNative = fixture.value("native").toString();
        for (const auto &path : {fixturePython, fixtureSolver, fixtureNative})
            QVERIFY2(QFileInfo(path).isAbsolute() && QFileInfo(path).isFile(),
                     "Bootstrap Python, solver and native paths must be explicit absolute files.");
        QCOMPARE(QFileInfo(config.python).canonicalFilePath(), QFileInfo(fixturePython).canonicalFilePath());
        QCOMPARE(QFileInfo(config.solver).canonicalFilePath(), QFileInfo(fixtureSolver).canonicalFilePath());
        QCOMPARE(QFileInfo(config.native).canonicalFilePath(), QFileInfo(fixtureNative).canonicalFilePath());
        config.requestTimeoutMs = 30000;
        config.trustedExtensions = true;
        const auto previousSettings = agent->getProviderSettings();
        auto cleanup = qScopeGuard([&] {
            agent->taskRunner()->cancel();
            agent->setProviderSettings(previousSettings);
            workflow->transport()->stop();
        });
        workflow->openWorkspace(config);
        auto *service = workflow->transport();
        QTRY_VERIFY_WITH_TIMEOUT(service->isReady(), 15000);

        QString rpcError;
        const auto rpc = [&](const QString &operation, const QJsonObject &parameters = QJsonObject{}) {
            std::fprintf(stderr,"SCIENCE_RPC_START %s\n",operation.toUtf8().constData()); std::fflush(stderr);
            QEventLoop loop;
            QJsonObject result;
            QString id;
            rpcError.clear();
            const auto success = connect(service, &WorkflowProcess::replyReceived, &loop,
                [&](const QString &received, const QString &, const QJsonObject &value) {
                    if (received == id) { result = value; loop.quit(); }
                });
            const auto failure = connect(service, &WorkflowProcess::requestFailed, &loop,
                [&](const QString &received, const QString &, const QString &code, const QString &) {
                    if (received == id) { rpcError = code; loop.quit(); }
                });
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, [&] { rpcError = "RPC_TIMEOUT"; loop.quit(); });
            id = service->request(operation, parameters);
            timeout.start(30000); loop.exec();
            disconnect(success); disconnect(failure);
            std::fprintf(stderr,"SCIENCE_RPC_END %s code=%s\n",operation.toUtf8().constData(),rpcError.toUtf8().constData()); std::fflush(stderr);
            return result;
        };
        // ACK/query avoids blocking the 30-second GUI request deadline on full-file verification.
        const auto ack = rpc("extension.runtime.verify", {{"sdkVersion", "range.extension/v2"}, {"force", true}});
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QVERIFY(ack.value("state") == QJsonValue("queued") || ack.value("state") == QJsonValue("running"));
        QVERIFY(!ack.value("verificationId").toString().isEmpty());
        QJsonObject runtime;
        QElapsedTimer verificationDeadline; verificationDeadline.start();
        while (verificationDeadline.elapsed() < 360000) {
            runtime = rpc("extension.runtime.verification.query", {{"verificationId", ack.value("verificationId")}});
            QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
            const auto state = runtime.value("state").toString();
            if (state == "ready") break;
            QVERIFY2(state == "queued" || state == "running", "Scientific runtime verification failed.");
            QTest::qWait(100);
        }
        QCOMPARE(runtime.value("state").toString(), QString("ready"));
        QCOMPARE(runtime.value("sdkVersion").toString(), QString("range.extension/v2"));
        QCOMPARE(runtime.value("profile").toString(), QString("packaged"));
        QCOMPARE(runtime.value("currentity").toString(), QString("current"));
        QCOMPARE(runtime.value("currentityScope").toString(), QString("environment-stamp"));
        QVERIFY(runtime.value("isCurrent").toBool());
        QVERIFY(sha256(runtime.value("runtimeHash")));
        const auto fullVerify = runtime.value("fullFileVerify").toObject();
        QVERIFY(fullVerify.value("verifiedFiles").toBool());
        QCOMPARE(fullVerify.value("runtimeHash"), runtime.value("runtimeHash"));
        QCOMPARE(fullVerify.value("environmentStamp"), runtime.value("expectedEnvironmentStamp"));

        const auto existing = rpc("extension.list", {}).value("extensions").toArray();
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        for (const auto &entry : existing)
            QVERIFY2(entry.toObject().value("extensionId") != QJsonValue(extensionId),
                     "Use a fresh disposable bootstrap workspace for each provider attempt.");
        const auto sources = fixture.value("sources").toArray();
        QCOMPARE(sources.size(), 2);
        const auto catalog = rpc("result.catalog.query", {{"jobId", fixture.value("sourceJob")}, {"mode", "active"}, {"limit", 1}});
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QCOMPARE(catalog.value("frameCount").toInt(), 20);
        QCOMPARE(catalog.value("sourceKind").toString(), QString("native-active-fe"));
        QCOMPARE(catalog.value("artifactId"), fixture.value("nativeCatalog").toObject().value("artifactId"));

        // Only this opt-in test reads local credentials. Never print provider messages or HTTP bodies.
        QFile credentials(credentialsPath);
        QVERIFY(credentials.open(QIODevice::ReadOnly));
        QVERIFY(credentials.size() < 65536);
        QString apiKey;
        for (auto line : QString::fromUtf8(credentials.readAll()).split('\n')) {
            line = line.trimmed();
            if (line.startsWith("DEEPSEEK_API_KEY=")) apiKey = line.mid(17).trimmed();
        }
        QVERIFY(!apiKey.isEmpty());
        RAgentSettings settings(RAgentSettings::Type::DeepSeek);
        settings.setMaxTokens(qEnvironmentVariable("RANGE_TEST_DEEPSEEK_MAX_TOKENS", "8192").toInt());
        settings.setApiKey(apiKey); agent->setProviderSettings(settings);
        auto *effort = agent->findChild<QComboBox *>("agentThinkingEffort");
        QVERIFY(effort);
        const int effortIndex = effort->findData(qEnvironmentVariable("RANGE_TEST_DEEPSEEK_EFFORT", "off"));
        QVERIFY(effortIndex >= 0); effort->setCurrentIndex(effortIndex);

        QSignalSpy finished(agent->taskRunner(), &AgentTaskRunner::finished);
        QSignalSpy usage(agent, &AgentWidget::providerUsage);
        QSet<QString> schemas;
        bool knowledgeRead = false, capabilitiesRead = false, rejectedProposal = false;
        int approvals = 0;
        QJsonArray inspectedInputs;
        bool typedInspectionCompleted = false;
        QJsonArray toolTrace;
        QString stageJobId;
        QObject observationScope;
        connect(service, &WorkflowProcess::replyReceived, &observationScope,
            [&](const QString &, const QString &operation, const QJsonObject &result) {
                if (operation == "extension.scientific.input.inspect") inspectedInputs.append(result);
                if (operation == "schema.get") schemas.insert(result.value("schemaId").toString());
                if (operation == "knowledge.query") knowledgeRead = true;
                if (operation == "capabilities.query") capabilitiesRead = true;
                if (operation == "command.execute" && approvals == 1 && stageJobId.isEmpty())
                    stageJobId = result.value("jobId").toString();
            });
        connect(agent->taskRunner(), &AgentTaskRunner::toolActivity, &observationScope,
            [&](const QString &, const QString &, const QString &name, const QString &phase,
                const QString &, const QJsonObject &detail) {
                QJsonObject trace{{"tool",name},{"phase",phase}};
                const QString code=detail.value("code").toString();
                if(QRegularExpression("^[A-Z][A-Z0-9_]{0,63}$").match(code).hasMatch()) trace.insert("code",code);
                toolTrace.append(trace);
                std::fprintf(stderr,"SCIENCE_TOOL %s %s code=%s\n",name.toUtf8().constData(),phase.toUtf8().constData(),trace.value("code").toString().toUtf8().constData()); std::fflush(stderr);
                if (name == "workflow_capabilities" && phase == "completed") capabilitiesRead = true;
                if (name == "scientific_input_inspect" && phase == "completed") typedInspectionCompleted = true;
            });
        int clarificationCount=0;
        connect(agent->taskRunner(), &AgentTaskRunner::userInputRequested, &observationScope,
            [&](const QString &questionId,const QString &,const QJsonArray &) {
                ++clarificationCount;
                std::fprintf(stderr,"SCIENCE_CLARIFICATION count=%d\n",clarificationCount); std::fflush(stderr);
                if(clarificationCount>1) { agent->taskRunner()->cancel(); return; }
                // Repeat the fixed acceptance request; supply no expected
                // numerical answers or approval outside the staging boundary.
                QTimer::singleShot(0,agent->taskRunner(),[runner=agent->taskRunner(),questionId] {
                    runner->answerQuestion(questionId,QStringLiteral(
                        "Use the published frozen v2 schemas and repair the exact host-reported error. "
                        "Choose and derive your own small analytic test. Use scientific_input_inspect for its normalized input/hash. "
                        "Only staging is authorized; submit extension_propose with command extension.stage and manifest/source/tests. "
                        "The trusted test host will review that preview separately. No new dependencies or changes to native documents."));
                });
            });
        connect(agent->taskRunner(), &AgentTaskRunner::proposalReady, &observationScope,
            [&](const QString &origin, const QString &id, const QJsonObject &preview) {
                const auto normalized = preview.value("normalized").toObject();
                const bool allowed = approvals == 0 && origin == "extension"
                    && normalized.value("command") == QJsonValue("extension.stage")
                    && normalized.value("parameters").toObject().value("manifest").toObject().value("extensionId") == QJsonValue(extensionId)
                    && schemas.contains("range.extension/v2/manifest")
                    && schemas.contains("range.extension/v2/multichannel-series")
                    && schemas.contains("range.extension/v2/spectral-result") && knowledgeRead && capabilitiesRead
                    && typedInspectionCompleted && !inspectedInputs.isEmpty();
                if (allowed) ++approvals; else rejectedProposal = true;
                QTimer::singleShot(0, agent->taskRunner(), [runner = agent->taskRunner(), id, allowed] {
                    runner->authorizePending(id, allowed);
                });
            });
        agent->startTask(QStringLiteral(
            "Create a new scientific Python extension acceptance.scientific.generated version 1.0.0 for "
            "the frozen range.extension/v2 spectral-filter/v1 contract. First read workflow_capabilities, "
            "schema_get for range.extension/v2/manifest, range.extension/v2/multichannel-series and "
            "range.extension/v2/spectral-result, then knowledge_query for scientific spectral/filter v2. "
            "Implement your own process(data) supporting rectangular and periodic Hann windows, unnormalised "
            "rfft complex pairs, one-sided amplitude and PSD with undoubled DC/even Nyquist, and the specified "
            "causal zero-state first-order Butterworth lowpass before mean removal and windowing. Preserve "
            "explicit units and contract/input hashes. Only frozen numpy 2.2.6 and scipy 1.15.3 dependencies "
            "and permitted standard library are allowed. Never import range_workflow, scientific_reference, "
            "scientific_example or other host/test/reference modules. Author your own analytic small-N numerical "
            "case of 2..32 samples with complete input and expected output; do not borrow host golden cases. "
            "Use scientific_input_inspect with exactly {input: your own analytic input object}. Copy its "
            "normalizedInput exactly into the candidate case input and use its inputHash and contractHash "
            "in your independently authored expected output. This read-only tool binds metadata only; "
            "you must derive all expected numerical arrays yourself. Then stage your source and tests. "
            "Propose ONLY extension.stage with manifest, source and tests (object containing cases). The human "
            "test host explicitly approves staging only. Wait for the stage job with workflow_wait and finish "
            "only when staging succeeded. Use ONLY workflow_capabilities, schema_get, knowledge_query, "
            "scientific_input_inspect, extension_propose and workflow_wait. The generated extension is new; "
            "do not call extension_list or other extension/catalog tools. Do not validate, activate, run, modify documents, read files, use "
            "network/subprocesses or request direct host approval. The independent host test performs later "
            "validation and native-history execution separately."), false);
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 240000);
        QJsonObject diagnostic{{"state",finished.last().at(0).toString()},
            {"providerUsageReceipts",usage.size()},{"tools",toolTrace},{"stagingApprovals",approvals},
            {"proposalRejected",rejectedProposal},{"capabilitiesRead",capabilitiesRead},
            {"knowledgeRead",knowledgeRead},{"schemaCount",schemas.size()},
            {"typedInspectionCompleted",typedInspectionCompleted}};
        const QString failure=finished.last().at(1).toString();
        if(failure.startsWith("Tool '") || failure.startsWith("SANITIZED_LOCAL TOOL_ARGUMENTS_INVALID ")
            || failure.startsWith("Provider response ended before a complete assistant turn [SANITIZED_LOCAL ")
            || failure=="The model reused a tool-call ID from an earlier turn."
            || failure=="The model requested an unsupported tool."
            || failure=="The model returned an invalid or oversized tool response."
            || failure=="The model requested too many simultaneous tools."
            || failure=="Provider tool calls have an unexpected finish reason"
            || failure=="Provider response ended before a complete assistant turn")
            diagnostic.insert("localValidation",failure.left(512));
        QVERIFY2(finished.last().at(0).toString()==QString("completed"),
                 QJsonDocument(diagnostic).toJson(QJsonDocument::Compact).constData());
        QVERIFY(!usage.isEmpty());
        QVERIFY(!rejectedProposal); QCOMPARE(approvals, 1); QVERIFY(!stageJobId.isEmpty());
        agent->setProviderSettings(previousSettings); apiKey.clear();
        disconnect(service, nullptr, &observationScope, nullptr);
        disconnect(agent->taskRunner(), nullptr, &observationScope, nullptr);

        const auto stageJob = rpc("jobs.query", {{"jobId", stageJobId}}).value("job").toObject();
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QCOMPARE(stageJob.value("state").toString(), QString("succeeded"));
        const auto staged = stageJob.value("result").toObject();
        QVERIFY(sha256(staged.value("bundleHash")));
        const auto candidate = rpc("extension.candidate.get", {{"candidateId", staged.value("candidateId")}}).value("candidate").toObject();
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QCOMPARE(candidate.value("bundleHash"), staged.value("bundleHash"));
        QCOMPARE(candidate.value("manifest").toObject().value("sdkVersion").toString(), QString("range.extension/v2"));
        const auto source = candidate.value("source").toString();
        QVERIFY(!source.isEmpty());
        QVERIFY(!source.contains("range_workflow") && !source.contains("scientific_reference")
                && !source.contains("scientific_example"));
        const auto cases = candidate.value("tests").toObject().value("cases").toArray();
        QVERIFY(cases.size() >= 1 && cases.size() <= 4);
        QVERIFY(typedInspectionCompleted);
        for (const auto &value : cases) {
            const auto testCase = value.toObject();
            bool inspectionMatched = false;
            for (const auto &inspectionValue : inspectedInputs) {
                const auto inspection = inspectionValue.toObject();
                if (testCase.value("input") != inspection.value("normalizedInput")) continue;
                QCOMPARE(testCase.value("expected").toObject().value("inputHash"), inspection.value("inputHash"));
                QCOMPARE(testCase.value("expected").toObject().value("contractHash"), inspection.value("contractHash"));
                QCOMPARE(inspection.value("scope").toString(), QString("caller-provided-test-input-binding"));
                QVERIFY(!inspection.value("nativeProvenanceVerified").toBool());
                inspectionMatched = true;
            }
            QVERIFY2(inspectionMatched, "Each staged analytic case must exactly match an observed typed inspection result.");
            const int count = value.toObject().value("input").toObject().value("times").toArray().size();
            QVERIFY(count >= 2 && count <= 32);
        }

        const auto execute = [&](const QString &command, const QJsonObject &parameters) {
            const auto project = rpc("project.query");
            if (!rpcError.isEmpty()) return QJsonObject{};
            const auto preview = rpc("command.preview", {{"command", command}, {"parameters", parameters}, {"expectedRevision", project.value("revision")}});
            if (!rpcError.isEmpty()) return QJsonObject{};
            const auto approved = rpc("host.approve", {{"previewId", preview.value("previewId")}, {"previewHash", preview.value("previewHash")}});
            if (!rpcError.isEmpty()) return QJsonObject{};
            const auto submitted = rpc("command.execute", {{"previewId", preview.value("previewId")}, {"approvalToken", approved.value("approvalToken")}});
            if (!rpcError.isEmpty()) return QJsonObject{};
            QElapsedTimer deadline; deadline.start();
            while (deadline.elapsed() < 240000) {
                const auto job = rpc("jobs.query", {{"jobId", submitted.value("jobId")}}).value("job").toObject();
                if (!rpcError.isEmpty()) return QJsonObject{};
                const auto state = job.value("state").toString();
                if (state != "queued" && state != "running" && state != "cancelling") return job;
                QTest::qWait(100);
            }
            rpcError = "JOB_TIMEOUT"; return QJsonObject{};
        };
        const QJsonObject identity{{"candidateId", staged.value("candidateId")}, {"bundleHash", staged.value("bundleHash")}};
        const auto validation = execute("extension.validate", identity);
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QCOMPARE(validation.value("state").toString(), QString("succeeded"));
        const auto validated = validation.value("result").toObject();
        QVERIFY(validated.value("validation").toObject().value("passed").toBool());
        QCOMPARE(validated.value("validation").toObject().value("independentCaseCount").toInt(), 4);
        QCOMPARE(validated.value("validation").toObject().value("caseCount").toInt(), cases.size());
        QCOMPARE(validated.value("runtimeHash"), runtime.value("runtimeHash"));
        const auto activation = execute("extension.activate", identity);
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QCOMPARE(activation.value("state").toString(), QString("succeeded"));
        const auto run = execute("extension.run", {{"extensionId", extensionId}, {"bundleHash", staged.value("bundleHash")},
                                                   {"sources", sources}, {"options", fixture.value("options")}});
        QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
        QCOMPARE(run.value("state").toString(), QString("succeeded"));
        const auto result = run.value("result").toObject();
        QCOMPARE(result.value("bundleHash"), staged.value("bundleHash"));
        QCOMPARE(result.value("runtimeHash"), runtime.value("runtimeHash"));
        QCOMPARE(result.value("contractHash"), fixture.value("contractHash"));
        QCOMPARE(result.value("sourceKind").toString(), QString("verified-multichannel"));
        const auto artifact = result.value("scientificResult").toObject();
        QVERIFY(sha256(artifact.value("sha256")));
        const auto artifactPath = QDir(workspace).absoluteFilePath(artifact.value("path").toString());
        QVERIFY(QFileInfo(artifactPath).canonicalFilePath().startsWith(QFileInfo(workspace).canonicalFilePath() + "/"));
        QFile output(artifactPath); QVERIFY(output.open(QIODevice::ReadOnly));
        const auto raw = output.readAll();
        QVERIFY(QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex()) == artifact.value("sha256").toString());
        const auto actual = QJsonDocument::fromJson(raw).object();
        QCOMPARE(actual.value("inputHash"), result.value("seriesSha256"));
        QCOMPARE(actual.value("sampleCount").toInt(), 20);
        QCOMPARE(actual.value("channels").toArray().size(), 2);
        const auto expected = fixture.value("expected").toObject();
        QCOMPARE(actual.value("times"), expected.value("times"));
        for (int channel = 0; channel < 2; ++channel) {
            const auto a = actual.value("channels").toArray().at(channel).toObject();
            const auto e = expected.value("channels").toArray().at(channel).toObject();
            QCOMPARE(a.value("unit"), e.value("unit")); QCOMPARE(a.value("psdUnit"), e.value("psdUnit"));
            for (const auto &field : {"filteredValues", "amplitude", "psd"}) {
                const auto av = a.value(field).toArray(), ev = e.value(field).toArray();
                QCOMPARE(av.size(), ev.size());
                for (int point = 0; point < av.size(); ++point)
                    QVERIFY(qAbs(av.at(point).toDouble() - ev.at(point).toDouble()) <= 1e-9 + 1e-8 * qAbs(ev.at(point).toDouble()));
            }
            const auto page = rpc("extension.result.query", {{"jobId", run.value("jobId")}, {"channel", channel}, {"view", "amplitude"}, {"limit", 32}});
            QVERIFY2(rpcError.isEmpty(), qPrintable(rpcError));
            QCOMPARE(page.value("resultSha256"), artifact.value("sha256"));
            QCOMPARE(page.value("runtimeHash"), runtime.value("runtimeHash"));
            QCOMPARE(page.value("inputSources").toArray().at(channel).toObject().value("selector"), sources.at(channel));
            QCOMPARE(page.value("yUnit"), a.value("unit"));
        }
        int tokens = 0;
        for (const auto &entry : usage) tokens += entry.at(0).toJsonObject().value("totalTokens").toInt();
        qInfo("Scientific v2 real provider acceptance: requests=%d total_tokens=%d independent_cases=4 native_frames=20", int(usage.size()), tokens);
    }
};
}

int runScientificProviderTests(int argc, char **argv)
{
    TestScientificProvider test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_scientific_provider.moc"
