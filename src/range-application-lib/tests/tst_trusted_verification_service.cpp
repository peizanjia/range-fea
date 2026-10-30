#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <rtrusted_verification_service.h>
#include <rtrusted_mechanics_service.h>
#include <rnative_result_broker.h>
#include <rml_file_utils.h>
#include <rml_boundary_condition.h>
#include <rml_condition_component.h>
#include <rml_model.h>
#include <rml_point.h>
#include <rsolver.h>
#include <rbl_error.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
QJsonObject quantity(double value,const char *unit) {return {{"value",value},{"unit",unit}};}
QJsonArray vector(double x,double y,double z,const char *unit) {return {quantity(x,unit),quantity(y,unit),quantity(z,unit)};}
QString hash(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QJsonObject spec(bool beam=false,const QString &kind="static")
{
    QJsonObject s{{"schemaVersion",1},{"modelType",beam?"cantileverBeam":"axialRod"},{"name","verification fixture"},
        {"geometry",QJsonObject{{"length",quantity(1,"m")},{"segments",4}}},
        {"material",QJsonObject{{"name","Steel"},{"source","fixture"},{"youngModulus",quantity(210e9,"Pa")},{"poissonRatio",0.3},{"density",quantity(7850,"kg/m3")}}},
        {"section",QJsonObject{{"area",quantity(1e-4,"m2")}}},
        {"analysis",QJsonObject{{"kind",kind}}},
        {"supports",QJsonArray{QJsonObject{{"selector",QJsonObject{{"kind","fixedEnd"}}},{"dofs",QJsonArray{"ux","uy","uz"}}},QJsonObject{{"selector",QJsonObject{{"kind","allNodes"}}},{"dofs",QJsonArray{"uy","uz"}}}}},
        {"loads",QJsonArray{QJsonObject{{"kind","pointForce"},{"selector",QJsonObject{{"kind","freeEnd"}}},{"components",vector(1000,0,0,"N")}}}},
        {"initialConditions",QJsonArray{}},{"observables",QJsonArray{QJsonObject{{"selector",QJsonObject{{"kind","freeEnd"}}},{"quantity","displacement"},{"component","ux"}}}},
        {"precision",QJsonObject{{"displacementRelative",0.01}}}};
    if(beam) {
        s["section"]=QJsonObject{{"area",quantity(1e-4,"m2")},{"iy",quantity(1e-8,"m4")},{"iz",quantity(2e-8,"m4")},
            {"torsionalConstant",quantity(1e-8,"m4")},{"wy",quantity(1e-6,"m3")},{"wz",quantity(1e-6,"m3")},{"wt",quantity(1e-6,"m3")},{"localY",QJsonArray{0,1,0}}};
        s["supports"]=QJsonArray{QJsonObject{{"selector",QJsonObject{{"kind","fixedEnd"}}},{"dofs",QJsonArray{"ux","uy","uz","rx","ry","rz"}}}};
        s["loads"]=QJsonArray{QJsonObject{{"kind","pointForce"},{"selector",QJsonObject{{"kind","freeEnd"}}},{"components",vector(0,0,-100,"N")}}};
        s["observables"]=QJsonArray{QJsonObject{{"selector",QJsonObject{{"kind","freeEnd"}}},{"quantity","displacement"},{"component","uz"}}};
    }
    if(kind=="modal") {s["analysis"]=QJsonObject{{"kind",kind},{"modes",3}};s["loads"]=QJsonArray{};s["precision"]=QJsonObject{{"frequencyRelative",0.02}};}
    if(kind=="linearTransient") {
        s["precision"]=QJsonObject{{"responseRelative",0.01}};
        s["analysis"]=QJsonObject{{"kind",kind},{"timeStep",quantity(1e-6,"s")},{"steps",4},
            {"damping",QJsonObject{{"rayleighAlpha",quantity(0,"1/s")},{"rayleighBeta",quantity(0,"s")}}}};
        s["initialConditions"]=QJsonArray{QJsonObject{{"type","displacement"},{"selector",QJsonObject{{"kind","freeEnd"}}},{"components",vector(0,0,0,"m")}},
            QJsonObject{{"type","velocity"},{"selector",QJsonObject{{"kind","freeEnd"}}},{"components",vector(0,0,0,"m/s")}}};
    }
    return s;
}
QJsonObject frozenBeamModalSpec()
{
    auto s=spec(true,"modal");
    s["geometry"]=QJsonObject{{"length",quantity(2.0,"m")},{"segments",6}};
    s["material"]=QJsonObject{{"name","Stage 0 steel"},{"source","tst_solver_stress.cpp::buildBeam"},
        {"youngModulus",quantity(2.0e11,"Pa")},{"poissonRatio",0.3},{"density",quantity(8000.0,"kg/m3")}};
    s["section"]=QJsonObject{{"area",quantity(0.01,"m2")},{"iy",quantity(2.0e-5,"m4")},
        {"iz",quantity(5.0e-5,"m4")},{"torsionalConstant",quantity(7.0e-5,"m4")},
        {"wy",quantity(4.0e-4,"m3")},{"wz",quantity(5.0e-4,"m3")},
        {"wt",quantity(6.0e-4,"m3")},{"localY",QJsonArray{0,1,0}}};
    return s;
}
QJsonObject provenance()
{
    return {{"jobId","verify-job"},{"proofScope","solver-and-declared-controller-runtime/v1"},{"sourceRuntimeStamp",QJsonObject{{"documentId","doc"},{"revision","1"}}},
        {"sourceSnapshotRef",QJsonObject{{"projectId","project"},{"sourceRevision","1"},{"modelSha256",hash("pre-solve-model")}}},
        {"inputSnapshotSha256",hash("input")},{"inputPackageSha256",hash("package")},
        {"runtime",QJsonObject{{"fingerprintSha256",hash("runtime")},{"scope","native-solver-build/v1"}}},
        {"control",QJsonObject{{"enabled",false},{"controllerId",""},{"controllerVersionSha256",""}}}};
}
void addField(RModel &model,RVariableType type)
{
    RVariable field(type,R_VARIABLE_APPLY_NODE);field.resize(3,model.getNNodes());
    for(uint node=0;node<model.getNNodes();++node) for(uint axis=0;axis<3;++axis) field.setValue(axis,node,0.0);
    model.addVariable(field);
}
RModel resultModel(bool beam=false,bool modal=false,double scale=1,int segments=4)
{
    auto input=spec(beam,modal?"modal":"static");
    auto geometry=input["geometry"].toObject();geometry["segments"]=segments;input["geometry"]=geometry;
    const auto candidate=RTrustedMechanicsService::createCandidate(input,"doc","1");
    if(!candidate.valid) qFatal("Fixture candidate rejected: %s",qPrintable(candidate.message));
    RModel model=candidate.model;addField(model,R_VARIABLE_DISPLACEMENT);
    if(beam) addField(model,R_VARIABLE_ROTATION);
    for(uint node=0;node<model.getNNodes();++node) {
        const double x=model.getNode(node).getX();
        auto &u=model.getVariable(model.findVariable(R_VARIABLE_DISPLACEMENT));
        if(beam) {
            u.setValue(2,node,-100*x*x*(3-x)/(6*210e9*1e-8)*scale);
            model.getVariable(model.findVariable(R_VARIABLE_ROTATION)).setValue(1,node,100*x*(2-x)/(2*210e9*1e-8)*scale);
        } else u.setValue(0,node,1000*x/(210e9*1e-4)*scale);
    }
    if(modal) {
        auto &setup=model.getProblemSetup().getModalSetup();setup.setMode(0);
        setup.setFrequency(beam?std::pow(1.875104068711961,2)/(2*3.141592653589793)*std::sqrt(210e9*1e-8/(7850*1e-4)):std::sqrt(210e9/7850)/4);
    }
    return model;
}
struct Published
{
    RNativeResultBroker::PublishResult result;
    QJsonObject identity;
};
Published publish(QTemporaryDir &dir,RModel model)
{
    const QString logical=QDir(dir.path()).filePath("result.rbm");model.write(logical,true);
    const bool modal=model.getProblemTaskTree().getProblemType()==R_PROBLEM_STRESS_MODAL;
    const auto p=RNativeResultBroker::publishResult(dir.path(),modal?logical:RFileUtils::getFileNameWithTimeStep(logical,0),provenance());
    if(!p.valid) qFatal("Fixture publication rejected: %s: %s",qPrintable(p.code),qPrintable(p.message));
    auto identity=provenance();identity["artifactId"]=p.artifactId;
    return {p,identity};
}
QString state(const QJsonObject &report,const QString &metric) {return report["metrics"].toObject()[metric].toObject()["status"].toString();}
QJsonObject verify(const QJsonObject &s,const Published &p) {return RTrustedVerificationService::verify(s,p.identity,p.result.manifestPath,p.result.manifestSha256);}
QString requiredFailures(const QJsonObject &report)
{
    QJsonObject failures;
    const auto metrics=report["metrics"].toObject();
    for(auto it=metrics.begin();it!=metrics.end();++it) {
        const auto value=it.value().toObject();
        if(!value["required"].toBool()||value["status"]==QLatin1String("pass")) continue;
        QJsonObject summary{{"status",value["status"]},{"reason",value["reason"]}};
        if(it.key()==QLatin1String("modelSupport")) {
            QJsonArray issues;
            for(const auto &issueValue:value["evidence"].toObject()["issues"].toArray()) {
                const auto issue=issueValue.toObject(),details=issue["evidence"].toObject();
                QJsonObject compact{{"code",issue["code"]},{"message",issue["message"]}};
                for(const QString &side:{QString("expected"),QString("observed")}) {
                    const auto nodes=details[side].toObject()["nodes"].toArray();
                    if(nodes.isEmpty()) continue;
                    const auto signatures=nodes.first().toObject()["signatures"].toArray();
                    if(signatures.isEmpty()) continue;
                    const auto history=signatures.first().toObject()["history"].toArray();
                    compact[side+"HistoryCount"]=history.size();
                    if(!history.isEmpty()) compact[side+"FirstSample"]=history.first();
                }
                const auto expected=details["expected"].toObject()["nodes"].toArray();
                const auto observed=details["observed"].toObject()["nodes"].toArray();
                if(!expected.isEmpty()&&!observed.isEmpty()) {
                    const auto eSignatures=expected.first().toObject()["signatures"].toArray();
                    const auto oSignatures=observed.first().toObject()["signatures"].toArray();
                    if(!eSignatures.isEmpty()&&!oSignatures.isEmpty()) {
                        const auto eh=eSignatures.first().toObject()["history"].toArray();
                        const auto oh=oSignatures.first().toObject()["history"].toArray();
                        const int count=std::min(eh.size(),oh.size());
                        for(int index=0;index<count;++index) if(eh[index]!=oh[index]) {
                            compact["firstMismatchIndex"]=index;compact["expectedMismatch"]=eh[index];compact["observedMismatch"]=oh[index];break;
                        }
                    }
                }
                issues.append(compact);
            }
            summary["issues"]=issues;
        }
        failures.insert(it.key(),summary);
    }
    return QString::fromUtf8(QJsonDocument(failures).toJson(QJsonDocument::Compact));
}
QString run(RModel &model,const QString &path=QString())
{
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setNInnerIterations(10000);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setSolverCvgValue(1e-13);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setOutputFrequency(0);
    try {RSolver solver(model,path,QString());solver.run();return {};}
    catch(const RError &e) {return e.getMessage();}
    catch(const std::exception &e) {return QString::fromUtf8(e.what());}
}
}

