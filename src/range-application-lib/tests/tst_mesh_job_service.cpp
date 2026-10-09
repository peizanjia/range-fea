#include <QtTest>
#include <rmesh_job_service.h>
#include <rmesh_candidate_service.h>
#include <rgeometry_candidate_service.h>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

namespace
{
QJsonObject options(double volume=0.08)
{
    return {{"unit","m"},{"volumeUnit","m^3"},{"maxVolume",volume},{"radiusEdgeRatio",1.6}};
}
RModel tetra()
{
    RModel model; model.setNNodes(4); model.setNSurfaces(1);
    model.getSurface(0).setName("analytic tetrahedron boundary");
    model.getSurface(0).setMaterial(RMaterial::generateDefault());
    const double xyz[4][3]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    for (uint i=0;i<4;++i) { model.getNode(i).set(xyz[i][0],xyz[i][1],xyz[i][2]); model.setNodePhysicalId(i,101+i); }
    const uint faces[4][3]={{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    for (uint i=0;i<4;++i)
    {
        RElement element(R_ELEMENT_TRI1);
        for (uint j=0;j<3;++j) element.setNodeId(j,faces[i][j]);
        model.addElement(element,true,0); model.setElementPhysicalId(i,501+i);
    }
    model.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
    return model;
}
bool write(const QString &path,const QByteArray &bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size();
}
bool writeObject(const QString &path,const QJsonObject &object) { return write(path,QJsonDocument(object).toJson(QJsonDocument::Compact)); }
QJsonObject readObject(const QString &path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).object();
}
struct Fixture
{
    QTemporaryDir directory;
    RModel source=tetra();
    QJsonObject input;
    Fixture()
    {
        const QString sourcePath=directory.filePath("source.rbm");
        source.write(sourcePath,false);
        const QString runtimePath=directory.filePath("runtime.json");
        const QString sourceId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        writeObject(runtimePath,{{"sourceId",sourceId},{"sourceRevision","1"}});
        const QString attempt=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString worker=QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
        const QString sourceHash=RMeshJobService::fileSha256(sourcePath);
        input={{"schemaVersion",1},{"mode","closed_surface_tetrahedra"},{"attemptId",attempt},
            {"sourceSnapshotPath",sourcePath},{"sourceSnapshotSha256",sourceHash},{"options",options()},
            {"workerPath",worker},{"workerSha256",RMeshJobService::fileSha256(worker)},
            {"jobDirectory",directory.filePath(attempt)},
            {"runtimeContext",QJsonObject{{"authority","file-owner"},{"persistentModelPath",sourcePath},{"persistentModelSha256",sourceHash},
                {"runtimeSnapshotPath",runtimePath},{"runtimeSnapshotSha256",RMeshJobService::fileSha256(runtimePath)},
                {"sourceSnapshotRef",QJsonObject{{"sourceId",sourceId},{"sourceRevision","1"},{"modelSha256",sourceHash},{"instanceRef",QJsonValue::Null}}},
                {"sourceRuntimeStamp",QJsonValue::Null}}}};
    }
    QString path(const QJsonObject &frozen,const QString &name) const { return QDir(frozen.value("jobDirectory").toString()).filePath(name); }
};
}

class MeshJobServiceTest : public QObject
{
    Q_OBJECT
private slots:
    void descriptorBoundsAndExactSchema()
    {
        Fixture f;
        auto extra=f.input; extra.insert("argv",QJsonArray{"-q"}); QCOMPARE(RMeshJobService::prepare(extra).code,QString("invalid_request"));
        auto algorithm=f.input; auto args=options(); args.insert("tetGenInputParams","arbitrary"); algorithm.insert("options",args);
        QCOMPARE(RMeshJobService::prepare(algorithm).code,QString("invalid_arguments"));
        auto coarse=f.input; coarse.insert("options",options(1e-12)); QCOMPARE(RMeshJobService::prepare(coarse).code,QString("resource_limit"));
        auto identity=f.input; auto context=identity.value("runtimeContext").toObject(); context.insert("code","arbitrary"); identity.insert("runtimeContext",context);
        QCOMPARE(RMeshJobService::prepare(identity).code,QString("invalid_runtime_context"));
        auto path=f.input; path.insert("jobDirectory",f.directory.path()); QCOMPARE(RMeshJobService::prepare(path).code,QString("invalid_job_directory"));
        QVERIFY(!QFileInfo::exists(f.input.value("jobDirectory").toString()));
    }
    void prepareFreezesNativeBytesAndNormalizedOptions()
    {
        Fixture f; auto request=f.input; request.insert("options",options(0.123456789));
        const auto result=RMeshJobService::prepare(request); QVERIFY2(result.valid,qPrintable(result.message));
        const auto frozen=result.descriptor.value("frozenRequest").toObject();
        QCOMPARE(frozen.value("options").toObject().value("maxVolume").toDouble(),0.123457);
        auto hashInput=frozen; hashInput.remove("requestSha256");
        QCOMPARE(frozen.value("requestSha256").toString(),RMeshJobService::objectSha256(hashInput));
        QCOMPARE(readObject(result.descriptor.value("requestPath").toString()),frozen);
        QCOMPARE(RMeshJobService::prepare(request).code,QString("invalid_job_directory"));
        QCOMPARE(RMeshJobService::queryReconcile(frozen).code,QString("unknown"));
        QVERIFY(!QFileInfo::exists(f.path(frozen,"candidate.rbm")));
    }
    void snapshotHashAndLinkRecordsRejected()
    {
        Fixture f; auto bad=f.input; bad.insert("sourceSnapshotSha256",QString(64,'0'));
        auto context=bad.value("runtimeContext").toObject(); auto ref=context.value("sourceSnapshotRef").toObject();
        ref.insert("modelSha256",QString(64,'0')); context.insert("sourceSnapshotRef",ref); bad.insert("runtimeContext",context);
        QCOMPARE(RMeshJobService::prepare(bad).code,QString("source_changed"));
        const QString link=f.directory.filePath("link.rbm"); RModel::writeLink(link,f.input.value("sourceSnapshotPath").toString());
        auto linked=f.input; const auto linkHash=RMeshJobService::fileSha256(link);
        linked.insert("sourceSnapshotPath",link); linked.insert("sourceSnapshotSha256",linkHash);
        context=linked.value("runtimeContext").toObject(); ref=context.value("sourceSnapshotRef").toObject();
        ref.insert("modelSha256",linkHash); context.insert("sourceSnapshotRef",ref); linked.insert("runtimeContext",context);
        const auto result=RMeshJobService::prepare(linked); QVERIFY(!result.valid); QCOMPARE(result.code,QString("prepare_failed"));
        QVERIFY(result.message.contains("LINK"));
    }
    void analyticWorkerAndIdempotentReadback()
    {
        Fixture f; const QString sourceHash=f.input.value("sourceSnapshotSha256").toString();
        const auto prepared=RMeshJobService::prepare(f.input); QVERIFY2(prepared.valid,qPrintable(prepared.message));
        const auto frozen=prepared.descriptor.value("frozenRequest").toObject();
        const auto result=RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath());
        QVERIFY2(result.valid,qPrintable(result.code+": "+result.message)); QCOMPARE(result.code,QString("candidate_ready"));
        QCOMPARE(result.descriptor.value("accepted"),QJsonValue(false)); QVERIFY(result.descriptor.value("requiresRebinding").toBool());
        QCOMPARE(result.candidate.getProblemTaskTree().getProblemTypeMask(),RProblemTypeMask(R_PROBLEM_NONE));
        QCOMPARE(f.source.getProblemTaskTree().getProblemTypeMask(),RProblemTypeMask(R_PROBLEM_STRESS));
        const auto report=readObject(f.path(frozen,"report.json")).value("meshReport").toObject();
        QVERIFY(std::abs(report.value("totalVolume").toDouble()-1.0/6)<1e-8);
        QVERIFY(report.value("minimumDetJac").toDouble()>0); QVERIFY(report.value("maximumTetVolume").toDouble()<=0.08*(1+1e-8));
        QVERIFY(report.value("boundaryFacetCoverageVerified").toBool()); QCOMPARE(report.value("physicsState").toString(),QString("UNBOUND"));
        QCOMPARE(RMeshJobService::fileSha256(f.input.value("sourceSnapshotPath").toString()),sourceHash);
        const QString receiptHash=RMeshJobService::fileSha256(f.path(frozen,"receipt.json"));
        const auto replay=RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath()); QVERIFY2(replay.valid,qPrintable(replay.message));
        QCOMPARE(RMeshJobService::fileSha256(f.path(frozen,"receipt.json")),receiptHash);
        QVERIFY(RMeshJobService::verifyCandidate(frozen).valid); QVERIFY(RMeshJobService::queryReconcile(frozen).valid);
    }
    void fixedCliAcceptsOnlyTheOwnedFrozenRequest()
    {
        Fixture f;
#ifdef Q_OS_WIN
        const QString workerName="range-mesh-worker.exe";
#else
        const QString workerName="range-mesh-worker";
#endif
        const QString worker=QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(workerName)).canonicalFilePath();
        QVERIFY2(!worker.isEmpty(),"range-mesh-worker must be built next to this test.");
        auto input=f.input; input.insert("workerPath",worker); input.insert("workerSha256",RMeshJobService::fileSha256(worker));
        const auto prepared=RMeshJobService::prepare(input); QVERIFY2(prepared.valid,qPrintable(prepared.message));
        QProcess process; process.setProgram(worker); process.setArguments({"--request",prepared.descriptor.value("requestPath").toString()});
        process.start(); QVERIFY(process.waitForStarted(5000)); QVERIFY(process.waitForFinished(120000));
        const auto reply=QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QVERIFY2(reply.value("valid").toBool(),qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())+QString::fromUtf8(process.readAllStandardError())));
        QCOMPARE(process.exitCode(),0); QCOMPARE(reply.value("code").toString(),QString("candidate_ready"));
        QVERIFY(RMeshJobService::verifyCandidate(prepared.descriptor.value("frozenRequest").toObject()).valid);
        QProcess arbitrary; arbitrary.setProgram(worker); arbitrary.setArguments({"--request",prepared.descriptor.value("requestPath").toString(),"-q"});
        arbitrary.start(); QVERIFY(arbitrary.waitForStarted(5000)); QVERIFY(arbitrary.waitForFinished(5000));
        const auto rejected=QJsonDocument::fromJson(arbitrary.readAllStandardOutput()).object();
        QCOMPARE(rejected.value("code").toString(),QString("usage_error")); QVERIFY(!rejected.value("valid").toBool());
    }
    void sourceRuntimeWorkerAndRequestChangesReject()
    {
        Fixture f; const auto prepared=RMeshJobService::prepare(f.input); QVERIFY(prepared.valid);
        const auto frozen=prepared.descriptor.value("frozenRequest").toObject();
        auto edited=frozen; edited.insert("attemptId",QUuid::createUuid().toString(QUuid::WithoutBraces));
        QCOMPARE(RMeshJobService::queryReconcile(edited).code,QString("request_hash_mismatch"));
        QCOMPARE(RMeshJobService::runWorker(frozen,f.directory.filePath("other.exe")).code,QString("worker_identity_mismatch"));
        const QString runtime=frozen.value("runtimeContext").toObject().value("runtimeSnapshotPath").toString();
        QVERIFY(writeObject(runtime,{{"sourceRevision","changed"}})); QCOMPARE(RMeshJobService::queryReconcile(frozen).code,QString("runtime_context_changed"));
        RModel changed=f.source; changed.getNode(0).set(0.1,0,0); changed.write(frozen.value("sourceSnapshotPath").toString(),false);
        QCOMPARE(RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath()).code,QString("source_changed"));
    }
    void cancellationHasDurableFailureAndNeverRegenerates()
    {
        Fixture f; const auto prepared=RMeshJobService::prepare(f.input); QVERIFY(prepared.valid);
        const auto frozen=prepared.descriptor.value("frozenRequest").toObject(); std::atomic_bool cancel{true};
        const auto result=RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath(),&cancel);
        QVERIFY(!result.valid); QCOMPARE(result.code,QString("cancelled")); QVERIFY(result.candidate.isEmpty());
        QVERIFY(!QFileInfo::exists(f.path(frozen,"candidate.rbm"))); QVERIFY(!QFileInfo::exists(f.path(frozen,"report.json")));
        QCOMPARE(readObject(f.path(frozen,"receipt.json")).value("status").toString(),QString("cancelled"));
        cancel=false; QCOMPARE(RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath(),&cancel).code,QString("cancelled"));
    }
    void partialAndUnexpectedOutputsNeverBecomeSuccess()
    {
        Fixture f; const auto prepared=RMeshJobService::prepare(f.input); QVERIFY(prepared.valid);
        const auto frozen=prepared.descriptor.value("frozenRequest").toObject();
        QVERIFY(write(f.path(frozen,"candidate.rbm"),"partial"));
        QCOMPARE(RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath()).code,QString("output_conflict"));
        QCOMPARE(RMeshJobService::queryReconcile(frozen).code,QString("unknown"));
        QVERIFY(writeObject(f.path(frozen,"intent.json"),{{"status","running"}}));
        QCOMPARE(RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath()).code,QString("unknown"));
        QVERIFY(!RMeshJobService::verifyCandidate(frozen).valid);
    }
    void actualBytesAndForgedReportAreRejected()
    {
        Fixture f; const auto prepared=RMeshJobService::prepare(f.input); QVERIFY(prepared.valid);
        const auto frozen=prepared.descriptor.value("frozenRequest").toObject();
        const auto result=RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath()); QVERIFY2(result.valid,qPrintable(result.message));
        const QString reportPath=f.path(frozen,"report.json"),receiptPath=f.path(frozen,"receipt.json");
        auto report=readObject(reportPath); auto mesh=report.value("meshReport").toObject(); mesh.insert("totalVolume",2.0); report.insert("meshReport",mesh);
        QVERIFY(writeObject(reportPath,report)); QCOMPARE(RMeshJobService::verifyCandidate(frozen).code,QString("artifact_hash_mismatch"));
        auto receipt=readObject(receiptPath); receipt.insert("reportFileSha256",RMeshJobService::fileSha256(reportPath)); receipt.remove("receiptSha256");
        receipt.insert("receiptSha256",RMeshJobService::objectSha256(receipt)); QVERIFY(writeObject(receiptPath,receipt));
        QCOMPARE(RMeshJobService::verifyCandidate(frozen).code,QString("quality_report_mismatch"));
        receipt.insert("candidatePath",f.input.value("sourceSnapshotPath")); receipt.remove("receiptSha256");
        receipt.insert("receiptSha256",RMeshJobService::objectSha256(receipt)); QVERIFY(writeObject(receiptPath,receipt));
        QCOMPARE(RMeshJobService::verifyCandidate(frozen).code,QString("invalid_receipt"));
    }
    void independentVerifierRejectsIdsJacobianAndBindings()
    {
        const RModel source=tetra(); const QString fingerprint=RGeometryCandidateService::geometryFingerprint(source);
        const auto generated=RMeshCandidateService::generate(source,options()); QVERIFY2(generated.valid,qPrintable(generated.message));
        const auto verified=RMeshCandidateService::verify(source,generated.candidate,options()); QVERIFY2(verified.valid,qPrintable(verified.message));
        QCOMPARE(verified.report,generated.report);
        RModel ids=generated.candidate; ids.setNodePhysicalId(0,source.getNodePhysicalId(0));
        QCOMPARE(RMeshCandidateService::verify(source,ids,options()).code,QString("invalid_candidate_identity"));
        RModel inverted=generated.candidate;
        for (uint i=0;i<inverted.getNElements();++i) if (inverted.getElement(i).getType()==R_ELEMENT_TETRA1)
        {
            auto &element=inverted.getElement(i); const uint node=element.getNodeId(1); element.setNodeId(1,element.getNodeId(2)); element.setNodeId(2,node); break;
        }
        const auto bad=RMeshCandidateService::verify(source,inverted,options()); QVERIFY(!bad.valid); QCOMPARE(bad.code,QString("invalid_candidate_mesh"));
        QVERIFY(bad.message.contains("jacobian"));
        RModel bindings=generated.candidate; bindings.setProblemTaskTree(RProblemTaskItem::generateDefaultTree(R_PROBLEM_STRESS));
        QCOMPARE(RMeshCandidateService::verify(source,bindings,options()).code,QString("invalid_candidate_bindings"));
        bindings=generated.candidate; bindings.getSurface(0).addBoundaryCondition(RBoundaryCondition(R_BOUNDARY_CONDITION_DISPLACEMENT));
        QCOMPARE(RMeshCandidateService::verify(source,bindings,options()).code,QString("invalid_candidate_bindings"));
        std::atomic_bool cancel{true}; QCOMPARE(RMeshCandidateService::verify(source,generated.candidate,options(),&cancel).code,QString("cancelled"));
        QCOMPARE(RGeometryCandidateService::geometryFingerprint(source),fingerprint);
    }
    void nativeRoundtripHasNoEnvironmentOrDisplayBindings()
    {
        Fixture f;
        QCOMPARE(f.source.getSurface(0).getNEnvironmentConditions(),2u);
        const auto generated=RMeshCandidateService::generate(f.source,options()); QVERIFY2(generated.valid,qPrintable(generated.message));
        for (uint i=0;i<generated.candidate.getNElementGroups();++i)
        {
            const auto &group=*generated.candidate.getElementGroupPtr(i);
            QCOMPARE(group.getNBoundaryConditions(),0u); QCOMPARE(group.getNInitialConditions(),0u);
            QCOMPARE(group.getNEnvironmentConditions(),0u); QVERIFY(group.getData().getVariableData().empty());
        }
        const QString path=f.directory.filePath("roundtrip.rbm"); generated.candidate.write(path,false);
        RModel loaded; loaded.read(path);
        for (uint i=0;i<loaded.getNElementGroups();++i)
        {
            const auto &group=*loaded.getElementGroupPtr(i);
            QCOMPARE(group.getNEnvironmentConditions(),0u); QVERIFY(group.getData().getVariableData().empty());
        }
        const auto verified=RMeshCandidateService::verify(f.source,loaded,options()); QVERIFY2(verified.valid,qPrintable(verified.message));
        QCOMPARE(verified.report,generated.report);
        loaded.getSurface(0).getData().getVariableData()[R_VARIABLE_DISPLACEMENT]=R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT;
        loaded.write(path,false); RModel displayLoaded; displayLoaded.read(path);
        const auto displayFailure=RMeshCandidateService::verify(f.source,displayLoaded,options());
        QCOMPARE(displayFailure.code,QString("invalid_candidate_bindings")); QVERIFY(displayFailure.message.contains("displayBindings=1"));
        loaded=generated.candidate; loaded.getSurface(0).addEnvironmentCondition(REnvironmentCondition(R_ENVIRONMENT_CONDITION_G_ACCELERATION));
        loaded.write(path,false); RModel environmentLoaded; environmentLoaded.read(path);
        const auto environmentFailure=RMeshCandidateService::verify(f.source,environmentLoaded,options());
        QCOMPARE(environmentFailure.code,QString("invalid_candidate_bindings")); QVERIFY(environmentFailure.message.contains("environment=1"));
        QCOMPARE(f.source.getSurface(0).getNEnvironmentConditions(),2u);
    }
    void rereadCandidateRejectsForgedPhysicalIdsAndDeterminant()
    {
        Fixture f; const auto prepared=RMeshJobService::prepare(f.input); QVERIFY(prepared.valid);
        const auto frozen=prepared.descriptor.value("frozenRequest").toObject();
        const auto result=RMeshJobService::runWorker(frozen,QCoreApplication::applicationFilePath()); QVERIFY2(result.valid,qPrintable(result.message));
        const auto replace=[&](const RModel &model) {
            const QString candidatePath=f.path(frozen,"candidate.rbm"),reportPath=f.path(frozen,"report.json"),receiptPath=f.path(frozen,"receipt.json");
            model.write(candidatePath,false);
            const QString bytesHash=RMeshJobService::fileSha256(candidatePath);
            const QString geometry=RGeometryCandidateService::geometryFingerprint(model);
            auto report=readObject(reportPath); report.insert("candidateFileSha256",bytesHash); report.insert("candidateGeometrySha256",geometry);
            if (!writeObject(reportPath,report)) return false;
            auto receipt=readObject(receiptPath); receipt.insert("candidateFileSha256",bytesHash); receipt.insert("candidateGeometrySha256",geometry);
            receipt.insert("reportFileSha256",RMeshJobService::fileSha256(reportPath)); receipt.remove("receiptSha256");
            receipt.insert("receiptSha256",RMeshJobService::objectSha256(receipt)); return writeObject(receiptPath,receipt);
        };
        RModel reused=result.candidate; reused.setNodePhysicalId(0,f.source.getNodePhysicalId(0)); QVERIFY(replace(reused));
        QCOMPARE(RMeshJobService::verifyCandidate(frozen).code,QString("invalid_candidate_identity"));
        RModel inverted=result.candidate;
        for (uint i=0;i<inverted.getNElements();++i) if (inverted.getElement(i).getType()==R_ELEMENT_TETRA1)
        {
            auto &element=inverted.getElement(i); const uint node=element.getNodeId(1); element.setNodeId(1,element.getNodeId(2)); element.setNodeId(2,node); break;
        }
        QVERIFY(replace(inverted));
        const auto bad=RMeshJobService::verifyCandidate(frozen); QCOMPARE(bad.code,QString("invalid_candidate_mesh")); QVERIFY(bad.message.contains("jacobian"));
        RModel retainedDisplay=result.candidate;
        retainedDisplay.getSurface(0).getData().getVariableData()[R_VARIABLE_DISPLACEMENT]=R_ENTITY_GROUP_VARIABLE_DISPLAY_DISPLACEMENT;
        QVERIFY(replace(retainedDisplay));
        const auto retained=RMeshJobService::verifyCandidate(frozen); QCOMPARE(retained.code,QString("invalid_candidate_bindings"));
        QVERIFY(retained.message.contains("displayBindings=1"));
        RModel reversedBoundary=result.candidate;
        auto &triangle=reversedBoundary.getElement(reversedBoundary.getSurface(0).get(0));
        const uint corner=triangle.getNodeId(1); triangle.setNodeId(1,triangle.getNodeId(2)); triangle.setNodeId(2,corner);
        QVERIFY(replace(reversedBoundary));
        const auto inward=RMeshJobService::verifyCandidate(frozen); QCOMPARE(inward.code,QString("invalid_candidate_mesh"));
        QVERIFY(inward.message.contains("orientation is inward"));
        // Replaying the fixed CLI still reconciles the actual artifact and must
        // reject it; it may neither regenerate nor repair the reversed triangle.
#ifdef Q_OS_WIN
        const QString workerName="range-mesh-worker.exe";
#else
        const QString workerName="range-mesh-worker";
#endif
        Fixture cliFixture; auto cliInput=cliFixture.input;
        const QString worker=QFileInfo(QDir(QCoreApplication::applicationDirPath()).filePath(workerName)).canonicalFilePath();
        QVERIFY(!worker.isEmpty()); cliInput.insert("workerPath",worker); cliInput.insert("workerSha256",RMeshJobService::fileSha256(worker));
        const auto cliPrepared=RMeshJobService::prepare(cliInput); QVERIFY(cliPrepared.valid);
        const auto cliFrozen=cliPrepared.descriptor.value("frozenRequest").toObject();
        QProcess process; process.setProgram(worker); process.setArguments({"--request",cliPrepared.descriptor.value("requestPath").toString()});
        process.start(); QVERIFY(process.waitForStarted(5000)); QVERIFY(process.waitForFinished(120000));
        QCOMPARE(process.exitCode(),0);
        QVERIFY(QJsonDocument::fromJson(process.readAllStandardOutput()).object().value("valid").toBool());
        process.readAllStandardError();
        const QString candidatePath=cliFixture.path(cliFrozen,"candidate.rbm"),reportPath=cliFixture.path(cliFrozen,"report.json"),receiptPath=cliFixture.path(cliFrozen,"receipt.json");
        RModel cliCandidate; cliCandidate.read(candidatePath);
        auto &cliTriangle=cliCandidate.getElement(cliCandidate.getSurface(0).get(0));
        const uint cliCorner=cliTriangle.getNodeId(1); cliTriangle.setNodeId(1,cliTriangle.getNodeId(2)); cliTriangle.setNodeId(2,cliCorner);
        cliCandidate.write(candidatePath,false);
        const QString candidateHash=RMeshJobService::fileSha256(candidatePath),geometry=RGeometryCandidateService::geometryFingerprint(cliCandidate);
        auto forgedReport=readObject(reportPath); forgedReport.insert("candidateFileSha256",candidateHash); forgedReport.insert("candidateGeometrySha256",geometry);
        auto forgedMesh=forgedReport.value("meshReport").toObject(); forgedMesh.insert("postGeometrySha256",geometry);
        forgedMesh.insert("outwardBoundaryOrientationVerified",true); forgedMesh.remove("reportSha256");
        forgedMesh.insert("reportSha256",RMeshJobService::objectSha256(forgedMesh)); forgedReport.insert("meshReport",forgedMesh);
        QVERIFY(writeObject(reportPath,forgedReport));
        auto forgedReceipt=readObject(receiptPath); forgedReceipt.insert("candidateFileSha256",candidateHash); forgedReceipt.insert("candidateGeometrySha256",geometry);
        forgedReceipt.insert("reportFileSha256",RMeshJobService::fileSha256(reportPath)); forgedReceipt.remove("receiptSha256");
        forgedReceipt.insert("receiptSha256",RMeshJobService::objectSha256(forgedReceipt)); QVERIFY(writeObject(receiptPath,forgedReceipt));
        process.start(); QVERIFY(process.waitForStarted(5000)); QVERIFY(process.waitForFinished(120000));
        const auto rejectedReply=QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(rejectedReply.value("code").toString(),QString("invalid_candidate_mesh")); QVERIFY(!rejectedReply.value("valid").toBool());
        QVERIFY(rejectedReply.value("message").toString().contains("orientation is inward"));
        QCOMPARE(RMeshJobService::fileSha256(candidatePath),candidateHash);
    }
};

QTEST_GUILESS_MAIN(MeshJobServiceTest)
#include "tst_mesh_job_service.moc"
