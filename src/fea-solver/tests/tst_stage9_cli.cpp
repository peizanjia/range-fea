#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <cmath>
#include <limits>
#include "stage9_test_models.h"

namespace {
QJsonObject readJson(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QByteArray sha(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);
}
QString solverPath() {
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath("fea-solver.exe");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("fea-solver");
#endif
}
bool run(const QString &program,const QStringList &args,QByteArray &error) {
    QProcess p; p.start(program,args);
    if (!p.waitForFinished(180000)) { error=p.errorString().toUtf8(); p.kill(); p.waitForFinished(); return false; }
    error=p.readAllStandardOutput()+p.readAllStandardError();
    return p.exitStatus()==QProcess::NormalExit && p.exitCode()==0;
}
QString script(const char *name) { return QDir(QStringLiteral(RANGE_STAGE9_EXAMPLES)).filePath(name); }
double nodal(const RModel &model,RVariableType type,uint node) {
    const uint pos=model.findVariable(type);
    return pos==RConstants::eod ? std::numeric_limits<double>::quiet_NaN() : model.getVariable(pos).getValue(0,node);
}
}
class TestStage9Cli: public QObject {
    Q_OBJECT
private slots:
    void nativeExportReductionDesignAndFullFE();
    void invalidExportPreservesOutput();
};
void TestStage9Cli::nativeExportReductionDesignAndFullFE()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    constexpr uint count=4,steps=400;
    constexpr double dt=0.005,amplitude=0.001,pi=3.14159265358979323846;
    auto model=Stage9TestModels::makeTruss(count);
    auto &dynamics=model.getProblemSetup().getStructuralDynamicsSetup();
    dynamics.setEnabled(true); dynamics.setTimeStep(dt); dynamics.setNSteps(steps); dynamics.setOutputFrequency(steps);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setSolverCvgValue(1e-13);
    model.getMatrixSolverConf(RMatrixSolverConf::CG).setNOuterIterations(1000);
    for (uint node=1;node<=count;++node) {
        RElement point(R_ELEMENT_POINT); point.setNodeId(0,node);
        const uint group=model.getNPoints(); model.addElement(point,true,group);
        RInitialCondition ic(R_INITIAL_CONDITION_DISPLACEMENT);
        ic.getComponent(ic.findComponentPosition(R_VARIABLE_DISPLACEMENT_X)).add(0.0,amplitude*std::sin(pi*double(node)/(2.0*count)));
        model.getPoint(group).addInitialCondition(ic);
    }
    auto &control=model.getProblemSetup().getStructuralControlSetup();
    control.enabled=true; control.controllerId="stage9-velocity-feedback"; control.deterministicSeed=9;
    const auto input=model.write(dir.filePath("full-fe.ram"),false);
    const auto before=sha(input);
    const auto bundle=dir.filePath("linear.json"),report=dir.filePath("analysis.json"),gains=dir.filePath("gain.json"),reference=dir.filePath("reference.json");
    QByteArray errors;
    QVERIFY2(run(solverPath(),{"--file="+input,"--linear-model-output="+bundle},errors),errors.constData());
    QCOMPARE(sha(input),before);
    const QString artifactDir=qEnvironmentVariable("RANGE_STAGE9_ARTIFACT_DIR");
    if (!artifactDir.isEmpty()) {
        QVERIFY(QDir().mkpath(artifactDir));
        QVERIFY(QFile::copy(input,QDir(artifactDir).filePath("stage9_truss4.ram")));
    }
    const auto linear=readJson(bundle);
    QCOMPARE(linear["M"].toArray().size(),int(count));
    QCOMPARE(linear["source"].toObject()["sha256"].toString(),QString::fromLatin1(before.toHex()));
    QVERIFY2(run(QStringLiteral(RANGE_STAGE9_PYTHON),{script("stage9_analysis.py"),"--model",bundle,"--output",report,"--modes","2","--boundary-indices-json","[3]","--frequencies-json","[0,5,15,30]"},errors),errors.constData());
    const auto analysis=readJson(report);
    QVERIFY(!analysis.isEmpty());
    QVERIFY(analysis["errors"].toArray().isEmpty());
    const auto descriptors=analysis["descriptor"].toObject();
    QCOMPARE(descriptors["full"].toObject()["E"].toArray().size(),8);
    QCOMPARE(descriptors["modal"].toObject()["E"].toArray().size(),4);
    QCOMPARE(descriptors["craigBampton"].toObject()["E"].toArray().size(),6);
    const auto diagnostic=analysis["diagnostics"].toObject()["full"].toObject();
    QVERIFY(diagnostic["controllability"].toObject()["fullRank"].toBool());
    QVERIFY(diagnostic["observability"].toObject()["fullRank"].toBool());
    QCOMPARE(analysis["frequencyResponse"].toObject()["full"].toArray().size(),4);
    QVERIFY2(run(QStringLiteral(RANGE_STAGE9_PYTHON),{script("stage9_velocity_controller.py"),"--model",bundle,"--target-damping","0.1","--sensor-id","tip-velocity-x","--actuator-id","tip-force-x","--output",gains},errors),errors.constData());
    const auto gain=readJson(gains);
    QVERIFY(gain["gain"].toDouble()>0.0);
    QVERIFY(gain["fullOrderPoleCheck"].toObject()["noPositiveGrowthVerified"].toBool());
    QJsonArray q0;
    const auto dofs=linear["dofs"].toArray();
    for (const auto &value:dofs) {
        const auto dof=value.toObject(); const uint node=uint(dof["nodePosition"].toInt());
        const auto direction=dof["globalDirection"].toArray();
        q0.append(direction[0].toDouble()*amplitude*std::sin(pi*double(node)/(2.0*count)));
    }
    const QString initial=QString::fromUtf8(QJsonDocument(q0).toJson(QJsonDocument::Compact));
    QVERIFY2(run(QStringLiteral(RANGE_STAGE9_PYTHON),{script("stage9_velocity_controller.py"),"--replay-model",bundle,"--gains",gains,"--dt",QString::number(dt),"--steps",QString::number(steps),"--initial-q-json",initial,"--output",reference},errors),errors.constData());
    const auto prediction=readJson(reference);
    const QJsonArray controlArgs{script("stage9_velocity_controller.py"),"--gains",gains};
    const QString args=QString::fromUtf8(QJsonDocument(controlArgs).toJson(QJsonDocument::Compact));
    QVERIFY2(run(solverPath(),{"--file="+input,"--control-program="+QStringLiteral(RANGE_STAGE9_PYTHON),"--control-args-json="+args,"--control-timeout-ms=10000"},errors),errors.constData());
    RModel result; result.read(input);
    const auto final=prediction["fullOrder"].toObject();
    const auto q=final["q"].toArray(),v=final["v"].toArray();
    QCOMPARE(q.size(),int(count)); QCOMPARE(v.size(),int(count));
    double qError=0.0,vError=0.0;
    for (uint row=0;row<count;++row) {
        const auto dof=dofs[int(row)].toObject(); const uint node=uint(dof["nodePosition"].toInt());
        const double direction=dof["globalDirection"].toArray()[0].toDouble();
        qError=std::max(qError,std::fabs(nodal(result,R_VARIABLE_DISPLACEMENT,node)-direction*q[int(row)].toDouble()));
        vError=std::max(vError,std::fabs(nodal(result,R_VARIABLE_VELOCITY,node)-direction*v[int(row)].toDouble()));
        QVERIFY(qError<1e-9); QVERIFY(vError<1e-8);
    }
    QVERIFY(final["energy"].toDouble()<0.05*prediction["passive"].toObject()["energy"].toDouble());
    // Also execute the passive native FE baseline; a synthetic baseline alone
    // cannot certify how the controller behaves in the real application.
    auto passive=model;
    passive.getProblemSetup().getStructuralControlSetup()=RStructuralControlSetup();
    const auto passiveInput=passive.write(dir.filePath("passive-fe.ram"),false);
    QByteArray passiveErrors;
    QVERIFY2(run(solverPath(),{"--file="+passiveInput},passiveErrors),passiveErrors.constData());
    RModel passiveResult; passiveResult.read(passiveInput);
    const auto passiveExpected=prediction["passiveFullOrder"].toObject();
    double passiveQError=0.0,passiveVError=0.0,controlledEnergy=0.0,passiveEnergy=0.0;
    for (uint i=0;i<count;++i) {
        const auto di=dofs[int(i)].toObject(); const uint ni=uint(di["nodePosition"].toInt());
        const double si=di["globalDirection"].toArray()[0].toDouble();
        const double cq=si*nodal(result,R_VARIABLE_DISPLACEMENT,ni),cv=si*nodal(result,R_VARIABLE_VELOCITY,ni);
        const double pq=si*nodal(passiveResult,R_VARIABLE_DISPLACEMENT,ni),pv=si*nodal(passiveResult,R_VARIABLE_VELOCITY,ni);
        passiveQError=std::max(passiveQError,std::fabs(pq-passiveExpected["q"].toArray()[int(i)].toDouble()));
        passiveVError=std::max(passiveVError,std::fabs(pv-passiveExpected["v"].toArray()[int(i)].toDouble()));
        for (uint j=0;j<count;++j) {
            const auto dj=dofs[int(j)].toObject(); const uint nj=uint(dj["nodePosition"].toInt());
            const double sj=dj["globalDirection"].toArray()[0].toDouble();
            const double mij=linear["M"].toArray()[int(i)].toArray()[int(j)].toDouble(),kij=linear["K"].toArray()[int(i)].toArray()[int(j)].toDouble();
            controlledEnergy+=0.5*sj*(cv*mij*nodal(result,R_VARIABLE_VELOCITY,nj)+cq*kij*nodal(result,R_VARIABLE_DISPLACEMENT,nj));
            passiveEnergy+=0.5*sj*(pv*mij*nodal(passiveResult,R_VARIABLE_VELOCITY,nj)+pq*kij*nodal(passiveResult,R_VARIABLE_DISPLACEMENT,nj));
        }
    }
    QVERIFY(passiveQError<1e-8); QVERIFY(passiveVError<1e-7);
    QVERIFY(controlledEnergy<0.05*passiveEnergy);
    if (!artifactDir.isEmpty()) {
        for (const auto &path:{bundle,report,gains,reference})
            QVERIFY(QFile::copy(path,QDir(artifactDir).filePath(QFileInfo(path).fileName())));
        const auto resultPath=result.write(QDir(artifactDir).filePath("controlled-final.ram"),false);
        QVERIFY(QFile::exists(resultPath));
        QFile log(QDir(artifactDir).filePath("full-fe-process.log"));
        QVERIFY(log.open(QIODevice::WriteOnly)); QCOMPARE(log.write(errors),qint64(errors.size()));
        QFile verification(QDir(artifactDir).filePath("verification.json"));
        QVERIFY(verification.open(QIODevice::WriteOnly));
        verification.write(QJsonDocument(QJsonObject{{"maxDisplacementErrorM",qError},{"maxVelocityErrorMPerSecond",vError},{"passiveMaxDisplacementErrorM",passiveQError},{"passiveMaxVelocityErrorMPerSecond",passiveVError},{"controlledToPassiveEnergyRatio",controlledEnergy/passiveEnergy},{"nativeControlledEnergyJ",controlledEnergy},{"nativePassiveEnergyJ",passiveEnergy},{"steps",int(steps)},{"dtSeconds",dt},{"originalNativeSha256",QString::fromLatin1(before.toHex())}}).toJson());
    }
}
void TestStage9Cli::invalidExportPreservesOutput()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto model=Stage9TestModels::makeTruss();
    RLinearConstraint constraint; constraint.id="unsupported"; constraint.terms={{{1,RDofType::UX},1.0}};
    model.getProblemSetup().getStructuralInteractionSetup().getLinearConstraints().push_back(constraint);
    const auto input=model.write(dir.filePath("unsupported.ram"),false),output=dir.filePath("output.json");
    QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("existing output"); file.close();
    const auto before=sha(output),nativeBefore=sha(input);
    QByteArray errors; QVERIFY(!run(solverPath(),{"--file="+input,"--linear-model-output="+output},errors));
    QCOMPARE(sha(output),before); QCOMPARE(sha(input),nativeBefore);
    QVERIFY(!run(solverPath(),{"--file="+input,"--linear-model-output="+output,"--restart"},errors));
    QCOMPARE(sha(output),before);
}
QTEST_GUILESS_MAIN(TestStage9Cli)
#include "tst_stage9_cli.moc"