class TrustedVerificationServiceTest:public QObject
{
    Q_OBJECT
private slots:
    void verifiesClosedFormRodAndBeam();
    void rejectsRepublishedResultsWithAlteredForceMetadata();
    void rejectsDisplacementOutsideFrozenLimit();
    void verifiesSelectedModalFrequency();
    void preservesModelIssueRecordNumbers();
    void rejectsWrongIdentityAndTampering();
    void rejectsWrongUnitsAndNonfiniteFields();
    void keepsUnsupportedAndIncompleteMetricsExplicit();
    void verifiesActualTransientAndRejectsIncompleteGrid();
    void rejectsTransientTamperedNonfiniteAndMissingField();
    void requiresCompleteInputIdentity();
    void honorsStricterRequestedPrecision();
    void verifiesExactSingleElementRodTransients();
    void leavesOversizedDenseReferenceNotComputed();
    void excludesZeroExcitationFromExactSdofMetric();
    void verifiesPiecewiseLinearRodHistoryReference();
    void verifiesNativeBeamFreeVibration();
};
void TrustedVerificationServiceTest::verifiesClosedFormRodAndBeam()
{
    for(bool beam:{false,true}) {
        QTemporaryDir dir;const auto p=publish(dir,resultModel(beam));const auto r=verify(spec(beam),p);
        QVERIFY2(r["passed"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
        QCOMPARE(state(r,"displacement"),QString("pass"));QCOMPARE(state(r,"frequency"),QString("notApplicable"));
        const auto supportValues=r["metrics"].toObject()["supportReactions"].toObject()["evidence"].toObject()["values"].toArray();
        auto rootReaction=[&](const QString &component) {
            for(const auto &entry:supportValues) {
                const auto value=entry.toObject();
                if(value["nodePosition"].toInt()==0&&value["component"].toString()==component) return value;
            }
            return QJsonObject{};
        };
        const auto axialOrShear=rootReaction(beam?QStringLiteral("uz"):QStringLiteral("ux"));
        QVERIFY(!axialOrShear.isEmpty());
        QCOMPARE(axialOrShear["unit"].toString(),QStringLiteral("N"));
        QVERIFY(std::abs(axialOrShear["value"].toDouble()-(beam?100.0:-1000.0))<1e-8);
        if(beam) {
            const auto rootMoment=rootReaction(QStringLiteral("ry"));
            QVERIFY(!rootMoment.isEmpty());
            QCOMPARE(rootMoment["unit"].toString(),QStringLiteral("N*m"));
            QVERIFY(std::abs(rootMoment["value"].toDouble()+100.0)<1e-8);
        }
    }
}
void TrustedVerificationServiceTest::rejectsRepublishedResultsWithAlteredForceMetadata()
{
    const QStringList mutations{QStringLiteral("value-unit"), QStringLiteral("disabled"),
                                QStringLiteral("key-unit"), QStringLiteral("shifted-key")};
    for(const QString &mutation:mutations) {
        RModel altered=resultModel();
        RPoint *loadedPoint=nullptr;
        for(uint i=0;i<altered.getNPoints();++i) {
            RPoint &point=altered.getPoint(i);
            if(point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) loadedPoint=&point;
        }
        QVERIFY2(loadedPoint,"Fixture force condition missing.");
        RBoundaryCondition &condition=loadedPoint->getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
        RConditionComponent &component=condition.getComponent(
            condition.findComponentPosition(R_VARIABLE_FORCE_X));
        if(mutation==QLatin1String("value-unit")) component.setUnits(QStringLiteral("kN"));
        else if(mutation==QLatin1String("disabled")) component.setEnabled(false);
        else if(mutation==QLatin1String("key-unit"))
            static_cast<RValueTable &>(component).setKeyUnits(QStringLiteral("ms"));
        else {
            RConditionComponent &fy=condition.getComponent(
                condition.findComponentPosition(R_VARIABLE_FORCE_Y));
            fy.clear();
            fy.add(0.25,0.0);
        }

        QTemporaryDir dir;
        if(mutation==QLatin1String("key-unit")) {
            RModel copyProbe=altered;
            RPoint *copiedPoint=nullptr;
            for(uint i=0;i<copyProbe.getNPoints();++i) {
                RPoint &point=copyProbe.getPoint(i);
                if(point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) copiedPoint=&point;
            }
            QVERIFY2(copiedPoint,"Copied fixture force condition missing.");
            const RBoundaryCondition &copiedCondition=copiedPoint->getBoundaryCondition(
                R_BOUNDARY_CONDITION_FORCE);
            const RConditionComponent &copiedComponent=copiedCondition.getComponent(
                copiedCondition.findComponentPosition(R_VARIABLE_FORCE_X));
            QCOMPARE(copiedComponent.getKeyUnits(),QStringLiteral("ms"));
        }
        const Published republished=publish(dir,altered);
        if(mutation==QLatin1String("key-unit")) {
            const auto persistedModels=RNativeResultBroker::readVerifiedModels(
                republished.result.manifestPath,republished.result.manifestSha256);
            QVERIFY2(persistedModels.valid,qPrintable(persistedModels.message));
            QCOMPARE(persistedModels.records.size(),1);
            RModel &persisted=*persistedModels.records.first().model;
            RPoint *persistedPoint=nullptr;
            for(uint i=0;i<persisted.getNPoints();++i) {
                RPoint &point=persisted.getPoint(i);
                if(point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) persistedPoint=&point;
            }
            QVERIFY2(persistedPoint,"Persisted artifact force condition missing.");
            const RBoundaryCondition &persistedCondition=persistedPoint->getBoundaryCondition(
                R_BOUNDARY_CONDITION_FORCE);
            const RConditionComponent &persistedComponent=persistedCondition.getComponent(
                persistedCondition.findComponentPosition(R_VARIABLE_FORCE_X));
            QCOMPARE(persistedComponent.getKeyUnits(),QStringLiteral("ms"));
        }
        const QJsonObject report=verify(spec(),republished);
        QVERIFY2(!report.value("passed").toBool(),
                 qPrintable(QStringLiteral("Altered force metadata passed: %1").arg(mutation)));
        QCOMPARE(state(report,"modelSupport"),QStringLiteral("fail"));
        bool foundMismatch=false;
        const auto issues=report["metrics"].toObject()["modelSupport"].toObject()
            ["evidence"].toObject()["issues"].toArray();
        for(const QJsonValue &issueValue:issues)
            foundMismatch |= issueValue.toObject().value("code").toString()
                ==QLatin1String("MODEL_LOAD_SPEC_MISMATCH");
        QVERIFY2(foundMismatch,qPrintable(QStringLiteral("Missing load mismatch for %1").arg(mutation)));
    }
}
void TrustedVerificationServiceTest::rejectsDisplacementOutsideFrozenLimit()
{
    for(double scale:{1.009,1.011}) {
        QTemporaryDir dir;const auto p=publish(dir,resultModel(false,false,scale));const auto r=verify(spec(),p);
        QVERIFY(!r["passed"].toBool());QCOMPARE(state(r,"displacement"),scale<1.01?QString("pass"):QString("fail"));
    }
}
void TrustedVerificationServiceTest::verifiesSelectedModalFrequency()
{
    for(bool beam:{false,true}) {
        const auto modalSpec=beam?frozenBeamModalSpec():spec(false,"modal");
        auto candidate=RTrustedMechanicsService::createCandidate(modalSpec,"doc","1");QVERIFY(candidate.valid);
        candidate.model.getProblemSetup().getModalSetup().setNIterations(150);
        candidate.model.getProblemSetup().getModalSetup().setConvergenceValue(1e-12);
        const auto error=run(candidate.model);QVERIFY2(error.isEmpty(),qPrintable(error));
        const RModel baselineModel=candidate.model;
        const auto geometry=modalSpec["geometry"].toObject();
        const auto material=modalSpec["material"].toObject();
        const double length=geometry["length"].toObject()["value"].toDouble();
        const double young=material["youngModulus"].toObject()["value"].toDouble();
        const double density=material["density"].toObject()["value"].toDouble();
        double referenceFrequency=std::sqrt(young/density)/(4.0*length);
        if(beam) {
            constexpr double beta1=1.875104068711961;
            const auto section=modalSpec["section"].toObject();
            const double area=section["area"].toObject()["value"].toDouble();
            const double iy=section["iy"].toObject()["value"].toDouble();
            referenceFrequency=beta1*beta1/(2.0*std::acos(-1.0)*length*length)
                *std::sqrt(young*iy/(density*area));
        }
        for(double factor:{1.019,1.021}) {
            RModel perturbed=baselineModel;
            perturbed.getProblemSetup().getModalSetup().setFrequency(referenceFrequency*factor);
            QTemporaryDir thresholdDir;const auto thresholdResult=publish(thresholdDir,perturbed);
            const auto thresholdReport=verify(modalSpec,thresholdResult);
            QCOMPARE(state(thresholdReport,"frequency"),factor<1.02?QString("pass"):QString("fail"));
            QVERIFY(!thresholdReport["passed"].toBool()); // A plausible frequency alone does not establish a matching eigenpair.
        }
        QTemporaryDir dir;const auto p=publish(dir,baselineModel);const auto r=verify(modalSpec,p);
        QVERIFY2(r["passed"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
        if(beam) qInfo().noquote() << "BEAM-MODAL-01" << QString::fromUtf8(QJsonDocument(r["metrics"].toObject()["frequency"].toObject()["evidence"].toObject()).toJson(QJsonDocument::Compact));
    }
}
void TrustedVerificationServiceTest::preservesModelIssueRecordNumbers()
{
    QTemporaryDir dir;
    const auto s=spec(false,"linearTransient");
    auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");
    QVERIFY2(candidate.valid,qPrintable(candidate.message));
    const QString logical=QDir(dir.path()).filePath("dynamic.rbm");
    const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
    for(const int recordNo:{2,4}) {
        const QString frame=RFileUtils::getFileNameWithTimeStep(logical,recordNo);
        RModel changed;changed.read(frame);
        bool modified=false;
        for(uint i=0;i<changed.getNPoints();++i) {
            RPoint &point=changed.getPoint(i);
            if(!point.hasBoundaryCondition(R_BOUNDARY_CONDITION_FORCE)) continue;
            RBoundaryCondition &condition=point.getBoundaryCondition(R_BOUNDARY_CONDITION_FORCE);
            auto &fx=condition.getComponent(condition.findComponentPosition(R_VARIABLE_FORCE_X));
            fx.clear();fx.add(0.0,1200.0);modified=true;
        }
        QVERIFY(modified);changed.write(frame,true);
    }
    const auto published=RNativeResultBroker::publishResult(dir.path(),logical,provenance());
    QVERIFY2(published.valid,qPrintable(published.code+": "+published.message));
    auto identity=provenance();identity["artifactId"]=published.artifactId;
    const auto report=verify(s,Published{published,identity});
    QVERIFY(!report["passed"].toBool());
    const auto issues=report["metrics"].toObject()["modelSupport"].toObject()["evidence"].toObject()["issues"].toArray();
    QJsonObject grouped;
    for(const auto &value:issues) if(value.toObject()["code"]==QLatin1String("MODEL_LOAD_SPEC_MISMATCH")) grouped=value.toObject();
    QVERIFY(!grouped.isEmpty());
    QCOMPARE(grouped["occurrences"].toInt(),2);
    QCOMPARE(grouped["firstRecordNo"].toInt(),2);
    const QJsonArray expectedRecordNos{2,4};
    QCOMPARE(grouped["recordNos"].toArray(),expectedRecordNos);
}
void TrustedVerificationServiceTest::rejectsWrongIdentityAndTampering()
{
    QTemporaryDir dir;auto p=publish(dir,resultModel());
    for(const QString &key:{QString("jobId"),QString("artifactId"),QString("inputSnapshotSha256"),QString("inputPackageSha256"),QString("sourceSnapshotRef")}) {
        auto bad=p;bad.identity[key]=key=="sourceSnapshotRef"?QJsonValue(QJsonObject{{"projectId","wrong"}}):QJsonValue("wrong");
        const auto r=verify(spec(),bad);QVERIFY(!r["passed"].toBool());QCOMPARE(state(r,"identity"),QString("fail"));
    }
    const auto stale=RTrustedVerificationService::verify(spec(),p.identity,p.result.manifestPath,QString(64,'0'));
    QCOMPARE(state(stale,"identity"),QString("fail"));
    const auto terminal=p.result.manifest["terminalModel"].toObject();
    QFile file(QDir(dir.path()).filePath(terminal["relativePath"].toString()));QVERIFY(file.open(QIODevice::Append));file.write("tampered");file.close();
    QCOMPARE(state(verify(spec(),p),"identity"),QString("fail"));
}
void TrustedVerificationServiceTest::requiresCompleteInputIdentity()
{
    QTemporaryDir dir;const auto p=publish(dir,resultModel());
    for(const auto &key:{QString("artifactId"),QString("jobId"),QString("inputSnapshotSha256"),QString("inputPackageSha256"),QString("sourceSnapshotRef")}) {
        auto bad=p;bad.identity.remove(key);QCOMPARE(state(verify(spec(),bad),"identity"),QString("fail"));
    }
    for(const auto &wrong:{QString(),hash("other-model"),QString("invalid-sha")}) {
        auto bad=p;auto ref=bad.identity["sourceSnapshotRef"].toObject();
        if(wrong.isEmpty()) ref.remove("modelSha256");else ref["modelSha256"]=wrong;
        bad.identity["sourceSnapshotRef"]=ref;QCOMPARE(state(verify(spec(),bad),"identity"),QString("fail"));
    }
    const auto report=verify(spec(),p),identity=report["identity"].toObject();
    QCOMPARE(identity["inputModelSha256"].toString(),provenance()["sourceSnapshotRef"].toObject()["modelSha256"].toString());
    QCOMPARE(identity["inputPackageSha256"],provenance()["inputPackageSha256"]);
    QVERIFY(identity["normalizedProblemSpecSha256"].toString().size()==64);
    auto extra=p;extra.identity["untrustedCallerEvidence"]=QJsonArray{1,2,3};
    QVERIFY(!verify(spec(),extra)["identity"].toObject().contains("untrustedCallerEvidence"));
    QVERIFY(!verify(spec(),extra)["identity"].toObject().contains("expectedIdentity"));
    // Missing model provenance in the actual hash-bound manifest is also rejected,
    // even when the caller supplies precisely the same incomplete reference.
    QTemporaryDir missingDir;auto model=resultModel();const auto logical=QDir(missingDir.path()).filePath("result.rbm");model.write(logical,true);
    auto missing=provenance();auto ref=missing["sourceSnapshotRef"].toObject();ref.remove("modelSha256");missing["sourceSnapshotRef"]=ref;
    const auto published=RNativeResultBroker::publishResult(missingDir.path(),RFileUtils::getFileNameWithTimeStep(logical,0),missing);QVERIFY(published.valid);
    missing["artifactId"]=published.artifactId;
    QCOMPARE(state(verify(spec(),Published{published,missing}),"identity"),QString("fail"));
}
void TrustedVerificationServiceTest::honorsStricterRequestedPrecision()
{
    for(const QString &kind:{QString("static"),QString("modal")}) {
        const bool modal=kind=="modal";auto model=resultModel(false,modal,1.005);
        if(modal) {auto &setup=model.getProblemSetup().getModalSetup();setup.setFrequency(setup.getFrequency()*1.005);}
        QTemporaryDir dir;const auto p=publish(dir,model);auto s=spec(false,kind);
        const QString metric=modal?"frequency":"displacement",key=modal?"frequencyRelative":"displacementRelative";
        QCOMPARE(state(verify(s,p),metric),QString("pass"));
        s["precision"]=QJsonObject{{key,0.001}};const auto strict=verify(s,p);QCOMPARE(state(strict,metric),QString("fail"));
        const auto thresholds=strict["thresholds"].toObject();
        QCOMPARE(thresholds[modal?"effectiveFrequencyRelative":"effectiveDisplacementRelative"].toDouble(),0.001);
        QCOMPARE(thresholds[modal?"fixedFrequencyRelative":"fixedDisplacementRelative"].toDouble(),modal?0.02:0.01);
        s["precision"]=QJsonObject{{key,0.1}};
        QCOMPARE(verify(s,p)["thresholds"].toObject()[modal?"effectiveFrequencyRelative":"effectiveDisplacementRelative"].toDouble(),modal?0.02:0.01);
    }
}
void TrustedVerificationServiceTest::leavesOversizedDenseReferenceNotComputed()
{
    auto s=spec();auto geometry=s["geometry"].toObject();geometry["segments"]=192;s["geometry"]=geometry;
    auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY(candidate.valid);addField(candidate.model,R_VARIABLE_DISPLACEMENT);
    for(uint node=0;node<candidate.model.getNNodes();++node) candidate.model.getVariable(candidate.model.findVariable(R_VARIABLE_DISPLACEMENT)).setValue(0,node,1000*candidate.model.getNode(node).getX()/(210e9*1e-4));
    QTemporaryDir dir;const auto p=publish(dir,candidate.model);const auto report=verify(s,p);
    QVERIFY(!report["passed"].toBool());QCOMPARE(state(report,"modelSupport"),QString("pass"));QCOMPARE(state(report,"staticResidual"),QString("notComputed"));
    QVERIFY(report["metrics"].toObject()["staticResidual"].toObject()["reason"].toString().contains("no matrix allocated"));
}
void TrustedVerificationServiceTest::verifiesExactSingleElementRodTransients()
{
    for(bool initialDisplacement:{false,true}) {
        auto s=spec(false,"linearTransient");auto geometry=s["geometry"].toObject();geometry["segments"]=1;s["geometry"]=geometry;
        const double mass=7850*1e-4/3,stiffness=210e9*1e-4,period=2*3.141592653589793*std::sqrt(mass/stiffness);
        auto analysis=s["analysis"].toObject();analysis["timeStep"]=quantity(period/1024,"s");analysis["steps"]=256;s["analysis"]=analysis;
        if(initialDisplacement) {
            s["loads"]=QJsonArray{};auto conditions=s["initialConditions"].toArray();auto displacement=conditions[0].toObject();displacement["components"]=vector(1e-5,0,0,"m");conditions[0]=displacement;s["initialConditions"]=conditions;
        }
        QTemporaryDir dir;auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY2(candidate.valid,qPrintable(candidate.message));
        const auto logical=QDir(dir.path()).filePath("dynamic.rbm");const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto publication=RNativeResultBroker::publishResult(dir.path(),logical,provenance());QVERIFY2(publication.valid,qPrintable(publication.code+": "+publication.message));
        auto identity=provenance();identity["artifactId"]=publication.artifactId;const Published p{publication,identity};const auto report=verify(s,p);
        QVERIFY2(report["passed"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(report).toJson())));
        QCOMPARE(state(report,"sdofAnalyticDisplacement"),QString("pass"));
        const auto exact=report["metrics"].toObject()["sdofAnalyticDisplacement"].toObject()["evidence"].toObject();
        QCOMPARE(exact["fixedLimit"].toDouble(),1e-4);QVERIFY(exact["scaledMaximumAbsoluteError"].toDouble()<=1e-4);
        auto strict=s;strict["precision"]=QJsonObject{{"responseRelative",1e-8}};
        QCOMPARE(state(verify(strict,p),"displacement"),QString("fail"));
        // Create a separately published, hash-valid but physically altered result.
        QTemporaryDir changedDir;
        for(const auto &name:QDir(dir.path()).entryList({"*.rbm"},QDir::Files)) QVERIFY(QFile::copy(QDir(dir.path()).filePath(name),QDir(changedDir.path()).filePath(name)));
        const auto changedLogical=QDir(changedDir.path()).filePath("dynamic.rbm");
        RModel changed;changed.read(RFileUtils::getFileNameWithTimeStep(changedLogical,256));
        auto &u=changed.getVariable(changed.findVariable(R_VARIABLE_DISPLACEMENT));u.setValue(0,1,u.getValue(0,1)+(initialDisplacement?1e-5:1000/stiffness)*0.001);
        changed.write(changedLogical,true);
        const auto altered=RNativeResultBroker::publishResult(changedDir.path(),changedLogical,provenance());QVERIFY2(altered.valid,qPrintable(altered.code+": "+altered.message));
        auto changedIdentity=provenance();changedIdentity["artifactId"]=altered.artifactId;
        const auto bad=verify(s,Published{altered,changedIdentity});QCOMPARE(state(bad,"identity"),QString("pass"));QCOMPARE(state(bad,"sdofAnalyticDisplacement"),QString("fail"));QVERIFY(!bad["passed"].toBool());
    }
}
void TrustedVerificationServiceTest::excludesZeroExcitationFromExactSdofMetric()
{
    auto s=spec(false,"linearTransient");auto geometry=s["geometry"].toObject();geometry["segments"]=1;s["geometry"]=geometry;s["loads"]=QJsonArray{};
    auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY(candidate.valid);
    QTemporaryDir dir;const auto logical=QDir(dir.path()).filePath("dynamic.rbm");const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
    const auto published=RNativeResultBroker::publishResult(dir.path(),logical,provenance());QVERIFY2(published.valid,qPrintable(published.code+": "+published.message));
    auto identity=provenance();identity["artifactId"]=published.artifactId;
    QCOMPARE(state(verify(s,Published{published,identity}),"sdofAnalyticDisplacement"),QString("notApplicable"));
}
void TrustedVerificationServiceTest::verifiesPiecewiseLinearRodHistoryReference()
{
    auto s=spec(false,"linearTransient");
    s["geometry"]=QJsonObject{{"length",quantity(1000,"m")},{"segments",1}};
    auto material=s["material"].toObject();material["youngModulus"]=quantity(4e6,"Pa");material["density"]=quantity(3,"kg/m3");s["material"]=material;
    s["section"]=QJsonObject{{"area",quantity(0.001,"m2")}};
    // Match the frozen independent SDOF transient fixture (dt=0.02 s through t=2 s).
    auto analysis=s["analysis"].toObject();analysis["timeStep"]=quantity(0.02,"s");analysis["steps"]=100;s["analysis"]=analysis;
    QJsonArray history;
    for(int step=0;step<=100;++step) {const double t=step*0.02;history.append(QJsonObject{{"time",quantity(t,"s")},{"components",vector(std::sin(t),0,0,"N")}});}
    s["loads"]=QJsonArray{QJsonObject{{"kind","pointForce"},{"selector",QJsonObject{{"kind","freeEnd"}}},{"components",vector(0,0,0,"N")},{"history",history}}};
    QTemporaryDir dir;auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY2(candidate.valid,qPrintable(candidate.message));
    const auto logical=QDir(dir.path()).filePath("dynamic.rbm");const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
    const auto published=RNativeResultBroker::publishResult(dir.path(),logical,provenance());QVERIFY2(published.valid,qPrintable(published.code+": "+published.message));
    auto identity=provenance();identity["artifactId"]=published.artifactId;const auto report=verify(s,Published{published,identity});
    QVERIFY2(report["passed"].toBool(),qPrintable(requiredFailures(report)));
    QCOMPARE(state(report,"frozenForcedOscillator"),QString("notApplicable"));
    QCOMPARE(state(report,"rodHistoryReference"),QString("pass"));
    QCOMPARE(state(report,"sdofAnalyticDisplacement"),QString("notApplicable"));
    const auto evidence=report["metrics"].toObject()["rodHistoryReference"].toObject()["evidence"].toObject();
    QCOMPARE(evidence["records"].toInt(),100);QCOMPARE(evidence["fixedAbsoluteLimitM"].toDouble(),1e-4);
    QVERIFY(evidence["maximumAbsoluteErrorM"].toDouble()<=1e-4);
    qInfo().noquote() << "APP-ROD-HISTORY-01" << QString::fromUtf8(QJsonDocument(evidence).toJson(QJsonDocument::Compact));
    QTemporaryDir changedDir;
    for(const auto &name:QDir(dir.path()).entryList({"*.rbm"},QDir::Files)) QVERIFY(QFile::copy(QDir(dir.path()).filePath(name),QDir(changedDir.path()).filePath(name)));
    const auto changedLogical=QDir(changedDir.path()).filePath("dynamic.rbm");RModel changed;changed.read(RFileUtils::getFileNameWithTimeStep(changedLogical,100));
    auto &u=changed.getVariable(changed.findVariable(R_VARIABLE_DISPLACEMENT));u.setValue(0,1,u.getValue(0,1)+0.001);changed.write(changedLogical,true);
    const auto altered=RNativeResultBroker::publishResult(changedDir.path(),changedLogical,provenance());QVERIFY2(altered.valid,qPrintable(altered.code+": "+altered.message));
    auto alteredIdentity=provenance();alteredIdentity["artifactId"]=altered.artifactId;const auto bad=verify(s,Published{altered,alteredIdentity});
    QCOMPARE(state(bad,"identity"),QString("pass"));
    QCOMPARE(state(bad,"frozenForcedOscillator"),QString("notApplicable"));
    QCOMPARE(state(bad,"rodHistoryReference"),QString("fail"));QVERIFY(!bad["passed"].toBool());
}
void TrustedVerificationServiceTest::verifiesNativeBeamFreeVibration()
{
    auto s=spec(true,"linearTransient");s["loads"]=QJsonArray{};
    s["initialConditions"]=QJsonArray{QJsonObject{{"type","displacement"},{"selector",QJsonObject{{"kind","freeEnd"}}},{"components",vector(0,0,-1e-5,"m")}},
        QJsonObject{{"type","velocity"},{"selector",QJsonObject{{"kind","allNodes"}}},{"components",vector(0,0,0,"m/s")}}};
    QTemporaryDir dir;auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY2(candidate.valid,qPrintable(candidate.message));
    const auto logical=QDir(dir.path()).filePath("dynamic.rbm");const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
    const auto published=RNativeResultBroker::publishResult(dir.path(),logical,provenance());QVERIFY2(published.valid,qPrintable(published.code+": "+published.message));
    auto identity=provenance();identity["artifactId"]=published.artifactId;const auto report=verify(s,Published{published,identity});
    QVERIFY2(report["passed"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(report).toJson())));
    for(const auto &metric:{"identity","timeGrid","dynamicEquilibrium","energyBalance","kinematicConsistency"}) QCOMPARE(state(report,metric),QString("pass"));
    QCOMPARE(state(report,"sdofAnalyticDisplacement"),QString("notApplicable"));
    QCOMPARE(report["identity"].toObject()["resultRecords"].toArray().size(),4);
}
void TrustedVerificationServiceTest::rejectsWrongUnitsAndNonfiniteFields()
{
    for(int defect=0;defect<3;++defect) {
        auto model=resultModel();auto &u=model.getVariable(model.findVariable(R_VARIABLE_DISPLACEMENT));
        if(defect==0) u.setUnits("mm");
        if(defect==1) u.setValue(0,2,std::numeric_limits<double>::quiet_NaN());
        if(defect==2) u.resize(3,model.getNNodes()-1);
        QTemporaryDir dir;const auto logical=QDir(dir.path()).filePath("invalid.rbm");model.write(logical,true);
        const auto p=RNativeResultBroker::publishResult(dir.path(),RFileUtils::getFileNameWithTimeStep(logical,0),provenance());
        QVERIFY(!p.valid);QVERIFY(p.code=="INVALID_RESULT_FIELD"||p.code=="NONFINITE_RESULT_FIELD");
    }
    QTemporaryDir dir;const auto p=publish(dir,resultModel());auto s=spec();auto material=s["material"].toObject();material["youngModulus"]=quantity(210,"m");s["material"]=material;
    QCOMPARE(state(verify(s,p),"specification"),QString("fail"));
}
void TrustedVerificationServiceTest::keepsUnsupportedAndIncompleteMetricsExplicit()
{
    QTemporaryDir dir;const auto p=publish(dir,resultModel());auto s=spec();s["modelType"]="plate";const auto r=verify(s,p);
    QVERIFY(!r["passed"].toBool());
    const QSet<QString> allowed{"pass","fail","notComputed","notApplicable"};
    const auto metrics=r["metrics"].toObject();QVERIFY(!metrics.isEmpty());
    for(auto it=metrics.begin();it!=metrics.end();++it) QVERIFY(allowed.contains(it.value().toObject()["status"].toString()));
}
void TrustedVerificationServiceTest::verifiesActualTransientAndRejectsIncompleteGrid()
{
    for(bool beam:{false,true}) {
        QTemporaryDir dir;const auto s=spec(beam,"linearTransient");
        auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY2(candidate.valid,qPrintable(candidate.message));
        const QString logical=QDir(dir.path()).filePath("dynamic.rbm");
        const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto published=RNativeResultBroker::publishResult(dir.path(),logical,provenance());QVERIFY2(published.valid,qPrintable(published.code+": "+published.message));
        auto identity=provenance();identity["artifactId"]=published.artifactId;Published p{published,identity};
        const auto r=verify(s,p);QVERIFY2(r["passed"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
        QCOMPARE(r["identity"].toObject()["resultRecords"].toArray().size(),4);
        // Re-publish a valid shortened run against the original four-step specification.
        auto shorter=s;auto analysis=s["analysis"].toObject();analysis["steps"]=3;shorter["analysis"]=analysis;
        QTemporaryDir shortDir;auto shortCandidate=RTrustedMechanicsService::createCandidate(shorter,"doc","1");QVERIFY(shortCandidate.valid);
        const auto shortLogical=QDir(shortDir.path()).filePath("dynamic.rbm");const auto shortError=run(shortCandidate.model,shortLogical);QVERIFY2(shortError.isEmpty(),qPrintable(shortError));
        const auto shortPublished=RNativeResultBroker::publishResult(shortDir.path(),shortLogical,provenance());QVERIFY(shortPublished.valid);
        auto shortIdentity=provenance();shortIdentity["artifactId"]=shortPublished.artifactId;
        const auto bad=verify(s,Published{shortPublished,shortIdentity});QVERIFY(!bad["passed"].toBool());QCOMPARE(state(bad,"timeGrid"),QString("fail"));
    }
}
void TrustedVerificationServiceTest::rejectsTransientTamperedNonfiniteAndMissingField()
{
    for(int defect=0;defect<3;++defect) {
        QTemporaryDir dir;const auto s=spec(false,"linearTransient");auto candidate=RTrustedMechanicsService::createCandidate(s,"doc","1");QVERIFY(candidate.valid);
        const auto logical=QDir(dir.path()).filePath("dynamic.rbm");const auto error=run(candidate.model,logical);QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto published=RNativeResultBroker::publishResult(dir.path(),logical,provenance());QVERIFY(published.valid);
        auto identity=provenance();identity["artifactId"]=published.artifactId;
        const QString frame=RFileUtils::getFileNameWithTimeStep(logical,2);
        if(defect==0) {QFile file(frame);QVERIFY(file.open(QIODevice::Append));file.write("tampered");file.close();}
        else {
            RModel changed;changed.read(frame);
            auto &u=changed.getVariable(changed.findVariable(R_VARIABLE_DISPLACEMENT));
            if(defect==1) u.setValue(0,2,std::numeric_limits<double>::quiet_NaN());
            else u.resize(3,changed.getNNodes()-1);
            changed.write(frame,true);
        }
        const auto bad=verify(s,Published{published,identity});QVERIFY(!bad["passed"].toBool());QCOMPARE(state(bad,"identity"),QString("fail"));
        if(defect!=0) {
            // The broker rejects malformed physical fields even before a new
            // hash-bound manifest can be published; no trusted DTO is released.
            const auto malformed=RNativeResultBroker::publishResult(dir.path(),logical,provenance());
            QVERIFY(!malformed.valid);QVERIFY2(malformed.code=="INVALID_RESULT_FIELD"||malformed.code=="NONFINITE_RESULT_FIELD",qPrintable(malformed.code));
        }
    }
}
QTEST_APPLESS_MAIN(TrustedVerificationServiceTest)
#include "tst_trusted_verification_service.moc"
