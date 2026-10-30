#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>
#include <QTemporaryDir>

#include <memory>

#include <rtrusted_experiment_service.h>
#include <rtrusted_convergence_service.h>
#include <rtrusted_mechanics_service.h>

namespace
{
QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject quantity(double value, const char *unit)
{
    return {{QStringLiteral("value"), value}, {QStringLiteral("unit"), QString::fromLatin1(unit)}};
}

QJsonArray vector(double x, double y, double z, const char *unit)
{
    return {quantity(x, unit), quantity(y, unit), quantity(z, unit)};
}

QJsonObject rodSpec()
{
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("modelType"), QStringLiteral("axialRod")},
            {QStringLiteral("name"), QStringLiteral("experiment fixture")},
            {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("length"), quantity(1.0, "m")},
                                                       {QStringLiteral("segments"), 4}}},
            {QStringLiteral("material"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Steel")},
                                                       {QStringLiteral("source"), QStringLiteral("fixture")},
                                                       {QStringLiteral("youngModulus"), quantity(210e9, "Pa")},
                                                       {QStringLiteral("poissonRatio"), 0.3},
                                                       {QStringLiteral("density"), quantity(7850.0, "kg/m3")}}},
            {QStringLiteral("section"), QJsonObject{{QStringLiteral("area"), quantity(1e-4, "m2")}}},
            {QStringLiteral("analysis"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("static")}}},
            {QStringLiteral("supports"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("fixedEnd")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("ux"), QStringLiteral("uy"), QStringLiteral("uz")}}},
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
                             {QStringLiteral("dofs"), QJsonArray{QStringLiteral("uy"), QStringLiteral("uz")}}}}},
            {QStringLiteral("loads"), QJsonArray{
                 QJsonObject{{QStringLiteral("kind"), QStringLiteral("pointForce")},
                             {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("components"), vector(1000, 0, 0, "N")}}}},
            {QStringLiteral("initialConditions"), QJsonArray{}},
            {QStringLiteral("observables"), QJsonArray{
                 QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("freeEnd")}}},
                             {QStringLiteral("quantity"), QStringLiteral("displacement")},
                             {QStringLiteral("component"), QStringLiteral("ux")}}}},
            {QStringLiteral("precision"), QJsonObject{{QStringLiteral("displacementRelative"), 0.01}}}};
}

QJsonObject transientSpec(int steps)
{
    QJsonObject spec = rodSpec();
    spec.insert(QStringLiteral("analysis"), QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("linearTransient")},
        {QStringLiteral("timeStep"), quantity(0.01, "s")},
        {QStringLiteral("steps"), steps},
        {QStringLiteral("damping"), QJsonObject{
             {QStringLiteral("rayleighAlpha"), quantity(0.0, "1/s")},
             {QStringLiteral("rayleighBeta"), quantity(0.0, "s")}}}});
    spec.insert(QStringLiteral("precision"), QJsonObject{{QStringLiteral("responseRelative"), 0.01}});
    spec.insert(QStringLiteral("initialConditions"), QJsonArray{QJsonObject{
        {QStringLiteral("type"), QStringLiteral("displacement")},
        {QStringLiteral("selector"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("allNodes")}}},
        {QStringLiteral("components"), vector(0.0, 0.0, 0.0, "m")}}});
    return spec;
}

RTrustedExperimentService::SourceIdentity sourceIdentity()
{
    RTrustedExperimentService::SourceIdentity source;
    source.workspaceId = QStringLiteral("workspace-1");
    source.sessionId = QStringLiteral("session-1");
    source.sessionEpoch = QStringLiteral("epoch-1");
    source.documentId = QStringLiteral("document-1");
    source.documentRevision = QStringLiteral("7");
    source.snapshotSha256 = sha256(QByteArrayLiteral("source snapshot"));
    return source;
}

QJsonObject experimentRequest(const RTrustedExperimentService::SourceIdentity &source,
                              const QJsonObject &problem = rodSpec())
{
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("sourceBinding"), source.toJson()},
            {QStringLiteral("problemSpec"), problem},
            {QStringLiteral("allowedNumericalRanges"), QJsonArray{
                 QJsonObject{{QStringLiteral("path"), QStringLiteral("/geometry/segments")},
                             {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 32}}}},
            {QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 8},
                                                      {QStringLiteral("repairs"), 3},
                                                      {QStringLiteral("modelTurns"), 12}}}};
}

QString authorizeAndRun(RTrustedExperimentService *service,
                        const RTrustedExperimentService::SourceIdentity &source)
{
    const auto authorization = service->authorize(experimentRequest(source), source);
    if (!authorization.ok) return {};
    const QString id = authorization.data.value(QStringLiteral("experimentId")).toString();
    if (!service->create(id, source).ok || !service->run(id, source).ok) return {};
    return id;
}

QJsonObject withSegments(QJsonObject spec, int segments)
{
    QJsonObject geometry = spec.value(QStringLiteral("geometry")).toObject();
    geometry.insert(QStringLiteral("segments"), segments);
    spec.insert(QStringLiteral("geometry"), geometry);
    return spec;
}

QJsonObject observedPhysical(const QJsonObject &problem)
{
    const auto validation = RTrustedMechanicsService::validate(problem);
    return RTrustedMechanicsService::physicalSummary(validation.normalizedSpec);
}

QJsonObject solveIdentity(const QString &inputSha)
{
    return {{QStringLiteral("artifactId"), QStringLiteral("artifact-1")},
            {QStringLiteral("jobId"), QStringLiteral("job-1")},
            {QStringLiteral("inputSnapshotSha256"), inputSha},
            {QStringLiteral("inputPackageSha256"), sha256(QByteArrayLiteral("package"))},
            {QStringLiteral("sourceSnapshotRef"), QJsonObject{
                 {QStringLiteral("projectId"), QStringLiteral("project-1")},
                 {QStringLiteral("sourceRevision"), QStringLiteral("7")},
                 {QStringLiteral("modelSha256"), sha256(QByteArrayLiteral("candidate model"))}}}};
}
}

class TrustedExperimentServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        temporaryDirectory.reset(new QTemporaryDir);
        QVERIFY(temporaryDirectory->isValid());
        service.reset(new RTrustedExperimentService(temporaryDirectory->path()));
        source = sourceIdentity();
    }

    void lifecycleStateContractIsCanonical()
    {
        const QStringList &states = RTrustedExperimentService::lifecycleStates();
        QCOMPARE(states.size(), 12);
        QSet<QString> uniqueStates;
        for (const QString &state : states)
            uniqueStates.insert(state);
        QCOMPARE(states.size(), uniqueStates.size());
        for (const QString &state : states)
            QVERIFY2(!state.isEmpty(), qPrintable(state));
    }

    void authorizationPreviewDoesNotPersistUntilCommitted()
    {
        const QJsonObject request = experimentRequest(source);
        const auto preview = service->previewAuthorization(request, source);
        QVERIFY2(preview.ok, qPrintable(preview.code + QStringLiteral(": ") + preview.message));
        const QString id = preview.data.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(preview.data.value(QStringLiteral("state")).toString(), QStringLiteral("preview"));
        QCOMPARE(preview.data.value(QStringLiteral("problemSpecInput")).toObject(),
                 request.value(QStringLiteral("problemSpec")).toObject());
        QVERIFY(!preview.data.value(QStringLiteral("problemSpec")).toObject().isEmpty());
        QVERIFY(!service->query(id).ok);
        QVERIFY(QDir(temporaryDirectory->path())
                    .entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());

        QJsonObject tamperedAuthorization = preview.data.value(QStringLiteral("authorization")).toObject();
        tamperedAuthorization.insert(QStringLiteral("physicalSummaryDigest"), QString(64, QLatin1Char('0')));
        const auto rejected = service->authorizePreview(request, source, id, tamperedAuthorization);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("EXPERIMENT_PREVIEW_MISMATCH"));
        QVERIFY(!service->query(id).ok);
        QVERIFY(QDir(temporaryDirectory->path())
                    .entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());

        const auto committed = service->authorizePreview(
            request, source, id, preview.data.value(QStringLiteral("authorization")).toObject());
        QVERIFY2(committed.ok, qPrintable(committed.code + QStringLiteral(": ") + committed.message));
        QCOMPARE(committed.data.value(QStringLiteral("experimentId")).toString(), id);
        QCOMPARE(committed.data.value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
        QCOMPARE(committed.data.value(QStringLiteral("authorization")).toObject(),
                 preview.data.value(QStringLiteral("authorization")).toObject());
        const auto durable = service->query(id);
        QVERIFY(durable.ok);
        const QJsonObject record = durable.data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
        QCOMPARE(record.value(QStringLiteral("authorization")).toObject(),
                 preview.data.value(QStringLiteral("authorization")).toObject());
        QCOMPARE(record.value(QStringLiteral("problemSpecInput")).toObject(),
                 request.value(QStringLiteral("problemSpec")).toObject());
        QCOMPARE(record.value(QStringLiteral("problemSpec")).toObject(),
                 preview.data.value(QStringLiteral("problemSpec")).toObject());
        const QJsonArray history = record.value(QStringLiteral("history")).toArray();
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toObject().value(QStringLiteral("event")).toString(), QStringLiteral("authorized"));
        QCOMPARE(history.at(1).toObject().value(QStringLiteral("event")).toString(), QStringLiteral("created"));
        QCOMPARE(QDir(temporaryDirectory->path())
                     .entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    }

    void authorizationPreviewCommitRetriesAfterStorageLockFailure()
    {
        const QJsonObject request = experimentRequest(source);
        const auto preview = service->previewAuthorization(request, source);
        QVERIFY2(preview.ok, qPrintable(preview.code + QStringLiteral(": ") + preview.message));
        const QString id = preview.data.value(QStringLiteral("experimentId")).toString();
        const QJsonObject authorization = preview.data.value(QStringLiteral("authorization")).toObject();

        QLockFile competingLock(temporaryDirectory->filePath(id + QStringLiteral(".json.lock")));
        competingLock.setStaleLockTime(0);
        QVERIFY(competingLock.tryLock(0));
        const auto failedCommit = service->authorizePreview(request, source, id, authorization);
        QVERIFY(!failedCommit.ok);
        QCOMPARE(failedCommit.code, QStringLiteral("EXPERIMENT_STORAGE_LOCK_FAILED"));
        QVERIFY(!service->query(id).ok);
        QVERIFY(QDir(temporaryDirectory->path())
                    .entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());
        competingLock.unlock();

        const auto retriedCommit = service->authorizePreview(request, source, id, authorization);
        QVERIFY2(retriedCommit.ok, qPrintable(retriedCommit.code + QStringLiteral(": ") + retriedCommit.message));
        QCOMPARE(retriedCommit.data.value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
        QCOMPARE(retriedCommit.data.value(QStringLiteral("authorization")).toObject(), authorization);

        const auto repeatedCommit = service->authorizePreview(request, source, id, authorization);
        QVERIFY(!repeatedCommit.ok);
        QCOMPARE(repeatedCommit.code, QStringLiteral("EXPERIMENT_ALREADY_EXISTS"));
        QCOMPARE(service->query(id).data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("prepared"));
    }

    void limitsAndStrictFields()
    {
        QJsonObject request = experimentRequest(source);
        request.remove(QStringLiteral("budgets"));
        auto result = service->authorize(request, source);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("EXPERIMENT_SPEC_INVALID"));

        request = experimentRequest(source);
        QJsonObject budgets = request.value(QStringLiteral("budgets")).toObject();
        budgets.insert(QStringLiteral("solves"), RTrustedExperimentService::HardSolveLimit + 1);
        request.insert(QStringLiteral("budgets"), budgets);
        result = service->authorize(request, source);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("BUDGET_INVALID"));

        request = experimentRequest(source);
        request.insert(QStringLiteral("authorized"), true);
        result = service->authorize(request, source);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("EXPERIMENT_SPEC_INVALID"));

        request = experimentRequest(source);
        budgets = request.value(QStringLiteral("budgets")).toObject();
        budgets.insert(QStringLiteral("repairs"), RTrustedExperimentService::HardRepairLimit + 1);
        request.insert(QStringLiteral("budgets"), budgets);
        result = service->authorize(request, source);
        QVERIFY(!result.ok);

        request = experimentRequest(source);
        budgets = request.value(QStringLiteral("budgets")).toObject();
        budgets.insert(QStringLiteral("modelTurns"), RTrustedExperimentService::HardModelTurnLimit + 1);
        request.insert(QStringLiteral("budgets"), budgets);
        result = service->authorize(request, source);
        QVERIFY(!result.ok);
    }

    void sourceBindingIsRequiredAtAuthorizationAndCreate()
    {
        auto stale = source;
        stale.documentRevision = QStringLiteral("8");
        auto result = service->authorize(experimentRequest(source), stale);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("SOURCE_STALE"));

        result = service->authorize(experimentRequest(source), source);
        QVERIFY(result.ok);
        const QString id = result.data.value(QStringLiteral("experimentId")).toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(!service->create(id, stale).ok);
        const auto snapshot = service->query(id);
        QVERIFY(snapshot.ok);
        QCOMPARE(snapshot.data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QVERIFY(snapshot.data.value(QStringLiteral("experiment")).toObject()
                    .value(QStringLiteral("sourceInvalidated")).toBool());
    }

    void runIsIdempotentAndDurable()
    {
        const auto authorized = service->authorize(experimentRequest(source), source);
        QVERIFY(authorized.ok);
        const QString id = authorized.data.value(QStringLiteral("experimentId")).toString();
        QJsonObject authorization = authorized.data.value(QStringLiteral("authorization")).toObject();
        QCOMPARE(authorization.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("range.trusted-experiment-authorization/v1"));
        const QString authorizationDigest = authorization.take(QStringLiteral("authorizationDigest")).toString();
        QCOMPARE(authorizationDigest, sha256(QJsonDocument(authorization).toJson(QJsonDocument::Compact)));
        QVERIFY(service->create(id, source).ok);
        const auto first = service->run(id, source);
        QVERIFY(first.ok);
        QCOMPARE(first.data.value(QStringLiteral("state")).toString(), QStringLiteral("diagnosing"));
        QCOMPARE(first.data.value(QStringLiteral("idempotent")).toBool(), false);
        const auto second = service->run(id, source);
        QVERIFY(second.ok);
        QCOMPARE(second.data.value(QStringLiteral("state")).toString(), QStringLiteral("diagnosing"));
        QCOMPARE(second.data.value(QStringLiteral("idempotent")).toBool(), true);

        RTrustedExperimentService reopened(temporaryDirectory->path());
        const auto afterRestart = reopened.query(id);
        QVERIFY(afterRestart.ok);
        QCOMPARE(afterRestart.data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("diagnosing"));
    }

    void cancellationIsMonotonic()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        auto result = service->cancel(id);
        QVERIFY(result.ok);
        QCOMPARE(result.data.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QVERIFY(!service->run(id, source).ok);
        QVERIFY(!service->beginModelTurn(id, source).ok);
        result = service->cancel(id);
        QVERIFY(result.ok);
        QCOMPARE(result.data.value(QStringLiteral("idempotent")).toBool(), true);
        const auto snapshot = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(snapshot.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(snapshot.value(QStringLiteral("cancelRequested")).toBool(), true);
    }

    void cancellationClosesAnActiveSolveAttempt()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const QString inputSha = sha256(QByteArrayLiteral("input"));
        const QString packageSha = sha256(QByteArrayLiteral("package"));
        const auto started = service->beginSolve(id, source, rodSpec(), inputSha, packageSha);
        QVERIFY2(started.ok, qPrintable(started.message));
        const QString attemptId = started.data.value(QStringLiteral("attemptId")).toString();

        QVERIFY(service->cancel(id).ok);
        const QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("cancelled"));
        QVERIFY(record.value(QStringLiteral("activeAttempt")).toObject().isEmpty());
        const QJsonArray attempts = record.value(QStringLiteral("attempts")).toArray();
        QCOMPARE(attempts.size(), 1);
        const QJsonObject attempt = attempts.first().toObject();
        QCOMPARE(attempt.value(QStringLiteral("attemptId")).toString(), attemptId);
        QCOMPARE(attempt.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
        QVERIFY(!attempt.value(QStringLiteral("finishedAtUtc")).toString().isEmpty());
    }

    void staleSourceClosesAnActiveSolveAttempt()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const auto started = service->beginSolve(id, source, rodSpec(),
            sha256(QByteArrayLiteral("input")), sha256(QByteArrayLiteral("package")));
        QVERIFY(started.ok);
        const QString attemptId = started.data.value(QStringLiteral("attemptId")).toString();
        auto staleSource = source;
        staleSource.documentRevision = QStringLiteral("8");

        const auto completion = service->finishSolve(id, staleSource, attemptId, false, {}, QStringLiteral("stop"),
            QStringLiteral("SOLVE_FAILED"), QStringLiteral("The child ended after its source changed."));
        QVERIFY(!completion.ok);
        QCOMPARE(completion.code, QStringLiteral("SOURCE_STALE"));
        const QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("failed"));
        QVERIFY(record.value(QStringLiteral("activeAttempt")).toObject().isEmpty());
        const QJsonObject attempt = record.value(QStringLiteral("attempts")).toArray().first().toObject();
        QCOMPARE(attempt.value(QStringLiteral("status")).toString(), QStringLiteral("source_invalidated"));
        QVERIFY(!attempt.value(QStringLiteral("finishedAtUtc")).toString().isEmpty());
    }

    void submissionClaimIsDurableAndOneShot()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const auto started = service->beginSolve(id, source, rodSpec(),
            sha256(QByteArrayLiteral("input")), sha256(QByteArrayLiteral("package")));
        QVERIFY(started.ok);
        const QString attemptId = started.data.value(QStringLiteral("attemptId")).toString();
        const auto claimed = service->claimSolveSubmission(id, source, attemptId);
        QVERIFY2(claimed.ok, qPrintable(claimed.message));

        RTrustedExperimentService reopened(temporaryDirectory->path());
        const QJsonObject durableAttempt = reopened.query(id).data.value(QStringLiteral("experiment")).toObject()
            .value(QStringLiteral("activeAttempt")).toObject();
        QCOMPARE(durableAttempt.value(QStringLiteral("submissionClaimed")).toBool(), true);
        const auto replayed = reopened.claimSolveSubmission(id, source, attemptId);
        QVERIFY(!replayed.ok);
        QCOMPARE(replayed.code, QStringLiteral("ATTEMPT_ALREADY_SUBMITTED"));
    }

    void nonPrecisionVerificationFailuresCannotTriggerMeshRefinement()
    {
        const auto issued = service->authorize(experimentRequest(source), source);
        QVERIFY(issued.ok);
        const QJsonObject authorization = issued.data.value(QStringLiteral("authorization")).toObject();
        const QJsonObject spec = rodSpec();
        const QJsonObject precisionFailure{{QStringLiteral("required"), true},
                                           {QStringLiteral("status"), QStringLiteral("fail")},
                                           {QStringLiteral("evidence"), QJsonObject{
                                                {QStringLiteral("maximumRelativeError"), 0.5},
                                                {QStringLiteral("effectiveRelativeLimit"), 0.01}}}};
        const QJsonObject identityFailure{{QStringLiteral("required"), true},
                                          {QStringLiteral("status"), QStringLiteral("fail")}};
        const QJsonObject identityReport{{QStringLiteral("status"), QStringLiteral("fail")},
                                        {QStringLiteral("metrics"), QJsonObject{
                                             {QStringLiteral("displacement"), precisionFailure},
                                             {QStringLiteral("identity"), identityFailure}}}};
        QJsonObject refined;
        QVERIFY(!RTrustedExperimentService::proposeAuthorizedMeshRefinement(
            spec, authorization, identityReport, &refined));

        const QJsonObject notComputed{{QStringLiteral("required"), true},
                                      {QStringLiteral("status"), QStringLiteral("notComputed")}};
        const QJsonObject incompleteReport{{QStringLiteral("status"), QStringLiteral("fail")},
                                           {QStringLiteral("metrics"), QJsonObject{
                                                {QStringLiteral("displacement"), precisionFailure},
                                                {QStringLiteral("strainEnergy"), notComputed}}}};
        QVERIFY(!RTrustedExperimentService::proposeAuthorizedMeshRefinement(
            spec, authorization, incompleteReport, &refined));
    }

    void modelTurnBudgetIsConsumedBeforeTurn()
    {
        QJsonObject request = experimentRequest(source);
        request.insert(QStringLiteral("budgets"), QJsonObject{{QStringLiteral("solves"), 1},
                                                               {QStringLiteral("repairs"), 0},
                                                               {QStringLiteral("modelTurns"), 1}});
        const auto authorization = service->authorize(request, source);
        QVERIFY(authorization.ok);
        const QString id = authorization.data.value(QStringLiteral("experimentId")).toString();
        QVERIFY(service->create(id, source).ok);
        QVERIFY(service->run(id, source).ok);
        QVERIFY(service->beginModelTurn(id, source).ok);
        const auto overBudget = service->beginModelTurn(id, source);
        QVERIFY(!overBudget.ok);
        QCOMPARE(overBudget.code, QStringLiteral("BUDGET_EXHAUSTED"));
        const QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("state")).toString(), QStringLiteral("budget_exhausted"));
        QCOMPARE(record.value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("modelTurns")).toInt(), 1);
    }

    void problemSpecSizeIsBoundedForAuthorizationAndRepair()
    {
        QJsonObject oversizedProblem = rodSpec();
        oversizedProblem.insert(QStringLiteral("name"), QString(64 * 1024, QLatin1Char('x')));
        const auto rejectedAuthorization = service->authorize(
            experimentRequest(source, oversizedProblem), source);
        QVERIFY(!rejectedAuthorization.ok);
        QCOMPARE(rejectedAuthorization.code, QStringLiteral("PROBLEM_SPEC_TOO_LARGE"));
        QVERIFY(QDir(temporaryDirectory->path()).entryList(
                    {QStringLiteral("*.json")}, QDir::Files).isEmpty());

        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const QString inputSha = sha256(QByteArrayLiteral("input"));
        const auto started = service->beginSolve(id, source, rodSpec(), inputSha,
                                                 sha256(QByteArrayLiteral("package")));
        QVERIFY(started.ok);
        QVERIFY(service->finishSolve(id, source,
            started.data.value(QStringLiteral("attemptId")).toString(), false, {},
            QStringLiteral("numericRepairAllowed"), QStringLiteral("PRECISION"),
            QStringLiteral("Repair fixture.")).ok);

        QJsonObject oversizedRepair = withSegments(rodSpec(), 5);
        oversizedRepair.insert(QStringLiteral("name"), QString(64 * 1024, QLatin1Char('x')));
        const auto rejectedRepair = service->beginRepair(id, source, oversizedRepair);
        QVERIFY(!rejectedRepair.ok);
        QCOMPARE(rejectedRepair.code, QStringLiteral("PROBLEM_SPEC_TOO_LARGE"));
        const QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("repairs")).toInt(), 0);
        QVERIFY(!record.contains(QStringLiteral("pendingRepairProblemSpec")));
    }

    void transientPreflightRejectsOversizedRecordBeforeAuthorization()
    {
        QJsonObject problem = transientSpec(9999);
        QJsonArray observables = problem.value(QStringLiteral("observables")).toArray();
        while (observables.size() < 16) observables.append(observables.first());
        problem.insert(QStringLiteral("observables"), observables);
        QJsonObject request = experimentRequest(source, problem);
        request.insert(QStringLiteral("allowedNumericalRanges"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), QStringLiteral("/geometry/segments")},
            {QStringLiteral("minimum"), 4}, {QStringLiteral("maximum"), 32}}});
        request.insert(QStringLiteral("convergenceStudy"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("mesh")},
            {QStringLiteral("maximumLevels"), 4}});

        const auto rejected = service->authorize(request, source);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("STUDY_RECORD_SIZE_LIMIT"));
        QVERIFY(QDir(temporaryDirectory->path()).entryList(
                    {QStringLiteral("*.json")}, QDir::Files).isEmpty());
    }

    void convergenceRangesAreExactAndRepairsCannotLeaveThePlan()
    {
        QJsonObject request = experimentRequest(source);
        request.insert(QStringLiteral("convergenceStudy"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("mesh")},
            {QStringLiteral("maximumLevels"), 3}});
        auto rejected = service->authorize(request, source);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("CONVERGENCE_RANGE_NOT_AUTHORIZED"));
        QVERIFY(QDir(temporaryDirectory->path()).entryList(
                    {QStringLiteral("*.json")}, QDir::Files).isEmpty());

        request.insert(QStringLiteral("allowedNumericalRanges"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), QStringLiteral("/geometry/segments")},
            {QStringLiteral("minimum"), 4}, {QStringLiteral("maximum"), 16}}});
        const auto authorized = service->authorize(request, source);
        QVERIFY2(authorized.ok, qPrintable(authorized.code + QStringLiteral(": ") + authorized.message));
        const QString id = authorized.data.value(QStringLiteral("experimentId")).toString();
        QVERIFY(service->create(id, source).ok);
        QVERIFY(service->run(id, source).ok);

        const QString inputSha = sha256(QByteArrayLiteral("convergence-plan input"));
        auto started = service->beginSolve(id, source, rodSpec(), inputSha,
                                           sha256(QByteArrayLiteral("convergence-plan package")));
        QVERIFY2(started.ok, qPrintable(started.code + QStringLiteral(": ") + started.message));
        QVERIFY(service->finishSolve(id, source, started.data.value(QStringLiteral("attemptId")).toString(),
                                     false, {}, QStringLiteral("numericRepairAllowed"),
                                     QStringLiteral("PRECISION"), QStringLiteral("Need a finer planned mesh." )).ok);

        const QJsonObject before = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        for (const int segments : {5, 32}) {
            const auto overPlan = service->beginRepair(id, source, withSegments(rodSpec(), segments));
            QVERIFY(!overPlan.ok);
            QCOMPARE(overPlan.code, QStringLiteral("CONVERGENCE_PLAN_BOUNDS_EXCEEDED"));
            const QJsonObject after = service->query(id).data.value(QStringLiteral("experiment")).toObject();
            QCOMPARE(RTrustedRecordSerialization::compact(after),
                     RTrustedRecordSerialization::compact(before));
        }

        const QJsonObject planned = withSegments(rodSpec(), 8);
        const auto repaired = service->beginRepair(id, source, planned);
        QVERIFY2(repaired.ok, qPrintable(repaired.code + QStringLiteral(": ") + repaired.message));
        QVERIFY(service->finishRepair(id, source, planned, observedPhysical(planned)).ok);
        started = service->beginSolve(id, source, planned, sha256(QByteArrayLiteral("planned level input")),
                                      sha256(QByteArrayLiteral("convergence-plan package")));
        QVERIFY2(started.ok, qPrintable(started.code + QStringLiteral(": ") + started.message));
        const QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("solves")).toInt(), 2);
        QCOMPARE(record.value(QStringLiteral("counters")).toObject()
                     .value(QStringLiteral("repairs")).toInt(), 1);
    }

    void timeStepRangesContainPlanEndpointsAndAcceptPlannedRepair()
    {
        const QJsonObject problem = transientSpec(10);
        const QJsonObject study{{QStringLiteral("kind"), QStringLiteral("timeStep")},
                                {QStringLiteral("maximumLevels"), 3}};
        const QJsonArray exactRanges{
            QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/timeStep")},
                        {QStringLiteral("minimum"), 0.0025}, {QStringLiteral("maximum"), 0.01}},
            QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/steps")},
                        {QStringLiteral("minimum"), 10}, {QStringLiteral("maximum"), 40}}};

        for (const double inwardMinimum : {0.002500000000005, 0.0025}) {
            for (const double inwardMaximum : {0.01, 0.009999999999995}) {
                if (inwardMinimum == 0.0025 && inwardMaximum == 0.01) continue;
                QJsonObject request = experimentRequest(source, problem);
                request.insert(QStringLiteral("allowedNumericalRanges"), QJsonArray{
                    QJsonObject{{QStringLiteral("path"), QStringLiteral("/analysis/timeStep")},
                        {QStringLiteral("minimum"), inwardMinimum}, {QStringLiteral("maximum"), inwardMaximum}},
                    exactRanges.at(1)});
                request.insert(QStringLiteral("convergenceStudy"), study);
                const auto rejected = service->authorize(request, source);
                QVERIFY(!rejected.ok);
                const QString expectedCode = inwardMaximum < 0.01
                    ? QStringLiteral("NUMERICAL_RANGE_INVALID")
                    : QStringLiteral("CONVERGENCE_RANGE_NOT_AUTHORIZED");
                QCOMPARE(rejected.code, expectedCode);
            }
        }

        QJsonObject request = experimentRequest(source, problem);
        request.insert(QStringLiteral("allowedNumericalRanges"), exactRanges);
        request.insert(QStringLiteral("convergenceStudy"), study);
        const auto authorized = service->authorize(request, source);
        QVERIFY2(authorized.ok, qPrintable(authorized.code + QStringLiteral(": ") + authorized.message));
        const QString id = authorized.data.value(QStringLiteral("experimentId")).toString();
        QVERIFY(service->create(id, source).ok);
        QVERIFY(service->run(id, source).ok);
        const QString inputSha = sha256(QByteArrayLiteral("time-step-plan input"));
        auto started = service->beginSolve(id, source, problem, inputSha,
                                           sha256(QByteArrayLiteral("time-step-plan package")));
        QVERIFY2(started.ok, qPrintable(started.code + QStringLiteral(": ") + started.message));
        QVERIFY(service->finishSolve(id, source, started.data.value(QStringLiteral("attemptId")).toString(),
                                     false, {}, QStringLiteral("numericRepairAllowed"),
                                     QStringLiteral("PRECISION"), QStringLiteral("Use the next planned time step.")).ok);

        QJsonObject plannedRepair = problem;
        QJsonObject analysis = plannedRepair.value(QStringLiteral("analysis")).toObject();
        analysis.insert(QStringLiteral("timeStep"), quantity(0.005, "s"));
        analysis.insert(QStringLiteral("steps"), 20);
        plannedRepair.insert(QStringLiteral("analysis"), analysis);
        const auto repaired = service->beginRepair(id, source, plannedRepair);
        QVERIFY2(repaired.ok, qPrintable(repaired.code + QStringLiteral(": ") + repaired.message));
        QVERIFY(service->finishRepair(id, source, plannedRepair, observedPhysical(plannedRepair)).ok);
        started = service->beginSolve(id, source, plannedRepair,
                                      sha256(QByteArrayLiteral("time-step level input")),
                                      sha256(QByteArrayLiteral("time-step-plan package")));
        QVERIFY2(started.ok, qPrintable(started.code + QStringLiteral(": ") + started.message));
    }

    void largeTransientStudyWithEscapedIdentitiesPersistsWithinLimit()
    {
        source.workspaceId = QStringLiteral("w") + QString(255, QChar(0x0001));
        source.sessionId = QStringLiteral("s") + QString(255, QChar(0x0000));
        source.sessionEpoch = QStringLiteral("e") + QString(255, QChar(0x2028));
        source.documentId = QStringLiteral("d") + QString(255, QChar(0x0001));
        source.documentRevision = QStringLiteral("r") + QString(63, QChar(0x0001));

        QJsonObject problem = transientSpec(7500);
        problem.insert(QStringLiteral("name"), QString::fromUtf8("Rod雪")
                        + QChar(0x0001) + QChar(0x0000) + QChar(0x2028));
        QJsonObject request = experimentRequest(source, problem);
        request.insert(QStringLiteral("allowedNumericalRanges"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), QStringLiteral("/geometry/segments")},
            {QStringLiteral("minimum"), 4}, {QStringLiteral("maximum"), 32}}});
        request.insert(QStringLiteral("convergenceStudy"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("mesh")},
            {QStringLiteral("maximumLevels"), 4}});

        const auto authorized = service->authorize(request, source);
        QVERIFY2(authorized.ok, qPrintable(authorized.code + QStringLiteral(": ") + authorized.message));
        const QString id = authorized.data.value(QStringLiteral("experimentId")).toString();
        const QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QVERIFY(!record.isEmpty());
        QFile persisted(temporaryDirectory->filePath(id + QStringLiteral(".json")));
        QVERIFY(persisted.open(QIODevice::ReadOnly));
        const QByteArray persistedBytes = persisted.readAll();
        QVERIFY(persistedBytes.size() <= RTrustedConvergenceService::MaximumDurableRecordBytes);
        QVERIFY(persistedBytes.contains("\\u0001"));
        QVERIFY(persistedBytes.contains("\\u0000"));
    }

    void oversizedAtomicWriteLeavesPriorRecordUnchanged()
    {
        const auto authorized = service->authorize(experimentRequest(source), source);
        QVERIFY(authorized.ok);
        const QString id = authorized.data.value(QStringLiteral("experimentId")).toString();
        const QString path = temporaryDirectory->filePath(id + QStringLiteral(".json"));
        QJsonObject record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        record.insert(QStringLiteral("testPadding"), QString());
        const qsizetype baselineSize = RTrustedRecordSerialization::compact(record).size();
        const qsizetype targetSize = RTrustedConvergenceService::MaximumDurableRecordBytes - 8;
        QVERIFY(targetSize > baselineSize);
        record.insert(QStringLiteral("testPadding"), QString(targetSize - baselineSize, QLatin1Char('x')));
        const QByteArray before = RTrustedRecordSerialization::compact(record);
        QCOMPARE(before.size(), targetSize);
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(seed.write(before), qint64(before.size()));
        seed.close();

        const auto rejected = service->create(id, source);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("EXPERIMENT_STORAGE_FAILED"));
        QFile afterFile(path);
        QVERIFY(afterFile.open(QIODevice::ReadOnly));
        QCOMPARE(afterFile.readAll(), before);
        const auto stillReadable = service->query(id);
        QVERIFY(stillReadable.ok);
        QCOMPARE(stillReadable.data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("authorized"));
    }

    void historyCountAndBytesAreBoundedBeforeAtomicWrite()
    {
        const auto authorized = service->authorize(experimentRequest(source), source);
        QVERIFY(authorized.ok);
        const QString id = authorized.data.value(QStringLiteral("experimentId")).toString();
        const QString path = temporaryDirectory->filePath(id + QStringLiteral(".json"));
        const QJsonObject original = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        const QJsonObject event{{QStringLiteral("atUtc"), QStringLiteral("2026-01-01T00:00:00.000Z")},
            {QStringLiteral("event"), QStringLiteral("test")},
            {QStringLiteral("from"), QString()}, {QStringLiteral("to"), QStringLiteral("test")}};

        QJsonObject excessiveCount = original;
        QJsonArray manyEvents;
        for (int i = 0; i < 96; ++i) manyEvents.append(event);
        excessiveCount.insert(QStringLiteral("history"), manyEvents);
        QByteArray before = RTrustedRecordSerialization::compact(excessiveCount);
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(seed.write(before), qint64(before.size()));
        seed.close();
        auto rejected = service->create(id, source);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("EXPERIMENT_STORAGE_FAILED"));
        QFile afterCount(path);
        QVERIFY(afterCount.open(QIODevice::ReadOnly));
        const QByteArray afterCountBytes = afterCount.readAll();
        afterCount.close();
        QCOMPARE(afterCountBytes, before);

        QJsonObject excessiveBytes = original;
        QJsonObject largeEvent = event;
        largeEvent.insert(QStringLiteral("details"), QJsonObject{
            {QStringLiteral("padding"), QString(384 * 1024, QLatin1Char('x'))}});
        excessiveBytes.insert(QStringLiteral("history"), QJsonArray{largeEvent});
        before = RTrustedRecordSerialization::compact(excessiveBytes);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(seed.write(before), qint64(before.size()));
        seed.close();
        rejected = service->create(id, source);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("EXPERIMENT_STORAGE_FAILED"));
        QFile afterBytes(path);
        QVERIFY(afterBytes.open(QIODevice::ReadOnly));
        const QByteArray persistedBytes = afterBytes.readAll();
        afterBytes.close();
        QCOMPARE(persistedBytes, before);

        QJsonObject excessiveField = original;
        QJsonObject oversizedEvent = event;
        oversizedEvent.insert(QStringLiteral("event"), QString(65, QLatin1Char('x')));
        excessiveField.insert(QStringLiteral("history"), QJsonArray{oversizedEvent});
        before = RTrustedRecordSerialization::compact(excessiveField);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(seed.write(before), qint64(before.size()));
        seed.close();
        rejected = service->create(id, source);
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code, QStringLiteral("EXPERIMENT_STORAGE_FAILED"));
        QFile afterField(path);
        QVERIFY(afterField.open(QIODevice::ReadOnly));
        QCOMPARE(afterField.readAll(), before);
    }

    void transitionsRejectOutOfOrderCalls()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        auto result = service->finishSolve(id, source, QStringLiteral("missing"), false, {},
                                           QStringLiteral("stop"), QStringLiteral("NOPE"), QStringLiteral("out of order"));
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("TRANSITION_INVALID"));
        result = service->beginRepair(id, source, withSegments(rodSpec(), 5));
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("TRANSITION_INVALID"));
        QCOMPARE(service->query(id).data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("diagnosing"));
    }

    void failedConfigurationIsDeduplicated()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const QString inputSha = sha256(QByteArrayLiteral("same input artifact"));
        const QString packageSha = sha256(QByteArrayLiteral("same input package"));
        auto result = service->beginSolve(id, source, rodSpec(), inputSha, packageSha);
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        QString attempt = result.data.value(QStringLiteral("attemptId")).toString();
        QVERIFY(service->finishSolve(id, source, attempt, false, {}, QStringLiteral("numericRepairAllowed"),
                                     QStringLiteral("PRECISION"), QStringLiteral("Need a finer mesh.")).ok);

        const QJsonObject fiveSegments = withSegments(rodSpec(), 5);
        QVERIFY(service->beginRepair(id, source, fiveSegments).ok);
        QVERIFY(service->finishRepair(id, source, fiveSegments, observedPhysical(fiveSegments)).ok);
        result = service->beginSolve(id, source, fiveSegments, inputSha, packageSha);
        QVERIFY(result.ok);
        attempt = result.data.value(QStringLiteral("attemptId")).toString();
        QVERIFY(service->finishSolve(id, source, attempt, false, {}, QStringLiteral("numericRepairAllowed"),
                                     QStringLiteral("PRECISION"), QStringLiteral("Still inaccurate.")).ok);

        QVERIFY(service->beginRepair(id, source, rodSpec()).ok);
        QVERIFY(service->finishRepair(id, source, rodSpec(), observedPhysical(rodSpec())).ok);
        result = service->beginSolve(id, source, rodSpec(), inputSha, packageSha);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("FAILED_CONFIGURATION_REPEATED"));
        QCOMPARE(service->query(id).data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("counters")).toObject().value(QStringLiteral("solves")).toInt(), 2);
    }

    void repairCannotChangePhysics()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        auto result = service->beginSolve(id, source, rodSpec(), sha256(QByteArrayLiteral("input")),
                                          sha256(QByteArrayLiteral("input package")));
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        QVERIFY(service->finishSolve(id, source, result.data.value(QStringLiteral("attemptId")).toString(),
                                     false, {}, QStringLiteral("numericRepairAllowed"),
                                     QStringLiteral("RETRY"), QStringLiteral("Repair test.")).ok);
        QJsonObject changed = rodSpec();
        QJsonArray loads = changed.value(QStringLiteral("loads")).toArray();
        QJsonObject load = loads.first().toObject();
        load.insert(QStringLiteral("components"), vector(2000, 0, 0, "N"));
        loads.replace(0, load);
        changed.insert(QStringLiteral("loads"), loads);
        result = service->beginRepair(id, source, changed);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("REPAIR_NEEDS_CONFIRMATION"));
        QCOMPARE(service->query(id).data.value(QStringLiteral("experiment")).toObject()
                     .value(QStringLiteral("state")).toString(), QStringLiteral("needs_confirmation"));
    }

    void observedPhysicsMismatchDuringRepairRequiresConfirmation()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        auto result = service->beginSolve(id, source, rodSpec(), sha256(QByteArrayLiteral("input")),
                                          sha256(QByteArrayLiteral("input package")));
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        QVERIFY(service->finishSolve(id, source, result.data.value(QStringLiteral("attemptId")).toString(),
                                     false, {}, QStringLiteral("numericRepairAllowed"),
                                     QStringLiteral("RETRY"), QStringLiteral("Repair test.")).ok);

        const QJsonObject repair = withSegments(rodSpec(), 5);
        QVERIFY(service->beginRepair(id, source, repair).ok);

        QJsonObject changedObservation = rodSpec();
        QJsonArray loads = changedObservation.value(QStringLiteral("loads")).toArray();
        QJsonObject load = loads.first().toObject();
        load.insert(QStringLiteral("components"), vector(2000, 0, 0, "N"));
        loads.replace(0, load);
        changedObservation.insert(QStringLiteral("loads"), loads);

        result = service->finishRepair(id, source, repair, observedPhysical(changedObservation));
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("OBSERVED_PHYSICS_MISMATCH"));
        const QJsonObject experiment = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(experiment.value(QStringLiteral("state")).toString(), QStringLiteral("needs_confirmation"));
        QCOMPARE(experiment.value(QStringLiteral("terminalCode")).toString(),
                 QStringLiteral("OBSERVED_PHYSICS_MISMATCH"));
        QCOMPARE(experiment.value(QStringLiteral("repairInProgress")).toBool(), false);
    }

    void onlyVerifierCanMakeCandidateReady()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const QString inputSha = sha256(QByteArrayLiteral("input"));
        auto result = service->beginSolve(id, source, rodSpec(), inputSha,
                                          sha256(QByteArrayLiteral("package")));
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        QVERIFY(service->finishSolve(id, source, result.data.value(QStringLiteral("attemptId")).toString(),
                                     true, solveIdentity(inputSha)).ok);
        const QJsonObject identity = solveIdentity(inputSha);
        result = service->verifyCandidate(id, source, identity,
                                          temporaryDirectory->filePath(QStringLiteral("missing-manifest.json")),
                                          sha256(QByteArrayLiteral("expected manifest")));
        QVERIFY(result.ok);
        QCOMPARE(result.data.value(QStringLiteral("verificationPassed")).toBool(), false);
        QVERIFY(result.data.value(QStringLiteral("verificationReport")).toObject()
                    .value(QStringLiteral("status")).toString() != QStringLiteral("pass"));
        const auto snapshot = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QVERIFY(snapshot.value(QStringLiteral("state")).toString() != QStringLiteral("candidate_ready"));
        QCOMPARE(snapshot.value(QStringLiteral("verificationPassed")).toBool(), false);
    }

    void verificationIdentityAndCostEvidenceAreBounded()
    {
        const QString manifestSha = sha256(QByteArrayLiteral("manifest"));
        QJsonObject oversizedIdentity = solveIdentity(sha256(QByteArrayLiteral("input")));
        oversizedIdentity.insert(QStringLiteral("artifactId"),
                                 QString(257, QLatin1Char('a')));
        auto result = service->verifyCandidate(QStringLiteral("missing-experiment"), source,
                                               oversizedIdentity, QString(), manifestSha);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("VERIFICATION_IDENTITY_INVALID"));

        const QJsonObject validIdentity = solveIdentity(sha256(QByteArrayLiteral("input")));
        const QJsonObject oversizedCost{{QStringLiteral("padding"),
                                         QString(64 * 1024, QLatin1Char('x'))}};
        result = service->verifyCandidate(QStringLiteral("missing-experiment"), source,
                                          validIdentity, QString(), manifestSha, oversizedCost);
        QVERIFY(!result.ok);
        QCOMPARE(result.code, QStringLiteral("VERIFICATION_COST_EVIDENCE_TOO_LARGE"));
    }

    void verificationRequestIsBoundPerChildAttempt()
    {
        const QString id = authorizeAndRun(service.get(), source);
        QVERIFY(!id.isEmpty());
        const QString inputOne = sha256(QByteArrayLiteral("first candidate input"));
        auto result = service->beginSolve(id, source, rodSpec(), inputOne,
                                          sha256(QByteArrayLiteral("package")));
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        const auto firstIdentity = solveIdentity(inputOne);
        QVERIFY(service->finishSolve(id, source, result.data.value(QStringLiteral("attemptId")).toString(),
                                     true, firstIdentity).ok);
        const QString missingManifest = temporaryDirectory->filePath(QStringLiteral("missing-result-manifest.json"));
        result = service->verifyCandidate(id, source, firstIdentity, missingManifest,
                                          sha256(QByteArrayLiteral("first manifest")));
        QVERIFY(result.ok);
        QCOMPARE(result.data.value(QStringLiteral("verificationPassed")).toBool(), false);
        QCOMPARE(result.data.value(QStringLiteral("state")).toString(), QStringLiteral("repairing"));

        const QJsonObject repaired = withSegments(rodSpec(), 5);
        QVERIFY(service->beginRepair(id, source, repaired).ok);
        QVERIFY(service->finishRepair(id, source, repaired, observedPhysical(repaired)).ok);
        const QString inputTwo = sha256(QByteArrayLiteral("second candidate input"));
        result = service->beginSolve(id, source, repaired, inputTwo,
                                     sha256(QByteArrayLiteral("package")));
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        const auto secondIdentity = solveIdentity(inputTwo);
        QVERIFY(service->finishSolve(id, source, result.data.value(QStringLiteral("attemptId")).toString(),
                                     true, secondIdentity).ok);
        result = service->verifyCandidate(id, source, secondIdentity, missingManifest,
                                          sha256(QByteArrayLiteral("second manifest")));
        QVERIFY2(result.ok, qPrintable(result.code + QStringLiteral(": ") + result.message));
        QCOMPARE(result.data.value(QStringLiteral("verificationPassed")).toBool(), false);
        QCOMPARE(result.data.value(QStringLiteral("state")).toString(), QStringLiteral("repairing"));
        const auto record = service->query(id).data.value(QStringLiteral("experiment")).toObject();
        QCOMPARE(record.value(QStringLiteral("counters")).toObject().value(QStringLiteral("solves")).toInt(), 2);
        QCOMPARE(record.value(QStringLiteral("counters")).toObject().value(QStringLiteral("repairs")).toInt(), 1);
    }

private:
    std::unique_ptr<QTemporaryDir> temporaryDirectory;
    std::unique_ptr<RTrustedExperimentService> service;
    RTrustedExperimentService::SourceIdentity source;
};

QTEST_GUILESS_MAIN(TrustedExperimentServiceTest)
#include "tst_trusted_experiment_service.moc"
